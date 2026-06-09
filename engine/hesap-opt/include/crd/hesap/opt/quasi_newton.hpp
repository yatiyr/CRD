#pragma once

// quasi_newton.hpp — Phase 3.1.6 v7-d: DENSE quasi-Newton (BFGS + SR1), the small-n complement to L-BFGS. Both
// maintain the INVERSE-Hessian approximation H directly (p = −H·g, no linear solve) and update it per iteration:
//   BFGS (rank-2, keeps H SPD given sᵀy>0): H⁺ = (I−ρsyᵀ)H(I−ρysᵀ) + ρssᵀ,  ρ=1/sᵀy
//   SR1  (rank-1, can go indefinite):       H⁺ = H + (s−Hy)(s−Hy)ᵀ / (s−Hy)ᵀy,  skipped when the denom is tiny
// O(n²) per iteration ⇒ small n only (L-BFGS is the large-scale workhorse). SR1's line-search form falls back to
// steepest descent if H·g is not a descent direction (SR1's natural home is trust-region, v7-h). ADR-0090; N&W §6.

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

enum class QuasiNewtonUpdate : crd::u8
{
    Bfgs, // rank-2, SPD-preserving (the default)
    Sr1,  // rank-1, indefinite-capable (line-search form: steepest-descent fallback if not a descent direction)
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_quasi_newton(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                 const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                 QuasiNewtonUpdate update = QuasiNewtonUpdate::Bfgs,
                                                 const LineSearch<T>* line_search = nullptr)
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_quasi_newton needs an analytic gradient");
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

    crd::containers::Array<T> g(alloc), g_new(alloc), p(alloc), x_new(alloc), hy(alloc), s(alloc), y(alloc);
    crd::containers::Array<T> h(alloc); // dense inverse-Hessian approx (row-major n×n)
    g.resize(n);
    g_new.resize(n);
    p.resize(n);
    x_new.resize(n);
    hy.resize(n);
    s.resize(n);
    y.resize(n);
    h.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            h[i * n + j] = (i == j) ? static_cast<T>(1) : static_cast<T>(0); // H₀ = I
        }
    }

    const MoreThuenteLineSearch<T> default_ls;
    const LineSearch<T>&           ls = line_search != nullptr ? *line_search : default_ls;

    T* x = result.x.data();
    T  fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    T grad_norm = inf_norm({g.data(), n});

    bool       first_update = true;
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

        // p = −H·g.
        for (crd::usize i = 0; i < n; ++i)
        {
            T acc = static_cast<T>(0);
            const T* hi = &h[i * n];
            for (crd::usize j = 0; j < n; ++j)
            {
                acc += hi[j] * g[j];
            }
            p[i] = -acc;
        }
        // SR1's H may be indefinite ⇒ guard the descent direction.
        if (update == QuasiNewtonUpdate::Sr1 && dn::dot<T>({g.data(), n}, {p.data(), n}) >= static_cast<T>(0))
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                p[i] = -g[i];
            }
        }

        T step0 = static_cast<T>(1);
        if (first_update)
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

        T sy = static_cast<T>(0);
        T yy = static_cast<T>(0);
        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            s[i] = x_new[i] - x[i];
            y[i] = g_new[i] - g[i];
            sy += s[i] * y[i];
            yy += y[i] * y[i];
            step_norm_sq += s[i] * s[i];
        }

        const T df = std::fabs(r.fx_new - fx);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        fx = r.fx_new;
        grad_norm = inf_norm({g.data(), n});

        // Rescale H₀ ← (sᵀy/yᵀy)·I before the first update (Nocedal §6.1 — improves the initial scale).
        if (first_update && sy > std::numeric_limits<T>::epsilon() * yy)
        {
            const T scale = sy / yy;
            for (crd::usize i = 0; i < n * n; ++i)
            {
                h[i] = static_cast<T>(0);
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                h[i * n + i] = scale;
            }
            first_update = false;
        }

        if (update == QuasiNewtonUpdate::Bfgs)
        {
            if (sy > std::numeric_limits<T>::epsilon() * yy) // skip if curvature non-positive (keeps H SPD)
            {
                for (crd::usize i = 0; i < n; ++i) // Hy = H·y
                {
                    T acc = static_cast<T>(0);
                    const T* hi = &h[i * n];
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        acc += hi[j] * y[j];
                    }
                    hy[i] = acc;
                }
                const T yhy = dn::dot<T>({y.data(), n}, {hy.data(), n});
                const T rho = static_cast<T>(1) / sy;
                const T c1 = (static_cast<T>(1) + yhy * rho) * rho; // coeff of s·sᵀ
                for (crd::usize i = 0; i < n; ++i)
                {
                    T* hi = &h[i * n];
                    const T si = s[i];
                    const T hyi = hy[i];
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        hi[j] += c1 * si * s[j] - rho * (hyi * s[j] + si * hy[j]);
                    }
                }
            }
        }
        else // SR1
        {
            for (crd::usize i = 0; i < n; ++i) // hy = H·y; w = s − Hy stored back into hy
            {
                T acc = static_cast<T>(0);
                const T* hi = &h[i * n];
                for (crd::usize j = 0; j < n; ++j)
                {
                    acc += hi[j] * y[j];
                }
                hy[i] = s[i] - acc; // w
            }
            const T wy = dn::dot<T>({hy.data(), n}, {y.data(), n});
            const T wnorm = std::sqrt(dn::dot<T>({hy.data(), n}, {hy.data(), n}));
            const T ynorm = std::sqrt(yy);
            if (std::fabs(wy) > static_cast<T>(1e-8) * wnorm * ynorm) // SR1 skip safeguard
            {
                const T inv = static_cast<T>(1) / wy;
                for (crd::usize i = 0; i < n; ++i)
                {
                    T* hi = &h[i * n];
                    const T wi = hy[i];
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        hi[j] += inv * wi * hy[j];
                    }
                }
            }
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

// Convenience wrappers.
template <typename T>
[[nodiscard]] OptResult<T> minimize_bfgs(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                         const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                         const LineSearch<T>* line_search = nullptr)
{
    return minimize_quasi_newton<T>(obj, x0, opts, alloc, QuasiNewtonUpdate::Bfgs, line_search);
}

template <typename T>
[[nodiscard]] OptResult<T> minimize_sr1(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                        const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                        const LineSearch<T>* line_search = nullptr)
{
    return minimize_quasi_newton<T>(obj, x0, opts, alloc, QuasiNewtonUpdate::Sr1, line_search);
}

} // namespace crd::hesap::opt
