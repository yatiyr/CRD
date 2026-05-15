#pragma once

// ---------------------------------------------------------------------------
// crd-units — Quantity<D, T> zero-overhead dimensional wrapper (Phase
// 3.1.7.5 v0a-1).
//
// Quantity<D, T> wraps a scalar T (typically crd::f32 or crd::f64) tagged
// with a compile-time Dim<...>. All arithmetic that respects dimensions is
// allowed; dimensionally-invalid arithmetic is a compile error.
//
// Per the Strategic Execution Plan 2026-05-15 + ADR-0078 (candidate; mints
// at v0a close):
//   - **SI base everywhere internally.** The `.value` field is always in
//     SI base units for the tagged dimension (meters for Length, kg for
//     Mass, seconds for Time, radians for Angle, kelvin for Temperature,
//     ampere for Current, candela for LuminousI, mole for Amount).
//   - **Precision tier (f32/f64) is orthogonal to dimension.** Same
//     dimensional type system; scalar precision varies per consumer
//     (games + runtime use f32; aerospace large-world + CAD micrometer +
//     scientific computing use f64).
//   - **Layout pin.** sizeof(Quantity<D, T>) == sizeof(T) — single-member,
//     no padding. Quantity<D, T> is is_standard_layout_v +
//     is_trivially_copyable_v. SIMD/GPU upload paths reach the raw scalar
//     via `.value` (zero-cost bit-equal reinterpret).
//   - **`.value` is publicly accessible** (no encapsulation overhead). The
//     type safety lives at the API surface; inside SIMD/GPU hot paths,
//     consumers reach `.value` raw. Wrapping into a getter would defeat
//     this.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>

#include <compare>
#include <type_traits>

namespace crd::units
{

// ===========================================================================
// Quantity<D, T>
// ===========================================================================

template <typename D, typename T = crd::f32>
struct Quantity
{
    static_assert(std::is_floating_point_v<T>,
                  "Quantity<D, T>: T must be a floating-point type (typically crd::f32 or crd::f64).");

    using dimension = D;
    using scalar    = T;

    // ALWAYS in SI base units for D. Publicly accessible per the
    // architectural pin — SIMD/GPU upload paths reach `.value` raw.
    T value;

    constexpr Quantity() noexcept : value(static_cast<T>(0)) {}
    explicit constexpr Quantity(T v) noexcept : value(v) {}

    // =======================================================================
    // Same-dimension arithmetic
    // =======================================================================

    [[nodiscard]] constexpr Quantity operator+(Quantity rhs) const noexcept
    {
        return Quantity{value + rhs.value};
    }
    [[nodiscard]] constexpr Quantity operator-(Quantity rhs) const noexcept
    {
        return Quantity{value - rhs.value};
    }
    [[nodiscard]] constexpr Quantity operator-() const noexcept { return Quantity{-value}; }

    constexpr Quantity& operator+=(Quantity rhs) noexcept
    {
        value += rhs.value;
        return *this;
    }
    constexpr Quantity& operator-=(Quantity rhs) noexcept
    {
        value -= rhs.value;
        return *this;
    }

    // =======================================================================
    // Scalar multiplication / division (same-dimension result)
    // =======================================================================

    [[nodiscard]] constexpr Quantity operator*(T s) const noexcept { return Quantity{value * s}; }
    [[nodiscard]] constexpr Quantity operator/(T s) const noexcept { return Quantity{value / s}; }

    constexpr Quantity& operator*=(T s) noexcept
    {
        value *= s;
        return *this;
    }
    constexpr Quantity& operator/=(T s) noexcept
    {
        value /= s;
        return *this;
    }

    // =======================================================================
    // Comparison (same-dimension only — different-dimension comparison is a
    // compile error by virtue of the parameter type)
    // =======================================================================

    [[nodiscard]] constexpr bool operator==(Quantity rhs) const noexcept { return value == rhs.value; }
    [[nodiscard]] constexpr auto operator<=>(Quantity rhs) const noexcept { return value <=> rhs.value; }
};

// ===========================================================================
// Free-function operators — cross-dimension product / quotient + scalar-left
// ===========================================================================

// Scalar * Quantity (commutative form of Quantity::operator*).
template <typename D, typename T>
[[nodiscard]] constexpr Quantity<D, T> operator*(T s, Quantity<D, T> q) noexcept
{
    return Quantity<D, T>{s * q.value};
}

// Cross-dimension multiplication:
//     Quantity<D1, T> * Quantity<D2, T> -> Quantity<DimMul<D1, D2>, T>
// e.g. Mass * Acceleration -> Force.
template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Quantity<DimMul<D1, D2>, T>
operator*(Quantity<D1, T> a, Quantity<D2, T> b) noexcept
{
    return Quantity<DimMul<D1, D2>, T>{a.value * b.value};
}

// Cross-dimension division:
//     Quantity<D1, T> / Quantity<D2, T> -> Quantity<DimDiv<D1, D2>, T>
// e.g. Length / Time -> Velocity.
template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Quantity<DimDiv<D1, D2>, T>
operator/(Quantity<D1, T> a, Quantity<D2, T> b) noexcept
{
    return Quantity<DimDiv<D1, D2>, T>{a.value / b.value};
}

// Scalar / Quantity -> Quantity<DimInv<D>, T>.
// e.g. 1.0 / Time -> Frequency.
template <typename D, typename T>
[[nodiscard]] constexpr Quantity<DimInv<D>, T> operator/(T s, Quantity<D, T> q) noexcept
{
    return Quantity<DimInv<D>, T>{s / q.value};
}

// ===========================================================================
// Layout pins — compile-time enforced
// ===========================================================================
//
// If any of these fail, the SIMD/GPU upload paths are broken (consumers
// rely on Quantity<D, T> being layout-equal to T). The static_asserts fire
// at parse-time of this header — no consumer can compile against a
// regressed layout.

static_assert(sizeof(Quantity<dim::Length, crd::f32>)  == sizeof(crd::f32),
              "Quantity<D, f32>: sizeof must equal sizeof(f32) — single-member, no padding.");
static_assert(sizeof(Quantity<dim::Length, crd::f64>)  == sizeof(crd::f64),
              "Quantity<D, f64>: sizeof must equal sizeof(f64).");
static_assert(alignof(Quantity<dim::Length, crd::f32>) == alignof(crd::f32),
              "Quantity<D, f32>: alignof must equal alignof(f32).");
static_assert(alignof(Quantity<dim::Length, crd::f64>) == alignof(crd::f64),
              "Quantity<D, f64>: alignof must equal alignof(f64).");
static_assert(std::is_standard_layout_v<Quantity<dim::Length, crd::f32>>,
              "Quantity<D, T>: must be standard-layout for safe SIMD/GPU reinterpret.");
static_assert(std::is_trivially_copyable_v<Quantity<dim::Length, crd::f32>>,
              "Quantity<D, T>: must be trivially-copyable for memcpy / GPU upload.");
static_assert(std::is_standard_layout_v<Quantity<dim::Mass, crd::f64>>);
static_assert(std::is_trivially_copyable_v<Quantity<dim::Time, crd::f32>>);
static_assert(std::is_standard_layout_v<Quantity<dim::Angle, crd::f32>>);

} // namespace crd::units
