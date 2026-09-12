// crd-geometry-primitives v0d-2 — typed Quantity-overload query tests.
//
// Verifies ADR-0078 §4 D27: every typed `Vec3<Length32>` / `Sphere<Length32>` /
// etc. closest_point / distance / distance_squared call returns a typed result,
// while the underlying algorithm stays raw `<MathScalar T>`.

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/queries_typed.hpp>
#include <crd/units/quantity_aliases.hpp>
#include <crd/units/units.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::closest_point;
using crd::geometry::primitives::distance;
using crd::geometry::primitives::distance_squared;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3;
using crd::units::Area32;
using crd::units::Length32;

TEST_CASE("v0d-2 closest_point(Sphere<Length32>, Vec3<Length32>) returns Vec3<Length32>",
          "[geometry][v0d-2][typed-queries][sphere]")
{
    const Sphere<Length32> s{
        Vec3<Length32>{Length32{0.0F}, Length32{0.0F}, Length32{0.0F}},
        Length32{1.0F}};
    const Vec3<Length32> p{Length32{2.0F}, Length32{0.0F}, Length32{0.0F}};
    const Vec3<Length32> cp = closest_point(s, p);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(cp)>, Vec3<Length32>>);
    // Closest point on a unit sphere from (2,0,0) is (1,0,0).
    REQUIRE(cp.x.value == Catch::Approx(1.0F));
    REQUIRE(cp.y.value == Catch::Approx(0.0F));
    REQUIRE(cp.z.value == Catch::Approx(0.0F));
}

TEST_CASE("v0d-2 distance(Sphere<Length32>, Vec3<Length32>) returns Length32",
          "[geometry][v0d-2][typed-queries][sphere]")
{
    const Sphere<Length32> s{
        Vec3<Length32>{Length32{0.0F}, Length32{0.0F}, Length32{0.0F}},
        Length32{1.0F}};
    const Vec3<Length32> p{Length32{4.0F}, Length32{0.0F}, Length32{0.0F}};
    const Length32 d = distance(s, p);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(d)>, Length32>);
    // Surface dist from (4,0,0) to unit sphere = 3 m.
    REQUIRE(d.value == Catch::Approx(3.0F));
}

TEST_CASE("v0d-2 distance_squared(Sphere<Length32>, Vec3<Length32>) returns Area32",
          "[geometry][v0d-2][typed-queries][sphere]")
{
    const Sphere<Length32> s{
        Vec3<Length32>{Length32{0.0F}, Length32{0.0F}, Length32{0.0F}},
        Length32{1.0F}};
    const Vec3<Length32> p{Length32{4.0F}, Length32{0.0F}, Length32{0.0F}};
    const auto d_sq = distance_squared(s, p);
    STATIC_REQUIRE(std::is_same_v<std::remove_cv_t<decltype(d_sq)>, Area32>);
    REQUIRE(d_sq.value == Catch::Approx(9.0F)); // (3 m)^2 = 9 m^2
}

TEST_CASE("v0d-2 closest_point(AABB3<Length32>, Vec3<Length32>) returns Vec3<Length32>",
          "[geometry][v0d-2][typed-queries][aabb]")
{
    const AABB3<Length32> box{
        Vec3<Length32>{Length32{-1.0F}, Length32{-1.0F}, Length32{-1.0F}},
        Vec3<Length32>{Length32{ 1.0F}, Length32{ 1.0F}, Length32{ 1.0F}}};
    const Vec3<Length32> p{Length32{5.0F}, Length32{0.0F}, Length32{0.0F}};
    const Vec3<Length32> cp = closest_point(box, p);
    // Closest point on [-1, 1]^3 from (5, 0, 0) is (1, 0, 0).
    REQUIRE(cp.x.value == Catch::Approx(1.0F));
    REQUIRE(cp.y.value == Catch::Approx(0.0F));
    REQUIRE(cp.z.value == Catch::Approx(0.0F));
}

TEST_CASE("v0d-2 closest_point(Capsule3<Length32>, Vec3<Length32>) returns Vec3<Length32>",
          "[geometry][v0d-2][typed-queries][capsule]")
{
    // Vertical capsule from (0,0,0) to (0,2,0), radius 0.5.
    const Capsule3<Length32> cap{
        Vec3<Length32>{Length32{0.0F}, Length32{0.0F}, Length32{0.0F}},
        Vec3<Length32>{Length32{0.0F}, Length32{2.0F}, Length32{0.0F}},
        Length32{0.5F}};
    const Vec3<Length32> p{Length32{3.0F}, Length32{1.0F}, Length32{0.0F}};
    const Vec3<Length32> cp = closest_point(cap, p);
    // Closest point on the capsule surface from (3, 1, 0): segment closest =
    // (0, 1, 0), surface = (0.5, 1, 0).
    REQUIRE(cp.x.value == Catch::Approx(0.5F));
    REQUIRE(cp.y.value == Catch::Approx(1.0F));
    REQUIRE(cp.z.value == Catch::Approx(0.0F));
}

TEST_CASE("v0d-2 closest_point(Triangle3<Length32>, Vec3<Length32>) returns Vec3<Length32>",
          "[geometry][v0d-2][typed-queries][triangle]")
{
    const Triangle3<Length32> tri{
        Vec3<Length32>{Length32{0.0F}, Length32{0.0F}, Length32{0.0F}},
        Vec3<Length32>{Length32{1.0F}, Length32{0.0F}, Length32{0.0F}},
        Vec3<Length32>{Length32{0.0F}, Length32{1.0F}, Length32{0.0F}}};
    // Query point above triangle's centroid: (1/3, 1/3, 1).
    const Vec3<Length32> p{Length32{1.0F / 3.0F}, Length32{1.0F / 3.0F}, Length32{1.0F}};
    const Vec3<Length32> cp = closest_point(tri, p);
    REQUIRE(cp.x.value == Catch::Approx(1.0F / 3.0F));
    REQUIRE(cp.y.value == Catch::Approx(1.0F / 3.0F));
    REQUIRE(cp.z.value == Catch::Approx(0.0F));
}

TEST_CASE("v0d-2 closest_point(Plane<Length32>, Vec3<Length32>) returns Vec3<Length32>",
          "[geometry][v0d-2][typed-queries][plane]")
{
    // Plane: y = 0 (normal up, d = 0). For T=Length32 the normal field is
    // Vec3<Length32> by template uniformity; the underlying *meaning* is a
    // dimensionless unit direction. Tag the raw unit vector to satisfy the
    // type system; the algorithm uses the components by direction only.
    const Plane<Length32> plane{
        Vec3<Length32>{Length32{0.0F}, Length32{1.0F}, Length32{0.0F}},
        Length32{0.0F}};
    const Vec3<Length32> p{Length32{3.0F}, Length32{5.0F}, Length32{2.0F}};
    const Vec3<Length32> cp = closest_point(plane, p);
    // Projection drops y to 0.
    REQUIRE(cp.x.value == Catch::Approx(3.0F));
    REQUIRE(cp.y.value == Catch::Approx(0.0F));
    REQUIRE(cp.z.value == Catch::Approx(2.0F));
}

TEST_CASE("v0d-2 typed queries are bit-identical to raw path",
          "[geometry][v0d-2][typed-queries][determinism]")
{
    // ADR-0063 determinism contract: typed wrapper must produce bit-exact
    // same f32 result as the raw algorithm.
    const Sphere<Length32> typed_s{
        Vec3<Length32>{Length32{1.5F}, Length32{-2.25F}, Length32{0.0F}},
        Length32{0.75F}};
    const Vec3<Length32> typed_p{Length32{3.0F}, Length32{0.5F}, Length32{1.0F}};

    const Sphere<crd::f32> raw_s{
        crd::math::Vec3f{1.5F, -2.25F, 0.0F},
        0.75F};
    const crd::math::Vec3f raw_p{3.0F, 0.5F, 1.0F};

    const Vec3<Length32>   typed_cp = closest_point(typed_s, typed_p);
    const crd::math::Vec3f raw_cp   = closest_point(raw_s, raw_p);

    REQUIRE(typed_cp.x.value == raw_cp.x);
    REQUIRE(typed_cp.y.value == raw_cp.y);
    REQUIRE(typed_cp.z.value == raw_cp.z);
}
