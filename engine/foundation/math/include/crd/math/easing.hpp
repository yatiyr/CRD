#pragma once

#include <crd/math/scalar.hpp>

#include <cmath>

// Penner easing functions. Each curve takes a normalised parameter t ∈ [0, 1]
// and returns f(t) — typically also in [0, 1], except Back/Elastic/Bounce which
// deliberately overshoot or undershoot to produce the characteristic visual
// effect they are named for.
//
// Curves are decoupled from the value type: combine with `lerp` at the call
// site, e.g. `lerp(a, b, ease_out_cubic(t))`. There is no `Tween` class, no
// `EasingChannel`, no animation runtime here — those belong to a higher layer
// (Phase 3.2 and onward) and are shaped by their consumer.
//
// Polynomial families (Quad/Cubic/Quart/Quint, Back) are `constexpr`. Families
// that use std::sin / std::cos / std::pow / std::exp / std::sqrt are `inline`
// only — those <cmath> functions are not yet `constexpr` in C++20.
//
// Reference: Robert Penner's easing equations (2001), as canonised by
// easings.net, d3-ease, GSAP, tween.js. Identical names and curves.

namespace crd::math
{

// ─── Linear ──────────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] constexpr T ease_linear(T t) noexcept
{
    return t;
}

// ─── Sine ────────────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] inline T ease_in_sine(T t) noexcept
{
    return static_cast<T>(1) - std::cos(t * k_half_pi<T>);
}

template <MathScalar T> [[nodiscard]] inline T ease_out_sine(T t) noexcept
{
    return std::sin(t * k_half_pi<T>);
}

template <MathScalar T> [[nodiscard]] inline T ease_in_out_sine(T t) noexcept
{
    return -(std::cos(k_pi<T> * t) - static_cast<T>(1)) * static_cast<T>(0.5);
}

// ─── Quadratic (t²) ──────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] constexpr T ease_in_quad(T t) noexcept
{
    return t * t;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_out_quad(T t) noexcept
{
    const T u = static_cast<T>(1) - t;
    return static_cast<T>(1) - u * u;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_out_quad(T t) noexcept
{
    if (t < static_cast<T>(0.5))
    {
        return static_cast<T>(2) * t * t;
    }
    const T u = static_cast<T>(-2) * t + static_cast<T>(2);
    return static_cast<T>(1) - (u * u) * static_cast<T>(0.5);
}

// ─── Cubic (t³) ──────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] constexpr T ease_in_cubic(T t) noexcept
{
    return t * t * t;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_out_cubic(T t) noexcept
{
    const T u = static_cast<T>(1) - t;
    return static_cast<T>(1) - u * u * u;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_out_cubic(T t) noexcept
{
    if (t < static_cast<T>(0.5))
    {
        return static_cast<T>(4) * t * t * t;
    }
    const T u = static_cast<T>(-2) * t + static_cast<T>(2);
    return static_cast<T>(1) - (u * u * u) * static_cast<T>(0.5);
}

// ─── Quartic (t⁴) ────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] constexpr T ease_in_quart(T t) noexcept
{
    return t * t * t * t;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_out_quart(T t) noexcept
{
    const T u = static_cast<T>(1) - t;
    return static_cast<T>(1) - u * u * u * u;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_out_quart(T t) noexcept
{
    if (t < static_cast<T>(0.5))
    {
        return static_cast<T>(8) * t * t * t * t;
    }
    const T u = static_cast<T>(-2) * t + static_cast<T>(2);
    return static_cast<T>(1) - (u * u * u * u) * static_cast<T>(0.5);
}

// ─── Quintic (t⁵) ────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] constexpr T ease_in_quint(T t) noexcept
{
    return t * t * t * t * t;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_out_quint(T t) noexcept
{
    const T u = static_cast<T>(1) - t;
    return static_cast<T>(1) - u * u * u * u * u;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_out_quint(T t) noexcept
{
    if (t < static_cast<T>(0.5))
    {
        return static_cast<T>(16) * t * t * t * t * t;
    }
    const T u = static_cast<T>(-2) * t + static_cast<T>(2);
    return static_cast<T>(1) - (u * u * u * u * u) * static_cast<T>(0.5);
}

// ─── Exponential ─────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] inline T ease_in_expo(T t) noexcept
{
    return t <= static_cast<T>(0)
               ? static_cast<T>(0)
               : std::pow(static_cast<T>(2), static_cast<T>(10) * t - static_cast<T>(10));
}

template <MathScalar T> [[nodiscard]] inline T ease_out_expo(T t) noexcept
{
    return t >= static_cast<T>(1)
               ? static_cast<T>(1)
               : static_cast<T>(1) - std::pow(static_cast<T>(2), static_cast<T>(-10) * t);
}

template <MathScalar T> [[nodiscard]] inline T ease_in_out_expo(T t) noexcept
{
    if (t <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (t >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    if (t < static_cast<T>(0.5))
    {
        return std::pow(static_cast<T>(2), static_cast<T>(20) * t - static_cast<T>(10)) * static_cast<T>(0.5);
    }
    return (static_cast<T>(2) - std::pow(static_cast<T>(2), static_cast<T>(-20) * t + static_cast<T>(10)))
           * static_cast<T>(0.5);
}

// ─── Circular ────────────────────────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] inline T ease_in_circ(T t) noexcept
{
    return static_cast<T>(1) - std::sqrt(static_cast<T>(1) - t * t);
}

template <MathScalar T> [[nodiscard]] inline T ease_out_circ(T t) noexcept
{
    const T u = t - static_cast<T>(1);
    return std::sqrt(static_cast<T>(1) - u * u);
}

template <MathScalar T> [[nodiscard]] inline T ease_in_out_circ(T t) noexcept
{
    if (t < static_cast<T>(0.5))
    {
        const T u = static_cast<T>(2) * t;
        return (static_cast<T>(1) - std::sqrt(static_cast<T>(1) - u * u)) * static_cast<T>(0.5);
    }
    const T u = static_cast<T>(-2) * t + static_cast<T>(2);
    return (std::sqrt(static_cast<T>(1) - u * u) + static_cast<T>(1)) * static_cast<T>(0.5);
}

// ─── Back (overshoot) ────────────────────────────────────────────────────────
//
// Penner's `c1 = 1.70158` produces ~10% overshoot. `c2 = c1 * 1.525 ≈ 2.5949`
// is used in the InOut variant so the symmetric overshoot still feels right.

template <MathScalar T> [[nodiscard]] constexpr T ease_in_back(T t) noexcept
{
    constexpr T c1 = static_cast<T>(1.70158);
    constexpr T c3 = c1 + static_cast<T>(1);
    return c3 * t * t * t - c1 * t * t;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_out_back(T t) noexcept
{
    constexpr T c1 = static_cast<T>(1.70158);
    constexpr T c3 = c1 + static_cast<T>(1);
    const T u = t - static_cast<T>(1);
    return static_cast<T>(1) + c3 * u * u * u + c1 * u * u;
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_out_back(T t) noexcept
{
    constexpr T c1 = static_cast<T>(1.70158);
    constexpr T c2 = c1 * static_cast<T>(1.525);
    if (t < static_cast<T>(0.5))
    {
        const T u = static_cast<T>(2) * t;
        return (u * u * ((c2 + static_cast<T>(1)) * u - c2)) * static_cast<T>(0.5);
    }
    const T u = static_cast<T>(2) * t - static_cast<T>(2);
    return (u * u * ((c2 + static_cast<T>(1)) * u + c2) + static_cast<T>(2)) * static_cast<T>(0.5);
}

// ─── Elastic (decaying oscillation) ──────────────────────────────────────────

template <MathScalar T> [[nodiscard]] inline T ease_in_elastic(T t) noexcept
{
    constexpr T c4 = k_tau<T> / static_cast<T>(3);
    if (t <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (t >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    return -std::pow(static_cast<T>(2), static_cast<T>(10) * t - static_cast<T>(10))
           * std::sin((t * static_cast<T>(10) - static_cast<T>(10.75)) * c4);
}

template <MathScalar T> [[nodiscard]] inline T ease_out_elastic(T t) noexcept
{
    constexpr T c4 = k_tau<T> / static_cast<T>(3);
    if (t <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (t >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    return std::pow(static_cast<T>(2), static_cast<T>(-10) * t)
               * std::sin((t * static_cast<T>(10) - static_cast<T>(0.75)) * c4)
           + static_cast<T>(1);
}

template <MathScalar T> [[nodiscard]] inline T ease_in_out_elastic(T t) noexcept
{
    constexpr T c5 = k_tau<T> / static_cast<T>(4.5);
    if (t <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    if (t >= static_cast<T>(1))
    {
        return static_cast<T>(1);
    }
    if (t < static_cast<T>(0.5))
    {
        return -(std::pow(static_cast<T>(2), static_cast<T>(20) * t - static_cast<T>(10))
                 * std::sin((static_cast<T>(20) * t - static_cast<T>(11.125)) * c5))
               * static_cast<T>(0.5);
    }
    return (std::pow(static_cast<T>(2), static_cast<T>(-20) * t + static_cast<T>(10))
            * std::sin((static_cast<T>(20) * t - static_cast<T>(11.125)) * c5))
               * static_cast<T>(0.5)
           + static_cast<T>(1);
}

// ─── Bounce (piecewise quadratic) ────────────────────────────────────────────

template <MathScalar T> [[nodiscard]] constexpr T ease_out_bounce(T t) noexcept
{
    constexpr T n1 = static_cast<T>(7.5625);
    constexpr T d1 = static_cast<T>(2.75);
    if (t < static_cast<T>(1) / d1)
    {
        return n1 * t * t;
    }
    if (t < static_cast<T>(2) / d1)
    {
        const T u = t - static_cast<T>(1.5) / d1;
        return n1 * u * u + static_cast<T>(0.75);
    }
    if (t < static_cast<T>(2.5) / d1)
    {
        const T u = t - static_cast<T>(2.25) / d1;
        return n1 * u * u + static_cast<T>(0.9375);
    }
    const T u = t - static_cast<T>(2.625) / d1;
    return n1 * u * u + static_cast<T>(0.984375);
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_bounce(T t) noexcept
{
    return static_cast<T>(1) - ease_out_bounce(static_cast<T>(1) - t);
}

template <MathScalar T> [[nodiscard]] constexpr T ease_in_out_bounce(T t) noexcept
{
    if (t < static_cast<T>(0.5))
    {
        return (static_cast<T>(1) - ease_out_bounce(static_cast<T>(1) - static_cast<T>(2) * t))
               * static_cast<T>(0.5);
    }
    return (static_cast<T>(1) + ease_out_bounce(static_cast<T>(2) * t - static_cast<T>(1)))
           * static_cast<T>(0.5);
}

} // namespace crd::math
