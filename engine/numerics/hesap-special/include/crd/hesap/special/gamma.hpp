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
// no-std-math cross-platform guard is scoped to engine/numerics/hesap, NOT this sibling — ADR-0063 §2).

#include <crd/core/platform.hpp>
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
[[nodiscard]] CRD_FORCEINLINE T lgamma_shifted(T x, T prod) noexcept
{
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

// The same from x < cut: shifted up past the cut first, the product of the shifts carried to lgamma_shifted.
template <Real T>
[[nodiscard]] T lgamma_pos_from(T x, T cut) noexcept
{
    T prod = static_cast<T>(1);
    while (x < cut)
    {
        prod *= x; // lgamma(x) = lgamma(x+1) − log(x) ⇒ subtract log(∏(x+k)) once
        x += static_cast<T>(1);
    }
    return lgamma_shifted(x, prod);
}

// lgamma(x) for x > 0. In single precision the shifted Stirling term, (x−½)·ln x − x at x ≈ 12, is 17 to 30 and the few
// ulps of its rounding (up to 3e-6) outweigh ln Γ(x) near its zeros at 1 and 2 and for small x, where the result is of
// order one and gammainc_q = 1 − P below a+1 reads it with a gain of 1/Q. Below the cut the whole evaluation is done in
// double, with the argument shifted only up to 8 (the series' next term is 2e-13 there, far below single precision, and
// the shorter recurrence makes it faster than single precision shifted to 12), and rounded once; from the cut on
// nothing cancels and single precision is exact to an ulp.
template <Real T>
[[nodiscard]] T lgamma_pos(T x) noexcept
{
    if constexpr (sizeof(T) < sizeof(double))
    {
        // One test of the cut serves both sides: below it the double evaluation, from it no shift at all.
        if (x < kStirlingCut<T>)
        {
            return static_cast<T>(lgamma_pos_from<double>(static_cast<double>(x), 8.0));
        }
        return lgamma_shifted(x, static_cast<T>(1));
    }
    else
    {
        return lgamma_pos_from(x, kStirlingCut<T>);
    }
}

// Smallest argument from which the Stirling correction below is exact to the working precision (its first omitted
// term is 1.9e-3/z^11, DLMF 5.11.1).
template <Real T>
inline constexpr T kStirlingMin = static_cast<T>(sizeof(T) > sizeof(float) ? 15 : 8);

// ln Γ(z) − [(z−½)·ln z − z + ½·ln 2π] = Σ B_{2k}/(2k(2k−1)) z^{1−2k} (DLMF 5.11.1), the first five terms.
template <Real T>
[[nodiscard]] T stirling_correction(T z) noexcept
{
    const T inv = static_cast<T>(1) / z;
    const T inv2 = inv * inv;
    return inv * (static_cast<T>(1.0 / 12.0) +
                  inv2 * (static_cast<T>(-1.0 / 360.0) +
                          inv2 * (static_cast<T>(1.0 / 1260.0) +
                                  inv2 * (static_cast<T>(-1.0 / 1680.0) + inv2 * static_cast<T>(1.0 / 1188.0)))));
}

// ln Γ(1+m) = −γ·m + Σ_{k≥2} (−1)^k ζ(k)·m^k/k (DLMF 5.7.3) for 0 < m ≤ 1/20, where twelve terms reach 4e-19. The
// shifted Stirling evaluation of lgamma_pos adds terms of size 20 and carries an absolute error of a few 1e-15, which
// for the result of a small shape, ln Γ(m) = ln Γ(1+m) − ln m = 9.2 at m = 1e-4, is 4 ulps.
template <Real T>
[[nodiscard]] T lgamma_1p_small(T m) noexcept
{
    constexpr double zeta_over_k[12] = {1.6449340668482264 / 2,  1.2020569031595943 / 3, 1.0823232337111382 / 4,
                                        1.0369277551433699 / 5,  1.0173430619844491 / 6, 1.0083492773819228 / 7,
                                        1.0040773561979443 / 8,  1.0020083928260822 / 9, 1.0009945751278181 / 10,
                                        1.0004941886041195 / 11, 1.000246086553308 / 12, 1.0001227133475785 / 13};
    const T u = -m;
    T acc = static_cast<T>(0);
    for (int k = 13; k >= 2; --k)
    {
        acc = acc * u + static_cast<T>(zeta_over_k[k - 2]);
    }
    return static_cast<T>(-0.57721566490153286) * m + u * u * acc;
}

// ln B(a,b) for 0 < small ≤ big, big ≥ kStirlingMin, without the cancellation of lnΓ(a) + lnΓ(b) − lnΓ(a+b), whose
// terms are of size s·ln s (s = a+b) and whose rounding (a few ulps of that) is amplified by the exponential of a
// prefactor. Stirling's formula (DLMF 5.11.3, with the correction S above) for every gamma function that is large
// gives, with both shapes large,
//   ln B = ½·ln 2π − a·ln(1+b/a) − b·ln(1+a/b) − ½·ln(a·b/s) + S(a) + S(b) − S(s),
// whose terms are of size s only, and with one small shape m and the other, L, large (lnΓ(m) kept, the ratio of the
// other two expanded: lnΓ(L+m) − lnΓ(L) = m·ln L + (L+m−½)·ln(1+m/L) − m + S(L+m) − S(L))
//   ln B = lnΓ(m) − m·ln L − (L+m−½)·ln(1+m/L) + m − S(L+m) + S(L).
template <Real T>
[[nodiscard]] T lbeta_stirling(T small, T big) noexcept
{
    if (small >= kStirlingMin<T>)
    {
        const T s = small + big;
        return kLnSqrt2Pi<T> - small * crd::math::log1p(big / small) - big * crd::math::log1p(small / big) -
               static_cast<T>(0.5) * crd::math::log(small * (big / s)) + stirling_correction(small) +
               stirling_correction(big) - stirling_correction(s);
    }
    const T ln_gamma_small =
        (small <= static_cast<T>(0.05)) ? lgamma_1p_small(small) - crd::math::log(small) : lgamma_pos(small);
    return ln_gamma_small - small * crd::math::log(big) -
           (big + small - static_cast<T>(0.5)) * crd::math::log1p(small / big) + small -
           stirling_correction(big + small) + stirling_correction(big);
}
} // namespace detail

// ln|Γ(x)|: +inf at the poles (the non-positive integers) and at both infinities.
template <Real T>
[[nodiscard]] T lgamma(T x) noexcept
{
    if (std::isnan(x) || x == std::numeric_limits<T>::infinity())
    {
        return x;
    }
    if (x > static_cast<T>(0))
    {
        return detail::lgamma_pos(x);
    }
    // Poles at the non-positive integers (and -inf).
    if (x == crd::math::floor(x))
    {
        return std::numeric_limits<T>::infinity();
    }
    // Reflection: Γ(x)Γ(1−x) = π/sin(πx) ⇒ ln|Γ(x)| = ln(π/|sin(πx)|) − ln|Γ(1−x)|.
    const T s = std::abs(crd::math::sin(detail::kPi<T> * x));
    return crd::math::log(detail::kPi<T> / s) - detail::lgamma_pos(static_cast<T>(1) - x);
}

// The sign of Γ(x) as gamma() returns it: +1 for x > 0 (and +inf), −1 on the intervals (−1, 0), (−3, −2),
// (−5, −4), ... where Γ is negative, +1 on (−2, −1), (−4, −3), ...; at the poles the sign of gamma()'s value: +1 at +0
// and −1 at −0 (Γ(±0) = ±inf), NaN at the negative integers and −inf (where gamma() is NaN, the two one-sided limits
// having opposite signs) and for a NaN. So gamma_sign(x) · exp(lgamma(x)) has gamma(x)'s sign everywhere. Every
// float of magnitude 2^(digits − 1) or more is an integer, so a negative one is a pole; every other x converts to a
// long long, so the conversion below is defined for each input that reaches it (the earlier conversion of floor(x)
// was not for a NaN, an infinity or a magnitude of 2^63 and more).
template <Real T>
[[nodiscard]] T gamma_sign(T x) noexcept
{
    if (x > static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    constexpr T integral_from = static_cast<T>(1ULL << (std::numeric_limits<T>::digits - 1));
    // false for -inf, a NaN and the negative integers of magnitude 2^(digits - 1) and more
    if (!(crd::math::abs(x) < integral_from)) [[unlikely]]
    {
        return std::numeric_limits<T>::quiet_NaN();
    }
    const auto whole = static_cast<long long>(x); // toward zero, so whole >= x
    if (x == static_cast<T>(whole)) [[unlikely]]
    {
        return x == static_cast<T>(0) ? crd::math::copysign(static_cast<T>(1), x) : std::numeric_limits<T>::quiet_NaN();
    }
    // Γ(x) < 0 when ⌊x⌋ is odd (x in (−1,0): floor=−1 odd → −, Γ(−½) = −2√π; x in (−2,−1): floor=−2 even → +,
    // Γ(−3/2) = 4√π/3; ...). Here x lies in (whole − 1, whole), so ⌊x⌋ = whole − 1, odd exactly when whole is even.
    return ((whole & 1) == 0) ? static_cast<T>(-1) : static_cast<T>(1);
}

// Γ(x): ±inf at ±0, +inf at +inf, NaN at the negative integers and -inf (C's tgamma).
template <Real T>
[[nodiscard]] T gamma(T x) noexcept
{
    if (std::isnan(x) || x == std::numeric_limits<T>::infinity())
    {
        return x;
    }
    if (x == static_cast<T>(0))
    {
        return crd::math::copysign(std::numeric_limits<T>::infinity(), x); // the pole keeps the zero's sign
    }
    if (x > static_cast<T>(0))
    {
        return crd::math::exp(detail::lgamma_pos(x));
    }
    if (x == crd::math::floor(x))
    {
        return std::numeric_limits<T>::quiet_NaN(); // pole at −1, −2, …
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

namespace detail
{
// ln B(a,b) for a shape of at least kStirlingMin: the Stirling form for 0 < the smaller and a finite larger shape (in
// double for a single-precision pair), the sum of three lgamma terms for the rest of the plane. Out of line, so that
// lbeta of the small bulk stays as short as the sum it falls through to.
template <Real T>
CRD_NOINLINE T lbeta_large(T a, T b) noexcept
{
    const T big = (a < b) ? b : a;
    const T small = (a < b) ? a : b;
    if (small > static_cast<T>(0) && big >= kStirlingMin<T> && big < std::numeric_limits<T>::infinity())
    {
        if constexpr (sizeof(T) < sizeof(double))
        {
            return static_cast<T>(lbeta_stirling<double>(static_cast<double>(small), static_cast<double>(big)));
        }
        else
        {
            return lbeta_stirling(small, big);
        }
    }
    return lgamma(a) + lgamma(b) - lgamma(a + b);
}
} // namespace detail

// ln B(a,b) = lnΓ(a) + lnΓ(b) − lnΓ(a+b); from a shape of kStirlingMin on (and 0 < the other) the three terms would
// cancel to a result of size s, so the Stirling form of detail::lbeta_stirling replaces them (in double for a single
// precision pair, whose float terms of size 144 leave an absolute error of 1e-5 on a result of 7). Forced inline, which
// keeps the call of the small bulk as short as the sum it was (a call of lbeta cost it 3 %).
template <Real T>
[[nodiscard]] CRD_FORCEINLINE T lbeta(T a, T b) noexcept
{
    // Either shape large: two comparisons joined without a branch, one branch (not taken by the small bulk) after them.
    if ((a >= detail::kStirlingMin<T>) | (b >= detail::kStirlingMin<T>))
    {
        return detail::lbeta_large(a, b);
    }
    return lgamma(a) + lgamma(b) - lgamma(a + b);
}

// B(a,b).
template <Real T>
[[nodiscard]] T beta(T a, T b) noexcept
{
    return crd::math::exp(lbeta(a, b));
}

} // namespace crd::hesap::special
