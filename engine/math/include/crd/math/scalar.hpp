#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::math
{
template <typename T>
inline constexpr bool k_is_math_scalar_v = std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>;

template <typename T>
concept MathScalar = k_is_math_scalar_v<T>;

template <MathScalar T> inline constexpr T k_pi = static_cast<T>(3.14159265358979323846264338327950288L);

template <MathScalar T> inline constexpr T k_tau = static_cast<T>(2) * k_pi<T>;

template <MathScalar T> inline constexpr T k_half_pi = static_cast<T>(0.5) * k_pi<T>;

inline constexpr crd::f32 k_pi_f = k_pi<crd::f32>;
inline constexpr crd::f64 k_pi_d = k_pi<crd::f64>;
inline constexpr crd::f32 k_tau_f = k_tau<crd::f32>;
inline constexpr crd::f64 k_tau_d = k_tau<crd::f64>;
inline constexpr crd::f32 k_half_pi_f = k_half_pi<crd::f32>;
inline constexpr crd::f64 k_half_pi_d = k_half_pi<crd::f64>;

template <MathScalar T> inline constexpr T default_epsilon() noexcept
{
    return std::is_same_v<T, crd::f32> ? static_cast<T>(1.0e-5f) : static_cast<T>(1.0e-12);
}

template <MathScalar T> [[nodiscard]] constexpr T deg_to_rad(T degrees) noexcept
{
    return degrees * (k_pi<T> / static_cast<T>(180));
}

template <MathScalar T> [[nodiscard]] constexpr T rad_to_deg(T radians) noexcept
{
    return radians * (static_cast<T>(180) / k_pi<T>);
}

template <MathScalar T> [[nodiscard]] constexpr T abs(T value) noexcept
{
    return value < static_cast<T>(0) ? -value : value;
}

template <MathScalar T> [[nodiscard]] constexpr T min(T a, T b) noexcept
{
    return a < b ? a : b;
}

template <MathScalar T> [[nodiscard]] constexpr T max(T a, T b) noexcept
{
    return a > b ? a : b;
}

template <MathScalar T> [[nodiscard]] constexpr T clamp(T value, T lo, T hi) noexcept
{
    return min(max(value, lo), hi);
}

template <MathScalar T> [[nodiscard]] inline bool is_finite(T value) noexcept
{
    return std::isfinite(value);
}

template <MathScalar T> [[nodiscard]] inline bool is_nan(T value) noexcept
{
    return std::isnan(value);
}

template <MathScalar T>
[[nodiscard]] constexpr bool approx_equal_abs(T a, T b, T epsilon = default_epsilon<T>()) noexcept
{
    return abs(a - b) <= epsilon;
}

template <MathScalar T>
[[nodiscard]] constexpr bool approx_equal_rel(T a, T b, T epsilon = default_epsilon<T>()) noexcept
{
    const T scale = max(static_cast<T>(1), max(abs(a), abs(b)));
    return abs(a - b) <= epsilon * scale;
}

template <MathScalar T> [[nodiscard]] constexpr bool approx_zero(T value, T epsilon = default_epsilon<T>()) noexcept
{
    return abs(value) <= epsilon;
}
} // namespace crd::math
