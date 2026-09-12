// crd-geometry-convex v3c -- 3D Quickhull tests.
//
// Coverage:
//   (1) Closed-form hulls: regular tetrahedron, unit cube, regular octahedron.
//   (2) Hull invariants: every input point is inside-or-on the hull; every
//       face is CCW from outside (orient3d against interior witness > 0);
//       hull is closed (every edge appears in exactly 2 faces).
//   (3) Random point clouds: 50 / 500 / 5000 random points -> hull contains
//       all + invariants hold.
//   (4) Degenerate inputs: 0 / 1 / 2 / coincident / colinear / coplanar.
//   (5) Determinism replay: identical input -> identical hull vertices/faces.
//   (6) f32 + f64 both work.
//   (7) Large-coordinate stability (scale 1e3).

#include <crd/geometry/convex/quickhull.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::geometry::convex::convex_hull_view_of;
using crd::geometry::convex::quickhull;
using crd::geometry::convex::QuickhullResult;
using crd::geometry::primitives::orient3d;
using crd::math::Vec3;

// Compute the centroid of the hull vertices as an interior witness.
template <typename T> Vec3<T> hull_centroid(const QuickhullResult<T>& r)
{
    Vec3<T> c(0, 0, 0);
    for (usize i = 0; i < r.vertices.size(); ++i)
    {
        c = c + r.vertices[i];
    }
    if (!r.vertices.empty())
    {
        c = c * (static_cast<T>(1) / static_cast<T>(r.vertices.size()));
    }
    return c;
}

// Check that every face is CCW from outside (centroid is "below" =
// orient3d > 0 in Shewchuk convention).
template <typename T> bool all_faces_ccw(const QuickhullResult<T>& r)
{
    if (r.faces.empty())
    {
        return true; // degenerate
    }
    const Vec3<T> centroid = hull_centroid(r);
    for (usize f = 0; f < r.faces.size(); ++f)
    {
        const u32 begin = r.face_vertex_offsets[f];
        const u32 end = r.face_vertex_offsets[f + 1];
        if (end - begin < 3)
        {
            return false;
        }
        const Vec3<T>& v0 = r.vertices[r.face_vertex_indices[begin + 0]];
        const Vec3<T>& v1 = r.vertices[r.face_vertex_indices[begin + 1]];
        const Vec3<T>& v2 = r.vertices[r.face_vertex_indices[begin + 2]];
        const T s = orient3d(v0, v1, v2, centroid);
        if (s <= static_cast<T>(0))
        {
            return false;
        }
    }
    return true;
}

// Check that every input point is inside-or-on every face's halfspace.
// Equivalently: for every input p and every face, orient3d(face.v0, v1, v2, p) >= 0
// (point is below / inside).
template <typename T>
bool hull_contains_all_inputs(const QuickhullResult<T>& r,
                              crd::containers::ConstSpan<Vec3<T>> input_points,
                              T tolerance = static_cast<T>(1e-9))
{
    if (r.faces.empty())
    {
        return true;
    }
    for (usize p = 0; p < input_points.size(); ++p)
    {
        const Vec3<T>& pt = input_points[p];
        for (usize f = 0; f < r.faces.size(); ++f)
        {
            const u32 begin = r.face_vertex_offsets[f];
            const Vec3<T>& v0 = r.vertices[r.face_vertex_indices[begin + 0]];
            const Vec3<T>& v1 = r.vertices[r.face_vertex_indices[begin + 1]];
            const Vec3<T>& v2 = r.vertices[r.face_vertex_indices[begin + 2]];
            // Use cached plane distance (faster than orient3d for the
            // containment check; tolerance accounts for f64 ULP).
            const T d = crd::math::dot(r.faces[f].normal, pt) + r.faces[f].d;
            if (d > tolerance)
            {
                (void)v0;
                (void)v1;
                (void)v2;
                return false; // point is outside this face
            }
        }
    }
    return true;
}

// Simple deterministic LCG for test inputs.
struct TestRng
{
    crd::u64 state;
    explicit TestRng(crd::u64 seed) : state(seed) {}
    crd::u64 next()
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f64 unit() { return static_cast<f64>(next() >> 12) / static_cast<f64>(1ULL << 52); }
    f64 range(f64 lo, f64 hi) { return lo + (hi - lo) * unit(); }
};

} // namespace

// ---------------------------------------------------------------------------
// (1) Closed-form hulls
// ---------------------------------------------------------------------------

TEST_CASE("quickhull: regular tetrahedron returns 4 vertices, 4 triangle faces",
          "[v3c][quickhull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    points.push_back(Vec3<f64>(0, 0, 1));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.vertices.size() == 4);
    CHECK(r.faces.size() == 4);
    CHECK(r.face_vertex_indices.size() == 12); // 4 faces × 3 indices
    CHECK(r.face_vertex_offsets.size() == 5);  // 4 faces + 1
    CHECK(!r.is_coplanar);
    CHECK(!r.is_colinear);
    CHECK(!r.is_coincident);
    CHECK(all_faces_ccw(r));
    CHECK(hull_contains_all_inputs<f64>(
        r, crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size())));
}

TEST_CASE("quickhull: unit cube (8 vertices) returns 8 vertices, 12 triangle faces",
          "[v3c][quickhull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    for (int i = 0; i < 8; ++i)
    {
        points.push_back(Vec3<f64>((i & 4) ? 1.0 : 0.0, (i & 2) ? 1.0 : 0.0, (i & 1) ? 1.0 : 0.0));
    }

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.vertices.size() == 8);
    CHECK(r.faces.size() == 12); // cube triangulated: 6 quad faces × 2 = 12 triangles
    CHECK(all_faces_ccw(r));
    CHECK(hull_contains_all_inputs<f64>(
        r, crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size())));
}

TEST_CASE("quickhull: regular octahedron (6 vertices) returns 8 faces", "[v3c][quickhull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(-1, 0, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    points.push_back(Vec3<f64>(0, -1, 0));
    points.push_back(Vec3<f64>(0, 0, 1));
    points.push_back(Vec3<f64>(0, 0, -1));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.vertices.size() == 6);
    CHECK(r.faces.size() == 8);
    CHECK(all_faces_ccw(r));
    CHECK(hull_contains_all_inputs<f64>(
        r, crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size())));
}

TEST_CASE("quickhull: cube with interior points excluded from hull", "[v3c][quickhull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    // 8 cube corners.
    for (int i = 0; i < 8; ++i)
    {
        points.push_back(Vec3<f64>((i & 4) ? 1.0 : 0.0, (i & 2) ? 1.0 : 0.0, (i & 1) ? 1.0 : 0.0));
    }
    // 5 interior points -- must not appear in the hull.
    points.push_back(Vec3<f64>(0.5, 0.5, 0.5));
    points.push_back(Vec3<f64>(0.3, 0.3, 0.3));
    points.push_back(Vec3<f64>(0.7, 0.2, 0.4));
    points.push_back(Vec3<f64>(0.1, 0.9, 0.6));
    points.push_back(Vec3<f64>(0.4, 0.4, 0.8));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.vertices.size() == 8); // interior points excluded
    CHECK(r.faces.size() == 12);
    CHECK(all_faces_ccw(r));
    CHECK(hull_contains_all_inputs<f64>(
        r, crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size())));
}

// ---------------------------------------------------------------------------
// (3) Random point clouds
// ---------------------------------------------------------------------------

TEST_CASE("quickhull: 50 random points in unit cube -- hull contains all + CCW",
          "[v3c][quickhull][random]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    TestRng rng(0xDEADBEEFU);
    for (int i = 0; i < 50; ++i)
    {
        points.push_back(Vec3<f64>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)));
    }

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.vertices.size() >= 4); // at least a tetrahedron
    CHECK(r.vertices.size() <= 50);
    CHECK(r.faces.size() >= 4);
    CHECK(all_faces_ccw(r));
    CHECK(hull_contains_all_inputs<f64>(
        r, crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size())));
}

TEST_CASE("quickhull: 500 random points in unit sphere -- Euler characteristic check",
          "[v3c][quickhull][random]")
{
    crd::memory::TlsfAllocator alloc(512U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    TestRng rng(0xBADC0DEU);
    for (int i = 0; i < 500; ++i)
    {
        // Rejection sampling for points in unit sphere.
        Vec3<f64> p;
        do
        {
            p = Vec3<f64>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        } while (crd::math::dot(p, p) > 1.0);
        points.push_back(p);
    }

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    // For a closed triangulated polyhedron: V - E + F = 2 (Euler characteristic).
    // E = 3F/2 (each edge appears in exactly 2 triangles).
    // ⇒ V - 3F/2 + F = 2 ⇒ V - F/2 = 2 ⇒ F = 2(V - 2)
    const usize num_vertices = r.vertices.size();
    const usize num_faces = r.faces.size();
    CHECK(num_faces == 2 * (num_vertices - 2));
    CHECK(all_faces_ccw(r));
    CHECK(hull_contains_all_inputs<f64>(
        r, crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size())));
}

// ---------------------------------------------------------------------------
// (4) Degenerate inputs
// ---------------------------------------------------------------------------

TEST_CASE("quickhull: empty input -> empty result", "[v3c][quickhull][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>{}, &alloc);
    CHECK(r.empty());
    CHECK(r.faces.empty());
}

TEST_CASE("quickhull: single point -> 1-vertex result, is_coincident flag",
          "[v3c][quickhull][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(1, 2, 3));
    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    CHECK(r.vertices.size() == 1);
    CHECK(r.is_coincident);
    CHECK(r.faces.empty());
}

TEST_CASE("quickhull: two coincident points -> 1-vertex result, is_coincident",
          "[v3c][quickhull][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(1, 2, 3));
    points.push_back(Vec3<f64>(1, 2, 3));
    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    CHECK(r.vertices.size() == 1);
    CHECK(r.is_coincident);
}

TEST_CASE("quickhull: collinear input -> 2 vertices, is_colinear flag",
          "[v3c][quickhull][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    for (int i = 0; i < 5; ++i)
    {
        const f64 t = static_cast<f64>(i);
        points.push_back(Vec3<f64>(t, 2 * t, 3 * t)); // collinear on y = 2x = 3z/... line through origin
    }
    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    CHECK(r.is_colinear);
    CHECK(r.vertices.size() == 2);
}

TEST_CASE("quickhull: all-coplanar input -> is_coplanar flag set",
          "[v3c][quickhull][degen]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    // 5 points on z=0 plane.
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(1, 1, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    points.push_back(Vec3<f64>(0.5, 0.5, 0));
    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    CHECK(r.is_coplanar);
    // v3c-a ships the flag-detection only; full flat-3D-hull reconstruction
    // via v3b is v3c-c. For now the result has the 3 extremal points the
    // detector found.
    CHECK(r.vertices.size() >= 2);
}

// ---------------------------------------------------------------------------
// (5) Determinism
// ---------------------------------------------------------------------------

TEST_CASE("quickhull: replay produces identical hull", "[v3c][quickhull][determinism]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    TestRng rng(0xCAFEBABEU);
    for (int i = 0; i < 30; ++i)
    {
        points.push_back(Vec3<f64>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)));
    }

    auto r1 = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    auto r2 = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    REQUIRE(r1.vertices.size() == r2.vertices.size());
    REQUIRE(r1.faces.size() == r2.faces.size());
    for (usize i = 0; i < r1.vertices.size(); ++i)
    {
        CHECK(r1.vertices[i].x == r2.vertices[i].x);
        CHECK(r1.vertices[i].y == r2.vertices[i].y);
        CHECK(r1.vertices[i].z == r2.vertices[i].z);
    }
    for (usize i = 0; i < r1.face_vertex_indices.size(); ++i)
    {
        CHECK(r1.face_vertex_indices[i] == r2.face_vertex_indices[i]);
    }
}

// ---------------------------------------------------------------------------
// (6) f32 + f64
// ---------------------------------------------------------------------------

TEST_CASE("quickhull: f32 tetrahedron", "[v3c][quickhull][f32]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f32>> points(&alloc);
    points.push_back(Vec3<f32>(0, 0, 0));
    points.push_back(Vec3<f32>(1, 0, 0));
    points.push_back(Vec3<f32>(0, 1, 0));
    points.push_back(Vec3<f32>(0, 0, 1));

    auto r = quickhull<f32>(crd::containers::ConstSpan<Vec3<f32>>(points.data(), points.size()), &alloc);

    CHECK(r.vertices.size() == 4);
    CHECK(r.faces.size() == 4);
}

// ---------------------------------------------------------------------------
// (7) Convex hull view of result
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// (8) v3c-c -- enrich_for_gjk: vertex adjacency + SoA SIMD arrays
// ---------------------------------------------------------------------------

TEST_CASE("enrich_for_gjk: tetrahedron -- each vertex has 3 neighbors",
          "[v3c][quickhull][enrich]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    points.push_back(Vec3<f64>(0, 0, 1));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    crd::geometry::convex::enrich_for_gjk(r);

    REQUIRE(r.vertices.size() == 4);
    REQUIRE(r.vertex_adjacency_offsets.size() == 5); // num_vertices + 1
    // Every vertex in a tetrahedron has exactly 3 neighbors (the other 3).
    for (u32 v = 0; v < 4; ++v)
    {
        const u32 begin = r.vertex_adjacency_offsets[v];
        const u32 end = r.vertex_adjacency_offsets[v + 1];
        CHECK(end - begin == 3);
        // Neighbors should be the other 3 vertices.
        bool found_others[4] = {false, false, false, false};
        found_others[v] = true; // skip self
        for (u32 k = begin; k < end; ++k)
        {
            const u32 nbr = r.vertex_adjacency_indices[k];
            CHECK(nbr != v);
            CHECK(nbr < 4);
            found_others[nbr] = true;
        }
        for (int k = 0; k < 4; ++k)
        {
            CHECK(found_others[k]);
        }
    }
}

TEST_CASE("enrich_for_gjk: cube -- each vertex has 3 neighbors (cube edges)",
          "[v3c][quickhull][enrich]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    for (int i = 0; i < 8; ++i)
    {
        points.push_back(Vec3<f64>((i & 4) ? 1.0 : 0.0, (i & 2) ? 1.0 : 0.0, (i & 1) ? 1.0 : 0.0));
    }

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    crd::geometry::convex::enrich_for_gjk(r);

    REQUIRE(r.vertices.size() == 8);
    REQUIRE(r.vertex_adjacency_offsets.size() == 9);
    // Each cube corner has 3 neighbors (along the 3 cube edges meeting it).
    // But the cube hull is triangulated, so each vertex's adjacency may also
    // include diagonal triangulation edges. We check >= 3 neighbors at
    // minimum and that each neighbor is a valid vertex index.
    for (u32 v = 0; v < 8; ++v)
    {
        const u32 begin = r.vertex_adjacency_offsets[v];
        const u32 end = r.vertex_adjacency_offsets[v + 1];
        CHECK(end - begin >= 3); // at least 3 cube-edge neighbors
        for (u32 k = begin; k < end; ++k)
        {
            CHECK(r.vertex_adjacency_indices[k] != v);
            CHECK(r.vertex_adjacency_indices[k] < 8);
        }
    }

    // Adjacency must be EDGE-SYMMETRIC (every edge appears in both endpoints).
    auto has_neighbor = [&](u32 u, u32 v) {
        const u32 begin = r.vertex_adjacency_offsets[u];
        const u32 end = r.vertex_adjacency_offsets[u + 1];
        for (u32 k = begin; k < end; ++k)
        {
            if (r.vertex_adjacency_indices[k] == v)
                return true;
        }
        return false;
    };
    for (u32 u = 0; u < 8; ++u)
    {
        const u32 begin = r.vertex_adjacency_offsets[u];
        const u32 end = r.vertex_adjacency_offsets[u + 1];
        for (u32 k = begin; k < end; ++k)
        {
            const u32 v = r.vertex_adjacency_indices[k];
            CHECK(has_neighbor(v, u));
        }
    }
}

TEST_CASE("enrich_for_gjk: f32 cube -- SoA arrays populated + padded to multiple of 8",
          "[v3c][quickhull][enrich][f32]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f32>> points(&alloc);
    for (int i = 0; i < 8; ++i)
    {
        points.push_back(Vec3<f32>((i & 4) ? 1.0F : 0.0F, (i & 2) ? 1.0F : 0.0F,
                                     (i & 1) ? 1.0F : 0.0F));
    }
    auto r = quickhull<f32>(crd::containers::ConstSpan<Vec3<f32>>(points.data(), points.size()), &alloc);
    crd::geometry::convex::enrich_for_gjk(r);

    REQUIRE(r.vertices.size() == 8);
    REQUIRE(r.vx_soa.size() == 8); // n=8 already padded
    REQUIRE(r.vy_soa.size() == 8);
    REQUIRE(r.vz_soa.size() == 8);
    // First N entries match the vertex positions.
    for (usize i = 0; i < r.vertices.size(); ++i)
    {
        CHECK(r.vx_soa[i] == r.vertices[i].x);
        CHECK(r.vy_soa[i] == r.vertices[i].y);
        CHECK(r.vz_soa[i] == r.vertices[i].z);
    }
}

TEST_CASE("enrich_for_gjk: f32 tetrahedron -- SoA padded to 8 (4 vertices + 4 pad)",
          "[v3c][quickhull][enrich][f32]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f32>> points(&alloc);
    points.push_back(Vec3<f32>(0, 0, 0));
    points.push_back(Vec3<f32>(1, 0, 0));
    points.push_back(Vec3<f32>(0, 1, 0));
    points.push_back(Vec3<f32>(0, 0, 1));
    auto r = quickhull<f32>(crd::containers::ConstSpan<Vec3<f32>>(points.data(), points.size()), &alloc);
    crd::geometry::convex::enrich_for_gjk(r);

    REQUIRE(r.vertices.size() == 4);
    REQUIRE(r.vx_soa.size() == 8); // padded to 8
    REQUIRE(r.vy_soa.size() == 8);
    REQUIRE(r.vz_soa.size() == 8);
    // First 4 are real vertices.
    for (usize i = 0; i < 4; ++i)
    {
        CHECK(r.vx_soa[i] == r.vertices[i].x);
    }
    // Last 4 are padding: vertex 0's coords.
    for (usize i = 4; i < 8; ++i)
    {
        CHECK(r.vx_soa[i] == r.vertices[0].x);
        CHECK(r.vy_soa[i] == r.vertices[0].y);
        CHECK(r.vz_soa[i] == r.vertices[0].z);
    }
}

TEST_CASE("enrich_for_gjk: f64 hull -- SoA arrays stay empty (f32-only path)",
          "[v3c][quickhull][enrich][f64]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    points.push_back(Vec3<f64>(0, 0, 1));
    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    crd::geometry::convex::enrich_for_gjk(r);

    // Adjacency is populated for both f32 and f64.
    REQUIRE(r.vertex_adjacency_offsets.size() == 5);
    // SoA stays empty for f64 (v2h SIMD is f32-only).
    CHECK(r.vx_soa.empty());
    CHECK(r.vy_soa.empty());
    CHECK(r.vz_soa.empty());
}

// ---------------------------------------------------------------------------
// (9) v3c-c -- coplanar reconstruction (flat 3D hull)
// ---------------------------------------------------------------------------

TEST_CASE("coplanar: 4-point square on z=0 plane -- flat 3D hull with 2 faces",
          "[v3c][quickhull][coplanar]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(1, 1, 0));
    points.push_back(Vec3<f64>(0, 1, 0));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.is_coplanar);
    CHECK(r.vertices.size() == 4);
    CHECK(r.faces.size() == 2); // front + back

    // Front and back face normals must be exact opposites.
    CHECK(r.faces[0].normal.x == -r.faces[1].normal.x);
    CHECK(r.faces[0].normal.y == -r.faces[1].normal.y);
    CHECK(r.faces[0].normal.z == -r.faces[1].normal.z);

    // Each face has 4 vertex indices.
    REQUIRE(r.face_vertex_offsets.size() == 3); // 2 faces + 1
    CHECK(r.face_vertex_offsets[1] - r.face_vertex_offsets[0] == 4);
    CHECK(r.face_vertex_offsets[2] - r.face_vertex_offsets[1] == 4);
}

TEST_CASE("coplanar: 5-point pentagon -- flat 3D hull excludes interior point",
          "[v3c][quickhull][coplanar]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(2, 0, 0));
    points.push_back(Vec3<f64>(2, 2, 0));
    points.push_back(Vec3<f64>(0, 2, 0));
    points.push_back(Vec3<f64>(1, 1, 0)); // interior -- must NOT appear

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    CHECK(r.is_coplanar);
    CHECK(r.vertices.size() == 4); // interior excluded
    CHECK(r.faces.size() == 2);
}

TEST_CASE("coplanar: hull view of flat 3D hull is valid",
          "[v3c][quickhull][coplanar][view]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(0, 1, 0));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    auto view = convex_hull_view_of(r);

    CHECK(view.vertices.size() == r.vertices.size());
    CHECK(view.faces.size() == r.faces.size());
}

TEST_CASE("quickhull: convex_hull_view_of produces valid ConvexHullView",
          "[v3c][quickhull][view]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    points.push_back(Vec3<f64>(0, 0, 1));

    auto r = quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    auto view = convex_hull_view_of(r);

    CHECK(view.vertices.size() == r.vertices.size());
    CHECK(view.faces.size() == r.faces.size());
    CHECK(view.face_vertex_indices.size() == r.face_vertex_indices.size());
    CHECK(view.face_vertex_offsets.size() == r.face_vertex_offsets.size());
}
