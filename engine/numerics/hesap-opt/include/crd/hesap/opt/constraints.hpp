#pragma once

// constraints.hpp — Phase 3.1.6 v7-j: the Constraints<T> interface — the constraint block of a smooth
// constrained problem, paired with an Objective<T>:
//
//     min f(x)   s.t.   c_E(x) = 0   (num_eq equalities),   c_I(x) ≥ 0   (num_ineq inequalities)
//
// SIGN CONVENTIONS (PINNED here for every v7-j..n consumer; Nocedal & Wright Ch.12):
//   • c_E(x) = 0, c_I(x) ≥ 0 (a "≤" constraint enters negated; bounds enter as rows of c_I until v7-k/n add
//     a dedicated bound fast path).
//   • Lagrangian L(x, λ, μ) = f(x) − λᵀc_E(x) − μᵀc_I(x); KKT stationarity ∇f = J_Eᵀλ + J_Iᵀμ with μ ≥ 0.
// Raw lower-layer f32/f64 (ADR-0078 §5). ADR-0090.
//
// CAPABILITY CONTRACT (Objective/ResidualFunction-style): `has_jacobians()` queried, `jacobians()` returns true
// iff it filled both (an absent capability ⇒ the solver finite-differences — not built in v7-j).
// `has_lagrangian_hessian()`: true iff the constraints contribute curvature − Σλ_i∇²c_E,i − Σμ_i∇²c_I,i through
// `add_lagrangian_hessian` (LINEAR constraints have ZERO contribution — leave the capability false and the SQP
// Hessian is exact anyway; a false capability on NONLINEAR constraints means the solver runs with W = ∇²f, the
// standard "Hessian of the objective only" approximation — documented at the call site).
//
// VTABLE (LOCKED — new virtuals append AT END forever, per feedback_vtable_stability_append_at_end):
//   0 dtor · 1 num_eq · 2 num_ineq · 3 n · 4 eval · 5 jacobians · 6 add_lagrangian_hessian
//   [RESERVED, appended at END when the large-scale path needs them: SPARSE jacobians (CSR) + a fused
//    eval_and_jacobians.]

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::opt
{

template <typename T> class Constraints
{
public:
    virtual ~Constraints() = default;

    // ---- Required overrides ----

    [[nodiscard]] virtual crd::usize num_eq() const noexcept = 0;
    [[nodiscard]] virtual crd::usize num_ineq() const noexcept = 0;

    // The problem dimension (length of x) — must match the paired Objective's n().
    [[nodiscard]] virtual crd::usize n() const noexcept = 0;

    // Evaluate both constraint vectors at x: ce (length num_eq) and ci (length num_ineq).
    virtual void eval(crd::containers::ConstSpan<T> x, crd::containers::Span<T> ce,
                      crd::containers::Span<T> ci) const = 0;

    // ---- Optional overrides (default = not provided) ----

    // Dense ROW-MAJOR Jacobians: je (num_eq × n, je[i*n+j] = ∂c_E,i/∂x_j) and ji (num_ineq × n). Returns true
    // iff BOTH were filled. A subclass that sets has_jacobians=true MUST override and return true.
    [[nodiscard]] virtual bool jacobians(crd::containers::ConstSpan<T> x, crd::containers::Span<T> je,
                                         crd::containers::Span<T> ji) const
    {
        (void)x;
        (void)je;
        (void)ji;
        return false;
    }

    // ADD the constraint-curvature part of the Lagrangian Hessian into `h` (n×n row-major, FULL symmetric):
    //     h += −Σ_i λ_i·∇²c_E,i(x) − Σ_i μ_i·∇²c_I,i(x)
    // (the L = f − λᵀc_E − μᵀc_I convention above). Returns true iff applied. Linear constraints contribute
    // nothing — leave the capability false.
    [[nodiscard]] virtual bool add_lagrangian_hessian(crd::containers::ConstSpan<T> x,
                                                      crd::containers::ConstSpan<T> lambda,
                                                      crd::containers::ConstSpan<T> mu,
                                                      crd::containers::Span<T> h) const
    {
        (void)x;
        (void)lambda;
        (void)mu;
        (void)h;
        return false;
    }

    // ---- Capability queries (non-virtual; set in the protected ctor) ----

    [[nodiscard]] bool has_jacobians() const noexcept { return m_has_jacobians; }
    [[nodiscard]] bool has_lagrangian_hessian() const noexcept { return m_has_lagrangian_hessian; }

protected:
    Constraints() = default;
    explicit Constraints(bool has_jacobians, bool has_lagrangian_hessian = false) noexcept
        : m_has_jacobians(has_jacobians), m_has_lagrangian_hessian(has_lagrangian_hessian)
    {
    }
    Constraints(const Constraints&) = default;
    Constraints(Constraints&&) noexcept = default;
    Constraints& operator=(const Constraints&) = default;
    Constraints& operator=(Constraints&&) noexcept = default;

    bool m_has_jacobians = false;
    bool m_has_lagrangian_hessian = false;
};

} // namespace crd::hesap::opt
