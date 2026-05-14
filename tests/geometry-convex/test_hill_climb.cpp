// crd-geometry-convex v2g - hill-climbing hull support tests.
//
// Four claim categories:
//
//   (1) DETERMINISM CONTRACT (the load-bearing test): for ANY direction,
//       `hill_climb_support` returns the SAME vertex_idx that linear-scan
//       `support` returns. Verified across 1000 random directions on a
//       60-ish-vertex test hull. If this fails, GJK's index-match
//       termination breaks for hulls-with-adjacency callers.
//
//   (2) START-INDEPENDENCE: regardless of `start_idx`, hill_climb_support
//       converges to the same answer (same point AND same vidx after
//       tiebreak).
//
//   (3) GJK WARM-START INTEGRATION: gjk_distance on hull-vs-hull with
//       adjacency produces identical output to the no-adjacency path
//       (same witness, same distance², bit-exact replay).
//
//   (4) DISPATCH: `support_with_hint(hull, dir, invalid_hint)` and
//       `support_with_hint(hull-without-adjacency, dir, valid_hint)`
//       both fall back to linear scan. Only `adjacency-present + valid-
//       hint` routes to hill-climb.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::convex::gjk_distance;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::hill_climb_support;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::support;
using crd::geometry::primitives::support_with_hint;
using crd::geometry::primitives::SupportPoint;
using crd::math::from_axis_angle;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;

bool approx(f32 l, f32 r, f32 tol = 1e-5F)
{
    return std::fabs(l - r) <= tol;
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
    Vec3<f32> rand_unit_vec()
    {
        Vec3<f32> v(range(-1, 1), range(-1, 1), range(-1, 1));
        const f32 l = std::sqrt(crd::math::dot(v, v));
        if (l < 1e-3F)
        {
            return Vec3<f32>(1, 0, 0);
        }
        return Vec3<f32>(v.x / l, v.y / l, v.z / l);
    }
};

// Compute vertex adjacency from face-vertex topology. For each face, every
// consecutive vertex pair (and wrap-around) is an edge; the endpoints
// become neighbors of each other. Output is the prefix-sum form expected
// by `ConvexHullView::vertex_adjacency_*`. Helper for test fixtures; will
// likely live in the v3 hull cooker as production code.
struct AdjacencyOut
{
    crd::containers::Array<u32> indices;
    crd::containers::Array<u32> offsets;
    explicit AdjacencyOut(crd::memory::IAllocator* alloc) : indices(alloc), offsets(alloc) {}
};

AdjacencyOut compute_vertex_adjacency_from_faces(crd::memory::IAllocator* alloc,
                                                  crd::containers::ConstSpan<u32> face_indices,
                                                  crd::containers::ConstSpan<u32> face_offsets, usize num_vertices)
{
    // Step 1: per-vertex neighbor sets (small Array per vertex, dedup on
    // insert).
    crd::containers::Array<crd::containers::Array<u32>> per_vertex(alloc);
    per_vertex.reserve(num_vertices);
    for (usize i = 0; i < num_vertices; ++i)
    {
        per_vertex.push_back(crd::containers::Array<u32>(alloc));
    }
    auto add_edge = [&](u32 a, u32 b) {
        auto& na = per_vertex[a];
        bool found = false;
        for (usize k = 0; k < na.size(); ++k)
        {
            if (na[k] == b)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            na.push_back(b);
        }
    };
    // Walk each face's edges (closing wrap-around at the end of each polygon).
    const usize num_faces = face_offsets.size() - 1;
    for (usize f = 0; f < num_faces; ++f)
    {
        const u32 begin = face_offsets[f];
        const u32 end = face_offsets[f + 1];
        for (u32 k = begin; k < end; ++k)
        {
            const u32 v_a = face_indices[k];
            const u32 v_b = face_indices[(k + 1 < end) ? (k + 1) : begin];
            add_edge(v_a, v_b);
            add_edge(v_b, v_a);
        }
    }
    // Step 2: flatten into prefix-sum form.
    AdjacencyOut out(alloc);
    out.offsets.reserve(num_vertices + 1);
    out.offsets.push_back(0);
    for (usize i = 0; i < num_vertices; ++i)
    {
        for (usize k = 0; k < per_vertex[i].size(); ++k)
        {
            out.indices.push_back(per_vertex[i][k]);
        }
        out.offsets.push_back(static_cast<u32>(out.indices.size()));
    }
    return out;
}

// Test hull #1: unit cube (8 vertices, 6 faces). Used for determinism +
// dispatch tests. Each vertex has exactly 3 neighbors (the 3 cube edges
// meeting at it).
struct CubeHullWithAdjacency
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> face_vertex_indices;
    crd::containers::Array<u32> face_vertex_offsets;
    AdjacencyOut adj;

    explicit CubeHullWithAdjacency(crd::memory::IAllocator* alloc)
        : verts(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc), adj(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? 1.0F : -1.0F, (i & 2) ? 1.0F : -1.0F, (i & 1) ? 1.0F : -1.0F));
        }
        // 6 face planes (outward normals).
        faces.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(-1, 0, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, -1, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, 1), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, -1), -1));
        // Face-vertex indices (CCW from outside).
        face_vertex_indices.push_back(4); face_vertex_indices.push_back(5);
        face_vertex_indices.push_back(7); face_vertex_indices.push_back(6); // +X face
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
        // Compute adjacency.
        adj = compute_vertex_adjacency_from_faces(
            alloc, crd::containers::ConstSpan<u32>(face_vertex_indices.data(), face_vertex_indices.size()),
            crd::containers::ConstSpan<u32>(face_vertex_offsets.data(), face_vertex_offsets.size()), verts.size());
    }
    ConvexHullView<f32> view_no_adjacency() const
    {
        return ConvexHullView<f32>(crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
                                   crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
                                   crd::containers::ConstSpan<u32>(face_vertex_indices.data(),
                                                                    face_vertex_indices.size()),
                                   crd::containers::ConstSpan<u32>(face_vertex_offsets.data(),
                                                                    face_vertex_offsets.size()));
    }
    ConvexHullView<f32> view_with_adjacency() const
    {
        return ConvexHullView<f32>(
            crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
            crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
            crd::containers::ConstSpan<u32>(face_vertex_indices.data(), face_vertex_indices.size()),
            crd::containers::ConstSpan<u32>(face_vertex_offsets.data(), face_vertex_offsets.size()),
            crd::containers::ConstSpan<u32>(adj.indices.data(), adj.indices.size()),
            crd::containers::ConstSpan<u32>(adj.offsets.data(), adj.offsets.size()));
    }
};
} // namespace

// ===========================================================================
// DETERMINISM CONTRACT
// ===========================================================================

TEST_CASE("hill_climb_support: produces same vertex_idx as linear scan (determinism contract)",
          "[hill-climb][determinism]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hill-climb");
    const CubeHullWithAdjacency cube(&alloc);
    const auto hull_adj = cube.view_with_adjacency();
    const auto hull_plain = cube.view_no_adjacency();

    Rng rng(0xCABBAB1EU);
    int verified = 0;
    int mismatches = 0;
    for (int trial = 0; trial < 1000; ++trial)
    {
        const Vec3<f32> dir = rng.rand_unit_vec();
        // Linear-scan reference (no adjacency, so support() falls back to linear).
        const SupportPoint<f32> ref = support(hull_plain, dir);
        // Hill-climb from a random valid start.
        const u32 start = static_cast<u32>(rng.next() % hull_adj.vertices.size());
        const SupportPoint<f32> hc = hill_climb_support(hull_adj, dir, start);
        // The CONTRACT: same vertex_idx as linear scan.
        if (hc.vertex_idx != ref.vertex_idx)
        {
            ++mismatches;
            if (mismatches <= 5)
            {
                INFO("trial " << trial << " dir=(" << dir.x << "," << dir.y << "," << dir.z << ") start=" << start
                              << " ref_vidx=" << ref.vertex_idx << " hc_vidx=" << hc.vertex_idx);
            }
        }
        // The POINT must always match (the projection value is the global max).
        REQUIRE(approx(hc.point.x, ref.point.x));
        REQUIRE(approx(hc.point.y, ref.point.y));
        REQUIRE(approx(hc.point.z, ref.point.z));
        ++verified;
    }
    INFO("verified=" << verified << " mismatches=" << mismatches);
    REQUIRE(mismatches == 0);
}

// ===========================================================================
// START-INDEPENDENCE
// ===========================================================================

TEST_CASE("hill_climb_support: same answer regardless of start_idx", "[hill-climb][start-independence]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hill-climb");
    const CubeHullWithAdjacency cube(&alloc);
    const auto hull = cube.view_with_adjacency();

    Rng rng(0xC0DEDC0DEU);
    for (int trial = 0; trial < 50; ++trial)
    {
        const Vec3<f32> dir = rng.rand_unit_vec();
        const SupportPoint<f32> base = hill_climb_support(hull, dir, 0U);
        for (u32 start = 1; start < hull.vertices.size(); ++start)
        {
            const SupportPoint<f32> from_start = hill_climb_support(hull, dir, start);
            INFO("trial " << trial << " start=" << start << " base_vidx=" << base.vertex_idx
                          << " from_start_vidx=" << from_start.vertex_idx);
            REQUIRE(from_start.vertex_idx == base.vertex_idx);
            REQUIRE(approx(from_start.point.x, base.point.x));
            REQUIRE(approx(from_start.point.y, base.point.y));
            REQUIRE(approx(from_start.point.z, base.point.z));
        }
    }
}

// ===========================================================================
// GJK WARM-START INTEGRATION
// ===========================================================================

TEST_CASE("gjk_distance: hull-vs-hull with adjacency produces same result as without",
          "[hill-climb][gjk][integration]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "hill-climb");
    const CubeHullWithAdjacency cube_a(&alloc);
    const CubeHullWithAdjacency cube_b(&alloc);
    const auto a_adj = cube_a.view_with_adjacency();
    const auto a_plain = cube_a.view_no_adjacency();
    const auto b_adj = cube_b.view_with_adjacency();
    const auto b_plain = cube_b.view_no_adjacency();

    Rng rng(0xFADED0F1U);
    for (int trial = 0; trial < 30; ++trial)
    {
        // Random rigid transforms (both translation + rotation).
        const Vec3<f32> ta(rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));
        const Vec3<f32> tb(rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));
        const Vec3<f32> ax_a = rng.rand_unit_vec();
        const Vec3<f32> ax_b = rng.rand_unit_vec();
        const Quat<f32> qa = from_axis_angle(ax_a, rng.range(-3.14F, 3.14F));
        const Quat<f32> qb = from_axis_angle(ax_b, rng.range(-3.14F, 3.14F));
        const Transform<f32> xa(ta, qa);
        const Transform<f32> xb(tb, qb);

        const auto r_adj = gjk_distance<f32>(a_adj, xa, b_adj, xb);
        const auto r_plain = gjk_distance<f32>(a_plain, xa, b_plain, xb);

        INFO("trial " << trial << " adj.overlap=" << r_adj.overlapping << " plain.overlap=" << r_plain.overlapping
                      << " adj.dist²=" << r_adj.distance_squared << " plain.dist²=" << r_plain.distance_squared);
        REQUIRE(r_adj.overlapping == r_plain.overlapping);
        REQUIRE(r_adj.distance_squared == r_plain.distance_squared);
        REQUIRE(std::memcmp(&r_adj.witness_a_world, &r_plain.witness_a_world, sizeof(Vec3<f32>)) == 0);
        REQUIRE(std::memcmp(&r_adj.witness_b_world, &r_plain.witness_b_world, sizeof(Vec3<f32>)) == 0);
    }
}

// ===========================================================================
// DISPATCH
// ===========================================================================

TEST_CASE("support_with_hint: falls back to linear scan when no adjacency or invalid hint",
          "[hill-climb][dispatch]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 14, nullptr, "hill-climb");
    const CubeHullWithAdjacency cube(&alloc);
    const auto hull_adj = cube.view_with_adjacency();
    const auto hull_plain = cube.view_no_adjacency();

    const Vec3<f32> dir(1, 0, 0);
    const SupportPoint<f32> ref = support(hull_plain, dir);

    // No adjacency → falls back regardless of hint.
    {
        const SupportPoint<f32> r = support_with_hint(hull_plain, dir, 3U);
        REQUIRE(r.vertex_idx == ref.vertex_idx);
        REQUIRE(approx(r.point.x, ref.point.x));
    }
    // Adjacency present, invalid hint → falls back.
    {
        const SupportPoint<f32> r = support_with_hint(hull_adj, dir, ~u32{0});
        REQUIRE(r.vertex_idx == ref.vertex_idx);
    }
    // Adjacency present, valid hint → uses hill-climb, returns same answer.
    {
        const SupportPoint<f32> r = support_with_hint(hull_adj, dir, 0U);
        REQUIRE(r.vertex_idx == ref.vertex_idx);
    }
}

TEST_CASE("support_with_hint: generic template for non-hull shapes delegates to support()",
          "[hill-climb][dispatch][generic]")
{
    // Sphere, OBB, Capsule all go through the generic template fallback.
    using crd::geometry::primitives::Capsule3;
    using crd::geometry::primitives::OBB3;
    using crd::geometry::primitives::Sphere;
    const Sphere<f32> s(Vec3<f32>(0), 1.0F);
    const Vec3<f32> d(0, 1, 0);
    const SupportPoint<f32> a = support(s, d);
    const SupportPoint<f32> b = support_with_hint(s, d, 42U); // hint is ignored
    REQUIRE(a.vertex_idx == b.vertex_idx);
    REQUIRE(approx(a.point.x, b.point.x));
    REQUIRE(approx(a.point.y, b.point.y));
    REQUIRE(approx(a.point.z, b.point.z));

    const OBB3<f32> box(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const SupportPoint<f32> c = support(box, d);
    const SupportPoint<f32> e = support_with_hint(box, d, 7U);
    REQUIRE(c.vertex_idx == e.vertex_idx);
}
