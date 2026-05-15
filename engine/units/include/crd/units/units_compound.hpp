#pragma once

// ---------------------------------------------------------------------------
// crd-units — Layer 4: compound unit auto-derivation (Phase 3.1.7.5 v0a-2).
//
// UnitMul<A, B> / UnitDiv<Num, Den> / UnitPow<U, N> combine LinearUnit
// instances via std::ratio arithmetic at compile time. The result is a new
// LinearUnit-shaped type whose dimension comes from DimMul/DimDiv/DimPow
// of the operands' dimensions and whose factor_ratio comes from
// std::ratio_multiply / std::ratio_divide of the operands' factor_ratios.
//
// **The extensibility multiplier.** Adding ONE new base unit (e.g. a domain
// pack's `Mil = LinearUnit<dim::Length, std::ratio<254, 10'000'000>>`)
// unlocks N new compound units automatically — UnitDiv<Mil, Hour>,
// UnitMul<Mil, Mil>, UnitDiv<Volt, Mil>, etc. all become available with
// zero source-code burden.
//
// **Performance.** Cross-dimension product / quotient on Quantity<D1, T> *
// Quantity<D2, T> is one FP multiply at runtime (per quantity.hpp).
// The factor combination via std::ratio_multiply happens at COMPILE TIME —
// no runtime cost. Each compound unit's `factor` is a static constexpr f64
// derived from its compile-time-reduced factor_ratio.
//
// Example: 60.0_mph (a Velocity<f32> in m/s internally) converted via
// `.value_in<KilometerPerHour>()` is a single FP multiply at the boundary,
// where KilometerPerHour::factor was computed at compile time as
// `std::ratio_divide<Kilometer::factor_ratio, Hour::factor_ratio>` =
// 1000 / 3600 = 0.2777... .
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>
#include <crd/units/units_si.hpp>

#include <ratio>

namespace crd::units
{

// ===========================================================================
// UnitMul / UnitDiv / UnitPow
// ===========================================================================
//
// Each combines the source units' factor_ratios via std::ratio_multiply /
// std::ratio_divide and the source units' dimensions via DimMul / DimDiv /
// DimPow. The result is a new LinearUnit-shaped type — same surface as a
// hand-declared LinearUnit<Dim, std::ratio<>>, transparently
// substitutable.

template <typename A, typename B>
struct UnitMul
{
    using dimension    = DimMul<typename A::dimension, typename B::dimension>;
    using factor_ratio =
        std::ratio_multiply<typename A::factor_ratio, typename B::factor_ratio>;
    static constexpr crd::f64 factor =
        static_cast<crd::f64>(factor_ratio::num) / static_cast<crd::f64>(factor_ratio::den);
};

template <typename Num, typename Den>
struct UnitDiv
{
    using dimension    = DimDiv<typename Num::dimension, typename Den::dimension>;
    using factor_ratio =
        std::ratio_divide<typename Num::factor_ratio, typename Den::factor_ratio>;
    static constexpr crd::f64 factor =
        static_cast<crd::f64>(factor_ratio::num) / static_cast<crd::f64>(factor_ratio::den);
};

namespace detail
{
template <typename U, crd::i32 N>
struct UnitPowImpl
{
    using prev_type = typename UnitPowImpl<U, N - 1>::type;
    using type = UnitMul<U, prev_type>;
};

template <typename U>
struct UnitPowImpl<U, 0>
{
    using type = LinearUnit<dim::Dimensionless, std::ratio<1>>;
};

template <typename U>
struct UnitPowImpl<U, 1>
{
    using type = U;
};
} // namespace detail

template <typename U, crd::i32 N>
using UnitPow = typename detail::UnitPowImpl<U, N>::type;

// ===========================================================================
// Named compound units (the most-touched cross-dimension combinations)
// ===========================================================================
//
// These are convenience aliases — fully equivalent to writing the UnitMul /
// UnitDiv inline at the call site. Consumers can ALWAYS use the latter
// form for ad-hoc combinations (UnitDiv<Mile, Hour> at the call site is
// the same as MilePerHour declared here).

// Velocity — Length / Time
using KilometerPerHour  = UnitDiv<Kilometer, Hour>;
using MilePerHour       = UnitDiv<Mile, Hour>;
using Knot              = UnitDiv<NauticalMile, Hour>;
using FootPerSecond     = UnitDiv<Foot, Second>;
using MillimeterPerSec  = UnitDiv<Millimeter, Second>;
using MeterPerMinute    = UnitDiv<Meter, Minute>;
using KilometerPerSecond = UnitDiv<Kilometer, Second>;
using InchPerSecond     = UnitDiv<Inch, Second>;

// Acceleration — Length / Time²
using FootPerSecondSq   = UnitDiv<UnitDiv<Foot, Second>, Second>;
using KilometerPerHourPerSec = UnitDiv<KilometerPerHour, Second>;

// Angular Velocity — Angle / Time
using RadianPerSecond   = UnitDiv<Radian, Second>;
using DegreePerSecond   = UnitDiv<Degree, Second>;
using RPM               = UnitDiv<Revolution, Minute>;
using RPS               = UnitDiv<Revolution, Second>;

// Torque — Force * Length
using NewtonMeter       = UnitMul<Newton, Meter>;
using PoundForceFoot    = UnitMul<PoundForce, Foot>;
using PoundForceInch    = UnitMul<PoundForce, Inch>;
using KilogramForceMeter = UnitMul<KilogramForce, Meter>;

// Pressure-ish via compound (alternates to the LinearUnit named forms)
using NewtonPerSqMeter  = UnitDiv<Newton, UnitMul<Meter, Meter>>;
// Note: NewtonPerSqMeter has the same dimension + factor as Pascal — they
// are equivalent representations. The LinearUnit named `Pascal` is the
// canonical form for value_in<>.

// Density — Mass / Volume
using KilogramPerCubicMeter = UnitDiv<Kilogram, UnitPow<Meter, 3>>;
using GramPerCubicCentimeter = UnitDiv<Gram, UnitPow<Centimeter, 3>>;
using PoundMassPerCubicFoot  = UnitDiv<PoundMass, UnitPow<Foot, 3>>;

// Flow rate
using CubicMeterPerSecond   = UnitDiv<UnitPow<Meter, 3>, Second>;
using LiterPerSecond        = UnitDiv<UnitPow<Decimeter, 3>, Second>;  // 1 L = 1 dm^3
using KilogramPerSecond     = UnitDiv<Kilogram, Second>;

// Energy density / specific energy
using JoulePerKilogram      = UnitDiv<Joule, Kilogram>;
using JoulePerKilogramKelvin = UnitDiv<JoulePerKilogram, Kelvin>;
using WattPerMeter          = UnitDiv<Watt, Meter>;
using WattPerMeterKelvin    = UnitDiv<WattPerMeter, Kelvin>;
using WattPerSqMeter        = UnitDiv<Watt, UnitMul<Meter, Meter>>;

// Concentration / molar
using MolePerCubicMeter     = UnitDiv<Mole, UnitPow<Meter, 3>>;
using MolePerLiter          = UnitDiv<Mole, UnitPow<Decimeter, 3>>;

// Magnetic flux density alternate names
using TeslaCompound         = UnitDiv<Weber, UnitMul<Meter, Meter>>;
// Same as `Tesla` LinearUnit — equivalent form.

} // namespace crd::units
