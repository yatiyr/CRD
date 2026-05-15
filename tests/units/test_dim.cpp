// crd-units v0a-1 -- Dim<...> arithmetic tests.
//
// STATIC_REQUIRE the compile-time dimensional arithmetic:
//   - Base dimensions have the expected exponents.
//   - DimMul / DimDiv / DimInv / DimPow produce the algebraic result.
//   - Round-trip identities (L * L^-1 = Dimensionless, Frequency * Time =
//     Dimensionless, etc.).
//   - Derived dimensions in dim_aliases.hpp match the algebraic form
//     (dim::Velocity == DimDiv<dim::Length, dim::Time>).
//
// All tests are STATIC_REQUIRE -- purely compile-time. No runtime.

#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::units;
}

// ---------------------------------------------------------------------------
// (1) Base dimension exponents
// ---------------------------------------------------------------------------

TEST_CASE("Dim: base dimension exponents are correct", "[v0a-1][dim]")
{
    STATIC_REQUIRE(dim::Length::length == 1);
    STATIC_REQUIRE(dim::Length::mass == 0);
    STATIC_REQUIRE(dim::Length::time == 0);
    STATIC_REQUIRE(dim::Length::angle == 0);

    STATIC_REQUIRE(dim::Mass::mass == 1);
    STATIC_REQUIRE(dim::Mass::length == 0);

    STATIC_REQUIRE(dim::Time::time == 1);
    STATIC_REQUIRE(dim::Current::current == 1);
    STATIC_REQUIRE(dim::Temperature::temperature == 1);
    STATIC_REQUIRE(dim::Amount::amount == 1);
    STATIC_REQUIRE(dim::LuminousI::luminous_intensity == 1);
    STATIC_REQUIRE(dim::Angle::angle == 1);

    // Dimensionless = all zeros
    STATIC_REQUIRE(dim::Dimensionless::length == 0);
    STATIC_REQUIRE(dim::Dimensionless::mass == 0);
    STATIC_REQUIRE(dim::Dimensionless::time == 0);
    STATIC_REQUIRE(dim::Dimensionless::current == 0);
    STATIC_REQUIRE(dim::Dimensionless::temperature == 0);
    STATIC_REQUIRE(dim::Dimensionless::amount == 0);
    STATIC_REQUIRE(dim::Dimensionless::luminous_intensity == 0);
    STATIC_REQUIRE(dim::Dimensionless::angle == 0);
}

TEST_CASE("Dim: dim_equal_v identity / reflexivity / symmetry", "[v0a-1][dim]")
{
    STATIC_REQUIRE(dim_equal_v<dim::Length, dim::Length>);
    STATIC_REQUIRE(!dim_equal_v<dim::Length, dim::Mass>);
    STATIC_REQUIRE(!dim_equal_v<dim::Length, dim::Time>);
    STATIC_REQUIRE(dim_equal_v<dim::Dimensionless, dim::Dimensionless>);
}

// ---------------------------------------------------------------------------
// (2) DimMul / DimDiv produce the algebraic result
// ---------------------------------------------------------------------------

TEST_CASE("Dim: DimMul produces correct exponents", "[v0a-1][dim][arith]")
{
    using Area2 = DimMul<dim::Length, dim::Length>;
    STATIC_REQUIRE(Area2::length == 2);
    STATIC_REQUIRE(Area2::mass == 0);
    STATIC_REQUIRE(dim_equal_v<Area2, dim::Area>);

    using MassLen = DimMul<dim::Mass, dim::Length>;
    STATIC_REQUIRE(MassLen::mass == 1);
    STATIC_REQUIRE(MassLen::length == 1);
}

TEST_CASE("Dim: DimDiv produces correct exponents", "[v0a-1][dim][arith]")
{
    using V = DimDiv<dim::Length, dim::Time>;
    STATIC_REQUIRE(V::length == 1);
    STATIC_REQUIRE(V::time == -1);
    STATIC_REQUIRE(V::mass == 0);
    STATIC_REQUIRE(dim_equal_v<V, dim::Velocity>);

    using A = DimDiv<V, dim::Time>;
    STATIC_REQUIRE(A::length == 1);
    STATIC_REQUIRE(A::time == -2);
    STATIC_REQUIRE(dim_equal_v<A, dim::Acceleration>);
}

TEST_CASE("Dim: Newton's second law: F = m * a", "[v0a-1][dim][arith]")
{
    using F_from_ma = DimMul<dim::Mass, dim::Acceleration>;
    STATIC_REQUIRE(F_from_ma::mass == 1);
    STATIC_REQUIRE(F_from_ma::length == 1);
    STATIC_REQUIRE(F_from_ma::time == -2);
    STATIC_REQUIRE(dim_equal_v<F_from_ma, dim::Force>);
}

TEST_CASE("Dim: Energy = Force * Length = Mass * Velocity^2", "[v0a-1][dim][arith]")
{
    using E1 = DimMul<dim::Force, dim::Length>;
    using E2 = DimMul<dim::Mass, DimPow<dim::Velocity, 2>>;
    STATIC_REQUIRE(dim_equal_v<E1, E2>);
    STATIC_REQUIRE(dim_equal_v<E1, dim::Energy>);
}

TEST_CASE("Dim: Power = Energy / Time", "[v0a-1][dim][arith]")
{
    using P = DimDiv<dim::Energy, dim::Time>;
    STATIC_REQUIRE(P::mass == 1);
    STATIC_REQUIRE(P::length == 2);
    STATIC_REQUIRE(P::time == -3);
    STATIC_REQUIRE(dim_equal_v<P, dim::Power>);
}

TEST_CASE("Dim: Pressure = Force / Area", "[v0a-1][dim][arith]")
{
    using Pr = DimDiv<dim::Force, dim::Area>;
    STATIC_REQUIRE(Pr::mass == 1);
    STATIC_REQUIRE(Pr::length == -1);
    STATIC_REQUIRE(Pr::time == -2);
    STATIC_REQUIRE(dim_equal_v<Pr, dim::Pressure>);
}

// ---------------------------------------------------------------------------
// (3) DimInv and DimPow
// ---------------------------------------------------------------------------

TEST_CASE("Dim: DimInv negates all exponents", "[v0a-1][dim][arith]")
{
    using InvL = DimInv<dim::Length>;
    STATIC_REQUIRE(InvL::length == -1);
    STATIC_REQUIRE(InvL::mass == 0);

    using InvE = DimInv<dim::Energy>;
    STATIC_REQUIRE(InvE::mass == -1);
    STATIC_REQUIRE(InvE::length == -2);
    STATIC_REQUIRE(InvE::time == 2);
}

TEST_CASE("Dim: DimPow scales all exponents by N", "[v0a-1][dim][arith]")
{
    using AreaSquared = DimPow<dim::Area, 2>;  // Length^4
    STATIC_REQUIRE(AreaSquared::length == 4);

    using NoOp = DimPow<dim::Length, 0>;
    STATIC_REQUIRE(dim_equal_v<NoOp, dim::Dimensionless>);

    using Cube = DimPow<dim::Length, 3>;
    STATIC_REQUIRE(dim_equal_v<Cube, dim::Volume>);

    using NegSquare = DimPow<dim::Length, -2>;
    STATIC_REQUIRE(NegSquare::length == -2);
}

// ---------------------------------------------------------------------------
// (4) Round-trip identities
// ---------------------------------------------------------------------------

TEST_CASE("Dim: L * L^-1 = Dimensionless", "[v0a-1][dim][identity]")
{
    using Roundtrip = DimMul<dim::Length, DimInv<dim::Length>>;
    STATIC_REQUIRE(dim_equal_v<Roundtrip, dim::Dimensionless>);
}

TEST_CASE("Dim: (Length / Time) * Time = Length", "[v0a-1][dim][identity]")
{
    using Vel = DimDiv<dim::Length, dim::Time>;
    using BackToLen = DimMul<Vel, dim::Time>;
    STATIC_REQUIRE(dim_equal_v<BackToLen, dim::Length>);
}

TEST_CASE("Dim: Frequency * Time = Dimensionless", "[v0a-1][dim][identity]")
{
    using FT = DimMul<dim::Frequency, dim::Time>;
    STATIC_REQUIRE(dim_equal_v<FT, dim::Dimensionless>);
}

TEST_CASE("Dim: Momentum / Mass = Velocity", "[v0a-1][dim][identity]")
{
    using V = DimDiv<dim::Momentum, dim::Mass>;
    STATIC_REQUIRE(dim_equal_v<V, dim::Velocity>);
}

// ---------------------------------------------------------------------------
// (5) Dimensional degeneracies (documented in dim_aliases.hpp)
// ---------------------------------------------------------------------------

TEST_CASE("Dim: Energy and Torque share Dim in strict SI",
          "[v0a-1][dim][degenerate]")
{
    // Both reduce to kg*m^2/s^2. Disambiguation via "kind" tag is a v0a-3
    // (or later) amendment when a consumer needs it; v0a-1 ships them as
    // dimensional aliases.
    STATIC_REQUIRE(dim_equal_v<dim::Energy, dim::Torque>);
}

TEST_CASE("Dim: Frequency and AngularVelocity are DISTINCT under Cerid's Angle-tagged scheme",
          "[v0a-1][dim][degenerate]")
{
    // In strict SI both are s^-1, but tagging Angle as the 8th base lets
    // us split: Frequency has angle=0, AngularVelocity has angle=1.
    STATIC_REQUIRE(!dim_equal_v<dim::Frequency, dim::AngularVelocity>);
    STATIC_REQUIRE(dim::Frequency::angle == 0);
    STATIC_REQUIRE(dim::AngularVelocity::angle == 1);
}

// ---------------------------------------------------------------------------
// (6) Compound derived dimensions
// ---------------------------------------------------------------------------

TEST_CASE("Dim: Electrical aliases derive correctly", "[v0a-1][dim][electrical]")
{
    // Charge = Current * Time
    STATIC_REQUIRE(dim::Charge::current == 1);
    STATIC_REQUIRE(dim::Charge::time == 1);

    // Voltage = Power / Current = kg*m^2/(A*s^3)
    STATIC_REQUIRE(dim::Voltage::mass == 1);
    STATIC_REQUIRE(dim::Voltage::length == 2);
    STATIC_REQUIRE(dim::Voltage::time == -3);
    STATIC_REQUIRE(dim::Voltage::current == -1);

    // Resistance = Voltage / Current = kg*m^2/(A^2*s^3)
    STATIC_REQUIRE(dim::Resistance::current == -2);

    // Conductance = 1 / Resistance
    STATIC_REQUIRE(dim_equal_v<dim::Conductance, DimInv<dim::Resistance>>);
}

TEST_CASE("Dim: Thermodynamic aliases derive correctly", "[v0a-1][dim][thermo]")
{
    // SpecificHeat = HeatCapacity / Mass = J/(kg*K)
    STATIC_REQUIRE(dim::SpecificHeat::mass == 0);
    STATIC_REQUIRE(dim::SpecificHeat::length == 2);
    STATIC_REQUIRE(dim::SpecificHeat::time == -2);
    STATIC_REQUIRE(dim::SpecificHeat::temperature == -1);
}

TEST_CASE("Dim: Fluid viscosity aliases derive correctly", "[v0a-1][dim][fluid]")
{
    // DynamicViscosity = Pressure * Time = Pa*s = kg/(m*s)
    STATIC_REQUIRE(dim::DynamicViscosity::mass == 1);
    STATIC_REQUIRE(dim::DynamicViscosity::length == -1);
    STATIC_REQUIRE(dim::DynamicViscosity::time == -1);

    // KinematicViscosity = DynamicViscosity / Density = m^2/s
    STATIC_REQUIRE(dim::KinematicViscosity::length == 2);
    STATIC_REQUIRE(dim::KinematicViscosity::time == -1);
    STATIC_REQUIRE(dim::KinematicViscosity::mass == 0);
}
