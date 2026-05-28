// crd-units v0d-5 — Layer-6 UnitPreferences format/parse tests.
//
// Per ADR-0078 §4 D30: 11 discipline-preset factories + format/parse for
// the 13 common Dims.

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>
#include <crd/units/unit_preferences.hpp>
#include <crd/units/units.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>

using crd::units::Force32;
using crd::units::Length32;
using crd::units::Length64;
using crd::units::make_3d_print_prefs;
using crd::units::make_aerospace_prefs;
using crd::units::make_audio_prefs;
using crd::units::make_cad_prefs;
using crd::units::make_cam_prefs;
using crd::units::make_cinematic_prefs;
using crd::units::make_game_prefs;
using crd::units::make_imperial_prefs;
using crd::units::make_pcb_prefs;
using crd::units::make_robotics_prefs;
using crd::units::make_scientific_prefs;
using crd::units::make_si_strict_prefs;
using crd::units::Mass32;
using crd::units::Pressure32;
using crd::units::UnitPreferences;

TEST_CASE("v0d-5 11 discipline presets exist + return distinct configurations",
          "[units][v0d-5][presets]")
{
    const auto game        = make_game_prefs();
    const auto cad         = make_cad_prefs();
    const auto robotics    = make_robotics_prefs();
    const auto aerospace   = make_aerospace_prefs();
    const auto pcb         = make_pcb_prefs();
    const auto audio       = make_audio_prefs();
    const auto print3d     = make_3d_print_prefs();
    const auto cam         = make_cam_prefs();
    const auto cinematic   = make_cinematic_prefs();
    const auto imperial    = make_imperial_prefs();
    const auto si_strict   = make_si_strict_prefs();
    const auto scientific  = make_scientific_prefs();

    // Each preset pins at least one dimension to a discipline-typical unit.
    REQUIRE(cad.length      == crd::units::LengthUnitChoice::Millimeter);
    REQUIRE(pcb.length      == crd::units::LengthUnitChoice::Mil);
    REQUIRE(imperial.length == crd::units::LengthUnitChoice::Inch);
    REQUIRE(robotics.angle  == crd::units::AngleUnitChoice::Radian);
    REQUIRE(game.angle      == crd::units::AngleUnitChoice::Degree);
    REQUIRE(aerospace.velocity == crd::units::VelocityUnitChoice::Knots);
    REQUIRE(imperial.temperature == crd::units::TemperatureUnitChoice::Fahrenheit);
    REQUIRE(cam.frequency   == crd::units::FrequencyUnitChoice::RPM);
    REQUIRE(cinematic.length == crd::units::LengthUnitChoice::Centimeter);
    REQUIRE(print3d.length  == crd::units::LengthUnitChoice::Millimeter);
    REQUIRE(audio.frequency == crd::units::FrequencyUnitChoice::Hertz);
    REQUIRE(si_strict.length == crd::units::LengthUnitChoice::Meter);
    REQUIRE(scientific.scientific_notation);
    REQUIRE(scientific.precision_digits == 9);
}

TEST_CASE("v0d-5 format_length renders the SI value in the preferred unit",
          "[units][v0d-5][format][length]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const Length32 height{1.85F};

    const auto cad = make_cad_prefs();
    const auto s = crd::units::format_length(height, cad, &alloc);
    // 1.85 m = 1850 mm
    REQUIRE(std::string_view{s.c_str()} == "1850_mm");

    const auto si = make_si_strict_prefs();
    const auto s2 = crd::units::format_length(height, si, &alloc);
    REQUIRE(std::string_view{s2.c_str()} == "1.85_m");

    const auto imp = make_imperial_prefs();
    const auto s3 = crd::units::format_length(height, imp, &alloc);
    // 1.85 / 0.0254 = 72.8346...
    REQUIRE(std::string_view{s3.c_str()}.starts_with("72.8"));
}

TEST_CASE("v0d-5 format honours include_suffix toggle", "[units][v0d-5][format]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    Length32 h{1.0F};
    UnitPreferences prefs = make_si_strict_prefs();
    prefs.include_suffix = false;
    const auto s = crd::units::format_length(h, prefs, &alloc);
    REQUIRE(std::string_view{s.c_str()} == "1");
    prefs.include_suffix = true;
    const auto s2 = crd::units::format_length(h, prefs, &alloc);
    REQUIRE(std::string_view{s2.c_str()} == "1_m");
}

TEST_CASE("v0d-5 format_temperature affine conversion (K -> Celsius / Fahrenheit)",
          "[units][v0d-5][format][temperature]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    // Use 100 K offset above freezing so conversion has measurable magnitude
    // (avoids f32(273.15) - 273.15 ULP residue printing as scientific notation).
    const crd::f32 hundred_above_freezing_k = 373.15F;
    auto cel_prefs = make_si_strict_prefs();
    cel_prefs.temperature = crd::units::TemperatureUnitChoice::Celsius;
    const auto s_cel = crd::units::format_temperature(hundred_above_freezing_k, cel_prefs, &alloc);
    REQUIRE(std::string_view{s_cel.c_str()}.starts_with("100"));

    auto fah_prefs = make_si_strict_prefs();
    fah_prefs.temperature = crd::units::TemperatureUnitChoice::Fahrenheit;
    const auto s_fah = crd::units::format_temperature(hundred_above_freezing_k, fah_prefs, &alloc);
    // 373.15 K = 100 C = 212 F
    REQUIRE(std::string_view{s_fah.c_str()}.starts_with("212"));
}

TEST_CASE("v0d-5 parse_length: 1.85_m -> Length32{1.85}", "[units][v0d-5][parse][length]")
{
    const auto prefs = make_si_strict_prefs();
    const auto q = crd::units::parse_length<crd::f32>(
        crd::containers::StringView{"1.85_m"}, prefs);
    REQUIRE(q.has_value());
    REQUIRE(q->value == Catch::Approx(1.85F));
}

TEST_CASE("v0d-5 parse_length: 1850_mm -> Length32{1.85}", "[units][v0d-5][parse][length]")
{
    const auto prefs = make_cad_prefs();
    const auto q = crd::units::parse_length<crd::f32>(
        crd::containers::StringView{"1850_mm"}, prefs);
    REQUIRE(q.has_value());
    REQUIRE(q->value == Catch::Approx(1.85F));
}

TEST_CASE("v0d-5 parse_length: 1.0_in -> Length32{0.0254}", "[units][v0d-5][parse][length]")
{
    const auto prefs = make_imperial_prefs();
    const auto q = crd::units::parse_length<crd::f32>(
        crd::containers::StringView{"1.0_in"}, prefs);
    REQUIRE(q.has_value());
    REQUIRE(q->value == Catch::Approx(0.0254F));
}

TEST_CASE("v0d-5 parse_length: bare number = SI", "[units][v0d-5][parse][length]")
{
    const auto prefs = make_cad_prefs(); // prefs ignored when no suffix
    const auto q = crd::units::parse_length<crd::f32>(
        crd::containers::StringView{"2.5"}, prefs);
    REQUIRE(q.has_value());
    REQUIRE(q->value == Catch::Approx(2.5F));
}

TEST_CASE("v0d-5 parse_length: malformed input returns nullopt",
          "[units][v0d-5][parse][length]")
{
    const auto prefs = make_si_strict_prefs();
    REQUIRE_FALSE(crd::units::parse_length<crd::f32>(crd::containers::StringView{""}, prefs).has_value());
    REQUIRE_FALSE(crd::units::parse_length<crd::f32>(crd::containers::StringView{"abc"}, prefs).has_value());
}

TEST_CASE("v0d-5 parse_temperature_to_kelvin: 25_celsius -> 298.15 K",
          "[units][v0d-5][parse][temperature]")
{
    const auto prefs = make_si_strict_prefs();
    const auto k = crd::units::parse_temperature_to_kelvin(
        crd::containers::StringView{"25_celsius"}, prefs);
    REQUIRE(k.has_value());
    REQUIRE(*k == Catch::Approx(298.15F));
}

TEST_CASE("v0d-5 parse_temperature_to_kelvin: 32_fahrenheit -> 273.15 K",
          "[units][v0d-5][parse][temperature]")
{
    const auto prefs = make_si_strict_prefs();
    const auto k = crd::units::parse_temperature_to_kelvin(
        crd::containers::StringView{"32_fahrenheit"}, prefs);
    REQUIRE(k.has_value());
    REQUIRE(*k == Catch::Approx(273.15F).margin(0.01F));
}

TEST_CASE("v0d-5 format/parse round-trip: Length32 1.85 m via CAD prefs",
          "[units][v0d-5][roundtrip]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const auto prefs = make_cad_prefs();
    const Length32 original{1.85F};
    const auto text = crd::units::format_length(original, prefs, &alloc);
    const auto parsed = crd::units::parse_length<crd::f32>(
        crd::containers::StringView{text.data(), text.size()}, prefs);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->value == Catch::Approx(original.value).margin(1.0e-5F));
}

TEST_CASE("v0d-5 format_mass + format_pressure work across units",
          "[units][v0d-5][format][misc]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const auto cad = make_cad_prefs();
    const Mass32 m{1.0F};
    const auto s = crd::units::format_mass(m, cad, &alloc);
    REQUIRE(std::string_view{s.c_str()} == "1_kg");

    const Pressure32 p{1.0e5F}; // ~ 1 atm
    auto bar = make_si_strict_prefs();
    bar.pressure = crd::units::PressureUnitChoice::Bar;
    const auto s_bar = crd::units::format_pressure(p, bar, &alloc);
    REQUIRE(std::string_view{s_bar.c_str()}.starts_with("1"));
}

TEST_CASE("v0d-5 suffix_for returns expected strings",
          "[units][v0d-5][suffix]")
{
    REQUIRE(std::string_view{crd::units::suffix_for(crd::units::LengthUnitChoice::Millimeter).data()}.starts_with("_mm"));
    REQUIRE(std::string_view{crd::units::suffix_for(crd::units::AngleUnitChoice::Degree).data()}.starts_with("_deg"));
    REQUIRE(std::string_view{crd::units::suffix_for(crd::units::FrequencyUnitChoice::RPM).data()}.starts_with("_rpm"));
}
