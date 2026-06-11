#pragma once

// momentum.hpp — Phase 3.1.6 v7-f: fixed-step momentum methods — Polyak heavy-ball (1964) and Nesterov accelerated
// gradient (1983, FISTA form). These are the FULL-gradient deterministic forms (the stochastic SGD+momentum family
// is v7-i); they complete the v7-f first-order trio (steepest descent v7-a · nonlinear CG · momentum/Nesterov).
// No line search — the user supplies the step α (classically 1/L for L-smooth f) and optionally a fixed momentum
// μ; by default Nesterov uses the parameter-free FISTA t-sequence (Beck-Teboulle 2009) with the O'Donoghue-Candès
// (2015) adaptive GRADIENT restart (∇f(y)·(x⁺−x) > 0 ⇒ momentum points uphill ⇒ reset), which restores the
// linear rate on strongly-convex problems without knowing κ. ADR-0090; Nocedal & Wright; Recht-Wright Ch.4.
//
// DETERMINISM MOAT: the optimizer is SERIAL scalar arithmetic (fixed-order vector recurrences + scalar dots);
// only the objective eval may be parallel-but-bit-exact ⇒ the trajectory is bit-identical across worker counts.
//
// CONVERGENCE-TEST SEMANTICS (Nesterov): the only gradient the method computes is ∇f(y_k) at the LOOKAHEAD point,
// so the optimality test uses it (standard practice — y_k → x* alongside x_k). When ‖∇f(y_k)‖∞ ≤ grad_tol the
// returned iterate is y_k itself (gradient and iterate agree exactly); the bottom stall/flat tests use the same
// proxy norm, so a Success there reports a grad_norm within (1+αL)·grad_tol of the true iterate gradient.
// Heavy-ball evaluates ∇f at the iterate — no proxy.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

enum class MomentumVariant : crd::u8
{
    HeavyBall, // x⁺ = x − α·∇f(x) + μ·(x − x⁻)  (gradient at the iterate)
    Nesterov   // y = x + μ·(x − x⁻); x⁺ = y − α·∇f(y)  (gradient at the lookahead)
};

// Step/momentum knobs. `step` is REQUIRED (> 0). `momentum < 0` ⇒ AUTO: Nesterov uses the FISTA t-sequence
// (μₖ = (tₖ−1)/tₖ₊₁, tₖ₊₁ = (1+√(1+4tₖ²))/2 — the optimal O(1/k²) convex schedule, parameter-free); HeavyBall
// uses 0.9. For a strongly-convex quadratic the optimal fixed pair is α = 4/(√λmax+√λmin)², μ = ((√κ−1)/(√κ+1))².
template <typename T> struct MomentumOptions
{
    T step = static_cast<T>(0);      // α — required > 0 (1/L is the classical L-smooth choice)
    T momentum = static_cast<T>(-1); // fixed μ ∈ [0,1) if ≥ 0; < 0 ⇒ AUTO (see above)
    bool adaptive_restart = true;    // O'Donoghue-Candès gradient restart (Nesterov only)
};

namespace detail
{

template <typename T>
void momentum_heavy_ball_loop(const Objective<T>& obj, const OptOptions<T>& opts, const MomentumOptions<T>& mopts,
                              crd::memory::IAllocator* alloc, OptResult<T>& result)
{
    const crd::usize n = obj.n();
    const T alpha = mopts.step;
    const T mu = mopts.momentum >= static_cast<T>(0) ? mopts.momentum : static_cast<T>(0.9);

    auto inf_nrm = [](crd::containers::ConstSpan<T> w) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < w.size(); ++i)
        {
            const T a = std::fabs(w[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> g_new(alloc);
    crd::containers::Array<T> v(alloc); // the velocity x_k − x_{k−1} (zero initial)
    crd::containers::Array<T> x_new(alloc);
    g.resize(n);
    g_new.resize(n);
    v.resize(n);
    x_new.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] = static_cast<T>(0);
    }

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

        // v ← μ·v − α·g ; x⁺ = x + v  (Polyak heavy-ball)
        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            v[i] = mu * v[i] - alpha * g[i];
            x_new[i] = x[i] + v[i];
            step_norm_sq += v[i] * v[i];
        }
        const T fx_new = obj.value({x_new.data(), n});
        ++result.fn_evals;
        (void)obj.gradient({x_new.data(), n}, {g_new.data(), n});
        ++result.grad_evals;

        const T df = std::fabs(fx_new - fx);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        fx = fx_new;
        grad_norm = inf_nrm({g.data(), n});

        const auto stop = check_convergence<T>(grad_norm, std::sqrt(step_norm_sq), df, inf_nrm({x, n}), fx, opts);
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
}

template <typename T>
void momentum_nesterov_loop(const Objective<T>& obj, const OptOptions<T>& opts, const MomentumOptions<T>& mopts,
                            crd::memory::IAllocator* alloc, OptResult<T>& result)
{
    const crd::usize n = obj.n();
    const T alpha = mopts.step;
    const bool fixed_mu = mopts.momentum >= static_cast<T>(0);

    auto inf_nrm = [](crd::containers::ConstSpan<T> w) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < w.size(); ++i)
        {
            const T a = std::fabs(w[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g_y(alloc);
    crd::containers::Array<T> v(alloc); // the velocity x_k − x_{k−1} (zero initial ⇒ y₀ = x₀)
    crd::containers::Array<T> y(alloc);
    crd::containers::Array<T> x_new(alloc);
    g_y.resize(n);
    v.resize(n);
    y.resize(n);
    x_new.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] = static_cast<T>(0);
    }

    T* x = result.x.data();
    T fx = obj.value({x, n});
    ++result.fn_evals;

    T t = static_cast<T>(1); // FISTA sequence state (t₀ = 1 ⇒ μ₀ = 0)
    T grad_norm = std::numeric_limits<T>::max();
    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }

        T mu;
        if (fixed_mu)
        {
            mu = mopts.momentum;
        }
        else
        {
            const T t_new =
                static_cast<T>(0.5) * (static_cast<T>(1) + std::sqrt(static_cast<T>(1) + static_cast<T>(4) * t * t));
            mu = (t - static_cast<T>(1)) / t_new;
            t = t_new;
        }

        // Lookahead point y = x + μ·v, the only point the method differentiates.
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = x[i] + mu * v[i];
        }
        (void)obj.gradient({y.data(), n}, {g_y.data(), n});
        ++result.grad_evals;
        grad_norm = inf_nrm({g_y.data(), n});

        if (grad_norm <= opts.grad_tol)
        {
            // Accept y as the returned iterate — gradient and iterate then agree exactly (header note).
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] = y[i];
            }
            fx = obj.value({x, n});
            ++result.fn_evals;
            status = OptStatus::Success;
            break;
        }
        if (!std::isfinite(grad_norm))
        {
            status = OptStatus::NotFinite;
            break;
        }

        // Gradient step from the lookahead: x⁺ = y − α·∇f(y).
        T step_norm_sq = static_cast<T>(0);
        T gdx = static_cast<T>(0); // ∇f(y)·(x⁺ − x), the O'Donoghue-Candès restart test
        for (crd::usize i = 0; i < n; ++i)
        {
            x_new[i] = y[i] - alpha * g_y[i];
            const T d = x_new[i] - x[i];
            step_norm_sq += d * d;
            gdx += g_y[i] * d;
        }
        const T fx_new = obj.value({x_new.data(), n});
        ++result.fn_evals;

        // Gradient restart: momentum carried the step uphill along ∇f(y) ⇒ kill the velocity + reset the schedule.
        if (mopts.adaptive_restart && gdx > static_cast<T>(0))
        {
            t = static_cast<T>(1);
            for (crd::usize i = 0; i < n; ++i)
            {
                v[i] = static_cast<T>(0);
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                v[i] = x_new[i] - x[i];
            }
        }

        const T df = std::fabs(fx_new - fx);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
        }
        fx = fx_new;

        const auto stop = check_convergence<T>(grad_norm, std::sqrt(step_norm_sq), df, inf_nrm({x, n}), fx, opts);
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
}

} // namespace detail

// Minimize `obj` from `x0` by a fixed-step momentum method. `mopts.step` is required (> 0); see MomentumOptions
// for the μ semantics. `obj.has_gradient()` is required (wrap value-only objectives in FiniteDiffObjective /
// FunctorObjective).
template <typename T>
[[nodiscard]] OptResult<T> minimize_momentum(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                             const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                             const MomentumOptions<T>& mopts,
                                             MomentumVariant variant = MomentumVariant::Nesterov)
{
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_momentum needs an analytic gradient (wrap value-only objectives in "
                                       "FiniteDiffObjective / FunctorObjective)");
    CRD_ASSERT_MSG(mopts.step > static_cast<T>(0), "minimize_momentum: mopts.step (α) is required and must be > 0");
    CRD_ASSERT_MSG(mopts.momentum < static_cast<T>(1), "minimize_momentum: fixed momentum μ must be < 1");

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

    if (variant == MomentumVariant::HeavyBall)
    {
        detail::momentum_heavy_ball_loop<T>(obj, opts, mopts, alloc, result);
    }
    else
    {
        detail::momentum_nesterov_loop<T>(obj, opts, mopts, alloc, result);
    }
    return result;
}

} // namespace crd::hesap::opt
