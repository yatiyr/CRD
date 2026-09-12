#pragma once

// objective.hpp — Phase 3.1.6 v7-a: the Objective<T> interface — the scalar function f(x) an optimizer minimizes,
// matrix-free (the optimizer sees only value / gradient / Hessian-vector). Raw lower-layer (f32/f64), like
// crd::hesap::LinearOp — the optimization kernel is below the typed-units boundary (ADR-0078 §5). ADR-0090.
//
// CAPABILITY CONTRACT (advisor-pinned, so the 18 v7 subslices share one convention):
//   • `has_gradient()` / `has_hessian_vector()` are the QUERIED capabilities (set once in the protected ctor,
//     LinearOp-style). The optimizer checks these to decide analytic vs finite-difference (v7-b).
//   • `gradient(x, g)` returns TRUE iff it actually filled `g`. A subclass that sets `has_gradient=true` MUST
//     return true; one that leaves it false signals "no analytic gradient — the optimizer should finite-difference".
//   • The two MUST agree (a debug assert in the optimizer enforces it).
//
// VTABLE (LOCKED — new virtuals append AT END forever, per feedback_vtable_stability_append_at_end):
//   0 dtor · 1 value · 2 n · 3 gradient · 4 hessian_vector · 5 hessian (v7-g) · 6 sparse_hessian (v7-g)
//   [RESERVED slot, appended at END when the hot path needs it: a FUSED `value_and_gradient(x, g) -> T` for
//    objectives that share work between f and ∇f (e.g. A·x reused) — L-BFGS / LM / line searches want it; ADR-0090
//    reserves it so adding it later is non-breaking. NOT built in v7-a.]

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

namespace crd::hesap::opt
{

template <typename T> class Objective
{
public:
    virtual ~Objective() = default;

    // ---- Required overrides ----

    // f(x). Caller guarantees x.size() == n().
    [[nodiscard]] virtual T value(crd::containers::ConstSpan<T> x) const = 0;

    // The problem dimension (length of x).
    [[nodiscard]] virtual crd::usize n() const noexcept = 0;

    // ---- Optional overrides (default = not provided) ----

    // ∇f(x) → g (g.size() == n()). Returns true iff `g` was filled. Default = not provided ⇒ the optimizer
    // finite-differences (v7-b). A subclass that sets has_gradient=true MUST override this and return true.
    [[nodiscard]] virtual bool gradient(crd::containers::ConstSpan<T> x, crd::containers::Span<T> g) const
    {
        (void)x;
        (void)g;
        return false;
    }

    // Hessian-vector product (∇²f(x))·v → hv (for Newton-CG / trust-Krylov, v7-g/h). Returns true iff filled.
    [[nodiscard]] virtual bool hessian_vector(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> v,
                                              crd::containers::Span<T> hv) const
    {
        (void)x;
        (void)v;
        (void)hv;
        return false;
    }

    // Dense Hessian ∇²f(x) → h (n×n, ROW-MAJOR, FULL symmetric — both triangles filled). Returns true iff filled.
    // The v7-g full/modified Newton path factors it (with the N&W 3.4 τ·I modification when indefinite). Vtable
    // slot appended at END.
    [[nodiscard]] virtual bool hessian(crd::containers::ConstSpan<T> x, crd::containers::Span<T> h) const
    {
        (void)x;
        (void)h;
        return false;
    }

    // SPARSE Hessian ∇²f(x) (CSR, n×n, FULL symmetric storage): fills `out` (the caller owns + reuses it). The
    // v7-g sparse Newton path factors it with the moat-proven hesap-direct supernodal Cholesky; for a fixed-pattern
    // objective the sparsity is constant across iterations (values change) — the symbolic-once contract. Returns
    // true iff filled. Vtable slot appended at END.
    [[nodiscard]] virtual bool sparse_hessian(crd::containers::ConstSpan<T> x,
                                              sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& out) const
    {
        (void)x;
        (void)out;
        return false;
    }

    // ---- Capability queries (non-virtual; set in the protected ctor) ----

    [[nodiscard]] bool has_gradient() const noexcept { return m_has_gradient; }
    [[nodiscard]] bool has_hessian_vector() const noexcept { return m_has_hessian_vector; }
    [[nodiscard]] bool has_hessian() const noexcept { return m_has_hessian; }
    [[nodiscard]] bool has_sparse_hessian() const noexcept { return m_has_sparse_hessian; }

protected:
    Objective() = default;
    explicit Objective(bool has_gradient, bool has_hessian_vector, bool has_hessian = false,
                       bool has_sparse_hessian = false) noexcept
        : m_has_gradient(has_gradient), m_has_hessian_vector(has_hessian_vector), m_has_hessian(has_hessian),
          m_has_sparse_hessian(has_sparse_hessian)
    {
    }
    Objective(const Objective&) = default;
    Objective(Objective&&) noexcept = default;
    Objective& operator=(const Objective&) = default;
    Objective& operator=(Objective&&) noexcept = default;

    bool m_has_gradient = false;
    bool m_has_hessian_vector = false;
    bool m_has_hessian = false;
    bool m_has_sparse_hessian = false;
};

} // namespace crd::hesap::opt
