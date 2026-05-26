#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/linear_op.hpp>

#include <utility> // std::move

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// KrylovPreconditioner<T, InnerApply> -- a nested (inner) Krylov solver wrapped as
// a LinearOp preconditioner. Phase 3.1.6 v4f-1.
//
// `apply(r, z)` runs the caller-supplied inner solve to approximate z ≈ A⁻¹·r
// (the action of an APPROXIMATE inverse), starting from a zero guess. Plugging
// this into FGMRES's variable-preconditioner hook (v4b) gives inner-outer Krylov
// compositions a flat solver cannot express: e.g. FGMRES-outer with a
// few-iteration GMRES/CG inner, or any cheap approximate solve as a smoother.
// FGMRES is FLEXIBLE, so the inner solve need not be exactly linear/constant
// across outer iterations -- a fixed inner-iteration budget is fine.
//
// `InnerApply` is a callable `void(ConstSpan<T> r, Span<T> z)` (templated ⇒
// zero-overhead, no std::function / heap). It captures the inner operator, a
// PERSISTENT inner workspace, the inner IterativeOptions, and the allocator, and
// writes z ≈ A⁻¹r. Use `make_krylov_preconditioner` for type deduction.
//
// DETERMINISM: the wrapper adds nothing non-deterministic -- if the inner solve is
// deterministic (all hesap Krylov solvers are, over a bit-exact spmv), the
// preconditioner action is bit-deterministic and the outer solve stays
// thread-count-independent (the v4 moat).
//
// ARENA CONTRACT: if the inner and outer solvers use ParallelSparseLinearOp
// instances, each owns its own frame-arena reset in `apply`; there is no shared
// arena state between inner and outer (safe whether they wrap the same A or not).
// -----------------------------------------------------------------------

template <typename T, typename InnerApply>
class KrylovPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    KrylovPreconditioner(crd::usize n, InnerApply inner) noexcept
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/false), m_n(n), m_inner(std::move(inner))
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        CRD_ASSERT_MSG(r.size() == m_n && z.size() == m_n, "KrylovPreconditioner: size mismatch");
        for (crd::usize i = 0; i < m_n; ++i)
        {
            z[i] = T{}; // zero initial guess for the inner solve
        }
        m_inner(r, z); // z ≈ A⁻¹ r
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    crd::usize         m_n;
    mutable InnerApply m_inner; // mutates its captured inner workspace per apply
};

// Deduction helper: `auto P = make_krylov_preconditioner<T>(n, inner_lambda);`
template <typename T, typename InnerApply>
[[nodiscard]] KrylovPreconditioner<T, InnerApply> make_krylov_preconditioner(crd::usize n, InnerApply inner)
{
    return KrylovPreconditioner<T, InnerApply>(n, std::move(inner));
}

} // namespace crd::hesap::preconditioners
