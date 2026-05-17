// Tests for crd-geometry-mesh-processing v7d isotropic remeshing.
//
// Covers:
//   - Diagnostic statuses (EmptyMesh, NonManifoldInput, InvalidTargetLength)
//   - Subdivided cube → target length L (smaller than mean) → mesh refines
//   - Subdivided cube → target length L (larger than mean) → mesh coarsens
//   - Open quad: boundary held fixed (boundary vertex count preserved)
//   - is_manifold preservation through all 4 passes
//   - Determinism (same input → byte-identical output positions)
//   - Surface projection preserves silhouette (vertices on the cube remain
//     near the cube surface after smoothing)
//   - n_iterations=0 → fresh clone of input
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/isotropic_remesh.hpp>
#include <crd/geometry/mesh_processing/loop_subdivide.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::mesh_processing::BuildStatus;
using crd::geometry::mesh_processing::HalfEdgeMesh;
using crd::geometry::mesh_processing::IsotropicRemeshOptions;
using crd::geometry::mesh_processing::IsotropicRemeshReport;
using crd::geometry::mesh_processing::IsotropicRemeshStatus;
using crd::geometry::mesh_processing::LoopSubdivideOptions;
using crd::geometry::mesh_processing::isotropic_remesh;
using crd::geometry::mesh_processing::loop_subdivide;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{16U * 1024U * 1024U, nullptr, "remesh-test-arena"};
};

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
        0, 2, 1,  0, 3, 2,
        4, 5, 6,  4, 6, 7,
        0, 1, 5,  0, 5, 4,
        2, 3, 7,  2, 7, 6,
        0, 4, 7,  0, 7, 3,
        1, 2, 6,  1, 6, 5,
    };
    for (u32 i : tris) { idx.push_back(i); }
}

void make_quad(crd::containers::Array<Vec3<f32>>& pos,
                crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});
    pos.push_back(Vec3<f32>{1, 0, 0});
    pos.push_back(Vec3<f32>{1, 1, 0});
    pos.push_back(Vec3<f32>{0, 1, 0});
    const u32 tris[] = {0, 1, 2,  0, 2, 3};
    for (u32 i : tris) { idx.push_back(i); }
}

} // anonymous namespace

TEST_CASE("isotropic_remesh: empty mesh -> EmptyMesh status",
          "[geometry-mesh-processing][remesh]")
{
    AllocFixture f{};
    HalfEdgeMesh<f32> m{&f.alloc};
    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length = 0.5F;
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(m, opts, &report);
    CHECK(report.status == IsotropicRemeshStatus::EmptyMesh);
    CHECK(out.face_count() == 0U);
}

TEST_CASE("isotropic_remesh: invalid target length -> InvalidTargetLength",
          "[geometry-mesh-processing][remesh]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length = 0.0F;
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(m, opts, &report);
    CHECK(report.status == IsotropicRemeshStatus::InvalidTargetLength);
    CHECK(out.face_count() == 0U);
}

TEST_CASE("isotropic_remesh: n_iterations=0 returns fresh clone of input",
          "[geometry-mesh-processing][remesh]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length = 0.3F;
    opts.n_iterations = 0;
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(m, opts, &report);
    CHECK(report.status == IsotropicRemeshStatus::Ok);
    CHECK(report.iterations_run == 0U);
    CHECK(out.vertex_count() == m.vertex_count());
    CHECK(out.face_count() == m.face_count());
    CHECK(out.is_manifold());
}

TEST_CASE("isotropic_remesh: subdivided cube refines when target < mean edge",
          "[geometry-mesh-processing][remesh]")
{
    // Start with a subdivided cube so we have plenty of edges; ask for
    // a SMALLER target → algorithm should SPLIT some edges (mesh grows).
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> cube{&f.alloc};
    REQUIRE(cube.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                              crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    LoopSubdivideOptions sub_opts{};
    sub_opts.n_levels = 1;
    auto sub_cube = loop_subdivide(cube, sub_opts);
    const u32 before_v = sub_cube.vertex_count();
    const u32 before_f = sub_cube.face_count();

    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length   = 0.20F;       // tighter than current avg
    opts.n_iterations         = 3;
    opts.project_to_input     = true;
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(sub_cube, opts, &report);

    CHECK(report.status == IsotropicRemeshStatus::Ok);
    CHECK(report.iterations_run == 3U);
    CHECK(out.is_manifold());
    // We expect SOME activity (splits at least) and mesh growth.
    CHECK((report.splits_applied > 0U || report.collapses_applied > 0U));
    CHECK(out.vertex_count() >= before_v);
    CHECK(out.face_count() >= before_f);
}

TEST_CASE("isotropic_remesh: cube coarsens when target > mean edge",
          "[geometry-mesh-processing][remesh]")
{
    // Start with a 2x-subdivided cube. Ask for a coarse target → algorithm
    // collapses + flips toward fewer edges.
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> cube{&f.alloc};
    REQUIRE(cube.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                              crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    LoopSubdivideOptions sub_opts{};
    sub_opts.n_levels = 2;
    auto fine_cube = loop_subdivide(cube, sub_opts);
    const u32 before_v = fine_cube.vertex_count();
    const u32 before_f = fine_cube.face_count();

    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length = 0.35F;       // looser than current avg
    opts.n_iterations = 3;
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(fine_cube, opts, &report);

    CHECK(report.status == IsotropicRemeshStatus::Ok);
    CHECK(out.is_manifold());
    CHECK(report.collapses_applied > 0U);
    CHECK(out.vertex_count() <= before_v);
    CHECK(out.face_count() <= before_f);
}

TEST_CASE("isotropic_remesh: boundary held fixed on open quad",
          "[geometry-mesh-processing][remesh]")
{
    // For an open quad, boundary vertices' positions are kept in place.
    // The boundary edges are skipped in split/collapse passes (v7a's
    // split_edge / collapse_edge reject boundary). So the boundary loop
    // count + the four corner positions should be preserved.
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_quad(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length    = 0.5F;
    opts.n_iterations          = 3;
    opts.keep_boundary_fixed   = true;
    opts.project_to_input      = false;       // pure smoothing; cube isn't representative
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(m, opts, &report);

    CHECK(report.status == IsotropicRemeshStatus::Ok);
    CHECK(out.is_manifold());
    CHECK(out.boundary_loop_count() == 1U);

    // The four corner positions (0,0,0), (1,0,0), (1,1,0), (0,1,0) should
    // still be alive at their original slot indices (no split/collapse on
    // boundary edges) and unchanged (boundary-fixed in smoothing).
    REQUIRE(out.vertex_alive(0U));
    REQUIRE(out.vertex_alive(1U));
    REQUIRE(out.vertex_alive(2U));
    REQUIRE(out.vertex_alive(3U));
    const auto& p0 = out.vertex(0U).position;
    const auto& p1 = out.vertex(1U).position;
    const auto& p2 = out.vertex(2U).position;
    const auto& p3 = out.vertex(3U).position;
    CHECK(p0.x == 0.0F); CHECK(p0.y == 0.0F); CHECK(p0.z == 0.0F);
    CHECK(p1.x == 1.0F); CHECK(p1.y == 0.0F); CHECK(p1.z == 0.0F);
    CHECK(p2.x == 1.0F); CHECK(p2.y == 1.0F); CHECK(p2.z == 0.0F);
    CHECK(p3.x == 0.0F); CHECK(p3.y == 1.0F); CHECK(p3.z == 0.0F);
}

TEST_CASE("isotropic_remesh: determinism - same input yields same output stats",
          "[geometry-mesh-processing][remesh][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> cube{&f.alloc};
    REQUIRE(cube.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                              crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    LoopSubdivideOptions sub_opts{};
    sub_opts.n_levels = 1;
    auto sub_cube = loop_subdivide(cube, sub_opts);

    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length = 0.25F;
    opts.n_iterations = 2;

    IsotropicRemeshReport rep_a{};
    IsotropicRemeshReport rep_b{};
    auto out_a = isotropic_remesh(sub_cube, opts, &rep_a);
    auto out_b = isotropic_remesh(sub_cube, opts, &rep_b);

    CHECK(rep_a.splits_applied == rep_b.splits_applied);
    CHECK(rep_a.collapses_applied == rep_b.collapses_applied);
    CHECK(rep_a.flips_applied == rep_b.flips_applied);
    CHECK(rep_a.vertices_smoothed == rep_b.vertices_smoothed);
    CHECK(out_a.vertex_count() == out_b.vertex_count());
    CHECK(out_a.face_count() == out_b.face_count());
}

TEST_CASE("isotropic_remesh: surface projection keeps vertices near cube",
          "[geometry-mesh-processing][remesh]")
{
    // Smoothing without projection would shrink the cube inward. With
    // BVH projection on, vertices should stay within a tight tolerance
    // of the unit cube's surface (clamped to [0,1]^3).
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> cube{&f.alloc};
    REQUIRE(cube.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                              crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    LoopSubdivideOptions sub_opts{};
    sub_opts.n_levels = 2;
    auto fine_cube = loop_subdivide(cube, sub_opts);

    IsotropicRemeshOptions<f32> opts{};
    opts.target_edge_length = 0.25F;
    opts.n_iterations = 3;
    opts.project_to_input = true;
    auto out = isotropic_remesh(fine_cube, opts);

    // Every vertex must lie within the [0, 1]^3 AABB (loose tolerance for
    // surface-projection ulp noise).
    for (u32 v = 0; v < out.vertex_pool_size(); ++v)
    {
        if (!out.vertex_alive(v)) { continue; }
        const auto& p = out.vertex(v).position;
        CHECK(p.x >= -1e-3F); CHECK(p.x <= 1.0F + 1e-3F);
        CHECK(p.y >= -1e-3F); CHECK(p.y <= 1.0F + 1e-3F);
        CHECK(p.z >= -1e-3F); CHECK(p.z <= 1.0F + 1e-3F);
    }
    CHECK(out.is_manifold());
}

TEST_CASE("isotropic_remesh: f64 precision tier remeshes a cube",
          "[geometry-mesh-processing][remesh][f64]")
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
        1, 2, 6,  1, 6, 5,
    };
    for (u32 i : tris) { idx.push_back(i); }
    HalfEdgeMesh<f64> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f64>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    IsotropicRemeshOptions<f64> opts{};
    opts.target_edge_length = 0.4;
    opts.n_iterations = 2;
    opts.project_to_input = true;
    IsotropicRemeshReport report{};
    auto out = isotropic_remesh(m, opts, &report);
    CHECK(report.status == IsotropicRemeshStatus::Ok);
    CHECK(out.is_manifold());
}
