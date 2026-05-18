// ---------------------------------------------------------------------------
// crd-geometry-decomposition v9c-b — V-HACD decompose test corpus.
//
// CALIBRATION FIRST per advisor TDD + v8h precedent: a CONVEX input
// (cube, sphere) MUST decompose to exactly 1 part — concavity floor
// fires immediately. If this fails, the concavity metric is broken and
// no downstream test is meaningful.
//
// Discriminating tests follow once calibration is green:
//   * Dumbbell (two cubes + thin bar) → ≥ 2 parts (plane-search picks
//     the splitting axis correctly).
//   * L-shape → ≥ 2 parts.
//   * Hollow torus → ≥ 2 parts (genus-1 forces decomposition).
//   * Determinism + perf budget.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/convex/quickhull.hpp>
#include <crd/geometry/decomposition/voxelize.hpp>
#include <crd/geometry/decomposition/vhacd.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/measure.hpp>

namespace
{

using crd::geometry::decomposition::ClassificationMode;
using crd::geometry::decomposition::VhacdOptions;
using crd::geometry::decomposition::VhacdStatus;
using crd::geometry::decomposition::VoxelizationOptions;
using crd::geometry::decomposition::VoxelizationStatus;
using crd::geometry::decomposition::vhacd_decompose;
using crd::geometry::decomposition::voxelize_mesh;
using crd::geometry::mesh::TriangleMeshViewf;
using crd::math::Vec3;

// --- Mesh builders (mirror of test_voxelize.cpp; kept local so tests
// don't depend on each other) ---

struct OwnedMesh
{
    crd::containers::Array<Vec3<crd::f32>> vertices;
    crd::containers::Array<crd::u32>       indices;
    explicit OwnedMesh(crd::memory::IAllocator* a) : vertices(a), indices(a) {}
    [[nodiscard]] TriangleMeshViewf view() const noexcept
    {
        return TriangleMeshViewf{
            crd::containers::ConstSpan<Vec3<crd::f32>>{vertices.data(), vertices.size()},
            crd::containers::ConstSpan<crd::u32>{indices.data(), indices.size()},
        };
    }
};

OwnedMesh build_axis_aligned_box(crd::memory::IAllocator* alloc, Vec3<crd::f32> lo, Vec3<crd::f32> hi)
{
    OwnedMesh m(alloc);
    m.vertices.push_back({lo.x, lo.y, lo.z}); // 0
    m.vertices.push_back({hi.x, lo.y, lo.z}); // 1
    m.vertices.push_back({hi.x, hi.y, lo.z}); // 2
    m.vertices.push_back({lo.x, hi.y, lo.z}); // 3
    m.vertices.push_back({lo.x, lo.y, hi.z}); // 4
    m.vertices.push_back({hi.x, lo.y, hi.z}); // 5
    m.vertices.push_back({hi.x, hi.y, hi.z}); // 6
    m.vertices.push_back({lo.x, hi.y, hi.z}); // 7
    const crd::u32 tri[] = {
        0,2,1, 0,3,2,  4,5,6, 4,6,7,
        0,4,7, 0,7,3,  1,2,6, 1,6,5,
        0,1,5, 0,5,4,  3,7,6, 3,6,2,
    };
    for (const auto i : tri) { m.indices.push_back(i); }
    return m;
}

// Append a second mesh's geometry into `dst`. Used to build composite
// (non-convex) input shapes by stitching multiple boxes.
void append_mesh(OwnedMesh& dst, const OwnedMesh& src)
{
    const crd::u32 vbase = static_cast<crd::u32>(dst.vertices.size());
    for (const auto& v : src.vertices) { dst.vertices.push_back(v); }
    for (const auto i : src.indices)   { dst.indices.push_back(i + vbase); }
}

OwnedMesh build_unit_cube(crd::memory::IAllocator* alloc, Vec3<crd::f32> lo = {0.0F, 0.0F, 0.0F})
{
    OwnedMesh m(alloc);
    const crd::f32 x0 = lo.x;
    const crd::f32 x1 = lo.x + 1.0F;
    const crd::f32 y0 = lo.y;
    const crd::f32 y1 = lo.y + 1.0F;
    const crd::f32 z0 = lo.z;
    const crd::f32 z1 = lo.z + 1.0F;
    m.vertices.push_back({x0, y0, z0});
    m.vertices.push_back({x1, y0, z0});
    m.vertices.push_back({x1, y1, z0});
    m.vertices.push_back({x0, y1, z0});
    m.vertices.push_back({x0, y0, z1});
    m.vertices.push_back({x1, y0, z1});
    m.vertices.push_back({x1, y1, z1});
    m.vertices.push_back({x0, y1, z1});
    const crd::u32 tri[] = {
        0,2,1, 0,3,2,  4,5,6, 4,6,7,
        0,4,7, 0,7,3,  1,2,6, 1,6,5,
        0,1,5, 0,5,4,  3,7,6, 3,6,2,
    };
    for (const auto i : tri) { m.indices.push_back(i); }
    return m;
}

} // anonymous namespace

// --- CALIBRATION FIRST -----------------------------------------------

TEST_CASE("CALIBRATION: cube voxelized at res=16 -> exactly 1 part (concavity floor)",
          "[vhacd][calibration]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
const OwnedMesh cube = build_unit_cube(&alloc);
    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 16U;
    vox_opts.padding_voxels   = 1U;
    vox_opts.classify         = ClassificationMode::WindingNumber;
    const auto vox = voxelize_mesh(cube.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    VhacdOptions opts{};
    opts.max_parts     = 32U;
    opts.min_concavity = 0.05F;
    const auto r = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);

    REQUIRE(r.status == VhacdStatus::Ok);
    INFO("max_part_concavity=" << r.max_part_concavity
         << " parts=" << r.parts.size()
         << " total_input_voxels=" << r.total_input_voxels);
    CHECK(r.parts.size() == 1U);
    CHECK(r.max_part_concavity < opts.min_concavity);
    REQUIRE(r.parts[0].vertices.size() >= 4U);

}

// --- Diagnostics ----------------------------------------------------

TEST_CASE("vhacd_decompose reports EmptyGrid on empty input", "[vhacd][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    crd::geometry::decomposition::VoxelGrid empty_grid;
    crd::geometry::primitives::AABB3<crd::f32> aabb{{0,0,0},{1,1,1}};
    Vec3<crd::f32> vs{0.1F, 0.1F, 0.1F};
    VhacdOptions opts{};
    const auto r = vhacd_decompose(empty_grid, aabb, vs, opts, &alloc);
    CHECK(r.status == VhacdStatus::EmptyGrid);
    CHECK(r.parts.empty());
}

TEST_CASE("vhacd_decompose reports InvalidOptions on max_parts=0", "[vhacd][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    const OwnedMesh cube = build_unit_cube(&alloc);
    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 8U;
    const auto vox = voxelize_mesh(cube.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    VhacdOptions opts{};
    opts.max_parts = 0U;
    const auto r = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);
    CHECK(r.status == VhacdStatus::InvalidOptions);

}

// --- Discriminating: dumbbell (two cubes + bar) -> >= 2 parts ---

TEST_CASE("dumbbell (two cubes + thin bar) -> >= 2 parts (plane search picks split axis)",
          "[vhacd][discriminating]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U * 1024U);
    OwnedMesh dumbbell(&alloc);
    // Left ball: cube at x in [-1.5, -0.5], y/z in [-0.5, 0.5].
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, {-1.5F, -0.5F, -0.5F},
                                                  {-0.5F,  0.5F,  0.5F}));
    // Right ball: cube at x in [0.5, 1.5].
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, { 0.5F, -0.5F, -0.5F},
                                                  { 1.5F,  0.5F,  0.5F}));
    // Thin connecting bar: x in [-0.5, 0.5], y/z in [-0.1, 0.1].
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, {-0.5F, -0.1F, -0.1F},
                                                  { 0.5F,  0.1F,  0.1F}));

    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 24U;
    vox_opts.padding_voxels   = 1U;
    vox_opts.classify         = ClassificationMode::WindingNumber;
    const auto vox = voxelize_mesh(dumbbell.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    VhacdOptions opts{};
    opts.max_parts     = 16U;
    opts.min_concavity = 0.05F;
    const auto r = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);

    REQUIRE(r.status == VhacdStatus::Ok);
    INFO("dumbbell parts=" << r.parts.size()
         << " max_concavity=" << r.max_part_concavity);
    CHECK(r.parts.size() >= 2U);
}

// --- Discriminating: L-shape -> >= 2 parts ---

TEST_CASE("L-shape (two perpendicular boxes) -> >= 2 parts", "[vhacd][discriminating]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    OwnedMesh lshape(&alloc);
    // Long horizontal box.
    append_mesh(lshape, build_axis_aligned_box(&alloc, {0.0F, 0.0F, 0.0F},
                                                {3.0F, 1.0F, 1.0F}));
    // Short vertical leg meeting at the right end.
    append_mesh(lshape, build_axis_aligned_box(&alloc, {2.0F, 1.0F, 0.0F},
                                                {3.0F, 3.0F, 1.0F}));

    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 20U;
    vox_opts.padding_voxels   = 1U;
    vox_opts.classify         = ClassificationMode::WindingNumber;
    const auto vox = voxelize_mesh(lshape.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    VhacdOptions opts{};
    opts.max_parts     = 8U;
    opts.min_concavity = 0.05F;
    const auto r = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);

    REQUIRE(r.status == VhacdStatus::Ok);
    INFO("L-shape parts=" << r.parts.size()
         << " max_concavity=" << r.max_part_concavity);
    CHECK(r.parts.size() >= 2U);
}

// --- Determinism ---

TEST_CASE("vhacd_decompose determinism -- identical input yields identical part count",
          "[vhacd][determinism]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    OwnedMesh dumbbell(&alloc);
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, {-1.5F, -0.5F, -0.5F},
                                                  {-0.5F,  0.5F,  0.5F}));
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, { 0.5F, -0.5F, -0.5F},
                                                  { 1.5F,  0.5F,  0.5F}));
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, {-0.5F, -0.1F, -0.1F},
                                                  { 0.5F,  0.1F,  0.1F}));

    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 20U;
    vox_opts.padding_voxels   = 1U;
    const auto vox = voxelize_mesh(dumbbell.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    VhacdOptions opts{};
    opts.max_parts = 8U;
    const auto a = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);
    const auto b = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);
    REQUIRE(a.status == VhacdStatus::Ok);
    REQUIRE(b.status == VhacdStatus::Ok);
    CHECK(a.parts.size() == b.parts.size());
    CHECK(a.max_part_concavity == b.max_part_concavity);
}

// --- Perf budget ---

TEST_CASE("vhacd_decompose perf budget -- dumbbell res=16 under 5000 ms",
          "[vhacd][perf]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    OwnedMesh dumbbell(&alloc);
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, {-1.5F, -0.5F, -0.5F},
                                                  {-0.5F,  0.5F,  0.5F}));
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, { 0.5F, -0.5F, -0.5F},
                                                  { 1.5F,  0.5F,  0.5F}));
    append_mesh(dumbbell, build_axis_aligned_box(&alloc, {-0.5F, -0.1F, -0.1F},
                                                  { 0.5F,  0.1F,  0.1F}));

    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 16U;
    vox_opts.padding_voxels   = 1U;
    vox_opts.classify         = ClassificationMode::FloodFill; // skip the slow winding pass
    const auto vox = voxelize_mesh(dumbbell.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    VhacdOptions opts{};
    opts.max_parts       = 8U;
    opts.splits_per_axis = 8U;
    CRD_PERF_BUDGET_LE("vhacd_dumbbell_res16", 5000.0, [&]{
        const auto r = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world, opts, &alloc);
        REQUIRE(r.status == VhacdStatus::Ok);
    });
}

// --- v9c-close: eylem v1c convex-collider-conditioning stub integration smoke -----------
//
// Mimics the call site eylem v1c (paused) will use once it resumes. The
// flow is: triangle mesh -> voxelize -> vhacd_decompose -> per-part
// ConvexHullView<f32> -> minimal "is this hull usable as a collider?"
// check (vertices + faces non-empty + contains-point operates without
// crashing on a known-interior point).
//
// Lives here (not in tests/eylem/) because eylem v1c doesn't exist
// yet. Runs as part of the geometry-decomposition test binary; if the
// API surface drifts after eylem v1c lands, the smoke is the canary.
// Per `feedback_per_slice_run_ctest.md` per-sub-module eylem-stub
// integration smoke practice (locked 2026-05-15).

TEST_CASE("v9c-close: eylem v1c convex-collider conditioning stub -- end-to-end pipeline",
          "[vhacd][close][eylem-stub]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    // The eylem v1c "Collider::ConvexHull" pipeline: take an arbitrary
    // mesh (here: an L-shape — guaranteed non-convex so we exercise the
    // decompose path, not the trivial one-part fall-through).
    OwnedMesh lshape(&alloc);
    append_mesh(lshape, build_axis_aligned_box(&alloc, {0.0F, 0.0F, 0.0F},
                                                {3.0F, 1.0F, 1.0F}));
    append_mesh(lshape, build_axis_aligned_box(&alloc, {2.0F, 1.0F, 0.0F},
                                                {3.0F, 3.0F, 1.0F}));

    // Step 1: voxelize at conservative-quality res (matches eylem v1c
    // default for game collider conditioning).
    VoxelizationOptions vox_opts{};
    vox_opts.fixed_resolution = 16U;
    vox_opts.padding_voxels   = 1U;
    vox_opts.classify         = ClassificationMode::WindingNumber;
    const auto vox = voxelize_mesh(lshape.view(), vox_opts, &alloc);
    REQUIRE(vox.status == VoxelizationStatus::Ok);

    // Step 2: decompose to N convex parts.
    VhacdOptions vhacd_opts{};
    vhacd_opts.max_parts     = 8U;
    vhacd_opts.min_concavity = 0.05F;
    const auto vhacd = vhacd_decompose(vox.grid, vox.grid_aabb, vox.voxel_size_world,
                                          vhacd_opts, &alloc);
    REQUIRE(vhacd.status == VhacdStatus::Ok);
    REQUIRE(vhacd.parts.size() >= 2U); // L-shape must split

    // Step 3: per-part ConvexHullView extraction (the actual eylem v1c
    // entry-shape — `Collider::ConvexHull` will hold one of these per
    // part along with a transform).
    for (crd::usize p = 0U; p < vhacd.parts.size(); ++p)
    {
        const auto& part = vhacd.parts[p];
        const auto view  = crd::geometry::convex::convex_hull_view_of(part);

        // Eylem GJK + SAT both require: at least 4 vertices, at least
        // 4 faces. A degenerate hull (coplanar, colinear) would mean
        // the conditioner produced an unusable collider — fail loudly.
        INFO("part " << p << " vertex_count=" << view.vertices.size()
             << " face_count=" << view.faces.size());
        CHECK(view.vertices.size() >= 4U);
        CHECK(view.faces.size()    >= 4U);
        CHECK_FALSE(part.is_coplanar);
        CHECK_FALSE(part.is_colinear);
        CHECK_FALSE(part.is_coincident);

        // The minimal GJK-style operation: contains(hull, centroid).
        // The vertex-mean is always inside (or on the boundary of) any
        // convex hull of those vertices — so `contains` must return
        // true if the hull is well-formed.
        crd::math::Vec3<crd::f32> centroid{0.0F, 0.0F, 0.0F};
        for (const auto& v : view.vertices)
        {
            centroid.x += v.x;
            centroid.y += v.y;
            centroid.z += v.z;
        }
        const crd::f32 inv_n = 1.0F / static_cast<crd::f32>(view.vertices.size());
        centroid.x *= inv_n;
        centroid.y *= inv_n;
        centroid.z *= inv_n;
        CHECK(crd::geometry::primitives::contains(view, centroid));
    }
}
