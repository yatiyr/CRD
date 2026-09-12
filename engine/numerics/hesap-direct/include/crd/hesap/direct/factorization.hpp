#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::direct
{
// -----------------------------------------------------------------------
// IFactorization<T> — the common interface for every sparse DIRECT
// factorization (supernodal Cholesky, sparse LU, multifrontal QR, LDLᵀ).
//
// Phase 3.1.6 v5a-1. Factor-once / solve-many: `solve` is cheap and
// re-callable, and is MULTI-RHS from day 1 (the FEM time-stepping /
// eylem articulation / optimisation inner-solve access pattern — never
// bolted on later). Concrete factorizations own their factor storage and
// are produced by the per-family `factor_*` entry points.
//
// Vtable shape (LOCKED v5a-1 — new virtuals append AT END forever per
// feedback_vtable_stability_append_at_end):
//   Slot 0: dtor
//   Slot 1: solve   (pure)
//   Slot 2: n       (pure)
//   Slot 3: factor_nnz (pure)
//   Slot 4: info    (pure)
//   Slot 5: apply_inverse (v5f; non-pure, default = solve)
//   [future slots appended at END]
// -----------------------------------------------------------------------
template <typename T>
class IFactorization
{
public:
    virtual ~IFactorization() = default;

    // Solve A·X = B in place. `rhs` is a column-major n × nrhs block: it holds
    // B on entry and receives the solution X on exit. Returns false on numerical
    // failure (e.g. a non-positive pivot for Cholesky). Caller guarantees
    // rhs.size() == n() * nrhs.
    [[nodiscard]] virtual bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const = 0;

    [[nodiscard]] virtual crd::usize n() const noexcept = 0;          // matrix dimension
    [[nodiscard]] virtual crd::u64 factor_nnz() const noexcept = 0;   // nonzeros in the computed factor (fill)
    [[nodiscard]] virtual crd::usize info() const noexcept = 0;       // 0 = success; else failure code

    // Apply the factorization's inverse ONCE: `rhs` holds B in (column-major n × nrhs) and receives a RAW
    // solution X out, with NO internal iterative refinement. This is the building block the mixed-precision
    // IR driver composes (v5f `IterativeRefinedSolve`): the driver owns ALL refinement at the working
    // precision, so a low-precision factor must expose its UN-refined triangular / forward-backward core
    // here. Default = `solve` (correct for families whose solve() is already a raw apply — e.g. Cholesky);
    // families whose solve() runs internal IR (the static-pivot LU GESP refinement, the LDLᵀ residual loop)
    // MUST override, else the inner low-precision IR's stagnation / accept-gate spuriously fails the OUTER
    // working-precision IR on exactly the ill-conditioned systems mixed-precision targets. Appended at the
    // END of the vtable (v5f) per feedback_vtable_stability_append_at_end.
    virtual void apply_inverse(crd::containers::Span<T> rhs, crd::usize nrhs) const { (void)solve(rhs, nrhs); }

    // Convenience single-RHS in-place solve.
    [[nodiscard]] bool solve(crd::containers::Span<T> x) const { return solve(x, 1); }

protected:
    IFactorization() = default;
    IFactorization(const IFactorization&) = default;
    IFactorization(IFactorization&&) noexcept = default;
    IFactorization& operator=(const IFactorization&) = default;
    IFactorization& operator=(IFactorization&&) noexcept = default;
};

} // namespace crd::hesap::direct
