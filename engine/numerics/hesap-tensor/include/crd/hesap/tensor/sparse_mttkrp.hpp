#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-i: MTTKRP (matricized tensor times Khatri-Rao
// product), the CP-decomposition workhorse:
//   M(i, r) = sum over all nonzeros x(i, j, k, ...) of
//             val * F1(j, r) * F2(k, r) * ...   (target mode's factor excluded)
//
// CSF path (the SPLATT algorithm): the factored tree walk — each fiber's
// leaf sum is accumulated ONCE (axpy chains over r), then multiplied by the
// parent factor row on the way up, saving the redundant inner products of
// the naive per-nonzero form. Root = the target mode ⇒ every root fiber owns
// a DISJOINT output row ⇒ the deterministic crd-jobs partition over root
// fibers (grain = f(shape/nnz) ONLY) is BIT-IDENTICAL at any worker count
// (the {1..16} moat, gated).
//
// COO path (mttkrp_coo): the reference walk over the canonical sorted COO
// with fiber-boundary detection — the EXACT op sequence of the CSF walk by
// construction ⇒ CSF == COO bit-identity (gated). Target = mode 0 of the
// COO; use coo_reorder for other targets.
// ---------------------------------------------------------------------------
#include "sparse.hpp"

namespace crd::hesap::tensor
{

namespace sparsedetail
{

template <typename T> struct MttkrpCtx
{
    const crd::u32* fids[kMaxRank];
    const crd::u32* fptr[kMaxRank]; // levels 0..rank-2
    const T* fac[kMaxRank];         // per LEVEL: the level's mode's factor matrix
    const T* val;
    T* tmp[kMaxRank]; // task-local accumulators, levels 1..rank-2
    crd::u64 r;
    crd::u32 rank;
};

// The hot path: a fiber whose children are LEAVES. Register-chunked
// accumulation over the fiber's leaves (the accumulator never round-trips
// through memory) + the parent factor multiply FUSED into the store:
//   dst[q] += (sum_t val[t] * leaf_row_t[q]) * prow[q]
// Every element's chain is the exact op sequence of axpy_row-per-leaf +
// hadamard_acc (fma-for-fma) ⇒ bit-identical to the COO reference walk.
template <typename T>
inline void leaf_fiber_fused(const MttkrpCtx<T>& c, crd::u32 cb, crd::u32 ce, const T* prow, T* dst) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const crd::u32* lf = c.fids[c.rank - 1U];
    const T* fac = c.fac[c.rank - 1U];
    const T* val = c.val;
    const crd::u64 r = c.r;
    crd::u64 q0 = 0;
    while (q0 < r)
    {
        const crd::u64 rem = r - q0;
        if (rem >= 4U * W)
        {
            V a0(T(0));
            V a1(T(0));
            V a2(T(0));
            V a3(T(0));
            for (crd::u32 t = cb; t < ce; ++t)
            {
                const T* row = fac + static_cast<crd::u64>(lf[t]) * r + q0;
                const V vv(val[t]);
                a0 = simd::fma(vv, V::load(row), a0);
                a1 = simd::fma(vv, V::load(row + W), a1);
                a2 = simd::fma(vv, V::load(row + 2U * W), a2);
                a3 = simd::fma(vv, V::load(row + 3U * W), a3);
            }
            simd::fma(a0, V::load(prow + q0), V::load(dst + q0)).store(dst + q0);
            simd::fma(a1, V::load(prow + q0 + W), V::load(dst + q0 + W)).store(dst + q0 + W);
            simd::fma(a2, V::load(prow + q0 + 2U * W), V::load(dst + q0 + 2U * W)).store(dst + q0 + 2U * W);
            simd::fma(a3, V::load(prow + q0 + 3U * W), V::load(dst + q0 + 3U * W)).store(dst + q0 + 3U * W);
            q0 += 4U * W;
        }
        else if (rem >= W)
        {
            V a0(T(0));
            for (crd::u32 t = cb; t < ce; ++t)
            {
                a0 = simd::fma(V(val[t]), V::load(fac + static_cast<crd::u64>(lf[t]) * r + q0), a0);
            }
            simd::fma(a0, V::load(prow + q0), V::load(dst + q0)).store(dst + q0);
            q0 += W;
        }
        else
        {
            const crd::usize cw = static_cast<crd::usize>(rem);
            V a0(T(0));
            for (crd::u32 t = cb; t < ce; ++t)
            {
                a0 = simd::fma(V(val[t]),
                               V::load_partial(fac + static_cast<crd::u64>(lf[t]) * r + q0, cw), a0);
            }
            simd::fma(a0, V::load_partial(prow + q0, cw), V::load_partial(dst + q0, cw))
                .store_partial(dst + q0, cw);
            q0 = r;
        }
    }
}

// adds the subtree contribution of node z at level l (1 <= l <= rank-1)
// into dst[r]: leaf = val * leaf-factor row; inner = (sum of children) *
// this level's factor row. Bounded recursion (depth < kMaxRank).
template <typename T> inline void mttkrp_node(const MttkrpCtx<T>& c, crd::u32 l, crd::u64 z, T* dst) noexcept
{
    const T* row = c.fac[l] + static_cast<crd::u64>(c.fids[l][z]) * c.r;
    if (l == c.rank - 1U)
    {
        axpy_row(dst, row, c.val[z], c.r);
        return;
    }
    const crd::u32 cb = c.fptr[l][z];
    const crd::u32 ce = c.fptr[l][z + 1U];
    if (l + 1U == c.rank - 1U)
    {
        leaf_fiber_fused(c, cb, ce, row, dst); // the hot path — no staging buffer
        return;
    }
    T* acc = c.tmp[l];
    std::memset(acc, 0, static_cast<crd::usize>(c.r) * sizeof(T));
    for (crd::u32 t = cb; t < ce; ++t)
    {
        mttkrp_node(c, l + 1U, t, acc);
    }
    hadamard_acc(dst, acc, row, c.r);
}

} // namespace sparsedetail

// CSF MTTKRP. `factors` holds one [shape(m), R] tight row-major matrix per
// MODE (factors[x.order(0)] — the target mode — is ignored and may be a
// default view). out = [shape(order(0)), R] tight; fully overwritten (rows
// of empty fibers = 0). scratch: per-task accumulator rows.
template <typename T>
[[nodiscard]] TensorStatus mttkrp(const SparseCsf<T>& x, crd::containers::ConstSpan<TensorView<const T>> factors,
                                  TensorView<T> out, crd::memory::IAllocator* scratch, crd::u32 num_workers = 0)
{
    using namespace sparsedetail;
    const crd::u32 rank = x.rank();
    if (rank < 2U || factors.size() != rank || !tight2(out) || scratch == nullptr)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 r = out.shape(1);
    if (out.shape(0) != x.shape(x.order(0)))
    {
        return TensorStatus::ShapeMismatch;
    }
    for (crd::u32 l = 1; l < rank; ++l)
    {
        const crd::u32 m = x.order(l);
        const TensorView<const T>& fm = factors[m];
        if (!tight2(fm) || fm.shape(0) != x.shape(m) || fm.shape(1) != r)
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    T* pout = out.data();
    std::memset(pout, 0, static_cast<crd::usize>(out.shape(0) * r) * sizeof(T));
    const crd::u64 n0 = x.nodes(0);
    if (x.nnz() == 0U || n0 == 0U || r == 0U)
    {
        return TensorStatus::Ok;
    }
    MttkrpCtx<T> base{};
    for (crd::u32 l = 0; l < rank; ++l)
    {
        base.fids[l] = x.fids(l);
        if (l + 1U < rank)
        {
            base.fptr[l] = x.fptr(l);
        }
        base.fac[l] = l == 0U ? nullptr : factors[x.order(l)].data();
    }
    base.val = x.val();
    base.r = r;
    base.rank = rank;
    const crd::u32 tmp_levels = rank >= 3U ? rank - 2U : 0U;
    // deterministic tasking: grain a function of shape/nnz ONLY
    const crd::u64 per_root = 2ULL * r * (x.nnz() / n0 + 1ULL);
    crd::u32 tasks = task_count(n0, per_root, 1024U);
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || tasks < 2U)
    {
        tasks = 1U;
    }
    ScratchBlock<T> tmps(scratch, static_cast<crd::u64>(tasks) * tmp_levels * r);
    if (tmp_levels > 0U && tmps.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    const auto run_roots = [&base, tmp_levels, pout, n0](crd::u64 rb, crd::u64 re, T* tmpbase) noexcept
    {
        (void)n0;
        MttkrpCtx<T> c = base;
        for (crd::u32 l = 1; l <= tmp_levels; ++l)
        {
            c.tmp[l] = tmpbase + static_cast<crd::u64>(l - 1U) * c.r;
        }
        for (crd::u64 z0 = rb; z0 < re; ++z0)
        {
            T* dst = pout + static_cast<crd::u64>(c.fids[0][z0]) * c.r;
            const crd::u32 cb = c.fptr[0][z0];
            const crd::u32 ce = c.fptr[0][z0 + 1U];
            for (crd::u32 t = cb; t < ce; ++t)
            {
                mttkrp_node(c, 1U, t, dst);
            }
        }
    };
    if (tasks == 1U)
    {
        run_roots(0U, n0, tmps.m_ptr);
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_roots)* run;
        T* tmps;
        crd::u64 n0;
        crd::u64 stride; // tmp_levels * r
        crd::u32 tasks;
    };
    Ctx ctx{&run_roots, tmps.m_ptr, n0, static_cast<crd::u64>(tmp_levels) * r, tasks};
    Ctx* const cp = &ctx;
    auto* const counter = crd::jobs::parallel_for(tasks, nw, [cp](crd::u32 tb, crd::u32 te) {
        for (crd::u32 t = tb; t < te; ++t)
        {
            const crd::u64 rb = cp->n0 * t / cp->tasks;
            const crd::u64 re = cp->n0 * (t + 1U) / cp->tasks;
            if (rb < re)
            {
                (*cp->run)(rb, re, cp->tmps + static_cast<crd::u64>(t) * cp->stride);
            }
        }
    });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

// COO MTTKRP (reference path; serial): target = MODE 0 of the canonical COO.
// Walks nonzeros in canonical order with fiber-boundary detection — the
// exact CSF op sequence ⇒ bit-identical to mttkrp() on the identity-order
// CSF (gated). factors[0] is ignored.
template <typename T>
[[nodiscard]] TensorStatus mttkrp_coo(const SparseCoo<T>& x, crd::containers::ConstSpan<TensorView<const T>> factors,
                                      TensorView<T> out, crd::memory::IAllocator* scratch)
{
    using namespace sparsedetail;
    const crd::u32 rank = x.rank();
    if (rank < 2U || factors.size() != rank || !tight2(out) || scratch == nullptr)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 r = out.shape(1);
    if (out.shape(0) != x.shape(0))
    {
        return TensorStatus::ShapeMismatch;
    }
    for (crd::u32 m = 1; m < rank; ++m)
    {
        const TensorView<const T>& fm = factors[m];
        if (!tight2(fm) || fm.shape(0) != x.shape(m) || fm.shape(1) != r)
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    T* pout = out.data();
    std::memset(pout, 0, static_cast<crd::usize>(out.shape(0) * r) * sizeof(T));
    const crd::u64 n = x.nnz();
    if (n == 0U || r == 0U)
    {
        return TensorStatus::Ok;
    }
    const crd::u32 tmp_levels = rank >= 3U ? rank - 2U : 0U;
    ScratchBlock<T> tmps(scratch, static_cast<crd::u64>(tmp_levels) * r);
    if (tmp_levels > 0U && tmps.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    const crd::u32* planes[kMaxRank];
    const T* fac[kMaxRank];
    T* tmp[kMaxRank] = {};
    for (crd::u32 m = 0; m < rank; ++m)
    {
        planes[m] = x.idx(m);
        fac[m] = m == 0U ? nullptr : factors[m].data();
    }
    for (crd::u32 l = 1; l <= tmp_levels; ++l)
    {
        tmp[l] = tmps.m_ptr + static_cast<crd::u64>(l - 1U) * r;
    }
    const T* pval = x.val();
    T* dst = nullptr;
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u32 d = 0; // first level whose key changed vs the previous entry
        if (e > 0U)
        {
            d = rank; // canonical COO is deduped ⇒ some level always differs
            for (crd::u32 m = 0; m < rank; ++m)
            {
                if (planes[m][e] != planes[m][e - 1U])
                {
                    d = m;
                    break;
                }
            }
            CRD_ASSERT_MSG(d < rank, "mttkrp_coo: duplicate tuple in canonical COO");
            if (rank >= 3U) // close finished fibers, deepest first
            {
                const crd::u32 lo = d > 1U ? d : 1U;
                for (crd::u32 l = rank - 2U; l + 1U > lo; --l)
                {
                    T* parent = l == 1U ? dst : tmp[l - 1U];
                    hadamard_acc(parent, tmp[l],
                                 fac[l] + static_cast<crd::u64>(planes[l][e - 1U]) * r, r);
                }
            }
        }
        for (crd::u32 l = d; l + 1U < rank; ++l) // open levels d..rank-2
        {
            if (l == 0U)
            {
                dst = pout + static_cast<crd::u64>(planes[0][e]) * r;
            }
            else
            {
                std::memset(tmp[l], 0, static_cast<crd::usize>(r) * sizeof(T));
            }
        }
        T* leafdst = rank >= 3U ? tmp[rank - 2U] : dst;
        axpy_row(leafdst, fac[rank - 1U] + static_cast<crd::u64>(planes[rank - 1U][e]) * r, pval[e], r);
    }
    if (rank >= 3U) // close the trailing fibers
    {
        for (crd::u32 l = rank - 2U; l >= 1U; --l)
        {
            T* parent = l == 1U ? dst : tmp[l - 1U];
            hadamard_acc(parent, tmp[l], fac[l] + static_cast<crd::u64>(planes[l][n - 1U]) * r, r);
        }
    }
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
