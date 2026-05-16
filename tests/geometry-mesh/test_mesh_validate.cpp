// crd-geometry-mesh v4-validate — formal mesh validation tests.
//
// Each defect class gets its own focused fixture + assertion. Plus the
// watertight-cube reference case (no defects, watertight=true).

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh/mesh.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::geometry::mesh::MeshDefect;
using crd::geometry::mesh::MeshDefectKind;
using crd::geometry::mesh::MeshValidationOptions;
using crd::geometry::mesh::MeshValidationReport;
using crd::geometry::mesh::TriangleMeshViewf;
using crd::geometry::mesh::validate_triangle_mesh;
using crd::math::Vec3f;

namespace
{
// Canonical CCW-outward unit cube — 8 verts, 12 tris. Same fixture as v4a–v4d.
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

bool contains_defect(const MeshValidationReport& r, MeshDefectKind k) noexcept
{
    for (const MeshDefect& d : r.defects)
    {
        if (d.kind == k) { return true; }
    }
    return false;
}

crd::u32 count_defect(const MeshValidationReport& r, MeshDefectKind k) noexcept
{
    crd::u32 c = 0;
    for (const MeshDefect& d : r.defects)
    {
        if (d.kind == k) { ++c; }
    }
    return c;
}
} // namespace

TEST_CASE("v4-validate empty mesh: well_formed + non-watertight",
          "[geometry-mesh][v4-validate]")
{
    crd::memory::MallocAllocator alloc;
    const TriangleMeshViewf empty{};
    const auto r = validate_triangle_mesh(empty, &alloc);
    REQUIRE(r.triangle_count == 0U);
    REQUIRE(r.vertex_count   == 0U);
    REQUIRE(r.defects.empty());
    REQUIRE(r.well_formed);
    REQUIRE_FALSE(r.watertight); // empty surface is not watertight
}

TEST_CASE("v4-validate canonical cube is watertight + well_formed (no defects beyond boundary toggle)",
          "[geometry-mesh][v4-validate][watertight]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    MeshValidationOptions opts{};
    opts.report_boundary_edges = false; // cube has none; toggle off to keep defects empty
    const auto r = validate_triangle_mesh(view, &alloc, opts);
    REQUIRE(r.triangle_count == 12U);
    REQUIRE(r.vertex_count   == 8U);
    REQUIRE(r.manifold_edge_count == 18U); // 12 tris × 3 edges / 2 = 18 shared edges
    REQUIRE(r.boundary_edge_count == 0U);
    REQUIRE(r.non_manifold_edge_count == 0U);
    REQUIRE(r.defects.empty());
    REQUIRE(r.well_formed);
    REQUIRE(r.watertight);
}

TEST_CASE("v4-validate detects out-of-bounds index",
          "[geometry-mesh][v4-validate][out-of-bounds]")
{
    crd::memory::MallocAllocator alloc;
    const Vec3f verts[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 99U}; // 99 is out of bounds
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto r = validate_triangle_mesh(view, &alloc);
    REQUIRE(contains_defect(r, MeshDefectKind::OutOfBoundsIndex));
    REQUIRE_FALSE(r.well_formed);
}

TEST_CASE("v4-validate detects degenerate (repeated-vertex) triangle",
          "[geometry-mesh][v4-validate][degenerate]")
{
    crd::memory::MallocAllocator alloc;
    const Vec3f verts[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 1U}; // i1 == i2
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto r = validate_triangle_mesh(view, &alloc);
    REQUIRE(contains_defect(r, MeshDefectKind::DegenerateTriangle));
    REQUIRE_FALSE(r.well_formed);
}

TEST_CASE("v4-validate detects zero-area (collinear) triangle",
          "[geometry-mesh][v4-validate][zero-area]")
{
    crd::memory::MallocAllocator alloc;
    // 3 collinear vertices on the x-axis.
    const Vec3f verts[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 2U};
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto r = validate_triangle_mesh(view, &alloc);
    REQUIRE(contains_defect(r, MeshDefectKind::ZeroAreaTriangle));
    // Zero-area is an authoring smell, NOT a critical defect — `well_formed`
    // remains true (the surface topology is unaffected by area collapse).
    REQUIRE(r.well_formed);
}

TEST_CASE("v4-validate detects non-manifold edge (3 tris share one edge)",
          "[geometry-mesh][v4-validate][non-manifold]")
{
    crd::memory::MallocAllocator alloc;
    // 4 verts: 3 triangles all share edge (0, 1).
    const Vec3f verts[4] = {
        {0.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, 1.0F}};
    // Build a proper non-manifold: edge (0,1) shared by 3 distinct tris.
    const crd::u32 nm_inds[9] = {
        0, 1, 2,    // edge (0,1) appears: (0,1)
        0, 1, 3,    // edge (0,1) appears: (0,1)
        1, 0, 2,    // edge (0,1) appears: (1,0) — different tri (winding flipped)
    };
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 4U},
        crd::containers::ConstSpan<crd::u32>{nm_inds, 9U}};
    MeshValidationOptions opts{};
    opts.report_boundary_edges = false;
    const auto r = validate_triangle_mesh(view, &alloc, opts);
    REQUIRE(contains_defect(r, MeshDefectKind::NonManifoldEdge));
    REQUIRE(r.non_manifold_edge_count >= 1U);
    REQUIRE_FALSE(r.well_formed);
}

TEST_CASE("v4-validate detects boundary edges on open mesh",
          "[geometry-mesh][v4-validate][boundary]")
{
    crd::memory::MallocAllocator alloc;
    // Single triangle → all 3 edges are boundary.
    const Vec3f verts[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const crd::u32 inds[3] = {0U, 1U, 2U};
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 3U},
        crd::containers::ConstSpan<crd::u32>{inds, 3U}};
    const auto r = validate_triangle_mesh(view, &alloc);
    REQUIRE(r.boundary_edge_count == 3U);
    REQUIRE(count_defect(r, MeshDefectKind::BoundaryEdge) == 3U);
    REQUIRE(r.well_formed); // boundary edges aren't critical
    REQUIRE_FALSE(r.watertight);
}

TEST_CASE("v4-validate detects inconsistent orientation",
          "[geometry-mesh][v4-validate][orientation]")
{
    crd::memory::MallocAllocator alloc;
    // Two triangles sharing edge (0, 1) — both using it in SAME direction
    // (broken CCW consistency). Manifold but inconsistently oriented.
    const Vec3f verts[4] = {
        {0.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {1.0F, 1.0F, 0.0F}};
    // Tri 0: (0, 1, 2) — uses edge (0→1)
    // Tri 1: (0, 1, 3) — also uses edge (0→1) — SAME direction → inconsistent
    // (the correct CCW pairing would be (1, 0, 3) traversing the shared edge in the opposite direction)
    const crd::u32 inds[6] = {0, 1, 2,   0, 1, 3};
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{verts, 4U},
        crd::containers::ConstSpan<crd::u32>{inds, 6U}};
    MeshValidationOptions opts{};
    opts.report_boundary_edges = false;
    const auto r = validate_triangle_mesh(view, &alloc, opts);
    REQUIRE(contains_defect(r, MeshDefectKind::InconsistentOrientation));
    REQUIRE_FALSE(r.well_formed);
}

TEST_CASE("v4-validate disabling edge checks short-circuits the edge pass",
          "[geometry-mesh][v4-validate][toggles]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};
    MeshValidationOptions opts{};
    opts.check_edges = false; // skip pass 2/3
    const auto r = validate_triangle_mesh(view, &alloc, opts);
    REQUIRE(r.manifold_edge_count == 0U); // not populated when edge check off
    REQUIRE(r.boundary_edge_count == 0U);
    REQUIRE(r.non_manifold_edge_count == 0U);
    REQUIRE(r.well_formed); // tri-level checks still ran and passed
    REQUIRE_FALSE(r.watertight); // unknown without edge pass
}

TEST_CASE("v4-validate reports are deterministic across repeated runs",
          "[geometry-mesh][v4-validate][determinism]")
{
    crd::memory::MallocAllocator alloc;
    auto cube = make_cube(&alloc);
    // Inject a deliberate defect: triangle 5 with i2 out of bounds.
    cube.indices[5 * 3U + 2U] = 99U;
    const TriangleMeshViewf view{
        crd::containers::ConstSpan<Vec3f>{cube.vertices.data(), cube.vertices.size()},
        crd::containers::ConstSpan<crd::u32>{cube.indices.data(), cube.indices.size()}};

    const auto r1 = validate_triangle_mesh(view, &alloc);
    const auto r2 = validate_triangle_mesh(view, &alloc);
    REQUIRE(r1.defects.size() == r2.defects.size());
    for (crd::usize i = 0; i < r1.defects.size(); ++i)
    {
        CHECK(r1.defects[i].kind == r2.defects[i].kind);
        CHECK(r1.defects[i].a    == r2.defects[i].a);
        CHECK(r1.defects[i].b    == r2.defects[i].b);
    }
}
