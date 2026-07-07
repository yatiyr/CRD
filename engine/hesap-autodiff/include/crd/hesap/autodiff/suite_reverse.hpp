#pragma once

// suite_reverse.hpp — Phase 3.1.6 v16-d: the SUITE reverse differentials (FFT / DSP filtering / spline) — the
// transpose of the v15-f suite JVPs.
//   • FFT VJP = the ADJOINT DFT = the (unnormalised) INVERSE DFT.  The DFT is the linear map Y = F·X with
//     F_kj = e^{−2πi kj/n}; its adjoint wrt the real inner product ⟨u,v⟩=Re Σ conj(u)v is Fᴴ = conj(F), i.e.
//     X̄_j = Σ_k Ȳ_k e^{+2πi kj/n} — an IDFT with no 1/n. Exact (linear), deterministic (crd::math sincos).
//   • DSP filtering  y = h⊛x  (bilinear): h̄_i = Σ_j ȳ_{i+j} x_j, x̄_j = Σ_i ȳ_{i+j} h_i — "correlation = convolution
//     transpose".
//   • SPLINE control-value solve  u = T⁻¹r  (Thomas): r̄ = T⁻ᵀ·ū — ONE back-solve on the TRANSPOSED tridiagonal
//     (sub/super swapped), the factor-reuse principle (matrix_reverse solve VJP, tridiagonal edition).
//
// Self-contained + allocation-free (caller scratch); verified by the adjoint identity ⟨ȳ, JVP(v)⟩ == ⟨VJP(ȳ), v⟩ vs
// the FD-gated v15-f JVPs, + direct FD + JAX/`jax.numpy.fft` parity. ADR-0097.

#include <crd/hesap/autodiff/suite_jvp.hpp> // thomas_solve (reused) + dft reference

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <complex>

namespace crd::hesap::autodiff::reverse::suite
{

// FFT VJP = the adjoint DFT (unnormalised inverse): X̄_j = Σ_k Ȳ_k · e^{+2πi kj/n}.
inline void dft_vjp(const std::complex<crd::f64>* ybar, std::complex<crd::f64>* xbar, int n) noexcept
{
    const crd::f64 twopi = 6.283185307179586476925286766559; // +sign = conjugate of the forward kernel
    for (int j = 0; j < n; ++j)
    {
        crd::f64 re = 0.0;
        crd::f64 im = 0.0;
        for (int k = 0; k < n; ++k)
        {
            crd::f64       s   = 0.0;
            crd::f64       c   = 0.0;
            const crd::f64 ang = twopi * static_cast<crd::f64>((k * j) % n) / static_cast<crd::f64>(n);
            crd::math::sincos(ang, s, c);
            re += ybar[k].real() * c - ybar[k].imag() * s;
            im += ybar[k].real() * s + ybar[k].imag() * c;
        }
        xbar[j] = {re, im};
    }
}

// Filtering VJP  y = h⊛x (full, length nh+nx−1): h̄_i = Σ_j ȳ_{i+j}·x_j ; x̄_j = Σ_i ȳ_{i+j}·h_i.
inline void conv_vjp(const crd::f64* h, int nh, const crd::f64* x, int nx, const crd::f64* gy, crd::f64* gh,
                     crd::f64* gx) noexcept
{
    for (int i = 0; i < nh; ++i)
    {
        crd::f64 s = 0.0;
        for (int j = 0; j < nx; ++j) { s += gy[i + j] * x[j]; }
        gh[i] = s;
    }
    for (int j = 0; j < nx; ++j)
    {
        crd::f64 s = 0.0;
        for (int i = 0; i < nh; ++i) { s += gy[i + j] * h[i]; }
        gx[j] = s;
    }
}

// Tridiagonal solve VJP  u = T⁻¹r  (T = sub a, diag b, super c) : r̄ = T⁻ᵀ·ū — solve the TRANSPOSE (swap sub/super).
// scratch at,ct (n each: the transposed bands), cp,dp (n each: the Thomas work).
inline void thomas_solve_vjp(const crd::f64* a, const crd::f64* b, const crd::f64* c, const crd::f64* ubar,
                             crd::f64* rbar, int n, crd::f64* at, crd::f64* ct, crd::f64* cp, crd::f64* dp) noexcept
{
    at[0] = 0.0;                                       // Tᵀ sub'[i] = T[i-1][i] = c[i-1]
    for (int i = 1; i < n; ++i) { at[i] = c[i - 1]; }
    for (int i = 0; i < n - 1; ++i) { ct[i] = a[i + 1]; } // Tᵀ super'[i] = T[i+1][i] = a[i+1]
    ct[n - 1] = 0.0;
    forward::suite::thomas_solve(at, b, ct, ubar, rbar, n, cp, dp); // r̄ = Tᵀ⁻¹·ū
}

} // namespace crd::hesap::autodiff::reverse::suite
