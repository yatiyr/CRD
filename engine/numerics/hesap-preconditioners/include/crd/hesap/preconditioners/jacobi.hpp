#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// JacobiPreconditioner<T> -- diagonal (Jacobi) preconditioner. Phase 3.1.6 v4a.
//
// M = diag(A); the action z = M⁻¹ r is z[i] = r[i] / A[i,i], precomputed as a
// stored inverse-diagonal. A LinearOp<T> exposing M⁻¹. Works for ANY matrix
// with a nonzero diagonal (NOT restricted to SPD). Parallel-trivial and
// bit-deterministic (each output element is one independent multiply).
//
// Precondition: every diagonal entry is structurally present and nonzero
// (asserted at construction).
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
[[nodiscard]] inline T precond_conj(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return T{v.re, -v.im};
    }
    else
    {
        return v;
    }
}
} // namespace detail

template <typename T>
class JacobiPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    JacobiPreconditioner(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                         crd::memory::IAllocator*                                                          alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/true, /*has_adjoint=*/true), m_inv_diag(alloc), m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "JacobiPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "JacobiPreconditioner: requires a compressed CSR matrix");
        const auto& pat  = a.pattern();
        const auto* outer = pat.outer_ptr.data();
        const auto* inner = pat.inner_idx.data();
        const T*    vals  = a.values().values.data();
        m_inv_diag.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            T diag = T{};
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                if (inner[k] == i)
                {
                    diag = vals[k];
                    break;
                }
            }
            // Graceful fallback: a zero/missing diagonal -> identity for that row
            // (inv = 1), never a crash or an inf. (Eigen's DiagonalPreconditioner
            // stores 1/diag and lets the inf propagate; Cerid is deliberately more
            // robust here.) SPD/HPD inputs (the CG path) always have a nonzero
            // diagonal, so this only engages for general matrices (the GMRES path,
            // e.g. circuit / optimization matrices with structural-zero diagonals).
            m_inv_diag[i] = (diag == T{}) ? T(1) : T(1) / diag;
        }
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            y[i] = m_inv_diag[i] * x[i];
        }
        return true;
    }

    // diag(A)ᵀ = diag(A) ⇒ transpose action == apply.
    [[nodiscard]] bool apply_transpose(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        return apply(x, y);
    }

    // (M⁻¹)ᴴ = diag(conj(1/A[i,i])); for real T this equals apply.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            y[i] = detail::precond_conj<T>(m_inv_diag[i]) * x[i];
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    crd::containers::Array<T> m_inv_diag;
    crd::u32                  m_n;
};

} // namespace crd::hesap::preconditioners
