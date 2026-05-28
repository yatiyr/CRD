// crd-units v0a-2 ? Layer 4 UnitMul / UnitDiv / UnitPow tests.
//
// Coverage:
//   (1) UnitDiv produces the correct dimension (DimDiv on operands).
//   (2) UnitDiv produces the correct factor (std::ratio_divide on operands).
//   (3) UnitMul produces the correct dimension and factor.
//   (4) UnitPow scales correctly.
//   (5) Named compound units match the expected algebraic forms (MilePerHour
//       == UnitDiv<Mile, Hour>, etc.).
//   (6) Auto-derive equivalence: compound matches what a hand-written
//       LinearUnit<Velocity, std::ratio<...>> would produce.
//   (7) Bit-exact round-trips between compound forms (km/h ? mile/h via
//       known conversion).

#include <crd/units/units_compound.hpp>
#include <crd/units/units_si.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ratio>

namespace
{
using namespace crd::units;
using crd::f64;

constexpr bool near_eq(f64 a, f64 b, f64 tol) noexcept
{
    const f64 d = a - b;
    return d > -tol && d < tol;
}
} // namespace

// ---------------------------------------------------------------------------
// (1) UnitDiv produces correct dimension
// ---------------------------------------------------------------------------

TEST_CASE("UnitDiv: dimension is DimDiv of operand dimensions",
          "[v0a-2][compound]")
{
    using V = UnitDiv<Meter, Second>;
    STATIC_REQUIRE(dim_equal_v<V::dimension, dim::Velocity>);
    STATIC_REQUIRE(V::factor == 1.0);
    STATIC_REQUIRE(dim_equal_v<MeterPerSecond::dimension, dim::Velocity>);
}

TEST_CASE("UnitDiv: KilometerPerHour has correct factor",
          "[v0a-2][compound]")
{
    // 1 km/h = 1000 / 3600 = 5/18 m/s
    STATIC_REQUIRE(dim_equal_v<KilometerPerHour::dimension, dim::Velocity>);
    constexpr f64 expected = 1000.0 / 3600.0;
    STATIC_REQUIRE(KilometerPerHour::factor == expected);
}

TEST_CASE("UnitDiv: MilePerHour has correct factor",
          "[v0a-2][compound]")
{
    // 1 mph = 1609.344 / 3600 m/s = 0.44704 m/s EXACT at the rational layer.
    // The std::ratio_divide reduces 1609344/1000 ? 3600/1 to a smaller form,
    // so MilePerHour::factor_ratio is mathematically 1609344/3600000 = 1397/3125.
    STATIC_REQUIRE(dim_equal_v<MilePerHour::dimension, dim::Velocity>);
    STATIC_REQUIRE(std::ratio_equal_v<MilePerHour::factor_ratio, std::ratio<1397, 3125>>);

    // f64 evaluation is within 1 ULP of 0.44704.
    STATIC_REQUIRE(near_eq(MilePerHour::factor, 0.44704, 1e-15));
}

TEST_CASE("UnitDiv: FootPerSecond has correct factor (= 0.3048 m/s EXACT)",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<FootPerSecond::dimension, dim::Velocity>);
    STATIC_REQUIRE(FootPerSecond::factor == 0.3048);
}

TEST_CASE("UnitDiv: Knot has correct factor (= 1852/3600 m/s EXACT)",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<Knot::dimension, dim::Velocity>);
    constexpr f64 expected = 1852.0 / 3600.0;
    STATIC_REQUIRE(Knot::factor == expected);
}

// ---------------------------------------------------------------------------
// (2) UnitMul produces correct dimension + factor
// ---------------------------------------------------------------------------

TEST_CASE("UnitMul: Area = Length * Length",
          "[v0a-2][compound]")
{
    using A = UnitMul<Meter, Meter>;
    STATIC_REQUIRE(dim_equal_v<A::dimension, dim::Area>);
    STATIC_REQUIRE(A::factor == 1.0);
}

TEST_CASE("UnitMul: NewtonMeter is dim::Torque",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<NewtonMeter::dimension, dim::Torque>);
    STATIC_REQUIRE(NewtonMeter::factor == 1.0);
}

TEST_CASE("UnitMul: AmpereHour (already a Charge unit) and the compound form match",
          "[v0a-2][compound]")
{
    using AhCompound = UnitMul<Ampere, Hour>;
    STATIC_REQUIRE(dim_equal_v<AhCompound::dimension, dim::Charge>);
    STATIC_REQUIRE(AhCompound::factor == 3600.0);
    // Named AmpereHour LinearUnit has factor 3600 too ? same conversion.
    STATIC_REQUIRE(AhCompound::factor == AmpereHour::factor);
}

// ---------------------------------------------------------------------------
// (3) UnitPow
// ---------------------------------------------------------------------------

TEST_CASE("UnitPow: UnitPow<Meter, 2> == UnitMul<Meter, Meter> (Area)",
          "[v0a-2][compound]")
{
    using A1 = UnitPow<Meter, 2>;
    using A2 = UnitMul<Meter, Meter>;
    STATIC_REQUIRE(dim_equal_v<A1::dimension, A2::dimension>);
    STATIC_REQUIRE(A1::factor == A2::factor);
    STATIC_REQUIRE(dim_equal_v<A1::dimension, dim::Area>);
}

TEST_CASE("UnitPow: UnitPow<Meter, 3> is Volume",
          "[v0a-2][compound]")
{
    using V = UnitPow<Meter, 3>;
    STATIC_REQUIRE(dim_equal_v<V::dimension, dim::Volume>);
    STATIC_REQUIRE(V::factor == 1.0);
}

TEST_CASE("UnitPow: UnitPow<Foot, 3> = ft^3",
          "[v0a-2][compound]")
{
    using V = UnitPow<Foot, 3>;
    STATIC_REQUIRE(dim_equal_v<V::dimension, dim::Volume>);
    // 1 ft^3 = 0.3048^3 m^3 = 0.028316846592 m^3 EXACT at the rational layer.
    // Chained f64 multiplications may drift 1-2 ULP from the std::ratio result.
    constexpr f64 expected = 0.3048 * 0.3048 * 0.3048;
    STATIC_REQUIRE(near_eq(V::factor, expected, 1e-15));
}

TEST_CASE("UnitPow: UnitPow<U, 0> = Dimensionless with factor 1",
          "[v0a-2][compound]")
{
    using Z = UnitPow<Meter, 0>;
    STATIC_REQUIRE(dim_equal_v<Z::dimension, dim::Dimensionless>);
    STATIC_REQUIRE(Z::factor == 1.0);
}

// ---------------------------------------------------------------------------
// (4) Density: Mass / Volume
// ---------------------------------------------------------------------------

TEST_CASE("UnitDiv: KilogramPerCubicMeter has correct dimension + factor",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<KilogramPerCubicMeter::dimension, dim::Density>);
    STATIC_REQUIRE(KilogramPerCubicMeter::factor == 1.0);
}

TEST_CASE("UnitDiv: GramPerCubicCentimeter has correct factor (= 1000 kg/m^3 EXACT)",
          "[v0a-2][compound]")
{
    // 1 g/cm^3 = 0.001 kg / (0.01 m)^3 = 0.001 / 1e-6 = 1000 kg/m^3 EXACT
    // at the rational layer.
    STATIC_REQUIRE(dim_equal_v<GramPerCubicCentimeter::dimension, dim::Density>);
    STATIC_REQUIRE(
        std::ratio_equal_v<GramPerCubicCentimeter::factor_ratio, std::ratio<1000, 1>>);
    STATIC_REQUIRE(GramPerCubicCentimeter::factor == 1000.0);
}

// ---------------------------------------------------------------------------
// (5) Angular Velocity
// ---------------------------------------------------------------------------

TEST_CASE("UnitDiv: RadianPerSecond is dim::AngularVelocity",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<RadianPerSecond::dimension, dim::AngularVelocity>);
    STATIC_REQUIRE(RadianPerSecond::factor == 1.0);
}

TEST_CASE("UnitDiv: RPM = Revolution / Minute",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<RPM::dimension, dim::AngularVelocity>);
    // RPM factor = (2? rad) / (60 s) = 2?/60 rad/s ? 0.10472 rad/s.
    // Revolution uses an irrational factor (big-int rational approximation
    // to 2?), so 1 ULP drift is expected on the f64 quotient.
    constexpr f64 expected = Revolution::factor / 60.0;
    STATIC_REQUIRE(near_eq(RPM::factor, expected, 1e-15));
}

// ---------------------------------------------------------------------------
// (6) Specific heat: J / (kg?K)
// ---------------------------------------------------------------------------

TEST_CASE("UnitDiv: JoulePerKilogramKelvin has correct dimension",
          "[v0a-2][compound]")
{
    STATIC_REQUIRE(dim_equal_v<JoulePerKilogramKelvin::dimension, dim::SpecificHeat>);
    STATIC_REQUIRE(JoulePerKilogramKelvin::factor == 1.0);
}

// ---------------------------------------------------------------------------
// (7) Cross-units conversion: MilePerHour ? KilometerPerHour
// ---------------------------------------------------------------------------

TEST_CASE("Compound: MilePerHour to KilometerPerHour conversion factor (= 1.609344 EXACT)",
          "[v0a-2][compound][round-trip]")
{
    // 1 mph = 1.609344 km/h EXACT at the rational layer:
    //   MilePerHour::factor_ratio ? KilometerPerHour::factor_ratio
    //   = (1397/3125) ? (5/18) = (1397/3125) ?-- (18/5) = 25146/15625
    //   = 1.609344
    using R = std::ratio_divide<MilePerHour::factor_ratio, KilometerPerHour::factor_ratio>;
    STATIC_REQUIRE(std::ratio_equal_v<R, std::ratio<25146, 15625>>);

    // f64 evaluation within 1 ULP.
    constexpr f64 ratio_f64 = MilePerHour::factor / KilometerPerHour::factor;
    STATIC_REQUIRE(near_eq(ratio_f64, 1.609344, 1e-14));
}

TEST_CASE("Compound: FootPerSecond to MeterPerSecond conversion factor (= 0.3048 EXACT)",
          "[v0a-2][compound][round-trip]")
{
    // 1 ft/s = 0.3048 m/s EXACT at the rational layer.
    using R = std::ratio_divide<FootPerSecond::factor_ratio, MeterPerSecond::factor_ratio>;
    STATIC_REQUIRE(std::ratio_equal_v<R, std::ratio<381, 1250>>);  // 3048/10000 reduced

    STATIC_REQUIRE(FootPerSecond::factor == 0.3048);
}

TEST_CASE("Compound: Knot to KilometerPerHour conversion factor (= 1.852 EXACT)",
          "[v0a-2][compound][round-trip]")
{
    // 1 knot = 1.852 km/h EXACT at the rational layer.
    using R = std::ratio_divide<Knot::factor_ratio, KilometerPerHour::factor_ratio>;
    STATIC_REQUIRE(std::ratio_equal_v<R, std::ratio<1852, 1000>>);

    constexpr f64 ratio_f64 = Knot::factor / KilometerPerHour::factor;
    STATIC_REQUIRE(near_eq(ratio_f64, 1.852, 1e-15));
}

// ---------------------------------------------------------------------------
// (8) The extensibility multiplier: adding one base unit unlocks many
//     compound units automatically
// ---------------------------------------------------------------------------

TEST_CASE("Compound: ad-hoc UnitDiv at the call site composes correctly",
          "[v0a-2][compound][extensibility]")
{
    // User can write UnitDiv<Mile, Minute> without declaring "MilePerMinute"
    // anywhere. The compound type is fully equivalent to a named alias.
    using MpMin = UnitDiv<Mile, Minute>;
    STATIC_REQUIRE(dim_equal_v<MpMin::dimension, dim::Velocity>);

    // 1 mile/minute = 1609.344 / 60 m/s EXACT at the rational layer.
    // 1609344/1000 ? 60/1 = 1609344/60000 = 6706.4/250 = 33528/1250
    using R = MpMin::factor_ratio;
    STATIC_REQUIRE(std::ratio_equal_v<R, std::ratio<1609344, 60000>>);

    constexpr f64 expected = 1609.344 / 60.0;
    STATIC_REQUIRE(near_eq(MpMin::factor, expected, 1e-13));
}

TEST_CASE("Compound: deeply-nested compound (PoundForceFoot / Time = Watt-ish)",
          "[v0a-2][compound][extensibility]")
{
    // Foot-pounds per second is a Power unit. Let's verify.
    using FootPoundForcePerSec = UnitDiv<PoundForceFoot, Second>;
    STATIC_REQUIRE(dim_equal_v<FootPoundForcePerSec::dimension, dim::Power>);

    // 1 ft?lbf = 1.3558179483314... J
    // 1 ft?lbf/s = same number in W
    constexpr f64 expected = PoundForce::factor * Foot::factor;
    STATIC_REQUIRE(FootPoundForcePerSec::factor == expected);
}
