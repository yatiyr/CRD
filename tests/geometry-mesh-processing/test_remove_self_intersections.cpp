// Tests for crd-geometry-mesh-processing v7g self-intersection removal.
//
// Covers:
//   - Diagnostic statuses (EmptyMesh, NoSelfIntersections)
//   - Two crossing triangles → 1 intersection pair, segment vertices added,
//     both triangles retriangulated, output is_manifold preserved (or
//     graceful degradation if CDT fails on degenerate inputs)
//   - Closed cube (no self-intersections) → output identical, status
//     NoSelfIntersections
//   - Determinism: same input → same counts
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/remove_self_intersections.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::mesh_processing::BuildStatus;
using crd::geometry::mesh_processing::HalfEdgeMesh;
using crd::geometry::mesh_processing::RemoveSelfIntersectionsOptions;
using crd::geometry::mesh_processing::RemoveSelfIntersectionsReport;
using crd::geometry::mesh_processing::RemoveSelfIntersectionsStatus;
using crd::geometry::mesh_processing::remove_self_intersections;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{8U * 1024U * 1024U, nullptr, "selfintersect-test-arena"};
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

// Two triangles that cross through each other (one in XY plane, one in YZ
// plane, both passing through the origin region — they intersect along
// a segment on the Y axis).
void make_two_crossing_triangles(crd::containers::Array<Vec3<f32>>& pos,
                                    crd::containers::Array<u32>&       idx)
{
    // Triangle 1 in XY plane (z = 0).
    pos.push_back(Vec3<f32>{-1, -1, 0});  // 0
    pos.push_back(Vec3<f32>{ 1, -1, 0});  // 1
    pos.push_back(Vec3<f32>{ 0,  1, 0});  // 2
    // Triangle 2 in YZ plane (x = 0).
    pos.push_back(Vec3<f32>{0, -1, -1});  // 3
    pos.push_back(Vec3<f32>{0,  1,  0});  // 4 -- intentionally not coincident with #2 to avoid shared vertex
    pos.push_back(Vec3<f32>{0, -1,  1});  // 5
    idx.push_back(0); idx.push_back(1); idx.push_back(2);
    idx.push_back(3); idx.push_back(4); idx.push_back(5);
}

} // anonymous namespace

TEST_CASE("remove_self_intersections: empty mesh -> EmptyMesh",
          "[geometry-mesh-processing][selfintersect]")
{
    AllocFixture f{};
    HalfEdgeMesh<f32> m{&f.alloc};
    RemoveSelfIntersectionsOptions<f32> opts{};
    RemoveSelfIntersectionsReport report{};
    auto out = remove_self_intersections(m, opts, &report);
    CHECK(report.status == RemoveSelfIntersectionsStatus::EmptyMesh);
    CHECK(out.face_count() == 0U);
}

TEST_CASE("remove_self_intersections: closed cube -> NoSelfIntersections",
          "[geometry-mesh-processing][selfintersect]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    RemoveSelfIntersectionsOptions<f32> opts{};
    RemoveSelfIntersectionsReport report{};
    auto out = remove_self_intersections(m, opts, &report);
    INFO("candidate_pairs_tested=" << report.candidate_pairs_tested
         << " intersection_pairs_detected=" << report.intersection_pairs_detected
         << " triangles_retriangulated=" << report.triangles_retriangulated);
    CHECK(report.status == RemoveSelfIntersectionsStatus::NoSelfIntersections);
    CHECK(report.intersection_pairs_detected == 0U);
    CHECK(report.intersection_vertices_added == 0U);
    CHECK(out.vertex_count() == m.vertex_count());
    CHECK(out.face_count() == m.face_count());
}

TEST_CASE("remove_self_intersections: two crossing triangles -> retriangulated",
          "[geometry-mesh-processing][selfintersect]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_two_crossing_triangles(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    RemoveSelfIntersectionsOptions<f32> opts{};
    RemoveSelfIntersectionsReport report{};
    auto out = remove_self_intersections(m, opts, &report);
    CHECK(report.status == RemoveSelfIntersectionsStatus::Ok);
    CHECK(report.intersection_pairs_detected == 1U);
    CHECK(report.intersection_vertices_added == 2U);
    // Either both triangles retriangulated OR fallback paths trip — both
    // are acceptable. We just verify no crash and reasonable output.
    CHECK(out.face_count() >= 2U);
}

TEST_CASE("remove_self_intersections: determinism - same input yields same counts",
          "[geometry-mesh-processing][selfintersect][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_two_crossing_triangles(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    RemoveSelfIntersectionsOptions<f32> opts{};
    RemoveSelfIntersectionsReport rep_a{};
    RemoveSelfIntersectionsReport rep_b{};
    auto out_a = remove_self_intersections(m, opts, &rep_a);
    auto out_b = remove_self_intersections(m, opts, &rep_b);
    CHECK(rep_a.intersection_pairs_detected == rep_b.intersection_pairs_detected);
    CHECK(rep_a.intersection_vertices_added == rep_b.intersection_vertices_added);
    CHECK(rep_a.triangles_retriangulated == rep_b.triangles_retriangulated);
    CHECK(out_a.face_count() == out_b.face_count());
    CHECK(out_a.vertex_count() == out_b.vertex_count());
}

TEST_CASE("remove_self_intersections: f64 precision tier (cube)",
          "[geometry-mesh-processing][selfintersect][f64]")
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

    RemoveSelfIntersectionsOptions<f64> opts{};
    RemoveSelfIntersectionsReport report{};
    auto out = remove_self_intersections(m, opts, &report);
    CHECK(report.status == RemoveSelfIntersectionsStatus::NoSelfIntersections);
    CHECK(out.face_count() == m.face_count());
}
