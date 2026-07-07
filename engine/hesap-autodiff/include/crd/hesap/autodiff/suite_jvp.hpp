#pragma once

// suite_jvp.hpp — Phase 3.1.6 v15-f: the SUITE forward differentials (FFT / DSP filtering / spline). These are the
// two v15-f principles applied to the transform surface:
//   • LINEAR op  ⇒  jvp(op)(x, dx) = op(dx).  The FFT is linear, so the FFT JVP IS the transform applied to the
//     tangent — no new rule, the consumer reuses the engine FFT on dx (normalization passes through unchanged;
//     rfft with a real tangent is trivial — Hermitian packing only bites the v16 VJP). Reference `dft` below makes
//     the identity testable.
//   • BILINEAR op ⇒ product rule.  Filtering y = h⊛x ⇒ dy = dh⊛x + h⊛dx (`conv_jvp`).
//   • SPLINE (documented): wrt the eval point → the spline DERIVATIVE at t (Hermite derivative basis); wrt control
//     values → reuse the build's Thomas tridiagonal factor (`dd = T⁻¹(R·dy)`) — the SAME factor-reuse as a dense
//     solve (matrix_jvp.hpp `solve_spd_jvp`), one back-solve on the stored tridiagonal LU.
//
// Self-contained + allocation-free (caller owns scratch); ADR-0097.

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <complex>

namespace crd::hesap::autodiff::forward::suite
{

// Reference naive DFT (O(n²)): Y_k = Σ_j x_j · e^{−2πi kj/n}. Deterministic (crd::math sincos).
inline void dft(const std::complex<crd::f64>* x, std::complex<crd::f64>* y, int n) noexcept
{
    const crd::f64 twopi = -6.283185307179586476925286766559;
    for (int k = 0; k < n; ++k)
    {
        crd::f64 re = 0.0;
        crd::f64 im = 0.0;
        for (int j = 0; j < n; ++j)
        {
            crd::f64       s = 0.0;
            crd::f64       c = 0.0;
            const crd::f64 ang = twopi * static_cast<crd::f64>((k * j) % n) / static_cast<crd::f64>(n);
            crd::math::sincos(ang, s, c);
            re += x[j].real() * c - x[j].imag() * s;
            im += x[j].real() * s + x[j].imag() * c;
        }
        y[k] = {re, im};
    }
}
// FFT JVP: linear ⇒ dY = DFT(dX). (Provided for symmetry / testability; identical to calling dft on the tangent.)
inline void dft_jvp(const std::complex<crd::f64>* dx, std::complex<crd::f64>* dy, int n) noexcept { dft(dx, dy, n); }

// Full convolution y = h⊛x, y length nh+nx−1.
inline void conv(const crd::f64* h, int nh, const crd::f64* x, int nx, crd::f64* y) noexcept
{
    for (int i = 0; i < nh + nx - 1; ++i) { y[i] = 0.0; }
    for (int i = 0; i < nh; ++i)
    {
        for (int j = 0; j < nx; ++j) { y[i + j] += h[i] * x[j]; }
    }
}
// Filtering JVP (bilinear product rule): dy = dh⊛x + h⊛dx.
inline void conv_jvp(const crd::f64* h, const crd::f64* dh, int nh, const crd::f64* x, const crd::f64* dx, int nx,
                     crd::f64* dy, crd::f64* scratch /*nh+nx-1*/) noexcept
{
    conv(dh, nh, x, nx, dy);
    conv(h, nh, dx, nx, scratch);
    for (int i = 0; i < nh + nx - 1; ++i) { dy[i] += scratch[i]; }
}

// Tridiagonal solve (Thomas) T·u = r, T = (sub a, diag b, super c), all length n (a[0], c[n-1] unused). u may alias r.
// The spline-build factor-reuse: build the spline once (its Thomas factor), then the control-value JVP is ONE
// back-solve `du = T⁻¹(dr)` — the same principle as the dense solve JVP.
inline void thomas_solve(const crd::f64* a, const crd::f64* b, const crd::f64* c, const crd::f64* r, crd::f64* u,
                         int n, crd::f64* cp /*n*/, crd::f64* dp /*n*/) noexcept
{
    cp[0] = c[0] / b[0];
    dp[0] = r[0] / b[0];
    for (int i = 1; i < n; ++i)
    {
        const crd::f64 mden = b[i] - a[i] * cp[i - 1];
        cp[i]               = c[i] / mden;
        dp[i]               = (r[i] - a[i] * dp[i - 1]) / mden;
    }
    u[n - 1] = dp[n - 1];
    for (int i = n - 2; i >= 0; --i) { u[i] = dp[i] - cp[i] * u[i + 1]; }
}

} // namespace crd::hesap::autodiff::forward::suite
