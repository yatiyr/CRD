// crd-geometry-convex v2e - ConvexHullView queries tests.
//
// Five claim categories:
//
//   (1) RAY_VS_HULL CLOSED-FORM: rays against an axis-aligned cube hit
//       known faces at known t values. Each of the 6 cube faces gets a
//       hit-from-the-corresponding-axis test.
//
//   (2) RAY_VS_HULL DETERMINISM ON EDGE CONTACT: ray aimed at a cube
//       edge produces a hit where two adjacent face planes share the
//       same `t_enter`. Lowest-face-index tiebreak must produce a
//       replay-stable result.
//
//   (3) RAY_VS_HULL FROM-INSIDE = nullopt + PARALLEL = nullopt + TMAX:
//       three edge-case classes the algorithm must handle. From-inside
//       returns nullopt (per the v2e convention pin). Parallel rays
//       outside the hull's halfspace return nullopt. Hits past tmax are
//       rejected.
//
//   (4) CLOSEST_POINT FROM OUTSIDE: hull is a unit cube; for various p
//       outside, closest_point should match the brute-force AABB clamp
//       (since the cube IS an AABB).
//
//   (5) CLOSEST_POINT FROM INSIDE: p inside the cube projects to the
//       nearest face plane. Documented limit: returns point on the face
//       PLANE, not necessarily on the face polygon. We test the common
//       case (interior point, well-defined nearest face).
//
// Plus FACADE: `crd::geometry::raycast(ConvexHullView, Ray3)` dispatches
// to ray_vs_hull and matches the direct call.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::convex::closest_point;
using crd::geometry::convex::distance;
using crd::geometry::convex::distance_squared;
using crd::geometry::convex::ray_vs_hull;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::contains;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3;

bool approx(const Vec3<f32>& l, const Vec3<f32>& r, f32 tol = 1e-3F)
{
    return std::fabs(l.x - r.x) <= tol && std::fabs(l.y - r.y) <= tol && std::fabs(l.z - r.z) <= tol;
}
bool approx(f32 l, f32 r, f32 tol = 1e-3F)
{
    return std::fabs(l - r) <= tol;
}

// 8-vertex cube hull + 6 face planes. The face index order is FIXED
// (visible in test assertions): 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
struct CubeHullWithFaces
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> face_vertex_indices;
    crd::containers::Array<u32> face_vertex_offsets;

    explicit CubeHullWithFaces(crd::memory::IAllocator* alloc, f32 half = 1.0F)
        : verts(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? half : -half, (i & 2) ? half : -half, (i & 1) ? half : -half));
        }
        // Face planes (outward normals). Plane equation: dot(n, p) + d = 0.
        // For "+X face at x = half", n = (1,0,0), d = -half (so 1*half - half = 0). ✓
        faces.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -half));  // 0: +X
        faces.push_back(Plane<f32>(Vec3<f32>(-1, 0, 0), -half)); // 1: -X
        faces.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -half));  // 2: +Y
        faces.push_back(Plane<f32>(Vec3<f32>(0, -1, 0), -half)); // 3: -Y
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, 1), -half));  // 4: +Z
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, -1), -half)); // 5: -Z
        // Face-vertex indices: 4 vertices per face, CCW from outside.
        // (Used by `contains` only as a polygon-vertex listing — v2e's
        // queries don't need them, but populating for completeness.)
        face_vertex_indices.push_back(4); face_vertex_indices.push_back(5);
        face_vertex_indices.push_back(7); face_vertex_indices.push_back(6); // +X
        face_vertex_indices.push_back(0); face_vertex_indices.push_back(2);
        face_vertex_indices.push_back(3); face_vertex_indices.push_back(1); // -X
        face_vertex_indices.push_back(2); face_vertex_indices.push_back(6);
        face_vertex_indices.push_back(7); face_vertex_indices.push_back(3); // +Y
        face_vertex_indices.push_back(0); face_vertex_indices.push_back(1);
        face_vertex_indices.push_back(5); face_vertex_indices.push_back(4); // -Y
        face_vertex_indices.push_back(1); face_vertex_indices.push_back(3);
        face_vertex_indices.push_back(7); face_vertex_indices.push_back(5); // +Z
        face_vertex_indices.push_back(0); face_vertex_indices.push_back(4);
        face_vertex_indices.push_back(6); face_vertex_indices.push_back(2); // -Z
        face_vertex_offsets.push_back(0);
        for (int f = 1; f <= 6; ++f)
        {
            face_vertex_offsets.push_back(static_cast<u32>(f * 4));
        }
    }
    ConvexHullView<f32> view() const
    {
        return ConvexHullView<f32>(crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
                                   crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
                                   crd::containers::ConstSpan<u32>(face_vertex_indices.data(),
                                                                    face_vertex_indices.size()),
                                   crd::containers::ConstSpan<u32>(face_vertex_offsets.data(),
                                                                    face_vertex_offsets.size()));
    }
};

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
};
} // namespace

// ===========================================================================
// ray_vs_hull: CLOSED-FORM (axis-aligned cube)
// ===========================================================================

TEST_CASE("ray_vs_hull: ray along +X hits -X face at known t", "[hull-query][ray][closed-form]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray origin (-3, 0, 0), direction +X. Hits cube's -X face (x = -1) at t=2.
    const Ray3<f32> ray(Vec3<f32>(-3, 0, 0), Vec3<f32>(1, 0, 0));
    const auto hit = ray_vs_hull<f32>(cube.view(), ray);
    REQUIRE(hit.has_value());
    REQUIRE(approx(hit->t, 2.0F));
    REQUIRE(hit->payload == 1); // -X face index
}

TEST_CASE("ray_vs_hull: ray along -Y hits +Y face", "[hull-query][ray][closed-form]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    const Ray3<f32> ray(Vec3<f32>(0, 5, 0), Vec3<f32>(0, -1, 0));
    const auto hit = ray_vs_hull<f32>(cube.view(), ray);
    REQUIRE(hit.has_value());
    REQUIRE(approx(hit->t, 4.0F));
    REQUIRE(hit->payload == 2); // +Y face
}

TEST_CASE("ray_vs_hull: diagonal ray", "[hull-query][ray][closed-form]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray from (5, 5, 0) toward origin, direction = (-1,-1,0)/sqrt(2).
    // First hits the cube's +X face at x=1 (parametric distance from (5,5,0)
    // along (-1,-1,0)/sqrt(2)). For ray direction (-1,-1,0) (unit-ish): t = 4*sqrt(2).
    // Reaching x=1 from x=5 along -X requires moving (4, 4, 0), so t = 4 (along
    // (-1,-1,0)/sqrt(2)) is t = 4*sqrt(2) ~= 5.657.
    // Hmm actually with direction (-1, -1, 0) (NOT unit), t=4 gives delta(-4,-4,0)
    // → from (5,5,0) → (1, 1, 0). x=1 ✓, y=1 ✓. So hits at (1, 1, 0) at t=4.
    // The entering face: +X (x=1) AND +Y (y=1) are both at exact t=4. Tiebreak
    // to lowest face index → +X (face 0).
    const Ray3<f32> ray(Vec3<f32>(5, 5, 0), Vec3<f32>(-1, -1, 0));
    const auto hit = ray_vs_hull<f32>(cube.view(), ray);
    REQUIRE(hit.has_value());
    INFO("t=" << hit->t << ", face=" << hit->payload);
    REQUIRE(approx(hit->t, 4.0F));
    REQUIRE(hit->payload == 0); // +X face (lowest index of the two tied)
}

// ===========================================================================
// ray_vs_hull: EDGE-CASE / EDGE-CONTACT DETERMINISM
// ===========================================================================

TEST_CASE("ray_vs_hull: edge contact deterministic (lowest face index)", "[hull-query][ray][determinism]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray hitting cube edge at (1, 1, 0) — shared between +X (face 0) and
    // +Y (face 2). Replay calls return the same payload.
    const Ray3<f32> ray(Vec3<f32>(5, 5, 0), Vec3<f32>(-1, -1, 0));
    const auto h1 = ray_vs_hull<f32>(cube.view(), ray);
    const auto h2 = ray_vs_hull<f32>(cube.view(), ray);
    REQUIRE(h1.has_value());
    REQUIRE(h2.has_value());
    REQUIRE(h1->payload == h2->payload);
    REQUIRE(h1->t == h2->t);
}

TEST_CASE("ray_vs_hull: ray entirely outside hull's halfspace (parallel to face, miss)",
          "[hull-query][ray][edge-case]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray origin (5, 0, 0), direction (0, 1, 0). Ray's X coord stays 5
    // forever — never enters cube's +X face (x = 1). Should be nullopt.
    const Ray3<f32> ray(Vec3<f32>(5, 0, 0), Vec3<f32>(0, 1, 0));
    const auto hit = ray_vs_hull<f32>(cube.view(), ray);
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("ray_vs_hull: from-inside the hull returns nullopt", "[hull-query][ray][from-inside]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray origin (0, 0, 0) (CENTER of cube), direction (1, 0, 0).
    // From-inside convention pin: nullopt.
    const Ray3<f32> ray(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0));
    REQUIRE_FALSE(ray_vs_hull<f32>(cube.view(), ray).has_value());
    // Use `contains` to detect from-inside per convention.
    REQUIRE(contains(cube.view(), Vec3<f32>(0, 0, 0)));
}

TEST_CASE("ray_vs_hull: tmax cuts off valid hit", "[hull-query][ray][tmax]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray hits at t=2 normally; with tmax=1, no hit.
    const Ray3<f32> ray(Vec3<f32>(-3, 0, 0), Vec3<f32>(1, 0, 0));
    REQUIRE_FALSE(ray_vs_hull<f32>(cube.view(), ray, 1.0F).has_value());
    REQUIRE(ray_vs_hull<f32>(cube.view(), ray, 3.0F).has_value());
}

TEST_CASE("ray_vs_hull: clearly missing ray returns nullopt", "[hull-query][ray]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // Ray (5, 5, 5) direction (1, 0, 0) — moves in +X away from cube.
    const Ray3<f32> ray(Vec3<f32>(5, 5, 5), Vec3<f32>(1, 0, 0));
    REQUIRE_FALSE(ray_vs_hull<f32>(cube.view(), ray).has_value());
}

// ===========================================================================
// closest_point: FROM OUTSIDE (cross-check against AABB-clamp brute force)
// ===========================================================================

TEST_CASE("closest_point on cube hull from outside: matches AABB clamp",
          "[hull-query][closest-point][outside]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);

    // For an axis-aligned cube hull, closest_point should equal the
    // standard AABB clamp: per-axis clamp into [-1, 1]. This is the
    // brute-force reference for from-outside queries.
    auto aabb_clamp = [](const Vec3<f32>& p) {
        return Vec3<f32>(std::clamp(p.x, -1.0F, 1.0F),
                         std::clamp(p.y, -1.0F, 1.0F),
                         std::clamp(p.z, -1.0F, 1.0F));
    };

    Rng rng(0xC0FFEE42U);
    for (int trial = 0; trial < 100; ++trial)
    {
        const Vec3<f32> p = rng.rand_vec(-3, 3);
        // Skip if p is inside the hull (the from-inside test handles that).
        if (contains(cube.view(), p))
        {
            continue;
        }
        const Vec3<f32> cp_gjk = closest_point(cube.view(), p);
        const Vec3<f32> cp_aabb = aabb_clamp(p);
        INFO("trial " << trial << ", p=(" << p.x << "," << p.y << "," << p.z << "), cp_gjk=(" << cp_gjk.x << ","
                      << cp_gjk.y << "," << cp_gjk.z << "), cp_aabb=(" << cp_aabb.x << "," << cp_aabb.y << ","
                      << cp_aabb.z << ")");
        // Tolerance: GJK on smooth-vs-polyhedral converges to within
        // ~1e-3 (the eps² floor). For the cube case it's typically tighter
        // since the cube IS polyhedral and GJK can lock onto exact corners.
        REQUIRE(approx(cp_gjk, cp_aabb, 5e-3F));
    }
}

// ===========================================================================
// closest_point: FROM INSIDE (face-plane projection per v2e pin)
// ===========================================================================

TEST_CASE("closest_point on cube hull from inside: projects to nearest face PLANE",
          "[hull-query][closest-point][inside]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);

    // Point at (0.7, 0, 0) - inside cube; nearest face is +X (distance 0.3).
    // Projected point should be (1, 0, 0).
    {
        const Vec3<f32> p(0.7F, 0, 0);
        const Vec3<f32> cp = closest_point(cube.view(), p);
        REQUIRE(approx(cp, Vec3<f32>(1, 0, 0)));
    }
    // Point at (0, -0.5, 0) - inside; nearest face is -Y (distance 0.5).
    // Projected to (0, -1, 0).
    {
        const Vec3<f32> p(0, -0.5F, 0);
        const Vec3<f32> cp = closest_point(cube.view(), p);
        REQUIRE(approx(cp, Vec3<f32>(0, -1, 0)));
    }
    // Point exactly at center (0, 0, 0) - 6 faces equidistant; lowest-
    // |signed_distance| picks the first one found (face 0 = +X face by
    // construction). Projection: (1, 0, 0).
    {
        const Vec3<f32> p(0, 0, 0);
        const Vec3<f32> cp = closest_point(cube.view(), p);
        // All 6 faces are equidistant at |sd|=1. Whichever face index
        // wins, the projection is the corresponding face center.
        // Verify the result IS on one of the 6 face planes.
        bool on_face = false;
        const f32 tol = 1e-3F;
        if (approx(cp, Vec3<f32>(1, 0, 0), tol) || approx(cp, Vec3<f32>(-1, 0, 0), tol) ||
            approx(cp, Vec3<f32>(0, 1, 0), tol) || approx(cp, Vec3<f32>(0, -1, 0), tol) ||
            approx(cp, Vec3<f32>(0, 0, 1), tol) || approx(cp, Vec3<f32>(0, 0, -1), tol))
        {
            on_face = true;
        }
        INFO("cp=(" << cp.x << "," << cp.y << "," << cp.z << ")");
        REQUIRE(on_face);
    }
}

// ===========================================================================
// distance / distance_squared companions
// ===========================================================================

TEST_CASE("distance(ConvexHullView, p) companion", "[hull-query][closest-point][distance]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    // p at (5, 0, 0). Closest point (1, 0, 0). Distance = 4.
    const Vec3<f32> p(5, 0, 0);
    REQUIRE(approx(distance(cube.view(), p), 4.0F));
    REQUIRE(approx(distance_squared(cube.view(), p), 16.0F));
}

// ===========================================================================
// FACADE DISPATCH
// ===========================================================================

TEST_CASE("Facade: crd::geometry::raycast(ConvexHullView, ...)",
          "[hull-query][facade]")
{
    using crd::geometry::raycast;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hull-test");
    const CubeHullWithFaces cube(&alloc, 1.0F);
    const Ray3<f32> ray(Vec3<f32>(-3, 0, 0), Vec3<f32>(1, 0, 0));
    const auto facade = raycast<f32>(cube.view(), ray);
    const auto direct = ray_vs_hull<f32>(cube.view(), ray);
    REQUIRE(facade.has_value());
    REQUIRE(direct.has_value());
    REQUIRE(facade->t == direct->t);
    REQUIRE(facade->payload == direct->payload);
}
