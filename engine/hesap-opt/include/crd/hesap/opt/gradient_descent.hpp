#pragma once

// gradient_descent.hpp — Phase 3.1.6 v7-a: steepest descent + a line search. The first end-to-end optimizer; it
// exercises the whole substrate (Objective → gradient → descent direction → line search → convergence → result)
// and establishes the determinism contract: the optimizer is SERIAL scalar arithmetic, so given a bit-exact
// objective eval (e.g. ParallelSparseLinearOp) the trajectory is bit-identical across worker counts. ADR-0090.
//
// NOTE: steepest descent is the substrate proof, NOT the workhorse — L-BFGS (v7-d) and the rest build on the same
// interfaces. v7-a requires an analytic gradient (has_gradient()); the finite-difference fallback is v7-b.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{
namespace detail
{
template <typename T> [[nodiscard]] inline T inf_norm(crd::containers::ConstSpan<T> v) noexcept
{
    T m = static_cast<T>(0);
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        const T a = crd::math::fabs(v[i]);
        if (a > m)
        {
            m = a;
        }
    }
    return m;
}
} // namespace detail

// Minimize `obj` from `x0` by steepest descent with the supplied line search (default backtracking-Armijo).
template <typename T>
[[nodiscard]] OptResult<T> minimize_gradient_descent(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                     const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                     const LineSearch<T>* line_search = nullptr)
{
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_gradient_descent: v7-a needs an analytic gradient (FD = v7-b)");
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

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> g_new(alloc);
    g.resize(n);
    p.resize(n);
    x_new.resize(n);
    g_new.resize(n);

    const BacktrackingArmijo<T> default_ls;
    const LineSearch<T>& ls = line_search != nullptr ? *line_search : default_ls;

    T* x = result.x.data();
    T fx = obj.value({x, n});
    [[maybe_unused]] const bool gok = obj.gradient({x, n}, {g.data(), n});
    CRD_ASSERT_MSG(gok, "Objective::has_gradient() is true but gradient() returned false (capability contract)");
    T grad_norm = detail::inf_norm<T>({g.data(), n});

    OptStatus status = OptStatus::MaxIterations;
    crd::usize iter = 0;
    for (; iter < opts.max_iters; ++iter)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        if (grad_norm <= opts.grad_tol) // first-order optimality already met
        {
            status = OptStatus::Success;
            break;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            p[i] = -g[i]; // steepest-descent direction
        }
        const auto r = ls.search(obj, {x, n}, fx, {g.data(), n}, {p.data(), n}, static_cast<T>(1), {x_new.data(), n},
                                 {g_new.data(), n});
        if (!r.ok)
        {
            status = OptStatus::LineSearchFailed;
            break;
        }
        T step_norm = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T d = x_new[i] - x[i];
            step_norm += d * d;
        }
        step_norm = crd::math::sqrt(step_norm);
        const T df = crd::math::fabs(r.fx_new - fx);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
        }
        fx = r.fx_new;
        if (r.grad_at_new_valid) // the line search already computed ∇f at the accepted point (Wolfe, v7-c)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                g[i] = g_new[i];
            }
        }
        else
        {
            (void)obj.gradient({x, n}, {g.data(), n}); // Armijo: recompute ∇f once at the accepted point
        }
        grad_norm = detail::inf_norm<T>({g.data(), n});
        const T x_norm = detail::inf_norm<T>({x, n});
        const auto stop = check_convergence<T>(grad_norm, step_norm, df, x_norm, fx, opts);
        if (stop.has_value())
        {
            status = *stop;
            break;
        }
    }

    result.fx = fx;
    result.grad_norm = grad_norm;
    result.iterations = iter;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
