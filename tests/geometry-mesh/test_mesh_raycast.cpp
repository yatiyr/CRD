// crd-geometry-mesh v4b — mesh_raycast tests.
//
// Verifies the BVH-accelerated mesh raycast produces correct nearest-hit
// results, with Woop watertight on-leaf testing + deterministic lowest-
// triangle-index tiebreak on equal t.

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh/mesh.hpp>
#include <crd/geometry/mesh/mesh_queries_typed.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using crd::geometry::mesh::build_triangle_mesh_bvh;
using crd::geometry::mesh::mesh_raycast;
using crd::geometry::mesh::MeshHitPayload;
using crd::geometry::mesh::MeshRayHit;
using crd::geometry::mesh::TriangleMeshBvh;
using crd::geometry::mesh::TriangleMeshViewf;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3;
using crd::math::Vec3f;
using crd::units::Length32;

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
} // namespace

TEST_CASE("v4b empty mesh returns nullopt", "[geometry-mesh][v4b][raycast]")
{
    crd::memory::MallocAllocator alloc;
    const TriangleMeshViewf empty_view{};
    const TriangleMeshBvh empty_bvh{&alloc};
    const Ray3<crd::f32> ray{Vec3f{0.0F, 0.0F, -5.0F}, Vec3f{0.0F, 0.0F, 1.0F}};
    const auto r = mesh_raycast(empty_view, empty_bvh, ray);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("v4b single triangle: ray hits at parametric t=1",
          "[geometry-mesh][v4b][raycast]")
{
    crd::memory::MallocAllocator alloc;
    const Vec3f verts[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 2U};
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    // Ray from (0.25, 0.25, 1) shooting down through the triangle plane.
    const Ray3<crd::f32> ray{Vec3f{0.25F, 0.25F, 1.0F}, Vec3f{0.0F, 0.0F, -1.0F}};
    const auto r = mesh_raycast(view, bvh, ray);
    REQUIRE(r.has_value());
    REQUIRE(r->t == Catch::Approx(1.0F));
    REQUIRE(r->payload.tri == 0U);
}

TEST_CASE("v4b unit cube: ray from +X direction hits +X face at t=4.5",
          "[geometry-mesh][v4b][raycast]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    // Ray from (5, 0, 0) heading in -X direction, hits +X face of unit cube
    // (face at x=0.5) at t = 5 - 0.5 = 4.5.
    const Ray3<crd::f32> ray{Vec3f{5.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    const auto r = mesh_raycast(view, bvh, ray);
    REQUIRE(r.has_value());
    REQUIRE(r->t == Catch::Approx(4.5F));
}

TEST_CASE("v4b ray missing the mesh returns nullopt",
          "[geometry-mesh][v4b][raycast]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    // Ray parallel to the cube, well above it.
    const Ray3<crd::f32> ray{Vec3f{-5.0F, 5.0F, 0.0F}, Vec3f{1.0F, 0.0F, 0.0F}};
    const auto r = mesh_raycast(view, bvh, ray);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("v4b tmax culls hits beyond the parametric window",
          "[geometry-mesh][v4b][raycast][tmax]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{5.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    REQUIRE_FALSE(mesh_raycast(view, bvh, ray, 4.0F).has_value()); // 4.0 < 4.5
    REQUIRE      (mesh_raycast(view, bvh, ray, 5.0F).has_value()); // 5.0 > 4.5
}

TEST_CASE("v4b cull_back excludes back-face hits",
          "[geometry-mesh][v4b][raycast][cull]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    // Ray FROM INSIDE the cube going -X. Without cull_back, hits the back
    // (interior side) of the -X face at t=0.5. With cull_back, that face
    // is back-facing → skipped → no hit.
    const Ray3<crd::f32> ray{Vec3f{0.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    const auto r_with_back = mesh_raycast(view, bvh, ray, std::numeric_limits<crd::f32>::infinity(), /*cull_back=*/false);
    REQUIRE(r_with_back.has_value());
    REQUIRE(r_with_back->t == Catch::Approx(0.5F));

    const auto r_cull = mesh_raycast(view, bvh, ray, std::numeric_limits<crd::f32>::infinity(), /*cull_back=*/true);
    REQUIRE_FALSE(r_cull.has_value());
}

TEST_CASE("v4b barycentrics sum to ~1 on a hit",
          "[geometry-mesh][v4b][raycast][barycentric]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(view, &alloc);

    const Ray3<crd::f32> ray{Vec3f{5.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    const auto r = mesh_raycast(view, bvh, ray);
    REQUIRE(r.has_value());
    const Vec3f& b = r->payload.bary;
    REQUIRE((b.x + b.y + b.z) == Catch::Approx(1.0F).margin(1.0e-5F));
}

TEST_CASE("v4b typed Quantity wrapper bridges through raw algorithm",
          "[geometry-mesh][v4b][typed][units]")
{
    // ADR-0078 §5 D32 — typed surface lives one layer above the raw algorithm.
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf raw_view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const auto bvh = build_triangle_mesh_bvh(raw_view, &alloc);

    crd::geometry::mesh::TriangleMeshViewT<crd::units::dim::Length, crd::f32> typed_view{
        crd::containers::ConstSpan<crd::math::Vec3<Length32>>{},
        raw_view.indices};
    const auto raw_vert_span = crd::containers::ConstSpan<Vec3f>{
        cube.vertices.data(), cube.vertices.size()};

    crd::geometry::mesh::Ray3T<crd::units::dim::Length, crd::f32> typed_ray{};
    typed_ray.origin    = crd::math::Vec3<Length32>{Length32{5.0F}, Length32{0.0F}, Length32{0.0F}};
    typed_ray.direction = Vec3f{-1.0F, 0.0F, 0.0F};

    const auto r = crd::geometry::mesh::mesh_raycast(typed_view, raw_vert_span, bvh, typed_ray);
    REQUIRE(r.has_value());
    REQUIRE(r->t.value == Catch::Approx(4.5F)); // distance in metres
}
