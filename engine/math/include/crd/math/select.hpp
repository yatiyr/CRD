#pragma once

// crd-math select/round tier — EXACT primitives (hardware / bit-ops), deterministic by construction. The unified
// surface so engine code calls crd::math::{min,max,clamp,abs,floor,ceil,round,trunc,sqrt,copysign,fma,sign,lerp,
// saturate} instead of std:: (the guard target). No approximation ⇒ no ulp gate; correctness is exactness.

#include <cmath>

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
    return v < lo ? lo : (hi < v ? hi : v);
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
[[nodiscard]] inline double nearbyint(double x) noexcept { return std::nearbyint(x); }
[[nodiscard]] inline float nearbyint(float x) noexcept { return std::nearbyint(x); }
[[nodiscard]] inline long lround(double x) noexcept { return std::lround(x); }
[[nodiscard]] inline long lround(float x) noexcept { return std::lround(x); }
[[nodiscard]] inline double fmod(double x, double y) noexcept { return std::fmod(x, y); }
[[nodiscard]] inline float fmod(float x, float y) noexcept { return std::fmod(x, y); }

} // namespace crd::math
