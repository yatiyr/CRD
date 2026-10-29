#pragma once

// crd-math power/root family — pow/cbrt/hypot/rsqrt, deterministic. pow rides the log2 and exp2 pair cores
// (exp_log_core.hpp), cbrt Halley steps, hypot/rsqrt the IEEE-exact hardware sqrt. Same contract as
// transcendental.hpp.

#include <crd/math/libm.hpp>           // sqrt (IEEE-exact) + non-finite edge fallback
#include <crd/math/transcendental.hpp> // the exp/log cores

namespace crd::math::inline CRD_MATH_LEVEL_NS
{
// pow(x,y) = x^y = 2^{y·log2 x} for finite x > 0 (and x < 0 with integer y → (−1)^y·|x|^y): ≤0.58 ulp for a normal
// result, ≤0.76 for a subnormal one (its scale rounds a second time), against mpmath. log2|x| comes as a pair to
// 2^-69 and y·log2|x| as a pair; its head alone decides overflow (≥ 1025 → ∞) and underflow (< −1076 → 0) before the
// tail is formed, so a product that overflows never reaches it. The IEEE edge cases (0, ±Inf, NaN, x < 0 with
// non-integer y) route to the C library pow (exact or constant results, deterministic).
[[nodiscard]] inline double pow(double x, double y) noexcept
{
    if (y == 0.0)
    {
        return 1.0; // x^0 = 1 for every x (incl. NaN, per the common convention used in engine code)
    }
    const double a = libm::fabs(x);
    const bool neg_int = (x < 0.0) && (libm::round(y) == y); // x<0 with integer y ⇒ real (−1)^y·|x|^y
    if (a > 0.0 && libm::is_finite(x) && libm::is_finite(y) && (x > 0.0 || neg_int))
    {
        double ll = 0.0;
        const double lh = crd_log2_pair(a, ll);
        const double uh = y * lh;
        double m = 0.0;
        if (uh >= 1025.0)
        {
            m = __builtin_bit_cast(double, 0x7FF0000000000000ULL); // +infinity
        }
        else if (uh >= -1076.0)
        {
            m = crd_exp2_pair(uh, libm::fma(y, lh, -uh) + y * ll);
        }
        return (neg_int && libm::fmod(libm::round(y), 2.0) != 0.0) ? -m : m; // an even y for every |y| ≥ 2^53
    }
    return libm::pow(x, y); // 0 / ±Inf / NaN / x<0 non-integer y → IEEE special cases
}
[[nodiscard]] inline float pow(float x, float y) noexcept
{
    return static_cast<float>(pow(static_cast<double>(x), static_cast<double>(y)));
}
// cbrt(x) = sign·∛|x|. Fast bit-trick seed (exponent/3 + bias) ⇒ ~6-bit ∛, then Halley steps t·(t³+2a)/(2t³+a)
// (cubic) to ≤1 ulp — no transcendentals (the exp2/log2 route was 3× slower than libm). Subnormal → library (rare).
[[nodiscard]] inline double cbrt(double x) noexcept
{
    if (!libm::is_finite(x) || x == 0.0)
    {
        return libm::cbrt(x); // 0 / ±Inf / NaN → exact
    }
    const double a = libm::fabs(x);
    if (a < 2.2250738585072014e-308) // subnormal |x| → library (rare; bit-trick assumes a normal exponent)
    {
        return libm::copysign(libm::cbrt(a), x);
    }
    const crd::u64 ix = (__builtin_bit_cast(crd::u64, a) / 3) + 0x2A9F7893BFD96AB7ULL; // seed
    double t = __builtin_bit_cast(double, ix);
    for (int i = 0; i < 3; ++i) // Halley, cubic: ~6 → 18 → 54 → full
    {
        const double t3 = t * t * t;
        t = t * (t3 + a + a) / (t3 + t3 + a);
    }
    return libm::copysign(t, x);
}

// hypot(x,y) = √(x²+y²), scaled by the larger magnitude ⇒ no spurious overflow/underflow.
[[nodiscard]] inline double hypot(double x, double y) noexcept
{
    const double ax = libm::fabs(x);
    const double ay = libm::fabs(y);
    if (!libm::is_finite(ax) || !libm::is_finite(ay))
    {
        return libm::hypot(x, y); // ±Inf → +Inf (even if the other is NaN); NaN otherwise — IEEE semantics
    }
    const double hi = ax > ay ? ax : ay;
    const double lo = ax > ay ? ay : ax;
    if (hi == 0.0)
    {
        return 0.0;
    }
    const double r = lo / hi;
    return hi * libm::sqrt(1.0 + r * r);
}

// rsqrt(x) = 1/√x. Hardware sqrt (IEEE-exact) + reciprocal ⇒ ≤1 ulp, deterministic.
[[nodiscard]] inline double rsqrt(double x) noexcept { return 1.0 / libm::sqrt(x); }

[[nodiscard]] inline float cbrt(float x) noexcept { return static_cast<float>(cbrt(static_cast<double>(x))); }
[[nodiscard]] inline float hypot(float x, float y) noexcept
{
    return static_cast<float>(hypot(static_cast<double>(x), static_cast<double>(y)));
}
[[nodiscard]] inline float rsqrt(float x) noexcept { return 1.0F / libm::sqrt(x); }

} // namespace crd::math::inline CRD_MATH_LEVEL_NS
