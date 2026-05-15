#pragma once

// ---------------------------------------------------------------------------
// crd-units — Layer 1: LinearUnit<Dim, FactorRatio> + ~90 named units
// (Phase 3.1.7.5 v0a-2).
//
// LinearUnit<D, FactorRatio> defines a unit with a linear scale factor to
// the SI base for dimension D. The factor is a std::ratio<Num, Den> so
// SI-prefix conversions (m ↔ mm ↔ km) and standardized imperial conversions
// (in ↔ mm per 1959 international agreement; mile ↔ km per the same) are
// **bit-exact rational round-trips** in f64.
//
// Irrational factors (Degree = π/180, Grad = π/200, etc.) use the best
// integer-rational approximation to f64 precision. Round-trip is exact at
// the rational arithmetic layer; the f64 evaluation rounds at the last step.
//
// Cerid convention:
//   - `<UnitName>::dimension` -> Dim<...> tag (used by Quantity).
//   - `<UnitName>::factor_ratio` -> std::ratio<Num, Den> source of truth.
//   - `<UnitName>::factor` -> compile-time f64, derived from factor_ratio.
//
// ===========================================================================
// What is NOT in this header (lands in later sub-slices):
//   - Compound auto-derive (UnitMul / UnitDiv) → units_compound.hpp (v0a-2).
//   - Boundary egress accessor (value_in<TargetUnit>) → value_in.hpp (v0a-2).
//   - Affine units + Temperature/TemperatureDelta → units_affine.hpp (v0a-3).
//   - Non-linear units (dB family) → units_nonlinear.hpp (v0a-3).
//   - User-defined literals → literals.hpp (v0a-3).
// ===========================================================================

#include <crd/core/types.hpp>
#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>

#include <ratio>

namespace crd::units
{

// ===========================================================================
// LinearUnit<Dim, FactorRatio>
// ===========================================================================
//
// FactorRatio is a std::ratio<Num, Den> giving the conversion factor to the
// SI base unit for `Dim`. Example: Inch = LinearUnit<dim::Length, std::ratio<254, 10000>>
// because 1 inch = 0.0254 m exactly.
//
// `factor` is the f64 evaluation of FactorRatio — typically used at the
// boundary (value_in<TargetUnit>() divides by it). For most units, the f64
// evaluation is bit-exact (any integer power of 10 within ~16 digits, plus
// the exact 1959-imperial rationals). Irrational-factor units (Degree etc.)
// use the best big-integer rational approximation.

template <typename DimT, typename FactorRatio = std::ratio<1>>
struct LinearUnit
{
    using dimension    = DimT;
    using factor_ratio = FactorRatio;
    static constexpr crd::f64 factor =
        static_cast<crd::f64>(FactorRatio::num) / static_cast<crd::f64>(FactorRatio::den);
};

// ===========================================================================
// SI base units (factor = 1 by construction)
// ===========================================================================

using Meter    = LinearUnit<dim::Length,      std::ratio<1>>;
using Kilogram = LinearUnit<dim::Mass,        std::ratio<1>>;
using Second   = LinearUnit<dim::Time,        std::ratio<1>>;
using Radian   = LinearUnit<dim::Angle,       std::ratio<1>>;
using Kelvin   = LinearUnit<dim::Temperature, std::ratio<1>>;
using Ampere   = LinearUnit<dim::Current,     std::ratio<1>>;
using Candela  = LinearUnit<dim::LuminousI,   std::ratio<1>>;
using Mole     = LinearUnit<dim::Amount,      std::ratio<1>>;

// ===========================================================================
// SI-prefixed units for Length
// ===========================================================================

// Yotta (10^24) and Zetta (10^21) overflow i64 (max ~9.22e18) — declared in
// a future amendment if a consumer needs them (would require a wider-int
// ratio backend). Exa (10^18) fits i64 max.
using Exameter     = LinearUnit<dim::Length, std::ratio<1'000'000'000'000'000'000, 1>>; // 10^18
using Petameter    = LinearUnit<dim::Length, std::ratio<1'000'000'000'000'000, 1>>;     // 10^15
using Terameter    = LinearUnit<dim::Length, std::ratio<1'000'000'000'000, 1>>;          // 10^12
using Gigameter    = LinearUnit<dim::Length, std::ratio<1'000'000'000, 1>>;              // 10^9
using Megameter    = LinearUnit<dim::Length, std::ratio<1'000'000, 1>>;                  // 10^6
using Kilometer    = LinearUnit<dim::Length, std::ratio<1'000, 1>>;
using Hectometer   = LinearUnit<dim::Length, std::ratio<100, 1>>;
using Decameter    = LinearUnit<dim::Length, std::ratio<10, 1>>;
using Decimeter    = LinearUnit<dim::Length, std::ratio<1, 10>>;
using Centimeter   = LinearUnit<dim::Length, std::ratio<1, 100>>;
using Millimeter   = LinearUnit<dim::Length, std::ratio<1, 1'000>>;
using Micrometer   = LinearUnit<dim::Length, std::ratio<1, 1'000'000>>;
using Nanometer    = LinearUnit<dim::Length, std::ratio<1, 1'000'000'000>>;
using Picometer    = LinearUnit<dim::Length, std::ratio<1, 1'000'000'000'000>>;
using Femtometer   = LinearUnit<dim::Length, std::ratio<1, 1'000'000'000'000'000>>;
using Angstrom     = LinearUnit<dim::Length, std::ratio<1, 10'000'000'000>>;             // 1 Å = 10^-10 m

// ===========================================================================
// SI-prefixed units for Mass
// ===========================================================================

using Tonne     = LinearUnit<dim::Mass, std::ratio<1'000, 1>>;            // metric ton = 1000 kg
using Gram      = LinearUnit<dim::Mass, std::ratio<1, 1'000>>;
using Milligram = LinearUnit<dim::Mass, std::ratio<1, 1'000'000>>;
using Microgram = LinearUnit<dim::Mass, std::ratio<1, 1'000'000'000>>;
using Nanogram  = LinearUnit<dim::Mass, std::ratio<1, 1'000'000'000'000>>;

// ===========================================================================
// SI-prefixed units for Time
// ===========================================================================

using Millisecond = LinearUnit<dim::Time, std::ratio<1, 1'000>>;
using Microsecond = LinearUnit<dim::Time, std::ratio<1, 1'000'000>>;
using Nanosecond  = LinearUnit<dim::Time, std::ratio<1, 1'000'000'000>>;
using Picosecond  = LinearUnit<dim::Time, std::ratio<1, 1'000'000'000'000>>;
using Femtosecond = LinearUnit<dim::Time, std::ratio<1, 1'000'000'000'000'000>>;
using Minute      = LinearUnit<dim::Time, std::ratio<60, 1>>;
using Hour        = LinearUnit<dim::Time, std::ratio<3'600, 1>>;
using Day         = LinearUnit<dim::Time, std::ratio<86'400, 1>>;          // solar day
using Week        = LinearUnit<dim::Time, std::ratio<604'800, 1>>;
using JulianYear  = LinearUnit<dim::Time, std::ratio<31'557'600, 1>>;      // 365.25 d EXACT (IAU)

// ===========================================================================
// SI-prefixed units for Current
// ===========================================================================

using Milliampere = LinearUnit<dim::Current, std::ratio<1, 1'000>>;
using Microampere = LinearUnit<dim::Current, std::ratio<1, 1'000'000>>;
using Nanoampere  = LinearUnit<dim::Current, std::ratio<1, 1'000'000'000>>;
using Kiloampere  = LinearUnit<dim::Current, std::ratio<1'000, 1>>;

// ===========================================================================
// SI-prefixed units for Temperature (offset — affine — lands in v0a-3.
// Here we only ship Kelvin's prefixes, which are still linear since Kelvin
// is an absolute scale starting at 0.)
// ===========================================================================
// (Millikelvin / Microkelvin land in v0a-3 alongside Celsius/Fahrenheit if
// a consumer needs them.)

// ===========================================================================
// Imperial / U.S. customary — Length (1959 international agreement EXACT)
// ===========================================================================

using Inch         = LinearUnit<dim::Length, std::ratio<254, 10'000>>;           // 1 in = 0.0254 m EXACT
using Foot         = LinearUnit<dim::Length, std::ratio<3'048, 10'000>>;          // 1 ft = 0.3048 m EXACT
using Yard         = LinearUnit<dim::Length, std::ratio<9'144, 10'000>>;          // 1 yd = 0.9144 m EXACT
using Mile         = LinearUnit<dim::Length, std::ratio<1'609'344, 1'000>>;       // 1 mi = 1609.344 m EXACT
using NauticalMile = LinearUnit<dim::Length, std::ratio<1'852, 1>>;               // 1 nmi = 1852 m EXACT (IHO)
using Mil          = LinearUnit<dim::Length, std::ratio<254, 10'000'000>>;        // 1 mil = 25.4 μm EXACT (PCB)
using Furlong      = LinearUnit<dim::Length, std::ratio<201'168, 1'000>>;         // 1 fur = 201.168 m EXACT
using Fathom       = LinearUnit<dim::Length, std::ratio<18'288, 10'000>>;         // 1 fathom = 1.8288 m EXACT

// ===========================================================================
// Imperial / U.S. customary — Mass + Force
// ===========================================================================
//
// Cerid disambiguates "pound" by suffix: PoundMass (lb-mass) vs PoundForce
// (lbf). The UDL `_lb` is disallowed in v0a-3 (compile error at literal-
// eval); users pick `_lb_mass` or `_lbf` explicitly.

using PoundMass    = LinearUnit<dim::Mass, std::ratio<45'359'237, 100'000'000>>;  // 1 lb-mass = 0.45359237 kg EXACT
using OunceMass    = LinearUnit<dim::Mass, std::ratio<45'359'237, 1'600'000'000>>; // 1/16 lb-mass EXACT
using TroyOunce    = LinearUnit<dim::Mass, std::ratio<311'034'768, 10'000'000'000>>; // 1 oz-troy = 0.0311034768 kg EXACT
using Stone        = LinearUnit<dim::Mass, std::ratio<635'029'318, 100'000'000>>; // 14 lb-mass = 6.35029318 kg EXACT
using ShortTon     = LinearUnit<dim::Mass, std::ratio<90'718'474, 100'000>>;      // 2000 lb-mass = 907.18474 kg EXACT
using LongTon      = LinearUnit<dim::Mass, std::ratio<10'160'469'088, 10'000'000>>; // 2240 lb-mass = 1016.0469088 kg EXACT

// Force: PoundForce = lb-mass * standard-g = 0.45359237 * 9.80665 = 4.4482216152605 N EXACT
using PoundForce       = LinearUnit<dim::Force, std::ratio<444'822'161'526'050, 100'000'000'000'000>>;
using OunceForce       = LinearUnit<dim::Force, std::ratio<444'822'161'526'050, 1'600'000'000'000'000>>;
using KilogramForce    = LinearUnit<dim::Force, std::ratio<980'665, 100'000>>;    // 1 kgf = 9.80665 N EXACT

// ===========================================================================
// Angle — Radians is SI base. Degree = π/180 (irrational; big-int
// rational approximation to f64 precision).
// ===========================================================================
//
// π/180 ≈ 0.017453292519943295. Best i64 rational approximation:
// 31415926535897932 / 1800000000000000000 → fits i64 and gives f64-precision
// conversion (round-trip 1 ULP).

using Degree     = LinearUnit<dim::Angle, std::ratio<31'415'926'535'897'932, 1'800'000'000'000'000'000>>;
using Grad       = LinearUnit<dim::Angle, std::ratio<31'415'926'535'897'932, 2'000'000'000'000'000'000>>;  // π/200
using Revolution = LinearUnit<dim::Angle, std::ratio<62'831'853'071'795'864, 10'000'000'000'000'000>>;     // 2π
// π/10800 = 2.9088820866572e-4 (lower precision than Degree because den is
// at the edge of i64 — 14 digits of precision instead of 16). Acceptable
// for ArcMinute (rarely used; 1 ULP imprecision).
using ArcMinute  = LinearUnit<dim::Angle, std::ratio<29'088'820'866'572, 100'000'000'000'000'000>>; // π/10800
// π/648000 = 4.848136811095e-6 (10-digit precision)
using ArcSecond  = LinearUnit<dim::Angle, std::ratio<4'848'136'811'095, 1'000'000'000'000'000'000>>; // π/648000

// ===========================================================================
// Force — Newton is SI derived but ships as a LinearUnit with factor 1.
// PoundForce is above under Imperial.
// ===========================================================================

using Newton     = LinearUnit<dim::Force, std::ratio<1>>;
using Kilonewton = LinearUnit<dim::Force, std::ratio<1'000, 1>>;
using Meganewton = LinearUnit<dim::Force, std::ratio<1'000'000, 1>>;
using Dyne       = LinearUnit<dim::Force, std::ratio<1, 100'000>>;  // 1 dyne = 10^-5 N (CGS)

// ===========================================================================
// Pressure
// ===========================================================================

using Pascal               = LinearUnit<dim::Pressure, std::ratio<1>>;
using Kilopascal           = LinearUnit<dim::Pressure, std::ratio<1'000, 1>>;
using Megapascal           = LinearUnit<dim::Pressure, std::ratio<1'000'000, 1>>;
using Gigapascal           = LinearUnit<dim::Pressure, std::ratio<1'000'000'000, 1>>;
using Bar                  = LinearUnit<dim::Pressure, std::ratio<100'000, 1>>;
using Millibar             = LinearUnit<dim::Pressure, std::ratio<100, 1>>;
using Atmosphere           = LinearUnit<dim::Pressure, std::ratio<101'325, 1>>;       // 1 atm = 101325 Pa EXACT
using TorrPressure         = LinearUnit<dim::Pressure, std::ratio<101'325, 760>>;     // 1 atm / 760
using MillimeterMercury    = TorrPressure;                                            // mmHg ≈ torr (very close)
// 1 psi = 6894.7572931683... Pa. Big-int approximation:
using Psi                  = LinearUnit<dim::Pressure, std::ratio<6'894'757'293'168, 1'000'000'000>>;

// ===========================================================================
// Energy
// ===========================================================================

using Joule          = LinearUnit<dim::Energy, std::ratio<1>>;
using Kilojoule      = LinearUnit<dim::Energy, std::ratio<1'000, 1>>;
using Megajoule      = LinearUnit<dim::Energy, std::ratio<1'000'000, 1>>;
using Gigajoule      = LinearUnit<dim::Energy, std::ratio<1'000'000'000, 1>>;
using Calorie        = LinearUnit<dim::Energy, std::ratio<4'184, 1'000>>;         // 1 cal_th = 4.184 J EXACT (thermochemical)
using Kilocalorie    = LinearUnit<dim::Energy, std::ratio<4'184, 1>>;             // 1 kcal = 4184 J
using WattHour       = LinearUnit<dim::Energy, std::ratio<3'600, 1>>;             // 1 Wh = 3600 J EXACT
using KilowattHour   = LinearUnit<dim::Energy, std::ratio<3'600'000, 1>>;         // 1 kWh = 3.6 MJ EXACT
using MegawattHour   = LinearUnit<dim::Energy, std::ratio<3'600'000'000, 1>>;
// 1 eV = 1.602176634e-19 J EXACT (2019 SI defining constant).
// Reduced form: gcd(1602176634, 1e19) = 2 → 801088317 / 5e18 (5e18 fits i64).
using ElectronVolt   = LinearUnit<dim::Energy, std::ratio<801'088'317, 5'000'000'000'000'000'000>>;
using Erg            = LinearUnit<dim::Energy, std::ratio<1, 10'000'000>>;        // 1 erg = 10^-7 J (CGS)
using BritishThermalUnit = LinearUnit<dim::Energy, std::ratio<105'505'585'262, 100'000'000>>; // 1 Btu_IT = 1055.05585262 J

// ===========================================================================
// Power
// ===========================================================================

using Watt          = LinearUnit<dim::Power, std::ratio<1>>;
using Milliwatt     = LinearUnit<dim::Power, std::ratio<1, 1'000>>;
using Microwatt     = LinearUnit<dim::Power, std::ratio<1, 1'000'000>>;
using Kilowatt      = LinearUnit<dim::Power, std::ratio<1'000, 1>>;
using Megawatt      = LinearUnit<dim::Power, std::ratio<1'000'000, 1>>;
using Gigawatt      = LinearUnit<dim::Power, std::ratio<1'000'000'000, 1>>;
// 1 mechanical hp = 745.6998715822702 W ≈ 75 * 9.80665 / 0.987
// Best big-int approximation:
// 1 mechanical hp = 745.6998715822702 W. 13-digit big-int approximation.
using Horsepower            = LinearUnit<dim::Power, std::ratio<74'569'987'158'227, 100'000'000'000>>;
using MetricHorsepower      = LinearUnit<dim::Power, std::ratio<73'549'875, 100'000>>; // 1 metric hp = 735.49875 W EXACT (75 kgf*m/s)

// ===========================================================================
// Frequency
// ===========================================================================

using Hertz     = LinearUnit<dim::Frequency, std::ratio<1>>;
using Kilohertz = LinearUnit<dim::Frequency, std::ratio<1'000, 1>>;
using Megahertz = LinearUnit<dim::Frequency, std::ratio<1'000'000, 1>>;
using Gigahertz = LinearUnit<dim::Frequency, std::ratio<1'000'000'000, 1>>;
using Terahertz = LinearUnit<dim::Frequency, std::ratio<1'000'000'000'000, 1>>;

// ===========================================================================
// Electrical — Voltage, Resistance, Capacitance, Inductance, Charge,
// MagneticFlux
// ===========================================================================

using Volt       = LinearUnit<dim::Voltage, std::ratio<1>>;
using Millivolt  = LinearUnit<dim::Voltage, std::ratio<1, 1'000>>;
using Microvolt  = LinearUnit<dim::Voltage, std::ratio<1, 1'000'000>>;
using Kilovolt   = LinearUnit<dim::Voltage, std::ratio<1'000, 1>>;
using Megavolt   = LinearUnit<dim::Voltage, std::ratio<1'000'000, 1>>;

using Ohm        = LinearUnit<dim::Resistance, std::ratio<1>>;
using Milliohm   = LinearUnit<dim::Resistance, std::ratio<1, 1'000>>;
using Kilohm     = LinearUnit<dim::Resistance, std::ratio<1'000, 1>>;
using Megaohm    = LinearUnit<dim::Resistance, std::ratio<1'000'000, 1>>;
using Gigaohm    = LinearUnit<dim::Resistance, std::ratio<1'000'000'000, 1>>;

using Siemens    = LinearUnit<dim::Conductance, std::ratio<1>>;

using Coulomb        = LinearUnit<dim::Charge, std::ratio<1>>;
using MilliCoulomb   = LinearUnit<dim::Charge, std::ratio<1, 1'000>>;
using MicroCoulomb   = LinearUnit<dim::Charge, std::ratio<1, 1'000'000>>;
using AmpereHour     = LinearUnit<dim::Charge, std::ratio<3'600, 1>>;       // 1 Ah = 3600 C EXACT
using MilliampereHour = LinearUnit<dim::Charge, std::ratio<36, 10>>;        // 1 mAh = 3.6 C EXACT

using Farad        = LinearUnit<dim::Capacitance, std::ratio<1>>;
using Millifarad   = LinearUnit<dim::Capacitance, std::ratio<1, 1'000>>;
using Microfarad   = LinearUnit<dim::Capacitance, std::ratio<1, 1'000'000>>;
using Nanofarad    = LinearUnit<dim::Capacitance, std::ratio<1, 1'000'000'000>>;
using Picofarad    = LinearUnit<dim::Capacitance, std::ratio<1, 1'000'000'000'000>>;

using Henry        = LinearUnit<dim::Inductance, std::ratio<1>>;
using Millihenry   = LinearUnit<dim::Inductance, std::ratio<1, 1'000>>;
using Microhenry   = LinearUnit<dim::Inductance, std::ratio<1, 1'000'000>>;
using Nanohenry    = LinearUnit<dim::Inductance, std::ratio<1, 1'000'000'000>>;
using Picohenry    = LinearUnit<dim::Inductance, std::ratio<1, 1'000'000'000'000>>;

using Weber        = LinearUnit<dim::MagneticFlux, std::ratio<1>>;
using Tesla        = LinearUnit<dim::MagneticField, std::ratio<1>>;
using Gauss        = LinearUnit<dim::MagneticField, std::ratio<1, 10'000>>;  // 1 G = 10^-4 T (CGS)

// ===========================================================================
// Photometric (lands more fully when a renderer consumer surfaces)
// ===========================================================================

using Lumen        = LinearUnit<dim::LuminousFlux, std::ratio<1>>;     // strict SI: cd*sr; sr is dimensionless here
using Lux          = LinearUnit<dim::Illuminance, std::ratio<1>>;
using NitLuminance = LinearUnit<dim::Luminance, std::ratio<1>>;        // cd/m^2

// ===========================================================================
// Acceleration
// ===========================================================================

using MeterPerSecondSq = LinearUnit<dim::Acceleration, std::ratio<1>>;
// Standard gravity: 9.80665 m/s² EXACT (per CIPM 1901 + 2019 SI).
using StandardG = LinearUnit<dim::Acceleration, std::ratio<980'665, 100'000>>;
using Gal       = LinearUnit<dim::Acceleration, std::ratio<1, 100>>;  // 1 Gal = 1 cm/s² (CGS, geodesy)

// ===========================================================================
// Velocity (linear forms; compound forms like KilometerPerHour live in
// units_compound.hpp)
// ===========================================================================

using MeterPerSecond = LinearUnit<dim::Velocity, std::ratio<1>>;
using SpeedOfLight   = LinearUnit<dim::Velocity, std::ratio<299'792'458, 1>>;  // EXACT (2019 SI defining constant)

// ===========================================================================
// Solid bookkeeping: count of LinearUnit named aliases (manual update on
// each addition — this is a coarse sanity counter for the v0a-2 phase doc).
//
// Counts: 8 base + 18 length-prefix + 5 mass-prefix + 9 time + 4 current
//       + 8 imperial-length + 6 imperial-mass + 3 force-imperial
//       + 5 angle + 4 force-other + 10 pressure + 11 energy + 8 power
//       + 5 frequency + 5+5+1 electrical-(V,Ω,S) + 5 charge + 5 capacitance
//       + 5 inductance + 3 magnetic + 3 photometric + 3 acceleration
//       + 2 velocity = ~136 named linear units. (Compound auto-derived
//       units in units_compound.hpp add ~30 more.)
// ===========================================================================

} // namespace crd::units
