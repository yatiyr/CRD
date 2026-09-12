#pragma once

// ---------------------------------------------------------------------------
// crd-imgui — UnitPreferences ImGui inspector (Phase 3.1.7.5 v0d-6).
//
// Per ADR-0078 §4 D30: lets a user pick a discipline preset + override
// individual per-Dim display units at runtime. Persisting to `crd-config`
// is the caller's responsibility (this is a stateless inspector).
//
// Usage:
//
//     #include <crd/imgui/unit_preferences_inspector.hpp>
//     ...
//     if (ImGui::Begin("Unit Preferences"))
//     {
//         if (crd::imgui::draw_unit_preferences_inspector(my_prefs))
//         {
//             // prefs changed — persist to config, rebuild inspector cache.
//         }
//     }
//     ImGui::End();
//
// Pin: this header includes <imgui.h>. Consumers that don't already pull
// it in (i.e. headless smokes) should NOT include this file.

#include <crd/units/unit_preferences.hpp>

#include <imgui.h>

#include <array>

namespace crd::imgui
{

namespace detail_units_ui
{

// Display name for each unit choice — what the user sees in the combo box.
// Mirrors the suffix tables in unit_preferences.cpp but uses friendly names.

inline const char* length_label(crd::units::LengthUnitChoice c) noexcept
{
    switch (c)
    {
        case crd::units::LengthUnitChoice::Meter:      return "m (meter)";
        case crd::units::LengthUnitChoice::Millimeter: return "mm (millimeter)";
        case crd::units::LengthUnitChoice::Centimeter: return "cm (centimeter)";
        case crd::units::LengthUnitChoice::Kilometer:  return "km (kilometer)";
        case crd::units::LengthUnitChoice::Micrometer: return "um (micrometer)";
        case crd::units::LengthUnitChoice::Inch:       return "in (inch)";
        case crd::units::LengthUnitChoice::Foot:       return "ft (foot)";
        case crd::units::LengthUnitChoice::Yard:       return "yd (yard)";
        case crd::units::LengthUnitChoice::Mile:       return "mi (mile)";
        case crd::units::LengthUnitChoice::Mil:        return "mil (1/1000 inch)";
    }
    return "?";
}

inline const char* mass_label(crd::units::MassUnitChoice c) noexcept
{
    switch (c)
    {
        case crd::units::MassUnitChoice::Kilogram:    return "kg (kilogram)";
        case crd::units::MassUnitChoice::Gram:        return "g (gram)";
        case crd::units::MassUnitChoice::Milligram:   return "mg (milligram)";
        case crd::units::MassUnitChoice::MetricTonne: return "t (metric tonne)";
        case crd::units::MassUnitChoice::PoundMass:   return "lb (pound-mass)";
        case crd::units::MassUnitChoice::OunceMass:   return "oz (ounce-mass)";
    }
    return "?";
}

inline const char* angle_label(crd::units::AngleUnitChoice c) noexcept
{
    switch (c)
    {
        case crd::units::AngleUnitChoice::Radian: return "rad (radian)";
        case crd::units::AngleUnitChoice::Degree: return "deg (degree)";
        case crd::units::AngleUnitChoice::Turn:   return "turn";
    }
    return "?";
}

inline const char* time_label(crd::units::TimeUnitChoice c) noexcept
{
    switch (c)
    {
        case crd::units::TimeUnitChoice::Second:      return "s (second)";
        case crd::units::TimeUnitChoice::Millisecond: return "ms (millisecond)";
        case crd::units::TimeUnitChoice::Microsecond: return "us (microsecond)";
        case crd::units::TimeUnitChoice::Minute:      return "min (minute)";
        case crd::units::TimeUnitChoice::Hour:        return "h (hour)";
        case crd::units::TimeUnitChoice::Day:         return "day";
    }
    return "?";
}

inline const char* temperature_label(crd::units::TemperatureUnitChoice c) noexcept
{
    switch (c)
    {
        case crd::units::TemperatureUnitChoice::Kelvin:     return "K (kelvin)";
        case crd::units::TemperatureUnitChoice::Celsius:    return "C (celsius)";
        case crd::units::TemperatureUnitChoice::Fahrenheit: return "F (fahrenheit)";
        case crd::units::TemperatureUnitChoice::Rankine:    return "Ra (rankine)";
    }
    return "?";
}

template <typename Enum>
bool draw_enum_combo(const char* label, Enum& value, const char* (*to_label)(Enum), int count)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, to_label(value)))
    {
        for (int i = 0; i < count; ++i)
        {
            const auto candidate = static_cast<Enum>(i);
            const bool selected  = (candidate == value);
            if (ImGui::Selectable(to_label(candidate), selected))
            {
                if (candidate != value)
                {
                    value   = candidate;
                    changed = true;
                }
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace detail_units_ui

// Discipline preset picker. Returns true on change.
inline bool draw_unit_preset_picker(crd::units::UnitPreferences& prefs)
{
    using namespace crd::units;
    static const struct { const char* name; UnitPreferences (*make)(); } k_presets[] = {
        {"Game",          &make_game_prefs},
        {"CAD",           &make_cad_prefs},
        {"Robotics",      &make_robotics_prefs},
        {"Aerospace",     &make_aerospace_prefs},
        {"PCB / EDA",     &make_pcb_prefs},
        {"Audio",         &make_audio_prefs},
        {"3D Print",      &make_3d_print_prefs},
        {"CAM",           &make_cam_prefs},
        {"Cinematic",     &make_cinematic_prefs},
        {"Imperial",      &make_imperial_prefs},
        {"SI Strict",     &make_si_strict_prefs},
        {"Scientific",    &make_scientific_prefs},
    };
    bool changed = false;
    ImGui::TextUnformatted("Discipline preset:");
    ImGui::SameLine();
    if (ImGui::BeginCombo("##unit_preset", "Apply preset..."))
    {
        for (const auto& entry : k_presets)
        {
            if (ImGui::Selectable(entry.name, false))
            {
                prefs   = entry.make();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Full per-Dim inspector. Returns true if any dim changed.
inline bool draw_unit_preferences_inspector(crd::units::UnitPreferences& prefs)
{
    bool changed = false;
    changed |= draw_unit_preset_picker(prefs);
    ImGui::Separator();
    changed |= detail_units_ui::draw_enum_combo("Length", prefs.length, detail_units_ui::length_label, 10);
    changed |= detail_units_ui::draw_enum_combo("Mass",   prefs.mass,   detail_units_ui::mass_label,   6);
    changed |= detail_units_ui::draw_enum_combo("Time",   prefs.time,   detail_units_ui::time_label,   6);
    changed |= detail_units_ui::draw_enum_combo("Angle",  prefs.angle,  detail_units_ui::angle_label,  3);
    changed |= detail_units_ui::draw_enum_combo("Temperature", prefs.temperature,
                                                 detail_units_ui::temperature_label, 4);
    ImGui::Separator();

    int precision = static_cast<int>(prefs.precision_digits);
    if (ImGui::SliderInt("Precision digits", &precision, 1, 12))
    {
        prefs.precision_digits = static_cast<crd::u8>(precision);
        changed = true;
    }
    bool suffix = prefs.include_suffix;
    if (ImGui::Checkbox("Include unit suffix", &suffix))
    {
        prefs.include_suffix = suffix;
        changed = true;
    }
    bool sci = prefs.scientific_notation;
    if (ImGui::Checkbox("Scientific notation", &sci))
    {
        prefs.scientific_notation = sci;
        changed = true;
    }
    return changed;
}

} // namespace crd::imgui
