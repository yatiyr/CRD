// Tests for crd-geometry-mesh-processing v7f manifoldness repair.
//
// Covers:
//   - Diagnostic statuses (EmptyMesh, AlreadyManifold)
//   - Closed cube (already manifold) → output identical, AlreadyManifold
//   - Non-manifold edge: 3 triangles sharing one edge → repaired to 2 + 1
//   - Non-manifold edge: 4 triangles sharing one edge (paired) → repaired
//     to 2 pairs each on its own edge copy
//   - Bowtie vertex: 2 disjoint tetrahedra touching at one vertex →
//     repaired by duplicating the bowtie vertex (one per fan)
//   - Phase toggle: repair_non_manifold_edges = false leaves edges alone
//   - Phase toggle: repair_bowtie_vertices = false leaves bowties alone
//   - Determinism: same input → same counts
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/repair_manifoldness.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::mesh_processing::BuildStatus;
using crd::geometry::mesh_processing::HalfEdgeMesh;
using crd::geometry::mesh_processing::RepairManifoldnessOptions;
using crd::geometry::mesh_processing::RepairManifoldnessReport;
using crd::geometry::mesh_processing::RepairManifoldnessStatus;
using crd::geometry::mesh_processing::repair_manifoldness;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{4U * 1024U * 1024U, nullptr, "repair-test-arena"};
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

// Three triangles sharing edge (0, 1): T0=(0,1,2), T1=(0,1,3), T2=(1,0,4).
// Edge (0, 1) is non-manifold (3 incident triangles).
void make_three_tris_one_shared_edge(crd::containers::Array<Vec3<f32>>& pos,
                                       crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});
    pos.push_back(Vec3<f32>{1, 0, 0});
    pos.push_back(Vec3<f32>{0.5F, 1, 0});
    pos.push_back(Vec3<f32>{0.5F, 1, 1});
    pos.push_back(Vec3<f32>{0.5F, -1, 0});
    idx.push_back(0); idx.push_back(1); idx.push_back(2);
    idx.push_back(0); idx.push_back(1); idx.push_back(3);
    idx.push_back(1); idx.push_back(0); idx.push_back(4);
}

// Four triangles around edge (0, 1) — fully non-manifold "book with 4 pages":
// 2 forward + 2 backward → can be repaired into 2 manifold pairs.
void make_four_tris_one_shared_edge(crd::containers::Array<Vec3<f32>>& pos,
                                      crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});
    pos.push_back(Vec3<f32>{1, 0, 0});
    pos.push_back(Vec3<f32>{0.5F, 1, 0});
    pos.push_back(Vec3<f32>{0.5F, -1, 0});
    pos.push_back(Vec3<f32>{0.5F, 0, 1});
    pos.push_back(Vec3<f32>{0.5F, 0, -1});
    // 2 forward (origin 0 < dest 1):
    idx.push_back(0); idx.push_back(1); idx.push_back(2);
    idx.push_back(0); idx.push_back(1); idx.push_back(3);
    // 2 backward (origin 1, dest 0):
    idx.push_back(1); idx.push_back(0); idx.push_back(4);
    idx.push_back(1); idx.push_back(0); idx.push_back(5);
}

// Two disjoint triangle fans joined at vertex 0 — classic bowtie.
// Fan 1: triangles (0,1,2) (0,2,3) (0,3,1) → small triangle around (0..3).
// Fan 2: triangles (0,4,5) (0,5,6) (0,6,4) → separate triangle around (0,4..6).
// Vertex 0 has 6 outgoing HEs forming 2 disconnected fans.
void make_bowtie_vertex(crd::containers::Array<Vec3<f32>>& pos,
                          crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});         // 0 — the bowtie vertex
    pos.push_back(Vec3<f32>{1, 1, 1});         // 1
    pos.push_back(Vec3<f32>{1, -1, 1});        // 2
    pos.push_back(Vec3<f32>{-1, 0, 1});        // 3
    pos.push_back(Vec3<f32>{1, 1, -1});        // 4
    pos.push_back(Vec3<f32>{1, -1, -1});       // 5
    pos.push_back(Vec3<f32>{-1, 0, -1});       // 6
    // Fan 1 (closed-tetra-cap around vertex 0, top).
    idx.push_back(0); idx.push_back(1); idx.push_back(2);
    idx.push_back(0); idx.push_back(2); idx.push_back(3);
    idx.push_back(0); idx.push_back(3); idx.push_back(1);
    // Fan 2 (separate closed-tetra-cap around vertex 0, bottom).
    idx.push_back(0); idx.push_back(4); idx.push_back(5);
    idx.push_back(0); idx.push_back(5); idx.push_back(6);
    idx.push_back(0); idx.push_back(6); idx.push_back(4);
}

} // anonymous namespace

TEST_CASE("repair_manifoldness: empty mesh -> EmptyMesh",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    HalfEdgeMesh<f32> m{&f.alloc};
    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.status == RepairManifoldnessStatus::EmptyMesh);
    CHECK(out.face_count() == 0U);
}

TEST_CASE("repair_manifoldness: closed cube -> AlreadyManifold",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.is_manifold());

    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.status == RepairManifoldnessStatus::AlreadyManifold);
    CHECK(report.non_manifold_edges_detected == 0U);
    CHECK(report.bowtie_vertices_detected == 0U);
    CHECK(report.duplicated_vertices_added == 0U);
    CHECK(out.vertex_count() == m.vertex_count());
    CHECK(out.face_count() == m.face_count());
    CHECK(out.is_manifold());
}

TEST_CASE("repair_manifoldness: non-manifold edge (3 tris) -> repaired",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_three_tris_one_shared_edge(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    // build_from reports NonManifoldEdge — accepted as input.
    const auto bs = m.build_from(
        crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
        crd::containers::ConstSpan<u32>{idx.data(), idx.size()});
    REQUIRE(bs == BuildStatus::NonManifoldEdge);

    // Phase-A-only first (disable B to verify Phase A counts in isolation).
    RepairManifoldnessOptions opts_a_only{};
    opts_a_only.repair_bowtie_vertices = false;
    RepairManifoldnessReport report_a{};
    auto out_a = repair_manifoldness(m, opts_a_only, &report_a);
    CHECK(report_a.non_manifold_edges_detected == 1U);
    CHECK(report_a.non_manifold_edges_repaired == 1U);
    // 3 incident triangles → 1 pair + 1 singleton → 1 duplicated vertex.
    CHECK(report_a.duplicated_vertices_added == 1U);

    // Full repair (A + B). Phase A's duplication creates a bowtie at the
    // shared vertex 1 (the third triangle's fan disconnects from the
    // others); Phase B then detects + repairs that bowtie with ANOTHER
    // duplicate. Total = 2.
    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.status == RepairManifoldnessStatus::Ok);
    CHECK(report.non_manifold_edges_detected == 1U);
    CHECK(report.bowtie_vertices_detected >= 1U);
    CHECK(report.duplicated_vertices_added == 2U); // 1 from A + 1 from B
    CHECK(out.face_count() == 3U); // triangles preserved
    CHECK(out.is_manifold());      // strictly 2-manifold after both phases

    // Verify rebuild via to_indexed is clean.
    crd::containers::Array<Vec3<f32>> out_pos(&f.alloc);
    crd::containers::Array<u32>       out_idx(&f.alloc);
    out.to_indexed(out_pos, out_idx);
    HalfEdgeMesh<f32> verify{&f.alloc};
    CHECK(verify.build_from(crd::containers::ConstSpan<Vec3<f32>>{out_pos.data(), out_pos.size()},
                              crd::containers::ConstSpan<u32>{out_idx.data(), out_idx.size()})
          == BuildStatus::Ok);
}

TEST_CASE("repair_manifoldness: non-manifold edge (4 tris paired) -> repaired",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_four_tris_one_shared_edge(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    const auto bs = m.build_from(
        crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
        crd::containers::ConstSpan<u32>{idx.data(), idx.size()});
    REQUIRE(bs == BuildStatus::NonManifoldEdge);

    // Phase A only.
    RepairManifoldnessOptions opts_a_only{};
    opts_a_only.repair_bowtie_vertices = false;
    RepairManifoldnessReport report_a{};
    auto out_a = repair_manifoldness(m, opts_a_only, &report_a);
    CHECK(report_a.non_manifold_edges_detected == 1U);
    // 4 triangles, 2 forwards + 2 backwards → 2 pairs total → 1 extra
    // pair beyond the first → 1 duplicated vertex shared between both
    // triangles in the extra pair.
    CHECK(report_a.duplicated_vertices_added == 1U);

    // Full repair: Phase A's split creates 2 separate-fan groups at
    // vertex 1 (the shared edge endpoint that isn't duplicated). Phase
    // B duplicates vertex 1 → total 2 duplications.
    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.status == RepairManifoldnessStatus::Ok);
    CHECK(report.non_manifold_edges_detected == 1U);
    CHECK(report.duplicated_vertices_added == 2U); // 1 from A + 1 from B
    CHECK(out.face_count() == 4U);
    CHECK(out.is_manifold());
    crd::containers::Array<Vec3<f32>> out_pos(&f.alloc);
    crd::containers::Array<u32>       out_idx(&f.alloc);
    out.to_indexed(out_pos, out_idx);
    HalfEdgeMesh<f32> verify{&f.alloc};
    CHECK(verify.build_from(crd::containers::ConstSpan<Vec3<f32>>{out_pos.data(), out_pos.size()},
                              crd::containers::ConstSpan<u32>{out_idx.data(), out_idx.size()})
          == BuildStatus::Ok);
}

TEST_CASE("repair_manifoldness: bowtie vertex -> duplicated per fan",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_bowtie_vertex(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE_FALSE(m.is_manifold()); // vertex 0 has 2 disjoint fans

    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.status == RepairManifoldnessStatus::Ok);
    CHECK(report.bowtie_vertices_detected == 1U);
    CHECK(report.bowtie_vertices_repaired == 1U);
    CHECK(report.duplicated_vertices_added == 1U);
    CHECK(out.vertex_count() == 8U); // 7 original + 1 duplicated
    CHECK(out.face_count() == 6U);
    CHECK(out.is_manifold());
}

TEST_CASE("repair_manifoldness: phase A toggle off leaves non-manifold edges",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_three_tris_one_shared_edge(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    const auto bs = m.build_from(
        crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
        crd::containers::ConstSpan<u32>{idx.data(), idx.size()});
    REQUIRE(bs == BuildStatus::NonManifoldEdge);

    RepairManifoldnessOptions opts{};
    opts.repair_non_manifold_edges = false;
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.non_manifold_edges_detected == 0U);
    CHECK(report.duplicated_vertices_added == 0U);
}

TEST_CASE("repair_manifoldness: phase B toggle off leaves bowtie",
          "[geometry-mesh-processing][repair]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_bowtie_vertex(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    RepairManifoldnessOptions opts{};
    opts.repair_bowtie_vertices = false;
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.bowtie_vertices_detected == 0U);
    CHECK(report.duplicated_vertices_added == 0U);
    CHECK(out.vertex_count() == 7U); // unchanged
}

TEST_CASE("repair_manifoldness: determinism - same input yields same stats",
          "[geometry-mesh-processing][repair][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_bowtie_vertex(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport rep_a{};
    RepairManifoldnessReport rep_b{};
    auto out_a = repair_manifoldness(m, opts, &rep_a);
    auto out_b = repair_manifoldness(m, opts, &rep_b);
    CHECK(rep_a.bowtie_vertices_repaired == rep_b.bowtie_vertices_repaired);
    CHECK(rep_a.duplicated_vertices_added == rep_b.duplicated_vertices_added);
    CHECK(out_a.vertex_count() == out_b.vertex_count());
    CHECK(out_a.face_count() == out_b.face_count());
}

TEST_CASE("repair_manifoldness: f64 precision tier repairs bowtie",
          "[geometry-mesh-processing][repair][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    pos.push_back(Vec3<f64>{0, 0, 0});
    pos.push_back(Vec3<f64>{1, 1, 1});
    pos.push_back(Vec3<f64>{1, -1, 1});
    pos.push_back(Vec3<f64>{-1, 0, 1});
    pos.push_back(Vec3<f64>{1, 1, -1});
    pos.push_back(Vec3<f64>{1, -1, -1});
    pos.push_back(Vec3<f64>{-1, 0, -1});
    idx.push_back(0); idx.push_back(1); idx.push_back(2);
    idx.push_back(0); idx.push_back(2); idx.push_back(3);
    idx.push_back(0); idx.push_back(3); idx.push_back(1);
    idx.push_back(0); idx.push_back(4); idx.push_back(5);
    idx.push_back(0); idx.push_back(5); idx.push_back(6);
    idx.push_back(0); idx.push_back(6); idx.push_back(4);
    HalfEdgeMesh<f64> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f64>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    RepairManifoldnessOptions opts{};
    RepairManifoldnessReport report{};
    auto out = repair_manifoldness(m, opts, &report);
    CHECK(report.status == RepairManifoldnessStatus::Ok);
    CHECK(report.bowtie_vertices_repaired == 1U);
    CHECK(out.is_manifold());
}
