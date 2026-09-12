#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/units/quantity.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::math
{
template <typename T>
inline constexpr bool k_is_math_scalar_v = std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>;

template <typename T>
concept MathScalar = k_is_math_scalar_v<T>;

// MathValue — widened concept (Phase 3.1.7.5 v0b-1 / ADR-0078 §2 D3).
// Accepts raw scalars AND `crd::units::Quantity<D, T>` so consumers can
// write `Vec3<Length<f32>>` etc. directly. Reductions that would need
// fractional-exponent dimensions (dot, cross, length) keep the stricter
// `MathScalar` concept so they refuse Quantity at the type level
// (compile error rather than silently producing wrong-dim arithmetic).
template <typename T>
concept MathValue = MathScalar<T> || crd::units::IsQuantity<T>;

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
    return std::is_same_v<T, crd::f32> ? static_cast<T>(1.0e-5F) : static_cast<T>(1.0e-12);
}

// Widened default_epsilon for Quantity<D, T> (Phase 3.1.7.5 v0d-2). Returns a
// dimensional epsilon — `Quantity<D, T>{default_epsilon<T>()}`. Lets geometry
// algorithms templated on MathValue use the same `default_epsilon<T>()`
// boilerplate when T is a Quantity. The numeric magnitude matches the
// underlying scalar — comparisons against `len_sq` etc. require the same
// magnitude convention as raw arithmetic.
template <typename D, typename T>
inline constexpr crd::units::Quantity<D, T> default_epsilon_quantity() noexcept
{
    return crd::units::Quantity<D, T>{default_epsilon<T>()};
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

// ---------------------------------------------------------------------------
// crd-math foundation widening for Quantity (Phase 3.1.7.5 v0d-2 / ADR-0078 §4 D27).
//
// Lets geometry-primitives algorithms templated on MathValue compose
// arithmetic / comparison / clamp patterns over Quantity<D, T> exactly as
// they do for raw scalars. Each overload delegates to the underlying
// scalar; the dimensional tag rides through unchanged.

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<D, T> abs(crd::units::Quantity<D, T> q) noexcept
{
    return crd::units::Quantity<D, T>{abs(q.value)};
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<D, T>
min(crd::units::Quantity<D, T> a, crd::units::Quantity<D, T> b) noexcept
{
    return crd::units::Quantity<D, T>{min(a.value, b.value)};
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<D, T>
max(crd::units::Quantity<D, T> a, crd::units::Quantity<D, T> b) noexcept
{
    return crd::units::Quantity<D, T>{max(a.value, b.value)};
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<D, T>
clamp(crd::units::Quantity<D, T> value, crd::units::Quantity<D, T> lo, crd::units::Quantity<D, T> hi) noexcept
{
    return min(max(value, lo), hi);
}

template <typename D, typename T>
[[nodiscard]] inline bool is_finite(crd::units::Quantity<D, T> q) noexcept
{
    return std::isfinite(q.value);
}

template <typename D, typename T>
[[nodiscard]] inline bool is_nan(crd::units::Quantity<D, T> q) noexcept
{
    return std::isnan(q.value);
}

// `default_epsilon<Q>()` for Q = Quantity<D, T>. Returns a typed epsilon
// with the raw magnitude inherited from `default_epsilon<T>()`.
template <typename Q>
    requires crd::units::IsQuantity<Q>
[[nodiscard]] inline constexpr Q default_epsilon() noexcept
{
    return Q{default_epsilon<typename Q::scalar>()};
}

// `sqrt(Quantity<DimMul<D, D>, T>) -> Quantity<D, T>` (Phase 3.1.7.5 v0d-2).
// Scoped sqrt: the only general form we ship is "sqrt of an even-exponent
// Quantity returns the half-exponent Quantity, when the half-exponent is
// known from context". The common case is `sqrt(length_squared(v)) ->
// length(v)`. The free function below is a TARGETED overload — accepts a
// `Quantity<DimMul<D, D>, T>` and returns `Quantity<D, T>` — that requires
// the caller to spell out `D` (so the result type is unambiguous). General
// fractional-exponent dimensions are deferred per ADR-0078 §3 D24.
template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
sqrt_as(crd::units::Quantity<crd::units::DimMul<D, D>, T> q_sq) noexcept
{
    return crd::units::Quantity<D, T>{static_cast<T>(std::sqrt(q_sq.value))};
}

// ─── Interpolation primitives ────────────────────────────────────────────────
//
// Standard scalar interpolation utilities. Vector overloads (Vec2/3/4) live in
// vec.hpp. Easing curves live in easing.hpp. The convention is:
//   - lerp/mix:       value-space interpolation `a + (b - a) * t`.
//   - inverse_lerp:   given an interpolated value, recover `t`.
//   - remap:          rescale a value from one range to another.
//   - saturate/step/smoothstep: GLSL/HLSL-style fragment helpers.
//   - damp:           frame-rate-independent exponential approach.
//
// `t` is NOT clamped by lerp/mix — pass any value, including >1 or <0, when
// extrapolation is desired. saturate(t) at the call site if you need clamping.

template <MathScalar T> [[nodiscard]] constexpr T lerp(T a, T b, T t) noexcept
{
    return a + (b - a) * t;
}

// GLSL alias for lerp — provided so shader-port code reads naturally and so
// users from a GLSL background find the function they expect.
template <MathScalar T> [[nodiscard]] constexpr T mix(T a, T b, T t) noexcept
{
    return lerp(a, b, t);
}

template <MathScalar T> [[nodiscard]] constexpr T saturate(T value) noexcept
{
    return clamp(value, static_cast<T>(0), static_cast<T>(1));
}

template <MathScalar T> [[nodiscard]] constexpr T step(T edge, T x) noexcept
{
    return x < edge ? static_cast<T>(0) : static_cast<T>(1);
}

// Hermite C¹ smoothstep: `3t² − 2t³` after re-mapping `x` to [0,1].
// f(edge0)=0, f(edge1)=1, f'(edge0)=f'(edge1)=0.
template <MathScalar T> [[nodiscard]] constexpr T smoothstep(T edge0, T edge1, T x) noexcept
{
    CRD_ASSERT(!approx_equal_abs(edge0, edge1));
    const T t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (static_cast<T>(3) - static_cast<T>(2) * t);
}

// Ken Perlin's quintic smootherstep: `6t⁵ − 15t⁴ + 10t³`.
// f(edge0)=0, f(edge1)=1, f'(edge*)=f''(edge*)=0 — better for animation curves
// and heightmap blending where the second derivative being zero at the edges
// matters (no visible "kink" at the transition).
template <MathScalar T> [[nodiscard]] constexpr T smootherstep(T edge0, T edge1, T x) noexcept
{
    CRD_ASSERT(!approx_equal_abs(edge0, edge1));
    const T t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * t * (t * (t * static_cast<T>(6) - static_cast<T>(15)) + static_cast<T>(10));
}

// Recover the parameter `t` such that `lerp(a, b, t) == x`.
// Asserts a != b (the inverse is undefined for a degenerate range).
template <MathScalar T> [[nodiscard]] constexpr T inverse_lerp(T a, T b, T x) noexcept
{
    CRD_ASSERT(!approx_equal_abs(a, b));
    return (x - a) / (b - a);
}

// Linearly remap `x` from [in_min, in_max] to [out_min, out_max].
// Equivalent to lerp(out_min, out_max, inverse_lerp(in_min, in_max, x)).
template <MathScalar T>
[[nodiscard]] constexpr T remap(T x, T in_min, T in_max, T out_min, T out_max) noexcept
{
    return lerp(out_min, out_max, inverse_lerp(in_min, in_max, x));
}

// Frame-rate-independent exponential approach toward `target`.
//   damp(a, b, λ, dt) = a + (b - a) * (1 - exp(-λ * dt))
// `lambda` is the time constant (1/seconds) — larger = faster convergence.
// Reduces to identity at dt == 0 and (essentially) reaches the target as
// dt → ∞. Stable across frame-rate variation, unlike a fixed-`t` lerp.
template <MathScalar T> [[nodiscard]] inline T damp(T a, T b, T lambda, T dt) noexcept
{
    return lerp(a, b, static_cast<T>(1) - std::exp(-lambda * dt));
}
} // namespace crd::math
