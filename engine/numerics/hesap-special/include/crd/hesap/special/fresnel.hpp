#pragma once

// crd-hesap-special v12-d — Fresnel integrals S(x)=∫₀ˣ sin(πt²/2)dt, C(x)=∫₀ˣ cos(πt²/2)dt: the Maclaurin series
// (DLMF §7.6(i)) for |x| ≤ 1.5, the complex continued fraction of erfc (DLMF §7.9 with §7.2(iii)) for larger |x|,
// summed from its convergents' successive differences. Gated vs scipy.special.fresnel to <1e-13 (to |x| ≈ 5·10⁴;
// beyond it scipy's own phase πx²/2 is rounded and loses digits, the phase here is reduced from x² mod 4). S, C → ±½
// at ±∞.

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
inline void fresnel_impl(double x, double& s, double& c) noexcept
{
    constexpr double eps = 1.0e-16;
    constexpr int maxit = 100;
    constexpr double fpmin = 1.0e-300;
    constexpr double xmin = 1.5;
    constexpr double kPhaseSplit = 256.0; // beyond this the phase is formed from x² mod 4 (see the continued fraction)
    constexpr double piby2 = 1.5707963267948966192313216916397514;
    const double ax = crd::math::fabs(x);
    if (ax < crd::math::sqrt(fpmin))
    {
        s = 0.0;
        c = ax;
    }
    else if (ax <= xmin)
    {
        // With y = πx²/2:  C = x·Σ (−1)^j y^{2j}/((2j)!(4j+1)),  S = x·Σ (−1)^j y^{2j+1}/((2j+1)!(4j+3)).
        const double y = piby2 * ax * ax;
        const double y2 = y * y;
        double even = 1.0; // (−1)^j y^{2j}/(2j)!
        double cos_sum = 1.0;
        for (int j = 1; j <= maxit; ++j)
        {
            even *= -y2 / (static_cast<double>(2 * j - 1) * (2 * j));
            const double part = even / (4 * j + 1);
            cos_sum += part;
            if (crd::math::fabs(part) <= eps * crd::math::fabs(cos_sum))
            {
                break;
            }
        }
        double odd = y; // (−1)^j y^{2j+1}/(2j+1)!
        double sin_sum = y / 3.0;
        for (int j = 1; j <= maxit; ++j)
        {
            odd *= -y2 / (static_cast<double>(2 * j) * (2 * j + 1));
            const double part = odd / (4 * j + 3);
            sin_sum += part;
            if (crd::math::fabs(part) <= eps * crd::math::fabs(sin_sum))
            {
                break;
            }
        }
        c = ax * cos_sum;
        s = ax * sin_sum;
    }
    else if (ax > 1.0e154)
    {
        // The correction e^{iπx²/2}(1−i)xK of the fraction has modulus ≈ √2/(πx) < 10⁻¹⁵⁴ (and πx² overflows from
        // 7.5·10¹⁵³): C = S = ½ (sign applied below), also at ∞.
        s = 0.5;
        c = 0.5;
    }
    else
    {
        // C + iS = ((1+i)/2)·(1 − e^{iπx²/2}·(1−i)x·K), K = 1/(β₁ + α₂/(β₂ + …)), β_k = 4k−3 − iπx², α_k =
        // −(2k−3)(2k−2) (the contracted erfc fraction at z = (√π/2)(1−i)x). K is summed from its convergents'
        // differences Δ_k = −α_k·(q_{k−2}/q_k)·Δ_{k−1} (Δ₁ = 1/β₁, q_k = β_k·q_{k−1} + α_k·q_{k−2}), all complex, one
        // division a step.
        const double pix2 = 3.14159265358979323846264338327950288 * ax * ax;
        double lag_re = 1.0;
        double lag_im = 0.0;
        double now_re = 1.0;
        double now_im = -pix2;
        const double inv_p = 1.0 / pix2; // 1/β₁ = (1 + iπx²)/(1 + (πx²)²), πx² > 7
        const double norm1 = 1.0 / (1.0 + inv_p * inv_p);
        double d_re = inv_p * inv_p * norm1;
        double d_im = inv_p * norm1;
        double k_re = d_re;
        double k_im = d_im;
        double beta_re = 1.0;
        // for πx² beyond 1e100 the second term is below 1e-200 of the first (and q would overflow): β₁ alone is exact
        const int last = (pix2 < 1.0e100) ? maxit : 1;
        for (int k = 2; k <= last; ++k)
        {
            beta_re += 4.0;
            const double alpha = -static_cast<double>((2 * k - 3) * (2 * k - 2));
            const double next_re = beta_re * now_re + pix2 * now_im + alpha * lag_re;
            const double next_im = beta_re * now_im - pix2 * now_re + alpha * lag_im;
            const double weight = -alpha / (next_re * next_re + next_im * next_im);
            const double ratio_re = (lag_re * next_re + lag_im * next_im) * weight; // −α·q_{k−2}/q_k
            const double ratio_im = (lag_im * next_re - lag_re * next_im) * weight;
            const double step_re = d_re * ratio_re - d_im * ratio_im;
            d_im = d_re * ratio_im + d_im * ratio_re;
            d_re = step_re;
            k_re += d_re;
            k_im += d_im;
            if (crd::math::fabs(d_re) + crd::math::fabs(d_im) < eps * (crd::math::fabs(k_re) + crd::math::fabs(k_im)))
            {
                break;
            }
            lag_re = now_re;
            lag_im = now_im;
            now_re = next_re;
            now_im = next_im;
            if (crd::math::fabs(now_re) + crd::math::fabs(now_im) > 1.0e100) // only the q ratios enter: rescale
            {
                now_re *= 1.0e-100;
                now_im *= 1.0e-100;
                lag_re *= 1.0e-100;
                lag_im *= 1.0e-100;
            }
        }
        // E = e^{iπx²/2}·(1−i)x·K
        const double hr = ax * (k_re + k_im);
        const double hi = ax * (k_im - k_re);
        // The phase πx²/2 is needed to a fraction of a radian however large x² is, and a rounded πx² carries an error
        // of 1.1·10⁻¹⁶·πx²: beyond kPhaseSplit that error shows in the result (about 10⁻¹⁶·x). Since the phase is
        // (π/2)·x², only x² mod 4 matters. x² = sq + err exactly (Dekker's product: ax split into two 26-bit halves
        // by Veltkamp's 2²⁷+1 multiplier), and sq − 4·n for the integer n nearest sq/4 (rounded by adding and removing
        // 2⁵², exact beyond it too: a rounding error there is again an integer) is an exact subtraction of numbers of
        // the same binade, in (−2, 2].
        double phase = 0.5 * pix2;
        if (ax > kPhaseSplit)
        {
            const double sq = ax * ax;
            const double split = 134217729.0 * ax;
            const double high = split - (split - ax);
            const double low = ax - high;
            const double err = ((high * high - sq) + 2.0 * high * low) + low * low;
            const double whole = (0.25 * sq + 4503599627370496.0) - 4503599627370496.0;
            phase = piby2 * ((sq - 4.0 * whole) + err);
        }
        double sn;
        double cs;
        crd::math::sincos(phase, sn, cs);
        const double e_re = cs * hr - sn * hi;
        const double e_im = cs * hi + sn * hr;
        c = 0.5 * (1.0 - e_re + e_im);
        s = 0.5 * (1.0 - e_re - e_im);
    }
    if (x < 0.0)
    {
        c = -c;
        s = -s;
    }
}
} // namespace detail

template <Real T>
[[nodiscard]] T fresnel_s(T x) noexcept
{
    double s = 0.0;
    double c = 0.0;
    detail::fresnel_impl(static_cast<double>(x), s, c);
    return static_cast<T>(s);
}
template <Real T>
[[nodiscard]] T fresnel_c(T x) noexcept
{
    double s = 0.0;
    double c = 0.0;
    detail::fresnel_impl(static_cast<double>(x), s, c);
    return static_cast<T>(c);
}

} // namespace crd::hesap::special
