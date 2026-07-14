#pragma once

// ckir_scan.hpp — B-cmp: a device-wide PREFIX SUM (SCAN) authored as a CKIR shared-memory compute kernel. The keystone
// primitive: stream compaction (B8-l clustered light-cull, B4 visibility cluster-cull, particle compaction) AND radix sort
// (per-digit offsets) build on it. The CUB `DeviceScan`-class primitive — one source, bit-exact CPU oracle + Vulkan + DX12.
//
// Work-efficient 3-pass device scan (no atomics ⇒ portable + deterministic ⇒ bit-exact):
//   pass 0 `build_scan_block`  — each workgroup scans its contiguous span → the local scan (in place) + the span TOTAL to
//                                blocksums[w].
//   pass 1 `build_scan_block`  — exclusive-scan the blocksums (ONE workgroup) → block OFFSETS.
//   pass 2 `build_scan_addoff` — add offset[w] to every element of block w's local scan → the final device scan.
//
// The block scan is COALESCED: a STRIPED global load (thread t ← x[base + t + k·threads]) fills shared in natural order, then
// a BLOCKED shared scan (thread t owns shared chunk [t·pt, (t+1)·pt)) — phase 1 local scans, phase 2 a Hillis-Steele
// cross-thread scan of the thread totals (branchless via `Select` + a CLAMPED index, `Materialize` across the barriers, the
// FFT ping pattern), phase 3 adds each thread's exclusive prefix — then a STRIPED store. `elems` a multiple of `threads`,
// `threads` a power of two. `inclusive` = inclusive vs exclusive scan. Order is fixed at authoring time ⇒ the CPU oracle runs
// the identical graph (sum bit-exact vs the GPU). Buffers (set 0): in=0 (ro), out_scan=1 (rw), out_blocksum=2 (rw).

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

struct ScanPlan
{
    KEntry   block;                 // pass 0: grid = nblocks, each scans its span → local scan + blocksums[w]
    KEntry   scan_sums;             // pass 1: grid = 1, EXCLUSIVE-scan the nblocks blocksums → offsets
    KEntry   add_off;               // pass 2: grid = nblocks, out[i] = local_scan[i] + offset[w]
    KGraph*  block_graph     = nullptr;
    KGraph*  sums_graph      = nullptr;
    KGraph*  addoff_graph    = nullptr;
    int      n               = 0;
    int      threads         = 0;
    int      nblocks         = 0;
    int      elems_per_block = 0;
    bool     inclusive       = true;
    bool     single_pass     = false; // n ≤ one block ⇒ pass 0 alone is the whole scan (no blocksum/offset)
};

// Block scan: workgroup w scans its contiguous span [w·elems, (w+1)·elems) → out_scan (same positions) + out_blocksum[w]
// (the span total). Coalesced striped I/O, blocked shared scan.
[[nodiscard]] inline KEntry build_scan_block(KGraph& g, int elems, int threads, bool inclusive, bool write_blocksum)
{
    const int   pt  = elems / threads;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf   = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_buf  = g.buffer_decl(DType::F32, 0, 1, true);
    const int bsum_buf = g.buffer_decl(DType::F32, 0, 2, true);
    const int data     = g.shared_decl(DType::F32, elems);   // the block's values, natural order
    const int tsum     = g.shared_decl(DType::F32, threads); // per-thread chunk totals → cross-thread scan
    const int tid      = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid      = g.builtin(KBuiltin::WorkgroupIndex);
    const int base     = mul(wid, ku(static_cast<crd::u32>(elems)));
    const int fzero    = g.constant(0.0, sh1, DType::F32);

    const int mark = g.kernel_stmt_mark();

    // STRIPED coalesced load: data[t + k·threads] = in[base + t + k·threads].
    for (int k = 0; k < pt; ++k)
    {
        const int off = add(tid, ku(static_cast<crd::u32>(k * threads)));
        g.stmt_shared_store(data, off, g.buffer_load(in_buf, add(base, off)));
    }
    g.stmt_barrier();

    // Phase 1: thread t local-scans its BLOCKED chunk data[t·pt, (t+1)·pt) in place (inclusive) → tsum[t] = chunk total.
    // ⛔ CKIR re-reads shared LAZILY, so a running accumulator that reads-then-writes the SAME array corrupts on re-eval
    // (the FFT ping-pong scar). FREEZE the pt originals into registers (Materialize) FIRST, scan in registers, then write.
    const int c0 = mul(tid, ku(static_cast<crd::u32>(pt)));
    int       rr[64];
    for (int k = 0; k < pt; ++k)
    {
        rr[k] = g.shared_load(data, add(c0, ku(static_cast<crd::u32>(k))));
        g.stmt_materialize(rr[k]);
    }
    int s = rr[0];
    g.stmt_shared_store(data, c0, s);
    for (int k = 1; k < pt; ++k)
    {
        s = add(s, rr[k]);
        g.stmt_shared_store(data, add(c0, ku(static_cast<crd::u32>(k))), s);
    }
    g.stmt_shared_store(tsum, tid, s);
    g.stmt_barrier();

    // Phase 2: Hillis-Steele INCLUSIVE cross-thread scan of tsum[0..threads-1]. Branchless (Select + clamped index),
    // Materialize the new value across the read→write barrier.
    for (int stride = 1; stride < threads; stride *= 2)
    {
        const int cond    = g.binary(KOp::CmpGe, tid, ku(static_cast<crd::u32>(stride)));
        const int idxsafe = g.select(cond, sub(tid, ku(static_cast<crd::u32>(stride))), tid); // t for t<stride (in bounds)
        const int cur     = g.shared_load(tsum, tid);
        const int prev    = g.shared_load(tsum, idxsafe);
        const int v       = g.select(cond, add(cur, prev), cur);
        g.stmt_materialize(v);
        g.stmt_barrier();
        g.stmt_shared_store(tsum, tid, v);
        g.stmt_barrier();
    }

    // Phase 3: exclusive prefix of THIS thread = (t==0)?0:tsum[t-1]; add it to the whole chunk.
    const int cz      = g.binary(KOp::CmpEq, tid, ku(0));
    const int pidx    = g.select(cz, tid, sub(tid, ku(1))); // t for t==0 (in bounds)
    const int excl    = g.select(cz, fzero, g.shared_load(tsum, pidx));
    g.stmt_materialize(excl);
    for (int k = 0; k < pt; ++k)
    {
        const int idx = add(c0, ku(static_cast<crd::u32>(k)));
        g.stmt_shared_store(data, idx, add(g.shared_load(data, idx), excl));
    }
    g.stmt_barrier();

    // STRIPED coalesced store. Inclusive ⇒ data[i]; exclusive ⇒ data[i] − (the original element) = data[i-1]-style; we emit
    // exclusive as (inclusive − in[i]) which is the exclusive prefix (sum of strictly-earlier elements).
    for (int k = 0; k < pt; ++k)
    {
        const int off  = add(tid, ku(static_cast<crd::u32>(k * threads)));
        const int incl = g.shared_load(data, off);
        const int val  = inclusive ? incl : sub(incl, g.buffer_load(in_buf, add(base, off)));
        g.stmt_buffer_store(out_buf, add(base, off), val);
    }

    // block total = data[elems-1] (inclusive scan's last) → blocksums[w] (thread 0 writes).
    if (write_blocksum)
    {
        const int c0w = g.binary(KOp::CmpEq, tid, ku(0));
        const int ifw = g.stmt_if_begin(c0w);
        g.stmt_buffer_store(bsum_buf, wid, g.shared_load(data, ku(static_cast<crd::u32>(elems - 1))));
        g.stmt_if_end(ifw);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ⭐⭐⭐ SINGLE-PASS CHAINED SCAN — the 2N-traffic scan that MATCHES CUB (read N once, write N once), the only way to beat a
// portable multi-pass. Each block (WorkgroupIndex = bid) scans its span (COALESCED, same as build_scan_block), then a CHAINED
// LOOK-BACK: block bid SPINS on the coherent `flag[bid-1]` until its predecessor publishes, reads the predecessor's inclusive
// prefix `agg[bid-1]`, adds it as the block's exclusive base, and PUBLISHES its own inclusive prefix (agg[bid] = excl + blocksum,
// then a device barrier, then flag[bid]=1). Bit-exact: the sum order (excl + blocksum) is fixed, and the CPU oracle runs
// workgroups SEQUENTIALLY so the spin is a no-op there. ⚠ relies on GPU FORWARD PROGRESS (blocks scheduled ~in order) — the
// standard single-pass-scan assumption (CUB's too); works on desktop GPUs. Buffers (set 0): in=0 (ro), out=1 (rw), agg=2
// (coherent f32), flag=3 (coherent u32, ZERO-initialized). `flag` must be cleared to 0 before the dispatch.
[[nodiscard]] inline KEntry build_scan_single_pass(KGraph& g, int elems, int threads, bool inclusive)
{
    const int   pt  = elems / threads;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_buf = g.buffer_decl(DType::F32, 0, 1, true);
    const int agg_buf = g.buffer_decl_coherent(DType::F32, 0, 2); // published inclusive prefix per block
    const int flg_buf = g.buffer_decl_coherent(DType::U32, 0, 3); // ready flag per block (0 = not ready)
    const int data    = g.shared_decl(DType::F32, elems);
    const int tsum    = g.shared_decl(DType::F32, threads);
    const int sh_excl = g.shared_decl(DType::F32, 1); // the block's exclusive base, broadcast to all threads
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int base    = mul(wid, ku(static_cast<crd::u32>(elems)));
    const int fzero   = g.constant(0.0, sh1, DType::F32);
    const int uone    = g.constant(1.0, sh1, DType::U32);

    const int mark = g.kernel_stmt_mark();

    // striped coalesced load
    for (int k = 0; k < pt; ++k)
    {
        const int off = add(tid, ku(static_cast<crd::u32>(k * threads)));
        g.stmt_shared_store(data, off, g.buffer_load(in_buf, add(base, off)));
    }
    g.stmt_barrier();

    // phase 1: blocked local inclusive scan (freeze originals first — CKIR lazy shared re-read)
    const int c0 = mul(tid, ku(static_cast<crd::u32>(pt)));
    int       rr[64];
    for (int k = 0; k < pt; ++k) { rr[k] = g.shared_load(data, add(c0, ku(static_cast<crd::u32>(k)))); g.stmt_materialize(rr[k]); }
    int s = rr[0];
    g.stmt_shared_store(data, c0, s);
    for (int k = 1; k < pt; ++k) { s = add(s, rr[k]); g.stmt_shared_store(data, add(c0, ku(static_cast<crd::u32>(k))), s); }
    g.stmt_shared_store(tsum, tid, s);
    g.stmt_barrier();

    // phase 2: Hillis-Steele cross-thread inclusive scan of tsum
    for (int stride = 1; stride < threads; stride *= 2)
    {
        const int cond    = g.binary(KOp::CmpGe, tid, ku(static_cast<crd::u32>(stride)));
        const int idxsafe = g.select(cond, sub(tid, ku(static_cast<crd::u32>(stride))), tid);
        const int cur     = g.shared_load(tsum, tid);
        const int prev    = g.shared_load(tsum, idxsafe);
        const int v       = g.select(cond, add(cur, prev), cur);
        g.stmt_materialize(v);
        g.stmt_barrier();
        g.stmt_shared_store(tsum, tid, v);
        g.stmt_barrier();
    }

    // phase 3: add each thread's within-block exclusive prefix to its chunk ⇒ data = block-local inclusive scan
    const int cz   = g.binary(KOp::CmpEq, tid, ku(0));
    const int pidx = g.select(cz, tid, sub(tid, ku(1)));
    const int texc = g.select(cz, fzero, g.shared_load(tsum, pidx));
    g.stmt_materialize(texc);
    for (int k = 0; k < pt; ++k)
    {
        const int idx = add(c0, ku(static_cast<crd::u32>(k)));
        g.stmt_shared_store(data, idx, add(g.shared_load(data, idx), texc));
    }
    g.stmt_barrier();

    // CHAINED LOOK-BACK (thread 0): wait for the predecessor, read its published inclusive prefix, publish ours. Bit-exact
    // (fixed order) but SERIALIZES the prefix propagation ⇒ slow. ⛔ FUNDAMENTAL: the FAST single-pass (CUB's decoupled
    // look-back) sums the exclusive prefix in a TIMING-DEPENDENT order (aggregates vs prefixes) ⇒ NON-deterministic f32 rounding
    // ⇒ NOT bit-exact — incompatible with our all-backends-bit-exact mission. So a bit-exact portable scan CANNOT crush CUB;
    // this chained form is the correct single-pass demonstration. The portable 3-pass (build_scan) is the working scan.
    {
        const int c0t = g.binary(KOp::CmpEq, tid, ku(0));
        const int ift = g.stmt_if_begin(c0t);
        const int blocksum = g.shared_load(data, ku(static_cast<crd::u32>(elems - 1))); // block total (inclusive last)
        g.stmt_materialize(blocksum);
        // excl = (wid > 0) ? agg[wid-1] : 0. ⛔ GPU shared is NOT zero-initialized (the oracle zeros it) ⇒ write sh_excl=0
        // FIRST, then overwrite for wid>0 after spinning for the predecessor's publish.
        g.stmt_shared_store(sh_excl, ku(0), fzero);
        const int hasp = g.binary(KOp::CmpGt, wid, ku(0));
        const int ifp  = g.stmt_if_begin(hasp);
        g.stmt_spin_until_nonzero(flg_buf, sub(wid, ku(1)));
        g.stmt_shared_store(sh_excl, ku(0), g.buffer_load(agg_buf, sub(wid, ku(1)))); // excl = agg[wid-1]
        g.stmt_if_end(ifp);
        const int excl = g.shared_load(sh_excl, ku(0));
        g.stmt_materialize(excl);
        g.stmt_buffer_store(agg_buf, wid, add(excl, blocksum)); // publish inclusive prefix
        g.stmt_barrier(BarrierScope::Buffer);                    // agg visible BEFORE the flag
        g.stmt_buffer_store(flg_buf, wid, uone);                 // publish ready
        g.stmt_if_end(ift);
    }
    g.stmt_barrier(); // all threads read sh_excl

    const int excl_all = g.shared_load(sh_excl, ku(0));
    g.stmt_materialize(excl_all);
    for (int k = 0; k < pt; ++k)
    {
        const int off  = add(tid, ku(static_cast<crd::u32>(k * threads)));
        const int incl = add(g.shared_load(data, off), excl_all);          // block-local + block-exclusive base = global scan
        const int val  = inclusive ? incl : sub(incl, g.buffer_load(in_buf, add(base, off)));
        g.stmt_buffer_store(out_buf, add(base, off), val);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Pass 2: out[base + i] = in_scan[base + i] + offset[w], for the whole span (coalesced). in=0, offsets=1, out=2.
[[nodiscard]] inline KEntry build_scan_addoff(KGraph& g, int elems, int threads)
{
    const int   pt  = elems / threads;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int off_buf = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_buf = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int base    = mul(wid, ku(static_cast<crd::u32>(elems)));

    const int mark = g.kernel_stmt_mark();
    const int off  = g.buffer_load(off_buf, wid); // this block's exclusive prefix over all earlier blocks
    for (int k = 0; k < pt; ++k)
    {
        const int idx = add(base, add(tid, ku(static_cast<crd::u32>(k * threads))));
        g.stmt_buffer_store(out_buf, idx, add(g.buffer_load(in_buf, idx), off));
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Author a device-wide scan of `n` elements. `graphs` = THREE caller-owned KGraphs (pass 0, pass 1, pass 2). `nblocks` must be
// ≤ `elems_per_block` (so pass 1 scans the blocksums in ONE workgroup) and both n/nblocks and nblocks are multiples of a
// power-of-two thread count. Single-pass when n fits one workgroup.
[[nodiscard]] inline ScanPlan build_scan(KGraph** graphs, int n, bool inclusive, int threads = 256, int nblocks = 64)
{
    ScanPlan plan;
    plan.n         = n;
    plan.threads   = threads;
    plan.inclusive = inclusive;

    if (n <= threads * 8)
    {
        int t = threads;
        while (t > n) { t /= 2; }
        plan.single_pass     = true;
        plan.nblocks         = 1;
        plan.elems_per_block = n;
        plan.threads         = t;
        plan.block           = build_scan_block(*graphs[0], n, t, inclusive, false);
        plan.block_graph     = graphs[0];
        return plan;
    }

    plan.single_pass     = false;
    plan.nblocks         = nblocks;
    plan.elems_per_block = n / nblocks;
    plan.block           = build_scan_block(*graphs[0], plan.elems_per_block, threads, inclusive, true);
    plan.block_graph     = graphs[0];
    // pass 1: EXCLUSIVE scan of the nblocks blocksums in one workgroup (threads capped to nblocks).
    int st = threads;
    while (st > nblocks) { st /= 2; }
    plan.scan_sums   = build_scan_block(*graphs[1], nblocks, st, false, false);
    plan.sums_graph  = graphs[1];
    plan.add_off     = build_scan_addoff(*graphs[2], plan.elems_per_block, threads);
    plan.addoff_graph = graphs[2];
    return plan;
}

} // namespace crd::kir
