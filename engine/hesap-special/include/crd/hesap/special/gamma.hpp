#pragma once

// crd-hesap-special v12-a — the gamma family: the substrate under every distribution.
//
//   gamma / lgamma / gamma_sign  · digamma (ψ) · trigamma (ψ') · polygamma (ψ⁽ⁿ⁾)
//   beta / lbeta
//
// Algorithm: Stirling asymptotic series with up-recurrence (bring the argument to ≥ kStirlingCut, then the
// Bernoulli-coefficient asymptotic series — accurate to ~1e-15/1e-16 in f64 with 5 terms) + reflection for the
// left half-plane. The Bernoulli coefficients are exact rationals so there is no transcription risk. Reference
// (test-side oracle): Boost.Math / scipy.special / std::lgamma/tgamma. Gate ≤ 1e-13 (f64) / 1e-5 (f32).
//
// Determinism: crd::math::exp/log/sin give run-twice + cross-thread bit-identity on a fixed libm (the hesap moat; the
// no-std-math cross-platform guard is scoped to engine/hesap, NOT this sibling — ADR-0063 §2).

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>

namespace crd::hesap::special
{

template <typename T>
concept Real = std::is_floating_point_v<T>;

namespace detail
{
template <Real T>
inline constexpr T kPi = static_cast<T>(3.14159265358979323846264338327950288);
template <Real T>
inline constexpr T kLnSqrt2Pi = static_cast<T>(0.918938533204672741780329736405617639); // ½·ln(2π)

// Argument above which the Stirling asymptotic series is used directly.
template <Real T>
inline constexpr T kStirlingCut = static_cast<T>(12);

// lgamma for x > 0 via up-recurrence + Stirling. No reflection (caller handles x ≤ 0).
// The recurrence multiplies the shift factors into one product (Γ(x)=Γ(x+n)/∏(x+k)) so the whole shift costs a
// SINGLE log instead of n logs — ~2× faster for the small-x bulk, and the same shape the SIMD batch vectorizes.
template <Real T>
[[nodiscard]] T lgamma_pos(T x) noexcept
{
    T prod = static_cast<T>(1);
    while (x < kStirlingCut<T>)
    {
        prod *= x; // lgamma(x) = lgamma(x+1) − log(x) ⇒ subtract log(∏(x+k)) once
        x += static_cast<T>(1);
    }
    const T accum = -crd::math::log(prod);
    const T inv = static_cast<T>(1) / x;
    const T inv2 = inv * inv;
    // Σ B₂ₖ/(2k(2k−1)) x^{1−2k} = 1/(12x) − 1/(360x³) + 1/(1260x⁵) − 1/(1680x⁷) + 1/(1188x⁹)
    const T series =
        inv * (static_cast<T>(1.0 / 12.0) +
               inv2 * (static_cast<T>(-1.0 / 360.0) +
                       inv2 * (static_cast<T>(1.0 / 1260.0) +
                               inv2 * (static_cast<T>(-1.0 / 1680.0) + inv2 * static_cast<T>(1.0 / 1188.0)))));
    const T stirling = (x - static_cast<T>(0.5)) * crd::math::log(x) - x + kLnSqrt2Pi<T> + series;
    return accum + stirling;
}
} // namespace detail

// ln|Γ(x)|.
template <Real T>
[[nodiscard]] T lgamma(T x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    if (x > static_cast<T>(0))
    {
        return detail::lgamma_pos(x);
    }
    // Poles at the non-positive integers.
    if (x == crd::math::floor(x))
    {
        return std::numeric_limits<T>::infinity();
    }
    // Reflection: Γ(x)Γ(1−x) = π/sin(πx) ⇒ ln|Γ(x)| = ln(π/|sin(πx)|) − ln|Γ(1−x)|.
    const T s = std::abs(crd::math::sin(detail::kPi<T> * x));
    return crd::math::log(detail::kPi<T> / s) - detail::lgamma_pos(static_cast<T>(1) - x);
}

// sign of Γ(x): +1 except on (−2k−1, −2k) intervals where Γ is negative.
template <Real T>
[[nodiscard]] T gamma_sign(T x) noexcept
{
    if (x > static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    // Γ(x) < 0 when ⌊x⌋ is even (x in (−1,0): floor=−1 odd → +; x in (−2,−1): floor=−2 even → −; ...).
    const auto fl = static_cast<long long>(crd::math::floor(x));
    return ((fl % 2) == 0) ? static_cast<T>(-1) : static_cast<T>(1);
}

// Γ(x).
template <Real T>
[[nodiscard]] T gamma(T x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    if (x > static_cast<T>(0))
    {
        return crd::math::exp(detail::lgamma_pos(x));
    }
    if (x == crd::math::floor(x))
    {
        return std::numeric_limits<T>::quiet_NaN(); // pole
    }
    // Reflection: Γ(x) = π / (sin(πx)·Γ(1−x)).
    const T s = crd::math::sin(detail::kPi<T> * x);
    return detail::kPi<T> / (s * crd::math::exp(detail::lgamma_pos(static_cast<T>(1) - x)));
}

// Digamma ψ(x) = d/dx ln Γ(x).  Up-recurrence + asymptotic ψ(x) ~ ln x − 1/(2x) − Σ B₂ₖ/(2k x^{2k}).
template <Real T>
[[nodiscard]] T digamma(T x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    T result = static_cast<T>(0);
    // Reflection for x ≤ 0: ψ(1−x) − ψ(x) = π·cot(πx).
    if (x <= static_cast<T>(0))
    {
        if (x == crd::math::floor(x))
        {
            return std::numeric_limits<T>::quiet_NaN();
        }
        const T pix = detail::kPi<T> * x;
        result = -detail::kPi<T> * (crd::math::cos(pix) / crd::math::sin(pix));
        x = static_cast<T>(1) - x;
    }
    while (x < detail::kStirlingCut<T>)
    {
        result -= static_cast<T>(1) / x; // ψ(x) = ψ(x+1) − 1/x
        x += static_cast<T>(1);
    }
    const T inv = static_cast<T>(1) / x;
    const T inv2 = inv * inv;
    // ln x − 1/(2x) − 1/(12x²) + 1/(120x⁴) − 1/(252x⁶) + 1/(240x⁸)
    const T series =
        inv2 * (static_cast<T>(-1.0 / 12.0) +
                inv2 * (static_cast<T>(1.0 / 120.0) +
                        inv2 * (static_cast<T>(-1.0 / 252.0) + inv2 * static_cast<T>(1.0 / 240.0))));
    return result + crd::math::log(x) - static_cast<T>(0.5) * inv + series;
}

// Trigamma ψ'(x).  Up-recurrence + asymptotic ψ'(x) ~ 1/x + 1/(2x²) + Σ B₂ₖ/x^{2k+1}.
template <Real T>
[[nodiscard]] T trigamma(T x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    T result = static_cast<T>(0);
    if (x <= static_cast<T>(0))
    {
        if (x == crd::math::floor(x))
        {
            return std::numeric_limits<T>::infinity();
        }
        // Reflection: ψ'(1−x) + ψ'(x) = π²/sin²(πx).
        const T s = crd::math::sin(detail::kPi<T> * x);
        const T refl = detail::kPi<T> * detail::kPi<T> / (s * s);
        return refl - trigamma(static_cast<T>(1) - x);
    }
    while (x < detail::kStirlingCut<T>)
    {
        result += static_cast<T>(1) / (x * x); // ψ'(x) = ψ'(x+1) + 1/x²
        x += static_cast<T>(1);
    }
    const T inv = static_cast<T>(1) / x;
    const T inv2 = inv * inv;
    // 1/x + 1/(2x²) + 1/(6x³) − 1/(30x⁵) + 1/(42x⁷) − 1/(30x⁹)
    const T series =
        inv * (static_cast<T>(1) +
               inv * (static_cast<T>(0.5) +
                      inv * (static_cast<T>(1.0 / 6.0) +
                             inv2 * (static_cast<T>(-1.0 / 30.0) +
                                     inv2 * (static_cast<T>(1.0 / 42.0) + inv2 * static_cast<T>(-1.0 / 30.0))))));
    return result + series;
}

// Polygamma ψ⁽ⁿ⁾(x) for n ≥ 0 (n=0 → digamma, n=1 → trigamma).  General n via up-recurrence to the asymptotic
// regime + the Euler-Maclaurin tail ψ⁽ⁿ⁾(x) = (−1)^{n+1} n! [ 1/x^{n+1}·(…) ] — implemented through the Hurwitz
// relation ψ⁽ⁿ⁾(x) = (−1)^{n+1} n! ζ(n+1, x) approximated by the standard asymptotic expansion.
template <Real T>
[[nodiscard]] T polygamma(int n, T x) noexcept
{
    if (n == 0)
    {
        return digamma(x);
    }
    if (n == 1)
    {
        return trigamma(x);
    }
    // n ≥ 2. n! and (n−1)!.
    T factn = static_cast<T>(1);
    for (int k = 2; k <= n; ++k)
    {
        factn *= static_cast<T>(k);
    }
    const T factnm1 = factn / static_cast<T>(n);
    // The leading asymptotic sign is (−1)^{n+1}; the down-recurrence correction term
    // ψ⁽ⁿ⁾(x) = ψ⁽ⁿ⁾(x+1) + (−1)^{n+1} n!/x^{n+1} carries the SAME sign.
    const T sign = (n % 2 == 0) ? static_cast<T>(-1) : static_cast<T>(1);
    T accum = static_cast<T>(0);
    const T cut = static_cast<T>(n) + static_cast<T>(12);
    while (x < cut)
    {
        accum += sign * factn / crd::math::pow(x, static_cast<T>(n + 1));
        x += static_cast<T>(1);
    }
    // ψ⁽ⁿ⁾(x) ~ (−1)^{n+1}[ (n−1)!/xⁿ + n!/(2x^{n+1}) + Σ_{k=1..4} B₂ₖ·(2k+n−1)!/(2k)!·1/x^{2k+n} ].
    const T xn = crd::math::pow(x, static_cast<T>(n));
    const T term0 = factnm1 / xn;
    const T term1 = factn / (static_cast<T>(2) * xn * x);
    const T bern[4] = {static_cast<T>(1.0 / 6.0), static_cast<T>(-1.0 / 30.0), static_cast<T>(1.0 / 42.0),
                       static_cast<T>(-1.0 / 30.0)};
    T tail = static_cast<T>(0);
    for (int k = 1; k <= 4; ++k)
    {
        T ratio = static_cast<T>(1); // (2k+n−1)!/(2k)! = ∏_{j=2k+1}^{2k+n−1} j
        for (int j = 2 * k + 1; j <= 2 * k + n - 1; ++j)
        {
            ratio *= static_cast<T>(j);
        }
        tail += bern[k - 1] * ratio / crd::math::pow(x, static_cast<T>(2 * k + n));
    }
    return accum + sign * (term0 + term1 + tail);
}

// ln B(a,b) = lnΓ(a) + lnΓ(b) − lnΓ(a+b).
template <Real T>
[[nodiscard]] T lbeta(T a, T b) noexcept
{
    return lgamma(a) + lgamma(b) - lgamma(a + b);
}

// B(a,b).
template <Real T>
[[nodiscard]] T beta(T a, T b) noexcept
{
    return crd::math::exp(lbeta(a, b));
}

} // namespace crd::hesap::special
