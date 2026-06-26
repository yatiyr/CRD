#pragma once

// newton_cg.hpp — Phase 3.1.6 v7-g: NEWTON-CG / truncated Newton — the LARGE-SCALE Newton method. The Newton
// system ∇²f·p = −∇f is solved INEXACTLY by inner conjugate-gradient using ONLY Hessian-vector products
// (Objective::hessian_vector — never forms the Hessian; O(n) memory like L-BFGS but with true second-order
// information). Two textbook safeguards (Nocedal & Wright Algorithm 7.1, "Line Search Newton-CG"):
//   • NEGATIVE-CURVATURE EXIT — if the inner CG meets a direction d with dᵀ(∇²f)d ≤ 0, it stops and returns the
//     current inner iterate (or steepest descent if it is the FIRST inner step) — guarantees a descent direction
//     through indefinite regions, the truncated-Newton analog of the modified-Newton τ·I device.
//   • FORCING SEQUENCE — the inner solve stops at ‖r‖ ≤ η·‖∇f‖ with η = min(0.5, √‖∇f‖): loose when far away
//     (cheap), tight when close ⇒ SUPERLINEAR outer convergence (Eisenstat-Walker / Dembo-Steihaug; the same
//     rule scipy's 'Newton-CG' uses — the v7-z eval-parity peer).
// ADR-0090; Nocedal & Wright §7.1.
//
// DETERMINISM MOAT: the outer loop and the inner CG are SERIAL scalar recurrences; only value/gradient/
// hessian_vector may be parallel-but-bit-exact ⇒ the trajectory is bit-identical across worker counts.

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

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

// Minimize `obj` from `x0` by line-search Newton-CG. Requires has_gradient() + has_hessian_vector().
// `cg_max_iters` caps the inner CG (0 ⇒ 20·n, the scipy default). `OptResult::hess_evals` counts
// hessian_vector() PRODUCTS (the truncated-Newton cost metric).
template <typename T>
[[nodiscard]] OptResult<T> minimize_newton_cg(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                              const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                              const LineSearch<T>* line_search = nullptr, crd::usize cg_max_iters = 0)
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_newton_cg needs an analytic gradient");
    CRD_ASSERT_MSG(obj.has_hessian_vector(), "minimize_newton_cg needs hessian_vector (the matrix-free Newton "
                                             "path; use minimize_newton for dense-Hessian objectives)");
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
    const crd::usize cg_cap = cg_max_iters > 0 ? cg_max_iters : 20 * n;

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
    crd::containers::Array<T> p(alloc);  // the outer search direction (inner CG output z)
    crd::containers::Array<T> r(alloc);  // inner CG residual
    crd::containers::Array<T> d(alloc);  // inner CG direction
    crd::containers::Array<T> hd(alloc); // ∇²f·d
    crd::containers::Array<T> x_new(alloc);
    g.resize(n);
    g_new.resize(n);
    p.resize(n);
    r.resize(n);
    d.resize(n);
    hd.resize(n);
    x_new.resize(n);

    const WolfeLineSearch<T> default_ls; // strong Wolfe c1=1e-4, c2=0.9; α₀ = 1 (Newton steps self-scale)
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

        // Inner CG on ∇²f·z = −g (N&W Alg 7.1): z₀ = 0, r₀ = g, d₀ = −g.
        const T gnorm2 = crd::math::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
        const T eta = crd::math::sqrt(gnorm2) < static_cast<T>(0.5) ? crd::math::sqrt(gnorm2) : static_cast<T>(0.5);
        const T inner_tol = eta * gnorm2;

        for (crd::usize i = 0; i < n; ++i)
        {
            p[i] = static_cast<T>(0);
            r[i] = g[i];
            d[i] = -g[i];
        }
        T rr = dn::dot<T>({r.data(), n}, {r.data(), n});
        for (crd::usize j = 0; j < cg_cap; ++j)
        {
            (void)obj.hessian_vector({x, n}, {d.data(), n}, {hd.data(), n});
            ++result.hess_evals;
            const T dhd = dn::dot<T>({d.data(), n}, {hd.data(), n});
            if (!(dhd > static_cast<T>(0))) // negative curvature (or breakdown)
            {
                if (j == 0)
                {
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        p[i] = -g[i]; // first step: fall back to steepest descent
                    }
                }
                break; // else: return the z built so far (a descent direction)
            }
            const T alpha = rr / dhd;
            for (crd::usize i = 0; i < n; ++i)
            {
                p[i] += alpha * d[i];
                r[i] += alpha * hd[i];
            }
            const T rr_new = dn::dot<T>({r.data(), n}, {r.data(), n});
            if (crd::math::sqrt(rr_new) <= inner_tol)
            {
                break; // forcing test met — truncate
            }
            const T beta = rr_new / rr;
            for (crd::usize i = 0; i < n; ++i)
            {
                d[i] = -r[i] + beta * d[i];
            }
            rr = rr_new;
        }

        const auto lr = ls.search(obj, {x, n}, fx, {g.data(), n}, {p.data(), n}, static_cast<T>(1), {x_new.data(), n},
                                  {g_new.data(), n});
        result.fn_evals += lr.evals;
        result.grad_evals += lr.grad_evals;
        if (!lr.ok)
        {
            status = OptStatus::LineSearchFailed;
            break;
        }
        if (!lr.grad_at_new_valid)
        {
            (void)obj.gradient({x_new.data(), n}, {g_new.data(), n});
            ++result.grad_evals;
        }

        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T dx = x_new[i] - x[i];
            step_norm_sq += dx * dx;
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        const T df = crd::math::fabs(lr.fx_new - fx);
        fx = lr.fx_new;
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
