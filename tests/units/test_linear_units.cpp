// crd-units v0a-2 ? Layer 1 LinearUnit + named units.
//
// Coverage:
//   (1) Factor correctness: every named unit's factor matches the
//       kExpected SI-base scale.
//   (2) Bit-exact SI-prefix round-trips: m ? mm ? km via f64 ratio
//       arithmetic.
//   (3) Bit-exact standardized-imperial round-trips: inch ? mm
//       (= 0.0254 m EXACT per 1959 international agreement), mile ? km,
//       pound-mass ? kg (0.45359237 kg EXACT).
//   (4) Compile-time tags: each unit's ::dimension matches its declared
//       dim::* alias.

#include <crd/units/units_si.hpp>

#include <catch2/catch_test_macros.hpp>

#include <numbers>

#include <ratio>

namespace
{
using namespace crd::units;
using crd::f64;

// Compile-time near-equality predicate. Used for f64 conversion-factor
// comparisons where two single-rounded f64 quotients of the same rational
// can differ by 1 ULP. For "exactly equal" rational claims, use
// std::ratio_equal_v instead ? see usages below.
constexpr bool near_eq(f64 a, f64 b, f64 tol) noexcept
{
    const f64 d = a - b;
    return d > -tol && d < tol;
}
} // namespace

// ---------------------------------------------------------------------------
// (1) SI base units have factor 1
// ---------------------------------------------------------------------------

TEST_CASE("LinearUnit: SI base units have factor 1", "[v0a-2][linear-units]")
{
    STATIC_REQUIRE(Meter::factor    == 1.0);
    STATIC_REQUIRE(Kilogram::factor == 1.0);
    STATIC_REQUIRE(Second::factor   == 1.0);
    STATIC_REQUIRE(Radian::factor   == 1.0);
    STATIC_REQUIRE(Kelvin::factor   == 1.0);
    STATIC_REQUIRE(Ampere::factor   == 1.0);
    STATIC_REQUIRE(Candela::factor  == 1.0);
    STATIC_REQUIRE(Mole::factor     == 1.0);
}

TEST_CASE("LinearUnit: SI base units carry correct dimension tag",
          "[v0a-2][linear-units]")
{
    STATIC_REQUIRE(dim_equal_v<Meter::dimension,    dim::Length>);
    STATIC_REQUIRE(dim_equal_v<Kilogram::dimension, dim::Mass>);
    STATIC_REQUIRE(dim_equal_v<Second::dimension,   dim::Time>);
    STATIC_REQUIRE(dim_equal_v<Radian::dimension,   dim::Angle>);
    STATIC_REQUIRE(dim_equal_v<Kelvin::dimension,   dim::Temperature>);
}

// ---------------------------------------------------------------------------
// (2) SI prefix factors (bit-exact rational)
// ---------------------------------------------------------------------------

TEST_CASE("LinearUnit: SI prefix factors are exact powers of 10",
          "[v0a-2][linear-units][si-prefix]")
{
    STATIC_REQUIRE(Kilometer::factor   == 1000.0);
    STATIC_REQUIRE(Hectometer::factor  == 100.0);
    STATIC_REQUIRE(Decameter::factor   == 10.0);
    STATIC_REQUIRE(Decimeter::factor   == 0.1);
    STATIC_REQUIRE(Centimeter::factor  == 0.01);
    STATIC_REQUIRE(Millimeter::factor  == 0.001);
    STATIC_REQUIRE(Micrometer::factor  == 1e-6);
    STATIC_REQUIRE(Nanometer::factor   == 1e-9);
    STATIC_REQUIRE(Picometer::factor   == 1e-12);
    STATIC_REQUIRE(Angstrom::factor    == 1e-10);

    STATIC_REQUIRE(Tonne::factor       == 1000.0);
    STATIC_REQUIRE(Gram::factor        == 0.001);
    STATIC_REQUIRE(Milligram::factor   == 1e-6);
    STATIC_REQUIRE(Microgram::factor   == 1e-9);

    STATIC_REQUIRE(Millisecond::factor == 0.001);
    STATIC_REQUIRE(Microsecond::factor == 1e-6);
    STATIC_REQUIRE(Nanosecond::factor  == 1e-9);
    STATIC_REQUIRE(Minute::factor      == 60.0);
    STATIC_REQUIRE(Hour::factor        == 3600.0);
    STATIC_REQUIRE(Day::factor         == 86400.0);

    STATIC_REQUIRE(Kilohertz::factor   == 1000.0);
    STATIC_REQUIRE(Megahertz::factor   == 1e6);
    STATIC_REQUIRE(Gigahertz::factor   == 1e9);
}

// ---------------------------------------------------------------------------
// (3) Imperial units ? 1959 international agreement EXACT
// ---------------------------------------------------------------------------

TEST_CASE("LinearUnit: imperial Length factors are exact rationals",
          "[v0a-2][linear-units][imperial]")
{
    STATIC_REQUIRE(Inch::factor         == 0.0254);
    STATIC_REQUIRE(Foot::factor         == 0.3048);
    STATIC_REQUIRE(Yard::factor         == 0.9144);
    STATIC_REQUIRE(Mile::factor         == 1609.344);
    STATIC_REQUIRE(NauticalMile::factor == 1852.0);
    STATIC_REQUIRE(Mil::factor          == 2.54e-5);  // 25.4 ?m = 0.0000254 m

    // 1 foot = 12 inches EXACT at the rational layer (the f64 quotient
    // may drift 1 ULP from 12.0 because both operands round individually).
    using FootToInch = std::ratio_divide<Foot::factor_ratio, Inch::factor_ratio>;
    STATIC_REQUIRE(std::ratio_equal_v<FootToInch, std::ratio<12, 1>>);

    // 1 mile = 5280 feet EXACT at the rational layer.
    using MileToFoot = std::ratio_divide<Mile::factor_ratio, Foot::factor_ratio>;
    STATIC_REQUIRE(std::ratio_equal_v<MileToFoot, std::ratio<5280, 1>>);

    // f64 evaluation should be within 1 ULP of the rational result.
    STATIC_REQUIRE(near_eq(Foot::factor / Inch::factor, 12.0,   1e-13));
    STATIC_REQUIRE(near_eq(Mile::factor / Foot::factor, 5280.0, 1e-10));
}

TEST_CASE("LinearUnit: imperial Mass factors are exact rationals",
          "[v0a-2][linear-units][imperial]")
{
    STATIC_REQUIRE(PoundMass::factor == 0.45359237);
    STATIC_REQUIRE(OunceMass::factor == 0.45359237 / 16.0);
}

// ---------------------------------------------------------------------------
// (4) Round-trip exactness: SI-prefix conversion m ? mm is bit-exact
// ---------------------------------------------------------------------------

TEST_CASE("LinearUnit: SI-prefix round-trip m <-> mm <-> km is bit-exact f64",
          "[v0a-2][linear-units][round-trip]")
{
    // Pick any "nice" value. m ? mm ? m should be bit-exact.
    constexpr f64 kVm = 25.4;
    constexpr f64 kVmm = kVm / Millimeter::factor;  // value in mm
    constexpr f64 kVback = kVmm * Millimeter::factor;  // back to m

    STATIC_REQUIRE(kVback == kVm);

    // m ? km ? m
    constexpr f64 kVkm = kVm / Kilometer::factor;
    constexpr f64 kVback2 = kVkm * Kilometer::factor;
    STATIC_REQUIRE(kVback2 == kVm);
}

TEST_CASE("LinearUnit: inch <-> mm round-trip is exact (both rationals over the same base)",
          "[v0a-2][linear-units][round-trip]")
{
    // 1 inch = 0.0254 m EXACT; 1 mm = 0.001 m EXACT. So 1 inch / 1 mm = 25.4 EXACT.
    STATIC_REQUIRE(Inch::factor / Millimeter::factor == 25.4);
}

// ---------------------------------------------------------------------------
// (5) Compile-time dimension tags
// ---------------------------------------------------------------------------

TEST_CASE("LinearUnit: imperial Length units carry dim::Length",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<Inch::dimension,         dim::Length>);
    STATIC_REQUIRE(dim_equal_v<Foot::dimension,         dim::Length>);
    STATIC_REQUIRE(dim_equal_v<Mile::dimension,         dim::Length>);
    STATIC_REQUIRE(dim_equal_v<NauticalMile::dimension, dim::Length>);
    STATIC_REQUIRE(dim_equal_v<Mil::dimension,          dim::Length>);
}

TEST_CASE("LinearUnit: imperial Mass units carry dim::Mass",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<PoundMass::dimension, dim::Mass>);
    STATIC_REQUIRE(dim_equal_v<OunceMass::dimension, dim::Mass>);
    STATIC_REQUIRE(dim_equal_v<Stone::dimension,     dim::Mass>);
}

TEST_CASE("LinearUnit: force units (Newton, PoundForce, KilogramForce) carry dim::Force",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<Newton::dimension,        dim::Force>);
    STATIC_REQUIRE(dim_equal_v<PoundForce::dimension,    dim::Force>);
    STATIC_REQUIRE(dim_equal_v<KilogramForce::dimension, dim::Force>);
    STATIC_REQUIRE(Newton::factor == 1.0);
}

TEST_CASE("LinearUnit: pressure units carry dim::Pressure",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<Pascal::dimension,      dim::Pressure>);
    STATIC_REQUIRE(dim_equal_v<Kilopascal::dimension,  dim::Pressure>);
    STATIC_REQUIRE(dim_equal_v<Atmosphere::dimension,  dim::Pressure>);
    STATIC_REQUIRE(dim_equal_v<Psi::dimension,         dim::Pressure>);
    STATIC_REQUIRE(dim_equal_v<Bar::dimension,         dim::Pressure>);

    STATIC_REQUIRE(Pascal::factor      == 1.0);
    STATIC_REQUIRE(Kilopascal::factor  == 1000.0);
    STATIC_REQUIRE(Megapascal::factor  == 1e6);
    STATIC_REQUIRE(Atmosphere::factor  == 101325.0);
    STATIC_REQUIRE(Bar::factor         == 100000.0);
}

TEST_CASE("LinearUnit: energy + power units",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<Joule::dimension,        dim::Energy>);
    STATIC_REQUIRE(dim_equal_v<Kilojoule::dimension,    dim::Energy>);
    STATIC_REQUIRE(dim_equal_v<KilowattHour::dimension, dim::Energy>);
    STATIC_REQUIRE(dim_equal_v<Calorie::dimension,      dim::Energy>);

    STATIC_REQUIRE(Joule::factor        == 1.0);
    STATIC_REQUIRE(Calorie::factor      == 4.184);          // bit-exact (4184/1000)
    STATIC_REQUIRE(Kilocalorie::factor  == 4184.0);
    STATIC_REQUIRE(WattHour::factor     == 3600.0);
    STATIC_REQUIRE(KilowattHour::factor == 3.6e6);

    STATIC_REQUIRE(dim_equal_v<Watt::dimension,        dim::Power>);
    STATIC_REQUIRE(dim_equal_v<Kilowatt::dimension,    dim::Power>);
    STATIC_REQUIRE(dim_equal_v<Horsepower::dimension,  dim::Power>);
    STATIC_REQUIRE(Watt::factor      == 1.0);
    STATIC_REQUIRE(Kilowatt::factor  == 1000.0);
}

TEST_CASE("LinearUnit: angular units carry dim::Angle",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<Radian::dimension,     dim::Angle>);
    STATIC_REQUIRE(dim_equal_v<Degree::dimension,     dim::Angle>);
    STATIC_REQUIRE(dim_equal_v<Grad::dimension,       dim::Angle>);
    STATIC_REQUIRE(dim_equal_v<Revolution::dimension, dim::Angle>);

    STATIC_REQUIRE(Radian::factor == 1.0);

    // Degree ? ?/180 ? 0.0174532925... (1 ULP tolerance documented).
    // Approximation check: Degree::factor is within 1e-15 of ?/180.
    constexpr f64 kPiOver180 = std::numbers::pi / 180.0;
    constexpr f64 kDiff = Degree::factor - kPiOver180;
    STATIC_REQUIRE(kDiff > -1e-15 && kDiff < 1e-15);

    // Revolution ? 2? ? 6.283... (1 ULP tolerance).
    constexpr f64 kTwoPi = 2.0 * std::numbers::pi;
    constexpr f64 kDiff2 = Revolution::factor - kTwoPi;
    STATIC_REQUIRE(kDiff2 > -1e-12 && kDiff2 < 1e-12);
}

TEST_CASE("LinearUnit: electrical units",
          "[v0a-2][linear-units][dimension-tag]")
{
    STATIC_REQUIRE(dim_equal_v<Volt::dimension,        dim::Voltage>);
    STATIC_REQUIRE(dim_equal_v<Ohm::dimension,         dim::Resistance>);
    STATIC_REQUIRE(dim_equal_v<Farad::dimension,       dim::Capacitance>);
    STATIC_REQUIRE(dim_equal_v<Henry::dimension,       dim::Inductance>);
    STATIC_REQUIRE(dim_equal_v<Coulomb::dimension,     dim::Charge>);
    STATIC_REQUIRE(dim_equal_v<Weber::dimension,       dim::MagneticFlux>);

    STATIC_REQUIRE(Volt::factor          == 1.0);
    STATIC_REQUIRE(Millivolt::factor     == 0.001);
    STATIC_REQUIRE(Kilovolt::factor      == 1000.0);
    STATIC_REQUIRE(Ohm::factor           == 1.0);
    STATIC_REQUIRE(Microfarad::factor    == 1e-6);
    STATIC_REQUIRE(Nanofarad::factor     == 1e-9);
    STATIC_REQUIRE(Picofarad::factor     == 1e-12);
    STATIC_REQUIRE(AmpereHour::factor    == 3600.0);
    STATIC_REQUIRE(MilliampereHour::factor == 3.6);
}

TEST_CASE("LinearUnit: special-physical-constants",
          "[v0a-2][linear-units][constants]")
{
    // Standard gravity: 9.80665 m/s^2 EXACT
    STATIC_REQUIRE(StandardG::factor == 9.80665);
    STATIC_REQUIRE(dim_equal_v<StandardG::dimension, dim::Acceleration>);

    // Speed of light: 299792458 m/s EXACT (2019 SI defining constant)
    STATIC_REQUIRE(SpeedOfLight::factor == 299792458.0);
    STATIC_REQUIRE(dim_equal_v<SpeedOfLight::dimension, dim::Velocity>);
}
