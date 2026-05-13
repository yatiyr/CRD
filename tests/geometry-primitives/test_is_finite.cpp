// crd-geometry-primitives v1h -- the finiteness predicates + the NaN/Inf
// contract. is_finite(x) is true iff every component is finite; a NaN or +-Inf
// in any component flips it. The empty-AABB sentinel {+inf,-inf} is *deliberately*
// non-finite (it is the union identity, not data) -- documented here in code.

#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace crd;
using namespace crd::geometry::primitives;
using crd::math::Vec2;
using crd::math::Vec3;

namespace
{
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
const f32 kNan = std::numeric_limits<f32>::quiet_NaN();
} // namespace

TEST_CASE("is_finite: vectors", "[geometry][is_finite]")
{
    CHECK(is_finite(Vec3<f32>(1.0F, 2.0F, 3.0F)));
    CHECK(is_finite(Vec2<f32>(0.0F, -1.0F)));
    CHECK_FALSE(is_finite(Vec3<f32>(1.0F, kNan, 3.0F)));
    CHECK_FALSE(is_finite(Vec3<f32>(kInf, 2.0F, 3.0F)));
    CHECK_FALSE(is_finite(Vec3<f32>(1.0F, 2.0F, -kInf)));
    CHECK_FALSE(is_finite(Vec2<f32>(kNan, 0.0F)));
}

TEST_CASE("is_finite: 3D primitives -- finite accepted, one bad component rejected", "[geometry][is_finite]")
{
    CHECK(is_finite(Sphere<f32>(Vec3<f32>(1, 2, 3), 4.0F)));
    CHECK_FALSE(is_finite(Sphere<f32>(Vec3<f32>(1, 2, 3), kNan)));
    CHECK_FALSE(is_finite(Sphere<f32>(Vec3<f32>(1, kInf, 3), 4.0F)));

    CHECK(is_finite(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1))));
    CHECK_FALSE(is_finite(AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, kInf, 1))));

    CHECK(is_finite(Ray3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0))));
    CHECK_FALSE(is_finite(Ray3<f32>(Vec3<f32>(0, kNan, 0), Vec3<f32>(1, 0, 0))));

    CHECK(is_finite(Plane<f32>(Vec3<f32>(0, 1, 0), -2.0F)));
    CHECK_FALSE(is_finite(Plane<f32>(Vec3<f32>(0, 1, 0), kInf)));

    CHECK(is_finite(Capsule3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(0, 1, 0), 0.5F)));
    CHECK_FALSE(is_finite(Capsule3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(0, kNan, 0), 0.5F)));

    CHECK(is_finite(Triangle3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(0, 1, 0))));
    CHECK_FALSE(is_finite(Triangle3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(0, kInf, 0))));

    CHECK(is_finite(Tetrahedron<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(0, 1, 0), Vec3<f32>(0, 0, 1))));
    CHECK_FALSE(is_finite(OBB3<f32>(Vec3<f32>(kNan, 0, 0), Vec3<f32>(1, 1, 1), crd::math::Mat3<f32>::identity())));

    Frustum<f32> fr;
    CHECK(is_finite(fr)); // default-constructed planes are all-zero -> finite
    fr.planes[2] = Plane<f32>(Vec3<f32>(0, 0, kInf), 0.0F);
    CHECK_FALSE(is_finite(fr));
}

TEST_CASE("is_finite: 2D primitives", "[geometry][is_finite]")
{
    CHECK(is_finite(Circle<f32>(Vec2<f32>(0, 0), 1.0F)));
    CHECK_FALSE(is_finite(Circle<f32>(Vec2<f32>(0, 0), kNan)));
    CHECK(is_finite(AABB2<f32>(Vec2<f32>(-1, -1), Vec2<f32>(1, 1))));
    CHECK_FALSE(is_finite(AABB2<f32>(Vec2<f32>(-1, kInf), Vec2<f32>(1, 1))));
    CHECK(is_finite(Triangle2<f32>(Vec2<f32>(0, 0), Vec2<f32>(1, 0), Vec2<f32>(0, 1))));
    CHECK_FALSE(is_finite(Segment2<f32>(Vec2<f32>(0, 0), Vec2<f32>(kNan, 1))));
}

TEST_CASE("is_finite: the empty-AABB union sentinel is non-finite BY DESIGN", "[geometry][is_finite]")
{
    // builders use {min=+inf, max=-inf} as the identity for AABB union -- this
    // is NOT subject to the contract. Asserting its non-finiteness here pins
    // that distinction: finiteness asserts go on *inputs*, never on accumulators.
    const AABB3<f32> empty_sentinel(Vec3<f32>(kInf, kInf, kInf), Vec3<f32>(-kInf, -kInf, -kInf));
    CHECK_FALSE(is_finite(empty_sentinel));
}

TEST_CASE("all_finite: span helper -- the builder-reject predicate", "[geometry][is_finite]")
{
    crd::containers::StaticArray<AABB3<f32>, 3> good{
        AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1)),
        AABB3<f32>(Vec3<f32>(2, 2, 2), Vec3<f32>(3, 3, 3)),
        AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(0, 0, 0)),
    };
    CHECK(all_finite(crd::containers::ConstSpan<AABB3<f32>>(good.data(), good.size())));
    good[1] = AABB3<f32>(Vec3<f32>(2, kNan, 2), Vec3<f32>(3, 3, 3));
    CHECK_FALSE(all_finite(crd::containers::ConstSpan<AABB3<f32>>(good.data(), good.size())));
}
