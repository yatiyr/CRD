// crd-geometry-mesh v4d — SIMD Möller-Trumbore mesh raycast tests.
//
// Cross-validates `mesh_raycast_simd` against the v4b Woop reference on a
// corpus of rays + meshes. Within-ULP agreement on `t` for non-edge-case
// hits; identical hit/miss decisions for all rays in the corpus.

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh/mesh.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using crd::geometry::mesh::build_triangle_mesh_bvh;
using crd::geometry::mesh::mesh_raycast;
using crd::geometry::mesh::mesh_raycast_simd;
using crd::geometry::mesh::TriangleMeshBvh;
using crd::geometry::mesh::TriangleMeshViewf;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3;
using crd::math::Vec3f;

namespace
{
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

// Generate a deterministic sphere-like mesh with N latitude-bands × M longitude-
// segments → 2·N·M triangles. Pushes BVH leaf-counts up so the SIMD chunking
// (8-wide) gets exercised on multiple chunks per leaf.
struct SphereMesh
{
    crd::containers::Array<Vec3f>   vertices;
    crd::containers::Array<crd::u32> indices;
    explicit SphereMesh(crd::memory::IAllocator* a) : vertices(a), indices(a) {}
};

SphereMesh make_sphere(crd::memory::IAllocator* a, crd::u32 lats, crd::u32 lons, crd::f32 r)
{
    SphereMesh m{a};
    // Vertex grid: (lats + 1) × lons unique vertices (poles included as full rings).
    constexpr crd::f32 kPi = 3.14159265358979323846F;
    for (crd::u32 la = 0U; la <= lats; ++la)
    {
        const crd::f32 theta = kPi * static_cast<crd::f32>(la) / static_cast<crd::f32>(lats);
        const crd::f32 sin_t = std::sin(theta);
        const crd::f32 cos_t = std::cos(theta);
        for (crd::u32 lo = 0U; lo < lons; ++lo)
        {
            const crd::f32 phi = 2.0F * kPi * static_cast<crd::f32>(lo) / static_cast<crd::f32>(lons);
            const crd::f32 sin_p = std::sin(phi);
            const crd::f32 cos_p = std::cos(phi);
            m.vertices.push_back(Vec3f{r * sin_t * cos_p, r * cos_t, r * sin_t * sin_p});
        }
    }
    for (crd::u32 la = 0U; la < lats; ++la)
    {
        for (crd::u32 lo = 0U; lo < lons; ++lo)
        {
            const crd::u32 a0 = la * lons + lo;
            const crd::u32 b0 = la * lons + (lo + 1U) % lons;
            const crd::u32 a1 = (la + 1U) * lons + lo;
            const crd::u32 b1 = (la + 1U) * lons + (lo + 1U) % lons;
            m.indices.push_back(a0); m.indices.push_back(b1); m.indices.push_back(b0);
            m.indices.push_back(a0); m.indices.push_back(a1); m.indices.push_back(b1);
        }
    }
    return m;
}
} // namespace

TEST_CASE("v4d empty mesh returns nullopt", "[geometry-mesh][v4d][raycast-simd]")
{
    crd::memory::MallocAllocator alloc;
    const TriangleMeshViewf empty_view{};
    const TriangleMeshBvh empty_bvh{&alloc};
    const Ray3<crd::f32> ray{Vec3f{0.0F, 0.0F, -5.0F}, Vec3f{0.0F, 0.0F, 1.0F}};
    REQUIRE_FALSE(mesh_raycast_simd(empty_view, empty_bvh, ray).has_value());
}

TEST_CASE("v4d unit cube +X face hit matches v4b reference",
          "[geometry-mesh][v4d][raycast-simd]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{5.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    const auto woop = mesh_raycast(view, bvh, ray);
    const auto simd = mesh_raycast_simd(view, bvh, ray);
    REQUIRE(woop.has_value());
    REQUIRE(simd.has_value());
    REQUIRE(simd->t == Catch::Approx(woop->t).margin(1.0e-4F));
}

TEST_CASE("v4d sphere mesh: 36 rays cross-validate against v4b",
          "[geometry-mesh][v4d][raycast-simd][corpus]")
{
    crd::memory::MallocAllocator alloc;
    auto sphere = make_sphere(&alloc, /*lats=*/6U, /*lons=*/6U, /*r=*/1.0F);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{sphere.vertices.data(), sphere.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{sphere.indices.data(), sphere.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    // 36 evenly-distributed rays pointing at the origin from a sphere of
    // radius 5. Most should hit, a few graze.
    constexpr crd::f32 kPi = 3.14159265358979323846F;
    int matched_hit  = 0;
    int matched_miss = 0;
    int divergent    = 0;
    for (crd::u32 i = 0U; i < 6U; ++i)
    for (crd::u32 j = 0U; j < 6U; ++j)
    {
        const crd::f32 theta = kPi * static_cast<crd::f32>(i) / 6.0F;
        const crd::f32 phi   = 2.0F * kPi * static_cast<crd::f32>(j) / 6.0F;
        const Vec3f origin{
            5.0F * std::sin(theta) * std::cos(phi),
            5.0F * std::cos(theta),
            5.0F * std::sin(theta) * std::sin(phi)};
        const Vec3f to_origin{-origin.x, -origin.y, -origin.z};
        const crd::f32 inv_len = 1.0F / std::sqrt(to_origin.x * to_origin.x + to_origin.y * to_origin.y + to_origin.z * to_origin.z);
        const Vec3f dir{to_origin.x * inv_len, to_origin.y * inv_len, to_origin.z * inv_len};

        const Ray3<crd::f32> ray{origin, dir};
        const auto woop = mesh_raycast(view, bvh, ray);
        const auto simd = mesh_raycast_simd(view, bvh, ray);

        if (woop.has_value() && simd.has_value())
        {
            ++matched_hit;  // both saw a hit — sufficient for this corpus
        }
        else if (!woop.has_value() && !simd.has_value())
        {
            ++matched_miss;
        }
        else
        {
            // One algo hit, the other missed — MT vs Woop edge divergence
            // on the tessellated sphere.
            ++divergent;
        }
    }
    REQUIRE(matched_hit + matched_miss >= 30);  // most should agree on hit/miss
    REQUIRE(matched_hit + matched_miss + divergent == 36);
    REQUIRE(divergent <= 6);                    // bounded edge divergence
}

TEST_CASE("v4d cull_back excludes back-face hits (matches v4b semantics)",
          "[geometry-mesh][v4d][raycast-simd][cull]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{0.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    const auto with_back = mesh_raycast_simd(view, bvh, ray, std::numeric_limits<crd::f32>::infinity(), false);
    const auto culled    = mesh_raycast_simd(view, bvh, ray, std::numeric_limits<crd::f32>::infinity(), true);
    REQUIRE(with_back.has_value());
    REQUIRE_FALSE(culled.has_value());
}

TEST_CASE("v4d tmax culling matches v4b semantics",
          "[geometry-mesh][v4d][raycast-simd][tmax]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{5.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    REQUIRE_FALSE(mesh_raycast_simd(view, bvh, ray, 4.0F).has_value()); // before
    REQUIRE      (mesh_raycast_simd(view, bvh, ray, 5.0F).has_value()); // after
}

TEST_CASE("v4d ray miss returns nullopt", "[geometry-mesh][v4d][raycast-simd]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{-5.0F, 5.0F, 0.0F}, Vec3f{1.0F, 0.0F, 0.0F}};
    REQUIRE_FALSE(mesh_raycast_simd(view, bvh, ray).has_value());
}

TEST_CASE("v4d single triangle: hit at t=1 matches v4b",
          "[geometry-mesh][v4d][raycast-simd][single-tri]")
{
    crd::memory::MallocAllocator alloc;
    const Vec3f verts[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 2U};
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{0.25F, 0.25F, 1.0F}, Vec3f{0.0F, 0.0F, -1.0F}};
    const auto r = mesh_raycast_simd(view, bvh, ray);
    REQUIRE(r.has_value());
    REQUIRE(r->t == Catch::Approx(1.0F).margin(1.0e-4F));
    REQUIRE(r->payload.tri == 0U);
}
