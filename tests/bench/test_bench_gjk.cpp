// crd-geometry-convex v2b - off-by-default benchmarks (`[!benchmark]`, not in
// ctest). Cerid's own numbers; targets from `docs/phases/phase-3.1.7-geometry
// .md` Performance budgets table and ADR-0076 §16.
//
// Reference targets (Zen-4-class CPU, win-shipping):
//   * gjk_distance sphere-vs-sphere     ~20-40 ns/pair    (1-3 iters typical)
//   * gjk_distance box-vs-box           ~80-150 ns/pair   (3-6 iters typical)
//   * gjk_distance hull-vs-hull N=8     ~150 ns/pair      (substrate budget)
//   * gjk_overlap on overlapping pair   ~15-20% faster than gjk_distance
//                                       (witness reconstruction skipped on
//                                        the hot iters where GJK is
//                                        "closing in" on the simplex)
//   * gjk_overlap on separated pair     comparable to gjk_distance
//                                       (early-exit on no-progress fires at
//                                        the same iteration count)
//
// `gjk_distance<f64>` (v2i) target: within 1.5x of `f32` on identical
// inputs — the algorithm's per-iter cost grows ~30% from 4-wide to 4-wide
// f64 SIMD; aerospace orbital-scale precision is the value, not raw speed.
//
// `[!benchmark]` is a Catch2 marker that excludes the case from default test
// runs. Run with `crd-bench --benchmark-samples 100 "[!benchmark]"` to see
// the timings.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::f32;
using crd::geometry::convex::gjk_distance;
using crd::geometry::convex::gjk_overlap;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Sphere;
using crd::math::from_axis_angle;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;
} // namespace

TEST_CASE("bench: gjk_distance sphere-vs-sphere", "[!benchmark][gjk]")
{
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 0.7F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(3.0F, 1.0F, -0.5F), Quat<f32>::identity());

    BENCHMARK("sphere-sphere separated")
    {
        return gjk_distance<f32>(a, xa, b, xb);
    };
}

TEST_CASE("bench: gjk_distance box-vs-box rotated", "[!benchmark][gjk]")
{
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Quat<f32> q = from_axis_angle(Vec3<f32>(0, 1, 0), 0.5F);
    const Mat3<f32> rot = crd::math::to_mat3(q);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.8F, 1.2F, 0.5F), rot);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(3.5F, 0.5F, -1.0F), q);

    BENCHMARK("box-box separated rotated")
    {
        return gjk_distance<f32>(a, xa, b, xb);
    };
}

TEST_CASE("bench: gjk_distance sphere-vs-OBB", "[!benchmark][gjk]")
{
    const Sphere<f32> a(Vec3<f32>(0), 0.5F);
    const Quat<f32> q = from_axis_angle(Vec3<f32>(0, 1, 0), 0.5236F);
    const Mat3<f32> rot = crd::math::to_mat3(q);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), rot);
    const Transform<f32> xa(Vec3<f32>(4, 0, 0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0), q);

    BENCHMARK("sphere-OBB separated")
    {
        return gjk_distance<f32>(a, xa, b, xb);
    };
}

TEST_CASE("bench: gjk_overlap vs gjk_distance (overlapping pair)", "[!benchmark][gjk][overlap]")
{
    // Overlapping pair: GJK iterates more (the simplex "closes in" on origin).
    // Witness-reconstruction skip should be most valuable here.
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.8F, 1.2F, 0.5F), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0.5F, 0.3F, 0.1F), Quat<f32>::identity());

    BENCHMARK("gjk_distance (overlapping)")
    {
        return gjk_distance<f32>(a, xa, b, xb);
    };
    BENCHMARK("gjk_overlap (overlapping)")
    {
        return gjk_overlap<f32>(a, xa, b, xb);
    };
}

TEST_CASE("bench: gjk_overlap vs gjk_distance (separated pair)", "[!benchmark][gjk][overlap]")
{
    // Separated pair: same iter count expected for both (early-exit fires at
    // the same point). gjk_overlap saves the per-iter witness work.
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.8F, 1.2F, 0.5F), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(3.5F, 0.0F, 0.0F), Quat<f32>::identity());

    BENCHMARK("gjk_distance (separated)")
    {
        return gjk_distance<f32>(a, xa, b, xb);
    };
    BENCHMARK("gjk_overlap (separated)")
    {
        return gjk_overlap<f32>(a, xa, b, xb);
    };
}

TEST_CASE("bench: gjk_distance capsule-vs-capsule", "[!benchmark][gjk]")
{
    const Capsule3<f32> a(Vec3<f32>(0, 0, -1), Vec3<f32>(0, 0, 1), 0.3F);
    const Capsule3<f32> b(Vec3<f32>(0, 0, -1), Vec3<f32>(0, 0, 1), 0.5F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(2.5F, 0.5F, 0.5F),
                            from_axis_angle(Vec3<f32>(1, 0, 0), 0.7F));

    BENCHMARK("capsule-capsule separated rotated")
    {
        return gjk_distance<f32>(a, xa, b, xb);
    };
}

// ===========================================================================
// v2d: SAT vs GJK / GJK+EPA on OBB pairs.
//
// Expected speedup: SAT ~2-3x faster than GJK (overlap-only) on OBB-OBB,
// and ~3-5x faster than GJK+EPA. SAT is a fixed-cost 15-axis test with
// no iteration; GJK iterates 4-8 times, EPA another 10-30. SAT wins both
// on overlapping AND on separated pairs since it pays the same 15 axes
// in either case (vs GJK's variable iter count).
// ===========================================================================

TEST_CASE("bench: SAT vs GJK on overlapping OBB-OBB", "[!benchmark][sat][gjk]")
{
    const crd::geometry::primitives::OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const crd::geometry::primitives::OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.8F, 1.2F, 0.5F), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0.5F, 0.3F, 0.1F),
                            from_axis_angle(Vec3<f32>(0, 1, 0), 0.4F));

    BENCHMARK("SAT obb-obb (overlapping)")
    {
        return crd::geometry::convex::sat_obb_obb<f32>(a, xa, b, xb);
    };
    BENCHMARK("gjk_overlap obb-obb (overlapping, ConvexShape path)")
    {
        return crd::geometry::convex::gjk_overlap<f32>(a, xa, b, xb);
    };
    BENCHMARK("compute_contact obb-obb (GJK+EPA, ConvexShape path)")
    {
        return crd::geometry::convex::compute_contact<f32>(a, xa, b, xb);
    };
}

TEST_CASE("bench: SAT vs GJK on separated OBB-OBB", "[!benchmark][sat][gjk]")
{
    const crd::geometry::primitives::OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const crd::geometry::primitives::OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(0.8F, 1.2F, 0.5F), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(4.0F, 0.5F, -1.0F),
                            from_axis_angle(Vec3<f32>(0, 1, 0), 0.4F));

    BENCHMARK("SAT obb-obb (separated)")
    {
        return crd::geometry::convex::sat_obb_obb<f32>(a, xa, b, xb);
    };
    BENCHMARK("gjk_distance obb-obb (separated)")
    {
        return crd::geometry::convex::gjk_distance<f32>(a, xa, b, xb);
    };
}

// v2g: hill-climb vs linear-scan hull support, via GJK on hull-vs-hull.
// The hull is built at TU-scope so the bench measures only GJK's
// per-call cost (not hull setup). Target (Zen 4): ~150 ns linear-scan
// (N=8 cube) drops to ~50 ns with hill-climb for N=64; here we use the
// 8-vertex cube to show the warm-start floor (where hill-climb is at
// most marginally better than linear scan since N is small). The N=64
// number is a v2g-followup once the V-HACD pipeline outputs adjacency.
namespace bench_v2g
{
struct CubeHull
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<crd::geometry::primitives::Plane<f32>> faces;
    crd::containers::Array<crd::u32> face_vertex_indices;
    crd::containers::Array<crd::u32> face_vertex_offsets;
    crd::containers::Array<crd::u32> adj_indices;
    crd::containers::Array<crd::u32> adj_offsets;

    explicit CubeHull(crd::memory::IAllocator* alloc, f32 half = 1.0F)
        : verts(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc), adj_indices(alloc),
          adj_offsets(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? half : -half, (i & 2) ? half : -half, (i & 1) ? half : -half));
        }
        // Adjacency: each cube vertex has 3 neighbors (the 3 cube edges).
        // For vertex (sx, sy, sz): neighbors are the 3 vertices that differ
        // in exactly one bit (XYZ axes).
        adj_offsets.push_back(0);
        for (int i = 0; i < 8; ++i)
        {
            adj_indices.push_back(static_cast<crd::u32>(i ^ 4)); // flip x
            adj_indices.push_back(static_cast<crd::u32>(i ^ 2)); // flip y
            adj_indices.push_back(static_cast<crd::u32>(i ^ 1)); // flip z
            adj_offsets.push_back(static_cast<crd::u32>(adj_indices.size()));
        }
    }
    crd::geometry::primitives::ConvexHullView<f32> view_no_adjacency() const
    {
        return crd::geometry::primitives::ConvexHullView<f32>(
            crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
            crd::containers::ConstSpan<crd::geometry::primitives::Plane<f32>>(faces.data(), faces.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_indices.data(), face_vertex_indices.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_offsets.data(), face_vertex_offsets.size()));
    }
    crd::geometry::primitives::ConvexHullView<f32> view_with_adjacency() const
    {
        return crd::geometry::primitives::ConvexHullView<f32>(
            crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
            crd::containers::ConstSpan<crd::geometry::primitives::Plane<f32>>(faces.data(), faces.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_indices.data(), face_vertex_indices.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_offsets.data(), face_vertex_offsets.size()),
            crd::containers::ConstSpan<crd::u32>(adj_indices.data(), adj_indices.size()),
            crd::containers::ConstSpan<crd::u32>(adj_offsets.data(), adj_offsets.size()));
    }
};
} // namespace bench_v2g

TEST_CASE("bench: GJK hull-vs-hull WITH vs WITHOUT adjacency (hill-climb dispatch)",
          "[!benchmark][gjk][hill-climb]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "bench-hill");
    bench_v2g::CubeHull cube_a(&alloc);
    bench_v2g::CubeHull cube_b(&alloc);
    const auto a_plain = cube_a.view_no_adjacency();
    const auto a_adj = cube_a.view_with_adjacency();
    const auto b_plain = cube_b.view_no_adjacency();
    const auto b_adj = cube_b.view_with_adjacency();
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(3.5F, 0.5F, -1.0F),
                            from_axis_angle(Vec3<f32>(0, 1, 0), 0.4F));

    BENCHMARK("gjk_distance hull-hull (linear scan)")
    {
        return crd::geometry::convex::gjk_distance<f32>(a_plain, xa, b_plain, xb);
    };
    BENCHMARK("gjk_distance hull-hull (hill-climb)")
    {
        return crd::geometry::convex::gjk_distance<f32>(a_adj, xa, b_adj, xb);
    };
}

// v2h: SoA + Vec8f SIMD-batched hull support.
// Expected on Zen 4: ~150 ns linear-scan (N=8 cube) → ~50 ns with SIMD
// for N=32 hulls, and ~30 ns with v2g warm-start combined.
namespace bench_v2h
{
struct CubeHullSoA
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<crd::geometry::primitives::Plane<f32>> faces;
    crd::containers::Array<crd::u32> face_vertex_indices;
    crd::containers::Array<crd::u32> face_vertex_offsets;
    crd::containers::Array<f32> vx, vy, vz; // SoA padded to multiple of 8

    explicit CubeHullSoA(crd::memory::IAllocator* alloc, f32 half = 1.0F)
        : verts(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc), vx(alloc), vy(alloc),
          vz(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? half : -half, (i & 2) ? half : -half, (i & 1) ? half : -half));
        }
        // SoA: 8 vertices, padded to 8 (already multiple of 8 — no padding needed).
        for (int i = 0; i < 8; ++i)
        {
            vx.push_back(verts[i].x);
            vy.push_back(verts[i].y);
            vz.push_back(verts[i].z);
        }
    }
    crd::geometry::primitives::ConvexHullView<f32> view_no_soa() const
    {
        return crd::geometry::primitives::ConvexHullView<f32>(
            crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
            crd::containers::ConstSpan<crd::geometry::primitives::Plane<f32>>(faces.data(), faces.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_indices.data(), face_vertex_indices.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_offsets.data(), face_vertex_offsets.size()));
    }
    crd::geometry::primitives::ConvexHullView<f32> view_with_soa() const
    {
        return crd::geometry::primitives::ConvexHullView<f32>(
            crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
            crd::containers::ConstSpan<crd::geometry::primitives::Plane<f32>>(faces.data(), faces.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_indices.data(), face_vertex_indices.size()),
            crd::containers::ConstSpan<crd::u32>(face_vertex_offsets.data(), face_vertex_offsets.size()),
            crd::containers::ConstSpan<crd::u32>{}, crd::containers::ConstSpan<crd::u32>{},
            crd::containers::ConstSpan<f32>(vx.data(), vx.size()), crd::containers::ConstSpan<f32>(vy.data(), vy.size()),
            crd::containers::ConstSpan<f32>(vz.data(), vz.size()));
    }
};
} // namespace bench_v2h

TEST_CASE("bench: GJK hull-vs-hull WITH vs WITHOUT SoA (SIMD-batched support dispatch)",
          "[!benchmark][gjk][simd]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "bench-simd");
    bench_v2h::CubeHullSoA cube_a(&alloc);
    bench_v2h::CubeHullSoA cube_b(&alloc);
    const auto a_plain = cube_a.view_no_soa();
    const auto a_soa = cube_a.view_with_soa();
    const auto b_plain = cube_b.view_no_soa();
    const auto b_soa = cube_b.view_with_soa();
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(3.5F, 0.5F, -1.0F),
                            from_axis_angle(Vec3<f32>(0, 1, 0), 0.4F));

    BENCHMARK("gjk_distance hull-hull (AoS linear scan)")
    {
        return crd::geometry::convex::gjk_distance<f32>(a_plain, xa, b_plain, xb);
    };
    BENCHMARK("gjk_distance hull-hull (SoA Vec8f SIMD)")
    {
        return crd::geometry::convex::gjk_distance<f32>(a_soa, xa, b_soa, xb);
    };
}

// v2j: Sutherland-Hodgman convex polygon clipping throughput.
// Manifold-builder proxy: 5-vertex polygon clipped against a 6-plane convex
// volume (OBB-style side planes). On Zen 4 expect ~50-100 ns per clip
// (5 verts × 6 planes = 30 edge tests). Repeated runs reuse caller-supplied
// scratch — no per-call allocation.
TEST_CASE("bench: clip_against_convex_volume 5-vert vs 6-plane", "[!benchmark][clip][v2j]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "bench-clip");
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);
    crd::containers::Array<Vec3<f32>> scratch(&alloc);
    crd::containers::Array<crd::geometry::primitives::Plane<f32>> planes(&alloc);

    // 5-vertex convex polygon centred at origin in the z=0 plane.
    input.push_back(Vec3<f32>(-3.5F, -2.7F, 0.0F));
    input.push_back(Vec3<f32>(2.9F, -3.1F, 0.0F));
    input.push_back(Vec3<f32>(4.2F, 1.6F, 0.0F));
    input.push_back(Vec3<f32>(0.7F, 3.8F, 0.0F));
    input.push_back(Vec3<f32>(-3.1F, 2.2F, 0.0F));

    // Unit-cube-style 6 side planes (dot(n,x) + d <= 0 is inside).
    planes.push_back(crd::geometry::primitives::Plane<f32>(Vec3<f32>(1, 0, 0), -1));
    planes.push_back(crd::geometry::primitives::Plane<f32>(Vec3<f32>(-1, 0, 0), -1));
    planes.push_back(crd::geometry::primitives::Plane<f32>(Vec3<f32>(0, 1, 0), -1));
    planes.push_back(crd::geometry::primitives::Plane<f32>(Vec3<f32>(0, -1, 0), -1));
    planes.push_back(crd::geometry::primitives::Plane<f32>(Vec3<f32>(0, 0, 1), -1));
    planes.push_back(crd::geometry::primitives::Plane<f32>(Vec3<f32>(0, 0, -1), -1));

    BENCHMARK("clip_against_convex_volume 5x6")
    {
        crd::geometry::convex::clip_against_convex_volume<f32>(
            crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()),
            crd::containers::ConstSpan<crd::geometry::primitives::Plane<f32>>(planes.data(), planes.size()),
            output, scratch);
        return output.size();
    };
}
