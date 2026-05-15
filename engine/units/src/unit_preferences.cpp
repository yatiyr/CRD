// crd-units Layer-6 — UnitPreferences + format/parse impl (Phase 3.1.7.5 v0d-5).

#include <crd/units/unit_preferences.hpp>

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace crd::units
{

namespace
{

// SI conversion factors: VALUE_IN_DISPLAY = VALUE_SI / FACTOR_SI_PER_DISPLAY.
// E.g. length to display mm: 1.0 m / 0.001 = 1000.0 (display).
// Affine units (temperature) handled separately.

// ── Length: SI = meter ────────────────────────────────────────────────────
constexpr double k_length_factor[] = {
    1.0,         // Meter
    1.0e-3,      // Millimeter
    1.0e-2,      // Centimeter
    1.0e3,       // Kilometer
    1.0e-6,      // Micrometer
    0.0254,      // Inch
    0.3048,      // Foot
    0.9144,      // Yard
    1609.344,    // Mile
    0.0254e-3,   // Mil (1/1000 inch)
};
constexpr const char* k_length_suffix[] = {"_m", "_mm", "_cm", "_km", "_um", "_in", "_ft", "_yd", "_mi", "_mil"};

// ── Mass: SI = kilogram ───────────────────────────────────────────────────
constexpr double k_mass_factor[] = {
    1.0,         // Kilogram
    1.0e-3,      // Gram
    1.0e-6,      // Milligram
    1.0e3,       // MetricTonne
    0.45359237,  // PoundMass
    0.028349523125, // OunceMass
};
constexpr const char* k_mass_suffix[] = {"_kg", "_g", "_mg", "_t", "_lb_mass", "_oz_mass"};

// ── Time: SI = second ─────────────────────────────────────────────────────
constexpr double k_time_factor[] = {
    1.0,         // Second
    1.0e-3,      // Millisecond
    1.0e-6,      // Microsecond
    60.0,        // Minute
    3600.0,      // Hour
    86400.0,     // Day
};
constexpr const char* k_time_suffix[] = {"_s", "_ms", "_us", "_min", "_h", "_day"};

// ── Angle: SI = radian ────────────────────────────────────────────────────
constexpr double k_pi_d = 3.14159265358979323846;
constexpr double k_angle_factor[] = {
    1.0,            // Radian
    k_pi_d / 180.0, // Degree
    2.0 * k_pi_d,   // Turn
};
constexpr const char* k_angle_suffix[] = {"_rad", "_deg", "_turn"};

// ── Velocity: SI = m/s ────────────────────────────────────────────────────
constexpr double k_velocity_factor[] = {
    1.0,            // MeterPerSecond
    1.0 / 3.6,      // KilometerPerHour
    0.44704,        // MilePerHour
    0.514444,       // Knots
};
constexpr const char* k_velocity_suffix[] = {"_mps", "_kmph", "_mph", "_knots"};

// ── Force: SI = newton ────────────────────────────────────────────────────
constexpr double k_force_factor[] = {
    1.0,            // Newton
    1.0e3,          // Kilonewton
    4.4482216152605, // PoundForce
};
constexpr const char* k_force_suffix[] = {"_N", "_kN", "_lbf"};

// ── Pressure: SI = pascal ─────────────────────────────────────────────────
constexpr double k_pressure_factor[] = {
    1.0,            // Pascal
    1.0e3,          // Kilopascal
    1.0e6,          // Megapascal
    1.0e5,          // Bar
    6894.757293168, // Psi
    101325.0,       // Atm
};
constexpr const char* k_pressure_suffix[] = {"_Pa", "_kPa", "_MPa", "_bar", "_psi", "_atm"};

// ── Energy: SI = joule ────────────────────────────────────────────────────
constexpr double k_energy_factor[] = {
    1.0,            // Joule
    1.0e3,          // Kilojoule
    4.184,          // Calorie
    4184.0,         // Kilocalorie
    3.6e6,          // KilowattHour
    1.602176634e-19, // ElectronVolt
};
constexpr const char* k_energy_suffix[] = {"_J", "_kJ", "_cal", "_kcal", "_kWh", "_eV"};

// ── Power: SI = watt — `format_power` not yet shipped; tables reserved. ──
[[maybe_unused]] constexpr double k_power_factor[] = {
    1.0,            // Watt
    1.0e3,          // Kilowatt
    745.69987158227022, // Horsepower (mechanical, US)
};
constexpr const char* k_power_suffix[] = {"_W", "_kW", "_hp"};

// ── Voltage: SI = volt — `format_voltage` not yet shipped; tables reserved. ──
[[maybe_unused]] constexpr double k_voltage_factor[] = {
    1.0,            // Volt
    1.0e-3,         // Millivolt
    1.0e3,          // Kilovolt
};
constexpr const char* k_voltage_suffix[] = {"_V", "_mV", "_kV"};

// ── Current: SI = ampere — `format_current` not yet shipped; tables reserved. ──
[[maybe_unused]] constexpr double k_current_factor[] = {
    1.0,            // Ampere
    1.0e-3,         // Milliampere
    1.0e-6,         // Microampere
};
constexpr const char* k_current_suffix[] = {"_A", "_mA", "_uA"};

// ── Frequency: SI = hertz (= 1/s) ─────────────────────────────────────────
constexpr double k_frequency_factor[] = {
    1.0,            // Hertz
    1.0e3,          // Kilohertz
    1.0e6,          // Megahertz
    1.0e9,          // Gigahertz
    1.0 / 60.0,     // RPM (= 1/60 Hz)
};
constexpr const char* k_frequency_suffix[] = {"_Hz", "_kHz", "_MHz", "_GHz", "_rpm"};

// ── Temperature: affine — Kelvin is the SI base ───────────────────────────
constexpr const char* k_temperature_suffix[] = {"_kelvin", "_celsius", "_fahrenheit", "_rankine"};

// ---------------------------------------------------------------------------
// Format helper: convert SI value to display unit + format with prefs.
// ---------------------------------------------------------------------------

template <typename T>
crd::containers::String format_value_with_factor(T si_value, double factor, const char* suffix,
                                                  const UnitPreferences& prefs,
                                                  crd::memory::IAllocator* alloc)
{
    const double display = static_cast<double>(si_value) / factor;
    char buf[64] = {};
    const int prec = static_cast<int>(prefs.precision_digits);
    if (prefs.scientific_notation)
    {
        (void)std::snprintf(buf, sizeof(buf), "%.*e", prec, display);
    }
    else
    {
        (void)std::snprintf(buf, sizeof(buf), "%.*g", prec, display);
    }
    crd::containers::String out(alloc != nullptr ? alloc : crd::memory::default_allocator());
    out.append(buf);
    if (prefs.include_suffix)
    {
        out.append(suffix);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parse helper: extract a numeric value + match suffix against a table.
// ---------------------------------------------------------------------------

bool parse_value_with_suffix(crd::containers::StringView text,
                             const char* const* suffixes, crd::usize suffix_count,
                             double& out_value, crd::usize& out_unit_idx) noexcept
{
    if (text.empty()) { return false; }
    // Find the first non-digit / non-sign / non-decimal / non-exponent character that
    // begins the suffix. Conservative: scan from the right for matching suffix.
    for (crd::usize i = 0; i < suffix_count; ++i)
    {
        const std::string_view suf{suffixes[i]};
        if (text.size() >= suf.size())
        {
            const auto tail = std::string_view{text.data(), text.size()}.substr(text.size() - suf.size());
            if (tail == suf)
            {
                // Numeric portion is everything before the suffix.
                char buf[64] = {};
                const auto num_len = text.size() - suf.size();
                if (num_len == 0 || num_len >= sizeof(buf)) { return false; }
                for (crd::usize j = 0; j < num_len; ++j) { buf[j] = text[j]; }
                // Trim trailing whitespace before suffix start.
                while (num_len > 0 && (buf[num_len - 1] == ' ' || buf[num_len - 1] == '\t')) {}
                char* end = nullptr;
                const double v = std::strtod(buf, &end); // NOLINT(cert-err34-c)
                if (end == buf) { return false; }
                out_value = v;
                out_unit_idx = i;
                return true;
            }
        }
    }
    // No suffix — interpret as bare SI value.
    char buf[64] = {};
    if (text.size() >= sizeof(buf)) { return false; }
    for (crd::usize j = 0; j < text.size(); ++j) { buf[j] = text[j]; }
    char* end = nullptr;
    const double v = std::strtod(buf, &end); // NOLINT(cert-err34-c)
    if (end == buf) { return false; }
    out_value = v;
    out_unit_idx = static_cast<crd::usize>(-1); // sentinel = SI
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Suffix tables (public)
// ---------------------------------------------------------------------------

crd::containers::StringView suffix_for(LengthUnitChoice u) noexcept
{ return crd::containers::StringView{k_length_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(MassUnitChoice u) noexcept
{ return crd::containers::StringView{k_mass_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(TimeUnitChoice u) noexcept
{ return crd::containers::StringView{k_time_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(AngleUnitChoice u) noexcept
{ return crd::containers::StringView{k_angle_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(VelocityUnitChoice u) noexcept
{ return crd::containers::StringView{k_velocity_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(ForceUnitChoice u) noexcept
{ return crd::containers::StringView{k_force_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(PressureUnitChoice u) noexcept
{ return crd::containers::StringView{k_pressure_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(EnergyUnitChoice u) noexcept
{ return crd::containers::StringView{k_energy_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(PowerUnitChoice u) noexcept
{ return crd::containers::StringView{k_power_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(VoltageUnitChoice u) noexcept
{ return crd::containers::StringView{k_voltage_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(CurrentUnitChoice u) noexcept
{ return crd::containers::StringView{k_current_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(FrequencyUnitChoice u) noexcept
{ return crd::containers::StringView{k_frequency_suffix[static_cast<crd::usize>(u)]}; }
crd::containers::StringView suffix_for(TemperatureUnitChoice u) noexcept
{ return crd::containers::StringView{k_temperature_suffix[static_cast<crd::usize>(u)]}; }

// ---------------------------------------------------------------------------
// 11 Discipline presets
// ---------------------------------------------------------------------------

UnitPreferences make_game_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Meter;
    p.mass = MassUnitChoice::Kilogram;
    p.time = TimeUnitChoice::Second;
    p.angle = AngleUnitChoice::Degree;
    p.velocity = VelocityUnitChoice::MeterPerSecond;
    p.temperature = TemperatureUnitChoice::Celsius;
    return p;
}

UnitPreferences make_cad_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Millimeter;
    p.mass = MassUnitChoice::Kilogram;
    p.angle = AngleUnitChoice::Degree;
    p.force = ForceUnitChoice::Newton;
    p.pressure = PressureUnitChoice::Megapascal;
    return p;
}

UnitPreferences make_robotics_prefs() noexcept
{
    // ROS REP 103: SI everywhere, rad for angles, Quaternion for rotation.
    UnitPreferences p{};
    p.length = LengthUnitChoice::Meter;
    p.mass = MassUnitChoice::Kilogram;
    p.angle = AngleUnitChoice::Radian;
    p.velocity = VelocityUnitChoice::MeterPerSecond;
    return p;
}

UnitPreferences make_aerospace_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Meter;
    p.mass = MassUnitChoice::Kilogram;
    p.angle = AngleUnitChoice::Degree;
    p.velocity = VelocityUnitChoice::Knots;
    p.pressure = PressureUnitChoice::Kilopascal;
    return p;
}

UnitPreferences make_pcb_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Mil;
    p.mass = MassUnitChoice::Gram;
    p.frequency = FrequencyUnitChoice::Megahertz;
    p.voltage = VoltageUnitChoice::Volt;
    p.current = CurrentUnitChoice::Milliampere;
    return p;
}

UnitPreferences make_audio_prefs() noexcept
{
    UnitPreferences p{};
    p.time = TimeUnitChoice::Millisecond;
    p.frequency = FrequencyUnitChoice::Hertz;
    return p;
}

UnitPreferences make_3d_print_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Millimeter;
    p.mass = MassUnitChoice::Gram;
    p.temperature = TemperatureUnitChoice::Celsius;
    p.velocity = VelocityUnitChoice::MeterPerSecond;
    return p;
}

UnitPreferences make_cam_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Inch;
    p.mass = MassUnitChoice::PoundMass;
    p.time = TimeUnitChoice::Minute;
    p.frequency = FrequencyUnitChoice::RPM;
    p.velocity = VelocityUnitChoice::MeterPerSecond; // could be in/min in v2
    return p;
}

UnitPreferences make_cinematic_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Centimeter; // common film/cinematography unit
    p.time = TimeUnitChoice::Second;
    p.angle = AngleUnitChoice::Degree;
    return p;
}

UnitPreferences make_imperial_prefs() noexcept
{
    UnitPreferences p{};
    p.length = LengthUnitChoice::Inch;
    p.mass = MassUnitChoice::PoundMass;
    p.time = TimeUnitChoice::Second;
    p.angle = AngleUnitChoice::Degree;
    p.velocity = VelocityUnitChoice::MilePerHour;
    p.pressure = PressureUnitChoice::Psi;
    p.temperature = TemperatureUnitChoice::Fahrenheit;
    p.force = ForceUnitChoice::PoundForce;
    return p;
}

UnitPreferences make_si_strict_prefs() noexcept
{
    UnitPreferences p{}; // defaults are SI base
    return p;
}

UnitPreferences make_scientific_prefs() noexcept
{
    UnitPreferences p{}; // SI base
    p.precision_digits = 9;
    p.scientific_notation = true;
    return p;
}

// ---------------------------------------------------------------------------
// format_* implementations
// ---------------------------------------------------------------------------

crd::containers::String format_length(Length<crd::f32> q, const UnitPreferences& prefs,
                                       crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.length);
    return format_value_with_factor(q.value, k_length_factor[i], k_length_suffix[i], prefs, alloc);
}

crd::containers::String format_length(Length<crd::f64> q, const UnitPreferences& prefs,
                                       crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.length);
    return format_value_with_factor(q.value, k_length_factor[i], k_length_suffix[i], prefs, alloc);
}

crd::containers::String format_mass(Mass<crd::f32> q, const UnitPreferences& prefs,
                                     crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.mass);
    return format_value_with_factor(q.value, k_mass_factor[i], k_mass_suffix[i], prefs, alloc);
}

crd::containers::String format_time(Time<crd::f64> q, const UnitPreferences& prefs,
                                     crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.time);
    return format_value_with_factor(q.value, k_time_factor[i], k_time_suffix[i], prefs, alloc);
}

crd::containers::String format_angle(Angle<crd::f32> q, const UnitPreferences& prefs,
                                      crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.angle);
    return format_value_with_factor(q.value, k_angle_factor[i], k_angle_suffix[i], prefs, alloc);
}

crd::containers::String format_velocity(Velocity<crd::f32> q, const UnitPreferences& prefs,
                                         crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.velocity);
    return format_value_with_factor(q.value, k_velocity_factor[i], k_velocity_suffix[i], prefs, alloc);
}

crd::containers::String format_force(Force<crd::f32> q, const UnitPreferences& prefs,
                                      crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.force);
    return format_value_with_factor(q.value, k_force_factor[i], k_force_suffix[i], prefs, alloc);
}

crd::containers::String format_pressure(Pressure<crd::f32> q, const UnitPreferences& prefs,
                                         crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.pressure);
    return format_value_with_factor(q.value, k_pressure_factor[i], k_pressure_suffix[i], prefs, alloc);
}

crd::containers::String format_energy(Energy<crd::f32> q, const UnitPreferences& prefs,
                                       crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.energy);
    return format_value_with_factor(q.value, k_energy_factor[i], k_energy_suffix[i], prefs, alloc);
}

crd::containers::String format_frequency(Frequency<crd::f32> q, const UnitPreferences& prefs,
                                          crd::memory::IAllocator* alloc)
{
    const auto i = static_cast<crd::usize>(prefs.frequency);
    return format_value_with_factor(q.value, k_frequency_factor[i], k_frequency_suffix[i], prefs, alloc);
}

crd::containers::String format_temperature(crd::f32 kelvin, const UnitPreferences& prefs,
                                            crd::memory::IAllocator* alloc)
{
    // Affine. Kelvin = SI. Celsius = K - 273.15. Fahrenheit = K * 9/5 - 459.67.
    // Rankine = K * 9/5.
    double display = static_cast<double>(kelvin);
    const char* suffix = "_kelvin";
    switch (prefs.temperature)
    {
        case TemperatureUnitChoice::Kelvin:    display = static_cast<double>(kelvin); suffix = "_kelvin"; break;
        case TemperatureUnitChoice::Celsius:   display = static_cast<double>(kelvin) - 273.15; suffix = "_celsius"; break;
        case TemperatureUnitChoice::Fahrenheit: display = static_cast<double>(kelvin) * 9.0 / 5.0 - 459.67; suffix = "_fahrenheit"; break;
        case TemperatureUnitChoice::Rankine:   display = static_cast<double>(kelvin) * 9.0 / 5.0; suffix = "_rankine"; break;
    }
    char buf[64] = {};
    const int prec = static_cast<int>(prefs.precision_digits);
    if (prefs.scientific_notation) { (void)std::snprintf(buf, sizeof(buf), "%.*e", prec, display); }
    else                            { (void)std::snprintf(buf, sizeof(buf), "%.*g", prec, display); }
    crd::containers::String out(alloc != nullptr ? alloc : crd::memory::default_allocator());
    out.append(buf);
    if (prefs.include_suffix) { out.append(suffix); }
    return out;
}

// ---------------------------------------------------------------------------
// parse_* implementations
// ---------------------------------------------------------------------------

template <typename T>
std::optional<Length<T>> parse_length(crd::containers::StringView text, const UnitPreferences& /*prefs*/) noexcept
{
    double val = 0.0;
    crd::usize unit_idx = 0;
    if (!parse_value_with_suffix(text, k_length_suffix, std::size(k_length_factor), val, unit_idx))
    {
        return std::nullopt;
    }
    if (unit_idx == static_cast<crd::usize>(-1)) { return Length<T>{static_cast<T>(val)}; }
    return Length<T>{static_cast<T>(val * k_length_factor[unit_idx])};
}

template <typename T>
std::optional<Mass<T>> parse_mass(crd::containers::StringView text, const UnitPreferences& /*prefs*/) noexcept
{
    double val = 0.0;
    crd::usize unit_idx = 0;
    if (!parse_value_with_suffix(text, k_mass_suffix, std::size(k_mass_factor), val, unit_idx))
    {
        return std::nullopt;
    }
    if (unit_idx == static_cast<crd::usize>(-1)) { return Mass<T>{static_cast<T>(val)}; }
    return Mass<T>{static_cast<T>(val * k_mass_factor[unit_idx])};
}

template <typename T>
std::optional<Angle<T>> parse_angle(crd::containers::StringView text, const UnitPreferences& /*prefs*/) noexcept
{
    double val = 0.0;
    crd::usize unit_idx = 0;
    if (!parse_value_with_suffix(text, k_angle_suffix, std::size(k_angle_factor), val, unit_idx))
    {
        return std::nullopt;
    }
    if (unit_idx == static_cast<crd::usize>(-1)) { return Angle<T>{static_cast<T>(val)}; }
    return Angle<T>{static_cast<T>(val * k_angle_factor[unit_idx])};
}

template <typename T>
std::optional<Velocity<T>> parse_velocity(crd::containers::StringView text, const UnitPreferences& /*prefs*/) noexcept
{
    double val = 0.0;
    crd::usize unit_idx = 0;
    if (!parse_value_with_suffix(text, k_velocity_suffix, std::size(k_velocity_factor), val, unit_idx))
    {
        return std::nullopt;
    }
    if (unit_idx == static_cast<crd::usize>(-1)) { return Velocity<T>{static_cast<T>(val)}; }
    return Velocity<T>{static_cast<T>(val * k_velocity_factor[unit_idx])};
}

template <typename T>
std::optional<Force<T>> parse_force(crd::containers::StringView text, const UnitPreferences& /*prefs*/) noexcept
{
    double val = 0.0;
    crd::usize unit_idx = 0;
    if (!parse_value_with_suffix(text, k_force_suffix, std::size(k_force_factor), val, unit_idx))
    {
        return std::nullopt;
    }
    if (unit_idx == static_cast<crd::usize>(-1)) { return Force<T>{static_cast<T>(val)}; }
    return Force<T>{static_cast<T>(val * k_force_factor[unit_idx])};
}

template <typename T>
std::optional<Pressure<T>> parse_pressure(crd::containers::StringView text, const UnitPreferences& /*prefs*/) noexcept
{
    double val = 0.0;
    crd::usize unit_idx = 0;
    if (!parse_value_with_suffix(text, k_pressure_suffix, std::size(k_pressure_factor), val, unit_idx))
    {
        return std::nullopt;
    }
    if (unit_idx == static_cast<crd::usize>(-1)) { return Pressure<T>{static_cast<T>(val)}; }
    return Pressure<T>{static_cast<T>(val * k_pressure_factor[unit_idx])};
}

std::optional<crd::f32> parse_temperature_to_kelvin(crd::containers::StringView text,
                                                     const UnitPreferences& /*prefs*/) noexcept
{
    if (text.empty()) { return std::nullopt; }
    // Find suffix manually.
    static constexpr const char* k_suffixes[] = {"_kelvin", "_celsius", "_fahrenheit", "_rankine"};
    crd::usize matched = static_cast<crd::usize>(-1);
    crd::usize num_len = text.size();
    for (crd::usize i = 0; i < 4; ++i)
    {
        const std::string_view suf{k_suffixes[i]};
        if (text.size() >= suf.size() &&
            std::string_view{text.data(), text.size()}.substr(text.size() - suf.size()) == suf)
        {
            matched = i;
            num_len = text.size() - suf.size();
            break;
        }
    }
    char buf[64] = {};
    if (num_len == 0 || num_len >= sizeof(buf)) { return std::nullopt; }
    for (crd::usize j = 0; j < num_len; ++j) { buf[j] = text[j]; }
    char* end = nullptr;
    const double v = std::strtod(buf, &end); // NOLINT(cert-err34-c)
    if (end == buf) { return std::nullopt; }
    double k = v; // Kelvin
    switch (matched)
    {
        case 0: k = v; break;                       // Kelvin
        case 1: k = v + 273.15; break;              // Celsius
        case 2: k = (v + 459.67) * 5.0 / 9.0; break; // Fahrenheit
        case 3: k = v * 5.0 / 9.0; break;           // Rankine
        default: k = v; break;                      // bare = Kelvin
    }
    return static_cast<crd::f32>(k);
}

// Explicit instantiations for f32 / f64.
#define CRD_UNITS_INSTANTIATE_PARSE(NAME, RESULT)                                                                      \
    template std::optional<RESULT<crd::f32>> parse_##NAME<crd::f32>(crd::containers::StringView,                       \
                                                                    const UnitPreferences&) noexcept;                  \
    template std::optional<RESULT<crd::f64>> parse_##NAME<crd::f64>(crd::containers::StringView,                       \
                                                                    const UnitPreferences&) noexcept;

CRD_UNITS_INSTANTIATE_PARSE(length, Length)
CRD_UNITS_INSTANTIATE_PARSE(mass, Mass)
CRD_UNITS_INSTANTIATE_PARSE(angle, Angle)
CRD_UNITS_INSTANTIATE_PARSE(velocity, Velocity)
CRD_UNITS_INSTANTIATE_PARSE(force, Force)
CRD_UNITS_INSTANTIATE_PARSE(pressure, Pressure)

} // namespace crd::units
