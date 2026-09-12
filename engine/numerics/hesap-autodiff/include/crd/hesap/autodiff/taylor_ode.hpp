#pragma once

// taylor_ode.hpp — Phase 3.1.6 v15-g: the TAYLOR-SERIES ODE integrator. For y' = f(y,t), the solution's own Taylor
// coefficients y_k are built ORDER BY ORDER: knowing y_0..y_k, evaluate f on the truncated TaylorJet — its k-th
// coefficient [f]_k depends only on y_0..y_k, and y_{k+1} = [f]_k/(k+1). K such evaluations give the order-K local
// series; one Horner sum advances the step. A single order-K step covers a large interval on SMOOTH problems, so with
// the order MATCHED to the tolerance (`taylor_solve_auto`) it beats RK/DOP853 on accuracy-per-eval across the useful
// range and CRUSHES on the high-precision frontier RK can't reach. ADR-0097.

#include <crd/hesap/autodiff/taylor.hpp>

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

// Build the order-K local Taylor coefficients yc[0..K] of the solution of y'=rhs(y,t) at (t,y), order by order.
// rhs: `TJ rhs(const TJ& y, const TJ& t)` (scalar generic). `yc` scratch length K+1.
template <int K, class F>
inline void taylor_coeffs(const F& rhs, crd::f64 t, crd::f64 y, crd::f64* yc) noexcept
{
    yc[0] = y;
    for (int k = 0; k < K; ++k)
    {
        TaylorJet<crd::f64, K> yj;
        for (int i = 0; i <= K; ++i) { yj.a[i] = (i <= k) ? yc[i] : 0.0; }
        TaylorJet<crd::f64, K> tj;
        tj.a[0] = t;
        if constexpr (K >= 1) { tj.a[1] = 1.0; }
        const TaylorJet<crd::f64, K> fj = rhs(yj, tj);
        yc[k + 1]                       = fj.a[k] / static_cast<crd::f64>(k + 1);
    }
}

// Robust adaptive step from the local coefficients. The bare Jorba-Zou estimate (tol/|a_K|)^{1/K} is FOOLED when a
// trailing coefficient momentarily vanishes (oscillatory solutions) → a huge bogus step → divergence. Fix: take the
// MIN over the last few coefficients (a single near-zero one can no longer inflate h) AND cap the step growth vs the
// previous step. This yields a bounded, error-controlled step at every order.
template <int K>
[[nodiscard]] inline crd::f64 taylor_step_size(const crd::f64* yc, crd::f64 tol, crd::f64 h_prev) noexcept
{
    constexpr crd::f64 tiny   = 1e-300;
    constexpr crd::f64 safety = 0.7;
    crd::f64           h      = 1e300;
    const int          lo     = (K >= 3) ? (K - 2) : 1;
    for (int j = K; j >= lo; --j)
    {
        const crd::f64 aj = crd::math::abs(yc[j]);
        if (aj > tiny)
        {
            const crd::f64 hj = crd::math::pow(tol / aj, 1.0 / static_cast<crd::f64>(j));
            if (hj < h) { h = hj; }
        }
    }
    h *= safety;
    if (h_prev > 0.0 && h > 2.0 * h_prev) { h = 2.0 * h_prev; } // cap explosive growth (near-zero-coefficient guard)
    return h;
}

// Integrate y'=rhs(y,t) from (t0,y0) to t_end (t_end ≥ t0). Returns y(t_end); *nsteps out = steps taken.
template <int K, class F>
[[nodiscard]] inline crd::f64 taylor_solve(const F& rhs, crd::f64 t0, crd::f64 y0, crd::f64 t_end, crd::f64 tol,
                                           int* nsteps = nullptr) noexcept
{
    crd::f64 t      = t0;
    crd::f64 y      = y0;
    crd::f64 h_prev = 0.0;
    crd::f64 yc[K + 1];
    int      steps = 0;
    while (t < t_end)
    {
        taylor_coeffs<K>(rhs, t, y, yc);
        crd::f64 h = taylor_step_size<K>(yc, tol, h_prev);
        if (t + h > t_end) { h = t_end - t; }
        crd::f64 yn = yc[K]; // advance via Horner: y(t+h) = Σ yc[k] h^k
        for (int k = K - 1; k >= 0; --k) { yn = yn * h + yc[k]; }
        t += h;
        y      = yn;
        h_prev = h;
        ++steps;
    }
    if (nsteps != nullptr) { *nsteps = steps; }
    return y;
}

// ADAPTIVE-ORDER solve — the order K must be matched to the tolerance (a high-order method at loose tolerance is
// wasteful; a low-order one at tight tolerance stalls). K_opt ≈ ⌈−½·ln(tol)⌉ (Jorba-Zou 2005). This is Taylor "at its
// best" — the fair way to benchmark it, exactly as an RK stepper adapts its step. Dispatches to a compile-time K.
template <class F>
[[nodiscard]] inline crd::f64 taylor_solve_auto(const F& rhs, crd::f64 t0, crd::f64 y0, crd::f64 t_end, crd::f64 tol,
                                                int* nsteps = nullptr) noexcept
{
    if (tol >= 1e-4) { return taylor_solve<6>(rhs, t0, y0, t_end, tol, nsteps); }
    if (tol >= 1e-7) { return taylor_solve<10>(rhs, t0, y0, t_end, tol, nsteps); }
    if (tol >= 1e-10) { return taylor_solve<14>(rhs, t0, y0, t_end, tol, nsteps); }
    if (tol >= 1e-13) { return taylor_solve<18>(rhs, t0, y0, t_end, tol, nsteps); }
    return taylor_solve<24>(rhs, t0, y0, t_end, tol, nsteps);
}

} // namespace crd::hesap::autodiff::forward
