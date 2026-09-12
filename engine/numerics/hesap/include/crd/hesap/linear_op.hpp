#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap
{
// -----------------------------------------------------------------------
// LinearOp<T> — matrix-free linear operator interface.
//
// Per ADR-0065 §13 D3 (2026-05-19): foundational for Krylov solvers,
// FEM, PDE, Hessian-vector products in optimisation. Follows the
// PETSc Mat / Trilinos Tpetra::Operator pattern.
//
// Vtable shape (LOCKED v0a — new virtuals append AT END forever per
// feedback_vtable_stability_append_at_end):
//
//   Slot 0: dtor
//   Slot 1: apply           (pure)
//   Slot 2: n_rows          (pure)
//   Slot 3: n_cols          (pure)
//   Slot 4: apply_transpose (virtual; default returns false)
//   Slot 5: apply_adjoint   (virtual; default returns false)
//   [future slots appended at END]
//
// Why transpose/adjoint are virtual-with-default rather than pure:
// matrix-free preconditioners (SPAI, polynomial) and many sparse formats
// don't naturally provide a transpose action. Forcing them to fake one
// would mean every concrete LinearOp ships an extra dead method.
// BiCGSTAB / GMRES / LSQR query at runtime via has_transpose() /
// has_adjoint() and degrade to alternative algorithms when unavailable.
//
// Span discipline (per the two-layer typed architecture, ADR-0078 §5):
// the lower-layer kernel takes raw spans. Future Vector<T> overloads
// (v0b) will delegate to this raw entry point.
// -----------------------------------------------------------------------

template <typename T>
class LinearOp
{
public:
    virtual ~LinearOp() = default;

    // ---- Required overrides ---------------------------------------

    // Compute y = A * x. Caller guarantees x.size() == n_cols() and
    // y.size() == n_rows(). Returns true on success; concrete subclasses
    // may return false on numerical failure (rare).
    [[nodiscard]] virtual bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const = 0;

    [[nodiscard]] virtual crd::usize n_rows() const noexcept = 0;
    [[nodiscard]] virtual crd::usize n_cols() const noexcept = 0;

    // ---- Optional overrides (default returns false) ----------------

    // Compute y = A^T * x. Default = not implemented. Solvers should
    // check has_transpose() before calling.
    [[nodiscard]] virtual bool apply_transpose(
        crd::containers::ConstSpan<T> x,
        crd::containers::Span<T> y) const
    {
        (void)x;
        (void)y;
        return false;
    }

    // Compute y = A^H * x (Hermitian adjoint). For real T, equals
    // apply_transpose. Default = not implemented.
    [[nodiscard]] virtual bool apply_adjoint(
        crd::containers::ConstSpan<T> x,
        crd::containers::Span<T> y) const
    {
        (void)x;
        (void)y;
        return false;
    }

    // ---- Capability queries (non-virtual) -------------------------

    // Concrete subclasses set these via construction; queried by Krylov
    // solvers (GMRES, BiCGSTAB, LSQR) to choose an algorithm path.
    [[nodiscard]] bool has_transpose() const noexcept { return m_has_transpose; }
    [[nodiscard]] bool has_adjoint() const noexcept { return m_has_adjoint; }

    [[nodiscard]] bool is_square() const noexcept { return n_rows() == n_cols(); }

protected:
    // Subclasses set these in their ctor if they override apply_transpose
    // / apply_adjoint. Defaults to false; matches the default impls above.
    LinearOp() = default;
    explicit LinearOp(bool has_transpose, bool has_adjoint) noexcept
        : m_has_transpose(has_transpose), m_has_adjoint(has_adjoint)
    {
    }

    LinearOp(const LinearOp&) = default;
    LinearOp(LinearOp&&) noexcept = default;
    LinearOp& operator=(const LinearOp&) = default;
    LinearOp& operator=(LinearOp&&) noexcept = default;

    bool m_has_transpose = false;
    bool m_has_adjoint = false;
};

} // namespace crd::hesap
