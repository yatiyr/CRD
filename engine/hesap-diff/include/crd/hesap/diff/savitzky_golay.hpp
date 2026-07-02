#pragma once

// crd-hesap-diff v13-m — SAVITZKY-GOLAY differentiation: fit a degree-p polynomial to a sliding window by least
// squares and read off its k-th derivative at the centre. The robust way to differentiate NOISY sampled data (IMU /
// encoder / telemetry rates) — it smooths and differentiates in one pass, where a bare finite difference amplifies
// noise catastrophically. `savgol_coeffs` matches scipy.signal.savgol_coeffs; `savgol_filter` applies them.
//
// Method (scipy-faithful): positions x_j = j − halflen; A_{k,j} = x_j^k (k=0..p); the min-norm least-squares solution
// of A·c = e_deriv·(deriv!/Δ^deriv) is c = Aᵀ·G⁻¹·y with the small SPD Gram G = A·Aᵀ — solved by a deterministic
// Cholesky. Moat: determinism (crd::math, fixed FP order) + allocation-free (caller-sized output, stack Gram).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::diff
{

constexpr int kSavgolMaxOrder = 16; // polyorder+1 ≤ this (the Gram is (polyorder+1)²)

// Savitzky-Golay filter coefficients (centred window): fits a degree-`polyorder` polynomial to `window` samples and
// returns the convolution kernel that yields the `deriv`-th derivative at the centre, scaled by sample spacing Δ
// (delta). Matches scipy.signal.savgol_coeffs(window, polyorder, deriv, delta, use='dot'). coeffs has length `window`.
// Returns false on bad input (window even / polyorder ≥ window / deriv > polyorder / order too large).
template <typename T>
[[nodiscard]] bool savgol_coeffs(int window, int polyorder, int deriv, T delta, T* coeffs)
{
    if (window < 1 || (window % 2) == 0 || polyorder < 0 || polyorder >= window || deriv < 0 || deriv > polyorder
        || polyorder + 1 > kSavgolMaxOrder)
    {
        return false;
    }
    const int p   = polyorder;
    const int np1 = p + 1;
    const int hl  = window / 2;
    // Gram G = A Aᵀ, gram[k][l] = Σ_j x_j^{k+l}; powers of x_j cached per j.
    T gram[kSavgolMaxOrder * kSavgolMaxOrder] = {};
    for (int j = 0; j < window; ++j)
    {
        const T xj = static_cast<T>(j - hl);
        T       xp[2 * kSavgolMaxOrder];
        xp[0] = T{1};
        for (int e = 1; e <= 2 * p; ++e)
        {
            xp[e] = xp[e - 1] * xj;
        }
        for (int k = 0; k < np1; ++k)
        {
            for (int l = 0; l < np1; ++l)
            {
                gram[k * np1 + l] += xp[k + l];
            }
        }
    }
    // y = e_deriv · (deriv! / Δ^deriv)
    T y[kSavgolMaxOrder] = {};
    T factd              = T{1};
    for (int i = 2; i <= deriv; ++i)
    {
        factd *= static_cast<T>(i);
    }
    T deld = T{1};
    for (int i = 0; i < deriv; ++i)
    {
        deld *= delta;
    }
    y[deriv] = factd / deld;
    // Cholesky solve G z = y (G is SPD).
    T chol[kSavgolMaxOrder * kSavgolMaxOrder] = {};
    for (int i = 0; i < np1; ++i)
    {
        for (int k = 0; k <= i; ++k)
        {
            T s = gram[i * np1 + k];
            for (int m = 0; m < k; ++m)
            {
                s -= chol[i * np1 + m] * chol[k * np1 + m];
            }
            if (i == k)
            {
                chol[i * np1 + k] = crd::math::sqrt(s);
            }
            else
            {
                chol[i * np1 + k] = s / chol[k * np1 + k];
            }
        }
    }
    T z[kSavgolMaxOrder];
    for (int i = 0; i < np1; ++i) // forward solve L w = y
    {
        T s = y[i];
        for (int k = 0; k < i; ++k)
        {
            s -= chol[i * np1 + k] * z[k];
        }
        z[i] = s / chol[i * np1 + i];
    }
    for (int i = np1 - 1; i >= 0; --i) // back solve Lᵀ z = w
    {
        T s = z[i];
        for (int k = i + 1; k < np1; ++k)
        {
            s -= chol[k * np1 + i] * z[k];
        }
        z[i] = s / chol[i * np1 + i];
    }
    // coeffs[j] = Σ_k x_j^k z[k]
    for (int j = 0; j < window; ++j)
    {
        const T xj = static_cast<T>(j - hl);
        T       xpow = T{1};
        T       c    = T{0};
        for (int k = 0; k < np1; ++k)
        {
            c += xpow * z[k];
            xpow *= xj;
        }
        coeffs[j] = c;
    }
    return true;
}

// Apply a Savitzky-Golay filter to `y` (smoothing if deriv=0, differentiation if deriv>0), writing `out` (same
// length). Interior points use the centred kernel; edges mirror-reflect the signal (a robust default). Returns false
// on bad input.
template <typename T>
[[nodiscard]] bool savgol_filter(crd::containers::ConstSpan<T> y, int window, int polyorder, int deriv, T delta,
                                 T* out)
{
    T coeffs[1024];
    if (window > 1024 || !savgol_coeffs<T>(window, polyorder, deriv, delta, coeffs))
    {
        return false;
    }
    const int n  = static_cast<int>(y.size());
    const int hl = window / 2;
    for (int i = 0; i < n; ++i)
    {
        T s = T{0};
        for (int k = 0; k < window; ++k)
        {
            int idx = i + (k - hl);
            // mirror-reflect at the boundaries
            while (idx < 0 || idx >= n)
            {
                if (idx < 0)
                {
                    idx = -idx;
                }
                if (idx >= n)
                {
                    idx = 2 * n - 2 - idx;
                }
            }
            s += coeffs[k] * y[static_cast<crd::usize>(idx)];
        }
        out[i] = s;
    }
    return true;
}

} // namespace crd::hesap::diff
