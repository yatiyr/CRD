#pragma once

// lbfgs.hpp — Phase 3.1.6 v7-d: L-BFGS (limited-memory BFGS), the unconstrained workhorse. The search direction
// p = −H_k·g_k is formed by the Nocedal two-loop recursion over the last m curvature pairs (s_i, y_i) WITHOUT ever
// storing H — O(mn) per iteration, the reason L-BFGS scales to large n. Line search = More-Thuente by default (the
// liblbfgs/Ceres choice). ADR-0090; Nocedal & Wright Alg 7.4/7.5; Liu & Nocedal 1989.
//
// DETERMINISM MOAT: the optimizer is SERIAL scalar arithmetic (the two-loop dots, γ scaling, vector updates are a
// fixed-order serial recurrence); only the objective eval may be parallel-but-bit-exact ⇒ the L-BFGS trajectory is
// bit-identical across worker counts. Do NOT parallel_for the two-loop dots — that would reintroduce an
// order-dependence and break the moat.
//
// EVAL-COUNT (the L-BFGS verdict metric — wall-clock is ~iters×evals): OptResult::fn_evals / grad_evals are
// accumulated from the line search; the initial-step rule (α₀ = 1/‖g‖₂ on iter 0 where H₀=I would make α=1
// overshoot; α₀ = 1 thereafter, quasi-Newton steps self-scale) is most of matching liblbfgs's eval count.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/more_thuente_line_search.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

// Minimize `obj` from `x0` by L-BFGS with `memory` curvature pairs (default 8) and the supplied line search
// (default More-Thuente). `obj.has_gradient()` is required (use FiniteDiffObjective/FunctorObjective otherwise).
template <typename T>
[[nodiscard]] OptResult<T> minimize_lbfgs(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                          const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                          const LineSearch<T>* line_search = nullptr, crd::usize memory = 8)
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_lbfgs needs an analytic gradient (wrap value-only objectives in "
                                       "FiniteDiffObjective / FunctorObjective)");
    const crd::usize n = obj.n();
    const crd::usize m = memory > 0 ? memory : 1;

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
    crd::containers::Array<T> q(alloc);
    g.resize(n);
    g_new.resize(n);
    p.resize(n);
    x_new.resize(n);
    q.resize(n);

    // Ring buffers for the last m pairs. Slot of insertion i is i % m; active insertions are [total-count, total-1].
    crd::containers::Array<T> s_store(alloc);
    crd::containers::Array<T> y_store(alloc);
    crd::containers::Array<T> rho(alloc);
    crd::containers::Array<T> alpha_coef(alloc);
    s_store.resize(m * n);
    y_store.resize(m * n);
    rho.resize(m);
    alpha_coef.resize(m);

    const MoreThuenteLineSearch<T> default_ls;
    const LineSearch<T>&           ls = line_search != nullptr ? *line_search : default_ls;

    T* x = result.x.data();
    T  fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    T grad_norm = inf_norm({g.data(), n});

    crd::usize total = 0; // total pairs ever inserted
    T          gamma = static_cast<T>(1);
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

        // ---- two-loop recursion: p = −H_k·g_k ----
        const crd::usize count = total < m ? total : m;
        for (crd::usize i = 0; i < n; ++i)
        {
            q[i] = g[i];
        }
        for (crd::usize k = 0; k < count; ++k) // newest → oldest
        {
            const crd::usize slot = (total - 1 - k) % m;
            const T*         s_k = &s_store[slot * n];
            const T*         y_k = &y_store[slot * n];
            const T          a = rho[slot] * dn::dot<T>({s_k, n}, {q.data(), n});
            alpha_coef[k] = a;
            for (crd::usize i = 0; i < n; ++i)
            {
                q[i] -= a * y_k[i];
            }
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            q[i] *= gamma; // H₀ = γ·I
        }
        for (crd::usize kk = 0; kk < count; ++kk) // oldest → newest
        {
            const crd::usize k = count - 1 - kk;
            const crd::usize slot = (total - 1 - k) % m;
            const T*         s_k = &s_store[slot * n];
            const T*         y_k = &y_store[slot * n];
            const T          beta = rho[slot] * dn::dot<T>({y_k, n}, {q.data(), n});
            const T          coef = alpha_coef[k] - beta;
            for (crd::usize i = 0; i < n; ++i)
            {
                q[i] += coef * s_k[i];
            }
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            p[i] = -q[i]; // descent direction
        }

        // Initial step: iter 0 (H₀=I) scales by 1/‖g‖₂ to avoid an overshoot; afterwards the QN step self-scales ~1.
        T step0 = static_cast<T>(1);
        if (total == 0)
        {
            const T gnorm2 = std::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
            step0 = gnorm2 > static_cast<T>(0) ? static_cast<T>(1) / gnorm2 : static_cast<T>(1);
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

        // s = x_new − x, y = g_new − g; curvature sᵀy.
        T sy = static_cast<T>(0);
        T yy = static_cast<T>(0);
        T step_norm_sq = static_cast<T>(0);
        const crd::usize slot = total % m; // overwrite the oldest when full
        T*               s_slot = &s_store[slot * n];
        T*               y_slot = &y_store[slot * n];
        for (crd::usize i = 0; i < n; ++i)
        {
            const T si = x_new[i] - x[i];
            const T yi = g_new[i] - g[i];
            s_slot[i] = si;
            y_slot[i] = yi;
            sy += si * yi;
            yy += yi * yi;
            step_norm_sq += si * si;
        }

        const T df = std::fabs(r.fx_new - fx);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        fx = r.fx_new;
        grad_norm = inf_norm({g.data(), n});

        // Store the pair only if the curvature condition holds (else skip — keeps H positive definite). Strong
        // Wolfe guarantees sy>0; the Armijo-only fallback may not, hence the guard.
        if (sy > std::numeric_limits<T>::epsilon() * yy)
        {
            rho[slot] = static_cast<T>(1) / sy;
            gamma = sy / yy;
            ++total;
        }

        const T x_norm = inf_norm({x, n});
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
