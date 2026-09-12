// ---------------------------------------------------------------------------
// crd-config -- unit-tagged accessor implementations (Phase 3.1.7.5 v0b-2).
//
// Strategy: each per-dimension `get_<dim>` reads the raw f64 from the
// Config using the supplied (unit-suffixed) key, then converts to SI via
// a static suffix table. Missing key / wrong type / unknown suffix all
// return the caller's fallback per Config's safe-fallback contract.
//
// The suffix table is a small constexpr array per dimension; lookup is
// O(N) linear scan -- N <= 10 per dim, negligible cost. The "key carries
// the unit" approach is unambiguous: file says `_mm`, code asks `_mm`.
// No "suffix-search" magic.
// ---------------------------------------------------------------------------

#include <crd/config/unit_accessor.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <cstring>

namespace crd::config
{

namespace
{

// Test whether the key ends with the given suffix (including the leading
// underscore separator). Case-sensitive.
[[nodiscard]] bool key_ends_with(crd::containers::StringView key, const char* suffix) noexcept
{
    const auto suf_len = std::strlen(suffix);
    if (key.size() < suf_len)
    {
        return false;
    }
    return std::strncmp(key.data() + (key.size() - suf_len), suffix, suf_len) == 0;
}

// Suffix-conversion entry: ASCII suffix + f64 multiplier to SI base.
struct SuffixEntry
{
    const char* suffix;
    crd::f64    si_factor;
};

// Affine entry (temperature only): offset_from_kelvin and scale_to_kelvin.
//   K = source * scale + offset
struct AffineEntry
{
    const char* suffix;
    crd::f64    scale;
    crd::f64    offset;
};

// ---- Conversion tables -------------------------------------------------

constexpr SuffixEntry kLengthSuffixes[] = {
    {"_m",  1.0},          {"_mm", 1.0e-3},   {"_cm", 1.0e-2},    {"_km", 1000.0},
    {"_um", 1.0e-6},       {"_in", 0.0254},   {"_ft", 0.3048},    {"_yd", 0.9144},
    {"_mi", 1609.344},
};

constexpr SuffixEntry kMassSuffixes[] = {
    {"_kg", 1.0},      {"_g",  1.0e-3},   {"_mg", 1.0e-6},   {"_t",  1000.0},
    {"_lb_mass", 0.45359237},             {"_oz_mass", 0.0283495231},
};

constexpr SuffixEntry kTimeSuffixes[] = {
    {"_s",  1.0},      {"_ms", 1.0e-3},   {"_us", 1.0e-6},   {"_ns", 1.0e-9},
    {"_min", 60.0},    {"_hr", 3600.0},
};

constexpr SuffixEntry kAngleSuffixes[] = {
    {"_rad",  1.0},                       {"_deg",  0.017453292519943295},
    {"_turn", 6.283185307179586},
};

constexpr SuffixEntry kVelocitySuffixes[] = {
    {"_mps",   1.0},                      {"_kmph",  0.2777777777777778},
    {"_mph",   0.44704},                  {"_knots", 0.5144444444444445},
};

constexpr SuffixEntry kForceSuffixes[] = {
    {"_N", 1.0},   {"_kN", 1000.0},   {"_lbf", 4.4482216152605},
};

constexpr SuffixEntry kPressureSuffixes[] = {
    {"_Pa", 1.0},      {"_kPa", 1000.0},     {"_MPa", 1.0e6},
    {"_bar", 1.0e5},   {"_psi", 6894.757293168},
    {"_atm", 101325.0},
};

constexpr SuffixEntry kEnergySuffixes[] = {
    {"_J",   1.0},      {"_kJ",  1000.0},  {"_MJ",  1.0e6},
    {"_kWh", 3.6e6},    {"_cal", 4.184},   {"_kcal", 4184.0},
};

constexpr SuffixEntry kPowerSuffixes[] = {
    {"_W",  1.0},   {"_kW", 1000.0},   {"_MW", 1.0e6},   {"_hp", 745.6998715822702},
};

constexpr SuffixEntry kVoltageSuffixes[] = {
    {"_V", 1.0},   {"_kV", 1000.0},   {"_mV", 1.0e-3},
};

constexpr SuffixEntry kCurrentSuffixes[] = {
    {"_A", 1.0},   {"_mA", 1.0e-3},
};

constexpr SuffixEntry kFrequencySuffixes[] = {
    {"_Hz", 1.0},   {"_kHz", 1000.0},   {"_MHz", 1.0e6},   {"_GHz", 1.0e9},
};

constexpr AffineEntry kTemperatureSuffixes[] = {
    {"_kelvin",     1.0,         0.0},
    {"_celsius",    1.0,       273.15},
    {"_fahrenheit", 5.0 / 9.0, (273.15 - 32.0 * 5.0 / 9.0)},
};

// Generic linear-suffix resolver. Returns true and sets `out_si_value` if
// the key's suffix matches one in `table`. Returns false otherwise.
template <crd::usize N>
[[nodiscard]] bool resolve_linear(const Config& cfg, crd::containers::StringView key,
                                  const SuffixEntry (&table)[N], crd::f64& out_si_value) noexcept
{
    if (!cfg.contains(key))
    {
        return false;
    }
    for (const auto& entry : table)
    {
        if (key_ends_with(key, entry.suffix))
        {
            const auto raw = cfg.get<crd::f64>(key, 0.0);
            out_si_value = raw * entry.si_factor;
            return true;
        }
    }
    return false;
}

// Affine resolver for temperature.
template <crd::usize N>
[[nodiscard]] bool resolve_affine(const Config& cfg, crd::containers::StringView key,
                                  const AffineEntry (&table)[N], crd::f64& out_si_kelvin) noexcept
{
    if (!cfg.contains(key))
    {
        return false;
    }
    for (const auto& entry : table)
    {
        if (key_ends_with(key, entry.suffix))
        {
            const auto raw = cfg.get<crd::f64>(key, 0.0);
            out_si_kelvin  = raw * entry.scale + entry.offset;
            return true;
        }
    }
    return false;
}

} // namespace

// ---- Per-dimension template implementations + explicit instantiations --

template <typename T>
crd::units::Length<T> get_length(const Config& cfg, crd::containers::StringView key,
                                 crd::units::Length<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kLengthSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Length<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Mass<T> get_mass(const Config& cfg, crd::containers::StringView key,
                             crd::units::Mass<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kMassSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Mass<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Time<T> get_time(const Config& cfg, crd::containers::StringView key,
                             crd::units::Time<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kTimeSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Time<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Angle<T> get_angle(const Config& cfg, crd::containers::StringView key,
                               crd::units::Angle<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kAngleSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Angle<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Velocity<T> get_velocity(const Config& cfg, crd::containers::StringView key,
                                     crd::units::Velocity<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kVelocitySuffixes, si))
    {
        return fallback;
    }
    return crd::units::Velocity<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Force<T> get_force(const Config& cfg, crd::containers::StringView key,
                               crd::units::Force<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kForceSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Force<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Pressure<T> get_pressure(const Config& cfg, crd::containers::StringView key,
                                     crd::units::Pressure<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kPressureSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Pressure<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Energy<T> get_energy(const Config& cfg, crd::containers::StringView key,
                                 crd::units::Energy<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kEnergySuffixes, si))
    {
        return fallback;
    }
    return crd::units::Energy<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Power<T> get_power(const Config& cfg, crd::containers::StringView key,
                               crd::units::Power<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kPowerSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Power<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Voltage<T> get_voltage(const Config& cfg, crd::containers::StringView key,
                                   crd::units::Voltage<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kVoltageSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Voltage<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Current<T> get_current(const Config& cfg, crd::containers::StringView key,
                                   crd::units::Current<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kCurrentSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Current<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Frequency<T> get_frequency(const Config& cfg, crd::containers::StringView key,
                                       crd::units::Frequency<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_linear(cfg, key, kFrequencySuffixes, si))
    {
        return fallback;
    }
    return crd::units::Frequency<T>{static_cast<T>(si)};
}

template <typename T>
crd::units::Temperature<T> get_temperature(const Config& cfg,
                                           crd::containers::StringView key,
                                           crd::units::Temperature<T> fallback) noexcept
{
    crd::f64 si{};
    if (!resolve_affine(cfg, key, kTemperatureSuffixes, si))
    {
        return fallback;
    }
    return crd::units::Temperature<T>{static_cast<T>(si)};
}

// ---- Explicit instantiations: f32 + f64 per dimension ------------------

// Macro needed because explicit template instantiation has no template-
// function equivalent; arguments cannot be parenthesised because they are
// used as template-id heads (`Quantity<f32>`, `Func<f32>`).
// NOLINTBEGIN(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#define CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(Quantity, Func)                              \
    template Quantity<crd::f32> Func<crd::f32>(const Config&, crd::containers::StringView, \
                                                Quantity<crd::f32>) noexcept;             \
    template Quantity<crd::f64> Func<crd::f64>(const Config&, crd::containers::StringView, \
                                                Quantity<crd::f64>) noexcept
// NOLINTEND(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)

CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Length, get_length);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Mass, get_mass);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Time, get_time);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Angle, get_angle);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Velocity, get_velocity);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Force, get_force);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Pressure, get_pressure);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Energy, get_energy);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Power, get_power);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Voltage, get_voltage);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Current, get_current);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Frequency, get_frequency);
CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR(crd::units::Temperature, get_temperature);

#undef CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR

} // namespace crd::config
