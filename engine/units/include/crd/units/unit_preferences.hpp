#pragma once

// ---------------------------------------------------------------------------
// crd-units Layer-6 — UnitPreferences + format/parse (Phase 3.1.7.5 v0d-5).
//
// Per ADR-0078 §1 D3 layer 6: format/parse with a runtime-selectable
// preferred display unit per Dim. Lets the same `Length<f32>{1.0F}` SI
// value render as "1.0 m" (SI), "1000 mm" (CAD), "39.37 in" (imperial),
// or "100 cm" (cinematic) — without re-doing the underlying physics.
//
// API at a glance:
//
//     UnitPreferences prefs = make_cad_prefs();
//     Length32 height = Length32{1.85F};
//     auto s = format_length(height, prefs);  // "1850 mm"
//     auto q = parse_length<crd::f32>("39.37_in", prefs);  // optional<Length32>
//
// Discipline presets (11): game, CAD, robotics, aerospace, PCB, audio,
// 3D-print, CAM, cinematic, imperial, SI-strict, scientific.
//
// Per ADR-0078 §3 D22 SIMD-boundary pin, this layer is for UI / ImGui
// inspector / config display — never on the hot path. Format uses
// std::snprintf for predictable behaviour; parse is the inverse of v0b-2
// crd-config TOML unit-suffix accessors.

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <optional>

namespace crd::units
{

// ---------------------------------------------------------------------------
// Per-Dim unit choice enums (locked surface at v0d-5).
// ---------------------------------------------------------------------------

enum class LengthUnitChoice : crd::u8 { Meter, Millimeter, Centimeter, Kilometer, Micrometer, Inch, Foot, Yard, Mile, Mil };
enum class MassUnitChoice : crd::u8 { Kilogram, Gram, Milligram, MetricTonne, PoundMass, OunceMass };
enum class TimeUnitChoice : crd::u8 { Second, Millisecond, Microsecond, Minute, Hour, Day };
enum class AngleUnitChoice : crd::u8 { Radian, Degree, Turn };
enum class VelocityUnitChoice : crd::u8 { MeterPerSecond, KilometerPerHour, MilePerHour, Knots };
enum class ForceUnitChoice : crd::u8 { Newton, Kilonewton, PoundForce };
enum class PressureUnitChoice : crd::u8 { Pascal, Kilopascal, Megapascal, Bar, Psi, Atm };
enum class EnergyUnitChoice : crd::u8 { Joule, Kilojoule, Calorie, Kilocalorie, KilowattHour, ElectronVolt };
enum class PowerUnitChoice : crd::u8 { Watt, Kilowatt, Horsepower };
enum class VoltageUnitChoice : crd::u8 { Volt, Millivolt, Kilovolt };
enum class CurrentUnitChoice : crd::u8 { Ampere, Milliampere, Microampere };
enum class FrequencyUnitChoice : crd::u8 { Hertz, Kilohertz, Megahertz, Gigahertz, RPM };
enum class TemperatureUnitChoice : crd::u8 { Kelvin, Celsius, Fahrenheit, Rankine };

// ---------------------------------------------------------------------------
// UnitPreferences — per-Dim preferred display unit + numeric format hints.
// ---------------------------------------------------------------------------

struct UnitPreferences
{
    LengthUnitChoice      length      = LengthUnitChoice::Meter;
    MassUnitChoice        mass        = MassUnitChoice::Kilogram;
    TimeUnitChoice        time        = TimeUnitChoice::Second;
    AngleUnitChoice       angle       = AngleUnitChoice::Radian;
    VelocityUnitChoice    velocity    = VelocityUnitChoice::MeterPerSecond;
    ForceUnitChoice       force       = ForceUnitChoice::Newton;
    PressureUnitChoice    pressure    = PressureUnitChoice::Pascal;
    EnergyUnitChoice      energy      = EnergyUnitChoice::Joule;
    PowerUnitChoice       power       = PowerUnitChoice::Watt;
    VoltageUnitChoice     voltage     = VoltageUnitChoice::Volt;
    CurrentUnitChoice     current     = CurrentUnitChoice::Ampere;
    FrequencyUnitChoice   frequency   = FrequencyUnitChoice::Hertz;
    TemperatureUnitChoice temperature = TemperatureUnitChoice::Kelvin;

    // Number-of-significant-digits hint for format. Default 6 fits f32
    // round-trip precision; scientific preset bumps to 9 for f64.
    crd::u8 precision_digits = 6;

    // Append the unit suffix in format output (e.g. "1.5_m" vs "1.5").
    bool include_suffix = true;

    // Use scientific notation for format output ("1.5e3" vs "1500").
    bool scientific_notation = false;
};

// ---------------------------------------------------------------------------
// 11 discipline presets (ADR-0078 §4 D30).
// ---------------------------------------------------------------------------

[[nodiscard]] UnitPreferences make_game_prefs() noexcept;          // m, kg, s, deg
[[nodiscard]] UnitPreferences make_cad_prefs() noexcept;           // mm, kg, s, deg
[[nodiscard]] UnitPreferences make_robotics_prefs() noexcept;      // m, kg, s, rad (ROS REP 103)
[[nodiscard]] UnitPreferences make_aerospace_prefs() noexcept;     // m, kg, s, deg
[[nodiscard]] UnitPreferences make_pcb_prefs() noexcept;           // mil, g, s, deg
[[nodiscard]] UnitPreferences make_audio_prefs() noexcept;         // s, Hz
[[nodiscard]] UnitPreferences make_3d_print_prefs() noexcept;      // mm, g, s, °C
[[nodiscard]] UnitPreferences make_cam_prefs() noexcept;           // in, lb, min, RPM
[[nodiscard]] UnitPreferences make_cinematic_prefs() noexcept;     // cm, s, deg
[[nodiscard]] UnitPreferences make_imperial_prefs() noexcept;      // in, lb, s, °F
[[nodiscard]] UnitPreferences make_si_strict_prefs() noexcept;     // m, kg, s, rad (no derived prefixes)
[[nodiscard]] UnitPreferences make_scientific_prefs() noexcept;    // SI + scientific notation + 9 digits

// ---------------------------------------------------------------------------
// format / parse free functions (one per supported Dim).
// ---------------------------------------------------------------------------
//
// `format_*`: Quantity<D, T> -> "1.85_m" / "1850_mm" / etc.
// `parse_*`:  "1.85_m" / "1850_mm" / "1.85 m" -> optional<Quantity<D, T>>
//             Returns nullopt on malformed / unknown suffix. Whitespace
//             between value and suffix is tolerated; underscore-prefix
//             (matches the UDL convention) is the canonical form.

[[nodiscard]] crd::containers::String
format_length(Length<crd::f32> q, const UnitPreferences& prefs,
              crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_length(Length<crd::f64> q, const UnitPreferences& prefs,
              crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_mass(Mass<crd::f32> q, const UnitPreferences& prefs,
            crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_time(Time<crd::f64> q, const UnitPreferences& prefs,
            crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_angle(Angle<crd::f32> q, const UnitPreferences& prefs,
             crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_velocity(Velocity<crd::f32> q, const UnitPreferences& prefs,
                crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_force(Force<crd::f32> q, const UnitPreferences& prefs,
             crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_pressure(Pressure<crd::f32> q, const UnitPreferences& prefs,
                crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_energy(Energy<crd::f32> q, const UnitPreferences& prefs,
              crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_frequency(Frequency<crd::f32> q, const UnitPreferences& prefs,
                 crd::memory::IAllocator* alloc = nullptr);

[[nodiscard]] crd::containers::String
format_temperature(crd::f32 kelvin, const UnitPreferences& prefs,
                   crd::memory::IAllocator* alloc = nullptr);

// parse — returns nullopt on malformed input or unknown unit suffix.

template <typename T>
[[nodiscard]] std::optional<Length<T>>
parse_length(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

template <typename T>
[[nodiscard]] std::optional<Mass<T>>
parse_mass(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

template <typename T>
[[nodiscard]] std::optional<Angle<T>>
parse_angle(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

template <typename T>
[[nodiscard]] std::optional<Velocity<T>>
parse_velocity(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

template <typename T>
[[nodiscard]] std::optional<Force<T>>
parse_force(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

template <typename T>
[[nodiscard]] std::optional<Pressure<T>>
parse_pressure(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

// Temperature parse returns Kelvin (AbsoluteQuantity model).
[[nodiscard]] std::optional<crd::f32>
parse_temperature_to_kelvin(crd::containers::StringView text, const UnitPreferences& prefs) noexcept;

// Suffix string for a unit choice (for inspector UI labels).
[[nodiscard]] crd::containers::StringView suffix_for(LengthUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(MassUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(TimeUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(AngleUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(VelocityUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(ForceUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(PressureUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(EnergyUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(PowerUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(VoltageUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(CurrentUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(FrequencyUnitChoice u) noexcept;
[[nodiscard]] crd::containers::StringView suffix_for(TemperatureUnitChoice u) noexcept;

} // namespace crd::units
