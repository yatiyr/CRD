#pragma once

#include <crd/core/assert.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SparseLinearOp<T> -- the first sparse consumer of the v0a LinearOp<T>
// interface (ADR-0065 §13 D3). Wraps a compressed CSR SparseMatrix as a
// matrix-free operator for Krylov solvers (v4) etc. NON-OWNING: the wrapped
// matrix must outlive the operator.
//
//   apply           = spmv(None)           : y = A x
//   apply_transpose = spmv(Transpose)      : y = A^T x
//   apply_adjoint   = spmv(ConjTranspose)  : y = A^H x  (== transpose for real T)
//
// All three are exact wrappers over spmv with alpha=1, beta=0 -- so they are
// bit-identical to the kernel (the LinearOp wrapper introduces no drift; a
// test asserts this).
// -----------------------------------------------------------------------

template <typename T>
class SparseLinearOp final : public crd::hesap::LinearOp<T>
{
public:
    explicit SparseLinearOp(const SparseMatrix<T, SparseFormat::Csr>& matrix) noexcept
        : crd::hesap::LinearOp<T>(/*has_transpose=*/true, /*has_adjoint=*/true), m_matrix(&matrix)
    {
        CRD_ASSERT_MSG(matrix.pattern().is_compressed(), "SparseLinearOp requires a compressed CSR matrix");
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        spmv<T>(T(1), *m_matrix, Trans::None, x, T(0), y);
        return true;
    }

    [[nodiscard]] bool apply_transpose(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        spmv<T>(T(1), *m_matrix, Trans::Transpose, x, T(0), y);
        return true;
    }

    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        spmv<T>(T(1), *m_matrix, Trans::ConjTranspose, x, T(0), y);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_matrix->rows(); }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_matrix->cols(); }

private:
    const SparseMatrix<T, SparseFormat::Csr>* m_matrix;
};

} // namespace crd::hesap::sparse
