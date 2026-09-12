#pragma once

// residual_function.hpp — Phase 3.1.6 v7-e: ResidualFunction<T> — a vector residual r(x) ∈ R^m for nonlinear
// least-squares min ½‖r(x)‖² (curve fitting, bundle adjustment, Kalman/SLAM, calibration). DISTINCT from
// Objective<T> (a scalar f): least-squares needs the Jacobian J = ∂r/∂x to exploit the Gauss-Newton structure
// JᵀJ ≈ ∇²f, which a scalar interface throws away (advisor-pinned). Raw lower-layer f32/f64 (ADR-0078 §5). ADR-0090.
//
// CAPABILITY CONTRACT (LinearOp/Objective-style): `has_jacobian()` is the queried capability (set in the protected
// ctor); `jacobian()` returns true iff it filled J. No analytic Jacobian ⇒ the solver finite-differences (later).
//
// VTABLE (LOCKED — append AT END forever, feedback_vtable_stability_append_at_end):
//   0 dtor · 1 residuals · 2 num_residuals · 3 n · 4 jacobian
//   [RESERVED, appended at END when needed: a SPARSE jacobian (CSR) provider — the v7-e-2 sparse-LM crush path —
//    and a FUSED residuals_and_jacobian; reserving them now keeps adding them non-breaking.]

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

namespace crd::hesap::opt
{

template <typename T>
class ResidualFunction
{
public:
    virtual ~ResidualFunction() = default;

    // r = f(x) ∈ R^m. Caller guarantees x.size() == n() and r.size() == num_residuals().
    virtual void residuals(crd::containers::ConstSpan<T> x, crd::containers::Span<T> r) const = 0;

    // m — the number of residuals.
    [[nodiscard]] virtual crd::usize num_residuals() const noexcept = 0;

    // n — the number of parameters (length of x).
    [[nodiscard]] virtual crd::usize n() const noexcept = 0;

    // Dense Jacobian J (m×n), ROW-MAJOR: jac[i*n + j] = ∂r_i/∂x_j. Returns true iff filled. Default = not provided
    // (the solver finite-differences). A subclass that sets has_jacobian=true MUST override and return true.
    [[nodiscard]] virtual bool jacobian(crd::containers::ConstSpan<T> x, crd::containers::Span<T> jac) const
    {
        (void)x;
        (void)jac;
        return false;
    }

    [[nodiscard]] bool has_jacobian() const noexcept { return m_has_jacobian; }

    // SPARSE Jacobian (CSR, m×n): fills `out` (the caller owns + reuses it). The v7-e-2 sparse-LM path forms
    // JᵀJ from this and factors it with the moat-proven hesap-direct sparse Cholesky. For a fixed-pattern NLS the
    // sparsity is constant across iterations (values change). Returns true iff filled. Vtable slot appended at END.
    [[nodiscard]] virtual bool sparse_jacobian(crd::containers::ConstSpan<T> x,
                                               sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& out) const
    {
        (void)x;
        (void)out;
        return false;
    }

    [[nodiscard]] bool has_sparse_jacobian() const noexcept { return m_has_sparse_jacobian; }

protected:
    ResidualFunction() = default;
    explicit ResidualFunction(bool has_jacobian, bool has_sparse_jacobian = false) noexcept
        : m_has_jacobian(has_jacobian), m_has_sparse_jacobian(has_sparse_jacobian)
    {
    }
    ResidualFunction(const ResidualFunction&) = default;
    ResidualFunction(ResidualFunction&&) noexcept = default;
    ResidualFunction& operator=(const ResidualFunction&) = default;
    ResidualFunction& operator=(ResidualFunction&&) noexcept = default;

    bool m_has_jacobian = false;
    bool m_has_sparse_jacobian = false;
};

} // namespace crd::hesap::opt
