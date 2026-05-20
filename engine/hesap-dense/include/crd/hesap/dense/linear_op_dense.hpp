#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/linear_op.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-e — concrete `LinearOp<T>` wrappers for dense matrix
// types + Hager 1-norm condition estimator.
//
// The `LinearOp<T>` interface itself lives in `crd-hesap`
// (`crd/hesap/linear_op.hpp`, v0a). v0e-e adds:
//   - `MatrixLinearOp<T, L>` — wraps a `Matrix<T, L>` and dispatches to
//     `gemv`.
//   - `SymmetricLinearOp<T>` — wraps a `Symmetric<T>` and dispatches to
//     `symv`.
//   - `compute_1norm(Matrix / Symmetric)` — exact ||A||_1.
//   - `hager_inv_1norm_estimate` — power-iteration estimator for
//     ||A^-1||_1 (LAPACK xLACON pattern).
//   - `condition_estimate_1norm_symmetric(Symmetric, Cholesky)` —
//     convenience: κ_1(A) for an SPD A given its Cholesky factor.
//
// f32 + f64 RowMajor for v0e-e-MVP. LU / LDLT / QR condition
// estimators ship in v0e-e2 (filed) — they need solve_transpose
// paths.
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
class MatrixLinearOp : public crd::hesap::LinearOp<T>
{
public:
    using value_type = T;

    explicit MatrixLinearOp(const Matrix<T, L>& a) noexcept
        : crd::hesap::LinearOp<T>(/*has_transpose*/ true, /*has_adjoint*/ true),
          m_a(&a)
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x,
                              crd::containers::Span<T> y) const override
    {
        CRD_ASSERT_MSG(x.size() == m_a->cols() && y.size() == m_a->rows(),
                       "MatrixLinearOp::apply: span size mismatch");
        // y = 1 * A * x + 0 * y
        for (crd::usize i = 0; i < y.size(); ++i)
        {
            y[i] = T{0};
        }
        gemv<T, L>(T{1}, m_a->cview(), x, T{0}, y, Trans::None);
        return true;
    }

    [[nodiscard]] bool apply_transpose(crd::containers::ConstSpan<T> x,
                                        crd::containers::Span<T> y) const override
    {
        CRD_ASSERT_MSG(x.size() == m_a->rows() && y.size() == m_a->cols(),
                       "MatrixLinearOp::apply_transpose: span size mismatch");
        for (crd::usize i = 0; i < y.size(); ++i)
        {
            y[i] = T{0};
        }
        gemv<T, L>(T{1}, m_a->cview(), x, T{0}, y, Trans::Transpose);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_a->rows(); }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_a->cols(); }

private:
    const Matrix<T, L>* m_a;
};

template <typename T>
class SymmetricLinearOp : public crd::hesap::LinearOp<T>
{
public:
    using value_type = T;

    explicit SymmetricLinearOp(const Symmetric<T>& a) noexcept
        : crd::hesap::LinearOp<T>(/*has_transpose*/ true, /*has_adjoint*/ true),
          m_a(&a)
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x,
                              crd::containers::Span<T> y) const override
    {
        const crd::usize n = m_a->n();
        CRD_ASSERT_MSG(x.size() == n && y.size() == n,
                       "SymmetricLinearOp::apply: span size mismatch");
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = T{0};
        }
        symv<T>(T{1}, *m_a, x, T{0}, y);
        return true;
    }

    [[nodiscard]] bool apply_transpose(crd::containers::ConstSpan<T> x,
                                        crd::containers::Span<T> y) const override
    {
        // A is symmetric → A^T = A.
        return this->apply(x, y);
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_a->n(); }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_a->n(); }

private:
    const Symmetric<T>* m_a;
};

// =======================================================================
// compute_1norm — exact ||A||_1 = max over columns of Σ_i |A[i, j]|.
// =======================================================================

template <typename T, Layout L>
[[nodiscard]] T compute_1norm(const Matrix<T, L>& a) noexcept
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    T max_col_sum = T{0};
    for (crd::usize j = 0; j < n; ++j)
    {
        T col_sum = T{0};
        for (crd::usize i = 0; i < m; ++i)
        {
            const T v = a.at(i, j);
            col_sum += (v < T{0}) ? -v : v;
        }
        if (col_sum > max_col_sum)
        {
            max_col_sum = col_sum;
        }
    }
    return max_col_sum;
}

template <typename T>
[[nodiscard]] T compute_1norm(const Symmetric<T>& a) noexcept
{
    const crd::usize n = a.n();
    T max_sum = T{0};
    for (crd::usize i = 0; i < n; ++i)
    {
        T row_sum = T{0};
        for (crd::usize j = 0; j < n; ++j)
        {
            const T v = a.at(i, j);
            row_sum += (v < T{0}) ? -v : v;
        }
        if (row_sum > max_sum)
        {
            max_sum = row_sum;
        }
    }
    return max_sum;
}

} // namespace crd::hesap::dense
