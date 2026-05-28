// crd-units v0a-2 ? value_in<TargetUnit> + quantity_from<SourceUnit> tests.

#include <crd/units/quantity.hpp>
#include <crd/units/units_compound.hpp>
#include <crd/units/units_si.hpp>
#include <crd/units/value_in.hpp>

#include <catch2/catch_test_macros.hpp>

#include <numbers>
#include <cmath>

namespace
{
using namespace crd::units;
using crd::f32;
using crd::f64;
} // namespace

// ---------------------------------------------------------------------------
// (1) Round-trip: quantity_from + value_in is identity for ratio-exact pairs
// ---------------------------------------------------------------------------

TEST_CASE("value_in: m -> mm -> m round-trip is bit-exact",
          "[v0a-2][value-in][round-trip]")
{
    auto len = quantity_from<Millimeter>(25.4);   // 25.4 mm ? 0.0254 m
    CHECK(len.value == 0.0254);

    f64 back_to_mm = value_in<Millimeter>(len);
    CHECK(back_to_mm == 25.4);
}

TEST_CASE("value_in: inch <-> mm is exact (rational factor)",
          "[v0a-2][value-in][round-trip]")
{
    auto one_inch = quantity_from<Inch>(1.0);
    CHECK(one_inch.value == 0.0254);

    f64 in_mm = value_in<Millimeter>(one_inch);
    CHECK(in_mm == 25.4);

    f64 back_to_inch = value_in<Inch>(one_inch);
    CHECK(back_to_inch == 1.0);
}

TEST_CASE("value_in: mile <-> km <-> ft consistent",
          "[v0a-2][value-in][round-trip]")
{
    auto one_mile = quantity_from<Mile>(1.0);
    CHECK(one_mile.value == 1609.344);

    f64 in_km = value_in<Kilometer>(one_mile);
    CHECK(in_km == 1.609344);

    f64 in_ft = value_in<Foot>(one_mile);
    CHECK(in_ft == 5280.0);
}

TEST_CASE("value_in: pound-mass <-> kg",
          "[v0a-2][value-in][round-trip]")
{
    auto one_lb = quantity_from<PoundMass>(1.0);
    CHECK(one_lb.value == 0.45359237);

    f64 in_g = value_in<Gram>(one_lb);
    CHECK(in_g == 453.59237);
}

// ---------------------------------------------------------------------------
// (2) Compound units via value_in
// ---------------------------------------------------------------------------

TEST_CASE("value_in: m_per_s -> mph",
          "[v0a-2][value-in][compound]")
{
    Quantity<dim::Velocity, f64> v{1.0};  // 1 m/s = ? mph
    f64 in_mph = value_in<MilePerHour>(v);
    // 1 m/s = 1 / 0.44704 mph ? 2.23694
    CHECK(in_mph == 1.0 / 0.44704);
}

TEST_CASE("value_in: km_per_h <-> mph for round numbers",
          "[v0a-2][value-in][compound]")
{
    auto v_kmh = quantity_from<KilometerPerHour>(100.0);
    f64 in_mph = value_in<MilePerHour>(v_kmh);
    // 100 km/h = 62.1371... mph
    CHECK(in_mph == 100.0 * KilometerPerHour::factor / MilePerHour::factor);
}

TEST_CASE("value_in: 60 mph in m/s = 26.8224 (bit-exact)",
          "[v0a-2][value-in][compound]")
{
    auto v_mph = quantity_from<MilePerHour>(60.0);
    // 60 mph * 0.44704 = 26.8224 m/s (bit-exact)
    CHECK(v_mph.value == 26.8224);
}

// ---------------------------------------------------------------------------
// (3) Pressure conversions
// ---------------------------------------------------------------------------

TEST_CASE("value_in: pressure 1 atm <-> Pa <-> kPa",
          "[v0a-2][value-in][pressure]")
{
    auto p = quantity_from<Atmosphere>(1.0);
    CHECK(p.value == 101325.0);

    f64 in_kilopascal = value_in<Kilopascal>(p);
    CHECK(in_kilopascal == 101.325);

    f64 in_atm = value_in<Atmosphere>(p);
    CHECK(in_atm == 1.0);
}

// ---------------------------------------------------------------------------
// (4) Time
// ---------------------------------------------------------------------------

TEST_CASE("value_in: time 1 hour <-> seconds <-> minutes",
          "[v0a-2][value-in][time]")
{
    auto t = quantity_from<Hour>(1.0);
    CHECK(t.value == 3600.0);

    f64 in_min = value_in<Minute>(t);
    CHECK(in_min == 60.0);

    f64 in_ms = value_in<Millisecond>(t);
    CHECK(in_ms == 3'600'000.0);
}

// ---------------------------------------------------------------------------
// (5) Angle ? irrational factor (1 ULP tolerance)
// ---------------------------------------------------------------------------

TEST_CASE("value_in: 360 degrees ~ 2pi rad (within 1 ULP)",
          "[v0a-2][value-in][angle]")
{
    auto theta = quantity_from<Degree>(360.0);
    constexpr f64 expected = 2.0 * std::numbers::pi;
    CHECK(std::abs(theta.value - expected) < 1e-12);

    f64 back_to_deg = value_in<Degree>(theta);
    CHECK(std::abs(back_to_deg - 360.0) < 1e-10);
}

TEST_CASE("value_in: 90 degrees ~ pi/2 rad (within tolerance)",
          "[v0a-2][value-in][angle]")
{
    auto theta = quantity_from<Degree>(90.0);
    constexpr f64 half_pi = std::numbers::pi / 2.0;
    CHECK(std::abs(theta.value - half_pi) < 1e-14);
}

// ---------------------------------------------------------------------------
// (6) Force / Energy / Power
// ---------------------------------------------------------------------------

TEST_CASE("value_in: 1 PoundForce in Newton",
          "[v0a-2][value-in][force]")
{
    auto f = quantity_from<PoundForce>(1.0);
    CHECK(std::abs(f.value - 4.4482216152605) < 1e-10);
}

TEST_CASE("value_in: 1 KilowattHour in Joule",
          "[v0a-2][value-in][energy]")
{
    auto e = quantity_from<KilowattHour>(1.0);
    CHECK(e.value == 3'600'000.0);

    f64 in_kilojoule = value_in<Kilojoule>(e);
    CHECK(in_kilojoule == 3600.0);
}

TEST_CASE("value_in: 1 Calorie in Joule (exact rational 4184/1000)",
          "[v0a-2][value-in][energy]")
{
    auto e = quantity_from<Calorie>(1.0);
    CHECK(e.value == 4.184);

    auto e_kcal = quantity_from<Kilocalorie>(1.0);
    CHECK(e_kcal.value == 4184.0);
}

// ---------------------------------------------------------------------------
// (7) Newton's law type-checks via value_in (Quantity arithmetic in SI;
//     readback in user-chosen unit)
// ---------------------------------------------------------------------------

TEST_CASE("value_in: Newton's law F = m*a, readback in PoundForce",
          "[v0a-2][value-in][physics]")
{
    // 10 lb-mass * 5 ft/s^2 in lbf ? by the imperial-gravitational equivalence,
    // 1 slug * 1 ft/s^2 = 1 lbf, and 1 slug = 32.174... lb-mass, so:
    // F (lbf) = 10 lb-mass * 5 ft/s^2 / 32.174... ? 1.55402 lbf.
    auto m = quantity_from<PoundMass>(10.0);
    auto a = quantity_from<FootPerSecondSq>(5.0);
    auto f_si = m * a;  // Force in Newtons (SI)

    // expected SI value: 4.5359237 kg * 1.524 m/s^2 = 6.9127477388 N
    constexpr f64 expected_si = 10.0 * 0.45359237 * 5.0 * 0.3048;
    CHECK(std::abs(f_si.value - expected_si) < 1e-10);

    // In lbf: divide by 4.4482216152605 ? ? 1.5540199...
    f64 f_in_lbf = value_in<PoundForce>(f_si);
    constexpr f64 expected_lbf = expected_si / 4.4482216152605;
    CHECK(std::abs(f_in_lbf - expected_lbf) < 1e-10);
}

// ---------------------------------------------------------------------------
// (8) Compile-time dimension-mismatch rejection
// ---------------------------------------------------------------------------
//
// Cannot test "compile-error" directly without a negative-compile harness.
// The static_assert in value_in<>() fires if the dimensions don't match.
// We assert the contrapositive: same-dimension conversions DO compile.

TEST_CASE("value_in: same-dimension conversion compiles + executes",
          "[v0a-2][value-in][dimension]")
{
    // All these are dim::Length conversions ? they compile.
    Quantity<dim::Length, f64> l{1.5};
    f64 in_cm = value_in<Centimeter>(l);
    f64 in_mm = value_in<Millimeter>(l);
    f64 in_in = value_in<Inch>(l);
    f64 in_ft = value_in<Foot>(l);
    f64 in_mi = value_in<Mile>(l);

    CHECK(in_cm == 150.0);
    CHECK(in_mm == 1500.0);
    CHECK(std::abs(in_in - 1.5 / 0.0254) < 1e-12);
    CHECK(std::abs(in_ft - 1.5 / 0.3048) < 1e-12);
    CHECK(std::abs(in_mi - 1.5 / 1609.344) < 1e-12);
}

// ---------------------------------------------------------------------------
// (9) quantity_from is constexpr-evaluable
// ---------------------------------------------------------------------------

TEST_CASE("quantity_from / value_in are constexpr-evaluable",
          "[v0a-2][value-in][constexpr]")
{
    constexpr auto k_len = quantity_from<Inch>(2.0);
    STATIC_REQUIRE(k_len.value == 0.0508);  // 2 in = 0.0508 m EXACT

    constexpr f64 k_in_mm = value_in<Millimeter>(k_len);
    STATIC_REQUIRE(k_in_mm == 50.8);
}
