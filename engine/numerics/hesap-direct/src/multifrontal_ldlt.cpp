#include <crd/core/assert.hpp>
#include <crd/hesap/direct/dense_ldlt_kernels.hpp> // factor_front_ldlt (v5d-b)
#include <crd/hesap/direct/multifrontal_ldlt.hpp>
#include <crd/hesap/ordering/symbolic.hpp> // ordering::kNoParent (amalgamation front-parent remap)
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace crd::hesap::direct
{
namespace
{
// v5d-h RELAXED-FRONT AMALGAMATION — a faithful port of CHOLMOD's `cholmod_super_symbolic` relaxed merge (the
// lever that closes the front-overhead gap vs MUMPS: fundamental supernodes leave ~n tiny SINGLETON fronts
// running scalar/BLAS-2 + per-front assembly overhead, while CHOLMOD/MUMPS merge them into far fewer FAT
// BLAS-3 fronts with bigger fully-summed blocks ⇒ fewer delays). It merges ADJACENT fronts s and s+1 (chains
// in the supernode etree, tracked by union-find) ONLY when the new explicit zeros stay within the graduated
// nrelax/zrelax bound — so it is FILL-AWARE and never collapses sparse siblings into a dense front (that
// exploded the fill → OOM; the earlier subtree-collapse was the wrong model). nrelax/zrelax = CHOLMOD defaults.
// Merged front rows = [the chain's contiguous pivots] ++ [the chain TOP's contribution block] (the top's CB is
// the union of all members' external CBs by the etree fill-propagation theorem). Pure function of the pattern
// ⇒ deterministic ⇒ the moat holds.
MultifrontalSymbolic amalgamate_fronts(const MultifrontalSymbolic& mf, crd::u32 relax, crd::memory::IAllocator* alloc)
{
    const crd::u32 nf = mf.nfront;
    const crd::u32 n = mf.n;
    MultifrontalSymbolic out(alloc);
    out.n = n;
    if (nf == 0 || relax <= 1)
    {
        out.nfront = nf;
        out.pivot_first = mf.pivot_first;
        out.front_parent = mf.front_parent;
        out.row_ptr = mf.row_ptr;
        out.row_idx = mf.row_idx;
        out.col_ptr = mf.col_ptr;
        out.col_idx = mf.col_idx;
        return out;
    }
    constexpr crd::u32 empty_id = 0xFFFFFFFFU;
    const crd::u32 nrelax0 = (relax < 2) ? 4U : relax; // merge unconditionally if merged front cols ≤ this
    constexpr crd::u32 nrelax1 = 16;
    constexpr crd::u32 nrelax2 = 48;
    constexpr double zrelax0 = 0.8;
    constexpr double zrelax1 = 0.1;
    constexpr double zrelax2 = 0.05;
    crd::containers::Array<crd::u32> merged(alloc); // merged[s] = the front s was merged into (empty_id = live)
    crd::containers::Array<crd::u32> nscol(alloc);  // # columns in the (relaxed) supernode rooted at s
    crd::containers::Array<crd::u32> snz(alloc);     // # rows in the (relaxed) front's leading column
    crd::containers::Array<crd::u64> zeros(alloc);   // cumulative explicit zeros in the relaxed front
    merged.resize(nf);
    nscol.resize(nf);
    snz.resize(nf);
    zeros.resize(nf);
    for (crd::u32 s = 0; s < nf; ++s)
    {
        merged[s] = empty_id;
        nscol[s] = mf.npiv(s);
        snz[s] = mf.row_ptr[s + 1] - mf.row_ptr[s]; // entries in s's leading column = its front's row count
        zeros[s] = 0;
    }
    // Merge ADJACENT fronts s and s+1 — and ONLY when s+1 is s's CURRENT parent (a chain; union-find path
    // compression tracks the parent as lower fronts merge) — when the new explicit zeros stay within the
    // graduated zrelax bound. Fill-aware ⇒ it merges the nested separator chains into fat BLAS-3 fronts and
    // NEVER collapses sparse siblings into a dense front (that exploded the fill). Process s high→low.
    for (crd::u32 s = nf - 1; s-- > 0;) // s = nf-2 downto 0
    {
        const crd::u32 par = mf.front_parent[s];
        if (par >= nf)
        {
            continue; // s is a root
        }
        crd::u32 ss = par; // find s's current parent (compress the path through dead/merged fronts)
        while (merged[ss] != empty_id)
        {
            ss = merged[ss];
        }
        const crd::u32 sparent = ss;
        for (ss = par; merged[ss] != empty_id;)
        {
            const crd::u32 snext = merged[ss];
            merged[ss] = sparent;
            ss = snext;
        }
        if (sparent != s + 1)
        {
            continue; // not adjacent ⇒ no merge (keeps relaxed supernodes contiguous column ranges)
        }
        const crd::u32 nscol0 = nscol[s];
        const crd::u32 nscol1 = nscol[s + 1];
        const crd::u32 ns = nscol0 + nscol1;
        crd::u64 totzeros = zeros[s + 1];
        bool merge = false;
        if (ns <= nrelax0)
        {
            merge = true;
        }
        else
        {
            // newzeros = nscol0·(snz[s+1] + nscol0 − snz[s]) ≥ 0 (s's CB ⊆ s+1's front ⇒ snz[s]−nscol0 ≤ snz[s+1]).
            const crd::u64 ext = static_cast<crd::u64>(snz[s + 1]) + nscol0;
            const crd::u64 newzeros = (ext > snz[s]) ? static_cast<crd::u64>(nscol0) * (ext - snz[s]) : 0U;
            if (newzeros == 0)
            {
                merge = true;
            }
            else
            {
                const crd::u64 totz = totzeros + newzeros;
                const crd::u64 totsize = static_cast<crd::u64>(ns) * (ns + 1) / 2 +
                                         static_cast<crd::u64>(ns) * (snz[s + 1] - nscol1);
                const double z = static_cast<double>(totz) / static_cast<double>(totsize);
                merge = (ns <= nrelax1 && z < zrelax0) || (ns <= nrelax2 && z < zrelax1) || (z < zrelax2);
                totzeros = totz;
            }
        }
        if (merge)
        {
            zeros[s] = totzeros;
            merged[s + 1] = s;
            snz[s] = nscol0 + snz[s + 1];
            nscol[s] += nscol[s + 1];
        }
    }
    // Build the relaxed fronts. Each LIVE front (merged[s]==empty) is a representative spanning the contiguous
    // front range [s, e) up to the next live front e; its TOP (parent-most member) is e-1, whose CB is the
    // merged front's CB (etree fill-propagation: every member's external connection also reaches the chain top).
    crd::containers::Array<crd::u32> new_id(alloc);
    new_id.resize(nf);
    crd::u32 newnf = 0;
    for (crd::u32 s = 0; s < nf; ++s)
    {
        new_id[s] = (merged[s] == empty_id) ? newnf++ : empty_id;
    }
    crd::containers::Array<crd::u32> rep(alloc); // rep[f] = live representative of f's group (lowest index)
    rep.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        crd::u32 r = f;
        while (merged[r] != empty_id)
        {
            r = merged[r];
        }
        rep[f] = r;
    }
    out.nfront = newnf;
    out.pivot_first.resize(static_cast<crd::usize>(newnf) + 1);
    out.front_parent.resize(newnf);
    out.row_ptr.resize(static_cast<crd::usize>(newnf) + 1);
    out.col_ptr.resize(static_cast<crd::usize>(newnf) + 1);
    out.row_ptr[0] = 0;
    out.col_ptr[0] = 0;
    crd::u32 g = 0;
    for (crd::u32 s = 0; s < nf; ++s)
    {
        if (merged[s] != empty_id)
        {
            continue; // only live representatives build a new front
        }
        crd::u32 e = s + 1;
        while (e < nf && merged[e] != empty_id)
        {
            ++e;
        }
        const crd::u32 top = e - 1; // parent-most member of the chain group [s, e)
        const crd::u32 first = mf.pivot_first[s];
        const crd::u32 endp = mf.pivot_first[e];
        out.pivot_first[g] = first;
        for (crd::u32 c = first; c < endp; ++c) // merged fully-summed pivots (contiguous, ascending)
        {
            out.row_idx.push_back(c);
            out.col_idx.push_back(c);
        }
        const crd::u32 cb0 = mf.row_ptr[top] + mf.npiv(top); // the top front's CB = the merged front's CB
        for (crd::u32 q = cb0; q < mf.row_ptr[top + 1]; ++q)
        {
            out.row_idx.push_back(mf.row_idx[q]);
            out.col_idx.push_back(mf.row_idx[q]);
        }
        out.row_ptr[g + 1] = static_cast<crd::u32>(out.row_idx.size());
        out.col_ptr[g + 1] = static_cast<crd::u32>(out.col_idx.size());
        const crd::u32 par = mf.front_parent[top];
        out.front_parent[g] = (par >= nf) ? ordering::kNoParent : new_id[rep[par]];
        ++g;
    }
    out.pivot_first[newnf] = n;
    return out;
}
} // namespace

MultifrontalSymbolic build_ldlt_symbolic(const sparse::SparsePattern& a, crd::memory::IAllocator* alloc, crd::u32 relax)
{
    CRD_ASSERT_MSG(a.is_compressed(), "build_ldlt_symbolic requires a compressed pattern");
    CRD_ASSERT_MSG(a.rows == a.cols, "build_ldlt_symbolic requires a square (symmetric) matrix");
    // LDLᵀ fronts = chol(A) supernodes. build_symmetric_multifrontal_symbolic symmetrises A∪Aᵀ
    // internally (build_adjacency); for a symmetric A that is A's own pattern ⇒ the chol(A) supernode
    // tree — the same structure v5a Cholesky / v5b-3 LU use, reused here for the symmetric LDLᵀ fill.
    // Then RELAXED-FRONT AMALGAMATION (relax > 1) merges the tiny fundamental-supernode fronts into fat
    // BLAS-3 fronts — the MUMPS-class lever (v5b LU keeps relax=0 ⇒ unaffected).
    MultifrontalSymbolic mf = build_symmetric_multifrontal_symbolic(a, alloc);
    if (relax > 1)
    {
        return amalgamate_fronts(mf, relax, alloc); // EXPERIMENTAL (default OFF — see m_amalg_relax)
    }
    return mf;
}

template <typename T>
MultifrontalLDLT<T>::MultifrontalLDLT(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_lp(alloc), m_li(alloc), m_lx(alloc), m_dd(alloc), m_doff(alloc), m_block_kinds(alloc),
      m_perm(alloc), m_a_outer(alloc), m_a_inner(alloc), m_a_values(alloc)
{
}

template <typename T>
void MultifrontalLDLT<T>::factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a, crd::u32 num_workers,
                                    bool hermitian)
{
    m_n = static_cast<crd::u32>(a.pattern().cols);
    m_info = 0;
    m_lnz = 0;
    m_nfront = 0;
    m_ndelay = 0;
    m_max_front = 0;
    m_hermitian = dense::is_complex_v<T> ? hermitian : false; // LDLᴴ only meaningful for complex T

    const crd::u32 n = m_n;
    m_dd.resize(n);
    m_doff.resize(n);
    m_block_kinds.resize(n);
    m_perm.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        m_dd[i] = T{0};
        m_doff[i] = T{0};
        m_block_kinds[i] = 0U;
        m_perm[i] = i; // identity; BK swaps permute within each front's pivot range below
    }
    m_lp.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 i = 0; i <= n; ++i)
    {
        m_lp[i] = 0;
    }
    m_li.clear();
    m_lx.clear();

    // Store A's LOWER triangle (row ≥ col) for iterative refinement (the relaxed-threshold safety net). CSC,
    // same column order as given. The symmetric residual SpMV below reads only this (upper is the mirror).
    {
        const sparse::SparsePattern& ap = a.pattern();
        const T* av = a.values().values.data();
        m_a_outer.resize(static_cast<crd::usize>(n) + 1);
        m_a_inner.clear();
        m_a_values.clear();
        crd::u32 cnt = 0;
        for (crd::u32 c = 0; c < n; ++c)
        {
            m_a_outer[c] = cnt;
            for (crd::u32 p = ap.outer_ptr[c]; p < ap.outer_ptr[c + 1]; ++p)
            {
                if (ap.inner_idx[p] >= c)
                {
                    m_a_inner.push_back(ap.inner_idx[p]);
                    m_a_values.push_back(av[p]);
                    ++cnt;
                }
            }
        }
        m_a_outer[n] = cnt;
    }

    if (n == 0)
    {
        return;
    }

    MultifrontalSymbolic mf = build_ldlt_symbolic(a.pattern(), m_alloc, m_amalg_relax);
    m_nfront = mf.nfront;
    const crd::u32 nf = mf.nfront;

    // Child adjacency (CSR) from front_parent. A real parent is front_parent[f] < nf (the kNoParent sentinel
    // is >= nf). Ascending front order is a valid postorder ⇒ children are always lower-indexed than parents.
    crd::containers::Array<crd::u32> chld_ptr(m_alloc);
    chld_ptr.resize(static_cast<crd::usize>(nf) + 1);
    for (crd::u32 i = 0; i <= nf; ++i)
    {
        chld_ptr[i] = 0;
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 par = mf.front_parent[f];
        if (par < nf)
        {
            ++chld_ptr[par + 1];
        }
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        chld_ptr[f + 1] += chld_ptr[f];
    }
    crd::containers::Array<crd::u32> chld_idx(m_alloc);
    chld_idx.resize(chld_ptr[nf]);
    {
        crd::containers::Array<crd::u32> w(m_alloc);
        w.resize(nf);
        for (crd::u32 f = 0; f < nf; ++f)
        {
            w[f] = chld_ptr[f];
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            const crd::u32 par = mf.front_parent[f];
            if (par < nf)
            {
                chld_idx[w[par]++] = f;
            }
        }
    }

    // Worker count first — it selects the front allocator. num_workers <= 1 ⇒ serial (sw = 1, no jobs touched).
    const crd::u32 sw = (num_workers <= 1) ? 1U : crd::jobs::num_workers();
    // The front buffers' allocator. `ts` (ThreadSafeAllocator over m_alloc) guards buffers that MIGRATE across
    // workers in the PARALLEL path; the SERIAL path uses m_alloc DIRECTLY — the mutex lock/unlock on ~3
    // allocations × every front was pure uncontended overhead (measured ~the dominant slice of tiny-front
    // factorizations). `ts` is DECLARED BEFORE `cb` regardless (the allocator-lifetime rule —
    // feedback_container_allocator_must_outlive); for serial, m_alloc [the member] also outlives cb.
    crd::memory::ThreadSafeAllocator ts(m_alloc);
    crd::memory::IAllocator* const front_alloc = (sw <= 1) ? m_alloc : static_cast<crd::memory::IAllocator*>(&ts);
    crd::containers::Array<MfFront<T>> cb(m_alloc); // front f's factored buffer (Schur trailing), kept for its parent
    crd::containers::Array<crd::u32> cb_npiv(m_alloc);
    cb.reserve(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        cb.push_back(MfFront<T>(front_alloc));
    }
    cb_npiv.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        cb_npiv[f] = 0;
    }
    // v5d-h DELAYED PIVOTS (Duff-Reid): per-front records read by the serial postorder position pass.
    // fdelay[f] = global ids this front could NOT eliminate (relayed up to its parent's fully-summed set);
    // fbk[f] = block-kinds (1×1/2×2) of the r_f pivots it DID eliminate. Written single-writer per front BUT
    // by a worker thread in the parallel path (push_back/resize allocate) ⇒ their inner allocator MUST be
    // `front_alloc` (the thread-safe `ts` in parallel; m_alloc serial), like cb/eamap. `ts` (declared above)
    // outlives these (the allocator-lifetime rule — feedback_container_allocator_must_outlive).
    crd::containers::Array<crd::containers::Array<crd::u32>> fdelay(m_alloc);
    crd::containers::Array<crd::containers::Array<crd::u8>> fbk(m_alloc);
    fdelay.reserve(nf);
    fbk.reserve(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        fdelay.push_back(crd::containers::Array<crd::u32>(front_alloc));
        fbk.push_back(crd::containers::Array<crd::u8>(front_alloc));
    }

    constexpr crd::u32 no_loc = 0xFFFFFFFFU;
    crd::containers::Array<crd::u32> loc(m_alloc); // per-worker: global id -> front-local index (symbolic order)
    loc.resize(static_cast<crd::usize>(sw) * n);
    for (crd::usize i = 0; i < loc.size(); ++i)
    {
        loc[i] = no_loc;
    }
    crd::containers::Array<crd::u8> bk(m_alloc); // per-worker block kinds
    crd::containers::Array<crd::u32> piv(m_alloc); // per-worker BK swap targets
    bk.resize(static_cast<crd::usize>(sw) * n);
    piv.resize(static_cast<crd::usize>(sw) * n);
    crd::containers::Array<crd::containers::Array<crd::u32>> eamap(m_alloc); // per-worker extend-add map (scratch)
    crd::containers::Array<crd::containers::Array<crd::u32>> delayin(m_alloc); // per-worker delayed-in gather (scratch)
    for (crd::u32 w = 0; w < sw; ++w)
    {
        // front_alloc (ts in parallel) so a worker's scratch grow is thread-safe; scratch ⇒ no output change.
        eamap.push_back(crd::containers::Array<crd::u32>(front_alloc));
        delayin.push_back(crd::containers::Array<crd::u32>(front_alloc));
    }

    const sparse::SparsePattern& ap = a.pattern();
    const crd::u32* aouter = ap.outer_ptr.data();
    const crd::u32* ainner = ap.inner_idx.data();
    const T* aval = a.values().values.data();

    // Factor one front (v5d-h DELAYED PIVOTS). The fully-summed block = the columns DELAYED up from children
    // (descendant pivots they could not stably eliminate) PREPENDED to this front's own symbolic pivots; the
    // Bunch-Kaufman kernel eliminates as many as it stably can (r of them), and any it cannot are recorded in
    // fdelay[f] to be relayed to THIS front's parent (MA57/Duff-Reid). No D/perm is stored here — the factor
    // POSITION of an eliminated pivot is dynamic (a delayed pivot lands at an ancestor, not its symbolic
    // front), so all of perm/D/block_kinds/L are assigned by the serial postorder pass below. cb[f] (the full
    // factored front, row_index in POST-SWAP global-id order) + cb_npiv[f]=r + fbk[f] + fdelay[f] are the only
    // outputs; each front writes its OWN slots (single-writer ⇒ race-free). `wk` selects per-worker scratch.
    auto factor_one_front = [&](crd::u32 f, crd::u32 wk)
    {
        const crd::u32 c0 = mf.pivot_first[f];
        const crd::u32 npiv = mf.npiv(f);
        const crd::u32 rb = mf.row_ptr[f];
        const crd::u32 nrsym = mf.row_ptr[f + 1] - rb; // symbolic front extent (== col extent, symmetric)
        const crd::u32* rows = mf.row_idx.data() + rb;
        crd::u32* loc_w = loc.data() + static_cast<crd::usize>(wk) * n;
        crd::u8* bk_w = bk.data() + static_cast<crd::usize>(wk) * n;
        crd::u32* piv_w = piv.data() + static_cast<crd::usize>(wk) * n;

        // Gather DELAYED-IN columns from children (fixed chld_idx order ⇒ moat), then sort ascending. Every
        // delayed id is a descendant pivot with global id < c0 (postorder), and < every symbolic row id of
        // this front, so the sorted delayed block leads an ASCENDING front row_index (the extend-add invariant
        // is preserved) and forms the leading part of the fully-summed block.
        crd::containers::Array<crd::u32>& din = delayin[wk];
        din.clear();
        for (crd::u32 cc = chld_ptr[f]; cc < chld_ptr[f + 1]; ++cc)
        {
            const crd::u32 g = chld_idx[cc];
            for (crd::u32 t = 0; t < static_cast<crd::u32>(fdelay[g].size()); ++t)
            {
                din.push_back(fdelay[g][t]);
            }
        }
        for (crd::u32 i = 1; i < static_cast<crd::u32>(din.size()); ++i) // insertion sort (din is small)
        {
            const crd::u32 v = din[i];
            crd::u32 j = i;
            while (j > 0 && din[j - 1] > v)
            {
                din[j] = din[j - 1];
                --j;
            }
            din[j] = v;
        }
        const crd::u32 ndel = static_cast<crd::u32>(din.size());
        const crd::u32 fnr = ndel + nrsym;    // DYNAMIC front extent (delayed columns add rows/cols)
        const crd::u32 attempt = ndel + npiv; // fully-summed columns to attempt at this front

        MfFront<T> front(front_alloc);
        front.resize(fnr, fnr);
        front.zero_fill();
        for (crd::u32 t = 0; t < ndel; ++t) // [0, ndel): the delayed columns (global id < c0)
        {
            front.row_index[t] = din[t];
            front.col_index[t] = din[t];
            loc_w[din[t]] = t;
        }
        for (crd::u32 t = 0; t < nrsym; ++t) // [ndel, fnr): symbolic pivots then CB rows
        {
            front.row_index[ndel + t] = rows[t];
            front.col_index[ndel + t] = rows[t];
            loc_w[rows[t]] = ndel + t;
        }

        // Assemble A's LOWER triangle for the symbolic pivot columns (global c = c0 + k → front-local
        // loc_w[c] == ndel + k). The DELAYED columns receive their data purely from children's extend-add
        // below (their A entries were assembled at the descendant front where they were first a pivot).
        for (crd::u32 k = 0; k < npiv; ++k)
        {
            const crd::u32 c = c0 + k;
            const crd::u32 cl = loc_w[c];
            for (crd::u32 p = aouter[c]; p < aouter[c + 1]; ++p)
            {
                const crd::u32 i = ainner[p];
                if (i >= c)
                {
                    front.at(loc_w[i], cl) += aval[p];
                }
            }
        }

        // Symmetric extend-add of each child's Schur (trailing from cb_npiv[g] = the child's ELIMINATED count,
        // so the trailing carries the child's own delayed columns + its CB), in FIXED chld_idx order — THE
        // determinism-moat invariant (assembly is by pattern order, not completion order).
        for (crd::u32 cc = chld_ptr[f]; cc < chld_ptr[f + 1]; ++cc)
        {
            const crd::u32 g = chld_idx[cc];
            mf_extend_add_trailing_sym<T>(front, cb[g], cb_npiv[g], eamap[wk]);
        }

        // Factor the fully-summed block [0, attempt). r = number stably eliminated; [r, attempt) DELAY upward.
        // Real T instantiates ONLY <T,false> (the moat-pinned path); complex picks LDLᴴ/LDLᵀ at runtime.
        crd::u32 r = 0;
        const double pa = m_pivot_alpha; // BK threshold (lower ⇒ fewer Duff-Reid delays — v5d-h perf lever)
        if constexpr (dense::is_complex_v<T>)
        {
            r = m_hermitian ? factor_front_ldlt<T, true>(front.data.data(), fnr, fnr, attempt, bk_w, piv_w, pa)
                            : factor_front_ldlt<T, false>(front.data.data(), fnr, fnr, attempt, bk_w, piv_w, pa);
        }
        else
        {
            // BLOCKED-BLAS-3 BK fast path for BIG fronts (v5d-h/perf): full Bunch-Kaufman (1×1/2×2) blocked
            // kernel — handles indefinite + delays directly (returns r ≤ attempt), so no copy/bail-restore.
            // Small fronts use the unblocked kernel (the gemm overhead is not worth amortizing there).
            constexpr crd::u32 block_min = 128;
            constexpr crd::u32 block_nb = 128;
            if (attempt >= block_min)
            {
                r = factor_front_ldlt_blocked<T>(front.data.data(), fnr, fnr, attempt, bk_w, piv_w, block_nb,
                                                 front_alloc, pa);
            }
            else
            {
                r = factor_front_ldlt<T, false>(front.data.data(), fnr, fnr, attempt, bk_w, piv_w, pa);
            }
        }

        // Replay the BK swaps piv_w[0..r) on row_index/col_index (the kernel swapped the DATA within [0,attempt)
        // but not the id arrays). After this, front-local position p carries its true global id — needed for
        // the parent's extend-add of the delayed columns [r, attempt) and for the postorder id→position map.
        {
            crd::u32 kk = 0;
            while (kk < r)
            {
                const crd::u32 blocksz = (bk_w[kk] == 1U) ? 1U : 2U;
                const crd::u32 sp = kk + blocksz - 1U; // swap position: 1×1 swaps (kk,kp); 2×2 swaps (kk+1,kp)
                const crd::u32 kp = piv_w[kk];
                if (kp != sp)
                {
                    const crd::u32 tmp = front.row_index[sp];
                    front.row_index[sp] = front.row_index[kp];
                    front.row_index[kp] = tmp;
                    front.col_index[sp] = front.row_index[sp];
                    front.col_index[kp] = front.row_index[kp];
                }
                kk += blocksz;
            }
        }

        // Sort the DELAYED columns [r, attempt) ascending by global id (symmetric data swap via ldlt_swap_sym +
        // row_index), so the front's TRAILING [r, fnr) is ascending — the precondition the parent's
        // mf_extend_add_trailing_sym needs (monotone child→parent map + lower-triangle preservation). Every
        // delayed id < c0 ≤ every CB id [attempt, fnr), so sorting just [r, attempt) sorts the whole trailing.
        // ldlt_swap_sym permutes the eliminated L21 columns' rows too, consistently with row_index ⇒ L stays
        // correct. For a no-delay front (r == attempt) this loop is empty ⇒ byte-identical to the static path.
        for (crd::u32 i = r; i + 1 < attempt; ++i)
        {
            crd::u32 mn = i;
            for (crd::u32 j = i + 1; j < attempt; ++j)
            {
                if (front.row_index[j] < front.row_index[mn])
                {
                    mn = j;
                }
            }
            if (mn != i)
            {
                ldlt_swap_sym<T>(front.data.data(), fnr, fnr, i, mn);
                const crd::u32 tmp = front.row_index[i];
                front.row_index[i] = front.row_index[mn];
                front.row_index[mn] = tmp;
                front.col_index[i] = front.row_index[i];
                front.col_index[mn] = front.row_index[mn];
            }
        }

        // Record: eliminated block-kinds [0, r) + delayed-out global ids [r, attempt) (post-swap). cb[f] keeps
        // the full factored front (its trailing [r, fnr) — delayed columns + CB — is read by the parent).
        fbk[f].resize(r);
        for (crd::u32 t = 0; t < r; ++t)
        {
            fbk[f][t] = bk_w[t];
        }
        fdelay[f].clear();
        for (crd::u32 t = r; t < attempt; ++t)
        {
            fdelay[f].push_back(front.row_index[t]);
        }
        cb_npiv[f] = r;
        for (crd::u32 t = 0; t < ndel; ++t)
        {
            loc_w[din[t]] = no_loc;
        }
        for (crd::u32 t = 0; t < nrsym; ++t)
        {
            loc_w[rows[t]] = no_loc;
        }
        cb[f] = std::move(front); // single-writer; the L21 columns + trailing are read by the post-pass/parent
    };

    if (sw <= 1)
    {
        for (crd::u32 f = 0; f < nf; ++f)
        {
            factor_one_front(f, 0);
        }
    }
    else
    {
        // Assembly-tree levels: level[f] = 1 + max(level[child]); children g < f (ascending = postorder) ⇒ a
        // single ascending pass. Same-level fronts are mutually independent (a front's cb[f] is read ONLY by
        // its direct parent, at a higher level, after that level's wait barrier). Front-parallel only.
        crd::containers::Array<crd::u32> level(m_alloc);
        level.resize(nf);
        crd::u32 nlevels = 0;
        for (crd::u32 f = 0; f < nf; ++f)
        {
            crd::u32 lev = 0;
            for (crd::u32 cc = chld_ptr[f]; cc < chld_ptr[f + 1]; ++cc)
            {
                const crd::u32 lk = level[chld_idx[cc]] + 1;
                lev = lk > lev ? lk : lev;
            }
            level[f] = lev;
            nlevels = (lev + 1 > nlevels) ? lev + 1 : nlevels;
        }
        crd::containers::Array<crd::u32> lvl_ptr(m_alloc);
        lvl_ptr.resize(static_cast<crd::usize>(nlevels) + 1);
        for (crd::u32 i = 0; i <= nlevels; ++i)
        {
            lvl_ptr[i] = 0;
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            ++lvl_ptr[level[f] + 1];
        }
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            lvl_ptr[l + 1] += lvl_ptr[l];
        }
        crd::containers::Array<crd::u32> lvl_list(m_alloc);
        lvl_list.resize(nf);
        crd::containers::Array<crd::u32> wp(m_alloc);
        wp.resize(nlevels);
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            wp[l] = lvl_ptr[l];
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            lvl_list[wp[level[f]]++] = f; // ascending f within a level ⇒ worker-order-independent (the moat)
        }
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            const crd::u32 lo = lvl_ptr[l];
            const crd::u32 cnt = lvl_ptr[l + 1] - lo;
            crd::jobs::Counter* counter = crd::jobs::parallel_for(
                cnt, num_workers,
                [&](crd::u32 b, crd::u32 e)
                {
                    const crd::u32 wk = crd::jobs::worker_index();
                    for (crd::u32 t = b; t < e; ++t)
                    {
                        factor_one_front(lvl_list[lo + t], wk);
                    }
                });
            crd::jobs::wait(counter);
            crd::jobs::frame_reset();
        }
    }

    // ---- v5d-h: SERIAL postorder factor-POSITION assignment (deterministic ⇒ moat-safe). Factor positions
    // are DYNAMIC because a delayed pivot is eliminated at an ancestor, not at its symbolic front: walk
    // f = 0..nf-1 (ascending = postorder) with a running counter gp, assigning each front's r_f eliminated
    // pivots the consecutive positions [fgp[f], fgp[f]+r_f) and writing perm/D/block_kinds there. For the
    // NO-DELAY case gp == pivot_first[f] always ⇒ this is byte-identical to the former static scheme. If
    // gp != n at the end, some variable was never eliminated (a root-level delay / a singular matrix) ⇒
    // the factor is INVALID (info != 0). ----
    crd::containers::Array<crd::u32> fgp(m_alloc); // factor-position start of each front's eliminated pivots
    fgp.resize(nf);
    crd::u32 gp = 0;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        fgp[f] = gp;
        const MfFront<T>& front = cb[f];
        if (front.nrows > m_max_front)
        {
            m_max_front = front.nrows; // delay-blowup diagnostic
        }
        const crd::u32 rf = cb_npiv[f];
        const crd::u8* fk = fbk[f].data();
        const crd::u32 fc0 = mf.pivot_first[f]; // a pivot here with global id < fc0 was DELAYED in from a descendant
        crd::u32 kk = 0;
        while (kk < rf)
        {
            if (front.row_index[kk] < fc0)
            {
                ++m_ndelay;
            }
            if (fk[kk] == 1U)
            {
                m_perm[gp] = front.row_index[kk];
                m_dd[gp] = front.at(kk, kk);
                m_block_kinds[gp] = 1U;
                gp += 1;
                kk += 1;
            }
            else
            {
                m_perm[gp] = front.row_index[kk];
                m_perm[gp + 1] = front.row_index[kk + 1];
                m_dd[gp] = front.at(kk, kk);
                m_doff[gp] = front.at(kk + 1, kk);
                m_dd[gp + 1] = front.at(kk + 1, kk + 1);
                m_block_kinds[gp] = 2U;
                m_block_kinds[gp + 1] = 0U;
                gp += 2;
                kk += 2;
            }
        }
    }
    if (gp != n)
    {
        m_info = static_cast<crd::usize>(gp) + 1; // a variable was never eliminated ⇒ singular / unresolved delay
        return;
    }

    // ipos[global id] = factor position (inverse of m_perm). `has_swap` = perm is not the identity (a BK swap
    // OR a delayed pivot reordered the factor sequence) ⇒ a column's L21 tail may be out of factor order.
    crd::containers::Array<crd::u32> ipos(m_alloc);
    ipos.resize(n);
    bool has_swap = false;
    for (crd::u32 fp = 0; fp < n; ++fp)
    {
        ipos[m_perm[fp]] = fp;
        has_swap = has_swap || (m_perm[fp] != fp);
    }

    // Build L's CSC DIRECTLY. Pass A: per-column nnz — each eliminated pivot column (front-local k, factor
    // position fgp[f]+k) has fnr−(k+blocksz) sub-diagonal entries (the 2×2 partner row is D, not L).
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 rf = cb_npiv[f];
        const crd::u32 fnr = cb[f].nrows;
        const crd::u32 base = fgp[f];
        crd::u32 kk = 0;
        while (kk < rf)
        {
            const crd::u32 blocksz = (m_block_kinds[base + kk] == 1U) ? 1U : 2U;
            const crd::u32 cnt = fnr - (kk + blocksz);
            for (crd::u32 cc2 = 0; cc2 < blocksz; ++cc2)
            {
                m_lp[base + kk + cc2 + 1] = cnt;
            }
            kk += blocksz;
        }
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        m_lp[c + 1] += m_lp[c];
    }
    const crd::usize nnz = m_lp[n];
    m_li.resize(nnz);
    m_lx.resize(nnz);

    // Pass B: scatter each front's L21 at its columns' offsets. A sub-diagonal row t (front-local) is either a
    // later eliminated pivot of THIS front (t < r_f ⇒ factor position fgp[f]+t) or a delayed/CB row eliminated
    // at an ancestor (⇒ ipos[global id]).
    {
        crd::containers::Array<crd::u32> wpos(m_alloc);
        wpos.resize(n);
        for (crd::u32 c = 0; c < n; ++c)
        {
            wpos[c] = m_lp[c];
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            const MfFront<T>& front = cb[f];
            const crd::u32 rf = cb_npiv[f];
            const crd::u32 fnr = front.nrows;
            const crd::u32 base = fgp[f];
            crd::u32 kk = 0;
            while (kk < rf)
            {
                const crd::u32 blocksz = (m_block_kinds[base + kk] == 1U) ? 1U : 2U;
                for (crd::u32 cc2 = 0; cc2 < blocksz; ++cc2)
                {
                    const crd::u32 lk = kk + cc2;
                    crd::u32 w = wpos[base + lk];
                    for (crd::u32 t = kk + blocksz; t < fnr; ++t)
                    {
                        m_li[w] = (t < rf) ? (base + t) : ipos[front.row_index[t]];
                        m_lx[w] = front.at(t, lk);
                        ++w;
                    }
                    wpos[base + lk] = w;
                }
                kk += blocksz;
            }
        }
    }

    // Pass C: only when BK swapped, the CB-row tails can be out of order ⇒ sort each column ascending
    // (insertion sort — single-front columns, short ranges). Result is canonical either way (the moat needs
    // the stored L bit-identical, which a deterministic sort preserves).
    if (has_swap)
    {
        for (crd::u32 c = 0; c < n; ++c)
        {
            for (crd::u32 q = m_lp[c] + 1; q < m_lp[c + 1]; ++q)
            {
                const crd::u32 rr = m_li[q];
                const T vv = m_lx[q];
                crd::u32 j = q;
                while (j > m_lp[c] && m_li[j - 1] > rr)
                {
                    m_li[j] = m_li[j - 1];
                    m_lx[j] = m_lx[j - 1];
                    --j;
                }
                m_li[j] = rr;
                m_lx[j] = vv;
            }
        }
    }
    m_lnz = nnz;
}

// v5f: ONE P·L·D·Lᵀ·Pᵀ triangular solve (A·out = bin, ORIGINAL order; tmp = factor-order scratch). The raw,
// un-refined building block — solve()'s x0 + each IR-correction step forward to it, and apply_inverse drives
// it directly (NO IR). `bin` may alias `out` (bin is fully gathered into tmp before out is written).
// Conjugate-on-backward for Hermitian LDLᴴ (identity for real / complex-symmetric LDLᵀ).
template <typename T> void MultifrontalLDLT<T>::ldlt_apply_once(const T* bin, T* out, T* tmp) const
{
    const crd::u32 n = m_n;
    const crd::u32* lp = m_lp.data();
    const crd::u32* li = m_li.data();
    const T* lx = m_lx.data();
    const T* dd = m_dd.data();
    const T* doff = m_doff.data();
    const crd::u8* bk = m_block_kinds.data();
    const crd::u32* perm = m_perm.data();
    const bool herm = m_hermitian;
    auto cj = [herm](const T& x) -> T
    {
        if constexpr (dense::is_complex_v<T>)
        {
            return herm ? crd::hesap::conj(x) : x;
        }
        else
        {
            (void)herm;
            return x;
        }
    };
    for (crd::u32 fp = 0; fp < n; ++fp)
    {
        tmp[fp] = bin[perm[fp]]; // Pᵀ
    }
    for (crd::u32 j = 0; j < n; ++j) // forward unit-lower L
    {
        const T zj = tmp[j];
        for (crd::u32 p = lp[j]; p < lp[j + 1]; ++p)
        {
            tmp[li[p]] -= lx[p] * zj;
        }
    }
    crd::u32 k = 0; // block-diagonal D
    while (k < n)
    {
        if (bk[k] == 1U)
        {
            tmp[k] = tmp[k] / dd[k];
            k += 1;
        }
        else
        {
            const T d11 = dd[k];
            const T d21 = doff[k];
            const T d22 = dd[k + 1];
            const T cd21 = cj(d21);
            const T det = d11 * d22 - cd21 * d21;
            const T z0 = tmp[k];
            const T z1 = tmp[k + 1];
            tmp[k] = (d22 * z0 - cd21 * z1) / det;
            tmp[k + 1] = (d11 * z1 - d21 * z0) / det;
            k += 2;
        }
    }
    for (crd::u32 jj = n; jj > 0; --jj) // backward Lᵀ (Lᴴ for Hermitian)
    {
        const crd::u32 j = jj - 1;
        T s = tmp[j];
        for (crd::u32 p = lp[j]; p < lp[j + 1]; ++p)
        {
            s -= cj(lx[p]) * tmp[li[p]];
        }
        tmp[j] = s;
    }
    for (crd::u32 fp = 0; fp < n; ++fp)
    {
        out[perm[fp]] = tmp[fp]; // P
    }
}

// v5f: RAW factor apply (no internal IR) — the mixed-precision driver's building block. One ldlt_apply_once
// per column, in place; the driver owns all refinement at the working precision.
template <typename T> void MultifrontalLDLT<T>::apply_inverse(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return; // invalid factor; the driver checks info() before iterating, so rhs is left untouched
    }
    const crd::u32 n = m_n;
    if (n == 0)
    {
        return;
    }
    crd::containers::Array<T> tmp(m_alloc);
    tmp.resize(n);
    for (crd::usize col = 0; col < nrhs; ++col)
    {
        T* b = rhs.data() + col * n;
        ldlt_apply_once(b, b, tmp.data());
    }
}

template <typename T> bool MultifrontalLDLT<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    // A = P·L·D·Lᵀ·Pᵀ ⇒ A·x = b solved by: r = Pᵀ·b (gather to factor order) → forward unit-lower L·z = r
    // → block-aware D·w = z (1×1 divide / 2×2 inverse) → backward Lᵀ·y = w → x = P·y (scatter to original
    // order). L is CSC factor-position, strictly-lower (unit diagonal implicit; the 2×2 coupling lives in D,
    // so the L solves are plain unit-triangular). Multi-RHS: rhs is a column-major n × nrhs block, solved
    // column by column. Returns false if the factor is invalid (info != 0, e.g. a delayed pivot).
    if (m_info != 0)
    {
        return false;
    }
    const crd::u32 n = m_n;
    if (n == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(rhs.size() == static_cast<crd::usize>(n) * nrhs, "MultifrontalLDLT::solve: rhs size != n*nrhs");

    // Conjugate iff Hermitian (LDLᴴ): the backward solve is Lᴴ (not Lᵀ) and the 2×2 D-inverse uses conj(d21).
    // For real T (and complex LDLᵀ) this is the identity ⇒ the v5d-d real/symmetric solve is byte-identical.
    const bool herm = m_hermitian;
    auto cj = [herm](const T& x) -> T
    {
        if constexpr (dense::is_complex_v<T>)
        {
            return herm ? crd::hesap::conj(x) : x;
        }
        else
        {
            (void)herm;
            return x;
        }
    };

    using R = dense::RealType<T>;
    auto mag = [](const T& x) -> R
    {
        if constexpr (dense::is_complex_v<T>)
        {
            return crd::hesap::abs(x);
        }
        else
        {
            return x < R{0} ? -x : x;
        }
    };
    const crd::u32* aouter = m_a_outer.data();
    const crd::u32* ainner = m_a_inner.data();
    const T* avals = m_a_values.data();

    crd::containers::Array<T> tmp(m_alloc);   // tri_solve factor-order scratch
    crd::containers::Array<T> xcol(m_alloc);  // current solution (original order)
    crd::containers::Array<T> bcopy(m_alloc); // saved RHS (original order)
    crd::containers::Array<T> resid(m_alloc); // residual b − A·x (original order)
    crd::containers::Array<T> dx(m_alloc);    // IR correction (original order)
    tmp.resize(n);
    xcol.resize(n);
    bcopy.resize(n);
    resid.resize(n);
    dx.resize(n);

    // One triangular solve A·out = bin (P·L·D·Lᵀ·Pᵀ), bin/out in ORIGINAL order. Body extracted to
    // ldlt_apply_once (v5f, byte-identical) so the mixed-precision apply_inverse drives the same kernel.
    auto tri_solve = [&](const T* bin, T* out) { ldlt_apply_once(bin, out, tmp.data()); };

    // Symmetric SpMV y = A·x from the stored LOWER triangle (upper is the mirror; off-diagonals hit both rows).
    auto symv = [&](const T* x, T* y)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            y[i] = T{};
        }
        for (crd::u32 c = 0; c < n; ++c)
        {
            const T xc = x[c];
            for (crd::u32 p = aouter[c]; p < aouter[c + 1]; ++p)
            {
                const crd::u32 i = ainner[p];
                const T v = avals[p];
                y[i] += v * xc;
                if (i > c)
                {
                    y[c] += cj(v) * x[i]; // mirror: conj for Hermitian (LDLᴴ); plain for real / complex-symmetric
                }
            }
        }
    };

    // Iterative refinement (the relaxed-threshold safety net): refine x ← x + A⁻¹(b − A·x) until the backward
    // error stalls or hits ~machine precision, then a final guard ACCEPTS only if ‖b−A·x‖ ≤ accept_tol·‖b‖
    // (else return false — accurate-or-flagged, never silent garbage). Deterministic ⇒ the moat holds.
    const R eps = std::numeric_limits<R>::epsilon();
    const R refine_tol = static_cast<R>(8) * eps;
    const R accept_tol = static_cast<R>(1e-6); // loose: catches genuine breakdown, not honest round-off
    constexpr crd::u32 max_ir = 8;
    bool all_ok = true;
    for (crd::usize q = 0; q < nrhs; ++q)
    {
        T* col = rhs.data() + q * static_cast<crd::usize>(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            bcopy[i] = col[i];
        }
        R bnorm = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            bnorm = std::max(bnorm, mag(bcopy[i]));
        }
        if (bnorm == R{0})
        {
            bnorm = R{1};
        }
        tri_solve(bcopy.data(), xcol.data()); // x0
        R rnorm = bnorm;
        R prev = std::numeric_limits<R>::max();
        for (crd::u32 iter = 0; iter < max_ir; ++iter)
        {
            symv(xcol.data(), resid.data());
            for (crd::u32 i = 0; i < n; ++i)
            {
                resid[i] = bcopy[i] - resid[i]; // r = b − A·x
            }
            rnorm = R{0};
            for (crd::u32 i = 0; i < n; ++i)
            {
                rnorm = std::max(rnorm, mag(resid[i]));
            }
            if (rnorm <= refine_tol * bnorm || rnorm >= prev) // converged OR stalled (no improvement)
            {
                break;
            }
            prev = rnorm;
            tri_solve(resid.data(), dx.data());
            for (crd::u32 i = 0; i < n; ++i)
            {
                xcol[i] += dx[i];
            }
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            col[i] = xcol[i];
        }
        if (rnorm > accept_tol * bnorm)
        {
            all_ok = false; // backward-error guard: the relaxed factor failed to deliver — flag, don't lie
        }
    }
    return all_ok;
}

template <typename T>
MultifrontalLDLT<T> factor_multifrontal_ldlt(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a,
                                             crd::memory::IAllocator* alloc, crd::u32 num_workers, bool hermitian)
{
    MultifrontalLDLT<T> ldlt(alloc);
    ldlt.factorize(a, num_workers, hermitian);
    return ldlt;
}

// Explicit instantiations: f32 / f64 (real) + Complex32 / Complex64 (v5d-f — LDLᵀ AND LDLᴴ, runtime-selected).
template class MultifrontalLDLT<crd::f32>;
template class MultifrontalLDLT<crd::f64>;
template class MultifrontalLDLT<crd::hesap::Complex32>;
template class MultifrontalLDLT<crd::hesap::Complex64>;
template MultifrontalLDLT<crd::f32>
factor_multifrontal_ldlt<crd::f32>(const sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csc>&,
                                   crd::memory::IAllocator*, crd::u32, bool);
template MultifrontalLDLT<crd::f64>
factor_multifrontal_ldlt<crd::f64>(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csc>&,
                                   crd::memory::IAllocator*, crd::u32, bool);
template MultifrontalLDLT<crd::hesap::Complex32> factor_multifrontal_ldlt<crd::hesap::Complex32>(
    const sparse::SparseMatrix<crd::hesap::Complex32, sparse::SparseFormat::Csc>&, crd::memory::IAllocator*, crd::u32,
    bool);
template MultifrontalLDLT<crd::hesap::Complex64> factor_multifrontal_ldlt<crd::hesap::Complex64>(
    const sparse::SparseMatrix<crd::hesap::Complex64, sparse::SparseFormat::Csc>&, crd::memory::IAllocator*, crd::u32,
    bool);

} // namespace crd::hesap::direct
