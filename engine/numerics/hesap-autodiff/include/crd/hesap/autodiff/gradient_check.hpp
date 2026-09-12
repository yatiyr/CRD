#pragma once

// gradient_check.hpp — Phase 3.1.6 v15-b: the 3-ORACLE gradient gate. A scalar-generic functor
// `template <class S> S operator()(const S* x, int n)` is differentiated THREE independent ways and the gradients
// cross-checked — the standing correctness contract for every JVP rule and every downstream driver (ADR-0097):
//
//   1. ANALYTIC   — one Jet<double,N> pass (the rules under test).
//   2. COMPLEX-STEP — f(x + i·h)/h, imaginary part (h = 1e-30). Exact to machine precision, NO subtractive
//      cancellation (unlike FD) — the GOLD oracle. Requires the functor to instantiate on std::complex<double>
//      (holomorphic functions: exp/log/sin/cos/…, all crd::math complex-capable). NOT valid at kinks
//      (abs/min/max/hypot at 0) or for the non-holomorphic inverse funcs — use FD + curated singular inputs there.
//   3. FD (central) — (f(x+h) − f(x−h))/2h, the independent-implementation fallback (~1e-6 accuracy).
//
// All three route through crd::math (functors use `using crd::math::<fn>;`), so analytic and complex-step agree to
// ~1e-13. The harness is crd::math-only + allocation-free + Catch2-free (returns discrepancies; the test asserts).

#include <crd/hesap/autodiff/jet.hpp>

#include <crd/math/cmath.hpp>
#include <crd/math/complex.hpp>

#include <complex>

namespace crd::hesap::autodiff::testing
{

// Analytic gradient of f at x[0..N) via a single Jet pass -> g[0..N).
template <int N, class F>
inline void grad_analytic(const F& f, const double* x, double* g) noexcept
{
    forward::Jet<double, N> jx[N];
    for (int j = 0; j < N; ++j)
    {
        jx[j] = forward::Jet<double, N>(x[j], j);
    }
    const forward::Jet<double, N> y = f(jx, N);
    for (int k = 0; k < N; ++k)
    {
        g[k] = y.v[k];
    }
}

// Central-difference gradient (value path on double).
template <int N, class F>
inline void grad_fd(const F& f, const double* x, double* g) noexcept
{
    double xt[N];
    for (int j = 0; j < N; ++j)
    {
        xt[j] = x[j];
    }
    for (int k = 0; k < N; ++k)
    {
        const double h = 1e-6 * crd::math::max(1.0, crd::math::abs(x[k]));
        xt[k]          = x[k] + h;
        const double fp = f(static_cast<const double*>(xt), N);
        xt[k]          = x[k] - h;
        const double fm = f(static_cast<const double*>(xt), N);
        xt[k]          = x[k];
        g[k]           = (fp - fm) / (2.0 * h);
    }
}

// Complex-step gradient (exact; functor MUST compile on std::complex<double>).
template <int N, class F>
inline void grad_cstep(const F& f, const double* x, double* g) noexcept
{
    using C = std::complex<double>;
    C xc[N];
    for (int j = 0; j < N; ++j)
    {
        xc[j] = C(x[j], 0.0);
    }
    constexpr double h = 1e-30;
    for (int k = 0; k < N; ++k)
    {
        xc[k]      = C(x[k], h);
        const C y  = f(static_cast<const C*>(xc), N);
        xc[k]      = C(x[k], 0.0);
        g[k]       = y.imag() / h;
    }
}

// Max |a[k] − b[k]| over k (absolute; the test compares against a tolerance).
template <int N>
[[nodiscard]] inline double max_abs_diff(const double* a, const double* b) noexcept
{
    double m = 0.0;
    for (int k = 0; k < N; ++k)
    {
        const double d = crd::math::abs(a[k] - b[k]);
        m              = d > m ? d : m;
    }
    return m;
}

} // namespace crd::hesap::autodiff::testing
