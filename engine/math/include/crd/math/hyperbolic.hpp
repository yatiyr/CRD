#pragma once

// crd-math hyperbolic family — sinh/cosh/tanh/asinh/acosh/atanh, ≤1 ulp, deterministic. These COMPOSE the ≤1 ulp
// exp/expm1/log/log1p cores (anti-cancellation forms via expm1/log1p so they stay ≤1 ulp near 0) + the IEEE-exact
// hardware sqrt. Same contract as transcendental.hpp; std:: appears only in the non-finite edge fallback + sqrt.

#include <crd/math/transcendental.hpp> // crd::math::exp/expm1/log/log1p (≤1 ulp)

#include <cmath> // sqrt (IEEE-exact, deterministic) + non-finite edge fallback

namespace crd::math
{
// sinh(x): small/moderate via ½·t·(t+2)/(t+1), t=expm1(|x|) (no e^x−e^−x cancellation); |x|>22 via e^{|x|−ln2}
// (= ½e^|x|, but the subtracted ln2 keeps the near-overflow band x∈[709.78,710.5] finite instead of ½·Inf).
[[nodiscard]] inline double sinh(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::sinh(x);
    }
    const double a = std::fabs(x);
    if (a > 22.0)
    {
        return std::copysign(0.5 * exp(a), x); // ½·e^a (0.5 = exact exponent decrement; e^−a negligible)
    }
    const double t = expm1(a);
    return std::copysign(0.5 * t * (t + 2.0) / (t + 1.0), x);
}

// cosh(x): 1 + ½·t²/(1+t), t=expm1(|x|) (accurate near 0); |x|>22 via e^{|x|−ln2}.
[[nodiscard]] inline double cosh(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::cosh(x);
    }
    const double a = std::fabs(x);
    if (a > 22.0)
    {
        return 0.5 * exp(a); // ½·e^a (e^−a negligible)
    }
    const double t = expm1(a);
    return 1.0 + 0.5 * t * t / (1.0 + t);
}

// tanh(x) = sign·t/(t+2), t = expm1(2|x|); saturates to ±1 for large |x|.
[[nodiscard]] inline double tanh(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::tanh(x);
    }
    const double a = std::fabs(x);
    if (a > 20.0)
    {
        return std::copysign(1.0, x); // e^{−40} below 1 ulp of 1
    }
    const double t = expm1(2.0 * a);
    return std::copysign(t / (t + 2.0), x);
}

// asinh(x) = sign·log1p(|x| + x²/(1+√(1+x²)))  ⇒  ≈|x| near 0, ≈log(2|x|) for large |x|.
[[nodiscard]] inline double asinh(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::asinh(x);
    }
    const double a = std::fabs(x);
    const double r = log1p(a + a * a / (1.0 + std::sqrt(1.0 + a * a)));
    return std::copysign(r, x);
}

// acosh(x) = log1p(t + √(t·(t+2))), t = x−1, x ≥ 1  ⇒  accurate near x=1.
[[nodiscard]] inline double acosh(double x) noexcept
{
    if (!(x >= 1.0) || !std::isfinite(x))
    {
        return std::acosh(x); // x<1 / NaN / +Inf → library (NaN / +Inf)
    }
    const double t = x - 1.0;
    return log1p(t + std::sqrt(t * (t + 2.0)));
}

// atanh(x) = ½·(log1p(x) − log1p(−x)), |x| < 1  ⇒  the x² terms cancel ⇒ ≈x near 0, →±Inf at ±1.
[[nodiscard]] inline double atanh(double x) noexcept
{
    if (!(std::fabs(x) < 1.0) || !std::isfinite(x))
    {
        return std::atanh(x); // |x|≥1 / NaN → library (±Inf at ±1, NaN beyond)
    }
    return 0.5 * (log1p(x) - log1p(-x));
}

// f32 overloads through the f64 core
[[nodiscard]] inline float sinh(float x) noexcept { return static_cast<float>(sinh(static_cast<double>(x))); }
[[nodiscard]] inline float cosh(float x) noexcept { return static_cast<float>(cosh(static_cast<double>(x))); }
[[nodiscard]] inline float tanh(float x) noexcept { return static_cast<float>(tanh(static_cast<double>(x))); }
[[nodiscard]] inline float asinh(float x) noexcept { return static_cast<float>(asinh(static_cast<double>(x))); }
[[nodiscard]] inline float acosh(float x) noexcept { return static_cast<float>(acosh(static_cast<double>(x))); }
[[nodiscard]] inline float atanh(float x) noexcept { return static_cast<float>(atanh(static_cast<double>(x))); }

} // namespace crd::math
