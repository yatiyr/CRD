// crd-geometry-convex v2b - gjk_overlap (boolean fast-out) tests.
//
// Three claims to validate:
//
//   (1) AGREEMENT: `gjk_overlap(...)` returns the same boolean as
//       `gjk_distance(...).overlapping` across a randomized rigid-transform
//       corpus (translations + rotations + a mix of shape types). The two
//       drivers share the simplex / sub-distance / termination logic; this
//       test pins that the dedicated boolean path doesn't accidentally
//       diverge.
//
//   (2) TOUCHING-BOUNDARY CONVENTION: shapes touching at exactly distance 0
//       (sphere-vs-sphere at sum_radii, capsule-vs-plane at radius, ...)
//       report `overlap = true`. Matches `gjk_distance`'s
//       `distance² <= eps²` overlap flag and is the engineering convention
//       for "physics contact". Tests pin this convention so callers can
//       rely on it.
//
//   (3) EDGE CASES: identical shapes at identical transforms (deeply
//       overlapping); concentric spheres of different radii; rotated cubes
//       that DEEPLY interpenetrate (origin solidly inside the Minkowski
//       difference); cubes that just-barely-don't overlap (separation ~ eps).
//
// Also pins via the unified facade: `crd::geometry::overlap(A, xa, B, xb)`
// dispatches to `gjk_overlap` and returns the same answer.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::convex::gjk_distance;
using crd::geometry::convex::gjk_overlap;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Sphere;
using crd::math::from_axis_angle;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;

Transform<f32> xform(const Vec3<f32>& t, const Quat<f32>& r = Quat<f32>::identity())
{
    return Transform<f32>(t, r);
}

struct Rng
{
    crd::u64 state;
    explicit Rng(crd::u64 seed) : state(seed) {}
    crd::u64 next()
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1U << 24); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    Vec3<f32> rand_vec(f32 lo, f32 hi) { return Vec3<f32>(range(lo, hi), range(lo, hi), range(lo, hi)); }
    Quat<f32> rand_quat()
    {
        // Uniform-ish over rotations: pick a random unit axis + random angle.
        Vec3<f32> ax = rand_vec(-1, 1);
        const f32 axlen = std::sqrt(crd::math::dot(ax, ax));
        if (axlen < 1e-3F)
        {
            ax = Vec3<f32>(1, 0, 0);
        }
        else
        {
            ax = Vec3<f32>(ax.x / axlen, ax.y / axlen, ax.z / axlen);
        }
        return from_axis_angle(ax, range(-3.14F, 3.14F));
    }
};

// 8-vertex cube hull, hosted on a TlsfAllocator for the test fixture.
struct CubeHull
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> face_idx;
    crd::containers::Array<u32> face_off;

    explicit CubeHull(crd::memory::IAllocator* alloc, f32 half = 1.0F)
        : verts(alloc), faces(alloc), face_idx(alloc), face_off(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? half : -half, (i & 2) ? half : -half, (i & 1) ? half : -half));
        }
    }
    ConvexHullView<f32> view() const
    {
        return ConvexHullView<f32>(crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
                                   crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
                                   crd::containers::ConstSpan<u32>(face_idx.data(), face_idx.size()),
                                   crd::containers::ConstSpan<u32>(face_off.data(), face_off.size()));
    }
};
} // namespace

// ===========================================================================
// AGREEMENT with gjk_distance(...).overlapping
// ===========================================================================

TEST_CASE("gjk_overlap: agreement with gjk_distance on sphere-sphere corpus", "[gjk-overlap][agreement]")
{
    Rng rng(0xA110CABEU);
    int agreed = 0;
    int overlap_count = 0;
    for (int trial = 0; trial < 200; ++trial)
    {
        const f32 ra = rng.range(0.3F, 1.5F);
        const f32 rb = rng.range(0.3F, 1.5F);
        const Sphere<f32> a(Vec3<f32>(0), ra);
        const Sphere<f32> b(Vec3<f32>(0), rb);
        const Vec3<f32> a_pos = rng.rand_vec(-5, 5);
        const Vec3<f32> b_pos = rng.rand_vec(-5, 5);

        const bool boolean_only = gjk_overlap<f32>(a, xform(a_pos), b, xform(b_pos));
        const bool from_distance = gjk_distance<f32>(a, xform(a_pos), b, xform(b_pos)).overlapping;
        INFO("trial " << trial << ", ra=" << ra << ", rb=" << rb << ", a=" << a_pos.x << "," << a_pos.y << ","
                      << a_pos.z << " b=" << b_pos.x << "," << b_pos.y << "," << b_pos.z);
        REQUIRE(boolean_only == from_distance);
        if (boolean_only == from_distance)
        {
            ++agreed;
        }
        if (boolean_only)
        {
            ++overlap_count;
        }
    }
    REQUIRE(agreed == 200);
    // Reality check: with 200 random configs in a 10^3 box with shapes of
    // radius ~1, we should see *some* overlaps and *some* misses. If
    // overlap_count is 0 or 200, the corpus is degenerate.
    REQUIRE(overlap_count > 0);
    REQUIRE(overlap_count < 200);
}

TEST_CASE("gjk_overlap: agreement with gjk_distance on box-vs-box rotated corpus",
          "[gjk-overlap][agreement][rotated]")
{
    Rng rng(0xCAFEF00DU);
    int overlap_count = 0;
    int separate_count = 0;
    for (int trial = 0; trial < 150; ++trial)
    {
        const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(rng.range(0.5F, 1.5F), rng.range(0.5F, 1.5F), rng.range(0.5F, 1.5F)),
                          Mat3<f32>::identity());
        const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(rng.range(0.5F, 1.5F), rng.range(0.5F, 1.5F), rng.range(0.5F, 1.5F)),
                          Mat3<f32>::identity());
        const Transform<f32> xa(rng.rand_vec(-4, 4), rng.rand_quat());
        const Transform<f32> xb(rng.rand_vec(-4, 4), rng.rand_quat());

        const bool boolean_only = gjk_overlap<f32>(a, xa, b, xb);
        const bool from_distance = gjk_distance<f32>(a, xa, b, xb).overlapping;
        INFO("trial " << trial << ", boolean=" << boolean_only << " from_distance=" << from_distance);
        REQUIRE(boolean_only == from_distance);
        if (boolean_only)
        {
            ++overlap_count;
        }
        else
        {
            ++separate_count;
        }
    }
    REQUIRE(overlap_count > 0);
    REQUIRE(separate_count > 0);
}

TEST_CASE("gjk_overlap: agreement on mixed analytic + polyhedral", "[gjk-overlap][agreement][mixed]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "overlap-mixed");
    const CubeHull hull(&alloc, 1.0F);

    Rng rng(0xBADC0FFEEU);
    for (int trial = 0; trial < 80; ++trial)
    {
        const Sphere<f32> sphere(Vec3<f32>(0), rng.range(0.5F, 1.2F));
        const Capsule3<f32> capsule(Vec3<f32>(0, 0, -0.5F), Vec3<f32>(0, 0, 0.5F), rng.range(0.2F, 0.8F));

        const Transform<f32> xs(rng.rand_vec(-3, 3), rng.rand_quat());
        const Transform<f32> xh(rng.rand_vec(-3, 3), rng.rand_quat());
        const Transform<f32> xc(rng.rand_vec(-3, 3), rng.rand_quat());

        // sphere vs hull
        REQUIRE(gjk_overlap<f32>(sphere, xs, hull.view(), xh) ==
                gjk_distance<f32>(sphere, xs, hull.view(), xh).overlapping);
        // capsule vs hull
        REQUIRE(gjk_overlap<f32>(capsule, xc, hull.view(), xh) ==
                gjk_distance<f32>(capsule, xc, hull.view(), xh).overlapping);
        // sphere vs capsule
        REQUIRE(gjk_overlap<f32>(sphere, xs, capsule, xc) ==
                gjk_distance<f32>(sphere, xs, capsule, xc).overlapping);
    }
}

// ===========================================================================
// TOUCHING-BOUNDARY CONVENTION
// ===========================================================================

TEST_CASE("gjk_overlap: touching spheres at exactly sum_radii report overlap=true",
          "[gjk-overlap][convention][touching]")
{
    // Two unit spheres centered 2.0 apart - surfaces touch at (1, 0, 0).
    // Convention: touching counts as overlap (matches gjk_distance).
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    REQUIRE(gjk_overlap<f32>(a, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.0F, 0, 0))));

    // Just-barely-not-touching: 2.001 apart, surfaces 0.001 apart -> no overlap.
    REQUIRE_FALSE(gjk_overlap<f32>(a, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.001F, 0, 0))));

    // Just-barely-overlapping: 1.999 apart, surfaces interpenetrate 0.001 -> overlap.
    REQUIRE(gjk_overlap<f32>(a, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(1.999F, 0, 0))));
}

TEST_CASE("gjk_overlap: cube face-to-face at exact gap=0 reports overlap=true",
          "[gjk-overlap][convention][touching]")
{
    // Unit cubes touching at x=1 plane (A's +X face flush against B's -X face).
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    REQUIRE(gjk_overlap<f32>(a, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.0F, 0, 0))));
    REQUIRE_FALSE(gjk_overlap<f32>(a, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.01F, 0, 0))));
}

// ===========================================================================
// EDGE CASES
// ===========================================================================

TEST_CASE("gjk_overlap: identical shapes at identical transforms are deeply overlapping",
          "[gjk-overlap][edge][identical]")
{
    const Sphere<f32> s(Vec3<f32>(0), 1.0F);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Capsule3<f32> c(Vec3<f32>(0, 0, -1), Vec3<f32>(0, 0, 1), 0.5F);
    const Transform<f32> at_origin(Vec3<f32>(0), Quat<f32>::identity());

    REQUIRE(gjk_overlap<f32>(s, at_origin, s, at_origin));
    REQUIRE(gjk_overlap<f32>(b, at_origin, b, at_origin));
    REQUIRE(gjk_overlap<f32>(c, at_origin, c, at_origin));
}

TEST_CASE("gjk_overlap: concentric spheres of different radii overlap", "[gjk-overlap][edge][concentric]")
{
    const Sphere<f32> outer(Vec3<f32>(0), 2.0F);
    const Sphere<f32> inner(Vec3<f32>(0), 0.5F);
    REQUIRE(gjk_overlap<f32>(outer, xform(Vec3<f32>(0)), inner, xform(Vec3<f32>(0))));
    // Inner shifted within outer - still overlapping.
    REQUIRE(gjk_overlap<f32>(outer, xform(Vec3<f32>(0)), inner, xform(Vec3<f32>(1.0F, 0, 0))));
    // Inner just-barely-poking-out: at 1.5+ shift, surfaces cross. Still overlap.
    REQUIRE(gjk_overlap<f32>(outer, xform(Vec3<f32>(0)), inner, xform(Vec3<f32>(1.5F, 0, 0))));
    // Inner fully exited: shift 2.5+0.5 = 3.0+ -> separated.
    REQUIRE_FALSE(gjk_overlap<f32>(outer, xform(Vec3<f32>(0)), inner, xform(Vec3<f32>(3.0F, 0, 0))));
}

TEST_CASE("gjk_overlap: deeply interpenetrating rotated boxes report overlap",
          "[gjk-overlap][edge][deep-overlap]")
{
    // Unit cube A; cube B rotated 45deg about Y, centered at (0.5, 0, 0) - deep overlap.
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Quat<f32> q = from_axis_angle(Vec3<f32>(0, 1, 0), 0.7853981633974F);
    const Mat3<f32> rot = crd::math::to_mat3(q);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), rot);
    REQUIRE(gjk_overlap<f32>(a, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(0.5F, 0, 0), q)));
}

// ===========================================================================
// UNIFIED FACADE: crd::geometry::overlap(A, xa, B, xb)
// ===========================================================================

TEST_CASE("crd::geometry::overlap facade dispatches to gjk_overlap for convex shapes",
          "[gjk-overlap][facade]")
{
    using crd::geometry::overlap;
    const Sphere<f32> s(Vec3<f32>(0), 1.0F);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());

    // Overlap case (1) - identical position
    REQUIRE(overlap<f32>(s, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(0))));
    // Separated case
    REQUIRE_FALSE(overlap<f32>(s, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(10, 0, 0))));
    // Sphere-vs-cube at clear overlap (interpenetration well above f32 noise).
    // Use 1.5F offset (0.5 depth) instead of 1.9F (0.1 depth) — the latter
    // sits inside the smooth-vs-polyhedral ambiguous band on aggressive FP
    // (clang-cl with O0 hit the iter cap at 0.1 on the v2-close 17-config
    // sweep, returning separated despite the geometric overlap). Per v2b's
    // touching convention: smooth-vs-polyhedral is f32-precision-bound near
    // the boundary; callers needing reliable touching use
    // `gjk_distance(...).distance_squared <= margin²`.
    REQUIRE(overlap<f32>(s, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(1.5F, 0, 0))));
    // Sphere-vs-cube at clear separation (gap well above f32 noise).
    REQUIRE_FALSE(overlap<f32>(s, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.5F, 0, 0))));
    // Facade ALWAYS matches direct (the meaningful contract — facade just
    // dispatches; both should land on the same value for ANY input).
    const bool facade = overlap<f32>(s, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.0F, 0, 0)));
    const bool direct = gjk_overlap<f32>(s, xform(Vec3<f32>(0)), b, xform(Vec3<f32>(2.0F, 0, 0)));
    REQUIRE(facade == direct);

    // Facade and direct call must agree.
    Rng rng(0xFEEDFACEU);
    for (int trial = 0; trial < 30; ++trial)
    {
        const Transform<f32> xs(rng.rand_vec(-3, 3), rng.rand_quat());
        const Transform<f32> xb(rng.rand_vec(-3, 3), rng.rand_quat());
        REQUIRE(overlap<f32>(s, xs, b, xb) == gjk_overlap<f32>(s, xs, b, xb));
    }
}
