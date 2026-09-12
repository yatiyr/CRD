// crd-units v0a-3 -- Layer 2 AffineUnit + Temperature/TemperatureDelta tests.

#include <crd/units/units_affine.hpp>
#include <crd/units/units_si.hpp>  // Kelvin (the linear-form Temperature unit)

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
using namespace crd::units;
using crd::f64;
} // namespace

// ---------------------------------------------------------------------------
// (1) Affine factors
// ---------------------------------------------------------------------------

TEST_CASE("AffineUnit: Celsius scale=1 offset=273.15", "[v0a-3][affine]")
{
    STATIC_REQUIRE(Celsius::scale == 1.0);
    STATIC_REQUIRE(Celsius::offset == 273.15);
    STATIC_REQUIRE(Celsius::is_affine);
}

TEST_CASE("AffineUnit: Fahrenheit scale=5/9 offset~=255.372", "[v0a-3][affine]")
{
    // K = (5/9) * F + (5/9 * 459.67) = (5/9)*F + 45967/180
    STATIC_REQUIRE(Fahrenheit::scale == 5.0 / 9.0);
    constexpr f64 expected_offset = 45967.0 / 180.0;
    STATIC_REQUIRE(Fahrenheit::offset == expected_offset);
    STATIC_REQUIRE(Fahrenheit::is_affine);
}

TEST_CASE("AffineUnit: Rankine scale=5/9 offset=0", "[v0a-3][affine]")
{
    STATIC_REQUIRE(Rankine::scale == 5.0 / 9.0);
    STATIC_REQUIRE(Rankine::offset == 0.0);
}

// ---------------------------------------------------------------------------
// (2) temperature_from / value_in_temperature
// ---------------------------------------------------------------------------

TEST_CASE("AffineUnit: 0 C = 273.15 K", "[v0a-3][affine][celsius]")
{
    auto t = temperature_from<Celsius>(0.0);
    CHECK(std::abs(t.value - 273.15) < 1e-10);
}

TEST_CASE("AffineUnit: 100 C = 373.15 K", "[v0a-3][affine][celsius]")
{
    auto t = temperature_from<Celsius>(100.0);
    CHECK(std::abs(t.value - 373.15) < 1e-10);
}

TEST_CASE("AffineUnit: Celsius round-trip 0 C -> K -> 0 C",
          "[v0a-3][affine][celsius][round-trip]")
{
    auto t = temperature_from<Celsius>(0.0);
    f64 back = value_in_temperature<Celsius>(t);
    CHECK(back == 0.0);
}

TEST_CASE("AffineUnit: 32 F = 273.15 K (freezing water)",
          "[v0a-3][affine][fahrenheit]")
{
    auto t = temperature_from<Fahrenheit>(32.0);
    CHECK(std::abs(t.value - 273.15) < 1e-10);
}

TEST_CASE("AffineUnit: 212 F = 373.15 K (boiling water)",
          "[v0a-3][affine][fahrenheit]")
{
    auto t = temperature_from<Fahrenheit>(212.0);
    CHECK(std::abs(t.value - 373.15) < 1e-10);
}

TEST_CASE("AffineUnit: cross-conversion 100 C = 212 F",
          "[v0a-3][affine][cross]")
{
    auto t = temperature_from<Celsius>(100.0);
    f64 in_f = value_in_temperature<Fahrenheit>(t);
    CHECK(std::abs(in_f - 212.0) < 1e-10);
}

TEST_CASE("AffineUnit: cross-conversion 0 C = 32 F",
          "[v0a-3][affine][cross]")
{
    auto t = temperature_from<Celsius>(0.0);
    f64 in_f = value_in_temperature<Fahrenheit>(t);
    CHECK(std::abs(in_f - 32.0) < 1e-10);
}

TEST_CASE("AffineUnit: Kelvin (linear) ingress/egress",
          "[v0a-3][affine][kelvin]")
{
    auto t = temperature_from<Kelvin>(300.0);
    CHECK(t.value == 300.0);
    f64 back = value_in_temperature<Kelvin>(t);
    CHECK(back == 300.0);
}

// ---------------------------------------------------------------------------
// (3) Temperature - Temperature = TemperatureDelta (subtraction strips offset)
// ---------------------------------------------------------------------------

TEST_CASE("Temperature - Temperature returns TemperatureDelta",
          "[v0a-3][affine][delta]")
{
    auto a = temperature_from<Celsius>(100.0);  // 373.15 K
    auto b = temperature_from<Celsius>(25.0);   // 298.15 K

    TemperatureDelta<f64> diff = a - b;
    CHECK(diff.value == 75.0);  // delta of 75 K (= 75 C-degrees)
}

TEST_CASE("Temperature + TemperatureDelta returns Temperature",
          "[v0a-3][affine][delta]")
{
    auto base = temperature_from<Celsius>(25.0);  // 298.15 K
    TemperatureDelta<f64> delta{75.0};            // +75 K

    Temperature<f64> result = base + delta;
    CHECK(result.value == 373.15);
}

TEST_CASE("Temperature - TemperatureDelta returns Temperature",
          "[v0a-3][affine][delta]")
{
    auto base = temperature_from<Celsius>(100.0);  // 373.15 K
    TemperatureDelta<f64> delta{75.0};

    Temperature<f64> result = base - delta;
    CHECK(result.value == 298.15);
}

TEST_CASE("TemperatureDelta + TemperatureDelta = TemperatureDelta",
          "[v0a-3][affine][delta]")
{
    TemperatureDelta<f64> a{50.0};
    TemperatureDelta<f64> b{25.0};
    TemperatureDelta<f64> sum = a + b;
    CHECK(sum.value == 75.0);
}

TEST_CASE("TemperatureDelta * scalar is allowed (it's a regular Quantity)",
          "[v0a-3][affine][delta]")
{
    TemperatureDelta<f64> a{50.0};
    auto doubled = a * 2.0;
    CHECK(doubled.value == 100.0);
}

// ---------------------------------------------------------------------------
// (4) Comparison
// ---------------------------------------------------------------------------

TEST_CASE("Temperature comparison",
          "[v0a-3][affine][cmp]")
{
    auto a = temperature_from<Celsius>(25.0);
    auto b = temperature_from<Celsius>(100.0);
    auto c = temperature_from<Celsius>(25.0);

    CHECK(a == c);
    CHECK(a != b);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a <= c);
}

// ---------------------------------------------------------------------------
// (5) The trap closes at the type level (these would compile-error if
// uncommented):
//   Temperature<f64> t1 = ...;
//   Temperature<f64> t2 = ...;
//   auto bad = t1 + t2;     // ERROR: no operator+ between two AbsoluteQuantity
//   auto bad = -t1;          // ERROR: no unary-minus on AbsoluteQuantity
//   auto bad = t1 * 2.0;     // ERROR: no scalar mul on AbsoluteQuantity
//
// We verify the CONTRAPOSITIVE: allowed ops compile. Compile-error
// negative-tests would require a separate failed-compile harness.
// ---------------------------------------------------------------------------
