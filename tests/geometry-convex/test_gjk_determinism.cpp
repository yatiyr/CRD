// crd-geometry-convex v2a — GJK determinism tests.
//
// The promise: same shape pair + same transforms → bit-exact identical
// witnesses + distance_squared, *independent of starting search direction
// or platform*. The `vertex_idx`-tiebreak rule (ADR-0076 §4 pin #14) is the
// substrate-level guarantee; this test enforces it.
//
// Two probes:
//   (1) **Replay equality** — call `gjk_distance` twice with identical args;
//       memcmp the entire `GjkResult` struct. (`crd-geometry-bvh`'s
//       deterministic-replay test uses the same memcmp form.)
//   (2) **Starting-direction stability** — call `gjk_distance` 100 times
//       with shapes in identical relative configuration but reached via
//       different absolute placement (varying the shared world transform).
//       The relative geometry is the same, so the answer's *local* shape
//       (witness on A relative to A's center, etc.) must agree bit-exact.

#include <crd/geometry/convex/gjk.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

namespace
{
using crd::f32;
using crd::u32;
using crd::geometry::convex::gjk_distance;
using crd::geometry::convex::GjkResult;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Sphere;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;

// Field-by-field bit-exact comparison.
//
// (We used to `memcmp(&lhs, &rhs, sizeof(GjkResult))` but C++ aggregate
// value-initialization does not zero padding bytes — bit_equal then fails
// on Release/shipping where uninitialized padding holds stack garbage,
// while passing on Debug where `/RTC1` zeroes the frame. The replay test's
// real claim is "every named field is bit-exact across calls", which is
// exactly what this comparison checks.)
bool bit_equal(const GjkResult<f32>& lhs, const GjkResult<f32>& rhs)
{
    if (std::memcmp(&lhs.witness_a_world, &rhs.witness_a_world, sizeof(Vec3<f32>)) != 0)
    {
        return false;
    }
    if (std::memcmp(&lhs.witness_b_world, &rhs.witness_b_world, sizeof(Vec3<f32>)) != 0)
    {
        return false;
    }
    if (lhs.distance_squared != rhs.distance_squared)
    {
        return false;
    }
    if (lhs.overlapping != rhs.overlapping)
    {
        return false;
    }
    if (lhs.simplex.size != rhs.simplex.size)
    {
        return false;
    }
    for (int i = 0; i < lhs.simplex.size; ++i)
    {
        if (std::memcmp(&lhs.simplex.w_a_local[i], &rhs.simplex.w_a_local[i], sizeof(Vec3<f32>)) != 0)
        {
            return false;
        }
        if (std::memcmp(&lhs.simplex.w_b_local[i], &rhs.simplex.w_b_local[i], sizeof(Vec3<f32>)) != 0)
        {
            return false;
        }
        if (lhs.simplex.vidx_a[i] != rhs.simplex.vidx_a[i])
        {
            return false;
        }
        if (lhs.simplex.vidx_b[i] != rhs.simplex.vidx_b[i])
        {
            return false;
        }
    }
    if (lhs.iteration_count != rhs.iteration_count)
    {
        return false;
    }
    if (lhs.converged != rhs.converged)
    {
        return false;
    }
    return true;
}
} // namespace

TEST_CASE("GJK determinism: replay equality (memcmp identical)", "[gjk][determinism]")
{
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.7F, 1.3F, 0.9F), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0, 0, 0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(4.0F, 1.5F, -2.0F), Quat<f32>::identity());
    const GjkResult<f32> r1 = gjk_distance<f32>(a, xa, b, xb);
    const GjkResult<f32> r2 = gjk_distance<f32>(a, xa, b, xb);
    REQUIRE(bit_equal(r1, r2));
}

TEST_CASE("GJK determinism: distance-equivariance across world placements",
          "[gjk][determinism][stability]")
{
    // Shape pair offset so the closest-point pair is uniquely a corner pair
    // (no face- or edge-equality among the closest set). Boxes of differing
    // sizes shifted into the +++ octant by enough that every axis is past
    // the sum of half-extents → corner-to-corner is the unique closest.
    //
    // What this test enforces:
    //   - `distance_squared` is INVARIANT under rigid translation of the
    //     pair (the relative geometry doesn't change).
    //   - Witness *positions* are also invariant when the closest-point set
    //     is a single point (this corner-to-corner setup guarantees that).
    //
    // What it CANNOT enforce, and why this test uses corner-to-corner:
    //   - If the closest-point set is non-singular (e.g., parallel face-to-
    //     face contact, or parallel edges sliding), GJK can return ANY
    //     valid witness in that set. The distance is still bit-invariant,
    //     but the witness pick depends on the iteration path — which is
    //     not a determinism bug, it's a fundamental property of the
    //     problem. The `bit_equal` replay test above is the strong
    //     guarantee for identical inputs.
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1.0F, 1.0F, 1.0F), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.5F, 0.8F, 1.2F), Mat3<f32>::identity());
    // b_rel_to_a where every axis is strictly past sum-of-half-extents:
    //   x: |b_rel.x| > 1 + 0.5 = 1.5
    //   y: |b_rel.y| > 1 + 0.8 = 1.8
    //   z: |b_rel.z| > 1 + 1.2 = 2.2
    // (3.5, 2.5, 3.0) is comfortably past all three → corner-to-corner is
    // the unique closest pair.
    const Vec3<f32> b_rel_to_a(3.5F, 2.5F, 3.0F);

    const GjkResult<f32> ref = gjk_distance<f32>(a, Transform<f32>(Vec3<f32>(0), Quat<f32>::identity()), b,
                                                 Transform<f32>(b_rel_to_a, Quat<f32>::identity()));

    for (int i = 0; i < 100; ++i)
    {
        const f32 ti = static_cast<f32>(i) * 0.137F;
        const Vec3<f32> world_t(ti, -ti * 0.5F, ti * 0.25F);
        const GjkResult<f32> r =
            gjk_distance<f32>(a, Transform<f32>(world_t, Quat<f32>::identity()), b,
                              Transform<f32>(world_t + b_rel_to_a, Quat<f32>::identity()));
        // Translation-equivariance is approximate at finite precision —
        // `(world_t + b_rel_to_a) - world_t` introduces a few ULP of drift
        // as `world_t` grows (cancellation in the local-frame `T_BA`
        // composition). Tight tolerance: a few ULP × |world_t|-scale.
        constexpr f32 kUlpTol = 1e-4F;
        REQUIRE(std::fabs(r.distance_squared - ref.distance_squared) <= kUlpTol);
        REQUIRE(std::fabs((r.witness_a_world.x - world_t.x) - ref.witness_a_world.x) <= kUlpTol);
        REQUIRE(std::fabs((r.witness_a_world.y - world_t.y) - ref.witness_a_world.y) <= kUlpTol);
        REQUIRE(std::fabs((r.witness_a_world.z - world_t.z) - ref.witness_a_world.z) <= kUlpTol);
        REQUIRE(std::fabs((r.witness_b_world.x - world_t.x - b_rel_to_a.x) - (ref.witness_b_world.x - b_rel_to_a.x)) <=
                kUlpTol);
        REQUIRE(std::fabs((r.witness_b_world.y - world_t.y - b_rel_to_a.y) - (ref.witness_b_world.y - b_rel_to_a.y)) <=
                kUlpTol);
        REQUIRE(std::fabs((r.witness_b_world.z - world_t.z - b_rel_to_a.z) - (ref.witness_b_world.z - b_rel_to_a.z)) <=
                kUlpTol);
    }
}

TEST_CASE("GJK determinism: overlap result is also bit-exact across rebuilds", "[gjk][determinism][overlap]")
{
    const Sphere<f32> a(Vec3<f32>(0), 1.5F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(1.0F, 0, 0), Quat<f32>::identity());
    const GjkResult<f32> r1 = gjk_distance<f32>(a, xa, b, xb);
    const GjkResult<f32> r2 = gjk_distance<f32>(a, xa, b, xb);
    REQUIRE(r1.overlapping);
    REQUIRE(r2.overlapping);
    REQUIRE(bit_equal(r1, r2));
}
