#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// ReorderedPreconditioner<T, Inner> -- AMD fill-reducing-reordered wrapper for any
// factorization-based preconditioner. Phase 3.1.6 v4i-1.
//
// Eigen's IncompleteLUT / IncompleteCholesky apply an AMD reordering of the
// symmetrized pattern (Aᵀ+A) BEFORE factoring (IncompleteLUT.h analyzePattern) --
// reordering both shrinks the incomplete factor's fill AND raises its quality on
// hard nonsymmetric / non-diagonally-dominant systems (e.g. sherman3). Cerid's
// bare ILUT/IC0/ILU(p) factor the matrix as given; this adapter brings the same
// reference-standard reordering, reusing the v2 `amd_order` (deterministic, the
// D(ord) pins) so the determinism moat is preserved.
//
//   M_inner ≈ (P·A·Pᵀ)⁻¹  ⇒  A⁻¹ ≈ Pᵀ·M_inner⁻¹·P
//   apply(r)         = Pᵀ · M_inner.apply(P·r)
//   apply_adjoint(r) = Pᵀ · M_inner.apply_adjoint(P·r)   ((PᵀMP)ᴴ = PᵀMᴴP)
//
// `Inner` is constructed on the permuted matrix PAPᵀ (owned by the adapter) with
// the forwarded constructor arguments, e.g.
//   ReorderedPreconditioner<f64, IlutPreconditioner<f64>> m(a, alloc, lfil, droptol);
// The permutation P is symmetric (rows + columns), so PAPᵀ keeps the diagonal on
// the diagonal -- essential since ILUT does no numerical pivoting.
// -----------------------------------------------------------------------

template <typename T, typename Inner>
class ReorderedPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    template <typename... Args>
    ReorderedPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, Args&&... inner_args)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_perm(crd::hesap::ordering::amd_order(a.pattern(), alloc))
        , m_ar(build_permuted(a, m_perm, alloc))
        , m_inner(m_ar, alloc, std::forward<Args>(inner_args)...)
        , m_rp(alloc)
        , m_zp(alloc)
        , m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "ReorderedPreconditioner: matrix must be square");
        m_rp.resize(m_n);
        m_zp.resize(m_n);
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        const auto* p = m_perm.perm.data();
        for (crd::u32 i = 0; i < m_n; ++i) { m_rp[i] = r[p[i]]; }          // rp = P·r
        (void)m_inner.apply(crd::containers::ConstSpan<T>{m_rp.data(), m_n},
                            crd::containers::Span<T>{m_zp.data(), m_n});    // zp = M_inner⁻¹·rp
        for (crd::u32 i = 0; i < m_n; ++i) { z[p[i]] = m_zp[i]; }          // z  = Pᵀ·zp
        return true;
    }

    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        const auto* p = m_perm.perm.data();
        for (crd::u32 i = 0; i < m_n; ++i) { m_rp[i] = r[p[i]]; }
        (void)m_inner.apply_adjoint(crd::containers::ConstSpan<T>{m_rp.data(), m_n},
                                    crd::containers::Span<T>{m_zp.data(), m_n});
        for (crd::u32 i = 0; i < m_n; ++i) { z[p[i]] = m_zp[i]; }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    [[nodiscard]] const Inner& inner() const noexcept { return m_inner; }

private:
    // PAPᵀ with values: new(inv_perm[i], inv_perm[j]) = A(i, j) (symmetric reorder).
    [[nodiscard]] static Csr build_permuted(const Csr& a, const crd::hesap::ordering::Permutation& p,
                                            crd::memory::IAllocator* alloc)
    {
        const crd::u32 n     = a.rows();
        const auto*    ip    = p.inv_perm.data();
        const auto*    outer = a.pattern().outer_ptr.data();
        const auto*    inner = a.pattern().inner_idx.data();
        const T*       vals  = a.values().values.data();
        crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
            {
                tb.add(ip[i], ip[inner[q]], vals[q]);
            }
        }
        return tb.compress();
    }

    crd::hesap::ordering::Permutation m_perm;
    Csr                               m_ar;    // PAPᵀ (owned; Inner is built on it)
    Inner                             m_inner; // factorized on the reordered matrix
    mutable crd::containers::Array<T> m_rp;    // P·r scratch
    mutable crd::containers::Array<T> m_zp;    // M_inner⁻¹·(P·r) scratch
    crd::u32                          m_n;
};

} // namespace crd::hesap::preconditioners
