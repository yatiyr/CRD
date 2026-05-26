#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// LSMR -- least-squares MINRES (Fong-Saunders 2011). Phase 3.1.6 v4d-1.
//
// Like LSQR, solves min ‖A x − b‖₂ for RECTANGULAR A via the Golub-Kahan
// bidiagonalization (apply + apply_adjoint), but it is analytically MINRES on
// the normal equations: ‖Aᴴr‖ decreases MONOTONICALLY (LSQR's does not), so it is
// the more robust stopping behaviour and the preferred least-squares solver when
// early termination matters. Stops on ArNorm = |ζbar| (= ‖Aᴴr‖, the quantity LSMR
// minimizes). Eigen ships no LSMR -- breadth + determinism moat. Recurrence from
// Krylov.jl `lsmr.jl` (undamped, λ=0).
// -----------------------------------------------------------------------

template <typename T>
struct LsmrWorkspace
{
    crd::hesap::dense::Vector<T> u, av;              // size m (rows)
    crd::hesap::dense::Vector<T> v, h, hbar, atu, nv; // size n (cols); nv = residual-space v under N-precond

    LsmrWorkspace(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n)
        : u(alloc, m), av(alloc, m), v(alloc, n), h(alloc, n), hbar(alloc, n), atu(alloc, n), nv(alloc, n)
    {
    }

    [[nodiscard]] crd::usize rows() const noexcept { return u.size(); }
    [[nodiscard]] crd::usize cols() const noexcept { return v.size(); }
};

// Optional N preconditioner (SPD/HPD, n-space) -- same M-inner-product
// bidiagonalization as preconditioned LSQR; nullptr ⇒ bit-identical to plain LSMR.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> lsmr(const crd::hesap::LinearOp<T>&  a,
                                                     const crd::hesap::LinearOp<T>*  n_inv,
                                                     crd::containers::ConstSpan<T>   b,
                                                     crd::containers::Span<T>        x,
                                                     const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                     LsmrWorkspace<T>&               ws,
                                                     crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   m      = a.n_rows();
    [[maybe_unused]] const crd::usize n = a.n_cols(); // used only in asserts (compiled out in release)
    const bool         prec   = (n_inv != nullptr);
    CRD_ASSERT_MSG(b.size() == m && x.size() == n, "lsmr: b must be rows(), x must be cols()");
    CRD_ASSERT_MSG(ws.rows() == m && ws.cols() == n, "lsmr: workspace size mismatch");
    CRD_ASSERT_MSG(a.has_adjoint(), "lsmr: operator must provide apply_adjoint (Aᴴ)");

    const auto u    = ws.u.span();
    const auto av   = ws.av.span();
    const auto v    = ws.v.span();
    const auto h    = ws.h.span();
    const auto hbar = ws.hbar.span();
    const auto atu  = ws.atu.span();
    const auto nv   = prec ? ws.nv.span() : v;

    auto vnorm = [&]() -> R {
        if (prec)
        {
            return std::sqrt(detail::krylov_real<T>(detail::krylov_inner<T>(v, nv)));
        }
        return nrm2<T>(v);
    };

    // ---- Golub-Kahan setup ----
    (void)a.apply(x, av);
    for (crd::usize i = 0; i < m; ++i)
    {
        u[i] = b[i] - av[i];
    }
    const R beta1 = nrm2<T>(u);
    if (beta1 < smlnum)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = R(0);
        return result;
    }
    dense::scal<T>(T(R(1) / beta1), u);
    (void)a.apply_adjoint(u, nv); // nv = Aᴴ u
    if (prec)
    {
        (void)n_inv->apply(nv, v);
    }
    R alpha = vnorm();
    if (alpha < smlnum)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = beta1;
        return result;
    }
    dense::scal<T>(T(R(1) / alpha), v);
    if (prec)
    {
        dense::scal<T>(T(R(1) / alpha), nv);
    }
    dense::copy<T>(v, h);
    for (crd::usize i = 0; i < n; ++i)
    {
        hbar[i] = T{};
    }

    R zetabar  = alpha * beta1;
    R alphabar = alpha;
    R rho = R(1), rhobar = R(1), cbar = R(1), sbar = R(0);
    const R arnorm0 = alpha * beta1; // ‖Aᴴr₀‖
    R       res     = arnorm0;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(arnorm0);
    }

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        // Bidiagonalization: u = (A v - alpha u)/beta ; v = (Aᴴ u - beta v)/alpha.
        (void)a.apply(v, av);
        dense::axpy<T>(-T(alpha), u, av);
        R beta = nrm2<T>(av);
        if (beta >= smlnum)
        {
            dense::copy<T>(av, u);
            dense::scal<T>(T(R(1) / beta), u);
        }
        (void)a.apply_adjoint(u, atu);
        dense::axpy<T>(-T(beta), nv, atu); // atu = Aᴴ u - beta nv (residual-space)
        dense::copy<T>(atu, nv);           // nv = new residual-space v (unnormalized)
        if (prec)
        {
            (void)n_inv->apply(nv, v); // v = N⁻¹ nv
        }
        alpha = vnorm();
        if (alpha >= smlnum)
        {
            dense::scal<T>(T(R(1) / alpha), v);
            if (prec)
            {
                dense::scal<T>(T(R(1) / alpha), nv);
            }
        }

        // Rotation P (eliminates beta).  alphahat = alphabar (no damping).
        const R rhoold = rho;
        rho            = std::sqrt(alphabar * alphabar + beta * beta);
        if (rho < smlnum)
        {
            rho = smlnum;
        }
        const R c        = alphabar / rho;
        const R s        = beta / rho;
        const R thetanew = s * alpha;
        alphabar         = c * alpha;

        // Rotation Pbar (the LSMR second rotation on the cbar*rho / thetanew column).
        const R rhobarold = rhobar;
        const R thetabar  = sbar * rho;
        const R cbarrho   = cbar * rho;
        rhobar            = std::sqrt(cbarrho * cbarrho + thetanew * thetanew);
        if (rhobar < smlnum)
        {
            rhobar = smlnum;
        }
        cbar             = cbarrho / rhobar;
        sbar             = thetanew / rhobar;
        const R zeta     = cbar * zetabar;
        zetabar          = -sbar * zetabar;

        // hbar, x, h updates.
        const R denom = rhoold * rhobarold;
        dense::scal<T>(T(-(thetabar * rho) / denom), hbar);
        dense::axpy<T>(T(1), h, hbar);                 // hbar = h - (thetabar*rho/denom) hbar
        dense::axpy<T>(T(zeta / (rho * rhobar)), hbar, x); // x += (zeta/(rho*rhobar)) hbar
        dense::scal<T>(T(-thetanew / rho), h);
        dense::axpy<T>(T(1), v, h);                    // h = v - (thetanew/rho) h

        const R arnorm    = zetabar < R(0) ? -zetabar : zetabar; // ‖Aᴴr‖
        res               = arnorm;
        result.iterations = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(arnorm);
        }
        if (arnorm <= opts.rel_tol * arnorm0 || arnorm < opts.abs_tol)
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = arnorm;
            return result;
        }
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

// Plain (unpreconditioned) LSMR convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> lsmr(const crd::hesap::LinearOp<T>&                          a,
                                                     crd::containers::ConstSpan<T>                          b,
                                                     crd::containers::Span<T>                               x,
                                                     const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                     LsmrWorkspace<T>&                                      ws,
                                                     crd::memory::IAllocator* result_alloc)
{
    return lsmr<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
