#pragma once

// ---------------------------------------------------------------------------
// crd-units -- Layer 2: AffineUnit + Temperature/TemperatureDelta
// (Phase 3.1.7.5 v0a-3).
//
// Affine units have both scale and offset (Celsius = K - 273.15). Naive
// libraries treat them as linear, then `C_a - C_b` returns the wrong K-delta.
// Cerid closes this at compile time with two distinct types:
//
//   Temperature<T>      -- ABSOLUTE (has an origin at 0 K).
//                          Constructed from Celsius / Fahrenheit / Kelvin
//                          literals. Cannot be added to another Temperature.
//                          Cannot be scaled by a scalar. Cannot be negated.
//
//   TemperatureDelta<T> -- RELATIVE (a difference of two temperatures).
//                          A regular Quantity<dim::Temperature, T>. Supports
//                          full arithmetic (add, sub, scale, negate).
//
//   Temperature - Temperature -> TemperatureDelta   (subtraction strips offset)
//   Temperature + TemperatureDelta -> Temperature   (translate the absolute)
//   Temperature - TemperatureDelta -> Temperature
//   TemperatureDelta +/- TemperatureDelta -> TemperatureDelta
//
//   Temperature + Temperature -> COMPILE ERROR (semantically meaningless)
//   -Temperature              -> COMPILE ERROR (negate an absolute scale)
//   Temperature * scalar      -> COMPILE ERROR
//
// The same "absolute vs delta" pattern is reserved (not shipped v0a) for:
//   - Pressure / PressureDelta (gauge vs absolute -- for weather, aerospace
//     cabin, tire, CFD inlet)
//   - Datetime / Duration (if Cerid grows a calendar/datetime module)
//   - Voltage / VoltageDelta (rarely needed; potential differences are what
//     we measure)
//
// AffineUnit<Dim, ScaleRatio, OffsetRatio> declares the conversion:
//     SI_value = scale * unit_value + offset
//
// Example: Celsius has scale=1 and offset=273.15:
//     Kelvin = 1 * Celsius + 273.15
// Fahrenheit has scale=5/9 and offset=(5/9 * 459.67) = (459.67*5)/(180*1):
//     Kelvin = (5/9) * (Fahrenheit + 459.67)
//            = (5/9) * Fahrenheit + (5/9 * 459.67)
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>
#include <crd/units/quantity.hpp>

#include <ratio>
#include <type_traits>

namespace crd::units
{

// ===========================================================================
// AffineUnit<Dim, ScaleRatio, OffsetRatio>
// ===========================================================================
//
// SI_value = scale * unit_value + offset
//
// For temperature, ScaleRatio and OffsetRatio are std::ratio types giving
// the conversion factors. For Celsius: scale = 1/1, offset = 27315/100.
// For Fahrenheit: scale = 5/9, offset = (5/9 * 459.67) computed via
// integer-rational form (45967/180).

template <typename Dim, typename ScaleRatio, typename OffsetRatio>
struct AffineUnit
{
    using dimension     = Dim;
    using scale_ratio   = ScaleRatio;
    using offset_ratio  = OffsetRatio;
    static constexpr crd::f64 scale =
        static_cast<crd::f64>(ScaleRatio::num) / static_cast<crd::f64>(ScaleRatio::den);
    static constexpr crd::f64 offset =
        static_cast<crd::f64>(OffsetRatio::num) / static_cast<crd::f64>(OffsetRatio::den);
    static constexpr bool is_affine = true;
};

// Named affine units for temperature.
//   Celsius:    SI_K = 1 * C + 273.15
//   Fahrenheit: SI_K = (5/9) * F + (5/9 * 459.67) = (5/9)*F + 45967/180
//   Rankine:    SI_K = (5/9) * R + 0
using Celsius    = AffineUnit<dim::Temperature, std::ratio<1, 1>,    std::ratio<27'315, 100>>;
using Fahrenheit = AffineUnit<dim::Temperature, std::ratio<5, 9>,    std::ratio<45'967, 180>>;
using Rankine    = AffineUnit<dim::Temperature, std::ratio<5, 9>,    std::ratio<0, 1>>;

// ===========================================================================
// AbsoluteQuantity<D, T> -- a Quantity with an origin (no arithmetic between
// two absolutes).
// ===========================================================================
//
// Used for temperature where 0 has a fixed physical meaning (absolute zero,
// at 0 K). Adding two absolute temperatures is semantically meaningless.

template <typename D, typename T = crd::f32>
struct AbsoluteQuantity
{
    static_assert(std::is_floating_point_v<T>,
                  "AbsoluteQuantity<D, T>: T must be a floating-point type.");

    using dimension = D;
    using scalar    = T;

    // ALWAYS in SI base (kelvin for Temperature).
    T value;

    constexpr AbsoluteQuantity() noexcept : value(static_cast<T>(0)) {}
    explicit constexpr AbsoluteQuantity(T v) noexcept : value(v) {}

    // Comparison (same-dimension only).
    [[nodiscard]] constexpr bool operator==(AbsoluteQuantity rhs) const noexcept
    {
        return value == rhs.value;
    }
    [[nodiscard]] constexpr auto operator<=>(AbsoluteQuantity rhs) const noexcept
    {
        return value <=> rhs.value;
    }
};

// ===========================================================================
// Allowed arithmetic on Temperature / TemperatureDelta
// ===========================================================================
//
// AbsoluteQuantity<D, T> - AbsoluteQuantity<D, T>  -> Quantity<D, T>  (delta)
// AbsoluteQuantity<D, T> + Quantity<D, T>          -> AbsoluteQuantity<D, T>
// AbsoluteQuantity<D, T> - Quantity<D, T>          -> AbsoluteQuantity<D, T>

template <typename D, typename T>
[[nodiscard]] constexpr Quantity<D, T> operator-(AbsoluteQuantity<D, T> a,
                                                   AbsoluteQuantity<D, T> b) noexcept
{
    return Quantity<D, T>{a.value - b.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr AbsoluteQuantity<D, T> operator+(AbsoluteQuantity<D, T> a,
                                                           Quantity<D, T> delta) noexcept
{
    return AbsoluteQuantity<D, T>{a.value + delta.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr AbsoluteQuantity<D, T> operator+(Quantity<D, T> delta,
                                                           AbsoluteQuantity<D, T> a) noexcept
{
    return AbsoluteQuantity<D, T>{a.value + delta.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr AbsoluteQuantity<D, T> operator-(AbsoluteQuantity<D, T> a,
                                                           Quantity<D, T> delta) noexcept
{
    return AbsoluteQuantity<D, T>{a.value - delta.value};
}

// ===========================================================================
// Type aliases
// ===========================================================================

template <typename T = crd::f32>
using Temperature = AbsoluteQuantity<dim::Temperature, T>;

template <typename T = crd::f32>
using TemperatureDelta = Quantity<dim::Temperature, T>;

// ===========================================================================
// Construction + boundary egress for AbsoluteQuantity (affine units)
// ===========================================================================
//
// temperature_from<Unit>(scalar_value) -- construct AbsoluteQuantity from a
// scalar in the given affine unit (Celsius / Fahrenheit / Kelvin / Rankine).
//
// value_in_temperature<Unit>(temperature) -- convert AbsoluteQuantity to a
// scalar in the given affine unit.

template <typename Unit, typename T>
[[nodiscard]] constexpr AbsoluteQuantity<typename Unit::dimension, T>
temperature_from(T value_in_unit) noexcept
{
    if constexpr (requires { Unit::is_affine; })
    {
        return AbsoluteQuantity<typename Unit::dimension, T>{
            static_cast<T>(Unit::scale) * value_in_unit + static_cast<T>(Unit::offset)};
    }
    else
    {
        // Linear (Kelvin = LinearUnit<dim::Temperature, std::ratio<1>>).
        return AbsoluteQuantity<typename Unit::dimension, T>{
            value_in_unit * static_cast<T>(Unit::factor)};
    }
}

template <typename Unit, typename D, typename T>
[[nodiscard]] constexpr T value_in_temperature(AbsoluteQuantity<D, T> q) noexcept
{
    static_assert(dim_equal_v<typename Unit::dimension, D>,
                  "value_in_temperature<Unit>: dimension mismatch.");
    if constexpr (requires { Unit::is_affine; })
    {
        return (q.value - static_cast<T>(Unit::offset)) / static_cast<T>(Unit::scale);
    }
    else
    {
        return q.value / static_cast<T>(Unit::factor);
    }
}

} // namespace crd::units
