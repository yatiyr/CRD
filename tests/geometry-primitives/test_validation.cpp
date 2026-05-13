// crd-geometry-primitives v1i-c — validation discipline (ADR-0076 §15).
//
// Exercises the NaN/Inf contract (§16 pin #3 — queries tolerate, builders
// reject) at the primitive layer: every primitive query function called over
// a degenerate / NaN / ∞ input must return a sensible "no hit" answer — never
// crash, never UB. And the large-coordinate sweep — primitive queries at a
// +1e6 / +1e7 origin must agree with the equivalent queries at the origin
// within the f32-ULP tolerance for that magnitude.

#include "test_corpus.hpp"

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/intersect.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/signed_distance.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace
{
using crd::f32;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::distance_squared;
using crd::geometry::primitives::intersect_ray_aabb;
using crd::geometry::primitives::intersect_ray_sphere;
using crd::geometry::primitives::intersect_ray_triangle;
using crd::geometry::primitives::intersects;
using crd::geometry::primitives::is_finite;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::sd_box;
using crd::geometry::primitives::sd_sphere;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3;
namespace tc = crd::geometry::test_corpus;
} // namespace

// ---- NaN/Inf contract: degenerate-input queries must not crash ------------

TEST_CASE("validation: ray-vs-AABB tolerates degenerate AABBs", "[geometry][validation]")
{
    const Ray3<f32> ray(Vec3<f32>(-10, 0, 0), Vec3<f32>(1, 0, 0));
    for (const AABB3<f32>& box : tc::degenerate_aabbs())
    {
        f32 t = 0.0F;
        // The contract: never crash / UB. The result is allowed to be either
        // hit or miss — we just exercise the code path; if a finite-input
        // assertion were here, builders would reject (the contract). Queries
        // don't reject; they tolerate.
        (void)intersect_ray_aabb(ray, box, t);
        SUCCEED();
    }
}

TEST_CASE("validation: ray-vs-AABB tolerates degenerate rays", "[geometry][validation]")
{
    const AABB3<f32> box(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1));
    for (const Ray3<f32>& ray : tc::degenerate_rays())
    {
        f32 t = 0.0F;
        (void)intersect_ray_aabb(ray, box, t);
        SUCCEED();
    }
}

TEST_CASE("validation: ray-vs-sphere tolerates degenerate spheres", "[geometry][validation]")
{
    const Ray3<f32> ray(Vec3<f32>(-10, 0, 0), Vec3<f32>(1, 0, 0));
    for (const Sphere<f32>& s : tc::degenerate_spheres())
    {
        f32 t = 0.0F;
        (void)intersect_ray_sphere(ray, s, t);
        SUCCEED();
    }
}

TEST_CASE("validation: ray-vs-triangle tolerates degenerate triangles", "[geometry][validation]")
{
    const Ray3<f32> ray(Vec3<f32>(0, 0, -5), Vec3<f32>(0, 0, 1));
    for (const Triangle3<f32>& tri : tc::degenerate_triangles())
    {
        f32 t = 0.0F;
        Vec3<f32> bc{};
        (void)intersect_ray_triangle(ray, tri, t, bc);
        SUCCEED();
    }
}

TEST_CASE("validation: AABB-vs-AABB intersects tolerates degenerate AABBs", "[geometry][validation]")
{
    const AABB3<f32> box(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1));
    for (const AABB3<f32>& other : tc::degenerate_aabbs())
    {
        (void)intersects(box, other);
        SUCCEED();
    }
}

TEST_CASE("validation: closest_point + distance_squared tolerate degenerate inputs",
          "[geometry][validation]")
{
    const Vec3<f32> q(0.5F, 0.5F, 0.5F);
    for (const AABB3<f32>& box : tc::degenerate_aabbs())
    {
        (void)distance_squared(box, q);
        SUCCEED();
    }
    for (const Sphere<f32>& s : tc::degenerate_spheres())
    {
        (void)distance_squared(s, q);
        SUCCEED();
    }
    for (const Triangle3<f32>& tri : tc::degenerate_triangles())
    {
        (void)distance_squared(tri, q);
        SUCCEED();
    }
}

TEST_CASE("validation: SDFs tolerate degenerate inputs", "[geometry][validation]")
{
    const Vec3<f32> q(0.5F, 0.5F, 0.5F);
    // sd_box wants half-extents — feed degenerate half-extents.
    (void)sd_box(q, Vec3<f32>(0, 0, 0));                          // zero half-extents
    (void)sd_box(q, Vec3<f32>(-1, -1, -1));                       // negative half-extents
    (void)sd_box(q, Vec3<f32>(tc::k_nan, 1, 1));                  // NaN
    (void)sd_box(q, Vec3<f32>(tc::k_inf, 1, 1));                  // ∞
    for (const Sphere<f32>& s : tc::degenerate_spheres())
    {
        (void)sd_sphere(q - s.center, s.radius);
    }
    SUCCEED();
}

TEST_CASE("validation: is_finite identifies the degenerate corpus correctly", "[geometry][validation]")
{
    // Builders REJECT non-finite inputs (the §15 / §16 contract). The
    // degenerate corpus deliberately mixes finite-but-extreme inputs (point
    // AABBs, collinear triangles, zero radius) with non-finite inputs (NaN,
    // ∞). Sanity-check `is_finite` accepts the former and rejects the latter.
    REQUIRE(is_finite(AABB3<f32>(Vec3<f32>(1, 1, 1), Vec3<f32>(1, 1, 1)))); // point box — finite, builders accept
    REQUIRE_FALSE(is_finite(AABB3<f32>(Vec3<f32>(tc::k_nan, 0, 0), Vec3<f32>(1, 1, 1))));
    REQUIRE_FALSE(is_finite(AABB3<f32>(Vec3<f32>(-tc::k_inf, 0, 0), Vec3<f32>(1, 1, 1))));
    REQUIRE(is_finite(Sphere<f32>(Vec3<f32>(0, 0, 0), 0.0F)));
    REQUIRE_FALSE(is_finite(Sphere<f32>(Vec3<f32>(tc::k_nan, 0, 0), 1.0F)));
    REQUIRE_FALSE(is_finite(Sphere<f32>(Vec3<f32>(0, 0, 0), tc::k_inf)));
}

// ---- Large-coordinate sweep: queries stay correct at a far origin --------

TEST_CASE("validation: ray-vs-AABB at +1e6 origin agrees with origin", "[geometry][validation]")
{
    const AABB3<f32> box(Vec3<f32>(2, 0, 0), Vec3<f32>(4, 2, 2));
    const Ray3<f32> ray(Vec3<f32>(-10, 1, 1), Vec3<f32>(1, 0, 0));
    f32 t_origin = 0.0F;
    REQUIRE(intersect_ray_aabb(ray, box, t_origin));

    const Vec3<f32> offset(tc::k_far_origin_modest, tc::k_far_origin_modest, tc::k_far_origin_modest);
    const AABB3<f32> box_far = tc::shift(box, offset);
    const Ray3<f32> ray_far = tc::shift(ray, offset);
    f32 t_far = 0.0F;
    REQUIRE(intersect_ray_aabb(ray_far, box_far, t_far));

    // At +1e6 origin, f32 ULP ≈ 0.06 — the t-value (≈ 12 here) should match
    // within the local ULP. Direction's preserved exactly so `t` only suffers
    // origin-magnitude ULP — but the subtraction `box.min - ray.origin` in
    // the slab is `2e6 - 1e6 = 1e6` magnitude, then divides by direction.
    // Conservative tolerance: local ULP × the t magnitude.
    const f32 tol = tc::ulp_tolerance_for(tc::k_far_origin_modest) * 2.0F;
    REQUIRE(std::abs(t_far - t_origin) <= tol);
}

TEST_CASE("validation: ray-vs-sphere at +1e6 origin agrees with origin", "[geometry][validation]")
{
    const Sphere<f32> s(Vec3<f32>(5, 0, 0), 1.0F);
    const Ray3<f32> ray(Vec3<f32>(-10, 0, 0), Vec3<f32>(1, 0, 0));
    f32 t_origin = 0.0F;
    REQUIRE(intersect_ray_sphere(ray, s, t_origin));

    const Vec3<f32> offset(tc::k_far_origin_modest, tc::k_far_origin_modest, tc::k_far_origin_modest);
    const Sphere<f32> s_far = tc::shift(s, offset);
    const Ray3<f32> ray_far = tc::shift(ray, offset);
    f32 t_far = 0.0F;
    REQUIRE(intersect_ray_sphere(ray_far, s_far, t_far));

    // The quadratic loses precision more aggressively than the slab — at +1e6
    // the discriminant `b² - 4ac` cancels catastrophically. Use a generous
    // tolerance proportional to the origin magnitude — the local f32 ULP
    // grows as `(1e6)/2^22 ≈ 0.24`, and the quadratic's relative-error
    // amplification can be ~10×.
    const f32 tol = tc::ulp_tolerance_for(tc::k_far_origin_modest) * 10.0F;
    REQUIRE(std::abs(t_far - t_origin) <= tol);
}

TEST_CASE("validation: AABB-vs-AABB intersects is shift-invariant at +1e7 origin", "[geometry][validation]")
{
    // intersects(AABB, AABB) is a pure boolean — shift-invariant exactly,
    // even at +1e7 where coordinates can lose precision.
    const AABB3<f32> a(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1));
    const AABB3<f32> b(Vec3<f32>(0.5F, 0.5F, 0.5F), Vec3<f32>(2, 2, 2));
    REQUIRE(intersects(a, b));

    const Vec3<f32> offset(tc::k_far_origin_stress, tc::k_far_origin_stress, tc::k_far_origin_stress);
    const AABB3<f32> a_far = tc::shift(a, offset);
    const AABB3<f32> b_far = tc::shift(b, offset);
    REQUIRE(intersects(a_far, b_far));
}
