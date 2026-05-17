// crd-units v0a-1 ? Quantity<D, T> layout + arithmetic tests.
//
// Layout pins (compile-time): sizeof / alignof / standard_layout /
// trivially_copyable ? all required for SIMD-reinterpret + GPU upload to
// work. If any of these regress, the test target fails to compile.
//
// Runtime tests cover:
//   - Default construction zero-initializes.
//   - Explicit construction sets value.
//   - Same-dimension arithmetic (operator+ / - / += / -=).
//   - Scalar multiplication / division.
//   - Cross-dimension multiplication: Quantity<D1> * Quantity<D2> ->
//     Quantity<DimMul<D1, D2>>.
//   - Cross-dimension division: Quantity<D1> / Quantity<D2> ->
//     Quantity<DimDiv<D1, D2>>.
//   - Scalar / Quantity -> Quantity<DimInv<D>>.
//   - Newton's law type-checks: m * a == Force.
//   - Integration step type-checks: v += a * dt; pos += v * dt.
//   - Comparison.

#include <crd/units/quantity.hpp>

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace
{
using namespace crd::units;
using crd::f32;
using crd::f64;
} // namespace

// ---------------------------------------------------------------------------
// (1) Layout pins ? compile-time
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: layout pins (sizeof / alignof / std-layout / trivially-copyable)",
          "[v0a-1][quantity][layout]")
{
    STATIC_REQUIRE(sizeof(Quantity<dim::Length, f32>) == sizeof(f32));
    STATIC_REQUIRE(sizeof(Quantity<dim::Length, f64>) == sizeof(f64));
    STATIC_REQUIRE(sizeof(Quantity<dim::Mass, f32>) == sizeof(f32));
    STATIC_REQUIRE(sizeof(Quantity<dim::Mass, f64>) == sizeof(f64));
    STATIC_REQUIRE(sizeof(Quantity<dim::Time, f32>) == sizeof(f32));
    STATIC_REQUIRE(sizeof(Quantity<dim::Angle, f32>) == sizeof(f32));

    STATIC_REQUIRE(alignof(Quantity<dim::Length, f32>) == alignof(f32));
    STATIC_REQUIRE(alignof(Quantity<dim::Length, f64>) == alignof(f64));

    STATIC_REQUIRE(std::is_standard_layout_v<Quantity<dim::Length, f32>>);
    STATIC_REQUIRE(std::is_standard_layout_v<Quantity<dim::Mass, f64>>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Quantity<dim::Length, f32>>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Quantity<dim::Time, f64>>);
}

TEST_CASE("Quantity: requires floating-point T (i32 / int rejected at compile time)",
          "[v0a-1][quantity][layout]")
{
    // Force the floating-point requirement to bind.
    STATIC_REQUIRE(std::is_floating_point_v<Quantity<dim::Length, f32>::scalar>);
    STATIC_REQUIRE(std::is_floating_point_v<Quantity<dim::Length, f64>::scalar>);
    // Quantity<dim::Length, int> would static_assert at instantiation ? we
    // can't write that in a TEST_CASE without breaking compilation; the
    // static_assert on Quantity::T proves the contract.
}

// ---------------------------------------------------------------------------
// (2) Construction
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: default construction zero-initializes",
          "[v0a-1][quantity][ctor]")
{
    Quantity<dim::Length, f32> q;
    CHECK(q.value == 0.0F);

    Quantity<dim::Mass, f64> m;
    CHECK(m.value == 0.0);

    Quantity<dim::Time, f32> t;
    CHECK(t.value == 0.0F);
}

TEST_CASE("Quantity: explicit construction sets value",
          "[v0a-1][quantity][ctor]")
{
    Quantity<dim::Length, f32> q{2.5F};
    CHECK(q.value == 2.5F);

    Quantity<dim::Mass, f64> m{5.0};
    CHECK(m.value == 5.0);
}

// ---------------------------------------------------------------------------
// (3) Same-dimension arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: same-dimension addition / subtraction",
          "[v0a-1][quantity][arith]")
{
    Quantity<dim::Length, f32> a{2.0F};
    Quantity<dim::Length, f32> b{3.0F};

    auto sum = a + b;
    CHECK(sum.value == 5.0F);

    auto diff = a - b;
    CHECK(diff.value == -1.0F);

    auto neg = -a;
    CHECK(neg.value == -2.0F);

    a += b;
    CHECK(a.value == 5.0F);

    a -= b;
    CHECK(a.value == 2.0F);
}

TEST_CASE("Quantity: scalar multiplication / division (same dimension)",
          "[v0a-1][quantity][arith]")
{
    Quantity<dim::Length, f32> a{2.0F};

    auto scaled = a * 3.0F;
    CHECK(scaled.value == 6.0F);

    auto divided = a / 4.0F;
    CHECK(divided.value == 0.5F);

    auto scaled_left = 5.0F * a;
    CHECK(scaled_left.value == 10.0F);

    a *= 4.0F;
    CHECK(a.value == 8.0F);

    a /= 2.0F;
    CHECK(a.value == 4.0F);
}

// ---------------------------------------------------------------------------
// (4) Cross-dimension multiplication / division
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: cross-dimension multiplication produces DimMul",
          "[v0a-1][quantity][arith]")
{
    Quantity<dim::Length, f64> len{6.0};
    Quantity<dim::Length, f64> wid{4.0};

    auto area = len * wid;
    STATIC_REQUIRE(dim_equal_v<typename decltype(area)::dimension, dim::Area>);
    CHECK(area.value == 24.0);
}

TEST_CASE("Quantity: cross-dimension division produces DimDiv",
          "[v0a-1][quantity][arith]")
{
    Quantity<dim::Length, f64> len{6.0};
    Quantity<dim::Time, f64> dt{2.0};

    auto vel = len / dt;
    STATIC_REQUIRE(dim_equal_v<typename decltype(vel)::dimension, dim::Velocity>);
    CHECK(vel.value == 3.0);

    auto back_to_len = vel * dt;
    STATIC_REQUIRE(dim_equal_v<typename decltype(back_to_len)::dimension, dim::Length>);
    CHECK(back_to_len.value == 6.0);
}

TEST_CASE("Quantity: scalar / Quantity produces DimInv",
          "[v0a-1][quantity][arith]")
{
    Quantity<dim::Time, f32> period{4.0F};
    auto freq = 1.0F / period;
    STATIC_REQUIRE(dim_equal_v<typename decltype(freq)::dimension, dim::Frequency>);
    CHECK(freq.value == 0.25F);
}

// ---------------------------------------------------------------------------
// (5) Physics formula correctness
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: Newton's second law: F = m * a",
          "[v0a-1][quantity][physics]")
{
    Quantity<dim::Mass, f32> m{5.0F};
    Quantity<dim::Acceleration, f32> a{4.0F};

    auto force = m * a;
    STATIC_REQUIRE(dim_equal_v<typename decltype(force)::dimension, dim::Force>);
    CHECK(force.value == 20.0F);
}

TEST_CASE("Quantity: integration step type-checks (semi-implicit Euler)",
          "[v0a-1][quantity][physics]")
{
    // pos = 0 m; v = 10 m/s; a = 4 m/s^2; dt = 0.5 s
    Quantity<dim::Length, f32> position{0.0F};
    Quantity<dim::Velocity, f32> velocity{10.0F};
    Quantity<dim::Acceleration, f32> accel{4.0F};
    Quantity<dim::Time, f32> dt{0.5F};

    // v += a * dt -> 10 += 4 * 0.5 = 10 += 2.0 = 12.0 (bit-exact in f32)
    velocity += accel * dt;
    CHECK(velocity.value == 12.0F);

    // pos += v * dt -> 0 += 12.0 * 0.5 = 6.0 (bit-exact)
    position += velocity * dt;
    CHECK(position.value == 6.0F);
}

TEST_CASE("Quantity: angular kinematics: omega = theta / time",
          "[v0a-1][quantity][physics]")
{
    Quantity<dim::Angle, f32> theta{6.28F};  // ~2? rad
    Quantity<dim::Time, f32> period{1.0F};

    auto omega = theta / period;
    STATIC_REQUIRE(dim_equal_v<typename decltype(omega)::dimension, dim::AngularVelocity>);
    CHECK(omega.value == 6.28F);
}

TEST_CASE("Quantity: kinetic energy: E = 0.5 * m * v^2",
          "[v0a-1][quantity][physics]")
{
    Quantity<dim::Mass, f32> m{2.0F};
    Quantity<dim::Velocity, f32> v{3.0F};

    auto v_sq = v * v;
    STATIC_REQUIRE(dim_equal_v<typename decltype(v_sq)::dimension, DimPow<dim::Velocity, 2>>);

    auto half_m_v_sq = (m * v_sq) * 0.5F;
    STATIC_REQUIRE(dim_equal_v<typename decltype(half_m_v_sq)::dimension, dim::Energy>);
    CHECK(half_m_v_sq.value == 9.0F);
}

// ---------------------------------------------------------------------------
// (6) Comparison
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: comparison operators",
          "[v0a-1][quantity][cmp]")
{
    Quantity<dim::Length, f32> a{2.0F};
    Quantity<dim::Length, f32> b{3.0F};
    Quantity<dim::Length, f32> c{2.0F};

    CHECK(a == c);
    CHECK(a != b);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a <= c);
    CHECK(a >= c);
    CHECK(a <= b);
    CHECK(b >= a);
}

// ---------------------------------------------------------------------------
// (7) Mixed-dimension expressions
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: Pressure = Force / Area",
          "[v0a-1][quantity][arith]")
{
    // F = Force per SI base notation.
    Quantity<dim::Force, f64> F{1000.0};      // 1000 N  // NOLINT(readability-identifier-naming)
    Quantity<dim::Length, f64> side{2.0};      // 2 m
    Quantity<dim::Area, f64> area = side * side;

    auto pressure = F / area;
    STATIC_REQUIRE(dim_equal_v<typename decltype(pressure)::dimension, dim::Pressure>);
    CHECK(pressure.value == 250.0);  // 1000 N / 4 m^2 = 250 Pa
}

TEST_CASE("Quantity: Density = Mass / Volume",
          "[v0a-1][quantity][arith]")
{
    Quantity<dim::Mass, f64> m{1000.0};  // 1 metric ton
    Quantity<dim::Length, f64> side{1.0};

    auto volume = side * side * side;
    STATIC_REQUIRE(dim_equal_v<typename decltype(volume)::dimension, dim::Volume>);

    auto density = m / volume;
    STATIC_REQUIRE(dim_equal_v<typename decltype(density)::dimension, dim::Density>);
    CHECK(density.value == 1000.0);  // 1000 kg/m^3 ? water
}

// ---------------------------------------------------------------------------
// (8) Constexpr evaluation
// ---------------------------------------------------------------------------

TEST_CASE("Quantity: arithmetic is constexpr-evaluable",
          "[v0a-1][quantity][constexpr]")
{
    constexpr Quantity<dim::Length, f32> kA{2.0F};
    constexpr Quantity<dim::Length, f32> kB{3.0F};
    constexpr auto kSum = kA + kB;
    STATIC_REQUIRE(kSum.value == 5.0F);

    constexpr Quantity<dim::Velocity, f64> kV{10.0};
    constexpr Quantity<dim::Time, f64> kT{2.0};
    constexpr auto kDistance = kV * kT;
    STATIC_REQUIRE(dim_equal_v<typename decltype(kDistance)::dimension, dim::Length>);
    STATIC_REQUIRE(kDistance.value == 20.0);
}
