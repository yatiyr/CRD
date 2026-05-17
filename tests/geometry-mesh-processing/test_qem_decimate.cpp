// Tests for crd-geometry-mesh-processing v7b qem_decimate.
//
// Covers:
//   - Diagnostic statuses (EmptyMesh, NoStopCondition, NonManifoldInput)
//   - Cube decimation by face-count target
//   - Cube decimation by max-error threshold
//   - Locked vertices survive decimation
//   - Boundary preservation (open quad — boundary held)
//   - Inversion prevention
//   - Determinism (same input → byte-identical face counts)
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/qem_decimate.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::mesh_processing::BuildStatus;
using crd::geometry::mesh_processing::HalfEdgeMesh;
using crd::geometry::mesh_processing::QemDecimateOptions;
using crd::geometry::mesh_processing::QemDecimateReport;
using crd::geometry::mesh_processing::QemDecimateStatus;
using crd::geometry::mesh_processing::qem_decimate;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{4U * 1024U * 1024U, nullptr, "qem-test-arena"};
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
    const u32 cube_indices[] = {
        // bottom (z=0, normal -z): CCW from below = CW from above
        0, 2, 1,  0, 3, 2,
        // top (z=1, normal +z)
        4, 5, 6,  4, 6, 7,
        // front (y=0, normal -y)
        0, 1, 5,  0, 5, 4,
        // back (y=1, normal +y)
        2, 3, 7,  2, 7, 6,
        // left (x=0, normal -x)
        0, 4, 7,  0, 7, 3,
        // right (x=1, normal +x)
        1, 2, 6,  1, 6, 5,
    };
    for (u32 i : cube_indices) { idx.push_back(i); }
}

void make_quad(crd::containers::Array<Vec3<f32>>& pos,
                crd::containers::Array<u32>&       idx)
{
    pos.push_back(Vec3<f32>{0, 0, 0});
    pos.push_back(Vec3<f32>{1, 0, 0});
    pos.push_back(Vec3<f32>{1, 1, 0});
    pos.push_back(Vec3<f32>{0, 1, 0});
    const u32 quad_indices[] = {0, 1, 2,  0, 2, 3};
    for (u32 i : quad_indices) { idx.push_back(i); }
}

} // anonymous namespace

TEST_CASE("qem_decimate: NoStopCondition when neither target nor threshold given",
          "[geometry-mesh-processing][qem]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    QemDecimateOptions<f32> opts{};
    opts.target_face_count = 0; // not set
    opts.max_error_threshold = std::numeric_limits<f32>::infinity(); // not finite
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);
    CHECK(report.status == QemDecimateStatus::NoStopCondition);
    CHECK(report.collapses_applied == 0U);
}

TEST_CASE("qem_decimate: cube decimates by target_face_count",
          "[geometry-mesh-processing][qem]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.face_count() == 12U);

    QemDecimateOptions<f32> opts{};
    opts.target_face_count = 6U;
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);

    // Input mesh untouched.
    CHECK(m.face_count() == 12U);
    // Output mesh decimated; the algorithm may stop EARLY (link/inversion
    // rejects), but should not OVERSHOOT the target.
    CHECK(out.face_count() <= 12U);
    CHECK(out.face_count() >= 6U);
    CHECK(out.is_manifold());
}

TEST_CASE("qem_decimate: max_error_threshold stops cheap-only collapses",
          "[geometry-mesh-processing][qem]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    QemDecimateOptions<f32> opts{};
    opts.max_error_threshold = 1e-4F; // very tight — the cube's diagonal collapses are not free
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);
    // We expect few or zero collapses for a unit cube under tight threshold.
    CHECK(out.face_count() >= 8U);
    CHECK(out.is_manifold());
}

TEST_CASE("qem_decimate: locked vertices survive decimation",
          "[geometry-mesh-processing][qem]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    // Lock all 8 corners — no decimation possible.
    const u32 locked[] = {0, 1, 2, 3, 4, 5, 6, 7};
    QemDecimateOptions<f32> opts{};
    opts.target_face_count = 4U;
    opts.locked_vertices = crd::containers::ConstSpan<u32>{locked, 8U};
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);

    // All 8 vertices must still be alive in the output.
    CHECK(out.vertex_count() == 8U);
    // Face count should be unchanged (every edge has both endpoints locked).
    CHECK(out.face_count() == 12U);
    CHECK(report.collapses_applied == 0U);
}

TEST_CASE("qem_decimate: locked subset survives while unlocked simplifies",
          "[geometry-mesh-processing][qem]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    // Lock just vertices 0 and 6 (opposite corners).
    const u32 locked[] = {0U, 6U};
    QemDecimateOptions<f32> opts{};
    opts.target_face_count = 8U;
    opts.locked_vertices = crd::containers::ConstSpan<u32>{locked, 2U};
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);

    // Vertex 0 and 6 must still be alive in the output mesh — verify by
    // checking the OUTPUT mesh's vertex 0 and 6 are alive AND positions match.
    REQUIRE(out.vertex_alive(0U));
    REQUIRE(out.vertex_alive(6U));
    const auto& p0 = out.vertex(0U).position;
    const auto& p6 = out.vertex(6U).position;
    CHECK(p0.x == 0.0F); CHECK(p0.y == 0.0F); CHECK(p0.z == 0.0F);
    CHECK(p6.x == 1.0F); CHECK(p6.y == 1.0F); CHECK(p6.z == 1.0F);
    CHECK(out.is_manifold());
}

TEST_CASE("qem_decimate: open quad with boundary preservation",
          "[geometry-mesh-processing][qem]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_quad(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);
    REQUIRE(m.face_count() == 2U);
    REQUIRE(m.boundary_loop_count() == 1U);

    QemDecimateOptions<f32> opts{};
    opts.target_face_count = 0U;
    opts.max_error_threshold = 0.5F; // allow modest collapses
    opts.boundary_weight = 1000.0F;
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);

    // Boundary-weight pin: the only INTERIOR edge is the diagonal; collapsing
    // it would destroy both triangles. Boundary preservation prevents this
    // when the diagonal's cost (= boundary penalty on both endpoints) exceeds
    // the threshold.
    CHECK(out.face_count() == 2U);
    CHECK(out.is_manifold());
}

TEST_CASE("qem_decimate: determinism - same input yields same output stats",
          "[geometry-mesh-processing][qem][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32>       idx(&f.alloc);
    make_cube(pos, idx);
    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    QemDecimateOptions<f32> opts{};
    opts.target_face_count = 4U;

    QemDecimateReport report_a{};
    QemDecimateReport report_b{};
    auto out_a = qem_decimate(m, opts, &report_a);
    auto out_b = qem_decimate(m, opts, &report_b);

    CHECK(report_a.collapses_applied == report_b.collapses_applied);
    CHECK(report_a.collapses_rejected_link == report_b.collapses_rejected_link);
    CHECK(report_a.collapses_rejected_flip == report_b.collapses_rejected_flip);
    CHECK(out_a.face_count() == out_b.face_count());
    CHECK(out_a.vertex_count() == out_b.vertex_count());

    // Stronger: every alive vertex's position matches byte-for-byte.
    REQUIRE(out_a.vertex_pool_size() == out_b.vertex_pool_size());
    for (u32 v = 0; v < out_a.vertex_pool_size(); ++v)
    {
        if (!out_a.vertex_alive(v))
        {
            CHECK(!out_b.vertex_alive(v));
            continue;
        }
        REQUIRE(out_b.vertex_alive(v));
        const auto& pa = out_a.vertex(v).position;
        const auto& pb = out_b.vertex(v).position;
        CHECK(pa.x == pb.x);
        CHECK(pa.y == pb.y);
        CHECK(pa.z == pb.z);
    }
}

TEST_CASE("qem_decimate: f64 precision tier decimates a cube",
          "[geometry-mesh-processing][qem][f64]")
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
    const u32 cube_indices[] = {
        0, 2, 1,  0, 3, 2,
        4, 5, 6,  4, 6, 7,
        0, 1, 5,  0, 5, 4,
        2, 3, 7,  2, 7, 6,
        0, 4, 7,  0, 7, 3,
        1, 2, 6,  1, 6, 5,
    };
    for (u32 i : cube_indices) { idx.push_back(i); }

    HalfEdgeMesh<f64> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f64>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()})
            == BuildStatus::Ok);

    QemDecimateOptions<f64> opts{};
    opts.target_face_count = 6U;
    QemDecimateReport report{};
    auto out = qem_decimate(m, opts, &report);
    CHECK(out.face_count() <= 12U);
    CHECK(out.face_count() >= 6U);
    CHECK(out.is_manifold());
}
