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
// LeastSquaresColumnJacobi<T> -- column (Jacobi) preconditioner for LSQR/LSMR.
// Phase 3.1.6 v4d-1-precond.
//
// The least-squares analogue of Jacobi: M = diag(AᴴA), so the action is the
// n-space column scaling z[j] = r[j] / ‖A·,j‖₂². This is the ONLY Jacobi-type
// preconditioner that generalizes to a RECTANGULAR (m×n) A -- diag(A) (square
// Jacobi), block-Jacobi, and SSOR all require a square operator with a diagonal
// block structure a least-squares operator does not have, so they are
// deliberately absent from the least-squares selector (not deferred: no
// consumer ever applies SSOR to a least-squares normal operator).
//
// diag(AᴴA)[j] = Σ_i |A[i,j]|² is accumulated in one pass over the CSR values
// (A is stored row-major). The diagonal is real and positive (it is an SPD/HPD
// operator), so M⁻¹ is a real positive diagonal ⇒ apply == apply_transpose ==
// apply_adjoint, and the action is parallel-trivial and bit-deterministic.
//
// Graceful fallback: a structurally-empty column (‖A·,j‖₂² == 0) maps to inv = 1
// rather than an inf, mirroring JacobiPreconditioner.
// -----------------------------------------------------------------------

template <typename T>
class LeastSquaresColumnJacobi final : public crd::hesap::LinearOp<T>
{
public:
    using R = crd::hesap::dense::RealType<T>;

    LeastSquaresColumnJacobi(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                             crd::memory::IAllocator*                                                          alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/true, /*has_adjoint=*/true), m_inv_diag(alloc), m_n(a.cols())
    {
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "LeastSquaresColumnJacobi: requires a compressed CSR matrix");
        const auto&    pat   = a.pattern();
        const auto*    outer = pat.outer_ptr.data();
        const auto*    inner = pat.inner_idx.data();
        const T*       vals  = a.values().values.data();
        const crd::u32 rows  = a.rows();

        crd::containers::Array<R> colsq(alloc);
        colsq.resize(m_n);
        for (crd::u32 j = 0; j < m_n; ++j)
        {
            colsq[j] = R(0);
        }
        for (crd::u32 i = 0; i < rows; ++i)
        {
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                const T v = vals[k];
                if constexpr (crd::hesap::dense::is_complex_v<T>)
                {
                    colsq[inner[k]] += v.re * v.re + v.im * v.im; // |A[i,j]|²
                }
                else
                {
                    colsq[inner[k]] += v * v;
                }
            }
        }

        m_inv_diag.resize(m_n);
        for (crd::u32 j = 0; j < m_n; ++j)
        {
            // diag(AᴴA) is real-positive; store its reciprocal as a real scalar
            // promoted to T (zero column -> identity, never inf).
            m_inv_diag[j] = (colsq[j] == R(0)) ? T(1) : T(R(1) / colsq[j]);
        }
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        for (crd::u32 j = 0; j < m_n; ++j)
        {
            y[j] = m_inv_diag[j] * x[j];
        }
        return true;
    }

    // diag is real ⇒ transpose and adjoint actions are identical to apply.
    [[nodiscard]] bool apply_transpose(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        return apply(x, y);
    }

    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        return apply(x, y);
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    crd::containers::Array<T> m_inv_diag; // diag(AᴴA)⁻¹, real values stored in T
    crd::u32                  m_n;        // number of columns of A
};

} // namespace crd::hesap::preconditioners
