// crd-geometry-mesh v4c — generalised winding number tests.
//
// Verifies Jacobson 2013 inside/outside on watertight + non-watertight meshes.

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh/mesh.hpp>
#include <crd/geometry/mesh/mesh_queries_typed.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using crd::geometry::mesh::mesh_is_inside;
using crd::geometry::mesh::mesh_winding_number;
using crd::geometry::mesh::TriangleMeshViewf;
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

// 12-triangle unit cube, CCW-outward. Same as v4a/b test fixture.
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

// Cube with the +X face removed → non-watertight (2 fewer triangles).
CubeMesh make_open_cube(crd::memory::IAllocator* a, crd::f32 half = 0.5F)
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
    // 10 triangles — drops the two +X face triangles.
    const crd::u32 idx[30] = {
        0, 2, 1,  1, 2, 3,  // -Z
        4, 5, 6,  5, 7, 6,  // +Z
        0, 1, 4,  1, 5, 4,  // -Y
        2, 6, 3,  3, 6, 7,  // +Y
        0, 4, 2,  2, 4, 6,  // -X
        // +X removed
    };
    m.indices.reserve(30);
    for (crd::u32 j = 0U; j < 30U; ++j) { m.indices.push_back(idx[j]); }
    return m;
}
} // namespace

TEST_CASE("v4c empty mesh -> winding 0", "[geometry-mesh][v4c][winding]")
{
    const TriangleMeshViewf empty_view{};
    const Vec3f q{0.0F, 0.0F, 0.0F};
    REQUIRE(mesh_winding_number(empty_view, q) == Catch::Approx(0.0F));
    REQUIRE_FALSE(mesh_is_inside(empty_view, q));
}

TEST_CASE("v4c watertight cube: origin returns w=1, far point returns w=0",
          "[geometry-mesh][v4c][winding][watertight]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};

    const crd::f32 w_inside = mesh_winding_number(view, Vec3f{0.0F, 0.0F, 0.0F});
    REQUIRE(w_inside == Catch::Approx(1.0F).margin(1e-3F));
    REQUIRE(mesh_is_inside(view, Vec3f{0.0F, 0.0F, 0.0F}));

    const crd::f32 w_outside = mesh_winding_number(view, Vec3f{5.0F, 0.0F, 0.0F});
    REQUIRE(w_outside == Catch::Approx(0.0F).margin(1e-3F));
    REQUIRE_FALSE(mesh_is_inside(view, Vec3f{5.0F, 0.0F, 0.0F}));
}

TEST_CASE("v4c cube: multiple interior queries all return w=1",
          "[geometry-mesh][v4c][winding][watertight]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};

    // Spray of interior points in [-0.4, 0.4]^3 (well inside the unit cube).
    for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 5; ++j)
    for (int k = 0; k < 5; ++k)
    {
        const crd::f32 x = -0.4F + 0.2F * static_cast<crd::f32>(i);
        const crd::f32 y = -0.4F + 0.2F * static_cast<crd::f32>(j);
        const crd::f32 z = -0.4F + 0.2F * static_cast<crd::f32>(k);
        const Vec3f q{x, y, z};
        const crd::f32 w = mesh_winding_number(view, q);
        REQUIRE(w == Catch::Approx(1.0F).margin(1e-2F));
        REQUIRE(mesh_is_inside(view, q));
    }
}

TEST_CASE("v4c cube: many exterior queries all return w=0",
          "[geometry-mesh][v4c][winding][watertight]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};

    // Points strictly outside [-0.5, 0.5]^3 (|coord| > 0.6 on at least one axis).
    const Vec3f outside_points[] = {
        {2.0F, 0.0F, 0.0F},
        {-2.0F, 0.0F, 0.0F},
        {0.0F, 2.0F, 0.0F},
        {0.0F, -2.0F, 0.0F},
        {0.0F, 0.0F, 2.0F},
        {0.0F, 0.0F, -2.0F},
        {1.0F, 1.0F, 1.0F},     // diagonal corner outside
        {-1.5F, 1.5F, 0.7F},    // arbitrary
    };
    for (const Vec3f& q : outside_points)
    {
        const crd::f32 w = mesh_winding_number(view, q);
        REQUIRE(w == Catch::Approx(0.0F).margin(1e-2F));
        REQUIRE_FALSE(mesh_is_inside(view, q));
    }
}

TEST_CASE("v4c Jacobson robustness: open cube (+X face removed) still classifies "
          "interior correctly", "[geometry-mesh][v4c][winding][robust]")
{
    // Jacobson 2013's key claim — winding number remains meaningful on
    // non-watertight meshes. Open cube has the +X face removed; an
    // interior point (origin) should still register near w=1, robust to
    // the missing face.
    crd::memory::GrowableTlsfAllocator alloc;
    auto open_cube = make_open_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{open_cube.vertices.data(), open_cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{open_cube.indices.data(), open_cube.indices.size()}};

    const crd::f32 w_interior = mesh_winding_number(view, Vec3f{0.0F, 0.0F, 0.0F});
    // With +X face missing, the +X subtended solid angle (≈ 4π/6 ≈ 2.09 sr)
    // is lost — so w_interior ≈ 1 - 1/6 ≈ 0.833. Still above the 0.5
    // threshold → classified as inside per Jacobson's robustness claim.
    REQUIRE(w_interior > 0.5F);
    REQUIRE(mesh_is_inside(view, Vec3f{0.0F, 0.0F, 0.0F}));
}

TEST_CASE("v4c Jacobson robustness: open cube classifies points far outside "
          "as outside", "[geometry-mesh][v4c][winding][robust]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    auto open_cube = make_open_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{open_cube.vertices.data(), open_cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{open_cube.indices.data(), open_cube.indices.size()}};

    REQUIRE_FALSE(mesh_is_inside(view, Vec3f{-5.0F, 0.0F, 0.0F})); // far -X
    REQUIRE_FALSE(mesh_is_inside(view, Vec3f{0.0F, 5.0F, 0.0F}));  // far +Y
    REQUIRE_FALSE(mesh_is_inside(view, Vec3f{0.0F, 0.0F, -5.0F})); // far -Z
}

TEST_CASE("v4c winding is invariant under uniform translation",
          "[geometry-mesh][v4c][winding][invariant]")
{
    // Translating mesh + query by the same vector preserves winding (it
    // depends only on relative geometry).
    crd::memory::GrowableTlsfAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};

    const crd::f32 w_origin = mesh_winding_number(view, Vec3f{0.1F, 0.1F, 0.1F});

    // Translate both by (+100, -200, +50). Build a translated copy.
    crd::containers::Array<Vec3f> moved{&alloc};
    moved.reserve(8);
    for (const Vec3f& v : cube.vertices)
    {
        moved.push_back(v + Vec3f{100.0F, -200.0F, 50.0F});
    }
    const TriangleMeshViewf moved_view{
        crd::containers::ConstSpan<Vec3f>{moved.data(), moved.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    const crd::f32 w_moved = mesh_winding_number(moved_view, Vec3f{100.1F, -199.9F, 50.1F});

    REQUIRE(w_moved == Catch::Approx(w_origin).margin(1e-2F));
}

TEST_CASE("v4c typed Quantity wrapper bridges through raw algorithm",
          "[geometry-mesh][v4c][typed][units]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf raw_view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    crd::geometry::mesh::TriangleMeshViewT<crd::units::dim::Length, crd::f32> typed_view{
        crd::containers::ConstSpan<crd::math::Vec3<Length32>>{},
        raw_view.indices};
    const auto raw_vert_span = crd::containers::ConstSpan<Vec3f>{
        cube.vertices.data(), cube.vertices.size()};

    const crd::math::Vec3<Length32> typed_query_inside{
        Length32{0.0F}, Length32{0.0F}, Length32{0.0F}};
    const crd::f32 w_inside = crd::geometry::mesh::mesh_winding_number(
        typed_view, raw_vert_span, typed_query_inside);
    REQUIRE(w_inside > 0.9F);
    REQUIRE(crd::geometry::mesh::mesh_is_inside(typed_view, raw_vert_span, typed_query_inside));

    const crd::math::Vec3<Length32> typed_query_outside{
        Length32{5.0F}, Length32{0.0F}, Length32{0.0F}};
    REQUIRE_FALSE(crd::geometry::mesh::mesh_is_inside(typed_view, raw_vert_span, typed_query_outside));
}
