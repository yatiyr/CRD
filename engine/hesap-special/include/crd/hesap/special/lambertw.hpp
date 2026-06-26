#pragma once

// crd-hesap-special v12-d — Lambert W: solve w·e^w = x. Principal branch W₀ (x ≥ −1/e) and W₋₁ (x ∈ [−1/e, 0)).
// Branch-aware initial guess (series near the −1/e branch point, asymptotic for large x) + Halley iteration to
// machine precision. Gated vs scipy.special.lambertw(x, k).real to <1e-13.

#include <crd/hesap/special/gamma.hpp>     // Real concept
#include <crd/hesap/special/poly_eval.hpp> // detail::horner_t

#include "lambertw_poly.inc" // GENERATED iteration-free W0 minimax rationals (gen_lambertw_poly.py; v12-d perf)

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kInvE = -0.367879441171442321595523770161460867; // −1/e
inline constexpr double kE = 2.718281828459045235360287471352662498;

// Halley step (exp-based) — used only near the branch point where Fritsch's 1+w denominator vanishes.
[[nodiscard]] inline double lambertw_halley(double w, double x) noexcept
{
    for (int it = 0; it < 12; ++it)
    {
        const double ew = crd::math::exp(w);
        const double f = w * ew - x;
        const double wp1 = w + 1.0;
        const double denom = ew * wp1 - (w + 2.0) * f / (2.0 * wp1);
        const double dw = f / denom;
        w -= dw;
        if (crd::math::fabs(dw) <= 1e-14 * (1.0 + crd::math::fabs(w)))
        {
            break;
        }
    }
    return w;
}

// Fritsch-Shafer-Crowley 4th-order update: from a ~1e-3 seed, ONE step reaches machine precision (this is what makes
// the iteration-free libraries fast — 4th order + a `log` instead of an `exp`). Valid away from w=−1 (x not at −1/e).
[[nodiscard]] inline double lambertw_fritsch(double w, double x) noexcept
{
    for (int it = 0; it < 4; ++it)
    {
        const double z = crd::math::log(x / w) - w; // residual w·e^w = x  ⇔  ln(x/w) = w
        const double wp1 = 1.0 + w;
        const double q = 2.0 * wp1 * (wp1 + (2.0 / 3.0) * z);
        const double eps = (z / wp1) * (q - z) / (q - 2.0 * z);
        const double dw = w * eps;
        w += dw;
        if (crd::math::fabs(dw) <= 2e-15 * (1.0 + crd::math::fabs(w)))
        {
            break;
        }
    }
    return w;
}

[[nodiscard]] inline double lambertw0_impl(double x) noexcept
{
    if (x < kInvE)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (x == 0.0)
    {
        return 0.0;
    }
    if (x < -0.3) // near the branch point: w = −1 + p − p²/3 + 11p³/72, p = √(2(ex+1)); Fritsch is singular here.
    {
        const double p = crd::math::sqrt(2.0 * (kE * x + 1.0));
        return lambertw_halley(-1.0 + p - p * p / 3.0 + 11.0 * p * p * p / 72.0, x);
    }
    double w;
    if (x < 0.2) // Taylor: W₀ = x − x² + 3/2 x³ − 8/3 x⁴ + … (radius 1/e); ~1e-4 here
    {
        w = x * (1.0 + x * (-1.0 + x * (1.5 + x * (-8.0 / 3.0 + x * (125.0 / 24.0)))));
    }
    else // seed via the iterated logarithm (Barry-Culham), ~1e-2 over x≥0.2 ⇒ Fritsch in 1–2 steps
    {
        const double l1 = crd::math::log(x + 1.0);
        const double l2 = crd::math::log(l1 + 1.0);
        w = l1 * (1.0 - l2 / (2.0 + l1));
    }
    return lambertw_fritsch(w, x);
}

[[nodiscard]] inline double lambertwm1_impl(double x) noexcept
{
    if (x < kInvE || x >= 0.0)
    {
        return (x == 0.0) ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::quiet_NaN();
    }
    double w;
    if (x < -0.3) // near branch point, negative branch: p < 0
    {
        const double p = -crd::math::sqrt(2.0 * (kE * x + 1.0));
        w = -1.0 + p - p * p / 3.0 + 11.0 * p * p * p / 72.0;
    }
    else // x → 0⁻: W₋₁ ≈ ln(−x) − ln(−ln(−x))
    {
        const double l1 = crd::math::log(-x);
        const double l2 = crd::math::log(-l1);
        w = l1 - l2 + l2 / l1;
    }
    return lambertw_halley(w, x);
}
} // namespace detail

template <Real T>
[[nodiscard]] T lambert_w0(T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (xd >= 0.3 && xd <= 20.0) // iteration-free 3-piece rational over the common range (v12-d perf)
    {
        if (xd <= 1.5)
        {
            const double t = (2.0 * xd - 1.8) / 1.2;
            return static_cast<T>(detail::horner_t(detail::kW0R1P, t) / detail::horner_t(detail::kW0R1Q, t));
        }
        if (xd <= 5.0)
        {
            const double t = (2.0 * xd - 6.5) / 3.5;
            return static_cast<T>(detail::horner_t(detail::kW0R2P, t) / detail::horner_t(detail::kW0R2Q, t));
        }
        const double t = (2.0 * xd - 25.0) / 15.0;
        return static_cast<T>(detail::horner_t(detail::kW0R3P, t) / detail::horner_t(detail::kW0R3Q, t));
    }
    return static_cast<T>(detail::lambertw0_impl(xd)); // branch point, x<0.3, x>20: the Fritsch/Halley iteration
}
template <Real T>
[[nodiscard]] T lambert_wm1(T x) noexcept
{
    return static_cast<T>(detail::lambertwm1_impl(static_cast<double>(x)));
}

} // namespace crd::hesap::special
