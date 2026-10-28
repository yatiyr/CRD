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
//
// Both walks run in the per-level tensor kernels (tensor_kernels.hpp
// SparseTensorKernels); this header validates, partitions and allocates.
// ---------------------------------------------------------------------------
#include "sparse.hpp"

namespace crd::hesap::tensor
{

namespace sparsedetail
{

// No two neighbouring nonzeros of a canonical COO share their tuple (the debug check of mttkrp_coo's input).
template <typename T> [[nodiscard]] bool distinct_neighbours(const SparseCoo<T>& x) noexcept
{
    for (crd::u64 e = 1; e < x.nnz(); ++e)
    {
        bool same = true;
        for (crd::u32 m = 0; m < x.rank() && same; ++m)
        {
            same = x.idx(m)[e] == x.idx(m)[e - 1U];
        }
        if (same)
        {
            return false;
        }
    }
    return true;
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
    MttkrpCsfWalk<T> walk{};
    for (crd::u32 l = 0; l < rank; ++l)
    {
        walk.fids[l] = x.fids(l);
        if (l + 1U < rank)
        {
            walk.fptr[l] = x.fptr(l);
        }
        walk.fac[l] = l == 0U ? nullptr : factors[x.order(l)].data();
    }
    walk.val = x.val();
    walk.out = pout;
    walk.r = r;
    walk.rank = rank;
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
    const SparseTensorKernels<T>& kernels = sparse_kernels<T>();
    if (tasks == 1U)
    {
        kernels.mttkrp_csf(walk, 0U, n0, tmps.m_ptr);
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const SparseTensorKernels<T>* kernels;
        const MttkrpCsfWalk<T>* walk;
        T* tmps;
        crd::u64 n0;
        crd::u64 stride; // tmp_levels * r
        crd::u32 tasks;
    };
    Ctx ctx{&kernels, &walk, tmps.m_ptr, n0, static_cast<crd::u64>(tmp_levels) * r, tasks};
    Ctx* const cp = &ctx;
    auto* const counter = crd::jobs::parallel_for(
        tasks, nw,
        [cp](crd::u32 tb, crd::u32 te)
        {
            for (crd::u32 t = tb; t < te; ++t)
            {
                const crd::u64 rb = cp->n0 * t / cp->tasks;
                const crd::u64 re = cp->n0 * (t + 1U) / cp->tasks;
                if (rb < re)
                {
                    cp->kernels->mttkrp_csf(*cp->walk, rb, re, cp->tmps + static_cast<crd::u64>(t) * cp->stride);
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
    CRD_ASSERT_MSG(distinct_neighbours(x), "mttkrp_coo: duplicate tuple in canonical COO");
    const crd::u32 tmp_levels = rank >= 3U ? rank - 2U : 0U;
    ScratchBlock<T> tmps(scratch, static_cast<crd::u64>(tmp_levels) * r);
    if (tmp_levels > 0U && tmps.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    MttkrpCooWalk<T> walk{};
    for (crd::u32 m = 0; m < rank; ++m)
    {
        walk.planes[m] = x.idx(m);
        walk.fac[m] = m == 0U ? nullptr : factors[m].data();
    }
    walk.val = x.val();
    walk.out = pout;
    walk.n = n;
    walk.r = r;
    walk.rank = rank;
    sparse_kernels<T>().mttkrp_coo(walk, tmps.m_ptr);
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
