// crd-geometry-mesh v4a — closest_point(TriangleMesh, p) tests.
//
// Verifies the BVH-accelerated mesh closest-point query produces the same
// result as a brute-force per-triangle scan (Ericson §5.1.5 closest-point
// on the nearest triangle), plus the typed Quantity wrappers per ADR-0078
// §5 D27/D32.

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh/mesh.hpp>
#include <crd/geometry/mesh/mesh_queries_typed.hpp>
#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using crd::geometry::mesh::build_triangle_mesh_bvh;
using crd::geometry::mesh::mesh_closest_point;
using crd::geometry::mesh::TriangleMeshBvh;
using crd::geometry::mesh::TriangleMeshViewf;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3;
using crd::math::Vec3f;
using crd::units::Length32;

namespace
{
// Brute-force reference: scan every triangle, return min-squared-distance
// + winning index (lowest-index tiebreak on ties).
struct BruteResult { Vec3f point; crd::f32 dsq; crd::u32 tri; };
BruteResult brute_closest(const TriangleMeshViewf& view, const Vec3f& q) noexcept
{
    BruteResult best{Vec3f{}, std::numeric_limits<crd::f32>::infinity(), 0xFFFFFFFFU};
    for (crd::u32 ti = 0U; ti < view.triangle_count(); ++ti)
    {
        const Triangle3<crd::f32> tri{
            view.vertices[view.indices[ti * 3U + 0U]],
            view.vertices[view.indices[ti * 3U + 1U]],
            view.vertices[view.indices[ti * 3U + 2U]]};
        const Vec3f cp = crd::geometry::primitives::closest_point(tri, q);
        const Vec3f d = cp - q;
        const crd::f32 dsq = d.x * d.x + d.y * d.y + d.z * d.z;
        if (dsq < best.dsq || (dsq == best.dsq && ti < best.tri))
        {
            best = BruteResult{cp, dsq, ti};
        }
    }
    return best;
}

// Build a unit-cube mesh (12 triangles, 8 vertices) centered at origin.
struct CubeMesh
{
    crd::containers::Array<Vec3f>   vertices;
    crd::containers::Array<crd::u32> indices;
    explicit CubeMesh(crd::memory::IAllocator* a) : vertices(a), indices(a) {}
};

CubeMesh make_cube(crd::memory::IAllocator* a, crd::f32 half = 0.5F)
{
    CubeMesh m{a};
    m.vertices.reserve(8);
    for (int i = 0; i < 8; ++i)
    {
        m.vertices.push_back(Vec3f{
            (i & 1) ? half : -half,
            (i & 2) ? half : -half,
            (i & 4) ? half : -half});
    }
    // 12 triangles — 2 per face, 6 faces. CCW outward.
    const crd::u32 idx[36] = {
        0, 2, 1,  1, 2, 3,  // -Z
        4, 5, 6,  5, 7, 6,  // +Z
        0, 1, 4,  1, 5, 4,  // -Y
        2, 6, 3,  3, 6, 7,  // +Y
        0, 4, 2,  2, 4, 6,  // -X
        1, 3, 5,  3, 7, 5   // +X
    };
    m.indices.reserve(36);
    for (crd::u32 j = 0U; j < 36U; ++j) { m.indices.push_back(idx[j]); }
    return m;
}
} // namespace

TEST_CASE("v4a empty mesh returns nullopt", "[geometry-mesh][v4a][closest_point]")
{
    crd::memory::MallocAllocator alloc;
    const TriangleMeshViewf empty_view{};
    const TriangleMeshBvh empty_bvh{&alloc};
    const auto r = mesh_closest_point(empty_view, empty_bvh, Vec3f{0.0F, 0.0F, 0.0F});
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("v4a single triangle: query above centroid -> centroid",
          "[geometry-mesh][v4a][closest_point]")
{
    crd::memory::MallocAllocator alloc;
    const crd::math::Vec3f verts[3] = {
        {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 2U};
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Vec3f q{1.0F / 3.0F, 1.0F / 3.0F, 2.0F};
    const auto r = mesh_closest_point(view, bvh, q);
    REQUIRE(r.has_value());
    REQUIRE(r->point.x == Catch::Approx(1.0F / 3.0F));
    REQUIRE(r->point.y == Catch::Approx(1.0F / 3.0F));
    REQUIRE(r->point.z == Catch::Approx(0.0F));
    REQUIRE(r->distance_squared == Catch::Approx(4.0F)); // 2.0² = 4.0
    REQUIRE(r->payload == 0U);
}

TEST_CASE("v4a unit cube: query outside +X face returns face point",
          "[geometry-mesh][v4a][closest_point]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Vec3f q{5.0F, 0.0F, 0.0F};
    const auto r = mesh_closest_point(view, bvh, q);
    REQUIRE(r.has_value());
    // Closest point on the unit cube from (5, 0, 0) lies on the +X face at
    // (0.5, 0, 0). Distance² = 4.5² = 20.25.
    REQUIRE(r->point.x == Catch::Approx(0.5F));
    REQUIRE(r->point.y == Catch::Approx(0.0F));
    REQUIRE(r->point.z == Catch::Approx(0.0F));
    REQUIRE(r->distance_squared == Catch::Approx(20.25F));
}

TEST_CASE("v4a BVH result matches brute-force across a corpus of query points",
          "[geometry-mesh][v4a][closest_point][correctness]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    // 27 query points spanning [-2, 2]^3.
    int matches = 0;
    int probes = 0;
    for (int xi = -2; xi <= 2; ++xi)
    for (int yi = -2; yi <= 2; ++yi)
    for (int zi = -2; zi <= 2; ++zi)
    {
        const Vec3f q{static_cast<crd::f32>(xi), static_cast<crd::f32>(yi), static_cast<crd::f32>(zi)};
        const auto bvh_r = mesh_closest_point(view, bvh, q);
        const auto brute_r = brute_closest(view, q);
        ++probes;
        if (bvh_r.has_value())
        {
            CHECK(bvh_r->distance_squared == Catch::Approx(brute_r.dsq).margin(1.0e-5F));
            ++matches;
        }
    }
    REQUIRE(matches == probes);
}

TEST_CASE("v4a max_dist culls hits beyond the radius",
          "[geometry-mesh][v4a][closest_point][max_dist]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Vec3f q{5.0F, 0.0F, 0.0F};
    // Cube +X face is at x=0.5, so closest distance from (5,0,0) is 4.5.
    REQUIRE_FALSE(mesh_closest_point(view, bvh, q, 4.0F).has_value());   // 4.0 < 4.5
    REQUIRE      (mesh_closest_point(view, bvh, q, 5.0F).has_value());   // 5.0 > 4.5
}

TEST_CASE("v4a typed Quantity wrapper bridges through raw algorithm",
          "[geometry-mesh][v4a][typed][units]")
{
    // ADR-0078 §5 D32 — typed surface lives one layer above the raw algorithm.
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf raw_view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(raw_view, &alloc);

    // Caller's typed query point: (5, 0, 0) m.
    const crd::math::Vec3<Length32> typed_query{
        Length32{5.0F}, Length32{0.0F}, Length32{0.0F}};

    crd::geometry::mesh::TriangleMeshViewT<crd::units::dim::Length, crd::f32> typed_view{
        crd::containers::ConstSpan<crd::math::Vec3<Length32>>{}, // typed view's vertices
        raw_view.indices};
    // The typed wrapper accepts a raw vertex span (the bridge); same layout
    // by ADR-0078 §1 D2 (sizeof(Vec3<Length32>) == sizeof(Vec3f)).
    const auto raw_vert_span = crd::containers::ConstSpan<Vec3f>{
        cube.vertices.data(), cube.vertices.size()};

    const auto r = crd::geometry::mesh::mesh_closest_point(typed_view, raw_vert_span, bvh, typed_query);
    REQUIRE(r.has_value());
    // Closest point in metres = (0.5, 0, 0); distance² in m² = 20.25.
    REQUIRE(r->point.x.value == Catch::Approx(0.5F));
    REQUIRE(r->point.y.value == Catch::Approx(0.0F));
    REQUIRE(r->point.z.value == Catch::Approx(0.0F));
    REQUIRE(r->distance_squared.value == Catch::Approx(20.25F));
}
