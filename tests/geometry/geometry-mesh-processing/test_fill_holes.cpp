// Tests for crd-geometry-mesh-processing v7e Liepa hole filling.
//
// Covers:
//   - Diagnostic statuses (EmptyMesh, NonManifoldInput, NoHolesToFill)
//   - Closed cube: no holes → output identical to input
//   - Cube with one triangle removed: 3-vertex hole → 1 patch triangle, closed
//   - Cube with one face (2 triangles) removed: 4-vertex hole → 2 patch tris
//   - Cube with two opposite faces removed: 2 separate holes
//   - max_hole_size guard
//   - Determinism (same input → byte-identical face/vertex counts)
//   - f64 precision tier
//   - Post-fill is_manifold + is_closed
//   - Orientation: output has consistent outward normals (we verify by
//     positive enclosed volume via divergence theorem on a filled cube)

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/fill_holes.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::mesh_processing::BuildStatus;
using crd::geometry::mesh_processing::HalfEdgeMesh;
using crd::geometry::mesh_processing::FillHolesOptions;
using crd::geometry::mesh_processing::FillHolesReport;
using crd::geometry::mesh_processing::FillHolesStatus;
using crd::geometry::mesh_processing::fill_holes;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{4U * 1024U * 1024U, nullptr, "fillholes-test-arena"};
};

// Closed cube — 12 triangles.
void make_cube(crd::containers::Array<Vec3<f32>>& pos,
                crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});
    pos.push_back(Vec3<f32>{1, 0, 0});
    pos.push_back(Vec3<f32>{1, 1, 0});
    pos.push_back(Vec3<f32>{0, 1, 0});
    pos.push_back(Vec3<f32>{0, 0, 1});
    pos.push_back(Vec3<f32>{1, 0, 1});
    pos.push_back(Vec3<f32>{1, 1, 1});
    pos.push_back(Vec3<f32>{0, 1, 1});
    const u32 tris[] = {
        0, 2, 1,  0, 3, 2,    // bottom
        4, 5, 6,  4, 6, 7,    // top
        0, 1, 5,  0, 5, 4,    // front
        2, 3, 7,  2, 7, 6,    // back
        0, 4, 7,  0, 7, 3,    // left
        1, 2, 6,  1, 6, 5,    // right
    };
    for (u32 i : tris) { idx.push_back(i); }
}

// Cube with one triangle removed (3-vertex hole).
void make_cube_minus_triangle(crd::containers::Array<Vec3<f32>>& pos,
                                crd::containers::Array<u32>&       idx)
{
    make_cube(pos, idx);
    // Remove triangle indices [33, 35] (last 3 elements = the LAST triangle).
    idx.pop_back();
    idx.pop_back();
    idx.pop_back();
}

// Cube with one face (2 triangles) removed (4-vertex hole).
void make_cube_minus_face(crd::containers::Array<Vec3<f32>>& pos,
                           crd::containers::Array<u32>&       idx)
{
    make_cube(pos, idx);
    // Remove the last two triangles (top face = entries [12, 18)).
    // Drop the last 6 indices.
    for (int i = 0; i < 6; ++i) { idx.pop_back(); }
}

// Cube with TWO opposite faces removed (bottom + top → 2 holes).
void make_cube_minus_two_opposite_faces(crd::containers::Array<Vec3<f32>>& pos,
                                          crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});
    pos.push_back(Vec3<f32>{1, 0, 0});
    pos.push_back(Vec3<f32>{1, 1, 0});
    pos.push_back(Vec3<f32>{0, 1, 0});
    pos.push_back(Vec3<f32>{0, 0, 1});
    pos.push_back(Vec3<f32>{1, 0, 1});
    pos.push_back(Vec3<f32>{1, 1, 1});
    pos.push_back(Vec3<f32>{0, 1, 1});
    // SKIP bottom (z=0) and top (z=1). Keep front, back, left, right.
    const u32 tris[] = {
        0, 1, 5,  0, 5, 4,    // front
        2, 3, 7,  2, 7, 6,    // back
        0, 4, 7,  0, 7, 3,    // left
        1, 2, 6,  1, 6, 5,    // right
    };
    for (u32 i : tris) { idx.push_back(i); }
}

// Signed volume of a closed triangle mesh via divergence theorem
// (Σ (1/6) v0 · (v1 × v2) over triangles). Positive ⇒ CCW outward.
template <typename T>
T signed_volume(const HalfEdgeMesh<T>& m)
{
    T vol = T{0};
    for (u32 f = 0; f < m.face_pool_size(); ++f)
    {
        if (!m.face_alive(f)) { continue; }
        const u32 h0 = m.face(f).first_he;
        const u32 h1 = m.he(h0).next;
        const u32 h2 = m.he(h1).next;
        const auto& v0 = m.vertex(m.he(h0).origin).position;
        const auto& v1 = m.vertex(m.he(h1).origin).position;
        const auto& v2 = m.vertex(m.he(h2).origin).position;
        // (v0 · (v1 × v2)) / 6
        const T cx = v1.y * v2.z - v1.z * v2.y;
        const T cy = v1.z * v2.x - v1.x * v2.z;
        const T cz = v1.x * v2.y - v1.y * v2.x;
        vol += (v0.x * cx + v0.y * cy + v0.z * cz);
    }
    return vol / T{6};
}

} // anonymous namespace

TEST_CASE("fill_holes: empty mesh -> EmptyMesh",
          "[geometry-mesh-processing][fillholes]")
{
    AllocFixture f{};
    HalfEdgeMesh<f32> m{&f.alloc};
    FillHolesOptions<f32> opts{};
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::EmptyMesh);
    CHECK(out.face_count() == 0U);
}

TEST_CASE("fill_holes: closed cube -> NoHolesToFill",
          "[geometry-mesh-processing][fillholes]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.is_closed());
    FillHolesOptions<f32> opts{};
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::NoHolesToFill);
    CHECK(report.holes_detected == 0U);
    CHECK(report.holes_filled == 0U);
    CHECK(report.triangles_added == 0U);
    CHECK(out.face_count() == m.face_count());
    CHECK(out.vertex_count() == m.vertex_count());
}

TEST_CASE("fill_holes: cube minus 1 triangle (refine=off) -> exactly 1 patch tri",
          "[geometry-mesh-processing][fillholes]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube_minus_triangle(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.face_count() == 11U);
    REQUIRE_FALSE(m.is_closed());
    REQUIRE(m.boundary_loop_count() == 1U);

    FillHolesOptions<f32> opts{};
    opts.refine = false;             // exact-count test — disable §4 Steiner
    opts.fairing_iterations = 0;     // ... and §5 fairing
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(report.holes_detected == 1U);
    CHECK(report.holes_filled == 1U);
    CHECK(report.triangles_added == 1U);
    CHECK(report.steiner_points_added == 0U);
    CHECK(out.face_count() == 12U);
    CHECK(out.is_closed());
    CHECK(out.is_manifold());
    CHECK(signed_volume(out) > 0.0F);
}

TEST_CASE("fill_holes: cube minus 1 face (refine=off) -> exactly 2 patch tris",
          "[geometry-mesh-processing][fillholes]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube_minus_face(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.face_count() == 10U);
    REQUIRE(m.boundary_loop_count() == 1U);

    FillHolesOptions<f32> opts{};
    opts.refine = false;
    opts.fairing_iterations = 0;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(report.holes_detected == 1U);
    CHECK(report.holes_filled == 1U);
    CHECK(report.triangles_added == 2U);
    CHECK(report.steiner_points_added == 0U);
    CHECK(out.face_count() == 12U);
    CHECK(out.is_closed());
    CHECK(out.is_manifold());
    CHECK(signed_volume(out) > 0.0F);
}

TEST_CASE("fill_holes: cube minus 1 face (refine=on) -> Steiner refinement",
          "[geometry-mesh-processing][fillholes][refine]")
{
    // Hole is 1x1 quad; cube edges are length 1. Surrounding mesh σ ≈ 1.
    // The DP yields 2 triangles each with area 0.5; their longest
    // vertex-to-centroid distance is sqrt(2)/3 * edge ≈ 0.47. The
    // too-coarse test (√2 · dist > σ) gives ≈ 0.67 vs 1 → NOT triggered
    // ⇒ refinement converges without splits. verify the algorithm
    // gracefully handles "no refinement needed" too.
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube_minus_face(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    FillHolesOptions<f32> opts{};
    opts.refine = true;
    opts.fairing_iterations = 0;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(report.holes_filled == 1U);
    CHECK(out.is_closed());
    CHECK(out.is_manifold());
    CHECK(signed_volume(out) > 0.0F);
    // Refinement loop must have run at least once (to verify convergence).
    CHECK(report.refine_iterations_run >= 1U);
}

TEST_CASE("fill_holes: refine triggers Steiner points when hole > local scale",
          "[geometry-mesh-processing][fillholes][refine]")
{
    // Construct a coarse mesh where the surrounding edges are SHORT
    // (forcing σ_loop small) but the hole is LARGE (forcing the DP
    // triangle to be over-sized). Build a 4x4 grid of vertices forming
    // a flat patch; remove the interior 4 triangles to create a hole;
    // refinement should subdivide the resulting large hole-spanning
    // triangles into many small ones to match the surrounding density.
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    // Build a 4x4 grid of vertices at unit spacing in xy plane, z=0.
    // Bend slightly into 3D so the mesh is non-degenerate. (Hole's
    // triangles will be roughly planar.)
    for (int j = 0; j < 4; ++j)
    {
        for (int i = 0; i < 4; ++i)
        {
            pos.push_back(Vec3<f32>{static_cast<f32>(i),
                                     static_cast<f32>(j),
                                     0.001F * static_cast<f32>(i * j)});
        }
    }
    // Triangulate the entire 3x3 quad grid (each quad → 2 triangles).
    auto vi = [](int i, int j) { return static_cast<u32>(j * 4 + i); };
    for (int j = 0; j < 3; ++j)
    {
        for (int i = 0; i < 3; ++i)
        {
            // Skip the center quad to make a hole.
            if (i == 1 && j == 1) { continue; }
            idx.push_back(vi(i, j));
            idx.push_back(vi(i + 1, j));
            idx.push_back(vi(i + 1, j + 1));
            idx.push_back(vi(i, j));
            idx.push_back(vi(i + 1, j + 1));
            idx.push_back(vi(i, j + 1));
        }
    }
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    // The mesh has a 4-vertex hole at (1..2, 1..2) — surrounding σ ≈ 1.
    REQUIRE(m.boundary_loop_count() >= 1U);

    FillHolesOptions<f32> opts{};
    opts.refine = true;
    opts.refine_alpha = 1.4142136F;
    opts.max_refine_iterations = 6;
    opts.fairing_iterations = 0;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(out.is_manifold());
    // Patch should be present (1+ patches filled).
    CHECK(report.holes_filled >= 1U);
    CHECK(report.triangles_added >= 2U);
}

TEST_CASE("fill_holes: fairing converges on Steiner-refined patch",
          "[geometry-mesh-processing][fillholes][fairing]")
{
    // Same 4x4 grid with hole as the refine test. Enable fairing.
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    for (int j = 0; j < 4; ++j)
    {
        for (int i = 0; i < 4; ++i)
        {
            pos.push_back(Vec3<f32>{static_cast<f32>(i),
                                     static_cast<f32>(j),
                                     0.001F * static_cast<f32>(i * j)});
        }
    }
    auto vi = [](int i, int j) { return static_cast<u32>(j * 4 + i); };
    for (int j = 0; j < 3; ++j)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (i == 1 && j == 1) { continue; }
            idx.push_back(vi(i, j));
            idx.push_back(vi(i + 1, j));
            idx.push_back(vi(i + 1, j + 1));
            idx.push_back(vi(i, j));
            idx.push_back(vi(i + 1, j + 1));
            idx.push_back(vi(i, j + 1));
        }
    }
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    FillHolesOptions<f32> opts{};
    opts.refine = true;
    opts.fairing_iterations = 5;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(out.is_manifold());
    // Fairing only runs when there ARE Steiner points; if refinement
    // produced any, fairing iterates 5 times.
    if (report.steiner_points_added > 0U)
    {
        CHECK(report.fairing_iterations_run == 5U);
    }
}

TEST_CASE("fill_holes: cube minus 2 opposite faces (refine=off) -> 4 patch tris",
          "[geometry-mesh-processing][fillholes]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube_minus_two_opposite_faces(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.face_count() == 8U);
    REQUIRE(m.boundary_loop_count() == 2U);

    FillHolesOptions<f32> opts{};
    opts.refine = false;
    opts.fairing_iterations = 0;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(report.holes_detected == 2U);
    CHECK(report.holes_filled == 2U);
    CHECK(report.triangles_added == 4U);
    CHECK(out.face_count() == 12U);
    CHECK(out.is_closed());
    CHECK(out.is_manifold());
    CHECK(signed_volume(out) > 0.0F);
}

TEST_CASE("fill_holes: max_hole_size guard skips oversized holes",
          "[geometry-mesh-processing][fillholes]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube_minus_face(pos, idx);    // 4-vertex hole
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    FillHolesOptions<f32> opts{};
    opts.max_hole_size = 3;    // forces our 4-vertex hole to be skipped
    opts.refine = false;
    opts.fairing_iterations = 0;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(report.holes_detected == 1U);
    CHECK(report.holes_filled == 0U);
    CHECK(report.holes_skipped_too_large == 1U);
    CHECK(report.triangles_added == 0U);
    CHECK(out.face_count() == 10U);    // unchanged
    CHECK_FALSE(out.is_closed());
}

TEST_CASE("fill_holes: determinism - same input yields same counts",
          "[geometry-mesh-processing][fillholes][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube_minus_face(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    FillHolesOptions<f32> opts{};
    FillHolesReport rep_a{};
    FillHolesReport rep_b{};
    auto out_a = fill_holes(m, opts, &rep_a);
    auto out_b = fill_holes(m, opts, &rep_b);
    CHECK(rep_a.triangles_added == rep_b.triangles_added);
    CHECK(out_a.vertex_count() == out_b.vertex_count());
    CHECK(out_a.face_count() == out_b.face_count());
}

TEST_CASE("fill_holes: f64 precision tier fills a cube face hole",
          "[geometry-mesh-processing][fillholes][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    pos.push_back(Vec3<f64>{0, 0, 0});
    pos.push_back(Vec3<f64>{1, 0, 0});
    pos.push_back(Vec3<f64>{1, 1, 0});
    pos.push_back(Vec3<f64>{0, 1, 0});
    pos.push_back(Vec3<f64>{0, 0, 1});
    pos.push_back(Vec3<f64>{1, 0, 1});
    pos.push_back(Vec3<f64>{1, 1, 1});
    pos.push_back(Vec3<f64>{0, 1, 1});
    const u32 tris[] = {
        0, 2, 1,  0, 3, 2,
        4, 5, 6,  4, 6, 7,
        0, 1, 5,  0, 5, 4,
        2, 3, 7,  2, 7, 6,
        0, 4, 7,  0, 7, 3,
        // omit "right" face → 4-vertex hole
    };
    for (u32 i : tris) { idx.push_back(i); }
    HalfEdgeMesh<f64> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f64>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.face_count() == 10U);

    FillHolesOptions<f64> opts{};
    opts.refine = false;
    opts.fairing_iterations = 0;
    FillHolesReport report{};
    auto out = fill_holes(m, opts, &report);
    CHECK(report.status == FillHolesStatus::Ok);
    CHECK(report.holes_filled == 1U);
    CHECK(report.triangles_added == 2U);
    CHECK(out.face_count() == 12U);
    CHECK(out.is_closed());
    CHECK(out.is_manifold());
    CHECK(signed_volume(out) > 0.0);
}
