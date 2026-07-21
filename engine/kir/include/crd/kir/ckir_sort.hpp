#pragma once

// ckir_sort.hpp — B-cmp: a STABLE LSD RADIX SORT authored as CKIR shared-memory compute kernels. The CUB `DeviceRadixSort`-class
// primitive (B19 Gaussian depth · B17 A-buffer · light lists), bit-exact on the CPU oracle + Vulkan + DX12. A sort is a
// PERMUTATION (no float arithmetic) and this radix sort is fully DETERMINISTIC (stable ties via a per-block SERIAL rank, NOT an
// atomic-race counter) ⇒ bit-exact by construction — so unlike scan, sort CAN crush the vendor while staying bit-exact (its hot
// path is memory-bound scatter, and our kernels hit ~94 % peak).
//
// One LSD pass over `radix_bits` (digit = (key >> shift) & (2^radix_bits - 1)) is THREE dispatches:
//   1. `build_sort_histogram` — each block counts its digit histogram → block_hist[block*nbins + bin] (shared atomic-add; the
//      COUNT is order-independent ⇒ bit-exact).
//   2. offset scan (a scan over block_hist, digit-major) → the global start position for each (block, bin) — reuses the scan tier.
//   3. `build_sort_scatter` — each block re-reads its slab, computes each key's destination = global_offset[block,bin] +
//      per-block-local rank (a deterministic serial prefix ⇒ stable), writes out. Ping-pong the key buffer across passes.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

// HISTOGRAM (PRIVATIZED, BIN-MAJOR out): block `wid` counts its `elems`-key slab's digit at `shift` → out[bin*nblocks + wid]
// (BIN-major so the offset scan reads each bin's block-column CONSECUTIVELY — coalesced; the scattered histogram writes
// coalesce across concurrent blocks in L2, since consecutive blocks write consecutive addresses per bin). Uses K=threads/32
// PER-WARP sub-histograms to cut shared-atomic CONTENTION ~K× (each warp accumulates into its OWN copy `s_hist[warp][·]`), then
// merges the K copies. The count is an order-independent SUM ⇒ bit-exact regardless of the atomic order. Buffers: keys in = 0
// (ro, U32), block_hist out = 1 (rw, U32). `nbins` a multiple of `threads`; `threads` a multiple of 32.
[[nodiscard]] inline KEntry build_sort_histogram(KGraph& g, int elems, int threads, int radix_bits, int shift, int nblocks)
{
    const int   nbins = 1 << radix_bits;
    const int   mask  = nbins - 1;
    const int   pt    = elems / threads; // keys per thread
    const int   nb    = nbins / threads; // bins per thread (nbins multiple of threads)
    const int   ksub  = threads / 32;    // per-warp sub-histograms
    const Shape sh1   = make_shape({1});
    const auto  ku    = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add   = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul   = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::U32, 0, 0, false);
    const int out_buf = g.buffer_decl(DType::U32, 0, 1, true);
    const int s_hist  = g.shared_decl(DType::U32, ksub * nbins); // [warp][bin]
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int sg      = g.binary(KOp::Div, tid, ku(32)); // this thread's warp = its sub-histogram
    const int base    = mul(wid, ku(static_cast<crd::u32>(elems)));
    const int soff    = mul(sg, ku(static_cast<crd::u32>(nbins)));
    const int uone    = g.constant(1.0, sh1, DType::U32);

    const int mark = g.kernel_stmt_mark();

    for (int j = 0; j < ksub * nb; ++j) { g.stmt_shared_store(s_hist, add(tid, ku(static_cast<crd::u32>(j * threads))), ku(0)); }
    g.stmt_barrier();

    for (int k = 0; k < pt; ++k)
    {
        const int off   = add(tid, ku(static_cast<crd::u32>(k * threads)));
        const int key   = g.buffer_load(in_buf, add(base, off));
        const int digit = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key, ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(mask)));
        g.stmt_shared_atomic_add(s_hist, add(soff, digit), uone); // into THIS warp's sub-histogram
    }
    g.stmt_barrier();

    for (int b = 0; b < nb; ++b) // merge: out[bin*nblocks + wid] = Σ_k s_hist[k][bin] (BIN-major)
    {
        const int bin = add(tid, ku(static_cast<crd::u32>(b * threads)));
        int       sum = g.shared_load(s_hist, bin);
        for (int k = 1; k < ksub; ++k) { sum = add(sum, g.shared_load(s_hist, add(ku(static_cast<crd::u32>(k * nbins)), bin))); }
        g.stmt_buffer_store(out_buf, add(mul(bin, ku(static_cast<crd::u32>(nblocks))), wid), sum);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ⭐ PARALLEL OFFSET — replaces the single-workgroup serial scan (the ~10 ms sort bottleneck) with TWO kernels, each grid = nbins
// (one workgroup PER BIN ⇒ fully parallel over bins). `off_local`: WG `bin` BLOCKED-exclusive-scans its column hist[·][bin]
// (`scan_threads` threads, cpt=nblocks/scan_threads each) → off[·][bin] = within-bin block-prefix, total[bin] = grand total.
// `off_gbase`: WG `bin` reads all bin totals, exclusive-scans them (bin_base), adds bin_base[bin] to its whole off column ⇒ off =
// the FULL bin-major-then-block global offset, bit-IDENTICAL to the old serial kernel. `nblocks % scan_threads == 0`,
// `scan_threads` a power of two. Buffers — local: hist(0)→off(1),total(2); gbase: total(0)→off(1,rw).
[[nodiscard]] inline KEntry build_sort_offset_local(KGraph& g, int nblocks, int radix_bits, int scan_threads)
{
    static_cast<void>(radix_bits); // BIN-major layout: the bin id comes from WorkgroupIndex; nbins no longer enters the math
    const int   cpt   = nblocks / scan_threads;
    const Shape sh1   = make_shape({1});
    const auto  ku    = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add   = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub   = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul   = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int hist_buf = g.buffer_decl(DType::U32, 0, 0, false);
    const int off_buf  = g.buffer_decl(DType::U32, 0, 1, true);
    const int tot_buf  = g.buffer_decl(DType::U32, 0, 2, true);
    const int s_col    = g.shared_decl(DType::U32, nblocks);      // this bin's column
    const int s_tsum   = g.shared_decl(DType::U32, scan_threads); // per-thread chunk totals → cross-thread scan
    const int bin      = g.builtin(KBuiltin::WorkgroupIndex);
    const int tid      = g.builtin(KBuiltin::LocalInvocationIndex);
    const int u0       = ku(0);
    const int mark     = g.kernel_stmt_mark();
    const int c0       = mul(tid, ku(static_cast<crd::u32>(cpt)));

    const int hbase = mul(bin, ku(static_cast<crd::u32>(nblocks))); // BIN-major histogram ⇒ this bin's column is CONTIGUOUS
    for (int k = 0; k < cpt; ++k)                                   // striped coalesced load: s_col[b] = hist[bin*nblocks + b]
    {
        const int b = add(tid, ku(static_cast<crd::u32>(k * scan_threads)));
        g.stmt_shared_store(s_col, b, g.buffer_load(hist_buf, add(hbase, b)));
    }
    g.stmt_barrier();

    // blocked exclusive scan of s_col[nblocks]: freeze chunk, local exclusive (run), Hillis-Steele the thread totals, add base.
    int rr[64];
    for (int k = 0; k < cpt; ++k) { rr[k] = g.shared_load(s_col, add(c0, ku(static_cast<crd::u32>(k)))); g.stmt_materialize(rr[k]); }
    int run = u0;
    for (int k = 0; k < cpt; ++k) { g.stmt_shared_store(s_col, add(c0, ku(static_cast<crd::u32>(k))), run); run = add(run, rr[k]); }
    g.stmt_shared_store(s_tsum, tid, run);
    g.stmt_barrier();
    for (int stride = 1; stride < scan_threads; stride *= 2)
    {
        const int cond    = g.binary(KOp::CmpGe, tid, ku(static_cast<crd::u32>(stride)));
        const int idxsafe = g.select(cond, sub(tid, ku(static_cast<crd::u32>(stride))), tid);
        const int cur     = g.shared_load(s_tsum, tid);
        const int prev    = g.shared_load(s_tsum, idxsafe);
        const int v       = g.select(cond, add(cur, prev), cur);
        g.stmt_materialize(v);
        g.stmt_barrier();
        g.stmt_shared_store(s_tsum, tid, v);
        g.stmt_barrier();
    }
    const int cz    = g.binary(KOp::CmpEq, tid, u0);
    const int pidx  = g.select(cz, tid, sub(tid, ku(1)));
    const int tbase = g.select(cz, u0, g.shared_load(s_tsum, pidx));
    g.stmt_materialize(tbase);
    for (int k = 0; k < cpt; ++k) // fold each thread's cross-chunk base into ITS OWN chunk (blocked)
    {
        const int idx = add(c0, ku(static_cast<crd::u32>(k)));
        g.stmt_shared_store(s_col, idx, add(g.shared_load(s_col, idx), tbase));
    }
    g.stmt_barrier();
    for (int k = 0; k < cpt; ++k) // BIN-major off (off[bin*nblocks + b]) ⇒ striped coalesced stores
    {
        const int b = add(tid, ku(static_cast<crd::u32>(k * scan_threads)));
        g.stmt_buffer_store(off_buf, add(hbase, b), g.shared_load(s_col, b));
    }
    const int if0 = g.stmt_if_begin(cz); // thread 0 writes the grand total (inclusive scan's last thread)
    g.stmt_buffer_store(tot_buf, bin, g.shared_load(s_tsum, ku(static_cast<crd::u32>(scan_threads - 1))));
    g.stmt_if_end(if0);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(scan_threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// GBASE - ONE tiny workgroup (nbins threads): exclusive-scan the per-bin grand totals -> gb[bin] (the global start of each
// bin). Replaces the old full-buffer offset_gbase rewrite (nblocks*nbins read+write per pass): the scatter folds gb[d] into
// the destination instead. grid = 1. in = total(0), out = gb(1). Pure integer, fixed order => bit-exact.
[[nodiscard]] inline KEntry build_sort_gbase(KGraph& g, int radix_bits)
{
    const int   nbins = 1 << radix_bits;
    const Shape sh1   = make_shape({1});
    const auto  ku    = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add   = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub   = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };

    const int tot_buf = g.buffer_decl(DType::U32, 0, 0, false);
    const int gb_buf  = g.buffer_decl(DType::U32, 0, 1, true);
    const int s_tot   = g.shared_decl(DType::U32, nbins);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int u0      = ku(0);
    const int mark    = g.kernel_stmt_mark();
    // WG `wid` scans totals row wid: tot[wid*nbins + t] → gb[wid*nbins + t]. grid=1 keeps the classic single-row behaviour;
    // grid=4 scans all four onesweep pass rows in one dispatch.
    const int row = g.binary(KOp::Mul, wid, ku(static_cast<crd::u32>(nbins)));

    g.stmt_shared_store(s_tot, tid, g.buffer_load(tot_buf, add(row, tid)));
    g.stmt_barrier();
    for (int stride = 1; stride < nbins; stride *= 2) // Hillis-Steele INCLUSIVE scan
    {
        const int cond    = g.binary(KOp::CmpGe, tid, ku(static_cast<crd::u32>(stride)));
        const int idxsafe = g.select(cond, sub(tid, ku(static_cast<crd::u32>(stride))), tid);
        const int cur     = g.shared_load(s_tot, tid);
        const int prev    = g.shared_load(s_tot, idxsafe);
        const int v       = g.select(cond, add(cur, prev), cur);
        g.stmt_materialize(v);
        g.stmt_barrier();
        g.stmt_shared_store(s_tot, tid, v);
        g.stmt_barrier();
    }
    const int cz   = g.binary(KOp::CmpEq, tid, u0);
    const int pidx = g.select(cz, tid, sub(tid, ku(1)));
    g.stmt_buffer_store(gb_buf, add(row, tid), g.select(cz, u0, g.shared_load(s_tot, pidx))); // exclusive = inclusive[t-1], 0 at t=0

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(nbins);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// *** SCATTER (SUBGROUP rank + LOCAL REORDER -> COALESCED writes): block `wid` writes each key to
// out[gb[digit] + off[wid,digit] + stable block rank]. Rank via subgroup ballots (pt rounds, position order => stable =>
// bit-exact under a linear 32-lane subgroup mapping); every key + digit + rank is REGISTER-staged during the rounds, then the
// block's keys are locally REORDERED in shared (sorted by digit) so the global write phase emits per-digit RUNS (coalesced) --
// the CUB structure. dest bytes identical to a direct scatter (p - lbase[d] == rank). in = keys(0), out = sorted(1),
// off = within-bin block prefix(2), gb = per-bin global base(3). `nbins == threads`, `elems` a multiple of `threads`.
[[nodiscard]] inline KEntry build_sort_scatter(KGraph& g, int elems, int threads, int radix_bits, int shift, int nblocks,
                                               bool carry_val = false)
{
    const int   nbins   = 1 << radix_bits;
    const int   pt      = elems / threads;
    const int   sgs     = threads / 32;             // subgroups per workgroup
    const int   seginit = (sgs * nbins) / threads;  // seg entries each thread zeroes per round
    const Shape sh1     = make_shape({1});
    const auto  ku      = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add     = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul     = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::U32, 0, 0, false);
    const int out_buf = g.buffer_decl(DType::U32, 0, 1, true);
    const int off_buf = g.buffer_decl(DType::U32, 0, 2, false);
    const int gb_buf  = g.buffer_decl(DType::U32, 0, 3, false);
    // ── B19-a3: OPTIONAL PAYLOAD (key-value sort). A per-key value (bindings 4/5) rides the SAME permutation the keys
    //    are scattered by — the ballot/rank/offset logic is computed from keys only and is untouched, so the sort stays
    //    bit-exact; the value is just staged in a parallel shared array and written at the same destination. This makes
    //    the radix sort a general KEY-VALUE sort, which 3DGS (sort splats by depth, carry the index) and any indexed
    //    sort needs. `carry_val == false` ⇒ identical graph to before (existing callers unaffected). ──
    const int val_in  = carry_val ? g.buffer_decl(DType::U32, 0, 4, false) : -1;
    const int val_out = carry_val ? g.buffer_decl(DType::U32, 0, 5, true) : -1;
    const int s_keys  = g.shared_decl(DType::U32, elems);
    const int s_vals  = carry_val ? g.shared_decl(DType::U32, elems) : -1;
    const int seg     = g.shared_decl(DType::U32, sgs * nbins); // per-subgroup per-digit count -> exclusive base (in place)
    const int dhist   = g.shared_decl(DType::U32, nbins);       // cross-round per-digit accumulator
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int sg      = g.binary(KOp::Div, tid, ku(32));
    const int base    = mul(wid, ku(static_cast<crd::u32>(elems)));
    const int u0      = ku(0);

    const int mark = g.kernel_stmt_mark();

    g.stmt_shared_store(dhist, tid, u0); // dhist init (nbins == threads => one per thread)
    g.stmt_materialize(sg);              // hoist sg out of the leader-if scope (else its temp is declared inside the `if`)

    int keyr[64];  // per-round materialized key / digit / block-rank (register-staged for the reorder)
    int digr[64];
    int rnkr[64];
    int valr[64];  // the payload staged the same way, when carry_val
    for (int r = 0; r < pt; ++r) // SOFTWARE PIPELINE: pt independent DRAM loads in flight BEFORE the round barriers trap them
    {
        const int idx = add(base, add(tid, ku(static_cast<crd::u32>(r * threads))));
        keyr[r] = g.buffer_load(in_buf, idx);
        g.stmt_materialize(keyr[r]);
        if (carry_val) { valr[r] = g.buffer_load(val_in, idx); g.stmt_materialize(valr[r]); }
    }
    g.stmt_barrier();

    for (int r = 0; r < pt; ++r)
    {
        for (int j = 0; j < seginit; ++j) { g.stmt_shared_store(seg, add(tid, ku(static_cast<crd::u32>(j * threads))), u0); }
        g.stmt_barrier();

        const int key = keyr[r];
        const int d = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key, ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(nbins - 1)));
        g.stmt_materialize(d);
        // MATCH via radix_bits-ballot intersection. MEASURED faster than the hardware subgroupPartitionNV here (0.43 vs 0.59
        // ms/pass on Ada — the 8 independent ballots pipeline; the partition op serializes). Same mask either way (bit-exact).
        int mask = g.unary(KOp::BitNot, u0);
        for (int bit = 0; bit < radix_bits; ++bit) // branchless: keep = bal ^ (bitv-1) (bitv=1 -> bal; bitv=0 -> ~bal)
        {
            const int bitv = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key, ku(static_cast<crd::u32>(shift + bit))), ku(1));
            const int bal  = g.subgroup_ballot(bitv);
            mask           = g.binary(KOp::BitAnd, mask, g.binary(KOp::BitXor, bal, g.binary(KOp::Sub, bitv, ku(1))));
        }
        g.stmt_materialize(mask); // CRITICAL: freeze the subgroup ops in UNIFORM flow — a lazy re-eval inside the divergent
        const int wsr = g.subgroup_ballot_excl_count(mask); // leader-if would run a full-mask *_sync with inactive lanes = HANG
        g.stmt_materialize(wsr);
        const int sgc   = g.unary(KOp::BitCount, mask);
        const int isldr = g.stmt_if_begin(g.binary(KOp::CmpEq, wsr, u0)); // the per-digit subgroup leader writes the count
        g.stmt_shared_store(seg, add(mul(sg, ku(static_cast<crd::u32>(nbins))), d), sgc);
        g.stmt_if_end(isldr);
        g.stmt_barrier();

        // thread `d`=tid EXCLUSIVE-scans seg[.][d] over subgroups, folding dhist[d] as the base; updates dhist for next round.
        int run = g.shared_load(dhist, tid);
        g.stmt_materialize(run);
        for (int s = 0; s < sgs; ++s)
        {
            const int idx = add(ku(static_cast<crd::u32>(s * nbins)), tid);
            const int c   = g.shared_load(seg, idx);
            g.stmt_materialize(c);
            g.stmt_shared_store(seg, idx, run);
            run = add(run, c);
        }
        g.stmt_shared_store(dhist, tid, run);
        g.stmt_barrier();

        const int rank = add(g.shared_load(seg, add(mul(sg, ku(static_cast<crd::u32>(nbins))), d)), wsr); // block-rank within digit
        g.stmt_materialize(rank);
        g.stmt_barrier(); // rank read of seg must land before the next round zeroes seg
        digr[r] = d; rnkr[r] = rank;
    }

    // lbase = EXCLUSIVE scan of the block digit counts (dhist after the final round). Hillis-Steele in place; seg[0..nbins)
    // (dead after the rounds) becomes lbase.
    const int own = g.shared_load(dhist, tid);
    g.stmt_materialize(own);
    for (int stride = 1; stride < nbins; stride *= 2)
    {
        const int cond    = g.binary(KOp::CmpGe, tid, ku(static_cast<crd::u32>(stride)));
        const int idxsafe = g.select(cond, g.binary(KOp::Sub, tid, ku(static_cast<crd::u32>(stride))), tid);
        const int cur     = g.shared_load(dhist, tid);
        const int prev    = g.shared_load(dhist, idxsafe);
        const int v       = g.select(cond, add(cur, prev), cur);
        g.stmt_materialize(v);
        g.stmt_barrier();
        g.stmt_shared_store(dhist, tid, v);
        g.stmt_barrier();
    }
    const int lb = g.binary(KOp::Sub, g.shared_load(dhist, tid), own); // inclusive - own = exclusive
    g.stmt_materialize(lb);
    g.stmt_shared_store(seg, tid, lb); // seg[0..nbins) = lbase
    g.stmt_barrier();

    // LOCAL REORDER: every key is register-staged => s_keys becomes the block's keys sorted by digit (stable).
    for (int r = 0; r < pt; ++r)
    {
        const int loc = add(g.shared_load(seg, digr[r]), rnkr[r]);
        g.stmt_materialize(loc);
        g.stmt_shared_store(s_keys, loc, keyr[r]);
        if (carry_val) { g.stmt_shared_store(s_vals, loc, valr[r]); } // the value rides the same local slot as its key
    }
    g.stmt_barrier();

    // COALESCED write: consecutive threads take consecutive locally-sorted positions => destinations are runs per digit.
    // dest = gb[d] + off[d,wid] (BIN-major) + (p - lbase[d]) -- (p - lbase[d]) == rank, identical bytes to a direct scatter.
    for (int k = 0; k < pt; ++k)
    {
        const int p2   = add(tid, ku(static_cast<crd::u32>(k * threads)));
        const int key2 = g.shared_load(s_keys, p2);
        const int d2   = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key2, ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(nbins - 1)));
        const int offv = g.buffer_load(off_buf, add(mul(d2, ku(static_cast<crd::u32>(nblocks))), wid));
        const int dest = add(add(g.buffer_load(gb_buf, d2), offv), g.binary(KOp::Sub, p2, g.shared_load(seg, d2)));
        g.stmt_materialize(dest);
        g.stmt_buffer_store(out_buf, dest, key2);
        if (carry_val) { g.stmt_buffer_store(val_out, dest, g.shared_load(s_vals, p2)); } // value follows its key
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}



// ============================ ONESWEEP (integer decoupled-lookback -- bit-exact: u32 sums are order-independent) =============
// Structure per sort: [clear aux + fused GHIST] -> gbase4 (grid=4) -> 4 lookback scatters. Kills the per-pass histogram and
// offset kernels entirely (their info is recovered in-scatter from predecessors via the lookback). The aux buffer is
// [ghist 4*nbins | look 4*nblocks*nbins], each look cell = (count_or_prefix << 2) | status (0 none / 1 aggregate / 2 prefix).

// FUSED GLOBAL HISTOGRAM: one N-read computes ALL FOUR 8-bit digit histograms (global totals -- permutation-invariant, so one
// upfront pass serves every LSD pass). Per-block shared 4*nbins counts, then buffer-ATOMIC-add into aux[digit_pass*nbins+bin]
// (SUM => order-independent => bit-exact). in = keys(0), aux(1, rw; ghist region pre-zeroed). `nbins == threads`.
[[nodiscard]] inline KEntry build_sort_ghist(KGraph& g, int elems, int threads, int radix_bits)
{
    const int   nbins = 1 << radix_bits;
    const int   mask  = nbins - 1;
    const int   pt    = elems / threads;
    const Shape sh1   = make_shape({1});
    const auto  ku    = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add   = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul   = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::U32, 0, 0, false);
    const int aux_buf = g.buffer_decl(DType::U32, 0, 1, true);
    const int s_h     = g.shared_decl(DType::U32, 4 * nbins);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int base    = mul(wid, ku(static_cast<crd::u32>(elems)));
    const int uone    = g.constant(1.0, sh1, DType::U32);
    const int mark    = g.kernel_stmt_mark();

    for (int j = 0; j < 4 * nbins / threads; ++j) { g.stmt_shared_store(s_h, add(tid, ku(static_cast<crd::u32>(j * threads))), ku(0)); }
    g.stmt_barrier();
    for (int k = 0; k < pt; ++k)
    {
        const int key = g.buffer_load(in_buf, add(base, add(tid, ku(static_cast<crd::u32>(k * threads)))));
        g.stmt_materialize(key);
        for (int pass = 0; pass < 4; ++pass)
        {
            const int d = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key, ku(static_cast<crd::u32>(pass * 8))), ku(static_cast<crd::u32>(mask)));
            g.stmt_shared_atomic_add(s_h, add(ku(static_cast<crd::u32>(pass * nbins)), d), uone);
        }
    }
    g.stmt_barrier();
    for (int j = 0; j < 4 * nbins / threads; ++j) // merge into the global ghist (aux[0 .. 4*nbins))
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(j * threads)));
        g.stmt_buffer_atomic_add(aux_buf, idx, g.shared_load(s_h, idx));
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// AUX CLEAR: zero the [ghist | look] buffer before each sort. grid = ceil(total / (threads*8)). aux(0, rw).
[[nodiscard]] inline KEntry build_sort_clear(KGraph& g, int total_words, int threads)
{
    const Shape sh1  = make_shape({1});
    const auto  ku   = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const int aux_buf = g.buffer_decl(DType::U32, 0, 0, true);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int mark    = g.kernel_stmt_mark();
    const int base    = mul(wid, ku(static_cast<crd::u32>(threads * 8)));
    for (int k = 0; k < 8; ++k)
    {
        const int idx = add(base, add(tid, ku(static_cast<crd::u32>(k * threads))));
        const int ifg = g.stmt_if_begin(g.binary(KOp::CmpLt, idx, ku(static_cast<crd::u32>(total_words))));
        g.stmt_buffer_store(aux_buf, idx, ku(0));
        g.stmt_if_end(ifg);
    }
    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// *** ONESWEEP SCATTER: the reorder scatter with the per-pass histogram+offset REPLACED by a decoupled LOOKBACK. After the
// rank rounds (dhist = this block's digit counts), thread d publishes (count<<2|1) -- or (count<<2|2) for block 0 -- to its
// look cell, then WALKS BACK over predecessors: spin until published, add count, BREAK on a prefix (integer sums => the
// result is identical regardless of upgrade races => bit-exact). It then publishes its own inclusive prefix and the write
// phase uses dest = gb[pass][d] + excl[d] + (p - lbase[d]). Relies on GPU forward progress (predecessor blocks scheduled) --
// the standard onesweep assumption, same as the chained scan. in = keys(0), out(1), gb(2: 4*nbins), aux(3: COHERENT
// [ghist|look]). `nbins == threads`, `elems` a multiple of `threads`.
[[nodiscard]] inline KEntry build_sort_scatter_onesweep(KGraph& g, int elems, int threads, int radix_bits, int shift,
                                                        int pass, int nblocks, bool hw_match = false)
{
    const int   nbins   = 1 << radix_bits;
    const int   pt      = elems / threads;
    const int   sgs     = threads / 32;
    const int   seginit = (sgs * nbins) / threads;
    const Shape sh1     = make_shape({1});
    const auto  ku      = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add     = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul     = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::U32, 0, 0, false);
    const int out_buf = g.buffer_decl(DType::U32, 0, 1, true);
    const int gb_buf  = g.buffer_decl(DType::U32, 0, 2, false);
    const int aux_buf = g.buffer_decl_coherent(DType::U32, 0, 3);
    const int s_keys  = g.shared_decl(DType::U32, elems);
    const int seg     = g.shared_decl(DType::U32, sgs * nbins); // rounds workspace; row0 -> lbase, row1 -> excl afterwards
    const int dhist   = g.shared_decl(DType::U32, nbins);
    const int s_tk    = g.shared_decl(DType::U32, 1);          // the block's DYNAMIC position (atomic ticket)
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int sg      = g.binary(KOp::Div, tid, ku(32));
    const int u0      = ku(0);
    const int lookb   = ku(static_cast<crd::u32>(4 * nbins + 4 + pass * nblocks * nbins)); // look base (after ghist + 4 tickets)

    const int mark = g.kernel_stmt_mark();

    // FORWARD PROGRESS: take a dynamic ticket as the block id — blockIdx launch order is NOT guaranteed (deadlocked on Ada);
    // with tickets the resident blocks always hold the lowest unprocessed positions, so every spin's predecessor is running.
    g.stmt_buffer_ticket(aux_buf, ku(static_cast<crd::u32>(4 * nbins + pass)), s_tk);
    g.stmt_shared_store(dhist, tid, u0);
    g.stmt_materialize(sg);
    g.stmt_barrier();
    const int wid  = g.shared_load(s_tk, u0); // virtual block id (ticket)
    g.stmt_materialize(wid);
    const int base = mul(wid, ku(static_cast<crd::u32>(elems)));

    // *** WARP-SYNCHRONOUS RANK (the CUB structure): warp `sg` owns the CONTIGUOUS chunk [sg*cpw, (sg+1)*cpw) so position
    // order == (warp, round, lane) == the rank order (stability preserved). Each warp accumulates its per-digit counters in
    // seg[sg][.] across its rounds with only WARP syncs -- ZERO block barriers in the rank loop (was 4 per round). Then ONE
    // cross-warp scan per digit converts counters to warp bases; rank = wbase[sg][d] + within-warp running rank.
    const int cpw  = elems / sgs;                       // keys per warp (contiguous chunk)
    const int lane = g.binary(KOp::BitAnd, tid, ku(31));
    g.stmt_materialize(lane);
    const int gwb = add(base, mul(sg, ku(static_cast<crd::u32>(cpw)))); // this warp's chunk base in global memory
    for (int j = 0; j < seginit; ++j) { g.stmt_shared_store(seg, add(tid, ku(static_cast<crd::u32>(j * threads))), u0); } // zero counters ONCE (GPU shared is uninitialized)

    int keyr[64];
    int rnkw[64]; // within-warp running rank (prev + wsr) -- deferred exprs over materialized nodes (safe)
    for (int r = 0; r < pt; ++r) // hoisted loads: pt independent 128B warp-contiguous reads in flight
    {
        keyr[r] = g.buffer_load(in_buf, add(gwb, add(lane, ku(static_cast<crd::u32>(r * 32)))));
        g.stmt_materialize(keyr[r]);
    }
    g.stmt_barrier(); // init section done: ticket + dhist zeros + counter zeros all visible

    for (int r = 0; r < pt; ++r) // NO block barriers below -- warp-synchronous
    {
        const int key = keyr[r];
        const int d = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key, ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(nbins - 1)));
        g.stmt_materialize(d);
        int mask;
        if (hw_match) { mask = g.subgroup_match(d); } // CUDA __match_any_sync: 1 hardware op (bit-exact: same mask)
        else
        {
            mask = g.unary(KOp::BitNot, u0);
            for (int bit = 0; bit < radix_bits; ++bit) // branchless: keep = bal ^ (bitv-1)
            {
                const int bitv = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key, ku(static_cast<crd::u32>(shift + bit))), ku(1));
                const int bal  = g.subgroup_ballot(bitv);
                mask           = g.binary(KOp::BitAnd, mask, g.binary(KOp::BitXor, bal, g.binary(KOp::Sub, bitv, ku(1))));
            }
        }
        g.stmt_materialize(mask); // freeze subgroup ops in UNIFORM flow (lazy re-eval inside the leader-if = *_sync hang)
        const int wsr = g.subgroup_ballot_excl_count(mask);
        g.stmt_materialize(wsr);
        const int prev = g.shared_load(seg, add(mul(sg, ku(static_cast<crd::u32>(nbins))), d)); // warp's digit count so far
        g.stmt_materialize(prev);
        g.stmt_sync_warp(); // all lanes' prev reads land before the leader's update
        const int isldr = g.stmt_if_begin(g.binary(KOp::CmpEq, wsr, u0));
        g.stmt_shared_store(seg, add(mul(sg, ku(static_cast<crd::u32>(nbins))), d), add(prev, g.unary(KOp::BitCount, mask)));
        g.stmt_if_end(isldr);
        g.stmt_sync_warp(); // leader's update visible before the next round reads
        rnkw[r] = add(prev, wsr);
    }
    g.stmt_barrier(); // all warps' counters final

    // ONE cross-warp exclusive scan per digit (thread d owns digit d): seg[w][d] -> warp base; dhist[d] = block total.
    int run = u0;
    for (int si = 0; si < sgs; ++si)
    {
        const int idx = add(ku(static_cast<crd::u32>(si * nbins)), tid);
        const int c   = g.shared_load(seg, idx);
        g.stmt_materialize(c);
        g.stmt_shared_store(seg, idx, run);
        run = add(run, c);
    }
    g.stmt_shared_store(dhist, tid, run);
    g.stmt_barrier();

    int rnkr[64];
    for (int r = 0; r < pt; ++r) // block rank = warp base + within-warp rank (freeze before seg rows get reused)
    {
        const int dr = g.binary(KOp::BitAnd, g.binary(KOp::Shr, keyr[r], ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(nbins - 1))); // recompute (16 fewer registers)
        rnkr[r] = add(g.shared_load(seg, add(mul(sg, ku(static_cast<crd::u32>(nbins))), dr)), rnkw[r]);
        g.stmt_materialize(rnkr[r]);
    }
    g.stmt_barrier(); // rank reads done before the lookback/lbase phases reuse seg rows

    // LOOKBACK. seg row1 = excl accumulator (per digit). Publish own aggregate (block 0: prefix) FIRST so successors progress.
    const int ex_idx = add(ku(static_cast<crd::u32>(nbins)), tid); // seg row1 cell for digit `tid`
    g.stmt_shared_store(seg, ex_idx, u0);
    const int cnt = g.shared_load(dhist, tid);
    g.stmt_materialize(cnt);
    const int pub_idx = add(lookb, add(mul(wid, ku(static_cast<crd::u32>(nbins))), tid));
    const int cw0  = g.binary(KOp::CmpEq, wid, u0);
    const int stat = g.select(cw0, ku(2), ku(1)); // block 0 publishes its PREFIX (= its own count) immediately
    g.stmt_buffer_store(aux_buf, pub_idx, g.binary(KOp::BitOr, g.binary(KOp::Shl, cnt, ku(2)), stat));
    {
        const int ifl = g.stmt_if_begin(g.binary(KOp::CmpGt, wid, u0)); // walk back: pred = wid-1-j
        const int f   = g.stmt_for_begin(wid);
        const int j   = g.kernel_loop_var(f);
        const int prd = g.binary(KOp::Sub, g.binary(KOp::Sub, wid, ku(1)), j);
        const int look_idx  = add(lookb, add(mul(prd, ku(static_cast<crd::u32>(nbins))), tid));
        g.stmt_spin_until_nonzero(aux_buf, look_idx);
        const int v = g.buffer_load(aux_buf, look_idx);
        g.stmt_materialize(v);
        const int r2 = g.shared_load(seg, ex_idx);
        g.stmt_materialize(r2);
        g.stmt_shared_store(seg, ex_idx, add(r2, g.binary(KOp::Shr, v, ku(2))));
        g.stmt_for_break_if(g.binary(KOp::CmpEq, g.binary(KOp::BitAnd, v, ku(3)), ku(2))); // prefix reached: sum complete
        g.stmt_for_end(f);
        const int ex = g.shared_load(seg, ex_idx);
        g.stmt_materialize(ex);
        g.stmt_buffer_store(aux_buf, pub_idx, g.binary(KOp::BitOr, g.binary(KOp::Shl, add(ex, cnt), ku(2)), ku(2))); // publish prefix
        g.stmt_if_end(ifl);
    }
    g.stmt_barrier();

    // lbase = exclusive scan of dhist -> seg row0 (destroys dhist; excl in row1 stays).
    const int own = g.shared_load(dhist, tid);
    g.stmt_materialize(own);
    for (int stride = 1; stride < nbins; stride *= 2)
    {
        const int cond    = g.binary(KOp::CmpGe, tid, ku(static_cast<crd::u32>(stride)));
        const int idxsafe = g.select(cond, g.binary(KOp::Sub, tid, ku(static_cast<crd::u32>(stride))), tid);
        const int cur     = g.shared_load(dhist, tid);
        const int prev    = g.shared_load(dhist, idxsafe);
        const int v       = g.select(cond, add(cur, prev), cur);
        g.stmt_materialize(v);
        g.stmt_barrier();
        g.stmt_shared_store(dhist, tid, v);
        g.stmt_barrier();
    }
    const int lb = g.binary(KOp::Sub, g.shared_load(dhist, tid), own);
    g.stmt_materialize(lb);
    g.stmt_shared_store(seg, tid, lb);
    g.stmt_barrier();

    for (int r = 0; r < pt; ++r)
    {
        const int dr = g.binary(KOp::BitAnd, g.binary(KOp::Shr, keyr[r], ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(nbins - 1)));
        g.stmt_shared_store(s_keys, add(g.shared_load(seg, dr), rnkr[r]), keyr[r]); // padding MEASURED WORSE (random addrs ~conflict-free)
    }
    g.stmt_barrier();

    for (int k = 0; k < pt; ++k) // dest = gb[pass][d] + excl[d] + (p - lbase[d])
    {
        const int p2   = add(tid, ku(static_cast<crd::u32>(k * threads)));
        const int key2 = g.shared_load(s_keys, p2);
        const int d2   = g.binary(KOp::BitAnd, g.binary(KOp::Shr, key2, ku(static_cast<crd::u32>(shift))), ku(static_cast<crd::u32>(nbins - 1)));
        const int gbv  = g.buffer_load(gb_buf, add(ku(static_cast<crd::u32>(pass * nbins)), d2));
        const int exv  = g.shared_load(seg, add(ku(static_cast<crd::u32>(nbins)), d2));
        const int dest = add(add(gbv, exv), g.binary(KOp::Sub, p2, g.shared_load(seg, d2)));
        g.stmt_buffer_store(out_buf, dest, key2);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir
