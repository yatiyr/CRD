#pragma once

// conjugate_gradient.hpp — Phase 3.1.6 v7-f: nonlinear conjugate gradient, the low-memory first-order method for
// large-scale smooth unconstrained problems (O(n) storage — no curvature pairs, unlike L-BFGS). The direction is
// p_k = −g_k + β_k·p_{k-1}, with β_k by Fletcher-Reeves / Polak-Ribière⁺ / Hestenes-Stiefel / Dai-Yuan. Practical
// hardening: Powell restart + n-step restart + a descent-direction safeguard. Line search = STRONG Wolfe with a
// TIGHT c2 (0.1) — nonlinear CG needs the strong curvature condition for a guaranteed descent direction (Al-Baali;
// FR in particular requires c2 < ½). ADR-0090; Nocedal & Wright Ch.5; Hager-Zhang 2006 survey.
//
// DETERMINISM MOAT: the optimizer is SERIAL scalar arithmetic (the β dots and vector updates are a fixed-order
// recurrence); only the objective eval may be parallel-but-bit-exact ⇒ the CG trajectory is bit-identical across
// worker counts. Do NOT parallel_for the dots — that reintroduces order-dependence and breaks the moat.
//
// EVAL-COUNT (the v7-z scipy.optimize 'CG' eval-parity metric — plain nonlinear CG is the same algorithm, so
// eval-parity is the honest ceiling + the {1..16} moat is the differentiator): OptResult::fn_evals/grad_evals are
// accumulated from the line search, mirroring minimize_lbfgs.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/wolfe_line_search.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

// The β_k update formula. PolakRibierePlus is the practical default (PR with the max(0,·) reset that gives an
// automatic restart on a bad step + the descent guarantee under strong Wolfe).
enum class CgVariant : crd::u8
{
    FletcherReeves,    // β = (gₖ·gₖ) / (gₖ₋₁·gₖ₋₁)
    PolakRibierePlus,  // β = max(0, gₖ·(gₖ−gₖ₋₁) / (gₖ₋₁·gₖ₋₁))
    HestenesStiefel,   // β = max(0, gₖ·(gₖ−gₖ₋₁) / (pₖ₋₁·(gₖ−gₖ₋₁)))
    DaiYuan            // β = (gₖ·gₖ) / (pₖ₋₁·(gₖ−gₖ₋₁))
};

// Minimize `obj` from `x0` by nonlinear CG with the given β variant and line search (default strong Wolfe, c2=0.1).
// `obj.has_gradient()` is required (wrap value-only objectives in FiniteDiffObjective / FunctorObjective).
template <typename T>
[[nodiscard]] OptResult<T> minimize_nonlinear_cg(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                 const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                 const LineSearch<T>* line_search = nullptr,
                                                 CgVariant variant = CgVariant::PolakRibierePlus)
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_nonlinear_cg needs an analytic gradient (wrap value-only objectives "
                                       "in FiniteDiffObjective / FunctorObjective)");
    const crd::usize n = obj.n();

    OptResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    auto inf_norm = [](crd::containers::ConstSpan<T> v) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < v.size(); ++i)
        {
            const T a = std::fabs(v[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> g_new(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> x_new(alloc);
    g.resize(n);
    g_new.resize(n);
    p.resize(n);
    x_new.resize(n);

    // Default: strong Wolfe with a TIGHT c2 (0.1) — nonlinear CG's descent guarantee needs it (a loose c2 like
    // L-BFGS's 0.9 can yield a non-descent CG direction). c1 = 1e-4, strong = true.
    const WolfeLineSearch<T> default_ls(static_cast<T>(1e-4), static_cast<T>(0.1), /*strong=*/true, 50);
    const LineSearch<T>&     ls = line_search != nullptr ? *line_search : default_ls;

    T* x = result.x.data();
    T  fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    T grad_norm = inf_norm({g.data(), n});
    T gg = dn::dot<T>({g.data(), n}, {g.data(), n}); // gₖ·gₖ (FR/DY numerator, PR/FR denominator, Powell rhs)

    for (crd::usize i = 0; i < n; ++i)
    {
        p[i] = -g[i]; // p₀ = −g₀ (steepest descent)
    }

    T          prev_alpha = static_cast<T>(1);
    T          prev_dphi0 = static_cast<T>(0);
    bool       restarted = true; // iter 0 is a "restart" (p = −g) ⇒ uses the restart initial-step rule
    OptStatus  status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        if (grad_norm <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }

        // Descent-direction safeguard: if p is not a descent direction (gₖ·pₖ ≥ 0 — a bad β can cause this),
        // reset to steepest descent. Guarantees the line search sees φ'(0) < 0.
        T dphi0 = dn::dot<T>({g.data(), n}, {p.data(), n});
        if (!(dphi0 < static_cast<T>(0)))
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                p[i] = -g[i];
            }
            dphi0 = -gg;
            restarted = true;
        }

        // Initial step (Nocedal & Wright §3.5): on a restart use 1/‖g‖₂ (steepest-like, avoids overshoot); otherwise
        // the CG guess α₀ = α_{k-1}·(φ'_{k-1}(0)/φ'_k(0)) — CG steps do NOT self-scale to 1 like quasi-Newton.
        T step0;
        if (restarted)
        {
            const T gnorm2 = std::sqrt(gg);
            step0 = gnorm2 > static_cast<T>(0) ? static_cast<T>(1) / gnorm2 : static_cast<T>(1);
        }
        else
        {
            step0 = prev_alpha * prev_dphi0 / dphi0; // both dphi0 < 0 ⇒ positive
            if (!(step0 > static_cast<T>(0)) || !std::isfinite(step0))
            {
                step0 = static_cast<T>(1);
            }
        }

        const auto r = ls.search(obj, {x, n}, fx, {g.data(), n}, {p.data(), n}, step0, {x_new.data(), n},
                                 {g_new.data(), n});
        result.fn_evals += r.evals;
        result.grad_evals += r.grad_evals;
        if (!r.ok)
        {
            status = OptStatus::LineSearchFailed;
            break;
        }
        if (!r.grad_at_new_valid)
        {
            (void)obj.gradient({x_new.data(), n}, {g_new.data(), n});
            ++result.grad_evals;
        }

        // Curvature quantities for β (computed from g = gₖ, g_new = gₖ₊₁, p = pₖ).
        const T gg_new = dn::dot<T>({g_new.data(), n}, {g_new.data(), n}); // gₖ₊₁·gₖ₊₁
        T       gy = static_cast<T>(0);                                    // gₖ₊₁·(gₖ₊₁−gₖ)
        T       py = static_cast<T>(0);                                    // pₖ·(gₖ₊₁−gₖ)
        T       g_new_dot_g = static_cast<T>(0);                           // gₖ₊₁·gₖ  (Powell restart test)
        T       step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T yi = g_new[i] - g[i];
            gy += g_new[i] * yi;
            py += p[i] * yi;
            g_new_dot_g += g_new[i] * g[i];
            const T si = x_new[i] - x[i];
            step_norm_sq += si * si;
        }

        // β by variant (denominators guarded; HS/PR clamped to ≥ 0 — a negative β can destroy the descent property).
        const T tiny = std::numeric_limits<T>::min();
        T       beta;
        switch (variant)
        {
        case CgVariant::FletcherReeves:
            beta = gg > tiny ? gg_new / gg : static_cast<T>(0);
            break;
        case CgVariant::PolakRibierePlus:
            beta = gg > tiny ? gy / gg : static_cast<T>(0);
            beta = beta > static_cast<T>(0) ? beta : static_cast<T>(0);
            break;
        case CgVariant::HestenesStiefel:
            beta = std::fabs(py) > tiny ? gy / py : static_cast<T>(0);
            beta = beta > static_cast<T>(0) ? beta : static_cast<T>(0);
            break;
        case CgVariant::DaiYuan:
        default:
            beta = std::fabs(py) > tiny ? gg_new / py : static_cast<T>(0);
            break;
        }

        // Restarts → β = 0 (pure steepest descent next step): Powell's test (loss of conjugacy: gₖ₊₁ not ~orthogonal
        // to gₖ) + the classic n-step periodic restart.
        const bool powell = std::fabs(g_new_dot_g) >= static_cast<T>(0.2) * gg_new;
        const bool n_step = ((it + 1) % n) == 0;
        if (powell || n_step)
        {
            beta = static_cast<T>(0);
        }

        // Accept the step and form the next direction p ← −gₖ₊₁ + β·pₖ.
        const T df = std::fabs(r.fx_new - fx);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
            p[i] = -g_new[i] + beta * p[i];
            g[i] = g_new[i];
        }
        fx = r.fx_new;
        gg = gg_new;
        grad_norm = inf_norm({g.data(), n});
        restarted = (beta == static_cast<T>(0));
        prev_alpha = r.alpha;
        prev_dphi0 = dphi0;

        const T    x_norm = inf_norm({x, n});
        const auto stop = check_convergence<T>(grad_norm, std::sqrt(step_norm_sq), df, x_norm, fx, opts);
        if (stop.has_value())
        {
            status = *stop;
            break;
        }
    }

    result.fx = fx;
    result.grad_norm = grad_norm;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
