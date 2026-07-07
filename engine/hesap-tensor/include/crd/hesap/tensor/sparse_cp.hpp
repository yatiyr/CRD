#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — the v14-i × v14-j integration glue: SPARSE CP-ALS.
// v14-j's `cp_als_generic` accepts the tensor ONLY through its MTTKRP seam;
// this header wires v14-i's CSF MTTKRP into that seam. One CSF per mode is
// built lazily on first use (SPLATT-standard [mode, ascending-others] level
// order — the root level IS the MTTKRP target).
//
// Lifetime (the borrowed-lifetime scar): SparseCpMttkrp BORROWS the COO for
// the duration of ONE cp_als_sparse call, exactly like DenseMttkrp borrows its
// view — never store either beyond the call. cp_als_sparse enforces this by
// construction (the functor lives on its stack).
// ---------------------------------------------------------------------------
#include "decomp.hpp"
#include "sparse.hpp"
#include "sparse_mttkrp.hpp"

namespace crd::hesap::tensor
{

template <typename T>
class SparseCpMttkrp
{
public:
    SparseCpMttkrp(const SparseCoo<T>& x, crd::memory::IAllocator* alloc) noexcept
        : m_x(&x), m_alloc(alloc),
          m_csf{SparseCsf<T>(alloc), SparseCsf<T>(alloc), SparseCsf<T>(alloc), SparseCsf<T>(alloc),
                SparseCsf<T>(alloc), SparseCsf<T>(alloc), SparseCsf<T>(alloc), SparseCsf<T>(alloc)}
    {
        static_assert(kMaxRank == 8U, "the eager per-mode CSF init above assumes kMaxRank == 8");
        for (crd::u32 m = 0; m < kMaxRank; ++m)
        {
            m_built[m] = false;
        }
    }

    [[nodiscard]] TensorStatus operator()(crd::u32 mode, crd::containers::ConstSpan<Tensor<T>> factors,
                                          crd::u64 rank, crd::containers::Span<T> out) noexcept
    {
        const crd::u32 nd = m_x->rank();
        if (mode >= nd || factors.size() != nd)
        {
            return TensorStatus::BadInput;
        }
        if (!m_built[mode])
        {
            crd::u32 order[kMaxRank];
            order[0] = mode;
            crd::u32 w = 1;
            for (crd::u32 m = 0; m < nd; ++m)
            {
                if (m != mode)
                {
                    order[w++] = m;
                }
            }
            const TensorStatus st = coo_to_csf(*m_x, {order, nd}, m_csf[mode]);
            if (st != TensorStatus::Ok)
            {
                return st;
            }
            m_built[mode] = true;
        }
        // adapt the seam's Tensor spans to the sparse kernel's view spans
        TensorView<const T> views[kMaxRank];
        for (crd::u32 m = 0; m < nd; ++m)
        {
            views[m] = TensorView<const T>(factors[m].view());
        }
        const crd::u64 rows = m_x->shape(mode);
        if (out.size() != rows * rank)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 oshape[2] = {rows, rank};
        const crd::i64 ostride[2] = {static_cast<crd::i64>(rank), 1};
        TensorView<T> ov(out.data(), {oshape, 2U}, {ostride, 2U});
        return mttkrp(m_csf[mode], {views, nd}, ov, m_alloc, 1U);
    }

private:
    const SparseCoo<T>* m_x; // borrowed — call-scoped only (see the header note)
    crd::memory::IAllocator* m_alloc;
    SparseCsf<T> m_csf[kMaxRank];
    bool m_built[kMaxRank];
};

// Sparse CP-ALS: `factors` arrive initialized (the caller picks the init; the
// dense wrapper's Philox init pattern is the house default). weights.size()
// == rank. Bounded per CpOptions; NotConverged reported via DecompStatus.
template <typename T>
[[nodiscard]] DecompStatus cp_als_sparse(const SparseCoo<T>& x, crd::u64 rank,
                                         crd::containers::Span<Tensor<T>> factors, crd::containers::Span<T> weights,
                                         CpInfo<T>& info, crd::memory::IAllocator* alloc,
                                         const CpOptions<T>& opts = {}) noexcept
{
    const crd::u32 nd = x.rank();
    if (nd < 2U || factors.size() != nd)
    {
        return DecompStatus::BadInput;
    }
    crd::u64 shape[kMaxRank];
    for (crd::u32 m = 0; m < nd; ++m)
    {
        shape[m] = x.shape(m);
    }
    // ||X||^2 over the stored values (implicit zeros contribute nothing)
    T nrm2 = T(0);
    const T* vals = x.val();
    for (crd::u64 i = 0; i < x.nnz(); ++i)
    {
        nrm2 = std::fma(vals[i], vals[i], nrm2);
    }
    SparseCpMttkrp<T> seam(x, alloc);
    return cp_als_generic<T>(crd::containers::ConstSpan<crd::u64>{shape, nd}, nrm2, seam, rank, factors, weights,
                             info, alloc, opts);
}

} // namespace crd::hesap::tensor
