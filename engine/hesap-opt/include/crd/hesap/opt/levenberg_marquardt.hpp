#pragma once

// levenberg_marquardt.hpp — Phase 3.1.6 v7-e: Levenberg-Marquardt + Gauss-Newton for nonlinear least-squares
// min ½‖r(x)‖². Solves the damped normal equations (JᵀJ + λ·diag(JᵀJ))·δ = −Jᵀr each step (Marquardt scaling),
// with the **Madsen-Nielsen-Tingleff ν damping update** driven by the trust-region gain ratio ρ — the SAME update
// MINPACK lmder / Ceres use, so the iteration count is comparable to the gold standard (the damping rule IS the
// iteration determinant; advisor-pinned). Gauss-Newton = λ fixed at 0. Robust losses (Huber/Cauchy/Tukey) reweight
// the residuals (IRLS M-estimate). ADR-0090; Madsen-Nielsen-Tingleff "Methods for Non-Linear Least Squares".
//
// ⚠ NORMAL-EQUATIONS HONESTY (advisor): (JᵀJ+λD) squares κ(J); λ mitigates it, and forming JᵀJ is exactly what
// lets the SPARSE crush (v7-e-2: hesap-direct sparse Cholesky on JᵀJ) work. MINPACK 'lm' is QR-based (never forms
// JᵀJ) ⇒ on ill-conditioned J expect different iters/accuracy; report final cost + ‖Jᵀr‖, not an eval-count match.
// This v7-e-1 dense path solves the small n×n normal equations with an inline Cholesky.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/residual_function.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

enum class RobustLoss : crd::u8
{
    None,   // plain least-squares (the gold-standard head-to-head case)
    Huber,  // quadratic near 0, linear in the tail
    Cauchy, // log — strong outlier rejection
    Tukey,  // redescending — full rejection beyond the scale
};

namespace detail
{
// IRLS weight w with r_weighted = w·r, J_row_weighted = w·J_row, so the reweighted normal equations approximate
// the M-estimate min Σ ρ(r_i²). w = sqrt(ρ'(s)), s = (r/scale)². (loss==None ⇒ w=1.)
template <typename T>
[[nodiscard]] inline T robust_weight(RobustLoss loss, T ri, T scale) noexcept
{
    if (loss == RobustLoss::None)
    {
        return static_cast<T>(1);
    }
    const T k = scale;
    const T s = (ri / k) * (ri / k);
    switch (loss)
    {
    case RobustLoss::Huber:
        return s <= static_cast<T>(1) ? static_cast<T>(1) : crd::math::sqrt(static_cast<T>(1) / crd::math::sqrt(s));
    case RobustLoss::Cauchy:
        return crd::math::sqrt(static_cast<T>(1) / (static_cast<T>(1) + s));
    case RobustLoss::Tukey:
        return s <= static_cast<T>(1) ? (static_cast<T>(1) - s) : static_cast<T>(0); // sqrt((1-s)²)=|1-s|, s≤1
    case RobustLoss::None:
        break;
    }
    return static_cast<T>(1);
}

// In-place Cholesky factor (lower) of an n×n SPD matrix a (row-major), then solve a·x = b (b in/out → x).
// Returns false if a is not positive-definite (a non-positive pivot) — the LM driver then raises λ.
template <typename T>
[[nodiscard]] inline bool chol_solve(T* a, crd::usize n, T* b) noexcept
{
    for (crd::usize j = 0; j < n; ++j)
    {
        T d = a[j * n + j];
        for (crd::usize k = 0; k < j; ++k)
        {
            d -= a[j * n + k] * a[j * n + k];
        }
        if (!(d > static_cast<T>(0)))
        {
            return false; // not positive-definite
        }
        d = crd::math::sqrt(d);
        a[j * n + j] = d;
        for (crd::usize i = j + 1; i < n; ++i)
        {
            T s = a[i * n + j];
            for (crd::usize k = 0; k < j; ++k)
            {
                s -= a[i * n + k] * a[j * n + k];
            }
            a[i * n + j] = s / d;
        }
    }
    // forward solve L·y = b
    for (crd::usize i = 0; i < n; ++i)
    {
        T s = b[i];
        for (crd::usize k = 0; k < i; ++k)
        {
            s -= a[i * n + k] * b[k];
        }
        b[i] = s / a[i * n + i];
    }
    // back solve Lᵀ·x = y
    for (crd::usize ii = n; ii-- > 0;)
    {
        T s = b[ii];
        for (crd::usize k = ii + 1; k < n; ++k)
        {
            s -= a[k * n + ii] * b[k];
        }
        b[ii] = s / a[ii * n + ii];
    }
    return true;
}
} // namespace detail

// Minimize ½‖r(x)‖² from x0. `tau` scales the initial damping (λ0 = τ·max diag(JᵀJ)); `gauss_newton` forces λ=0
// (undamped GN — only for well-conditioned, near-quadratic problems). Robust loss reweights residuals (IRLS).
template <typename T>
[[nodiscard]] OptResult<T> minimize_levenberg_marquardt(const ResidualFunction<T>& res,
                                                        crd::containers::ConstSpan<T> x0, const OptOptions<T>& opts,
                                                        crd::memory::IAllocator* alloc, T tau = static_cast<T>(1e-3),
                                                        bool gauss_newton = false, RobustLoss loss = RobustLoss::None,
                                                        T loss_scale = static_cast<T>(1))
{
    CRD_ASSERT_MSG(res.has_jacobian(), "minimize_levenberg_marquardt: v7-e-1 needs an analytic Jacobian (FD = later)");
    const crd::usize n = res.n();
    const crd::usize m = res.num_residuals();

    OptResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    if (n == 0 || m == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    crd::containers::Array<T> r(alloc);
    crd::containers::Array<T> jac(alloc);
    crd::containers::Array<T> jtj(alloc);
    crd::containers::Array<T> mtx(alloc);
    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> delta(alloc);
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> r_new(alloc);
    crd::containers::Array<T> diag(alloc);
    r.resize(m);
    jac.resize(m * n);
    jtj.resize(n * n);
    mtx.resize(n * n);
    g.resize(n);
    delta.resize(n);
    x_new.resize(n);
    r_new.resize(n > m ? n : m);
    diag.resize(n);

    T* x = result.x.data();

    // Evaluate r, J at x with robust weights; returns ½‖r‖². Fills r (weighted) + jac (weighted, row-major).
    auto eval_rj = [&](const T* xv, bool need_jac) -> T
    {
        res.residuals({xv, n}, {r.data(), m});
        if (need_jac)
        {
            (void)res.jacobian({xv, n}, {jac.data(), m * n});
        }
        T cost = static_cast<T>(0);
        for (crd::usize i = 0; i < m; ++i)
        {
            const T w = detail::robust_weight<T>(loss, r[i], loss_scale);
            r[i] *= w;
            if (need_jac && w != static_cast<T>(1))
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    jac[i * n + j] *= w;
                }
            }
            cost += r[i] * r[i];
        }
        return static_cast<T>(0.5) * cost;
    };

    auto inf_norm = [](const T* v, crd::usize len) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < len; ++i)
        {
            const T a = crd::math::fabs(v[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    // JtJ = JᵀJ, g = Jᵀr (the gradient of ½‖r‖²).
    auto form_normal = [&]()
    {
        for (crd::usize a2 = 0; a2 < n; ++a2)
        {
            g[a2] = static_cast<T>(0);
            for (crd::usize b2 = 0; b2 < n; ++b2)
            {
                jtj[a2 * n + b2] = static_cast<T>(0);
            }
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            const T* ji = &jac[i * n];
            const T  ri = r[i];
            for (crd::usize a2 = 0; a2 < n; ++a2)
            {
                g[a2] += ji[a2] * ri;
                const T jia = ji[a2];
                for (crd::usize b2 = a2; b2 < n; ++b2)
                {
                    jtj[a2 * n + b2] += jia * ji[b2];
                }
            }
        }
        for (crd::usize a2 = 0; a2 < n; ++a2) // mirror to the lower triangle
        {
            for (crd::usize b2 = a2 + 1; b2 < n; ++b2)
            {
                jtj[b2 * n + a2] = jtj[a2 * n + b2];
            }
        }
    };

    T cost = eval_rj(x, true);
    form_normal();
    T grad_norm = inf_norm(g.data(), n);

    T lambda = static_cast<T>(0);
    if (!gauss_newton)
    {
        T maxdiag = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            maxdiag = jtj[i * n + i] > maxdiag ? jtj[i * n + i] : maxdiag;
        }
        lambda = tau * maxdiag;
    }
    T nu = static_cast<T>(2);

    OptStatus  status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(cost);
        }
        if (grad_norm <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }

        // Build M = JtJ + λ·diag(JtJ) and solve M·δ = −g (retry with larger λ if not PD).
        bool solved = false;
        for (int attempt = 0; attempt < 30 && !solved; ++attempt)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                diag[i] = jtj[i * n + i];
                for (crd::usize j = 0; j < n; ++j)
                {
                    mtx[i * n + j] = jtj[i * n + j];
                }
                mtx[i * n + i] += lambda * diag[i];
                delta[i] = -g[i];
            }
            solved = detail::chol_solve<T>(mtx.data(), n, delta.data());
            if (!solved)
            {
                if (gauss_newton)
                {
                    break; // GN with a singular JtJ — give up (use LM for rank-deficient)
                }
                lambda = lambda > static_cast<T>(0) ? lambda * nu : static_cast<T>(1);
                nu *= static_cast<T>(2);
            }
        }
        if (!solved)
        {
            status = OptStatus::LineSearchFailed; // could not form a descent step
            break;
        }

        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            x_new[i] = x[i] + delta[i];
            step_norm_sq += delta[i] * delta[i];
        }
        const T new_cost = eval_rj(x_new.data(), false); // residuals only at the trial point

        // Predicted gain L(0)−L(δ) = ½·δᵀ(λ·D·δ − g)  (Madsen). For GN, pred = ½·δᵀ(−g) = −½ gᵀδ.
        T pred = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            pred += delta[i] * (lambda * diag[i] * delta[i] - g[i]);
        }
        pred *= static_cast<T>(0.5);
        const T rho = pred > static_cast<T>(0) ? (cost - new_cost) / pred : (cost - new_cost);

        if (rho > static_cast<T>(0) && std::isfinite(new_cost)) // step accepted
        {
            const T df = cost - new_cost;
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] = x_new[i];
            }
            // recompute r,J at the new x (r currently holds r_new; need J too)
            cost = eval_rj(x, true);
            form_normal();
            grad_norm = inf_norm(g.data(), n);
            if (!gauss_newton)
            {
                const T factor = static_cast<T>(1) - (static_cast<T>(2) * rho - static_cast<T>(1)) *
                                                         (static_cast<T>(2) * rho - static_cast<T>(1)) *
                                                         (static_cast<T>(2) * rho - static_cast<T>(1));
                const T lo = static_cast<T>(1) / static_cast<T>(3);
                lambda *= factor > lo ? factor : lo;
                nu = static_cast<T>(2);
            }
            const T x_norm = inf_norm(x, n);
            const auto stop = check_convergence<T>(grad_norm, crd::math::sqrt(step_norm_sq), crd::math::fabs(df), x_norm, cost,
                                                   opts);
            if (stop.has_value())
            {
                status = *stop;
                break;
            }
        }
        else // step rejected: increase damping
        {
            if (gauss_newton)
            {
                status = OptStatus::LineSearchFailed; // undamped GN cannot recover from a bad step
                break;
            }
            lambda *= nu;
            nu *= static_cast<T>(2);
        }
    }

    result.fx = cost; // ½‖r(x*)‖²
    result.grad_norm = grad_norm;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

// Convenience: undamped Gauss-Newton (λ ≡ 0).
template <typename T>
[[nodiscard]] OptResult<T> minimize_gauss_newton(const ResidualFunction<T>& res, crd::containers::ConstSpan<T> x0,
                                                 const OptOptions<T>& opts, crd::memory::IAllocator* alloc)
{
    return minimize_levenberg_marquardt<T>(res, x0, opts, alloc, static_cast<T>(0), /*gauss_newton=*/true);
}

} // namespace crd::hesap::opt
