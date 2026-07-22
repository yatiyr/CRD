#pragma once

// crd-math select/round tier — EXACT primitives (hardware / bit-ops), deterministic by construction. The unified
// surface so engine code calls crd::math::{min,max,clamp,abs,floor,ceil,round,trunc,sqrt,copysign,fma,sign,lerp,
// saturate} instead of std:: (the guard target). No approximation ⇒ no ulp gate; correctness is exactness.

#include <crd/core/types.hpp>

#include <bit>
#include <cmath>
#include <limits>

namespace crd::math
{
template <class T>
[[nodiscard]] constexpr T min(T a, T b) noexcept
{
    return b < a ? b : a;
}
template <class T>
[[nodiscard]] constexpr T max(T a, T b) noexcept
{
    return a < b ? b : a;
}
template <class T>
[[nodiscard]] constexpr T clamp(T v, T lo, T hi) noexcept
{
    return min(max(v, lo), hi);
}
template <class T>
[[nodiscard]] constexpr T sign(T x) noexcept
{
    return static_cast<T>((T(0) < x) - (x < T(0))); // −1 / 0 / +1 (0 for NaN/±0)
}
template <class T>
[[nodiscard]] constexpr T saturate(T x) noexcept
{
    return clamp(x, T(0), T(1));
}

[[nodiscard]] inline double abs(double x) noexcept { return std::fabs(x); }
[[nodiscard]] inline float abs(float x) noexcept { return std::fabs(x); }
[[nodiscard]] inline double fabs(double x) noexcept { return std::fabs(x); }
[[nodiscard]] inline float fabs(float x) noexcept { return std::fabs(x); }
[[nodiscard]] inline double floor(double x) noexcept { return std::floor(x); }
[[nodiscard]] inline float floor(float x) noexcept { return std::floor(x); }
[[nodiscard]] inline double ceil(double x) noexcept { return std::ceil(x); }
[[nodiscard]] inline float ceil(float x) noexcept { return std::ceil(x); }
[[nodiscard]] inline double round(double x) noexcept { return std::round(x); }
[[nodiscard]] inline float round(float x) noexcept { return std::round(x); }
[[nodiscard]] inline double trunc(double x) noexcept { return std::trunc(x); }
[[nodiscard]] inline float trunc(float x) noexcept { return std::trunc(x); }
[[nodiscard]] inline double sqrt(double x) noexcept { return std::sqrt(x); } // IEEE-exact, deterministic
[[nodiscard]] inline float sqrt(float x) noexcept { return std::sqrt(x); }
[[nodiscard]] inline double copysign(double m, double s) noexcept { return std::copysign(m, s); }
[[nodiscard]] inline float copysign(float m, float s) noexcept { return std::copysign(m, s); }
[[nodiscard]] inline double fma(double a, double b, double c) noexcept { return std::fma(a, b, c); } // single-rounded
[[nodiscard]] inline float fma(float a, float b, float c) noexcept { return std::fma(a, b, c); }
[[nodiscard]] inline double lerp(double a, double b, double t) noexcept { return std::fma(t, b - a, a); }
[[nodiscard]] inline float lerp(float a, float b, float t) noexcept { return std::fma(t, b - a, a); }
[[nodiscard]] inline long lround(double x) noexcept { return std::lround(x); }
[[nodiscard]] inline long lround(float x) noexcept { return std::lround(x); }
[[nodiscard]] inline double fmod(double x, double y) noexcept { return std::fmod(x, y); }
[[nodiscard]] inline float fmod(float x, float y) noexcept { return std::fmod(x, y); }

// ── native gap-fills (our OWN logic — bit-ops + composition of the exact primitives above, no std::<fn> passthrough) ──
// These cmath functions were absent from crd::math; each is IEEE-exact and deterministic by construction.

// IEEE sign bit — correct for −0 and NaN (unlike `x < 0`). Pure bit test.
[[nodiscard]] inline bool signbit(double x) noexcept { return (std::bit_cast<crd::u64>(x) >> 63U) != 0U; }
[[nodiscard]] inline bool signbit(float x) noexcept { return (std::bit_cast<crd::u32>(x) >> 31U) != 0U; }

// NaN-IGNORING max/min (IEEE-754 maxNum/minNum): a NaN operand is dropped in favour of the number — distinct from min/max,
// which propagate whatever the comparison yields. `a != a` is the branchless isnan.
[[nodiscard]] inline double fmax(double a, double b) noexcept { if (a != a) { return b; } if (b != b) { return a; } return a < b ? b : a; }
[[nodiscard]] inline float fmax(float a, float b) noexcept { if (a != a) { return b; } if (b != b) { return a; } return a < b ? b : a; }
[[nodiscard]] inline double fmin(double a, double b) noexcept { if (a != a) { return b; } if (b != b) { return a; } return b < a ? b : a; }
[[nodiscard]] inline float fmin(float a, float b) noexcept { if (a != a) { return b; } if (b != b) { return a; } return b < a ? b : a; }

// positive difference: max(x − y, 0), NaN-propagating.
[[nodiscard]] inline double fdim(double x, double y) noexcept
{
    if (x != x || y != y) { return x - y; } // NaN propagates
    return x > y ? x - y : 0.0;
}
[[nodiscard]] inline float fdim(float x, float y) noexcept
{
    if (x != x || y != y) { return x - y; }
    return x > y ? x - y : 0.0F;
}

// round to nearest, ties to EVEN (the IEEE default) — built from trunc/abs/copysign; no libm rounding call.
[[nodiscard]] inline double rint(double x) noexcept
{
    const double t = trunc(x);
    const double f = abs(x - t); // fractional magnitude in [0,1)
    if (f < 0.5) { return t; }
    if (f > 0.5) { return t + copysign(1.0, x); }
    const double h = t * 0.5; // exact tie → keep t if even, else step to the even neighbour
    return (trunc(h) == h) ? t : t + copysign(1.0, x);
}
[[nodiscard]] inline float rint(float x) noexcept
{
    const float t = trunc(x);
    const float f = abs(x - t);
    if (f < 0.5F) { return t; }
    if (f > 0.5F) { return t + copysign(1.0F, x); }
    const float h = t * 0.5F;
    return (trunc(h) == h) ? t : t + copysign(1.0F, x);
}
// nearbyint == rint under the (fixed) round-to-nearest-even mode; native, no std passthrough.
[[nodiscard]] inline double nearbyint(double x) noexcept { return rint(x); }
[[nodiscard]] inline float nearbyint(float x) noexcept { return rint(x); }

// split into integer (toward zero) and fractional parts; *ip = trunc(x), returns the fraction — which carries the SIGN of x
// (so modf(-100, ·) → −0.0, matching IEEE/std). copysign supplies that sign on a zero fraction.
[[nodiscard]] inline double modf(double x, double* ip) noexcept { const double t = trunc(x); *ip = t; return copysign(x - t, x); }
[[nodiscard]] inline float modf(float x, float* ip) noexcept { const float t = trunc(x); *ip = t; return copysign(x - t, x); }

// decompose x = m·2^e with m ∈ [0.5,1) (m = x for 0/inf/nan). Pure IEEE bit surgery (subnormals normalised by a 2^54 scale).
[[nodiscard]] inline double frexp(double x, int* e) noexcept
{
    crd::u64  b      = std::bit_cast<crd::u64>(x);
    const int rawexp = static_cast<int>((b >> 52U) & 0x7FFU);
    if (rawexp == 0x7FF || x == 0.0) { *e = 0; return x; } // inf/nan/±0
    int shift = 0;
    if (rawexp == 0) // subnormal: scale up by 2^54, re-read the exponent, account for the scale
    {
        b     = std::bit_cast<crd::u64>(x * 18014398509481984.0); // 2^54
        shift = -54;
    }
    *e = (static_cast<int>((b >> 52U) & 0x7FFU) - 1022) + shift; // bias 1023, m∈[0.5,1) ⇒ stored exponent 1022
    b  = (b & ~(0x7FFULL << 52U)) | (static_cast<crd::u64>(1022) << 52U);
    return std::bit_cast<double>(b);
}
[[nodiscard]] inline float frexp(float x, int* e) noexcept
{
    crd::u32  b      = std::bit_cast<crd::u32>(x);
    const int rawexp = static_cast<int>((b >> 23U) & 0xFFU);
    if (rawexp == 0xFF || x == 0.0F) { *e = 0; return x; }
    int shift = 0;
    if (rawexp == 0)
    {
        b     = std::bit_cast<crd::u32>(x * 16777216.0F); // 2^24
        shift = -24;
    }
    *e = (static_cast<int>((b >> 23U) & 0xFFU) - 126) + shift; // bias 127, m∈[0.5,1) ⇒ stored exponent 126
    b  = (b & ~(0xFFU << 23U)) | (static_cast<crd::u32>(126) << 23U);
    return std::bit_cast<float>(b);
}

// IEEE remainder: x − n·y with n = round-to-nearest-EVEN(x/y); the result lies in [−|y|/2, |y|/2] and is EXACT (fdlibm
// reduction, Stephen Moshier / Sun). Built on our exact fmod + abs/copysign — NOT std::remainder. The round-to-even tie
// falls out of reducing mod 2|y| (each subtraction is exact by Sterbenz's lemma). Distinct from fmod, which truncates the
// quotient toward zero. remainder(x,0)=NaN, remainder(±inf,y)=NaN, remainder(x,±inf)=x.
[[nodiscard]] inline double remainder(double x, double y) noexcept
{
    const double ay = abs(y);
    const double ax = abs(x);
    // fmod resolves the special cases for free: fmod(·,0)/fmod(±inf,·)=NaN (⇒ NaN out), fmod(·,±inf)=|x| (⇒ x out); 2|y| that
    // overflows to +inf leaves r=|x| (correct, since |x|<2|y|). Otherwise r ∈ [0, 2|y|).
    double r = fmod(ax, ay + ay);
    if (ay < std::numeric_limits<double>::min()) // tiny/zero |y|: 0.5*|y| would underflow ⇒ compare via r+r
    {
        if (r + r > ay) { r -= ay; if (r + r >= ay) { r -= ay; } }
    }
    else
    {
        const double hp = 0.5 * ay; // reduce to [−|y|/2, |y|/2]; ties-to-even fall out of having reduced mod 2|y|
        if (r > hp) { r -= ay; if (r >= hp) { r -= ay; } }
    }
    return signbit(x) ? -r : r; // remainder is ODD in x; r carries its own sign (NOT x's) — copysign would be wrong
}
[[nodiscard]] inline float remainder(float x, float y) noexcept
{
    const float ay = abs(y);
    const float ax = abs(x);
    float       r  = fmod(ax, ay + ay);
    if (ay < std::numeric_limits<float>::min())
    {
        if (r + r > ay) { r -= ay; if (r + r >= ay) { r -= ay; } }
    }
    else
    {
        const float hp = 0.5F * ay;
        if (r > hp) { r -= ay; if (r >= hp) { r -= ay; } }
    }
    return signbit(x) ? -r : r; // remainder is ODD in x; r carries its own sign
}

} // namespace crd::math
