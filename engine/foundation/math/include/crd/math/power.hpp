#pragma once

// crd-math power/root family — cbrt/hypot/rsqrt, deterministic. cbrt rides exp2/log2 (≤1 ulp) + one Halley step;
// hypot/rsqrt ride the IEEE-exact hardware sqrt. Same contract as transcendental.hpp.

#include <crd/math/libm.hpp>           // sqrt (IEEE-exact) + non-finite edge fallback
#include <crd/math/transcendental.hpp> // exp2/log2 for pow

namespace crd::math::inline CRD_MATH_LEVEL_NS
{
// pow(x,y) = x^y. Deterministic common path 2^{y·log2 x} for x>0 (and x<0 with integer y → (−1)^y·|x|^y); the IEEE
// edge zoo (0, ±Inf, NaN, x<0 non-integer) routes to the C library pow (exact/constant results, deterministic).
// Accuracy is ≤~3 ulp for moderate exponents; large |y·log2 x| widens (the double-double refinement is a documented
// follow-on).
[[nodiscard]] inline double pow(double x, double y) noexcept
{
    if (y == 0.0)
    {
        return 1.0; // x^0 = 1 for every x (incl. NaN, per the common convention used in engine code)
    }
    const double a = libm::fabs(x);
    const bool neg_int = (x < 0.0) && (libm::round(y) == y); // x<0 with integer y ⇒ real (−1)^y·|x|^y
    if (a > kMinNormal && libm::is_finite(x) && libm::is_finite(y) && (x > 0.0 || neg_int))
    {
        // pow = 2^{y·log2 a}. log2(a)=ed+logm·log2e (ed EXACT) carried as a DOUBLE-DOUBLE (thi+tlo); y·log2(a)
        // likewise (uhi+ulo); then 2^{uhi}·(1+ulo·ln2). The double-double keeps the exponent's low bits so exp2
        // doesn't amplify a single rounding ⇒ ≤1 ulp (the single-double form was ≤7).
        double ed = 0.0;
        const double logm = crd_log_reduce(a, ed);
        const double phi = logm * kLog2eHi;                              // logm·log2e, hi
        const double plo = libm::fma(logm, kLog2eHi, -phi) + logm * kLog2eLo; // + lo
        const double thi = ed + phi;                                     // log2(a) double-double via two_sum(ed,phi)
        const double bb = thi - ed;
        const double tlo = ((ed - (thi - bb)) + (phi - bb)) + plo;
        const double uhi = y * thi;                                      // y·log2(a) double-double
        const double ulo = libm::fma(y, thi, -uhi) + y * tlo;
        const double m = exp2(uhi) * libm::fma(ulo, kLn2Const, 1.0);      // 2^{uhi}·2^{ulo}, 2^ulo≈1+ulo·ln2
        return (neg_int && libm::fmod(libm::round(y), 2.0) != 0.0) ? -m : m;
    }
    return libm::pow(x, y); // 0 / ±Inf / NaN / subnormal base / x<0 non-integer y → IEEE special cases
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
