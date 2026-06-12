#pragma once

// ode_function.hpp — Phase 3.1.6 v9-a: the OdeFunction<T> interface — the right-hand side y' = f(t, y) an
// integrator advances, with optional derivative capabilities. Mirrors crd::hesap::opt::Objective<T>'s
// capability contract (the v7 convention all 13 v9 subslices share). Raw lower-layer per ADR-0078 §5 —
// typed-units bridges live at consumer surfaces (eylem/time), not here. ADR-0091.
//
// CAPABILITY CONTRACT (the v7 Objective convention):
//   • `has_*()` are the QUERIED capabilities (set once in the protected ctor). Drivers check these to pick
//     analytic vs finite-difference Jacobians (v9-d).
//   • Optional virtuals return TRUE iff they actually filled the output. A subclass that sets the flag MUST
//     return true; leaving it false signals "not provided".
//   • Drivers own ALL work counting (OdeWork) — implementations stay pure evaluations (const, reentrant).
//
// VTABLE (LOCKED — new virtuals append AT END forever, per feedback_vtable_stability_append_at_end):
//   0 dtor · 1 rhs · 2 dim · 3 jacobian · 4 jacobian_vector · 5 mass_matrix (v9-h)
//   [PLANNED append: sparse_jacobian over hesap-sparse CSR (v9-j). Event functions are deliberately NOT
//    part of this interface — they are integration options (v9-c), scipy-style.]
//
// NOTE for hot loops: this virtual interface is the DRIVER-layer contract (callback cost amortized over the
// state dimension). Per-body game/animation integration must use the steppers.hpp kernels directly with an
// inlined RHS — never a virtual f per body per substep (memory `project_ode_in_games_layering`).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

#include <utility>

namespace crd::hesap::ode
{

template <typename T> class OdeFunction
{
public:
    virtual ~OdeFunction() = default;

    // ---- Required overrides ----

    // f(t, y) → dydt. Caller guarantees y.size() == dim() == dydt.size().
    virtual void rhs(T t, crd::containers::ConstSpan<T> y, crd::containers::Span<T> dydt) const = 0;

    // The state dimension n.
    [[nodiscard]] virtual crd::usize dim() const noexcept = 0;

    // ---- Optional overrides (default = not provided) ----

    // The Jacobian J = ∂f/∂y at (t, y) → jac (n×n, ROW-MAJOR, jac.size() == n²). Returns true iff filled.
    // Default = not provided ⇒ stiff drivers finite-difference (v9-d). Vtable slot 3.
    [[nodiscard]] virtual bool jacobian(T t, crd::containers::ConstSpan<T> y, crd::containers::Span<T> jac) const
    {
        (void)t;
        (void)y;
        (void)jac;
        return false;
    }

    // Jacobian-vector product J·v → jv (matrix-free Krylov Newton, v9-j = the CVODE SPGMR mode). Returns
    // true iff filled. Vtable slot 4.
    [[nodiscard]] virtual bool jacobian_vector(T t, crd::containers::ConstSpan<T> y, crd::containers::ConstSpan<T> v,
                                               crd::containers::Span<T> jv) const
    {
        (void)t;
        (void)y;
        (void)v;
        (void)jv;
        return false;
    }

    // CONSTANT mass matrix M (n×n ROW-MAJOR, possibly SINGULAR ⇒ semi-explicit index-1 DAE: the system
    // is M·y' = f(t, y)). Returns true iff filled. Queried ONCE per integration. v9-h append (vtable
    // slot 5 — appended at END per the locked-vtable discipline). Time/state-dependent M(t, y) = a named
    // follow-up decided with measurements (the v9-h plan note).
    [[nodiscard]] virtual bool mass_matrix(crd::containers::Span<T> m) const
    {
        (void)m;
        return false;
    }

    // SPARSE Jacobian ∂f/∂y (CSR, n×n; the caller owns + reuses `out` — the fixed-pattern symbolic-once
    // contract, exactly the v7-g sparse_hessian convention). The v9-j large-n MOL path: stiff drivers
    // hand it to a sparse `OdeLinearSolver` (hesap-direct multifrontal LU). Returns true iff filled.
    // v9-j append (vtable slot 6 — END).
    [[nodiscard]] virtual bool sparse_jacobian(T t, crd::containers::ConstSpan<T> y,
                                               crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& out) const
    {
        (void)t;
        (void)y;
        (void)out;
        return false;
    }

    // ---- Capability queries (non-virtual; set in the protected ctor) ----

    [[nodiscard]] bool has_jacobian() const noexcept { return m_has_jacobian; }
    [[nodiscard]] bool has_jacobian_vector() const noexcept { return m_has_jacobian_vector; }
    [[nodiscard]] bool has_mass_matrix() const noexcept { return m_has_mass_matrix; }
    [[nodiscard]] bool has_sparse_jacobian() const noexcept { return m_has_sparse_jacobian; }

protected:
    void set_has_sparse_jacobian(bool v) noexcept { m_has_sparse_jacobian = v; } // ctor-time, v9-j

protected:
    OdeFunction() = default;
    explicit OdeFunction(bool has_jacobian, bool has_jacobian_vector = false, bool has_mass_matrix = false) noexcept
        : m_has_jacobian(has_jacobian), m_has_jacobian_vector(has_jacobian_vector),
          m_has_mass_matrix(has_mass_matrix)
    {
    }
    OdeFunction(const OdeFunction&) = default;
    OdeFunction(OdeFunction&&) noexcept = default;
    OdeFunction& operator=(const OdeFunction&) = default;
    OdeFunction& operator=(OdeFunction&&) noexcept = default;

    bool m_has_jacobian = false;
    bool m_has_jacobian_vector = false;
    bool m_has_mass_matrix = false;
    bool m_has_sparse_jacobian = false;
};

// Adapter: wrap any callable `f(T t, ConstSpan<const T> y, Span<T> dydt)` as an OdeFunction (the v7
// FunctorObjective pattern — scripts/tests/CLI ergonomics; RHS-only, no derivative capabilities).
template <typename T, typename F> class FunctorOdeFunction final : public OdeFunction<T>
{
public:
    FunctorOdeFunction(crd::usize n, F f) noexcept : m_n(n), m_f(std::move(f)) {}

    void rhs(T t, crd::containers::ConstSpan<T> y, crd::containers::Span<T> dydt) const override { m_f(t, y, dydt); }

    [[nodiscard]] crd::usize dim() const noexcept override { return m_n; }

private:
    crd::usize m_n;
    F m_f;
};

template <typename T, typename F> [[nodiscard]] FunctorOdeFunction<T, F> make_ode_function(crd::usize n, F f)
{
    return FunctorOdeFunction<T, F>(n, std::move(f));
}

} // namespace crd::hesap::ode
