#pragma once

// ---------------------------------------------------------------------------
// crd-config -- unit-tagged TOML accessors (Phase 3.1.7.5 v0b-2).
//
// Reads dimension-tagged values from a Config (TOML source). The key
// itself carries the unit via a trailing suffix; the parser strips the
// suffix and converts to SI base units.
//
// Example TOML:
//     [camera]
//     height_mm   = 1700.0
//     focal_mm    = 35.0
//     fov_deg     = 60.0
//     [body]
//     mass_kg     = 5.0
//     mass_lb     = 11.0      # alternate -- author commits to one suffix
//
// Example C++:
//     const auto h = crd::config::get_length<f32>(cfg, "camera.height_mm",
//                                                  crd::units::Length32{1.7F});
//     const auto m = crd::config::get_mass<f32>  (cfg, "body.mass_kg",
//                                                  crd::units::Mass32{5.0F});
//     const auto a = crd::config::get_angle<f32> (cfg, "camera.fov_deg",
//                                                  crd::units::Angle32{1.047F});
//
// The author commits to a unit AT WRITE TIME (the suffix is part of the
// key). The C++ caller uses the same key. No "magic suffix search" -- if
// the file says `height_mm` and code asks `height_cm`, it's a missing key
// and the fallback is returned.
//
// Supported suffixes (case-sensitive, snake_case):
//   length:      _m _mm _cm _km _um _in _ft _yd _mi
//   mass:        _kg _g _mg _t _lb _oz_mass
//   time:        _s _ms _us _ns _min _hr
//   angle:       _rad _deg _turn
//   velocity:    _mps _kmph _mph _knots
//   force:       _N _kN _lbf
//   pressure:    _Pa _kPa _MPa _bar _psi _atm
//   energy:      _J _kJ _MJ _kWh _cal _kcal
//   power:       _W _kW _MW _hp
//   voltage:     _V _kV _mV
//   current:     _A _mA
//   frequency:   _Hz _kHz _MHz _GHz
//   temperature: _kelvin _celsius _fahrenheit       (affine; see note)
//
// **Temperature is affine.** `_celsius = 25.0` reads as `Temperature{298.15}` K
// (the absolute Kelvin equivalent), NOT `TemperatureDelta{25.0}`. Cerid's
// existing `AbsoluteQuantity<dim::Temperature>` handles the K0 offset.
//
// Missing keys return `fallback`. Wrong-type values (string in a numeric
// field, etc.) return `fallback` per Config's safe-fallback contract.
// ---------------------------------------------------------------------------

#include <crd/config/config.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/units/quantity_aliases.hpp>
#include <crd/units/units_affine.hpp>

namespace crd::config
{

// ---- Length ------------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Length<T>
get_length(const Config& cfg, crd::containers::StringView key,
           crd::units::Length<T> fallback) noexcept;

// ---- Mass --------------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Mass<T>
get_mass(const Config& cfg, crd::containers::StringView key,
         crd::units::Mass<T> fallback) noexcept;

// ---- Time --------------------------------------------------------------

template <typename T = crd::f64>
[[nodiscard]] crd::units::Time<T>
get_time(const Config& cfg, crd::containers::StringView key,
         crd::units::Time<T> fallback) noexcept;

// ---- Angle -------------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Angle<T>
get_angle(const Config& cfg, crd::containers::StringView key,
          crd::units::Angle<T> fallback) noexcept;

// ---- Velocity ----------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Velocity<T>
get_velocity(const Config& cfg, crd::containers::StringView key,
             crd::units::Velocity<T> fallback) noexcept;

// ---- Force -------------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Force<T>
get_force(const Config& cfg, crd::containers::StringView key,
          crd::units::Force<T> fallback) noexcept;

// ---- Pressure ----------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Pressure<T>
get_pressure(const Config& cfg, crd::containers::StringView key,
             crd::units::Pressure<T> fallback) noexcept;

// ---- Energy ------------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Energy<T>
get_energy(const Config& cfg, crd::containers::StringView key,
           crd::units::Energy<T> fallback) noexcept;

// ---- Power -------------------------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Power<T>
get_power(const Config& cfg, crd::containers::StringView key,
          crd::units::Power<T> fallback) noexcept;

// ---- Voltage / Current / Frequency -------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Voltage<T>
get_voltage(const Config& cfg, crd::containers::StringView key,
            crd::units::Voltage<T> fallback) noexcept;

template <typename T = crd::f32>
[[nodiscard]] crd::units::Current<T>
get_current(const Config& cfg, crd::containers::StringView key,
            crd::units::Current<T> fallback) noexcept;

template <typename T = crd::f32>
[[nodiscard]] crd::units::Frequency<T>
get_frequency(const Config& cfg, crd::containers::StringView key,
              crd::units::Frequency<T> fallback) noexcept;

// ---- Temperature (affine) ----------------------------------------------

template <typename T = crd::f32>
[[nodiscard]] crd::units::Temperature<T>
get_temperature(const Config& cfg, crd::containers::StringView key,
                crd::units::Temperature<T> fallback) noexcept;

} // namespace crd::config
