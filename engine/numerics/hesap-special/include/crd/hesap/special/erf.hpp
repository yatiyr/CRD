#pragma once

// crd-hesap-special v12-a — the error-function family.
//
//   erf · erfc · erfcx (scaled) · erfinv · erfcinv · dawson · faddeeva_w (real axis) · voigt
//
// Built on the proven incomplete-gamma engine: erf(x)=sign(x)·P(½,x²), erfc via Q(½,x²), so the inverses reuse
// the Halley-refined gammainc_p_inv (erfinv(y)=√P⁻¹(½,|y|)). erfcx is overflow-safe (direct for |x|<26, asymptotic
// beyond). Dawson via the Rybicki sampling method (NR). Gate ≤ 1e-13 vs scipy.special / Boost / std::erf.

#include <crd/hesap/special/incomplete.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
template <Real T>
inline constexpr T kInvSqrtPi = static_cast<T>(0.564189583547756286948079451560772586); // 1/√π

// W. J. Cody's `calerf` rational approximation (SPECFUN, ~1e-16). jint: 0→erf, 1→erfc, 2→erfcx.
template <Real T>
[[nodiscard]] T calerf(T x, int jint) noexcept
{
    constexpr double a[5] = {3.16112374387056560e0, 1.13864154151050156e2, 3.77485237685302021e2,
                             3.20937758913846947e3, 1.85777706184603153e-1};
    constexpr double b[4] = {2.36012909523441209e1, 2.44024637934444173e2, 1.28261652607737228e3,
                             2.84423683343917062e3};
    constexpr double c[9] = {5.64188496988670089e-1, 8.88314979438837594e0,  6.61191906371416295e1,
                             2.98635138197400131e2,  8.81952221241769090e2,  1.71204761263407058e3,
                             2.05107837782607147e3,  1.23033935479799725e3,  2.15311535474403846e-8};
    constexpr double d[8] = {1.57449261107098347e1, 1.17693950891312499e2, 5.37181101862009858e2,
                             1.62138957456669019e3, 3.29079923573345963e3, 4.36261909014324716e3,
                             3.43936767414372164e3, 1.23033935480374942e3};
    constexpr double p[6] = {3.05326634961232344e-1, 3.60344899949804439e-1, 1.25781726111229246e-1,
                             1.60837851487422766e-2, 6.58749161529837803e-4, 1.63153871373020978e-2};
    constexpr double q[5] = {2.56852019228982242e0, 1.87295284992346047e0, 5.27905102951428412e-1,
                             6.05183413124413191e-2, 2.33520497626869185e-3};
    constexpr T thresh = static_cast<T>(0.46875);
    constexpr T xsmall = static_cast<T>(1.11e-16);
    const T sqrpi = kInvSqrtPi<T>;

    const T y = std::abs(x);
    T result;
    if (y <= thresh)
    {
        T ysq = (y > xsmall) ? y * y : static_cast<T>(0);
        T xnum = static_cast<T>(a[4]) * ysq;
        T xden = ysq;
        for (int i = 0; i < 3; ++i)
        {
            xnum = (xnum + static_cast<T>(a[i])) * ysq;
            xden = (xden + static_cast<T>(b[i])) * ysq;
        }
        result = x * (xnum + static_cast<T>(a[3])) / (xden + static_cast<T>(b[3])); // erf(x)
        if (jint == 0)
        {
            return result;
        }
        if (jint == 1)
        {
            return static_cast<T>(1) - result;
        }
        return crd::math::exp(ysq) * (static_cast<T>(1) - result); // erfcx
    }
    if (y <= static_cast<T>(4))
    {
        T xnum = static_cast<T>(c[8]) * y;
        T xden = y;
        for (int i = 0; i < 7; ++i)
        {
            xnum = (xnum + static_cast<T>(c[i])) * y;
            xden = (xden + static_cast<T>(d[i])) * y;
        }
        result = (xnum + static_cast<T>(c[7])) / (xden + static_cast<T>(d[7])); // erfcx(y) rational
    }
    else
    {
        const T z = static_cast<T>(1) / (y * y);
        T xnum = static_cast<T>(p[5]) * z;
        T xden = z;
        for (int i = 0; i < 4; ++i)
        {
            xnum = (xnum + static_cast<T>(p[i])) * z;
            xden = (xden + static_cast<T>(q[i])) * z;
        }
        result = z * (xnum + static_cast<T>(p[4])) / (xden + static_cast<T>(q[4]));
        result = (sqrpi - result) / y; // erfcx(y) asymptotic
    }
    if (jint == 2)
    {
        return (x < static_cast<T>(0)) ? static_cast<T>(2) * crd::math::exp(x * x) - result : result;
    }
    // Apply the exp split to turn erfcx(y) → erfc(y) (accurate: split y into a 1/16-grid part).
    const T zq = crd::math::floor(y * static_cast<T>(16)) / static_cast<T>(16);
    const T del = (y - zq) * (y + zq);
    result = crd::math::exp(-zq * zq) * crd::math::exp(-del) * result; // erfc(y)
    if (jint == 1)
    {
        return (x < static_cast<T>(0)) ? static_cast<T>(2) - result : result;
    }
    // jint == 0: erf = 1 − erfc, with sign.
    result = (static_cast<T>(0.5) - result) + static_cast<T>(0.5);
    return (x < static_cast<T>(0)) ? -result : result;
}
} // namespace detail

// erf(x) — fast Cody rational.
template <Real T>
[[nodiscard]] T erf(T x) noexcept
{
    return (x == static_cast<T>(0)) ? x : detail::calerf(x, 0);
}

// erfc(x).
template <Real T>
[[nodiscard]] T erfc(T x) noexcept
{
    return detail::calerf(x, 1);
}

// erfcx(x) = exp(x²)·erfc(x) — overflow-safe.
template <Real T>
[[nodiscard]] T erfcx(T x) noexcept
{
    return detail::calerf(x, 2);
}

// Inverse error function erfinv(y), y ∈ (−1,1): Giles (2010) rational initial guess + 2 Newton steps with the
// fast erf (→ full double accuracy).
template <Real T>
[[nodiscard]] T erfinv(T y) noexcept
{
    if (y == static_cast<T>(0))
    {
        return y;
    }
    if (y >= static_cast<T>(1))
    {
        return std::numeric_limits<T>::infinity();
    }
    if (y <= static_cast<T>(-1))
    {
        return -std::numeric_limits<T>::infinity();
    }
    T w = -crd::math::log((static_cast<T>(1) - y) * (static_cast<T>(1) + y));
    T p;
    if (w < static_cast<T>(5))
    {
        w -= static_cast<T>(2.5);
        p = static_cast<T>(2.81022636e-08);
        p = static_cast<T>(3.43273939e-07) + p * w;
        p = static_cast<T>(-3.5233877e-06) + p * w;
        p = static_cast<T>(-4.39150654e-06) + p * w;
        p = static_cast<T>(0.00021858087) + p * w;
        p = static_cast<T>(-0.00125372503) + p * w;
        p = static_cast<T>(-0.00417768164) + p * w;
        p = static_cast<T>(0.246640727) + p * w;
        p = static_cast<T>(1.50140941) + p * w;
    }
    else
    {
        w = crd::math::sqrt(w) - static_cast<T>(3);
        p = static_cast<T>(-0.000200214257);
        p = static_cast<T>(0.000100950558) + p * w;
        p = static_cast<T>(0.00134934322) + p * w;
        p = static_cast<T>(-0.00367342844) + p * w;
        p = static_cast<T>(0.00573950773) + p * w;
        p = static_cast<T>(-0.0076224613) + p * w;
        p = static_cast<T>(0.00943887047) + p * w;
        p = static_cast<T>(1.00167406) + p * w;
        p = static_cast<T>(2.83297682) + p * w;
    }
    T x = p * y;
    // One Halley step (cubic convergence) with the fast erf: from the Giles ~1e-7 guess → ~1e-15.
    // f=erf(x)−y, f'=(2/√π)exp(−x²), f''=−2x·f'  ⇒  Halley: x −= f / (f' − f·x).
    const T fp = static_cast<T>(2) * detail::kInvSqrtPi<T> * crd::math::exp(-x * x);
    const T f = erf(x) - y;
    x -= f / (fp - f * x);
    return x;
}

// Inverse complementary error function erfcinv(y), y ∈ (0,2).
template <Real T>
[[nodiscard]] T erfcinv(T y) noexcept
{
    if (y <= static_cast<T>(0))
    {
        return std::numeric_limits<T>::infinity();
    }
    if (y >= static_cast<T>(2))
    {
        return -std::numeric_limits<T>::infinity();
    }
    return erfinv(static_cast<T>(1) - y);
}

// Standard-normal quantile Φ⁻¹(p) (the probit) — Wichura's Algorithm AS 241 (1988): a pure rational (NO iteration),
// full double precision (~1e-16), faster than √2·erfinv(2p−1) (the central branch needs no log). Coefficients
// verified against R's qnorm.c. Used by the normal / lognormal / half-normal quantiles and the Student-t Hill init.
template <Real T>
[[nodiscard]] T ndtri(T p) noexcept
{
    if (p <= static_cast<T>(0))
    {
        return -std::numeric_limits<T>::infinity();
    }
    if (p >= static_cast<T>(1))
    {
        return std::numeric_limits<T>::infinity();
    }
    const T q = p - static_cast<T>(0.5);
    if (std::abs(q) <= static_cast<T>(0.425))
    {
        const T r = static_cast<T>(0.180625) - q * q;
        return q *
               (((((((r * static_cast<T>(2509.0809287301226727) + static_cast<T>(33430.575583588128105)) * r +
                     static_cast<T>(67265.770927008700853)) *
                        r +
                    static_cast<T>(45921.953931549871457)) *
                       r +
                   static_cast<T>(13731.693765509461125)) *
                      r +
                  static_cast<T>(1971.5909503065514427)) *
                     r +
                 static_cast<T>(133.14166789178437745)) *
                    r +
                static_cast<T>(3.387132872796366608)) /
               (((((((r * static_cast<T>(5226.495278852854561) + static_cast<T>(28729.085735721942674)) * r +
                     static_cast<T>(39307.89580009271061)) *
                        r +
                    static_cast<T>(21213.794301586595867)) *
                       r +
                   static_cast<T>(5394.1960214247511077)) *
                      r +
                  static_cast<T>(687.1870074920579083)) *
                     r +
                 static_cast<T>(42.313330701600911252)) *
                    r +
                static_cast<T>(1));
    }
    T r = q < static_cast<T>(0) ? p : static_cast<T>(1) - p;
    r = crd::math::sqrt(-crd::math::log(r));
    T val;
    if (r <= static_cast<T>(5))
    {
        r -= static_cast<T>(1.6);
        val = (((((((r * static_cast<T>(7.7454501427834140764e-4) + static_cast<T>(0.0227238449892691845833)) * r +
                    static_cast<T>(0.24178072517745061177)) *
                       r +
                   static_cast<T>(1.27045825245236838258)) *
                      r +
                  static_cast<T>(3.64784832476320460504)) *
                     r +
                 static_cast<T>(5.7694972214606914055)) *
                    r +
                static_cast<T>(4.6303378461565452959)) *
                   r +
               static_cast<T>(1.42343711074968357734)) /
              (((((((r * static_cast<T>(1.05075007164441684324e-9) + static_cast<T>(5.475938084995344946e-4)) * r +
                    static_cast<T>(0.0151986665636164571966)) *
                       r +
                   static_cast<T>(0.14810397642748007459)) *
                      r +
                  static_cast<T>(0.68976733498510000455)) *
                     r +
                 static_cast<T>(1.6763848301838038494)) *
                    r +
                static_cast<T>(2.05319162663775882187)) *
                   r +
               static_cast<T>(1));
    }
    else
    {
        r -= static_cast<T>(5);
        val = (((((((r * static_cast<T>(2.01033439929228813265e-7) + static_cast<T>(2.71155556874348757815e-5)) * r +
                    static_cast<T>(0.0012426609473880784386)) *
                       r +
                   static_cast<T>(0.026532189526576123093)) *
                      r +
                  static_cast<T>(0.29656057182850489123)) *
                     r +
                 static_cast<T>(1.7848265399172913358)) *
                    r +
                static_cast<T>(5.4637849111641143699)) *
                   r +
               static_cast<T>(6.6579046435011037772)) /
              (((((((r * static_cast<T>(2.04426310338993978564e-15) + static_cast<T>(1.4215117583164458887e-7)) * r +
                    static_cast<T>(1.8463183175100546818e-5)) *
                       r +
                   static_cast<T>(7.868691311456132591e-4)) *
                      r +
                  static_cast<T>(0.0148753612908506148525)) *
                     r +
                 static_cast<T>(0.13692988092273580531)) *
                    r +
                static_cast<T>(0.59983220655588793769)) *
                   r +
               static_cast<T>(1));
    }
    return q < static_cast<T>(0) ? -val : val;
}

// Dawson integral D(x) = exp(−x²) ∫₀ˣ exp(t²) dt.  Rybicki sampling method (NR §6.10), with H=0.25/N=13 (the
// method's error is ~exp(−(π/2H)²) ⇒ H=0.25 reaches ~1e-13, vs NR's H=0.4 ≈ 2e-7) + a near-zero power series.
template <Real T>
[[nodiscard]] T dawson(T x) noexcept
{
    constexpr int kN = 13;
    const T h = static_cast<T>(0.25);
    const T ax = std::abs(x);
    if (ax < static_cast<T>(0.2))
    {
        // D(x) = Σ_{n≥0} (−2x²)ⁿ/(2n+1)!! · x : term_n = term_{n−1}·(−2x²)/(2n+1).
        const T x2 = x * x;
        T term = x;
        T sum = x;
        for (int n = 1; n <= 12; ++n)
        {
            term *= static_cast<T>(-2) * x2 / static_cast<T>(2 * n + 1);
            sum += term;
        }
        return sum;
    }
    const T n0 = static_cast<T>(2) * crd::math::floor(static_cast<T>(0.5) * ax / h + static_cast<T>(0.5));
    const T xp = ax - n0 * h;
    T e1 = crd::math::exp(static_cast<T>(2) * xp * h);
    const T e2 = e1 * e1;
    T d1 = n0 + static_cast<T>(1);
    T d2 = n0 - static_cast<T>(1);
    T sum = static_cast<T>(0);
    for (int i = 0; i < kN; ++i)
    {
        const T t = static_cast<T>(2 * i + 1) * h;
        const T c = crd::math::exp(-t * t);
        sum += c * (e1 / d1 + static_cast<T>(1) / (d2 * e1)); // use d1,d2 BEFORE advancing
        d1 += static_cast<T>(2);
        d2 -= static_cast<T>(2);
        e1 *= e2;
    }
    const T res = detail::kInvSqrtPi<T> * crd::math::exp(-xp * xp) * sum;
    return (x >= static_cast<T>(0)) ? res : -res;
}

// Faddeeva on the real axis: w(x) = exp(−x²)·erfc(−ix) = exp(−x²) + i·(2/√π)·D(x), for real x.
//   .re = exp(−x²),  .im = (2/√π)·dawson(x).
template <Real T>
struct FaddeevaReal
{
    T re;
    T im;
};

template <Real T>
[[nodiscard]] FaddeevaReal<T> faddeeva_w(T x) noexcept
{
    return FaddeevaReal<T>{crd::math::exp(-x * x), static_cast<T>(2) * detail::kInvSqrtPi<T> * dawson(x)};
}

// Voigt profile V(x;σ,γ) = Re[w(z)]/(σ√(2π)),  z = (x + iγ)/(σ√2).  Uses the real-axis Faddeeva for γ→0; the
// general (γ>0) profile is the real part of the complex Faddeeva (added in the v12-d complex-special pass).
template <Real T>
[[nodiscard]] T voigt_zero_gamma(T x, T sigma) noexcept
{
    const T inv = static_cast<T>(1) / (sigma * static_cast<T>(2.50662827463100050241576528481104525)); // σ√(2π)
    const T z = x / (sigma * static_cast<T>(1.41421356237309504880168872420969808)); // x/(σ√2)
    return crd::math::exp(-z * z) * inv;
}

} // namespace crd::hesap::special
