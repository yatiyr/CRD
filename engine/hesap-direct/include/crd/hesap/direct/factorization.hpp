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
