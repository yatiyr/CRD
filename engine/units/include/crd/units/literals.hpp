#pragma once

// ---------------------------------------------------------------------------
// crd-units -- user-defined literals (Phase 3.1.7.5 v0a-3).
//
// 80+ ergonomic UDLs for the most-used units. Consumers `using namespace
// crd::units::literals;` to get them at translation-unit scope (or in a
// function scope for narrower visibility).
//
// **Ambiguous-literal policy.** Some unit names are ambiguous (`lb` could
// be mass or force; `oz` could be mass, troy mass, fluid US, or fluid
// imperial). Cerid disallows these at the literal site by NOT defining
// `_lb` / `_oz` UDLs. Users explicitly pick `_lb_mass` / `_lbf` /
// `_oz_mass` / `_oz_troy` / `_oz_fluid_us` / `_oz_fluid_imp` etc.
//
// **Literal types.** Each UDL provides both:
//   - long double form: `operator""_m(long double v)` for `1.5_m`
//   - integer form:     `operator""_m(unsigned long long v)` for `1_m`
//
// All return Quantity<D, f64> by default (f64 is the precision-agnostic
// choice; consumers cast to Quantity<D, f32> at the API surface if they
// want game-tier precision).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/units_affine.hpp>
#include <crd/units/units_compound.hpp>
#include <crd/units/units_nonlinear.hpp>
#include <crd/units/units_si.hpp>
#include <crd/units/value_in.hpp>

namespace crd::units::literals
{

// ===========================================================================
// Length
// ===========================================================================

#define CRD_UNITS_LENGTH_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Length, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Length, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_LENGTH_UDL(m,    Meter)
CRD_UNITS_LENGTH_UDL(km,   Kilometer)
CRD_UNITS_LENGTH_UDL(cm,   Centimeter)
CRD_UNITS_LENGTH_UDL(mm,   Millimeter)
CRD_UNITS_LENGTH_UDL(um,   Micrometer)
CRD_UNITS_LENGTH_UDL(nm,   Nanometer)
CRD_UNITS_LENGTH_UDL(in,   Inch)
CRD_UNITS_LENGTH_UDL(ft,   Foot)
CRD_UNITS_LENGTH_UDL(yd,   Yard)
CRD_UNITS_LENGTH_UDL(mi,   Mile)
CRD_UNITS_LENGTH_UDL(nmi,  NauticalMile)
CRD_UNITS_LENGTH_UDL(mil,  Mil)

#undef CRD_UNITS_LENGTH_UDL

// ===========================================================================
// Mass
// ===========================================================================

#define CRD_UNITS_MASS_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Mass, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Mass, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_MASS_UDL(kg,      Kilogram)
CRD_UNITS_MASS_UDL(g,       Gram)
CRD_UNITS_MASS_UDL(mg,      Milligram)
CRD_UNITS_MASS_UDL(tonne,   Tonne)
CRD_UNITS_MASS_UDL(lb_mass, PoundMass)
CRD_UNITS_MASS_UDL(oz_mass, OunceMass)

#undef CRD_UNITS_MASS_UDL

// NOTE: `_lb` and `_oz` deliberately NOT defined -- ambiguous between mass
// and force / between mass and fluid-volume. Compile error at literal site
// guides users to `_lb_mass` / `_lbf` / `_oz_mass` / etc.

// ===========================================================================
// Time
// ===========================================================================

#define CRD_UNITS_TIME_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Time, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Time, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_TIME_UDL(s,    Second)
CRD_UNITS_TIME_UDL(ms,   Millisecond)
CRD_UNITS_TIME_UDL(us,   Microsecond)
CRD_UNITS_TIME_UDL(ns,   Nanosecond)
CRD_UNITS_TIME_UDL(min,  Minute)
CRD_UNITS_TIME_UDL(h,    Hour)

#undef CRD_UNITS_TIME_UDL

// ===========================================================================
// Angle
// ===========================================================================

#define CRD_UNITS_ANGLE_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Angle, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Angle, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_ANGLE_UDL(rad,  Radian)
CRD_UNITS_ANGLE_UDL(deg,  Degree)
CRD_UNITS_ANGLE_UDL(grad, Grad)

#undef CRD_UNITS_ANGLE_UDL

// ===========================================================================
// Force
// ===========================================================================

#define CRD_UNITS_FORCE_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Force, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Force, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_FORCE_UDL(N,    Newton)
CRD_UNITS_FORCE_UDL(kN,   Kilonewton)
CRD_UNITS_FORCE_UDL(lbf,  PoundForce)
CRD_UNITS_FORCE_UDL(kgf,  KilogramForce)

#undef CRD_UNITS_FORCE_UDL

// ===========================================================================
// Pressure
// ===========================================================================

#define CRD_UNITS_PRESSURE_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Pressure, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Pressure, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_PRESSURE_UDL(Pa,   Pascal)
CRD_UNITS_PRESSURE_UDL(kPa,  Kilopascal)
CRD_UNITS_PRESSURE_UDL(MPa,  Megapascal)
CRD_UNITS_PRESSURE_UDL(bar,  Bar)
CRD_UNITS_PRESSURE_UDL(atm,  Atmosphere)
CRD_UNITS_PRESSURE_UDL(psi,  Psi)

#undef CRD_UNITS_PRESSURE_UDL

// ===========================================================================
// Energy + Power
// ===========================================================================

#define CRD_UNITS_ENERGY_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Energy, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Energy, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_ENERGY_UDL(J,    Joule)
CRD_UNITS_ENERGY_UDL(kJ,   Kilojoule)
CRD_UNITS_ENERGY_UDL(MJ,   Megajoule)
CRD_UNITS_ENERGY_UDL(kWh,  KilowattHour)
CRD_UNITS_ENERGY_UDL(cal,  Calorie)
CRD_UNITS_ENERGY_UDL(kcal, Kilocalorie)

#undef CRD_UNITS_ENERGY_UDL

#define CRD_UNITS_POWER_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Power, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Power, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_POWER_UDL(W,   Watt)
CRD_UNITS_POWER_UDL(mW,  Milliwatt)
CRD_UNITS_POWER_UDL(kW,  Kilowatt)
CRD_UNITS_POWER_UDL(MW,  Megawatt)
CRD_UNITS_POWER_UDL(hp,  Horsepower)

#undef CRD_UNITS_POWER_UDL

// ===========================================================================
// Frequency
// ===========================================================================

#define CRD_UNITS_FREQ_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Frequency, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Frequency, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_FREQ_UDL(Hz,   Hertz)
CRD_UNITS_FREQ_UDL(kHz,  Kilohertz)
CRD_UNITS_FREQ_UDL(MHz,  Megahertz)
CRD_UNITS_FREQ_UDL(GHz,  Gigahertz)

#undef CRD_UNITS_FREQ_UDL

// Angular velocity
[[nodiscard]] constexpr Quantity<dim::AngularVelocity, crd::f64> operator""_rpm(long double v) noexcept
{ return quantity_from<RPM>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::AngularVelocity, crd::f64> operator""_rpm(unsigned long long v) noexcept
{ return quantity_from<RPM>(static_cast<crd::f64>(v)); }

// ===========================================================================
// Electrical
// ===========================================================================

#define CRD_UNITS_VOLTAGE_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Voltage, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Voltage, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_VOLTAGE_UDL(V,    Volt)
CRD_UNITS_VOLTAGE_UDL(mV,   Millivolt)
CRD_UNITS_VOLTAGE_UDL(kV,   Kilovolt)

#undef CRD_UNITS_VOLTAGE_UDL

#define CRD_UNITS_CURRENT_UDL(name, unit) \
    [[nodiscard]] constexpr Quantity<dim::Current, crd::f64> operator""_##name(long double v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); } \
    [[nodiscard]] constexpr Quantity<dim::Current, crd::f64> operator""_##name(unsigned long long v) noexcept \
    { return quantity_from<unit>(static_cast<crd::f64>(v)); }

CRD_UNITS_CURRENT_UDL(A,    Ampere)
CRD_UNITS_CURRENT_UDL(mA,   Milliampere)
CRD_UNITS_CURRENT_UDL(uA,   Microampere)

#undef CRD_UNITS_CURRENT_UDL

// Resistance / Capacitance / Inductance (single-unit forms; prefixed via SI mul):
[[nodiscard]] constexpr Quantity<dim::Resistance, crd::f64> operator""_ohm(long double v) noexcept
{ return quantity_from<Ohm>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::Resistance, crd::f64> operator""_ohm(unsigned long long v) noexcept
{ return quantity_from<Ohm>(static_cast<crd::f64>(v)); }

[[nodiscard]] constexpr Quantity<dim::Capacitance, crd::f64> operator""_F(long double v) noexcept
{ return quantity_from<Farad>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::Capacitance, crd::f64> operator""_uF(long double v) noexcept
{ return quantity_from<Microfarad>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::Capacitance, crd::f64> operator""_nF(long double v) noexcept
{ return quantity_from<Nanofarad>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::Capacitance, crd::f64> operator""_pF(long double v) noexcept
{ return quantity_from<Picofarad>(static_cast<crd::f64>(v)); }

[[nodiscard]] constexpr Quantity<dim::Inductance, crd::f64> operator""_H(long double v) noexcept
{ return quantity_from<Henry>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::Inductance, crd::f64> operator""_mH(long double v) noexcept
{ return quantity_from<Millihenry>(static_cast<crd::f64>(v)); }
[[nodiscard]] constexpr Quantity<dim::Inductance, crd::f64> operator""_uH(long double v) noexcept
{ return quantity_from<Microhenry>(static_cast<crd::f64>(v)); }

// ===========================================================================
// Temperature (Layer 2 -- affine; returns AbsoluteQuantity)
// ===========================================================================

[[nodiscard]] inline Temperature<crd::f64> operator""_kelvin(long double v) noexcept
{ return temperature_from<Kelvin>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Temperature<crd::f64> operator""_kelvin(unsigned long long v) noexcept
{ return temperature_from<Kelvin>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Temperature<crd::f64> operator""_celsius(long double v) noexcept
{ return temperature_from<Celsius>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Temperature<crd::f64> operator""_celsius(unsigned long long v) noexcept
{ return temperature_from<Celsius>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Temperature<crd::f64> operator""_fahrenheit(long double v) noexcept
{ return temperature_from<Fahrenheit>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Temperature<crd::f64> operator""_fahrenheit(unsigned long long v) noexcept
{ return temperature_from<Fahrenheit>(static_cast<crd::f64>(v)); }

// ===========================================================================
// Non-linear (Layer 3 -- dB family, cents/semitones)
// ===========================================================================

[[nodiscard]] inline Quantity<dim::Pressure, crd::f64> operator""_dB_spl(long double v) noexcept
{ return quantity_from_nonlinear<DecibelSPL>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Quantity<dim::Pressure, crd::f64> operator""_dB_spl(unsigned long long v) noexcept
{ return quantity_from_nonlinear<DecibelSPL>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Quantity<dim::Voltage, crd::f64> operator""_dB_v(long double v) noexcept
{ return quantity_from_nonlinear<DecibelV>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Quantity<dim::Voltage, crd::f64> operator""_dB_v(unsigned long long v) noexcept
{ return quantity_from_nonlinear<DecibelV>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Quantity<dim::Power, crd::f64> operator""_dB_w(long double v) noexcept
{ return quantity_from_nonlinear<DecibelW>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Quantity<dim::Power, crd::f64> operator""_dB_w(unsigned long long v) noexcept
{ return quantity_from_nonlinear<DecibelW>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Quantity<dim::Power, crd::f64> operator""_dBm(long double v) noexcept
{ return quantity_from_nonlinear<DecibelMilliwatt>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Quantity<dim::Power, crd::f64> operator""_dBm(unsigned long long v) noexcept
{ return quantity_from_nonlinear<DecibelMilliwatt>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Quantity<dim::Frequency, crd::f64> operator""_cents(long double v) noexcept
{ return quantity_from_nonlinear<Cents>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Quantity<dim::Frequency, crd::f64> operator""_cents(unsigned long long v) noexcept
{ return quantity_from_nonlinear<Cents>(static_cast<crd::f64>(v)); }

[[nodiscard]] inline Quantity<dim::Frequency, crd::f64> operator""_semitones(long double v) noexcept
{ return quantity_from_nonlinear<Semitones>(static_cast<crd::f64>(v)); }
[[nodiscard]] inline Quantity<dim::Frequency, crd::f64> operator""_semitones(unsigned long long v) noexcept
{ return quantity_from_nonlinear<Semitones>(static_cast<crd::f64>(v)); }

} // namespace crd::units::literals
