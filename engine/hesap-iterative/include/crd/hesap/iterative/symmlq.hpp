#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_real / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// SYMMLQ -- symmetric LQ method (Paige-Saunders 1975). Phase 3.1.6 v4c-2b.
//
// For SYMMETRIC / HERMITIAN (possibly INDEFINITE) A: the same symmetric Lanczos
// 3-term recurrence as MINRES, but an LQ (not QR) factorization of the
// tridiagonal -- the indefinite-robust analog of CG. NOTE: the SYMMLQ iterate is
// NOT residual-minimizing; ‖r‖ is not monotone. The stopping test uses SYMMLQ's
// internal residual estimate `sqrt(γ²ζ² + ε_old²ζ_old²)`; the FINAL solution is
// what should be verified by ‖A x − b‖ (tests do this), not per-iter monotonicity.
//
// Recurrence transcribed from Krylov.jl `symmlq.jl` (Paige-Saunders); the
// Hermitian contract makes the tridiagonal real (α = krylov_real(vᴴAv) signed,
// β = ‖·‖) so all LQ scalars are real even for complex A -- only vectors are T.
// Eigen ships no SYMMLQ; this is a breadth win + the determinism moat (reductions
// KBN-pairwise, spmv bit-exact across threads ⇒ thread-count-independent solve).
//
// Optional preconditioner M (SPD/HPD ONLY): the M-inner-product Lanczos tracks the
// preconditioned vector vl (= M⁻¹·residual-space) the A-apply + point use, and the
// residual-space mvl (= M·vl) the projections use; β = √Re⟨vl, M·vl⟩. With
// m_inv == nullptr the path is bit-identical to plain SYMMLQ (mvl aliases vl,
// β = nrm2). Same M-inner-product pattern as preconditioned MINRES.
// -----------------------------------------------------------------------

template <typename T>
struct SymmlqWorkspace
{
    // vl*: preconditioned (M⁻¹) Lanczos vectors (A-apply + point use these); mvl*:
    // residual-space (M·vl); w̅: solution dir; p/y: scratch. mvl*/y unused if no precond.
    crd::hesap::dense::Vector<T> vl_old, vl, wbar, p, mvl_old, mvl, y;

    SymmlqWorkspace(crd::memory::IAllocator* alloc, crd::usize n)
        : vl_old(alloc, n), vl(alloc, n), wbar(alloc, n), p(alloc, n), mvl_old(alloc, n), mvl(alloc, n), y(alloc, n)
    {
    }

    [[nodiscard]] crd::usize size() const noexcept { return vl.size(); }
};

// Preconditioned SYMMLQ. `m_inv` (SPD/HPD) applies M⁻¹; nullptr = plain SYMMLQ.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> symmlq(const crd::hesap::LinearOp<T>&  a,
                                                       const crd::hesap::LinearOp<T>*  m_inv,
                                                       crd::containers::ConstSpan<T>   b,
                                                       crd::containers::Span<T>        x,
                                                       const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                       SymmlqWorkspace<T>&             ws,
                                                       crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "symmlq: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "symmlq: span/workspace size mismatch");

    const auto vl_old = ws.vl_old.span();
    const auto vl     = ws.vl.span();
    const auto wbar   = ws.wbar.span();
    const auto p      = ws.p.span();
    // mvl* (residual-space M·vl) alias vl* when unpreconditioned (M = I ⇒ M·vl = vl).
    const auto mvl_old = prec ? ws.mvl_old.span() : vl_old;
    const auto mvl     = prec ? ws.mvl.span() : vl;

    // M⁻¹-norm helper: β = √Re⟨u_precond, u_residual⟩ (= nrm2 when unpreconditioned).
    auto mnorm = [&](crd::containers::ConstSpan<T> u_prec, crd::containers::ConstSpan<T> u_res) -> R {
        if (prec)
        {
            return std::sqrt(detail::krylov_real<T>(detail::krylov_inner<T>(u_prec, u_res)));
        }
        return nrm2<T>(u_res);
    };

    // ---- setup: r0 = b - A x (residual-space, in mvl_old) ; v1 ; first Lanczos -> v2 ----
    (void)a.apply(x, p);
    for (crd::usize i = 0; i < n; ++i)
    {
        mvl_old[i] = b[i] - p[i]; // r0 (residual-space); for !prec mvl_old===vl_old.
    }
    if (prec)
    {
        (void)m_inv->apply(mvl_old, vl_old); // vl_old = M⁻¹ r0
    }
    const R beta1 = mnorm(vl_old, mvl_old);
    R       res   = beta1;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(beta1);
    }
    if (is_converged<R>(res, beta1, opts) || n == 0 || beta1 < smlnum)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }
    dense::scal<T>(T(R(1) / beta1), vl_old); // v1 (preconditioned)
    if (prec)
    {
        dense::scal<T>(T(R(1) / beta1), mvl_old); // M v1
    }
    dense::copy<T>(vl_old, wbar); // w̅ = v1

    (void)a.apply(vl_old, p); // A v1
    R alpha = detail::krylov_real<T>(detail::krylov_inner<T>(vl_old, p)); // alpha_1
    dense::axpy<T>(-T(alpha), mvl_old, p);                               // A v1 - alpha_1 (M v1)  -> M v2 unnormalized
    R beta;
    if (prec)
    {
        dense::copy<T>(p, mvl);          // residual-space M v2 (unnormalized)
        (void)m_inv->apply(p, vl);       // vl = M⁻¹ (M v2)
        beta = mnorm(vl, mvl);           // beta_2
    }
    else
    {
        beta = nrm2<T>(p); // beta_2  (mvl===vl)
    }
    if (beta < smlnum) // 1x1 invariant subspace: SYMMLQ point = CG point.
    {
        const R zeta = beta1 / alpha;
        dense::axpy<T>(T(zeta), vl_old, x);
        result.iterations          = 1;
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = R(0);
        return result;
    }
    if (prec)
    {
        dense::scal<T>(T(R(1) / beta), vl);
        dense::scal<T>(T(R(1) / beta), mvl);
    }
    else
    {
        dense::copy<T>(p, vl);
        dense::scal<T>(T(R(1) / beta), vl); // v2
    }

    R gbar = alpha, dbar = beta;
    R eps_old = R(0), zeta_old = R(0), eta = beta1;

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        R gamma = std::sqrt(gbar * gbar + beta * beta);
        if (gamma < smlnum)
        {
            gamma = smlnum;
        }
        const R c = gbar / gamma;
        const R s = beta / gamma;

        // SYMMLQ point (preconditioned vectors): x += c·ζ·w̅ + s·ζ·vl ; w̅ = s·w̅ - c·vl
        const R eta_old = eta;
        const R zeta     = eta_old / gamma;
        dense::axpy<T>(T(c * zeta), wbar, x);
        dense::axpy<T>(T(s * zeta), vl, x);
        dense::scal<T>(T(s), wbar);
        dense::axpy<T>(-T(c), vl, wbar);

        // Lanczos: p = A vl - oldβ·(M v_{k-1}) - α·(M v_k)  -> M v_{k+1} unnormalized.
        const R oldbeta = beta;
        (void)a.apply(vl, p);
        alpha = detail::krylov_real<T>(detail::krylov_inner<T>(vl, p));
        dense::axpy<T>(-T(oldbeta), mvl_old, p);
        dense::copy<T>(mvl, mvl_old); // M v_{k-1} <- M v_k  (for !prec: vl_old <- vl)
        dense::axpy<T>(-T(alpha), mvl, p);
        if (prec)
        {
            dense::copy<T>(p, mvl);    // residual-space M v_{k+1}
            (void)m_inv->apply(p, vl); // vl = M⁻¹ (M v_{k+1})
            beta = mnorm(vl, mvl);
        }
        else
        {
            beta = nrm2<T>(p); // mvl===vl below
        }
        const bool lucky = beta < smlnum;
        if (!lucky)
        {
            if (prec)
            {
                dense::scal<T>(T(R(1) / beta), vl);
                dense::scal<T>(T(R(1) / beta), mvl);
            }
            else
            {
                dense::copy<T>(p, vl);
                dense::scal<T>(T(R(1) / beta), vl);
            }
        }

        // Continue LQ factorization.
        const R delta = dbar * c + alpha * s;
        gbar          = dbar * s - alpha * c;
        const R eps   = beta * s;
        dbar          = -beta * c;
        eta           = -eps_old * zeta_old - delta * zeta;

        res               = std::sqrt(gamma * gamma * zeta * zeta + eps_old * eps_old * zeta_old * zeta_old);
        result.iterations = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        eps_old  = eps;
        zeta_old = zeta;

        if (is_converged<R>(res, beta1, opts) || lucky)
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

// Plain (unpreconditioned) SYMMLQ convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> symmlq(const crd::hesap::LinearOp<T>&                          a,
                                                       crd::containers::ConstSpan<T>                          b,
                                                       crd::containers::Span<T>                               x,
                                                       const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                       SymmlqWorkspace<T>&                                    ws,
                                                       crd::memory::IAllocator* result_alloc)
{
    return symmlq<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
