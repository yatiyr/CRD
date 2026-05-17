// crd-units v0d-1 — Vec<Quantity> reduction widening tests.
//
// Per ADR-0078 §4 D26, the scoped reductions add typed returns:
//   length(Vec<Q>) -> Q
//   length_squared(Vec<Q>) -> Q²
//   dot(Vec<Q1>, Vec<Q2>) -> Q1*Q2
//   cross(Vec<Q>, Vec<Q>) -> Vec<Q²>
//   distance(Vec<Q>, Vec<Q>) -> Q
//   distance_squared(Vec<Q>, Vec<Q>) -> Q²
//   hadamard(Vec<Q1>, Vec<Q2>) -> Vec<Q1*Q2>
//   normalized(Vec<Q>) -> Vec<T> (unit vector, dimensionless)
//
// Determinism contract (ADR-0063): all results bit-identical to the raw
// MathScalar path with `.value` accessed before reduction.

#include <crd/math/vec.hpp>
#include <crd/units/quantity_aliases.hpp>
#include <crd/units/units.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <type_traits>

using crd::math::cross;
using crd::math::distance;
using crd::math::dot;
using crd::math::hadamard;
using crd::math::length;
using crd::math::length_squared;
using crd::math::normalized;
using crd::math::Vec3;
using crd::math::Vec3f;
using crd::units::Area32;
using crd::units::Length32;
using crd::units::Velocity32;

TEST_CASE("v0d-1 length(Vec3<Length32>) returns Length32", "[units][vec][reductions][length]")
{
    const Vec3<Length32> v{Length32{3.0F}, Length32{4.0F}, Length32{0.0F}};
    const Length32 len_q = length(v);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(len_q)>, Length32>);
    REQUIRE(len_q.value == Catch::Approx(5.0F));
}

TEST_CASE("v0d-1 length_squared(Vec3<Length32>) returns Area32", "[units][vec][reductions][length_squared]")
{
    const Vec3<Length32> v{Length32{1.0F}, Length32{2.0F}, Length32{2.0F}};
    const auto ls = length_squared(v);
    // Area32 = Quantity<dim::Area, f32> = Quantity<DimMul<dim::Length, dim::Length>, f32>
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(ls)>, Area32>);
    REQUIRE(ls.value == Catch::Approx(9.0F)); // 1 + 4 + 4
}

TEST_CASE("v0d-1 dot(Vec3<Length32>, Vec3<Length32>) returns Area32", "[units][vec][reductions][dot]")
{
    const Vec3<Length32> a{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    const Vec3<Length32> b{Length32{4.0F}, Length32{5.0F}, Length32{6.0F}};
    const auto d = dot(a, b);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(d)>, Area32>);
    REQUIRE(d.value == Catch::Approx(32.0F)); // 4 + 10 + 18
}

TEST_CASE("v0d-1 cross(Vec3<Length32>, Vec3<Length32>) returns Vec3<Area32>",
          "[units][vec][reductions][cross]")
{
    const Vec3<Length32> e1{Length32{1.0F}, Length32{0.0F}, Length32{0.0F}};
    const Vec3<Length32> e2{Length32{0.0F}, Length32{1.0F}, Length32{0.0F}};
    const Vec3<Area32> c = cross(e1, e2);
    // e1 × e2 = e3
    REQUIRE(c.x.value == Catch::Approx(0.0F));
    REQUIRE(c.y.value == Catch::Approx(0.0F));
    REQUIRE(c.z.value == Catch::Approx(1.0F));
}

TEST_CASE("v0d-1 distance(Vec3<Length32>, Vec3<Length32>) returns Length32",
          "[units][vec][reductions][distance]")
{
    const Vec3<Length32> p1{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    const Vec3<Length32> p2{Length32{4.0F}, Length32{6.0F}, Length32{3.0F}};
    const Length32 d = distance(p1, p2);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(d)>, Length32>);
    REQUIRE(d.value == Catch::Approx(5.0F)); // sqrt(9 + 16 + 0)
}

TEST_CASE("v0d-1 hadamard(Vec3<Velocity32>, Vec3<Length32>) returns Vec3<Velocity * Length>",
          "[units][vec][reductions][hadamard]")
{
    const Vec3<Velocity32> v{Velocity32{2.0F}, Velocity32{3.0F}, Velocity32{4.0F}};
    const Vec3<Length32>   l{Length32{5.0F}, Length32{6.0F}, Length32{7.0F}};
    const auto h = hadamard(v, l);
    // Per-axis: m/s · m = m²/s
    REQUIRE(h.x.value == Catch::Approx(10.0F));
    REQUIRE(h.y.value == Catch::Approx(18.0F));
    REQUIRE(h.z.value == Catch::Approx(28.0F));
}

TEST_CASE("v0d-1 normalized(Vec3<Length32>) returns Vec3<f32> unit vector",
          "[units][vec][reductions][normalized]")
{
    const Vec3<Length32> v{Length32{3.0F}, Length32{4.0F}, Length32{0.0F}};
    const Vec3f unit = normalized(v);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(unit)>, Vec3f>);
    REQUIRE(unit.x == Catch::Approx(0.6F));
    REQUIRE(unit.y == Catch::Approx(0.8F));
    REQUIRE(unit.z == Catch::Approx(0.0F));
    // |unit| == 1
    REQUIRE(std::sqrt(unit.x * unit.x + unit.y * unit.y + unit.z * unit.z) == Catch::Approx(1.0F));
}

TEST_CASE("v0d-1 reductions are bit-identical to raw MathScalar path",
          "[units][vec][reductions][determinism]")
{
    // Determinism contract: typed reductions produce bit-exact same FP
    // result as their raw counterparts under identical input bits.
    const Vec3f          raw{1.5F, -2.25F, 3.125F};
    const Vec3<Length32> typed{Length32{1.5F}, Length32{-2.25F}, Length32{3.125F}};

    const float          raw_len_sq = crd::math::length_squared(raw);
    const float          typed_len_sq = length_squared(typed).value;
    CHECK(std::bit_cast<crd::u32>(raw_len_sq) == std::bit_cast<crd::u32>(typed_len_sq));

    const float          raw_len = crd::math::length(raw);
    const float          typed_len = length(typed).value;
    CHECK(std::bit_cast<crd::u32>(raw_len) == std::bit_cast<crd::u32>(typed_len));
}

TEST_CASE("v0d-1 cross-Dim dot: Vec3<Length> . Vec3<Velocity> -> Q<L*V> = Area/Time",
          "[units][vec][reductions][cross-dim]")
{
    // Cross-Dim dot: dot(Vec<Length>, Vec<Velocity>) -> Quantity<Length*Velocity, T>.
    // Length * Velocity = m * m/s = m^2/s — physically: linear momentum flux per unit
    // density. Test the type composition, not a physical meaning.
    const Vec3<Length32>   l{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    const Vec3<Velocity32> v{Velocity32{4.0F}, Velocity32{5.0F}, Velocity32{6.0F}};
    const auto d = dot(l, v);
    REQUIRE(d.value == Catch::Approx(32.0F));
    // Type is Quantity<DimMul<Length, Velocity>, f32>
    using ResultT = std::remove_cv_t<decltype(d)>;
    STATIC_REQUIRE(crd::units::is_quantity_v<ResultT>);
}
