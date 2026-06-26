#pragma once

// dual.hpp — Phase 3.1.6 v7-b: Dual<T> — the forward-mode automatic-differentiation scalar. A dual number
// carries a value v and ONE directional derivative d; arithmetic propagates the chain rule exactly (no truncation,
// no subtractive cancellation — unlike finite differences). Seeding x_i with d=1 (others d=0) and evaluating a
// scalar-generic functor f yields the EXACT ∂f/∂x_i in the result's derivative part. ADR-0090; forward_ad.hpp
// drives the n-pass gradient.
//
// SCALAR-GENERIC FUNCTOR CONTRACT: a functor written `template <class S> S operator()(ConstSpan<S> x) const`
// instantiates on both T (the real value path) and Dual<T> (the derivative path). It should use the arithmetic
// operators (which work for both) and, for transcendentals, the unqualified-call-with-`using crd::math::sin;` idiom so
// ADL finds crd::math::sin for T and crd::hesap::opt::sin for Dual<T> (the standard autodiff idiom).
//
// NOTE: Dual<T> lives in crd-hesap-opt for v7-b. When the ADR-0065 reverse-mode autodiff module lands it may
// migrate to a shared autodiff home; the type + free functions are header-only so a move is mechanical.

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

// Forward-mode dual number: value `v` + derivative `d` (one tangent direction). Trivially copyable.
template <typename T>
struct Dual
{
    T v; // value      f(x)
    T d; // derivative f'(x) along the seeded direction

    constexpr Dual() noexcept : v(static_cast<T>(0)), d(static_cast<T>(0)) {}
    // Implicit from a constant: a plain value has zero derivative (so `2.0 * dual` etc. just works).
    constexpr Dual(T value) noexcept : v(value), d(static_cast<T>(0)) {} // NOLINT(google-explicit-constructor)
    constexpr Dual(T value, T deriv) noexcept : v(value), d(deriv) {}

    [[nodiscard]] constexpr Dual operator+(const Dual& b) const noexcept { return Dual{v + b.v, d + b.d}; }
    [[nodiscard]] constexpr Dual operator-(const Dual& b) const noexcept { return Dual{v - b.v, d - b.d}; }
    [[nodiscard]] constexpr Dual operator-() const noexcept { return Dual{-v, -d}; }

    // Product rule: (a·b)' = a'·b + a·b'.
    [[nodiscard]] constexpr Dual operator*(const Dual& b) const noexcept
    {
        return Dual{v * b.v, d * b.v + v * b.d};
    }

    // Quotient rule: (a/b)' = (a'·b − a·b') / b².
    [[nodiscard]] constexpr Dual operator/(const Dual& b) const noexcept
    {
        const T inv = static_cast<T>(1) / b.v;
        return Dual{v * inv, (d - (v * inv) * b.d) * inv};
    }

    constexpr Dual& operator+=(const Dual& b) noexcept
    {
        *this = *this + b;
        return *this;
    }
    constexpr Dual& operator-=(const Dual& b) noexcept
    {
        *this = *this - b;
        return *this;
    }
    constexpr Dual& operator*=(const Dual& b) noexcept
    {
        *this = *this * b;
        return *this;
    }
    constexpr Dual& operator/=(const Dual& b) noexcept
    {
        *this = *this / b;
        return *this;
    }

    // Comparisons act on the VALUE (so functor branching `if (x > 0)` chooses a branch by the point, not the
    // tangent — the standard forward-AD convention).
    [[nodiscard]] constexpr bool operator==(const Dual& b) const noexcept { return v == b.v; }
    [[nodiscard]] constexpr bool operator!=(const Dual& b) const noexcept { return v != b.v; }
    [[nodiscard]] constexpr bool operator<(const Dual& b) const noexcept { return v < b.v; }
    [[nodiscard]] constexpr bool operator<=(const Dual& b) const noexcept { return v <= b.v; }
    [[nodiscard]] constexpr bool operator>(const Dual& b) const noexcept { return v > b.v; }
    [[nodiscard]] constexpr bool operator>=(const Dual& b) const noexcept { return v >= b.v; }
};

// ---- Transcendentals (ADL-found for Dual<T>; chain rule baked in) ------------------------------------------

template <typename T>
[[nodiscard]] inline Dual<T> sin(const Dual<T>& x) noexcept
{
    return Dual<T>{crd::math::sin(x.v), x.d * crd::math::cos(x.v)};
}

template <typename T>
[[nodiscard]] inline Dual<T> cos(const Dual<T>& x) noexcept
{
    return Dual<T>{crd::math::cos(x.v), -x.d * crd::math::sin(x.v)};
}

template <typename T>
[[nodiscard]] inline Dual<T> tan(const Dual<T>& x) noexcept
{
    const T c = crd::math::cos(x.v);
    return Dual<T>{crd::math::tan(x.v), x.d / (c * c)}; // sec²(x)·x'
}

template <typename T>
[[nodiscard]] inline Dual<T> exp(const Dual<T>& x) noexcept
{
    const T e = crd::math::exp(x.v);
    return Dual<T>{e, x.d * e};
}

template <typename T>
[[nodiscard]] inline Dual<T> log(const Dual<T>& x) noexcept
{
    return Dual<T>{crd::math::log(x.v), x.d / x.v};
}

template <typename T>
[[nodiscard]] inline Dual<T> sqrt(const Dual<T>& x) noexcept
{
    const T s = crd::math::sqrt(x.v);
    return Dual<T>{s, x.d / (static_cast<T>(2) * s)};
}

template <typename T>
[[nodiscard]] inline Dual<T> tanh(const Dual<T>& x) noexcept
{
    const T t = crd::math::tanh(x.v);
    return Dual<T>{t, x.d * (static_cast<T>(1) - t * t)}; // sech²(x)·x'
}

template <typename T>
[[nodiscard]] inline Dual<T> abs(const Dual<T>& x) noexcept
{
    // Subgradient at 0 takes the +1 branch (convention); fine for optimization use.
    return x.v >= static_cast<T>(0) ? x : Dual<T>{-x.v, -x.d};
}

// pow with a constant exponent: (x^p)' = p·x^(p−1)·x'.
template <typename T>
[[nodiscard]] inline Dual<T> pow(const Dual<T>& x, T p) noexcept
{
    return Dual<T>{crd::math::pow(x.v, p), x.d * p * crd::math::pow(x.v, p - static_cast<T>(1))};
}

// pow with a dual exponent: d/dt x^y = x^y·(y'·ln x + y·x'/x).
template <typename T>
[[nodiscard]] inline Dual<T> pow(const Dual<T>& x, const Dual<T>& y) noexcept
{
    const T f = crd::math::pow(x.v, y.v);
    return Dual<T>{f, f * (y.d * crd::math::log(x.v) + y.v * x.d / x.v)};
}

// ---- Mixed scalar/dual operators (so `2.0 * x`, `x + 1.0` read naturally in generic code) ------------------

template <typename T>
[[nodiscard]] constexpr Dual<T> operator+(T s, const Dual<T>& x) noexcept
{
    return Dual<T>{s + x.v, x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator+(const Dual<T>& x, T s) noexcept
{
    return Dual<T>{x.v + s, x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator-(T s, const Dual<T>& x) noexcept
{
    return Dual<T>{s - x.v, -x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator-(const Dual<T>& x, T s) noexcept
{
    return Dual<T>{x.v - s, x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator*(T s, const Dual<T>& x) noexcept
{
    return Dual<T>{s * x.v, s * x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator*(const Dual<T>& x, T s) noexcept
{
    return Dual<T>{x.v * s, x.d * s};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator/(const Dual<T>& x, T s) noexcept
{
    const T inv = static_cast<T>(1) / s;
    return Dual<T>{x.v * inv, x.d * inv};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator/(T s, const Dual<T>& x) noexcept
{
    const T inv = static_cast<T>(1) / x.v;
    return Dual<T>{s * inv, -(s * inv) * inv * x.d}; // (s/x)' = −s·x'/x²
}

} // namespace crd::hesap::opt
