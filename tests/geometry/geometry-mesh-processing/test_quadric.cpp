// Tests for crd-geometry-mesh-processing v7b Quadric<T>.
//
// Covers:
//   - Zero / from_plane construction
//   - Plane-distance invariant (evaluate at a point on the plane == 0)
//   - Addition + accumulation
//   - Scalar multiplication
//   - optimal_position (regular case + singular fallback)
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

using crd::f32;
using crd::f64;
using crd::math::Vec3;
using crd::geometry::mesh_processing::Quadric;
using crd::geometry::mesh_processing::evaluate;
using crd::geometry::mesh_processing::optimal_position;

TEST_CASE("Quadric: zero quadric evaluates to zero everywhere",
          "[geometry-mesh-processing][quadric]")
{
    const auto q = Quadric<f32>::zero();
    CHECK(evaluate(q, Vec3<f32>{0, 0, 0}) == 0.0F);
    CHECK(evaluate(q, Vec3<f32>{1, 2, 3}) == 0.0F);
    CHECK(evaluate(q, Vec3<f32>{-7, 11, 13}) == 0.0F);
}

TEST_CASE("Quadric: from_plane evaluates to zero at points on the plane",
          "[geometry-mesh-processing][quadric]")
{
    // Plane: x + 2y + 3z - 14 = 0 (passes through (1, 2, 3) since 1+4+9=14).
    // Normalise to unit normal: |n| = sqrt(1+4+9) = sqrt(14).
    const f32 inv = 1.0F / std::sqrt(14.0F);
    const f32 a = 1.0F * inv;
    const f32 b = 2.0F * inv;
    const f32 c = 3.0F * inv;
    const f32 d = -14.0F * inv;
    const auto q = Quadric<f32>::from_plane(a, b, c, d);

    // Points on the plane → evaluate ≈ 0.
    CHECK(crd::math::abs(evaluate(q, Vec3<f32>{1, 2, 3})) < 1e-5F);
    CHECK(crd::math::abs(evaluate(q, Vec3<f32>{0, 1, 4})) < 1e-5F);   // 0+2+12=14 ✓
    CHECK(crd::math::abs(evaluate(q, Vec3<f32>{4, 2, 2})) < 1e-5F);   // 4+4+6=14 ✓

    // Point off the plane → evaluate = signed_distance². For (0,0,0):
    // signed_dist = (0+0+0-14) / sqrt(14) = -14/sqrt(14) = -sqrt(14).
    // distance² = 14.
    const f32 dist_sq = evaluate(q, Vec3<f32>{0, 0, 0});
    CHECK(crd::math::abs(dist_sq - 14.0F) < 1e-4F);
}

TEST_CASE("Quadric: addition + accumulation",
          "[geometry-mesh-processing][quadric]")
{
    const auto q1 = Quadric<f32>::from_plane(1, 0, 0, -3); // plane x = 3
    const auto q2 = Quadric<f32>::from_plane(0, 1, 0, -4); // plane y = 4
    const auto q3 = Quadric<f32>::from_plane(0, 0, 1, -5); // plane z = 5

    const auto sum = q1 + q2 + q3;

    // At (3, 4, 5): all three planes contain the point → cost 0.
    CHECK(crd::math::abs(evaluate(sum, Vec3<f32>{3, 4, 5})) < 1e-5F);

    // At (0, 0, 0): dist² to x=3 = 9, to y=4 = 16, to z=5 = 25 → sum = 50.
    CHECK(crd::math::abs(evaluate(sum, Vec3<f32>{0, 0, 0}) - 50.0F) < 1e-4F);

    // += matches +.
    auto acc = Quadric<f32>::zero();
    acc += q1;
    acc += q2;
    acc += q3;
    for (int i = 0; i < 10; ++i) { CHECK(acc.data[i] == sum.data[i]); }
}

TEST_CASE("Quadric: scalar multiplication scales evaluate proportionally",
          "[geometry-mesh-processing][quadric]")
{
    const auto q = Quadric<f32>::from_plane(1, 0, 0, -3);
    const auto q2 = q * 2.5F;
    const f32 e_base   = evaluate(q,  Vec3<f32>{0, 0, 0});
    const f32 e_scaled = evaluate(q2, Vec3<f32>{0, 0, 0});
    CHECK(crd::math::abs(e_scaled - 2.5F * e_base) < 1e-4F);
}

TEST_CASE("Quadric: optimal_position recovers a planar intersection",
          "[geometry-mesh-processing][quadric]")
{
    // Three orthogonal planes meeting at (3, 4, 5).
    auto q = Quadric<f32>::zero();
    q += Quadric<f32>::from_plane(1, 0, 0, -3);
    q += Quadric<f32>::from_plane(0, 1, 0, -4);
    q += Quadric<f32>::from_plane(0, 0, 1, -5);

    const auto v = optimal_position(q);
    REQUIRE(v.has_value());
    const auto vec = *v;
    CHECK(crd::math::abs(vec.x - 3.0F) < 1e-4F);
    CHECK(crd::math::abs(vec.y - 4.0F) < 1e-4F);
    CHECK(crd::math::abs(vec.z - 5.0F) < 1e-4F);
}

TEST_CASE("Quadric: optimal_position returns nullopt for singular system",
          "[geometry-mesh-processing][quadric]")
{
    // Three parallel planes (all normal +x): system is rank-1; det = 0.
    auto q = Quadric<f32>::zero();
    q += Quadric<f32>::from_plane(1, 0, 0, -1);
    q += Quadric<f32>::from_plane(1, 0, 0, -2);
    q += Quadric<f32>::from_plane(1, 0, 0, -3);

    const auto v = optimal_position(q);
    CHECK_FALSE(v.has_value());
}

TEST_CASE("Quadric: f64 precision tier evaluates plane-distance correctly",
          "[geometry-mesh-processing][quadric][f64]")
{
    const f64 inv = 1.0 / std::sqrt(14.0);
    const auto q = Quadric<f64>::from_plane(1.0 * inv, 2.0 * inv, 3.0 * inv, -14.0 * inv);
    // Point on plane → 0.
    CHECK(crd::math::abs(evaluate(q, Vec3<f64>{1, 2, 3})) < 1e-12);
    // Origin → 14.
    CHECK(crd::math::abs(evaluate(q, Vec3<f64>{0, 0, 0}) - 14.0) < 1e-10);
}
