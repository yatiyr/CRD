#pragma once

// newton.hpp — Phase 3.1.6 v7-g: dense FULL + MODIFIED Newton with a line search. Direction from solving
// (∇²f + τ·I)·p = −∇f by dense Cholesky; τ by Nocedal & Wright Algorithm 3.4 ("Cholesky with added multiple of
// the identity"): τ = 0 when the Hessian is positive definite (PURE Newton — quadratic local convergence), else
// the smallest power-of-two escalation that makes the factor succeed (MODIFIED Newton — a guaranteed descent
// direction through indefinite/negative-curvature regions, the saddle-escape property pure Newton lacks).
// Line search = strong Wolfe with α₀ = 1 (Newton steps self-scale; α = 1 is accepted near the solution, which
// is what preserves the quadratic rate). For large-scale problems where forming/factoring the dense Hessian is
// infeasible use minimize_newton_cg (Hessian-vector only) or minimize_newton_sparse (CSR + hesap-direct).
// ADR-0090; Nocedal & Wright §3.4 + Alg 3.4.
//
// DETERMINISM MOAT: serial scalar loops + the serial dense Cholesky; only the objective eval may be
// parallel-but-bit-exact ⇒ the trajectory is bit-identical across worker counts.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/levenberg_marquardt.hpp> // detail::chol_solve (in-place dense SPD factor + solve)
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/wolfe_line_search.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

// Minimize `obj` from `x0` by (modified) Newton. Requires has_gradient() + has_hessian(). `OptResult::hess_evals`
// counts hessian() evaluations (one per iteration).
template <typename T>
[[nodiscard]] OptResult<T> minimize_newton(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                           const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                           const LineSearch<T>* line_search = nullptr)
{
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_newton needs an analytic gradient");
    CRD_ASSERT_MSG(obj.has_hessian(), "minimize_newton needs a dense Hessian (use minimize_newton_cg for "
                                      "Hessian-vector-only objectives)");
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

    auto inf_nrm = [](crd::containers::ConstSpan<T> w) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < w.size(); ++i)
        {
            const T a = crd::math::fabs(w[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> g_new(alloc);
    crd::containers::Array<T> h(alloc);   // ∇²f, n×n row-major (refilled per iteration)
    crd::containers::Array<T> mtx(alloc); // factor scratch (H + τ·I, destroyed by the Cholesky)
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> x_new(alloc);
    g.resize(n);
    g_new.resize(n);
    h.resize(n * n);
    mtx.resize(n * n);
    p.resize(n);
    x_new.resize(n);

    const WolfeLineSearch<T> default_ls; // strong Wolfe c1=1e-4, c2=0.9 — the Newton/quasi-Newton default
    const LineSearch<T>& ls = line_search != nullptr ? *line_search : default_ls;

    T* x = result.x.data();
    T fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    T grad_norm = inf_nrm({g.data(), n});

    OptStatus status = OptStatus::MaxIterations;
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

        (void)obj.hessian({x, n}, {h.data(), n * n});
        ++result.hess_evals;

        // N&W Algorithm 3.4: try τ = 0 first (pure Newton when ∇²f ≻ 0); on a failed factor escalate
        // τ ← max(2τ, β·scale) until (H + τ·I) is positive definite. β·scale anchors the first nonzero τ to the
        // Hessian's magnitude (N&W use an absolute β; scaling by max|h_ii| keeps the rule scale-invariant).
        T mindiag = h[0];
        T maxabsdiag = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T d = h[i * n + i];
            mindiag = d < mindiag ? d : mindiag;
            const T a = crd::math::fabs(d);
            maxabsdiag = a > maxabsdiag ? a : maxabsdiag;
        }
        const T beta = static_cast<T>(1e-3) * (maxabsdiag > static_cast<T>(0) ? maxabsdiag : static_cast<T>(1));
        T tau = mindiag > static_cast<T>(0) ? static_cast<T>(0) : (-mindiag + beta);

        bool solved = false;
        for (int attempt = 0; attempt < 60 && !solved; ++attempt)
        {
            for (crd::usize k = 0; k < n * n; ++k)
            {
                mtx[k] = h[k];
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                mtx[i * n + i] += tau;
                p[i] = -g[i];
            }
            solved = detail::chol_solve<T>(mtx.data(), n, p.data());
            if (!solved)
            {
                const T doubled = static_cast<T>(2) * tau;
                tau = doubled > beta ? doubled : beta; // N&W 3.4: τ ← max(2τ, β) — max, NOT a stuck-at-β ladder
            }
        }
        if (!solved)
        {
            status = OptStatus::LineSearchFailed; // could not form a descent direction
            break;
        }

        // (H + τ·I) ≻ 0 ⇒ p is a descent direction; α₀ = 1 (the Newton step).
        const auto r = ls.search(obj, {x, n}, fx, {g.data(), n}, {p.data(), n}, static_cast<T>(1), {x_new.data(), n},
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

        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T d = x_new[i] - x[i];
            step_norm_sq += d * d;
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        const T df = crd::math::fabs(r.fx_new - fx);
        fx = r.fx_new;
        grad_norm = inf_nrm({g.data(), n});

        const auto stop = check_convergence<T>(grad_norm, crd::math::sqrt(step_norm_sq), df, inf_nrm({x, n}), fx, opts);
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
