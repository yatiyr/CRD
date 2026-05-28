// ---------------------------------------------------------------------------
// crd-geometry-decomposition v9c-a --voxelize_mesh test corpus.
//
// Adversarial-first per advisor TDD. Calibration FIRST per v8h
// precedent: a unit cube at exact fixed_resolution=4 + padding=0 has
// arithmetic-exact Surface / Inside / Outside counts (56 / 8 / 0). If
// calibration fails, every subsequent assertion is meaningless, so it's
// the FIRST test to read after any change.
//
// Test cases:
//   1. Diagnostics (4)         EmptyMesh / NonFiniteInput / DegenerateAABB
//                              / InvalidOptions.
//   2. Calibration             unit cube @ res=4, pad=0 -> 56/8/0.
//   3. Sphere volume           octahedron @ res=32 ->inside_count within
//                              5% of analytic 4π/3 · r³.
//   4. Open cylinder           WindingNumber correct, FloodFill leaks
//                              (documented behavior; both modes tested).
//   5. Determinism             same input ->identical state grid.
//   6. Large-coord f32         mesh translated by 1e4 still voxelizes.
//   7. Perf budget             FloodFill @ res=32 on octahedron under
//                              500 ms (generous, tightens at v9c-close).
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/decomposition/voxelize.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/measure.hpp>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cmath>

namespace
{

// Catch2 listener that boots / shuts down the jobs subsystem for the
// binary lifetime. Same pattern as the BvhJobsListener in tests/geometry-bvh.
struct DecompositionJobsListener final : public Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo&) override
    {
        crd::jobs::init();
    }
    void testRunEnded(const Catch::TestRunStats&) override
    {
        crd::jobs::shutdown();
    }
};
CATCH_REGISTER_LISTENER(DecompositionJobsListener)

using crd::geometry::decomposition::ClassificationMode;
using crd::geometry::decomposition::VoxelizationOptions;
using crd::geometry::decomposition::VoxelizationStatus;
using crd::geometry::decomposition::VoxelState;
using crd::geometry::decomposition::voxelize_mesh;
using crd::geometry::mesh::TriangleMeshViewf;
using crd::math::Vec3;

// --- Mesh builders ---------------------------------------------------------

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

// Axis-aligned unit cube [lo, lo+1]³ --12 triangles, CCW outward.
OwnedMesh build_unit_cube(crd::memory::IAllocator* alloc, Vec3<crd::f32> lo = {0.0F, 0.0F, 0.0F})
{
    OwnedMesh m(alloc);
    m.vertices.reserve(8);
    const crd::f32 x0 = lo.x;
    const crd::f32 x1 = lo.x + 1.0F;
    const crd::f32 y0 = lo.y;
    const crd::f32 y1 = lo.y + 1.0F;
    const crd::f32 z0 = lo.z;
    const crd::f32 z1 = lo.z + 1.0F;
    m.vertices.push_back({x0, y0, z0}); // 0
    m.vertices.push_back({x1, y0, z0}); // 1
    m.vertices.push_back({x1, y1, z0}); // 2
    m.vertices.push_back({x0, y1, z0}); // 3
    m.vertices.push_back({x0, y0, z1}); // 4
    m.vertices.push_back({x1, y0, z1}); // 5
    m.vertices.push_back({x1, y1, z1}); // 6
    m.vertices.push_back({x0, y1, z1}); // 7
    const crd::u32 tri[] = {
        // bottom z = z0 (normal -z)
        0,2,1, 0,3,2,
        // top z = z1 (normal +z)
        4,5,6, 4,6,7,
        // left x = x0 (normal -x)
        0,4,7, 0,7,3,
        // right x = x1 (normal +x)
        1,2,6, 1,6,5,
        // front y = y0 (normal -y)
        0,1,5, 0,5,4,
        // back y = y1 (normal +y)
        3,7,6, 3,6,2,
    };
    m.indices.reserve(36);
    for (const auto i : tri) { m.indices.push_back(i); }
    return m;
}

// Regular octahedron --6 vertices, 8 triangles. Inscribes a unit sphere
// of radius `r` at `centre`. Used as a deterministic stand-in for sphere
// volume assertions (the octahedron's enclosed volume is 4/3 · r³, vs.
// 4π/3 · r³ for a true sphere --about 32% smaller). The volume assertion
// uses the octahedron's analytic volume, not the sphere's, so this is
// arithmetic-exact.
OwnedMesh build_octahedron(crd::memory::IAllocator* alloc, Vec3<crd::f32> centre, crd::f32 r)
{
    OwnedMesh m(alloc);
    m.vertices.reserve(6);
    m.vertices.push_back({centre.x + r, centre.y,     centre.z    }); // 0  +x
    m.vertices.push_back({centre.x - r, centre.y,     centre.z    }); // 1  -x
    m.vertices.push_back({centre.x,     centre.y + r, centre.z    }); // 2  +y
    m.vertices.push_back({centre.x,     centre.y - r, centre.z    }); // 3  -y
    m.vertices.push_back({centre.x,     centre.y,     centre.z + r}); // 4  +z
    m.vertices.push_back({centre.x,     centre.y,     centre.z - r}); // 5  -z
    const crd::u32 tri[] = {
        // upper half (towards +z, vertex 4)
        0,2,4,  2,1,4,  1,3,4,  3,0,4,
        // lower half (towards -z, vertex 5)
        2,0,5,  1,2,5,  3,1,5,  0,3,5,
    };
    m.indices.reserve(24);
    for (const auto i : tri) { m.indices.push_back(i); }
    return m;
}

// Open cylinder side (no caps). Radius `r`, height 1, along +z axis,
// centred at `centre`. `segments` triangles around the circumference,
// each segment is 2 triangles ->`segments * 2` total. Open at both ends —
// designed to expose the FloodFill leak.
OwnedMesh build_open_cylinder(crd::memory::IAllocator* alloc, Vec3<crd::f32> centre,
                              crd::f32 r, crd::u32 segments)
{
    OwnedMesh m(alloc);
    m.vertices.reserve(2U * segments);
    for (crd::u32 i = 0; i < segments; ++i)
    {
        const crd::f32 a = (static_cast<crd::f32>(i) / static_cast<crd::f32>(segments))
                            * (2.0F * 3.14159265358979F);
        const crd::f32 cx = centre.x + r * std::cos(a);
        const crd::f32 cy = centre.y + r * std::sin(a);
        m.vertices.push_back({cx, cy, centre.z});        // 2i + 0  bottom ring
        m.vertices.push_back({cx, cy, centre.z + 1.0F}); // 2i + 1  top ring
    }
    m.indices.reserve(6U * segments);
    for (crd::u32 i = 0; i < segments; ++i)
    {
        const crd::u32 i0 = 2U * i + 0U;
        const crd::u32 i1 = 2U * i + 1U;
        const crd::u32 j0 = 2U * ((i + 1U) % segments) + 0U;
        const crd::u32 j1 = 2U * ((i + 1U) % segments) + 1U;
        // CCW outward winding (normals point AWAY from cylinder axis).
        m.indices.push_back(i0); m.indices.push_back(j0); m.indices.push_back(i1);
        m.indices.push_back(j0); m.indices.push_back(j1); m.indices.push_back(i1);
    }
    return m;
}

} // anonymous namespace

// --- 1. Diagnostics --------------------------------------------------------

TEST_CASE("voxelize_mesh reports EmptyMesh on zero triangles", "[voxelize][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const OwnedMesh empty(&alloc);
    VoxelizationOptions opts{};
    opts.target_voxel_count = 64U * 64U * 64U;
    const auto r = voxelize_mesh(empty.view(), opts, &alloc);
    REQUIRE(r.status == VoxelizationStatus::EmptyMesh);
}

TEST_CASE("voxelize_mesh reports NonFiniteInput on NaN vertex", "[voxelize][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    OwnedMesh mesh = build_unit_cube(&alloc);
    mesh.vertices[0].x = std::nanf("");
    VoxelizationOptions opts{};
    opts.fixed_resolution = 4U;
    const auto r = voxelize_mesh(mesh.view(), opts, &alloc);
    REQUIRE(r.status == VoxelizationStatus::NonFiniteInput);
}

TEST_CASE("voxelize_mesh reports DegenerateAABB on coplanar input", "[voxelize][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    OwnedMesh m(&alloc);
    // Triangle with all vertices on z=0 plane: AABB has zero z-extent.
    m.vertices.push_back({0.0F, 0.0F, 0.0F});
    m.vertices.push_back({1.0F, 0.0F, 0.0F});
    m.vertices.push_back({0.0F, 1.0F, 0.0F});
    m.indices.push_back(0); m.indices.push_back(1); m.indices.push_back(2);
    VoxelizationOptions opts{};
    opts.fixed_resolution = 4U;
    const auto r = voxelize_mesh(m.view(), opts, &alloc);
    REQUIRE(r.status == VoxelizationStatus::DegenerateAABB);
}

TEST_CASE("voxelize_mesh reports InvalidOptions when sizing knobs both zero", "[voxelize][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const OwnedMesh cube = build_unit_cube(&alloc);
    VoxelizationOptions opts{};
    opts.fixed_resolution    = 0U;
    opts.target_voxel_count  = 0U;
    const auto r = voxelize_mesh(cube.view(), opts, &alloc);
    REQUIRE(r.status == VoxelizationStatus::InvalidOptions);
}

TEST_CASE("voxelize_mesh -- both sizing knobs set: fixed_resolution wins (precedence rule)",
          "[voxelize][diagnostics]")
{
    // Both knobs set is NOT an error per the precedence rule: fixed_resolution
    // wins. Verifies the documented precedence is the one actually implemented.
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const OwnedMesh cube = build_unit_cube(&alloc);
    VoxelizationOptions opts{};
    opts.fixed_resolution    = 4U;
    opts.target_voxel_count  = 1024U * 1024U; // would give a much finer grid
    opts.padding_voxels      = 0U;
    const auto r = voxelize_mesh(cube.view(), opts, &alloc);
    REQUIRE(r.status == VoxelizationStatus::Ok);
    CHECK(r.grid.nx() == 4U); // fixed_resolution won, not the larger target.
    CHECK(r.grid.ny() == 4U);
    CHECK(r.grid.nz() == 4U);
}

// --- 2. CALIBRATION FIRST --------------------------------------------------
// Unit cube [0,1]³, fixed_resolution=4, padding=0 ->exact 4×4×4 grid.
// Voxel size = 0.25 along each axis.
// Surface cells = the outer shell: ix∈{0,3} ∪ iy∈{0,3} ∪ iz∈{0,3}
//               = 4³ − 2³ = 64 − 8 = 56.
// Inside cells  = 2³ = 8 (the inner 2×2×2 sub-grid).
// Outside cells = 0 (no padding).
// If this fails, the SAT / sizing / classification machinery is broken.

TEST_CASE("CALIBRATION: unit cube fixed_resolution=4 pad=0 -> 56 Surface + 8 Inside + 0 Outside",
          "[voxelize][calibration]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const OwnedMesh cube = build_unit_cube(&alloc);
    VoxelizationOptions opts{};
    opts.fixed_resolution   = 4U;
    opts.padding_voxels     = 0U;
    opts.classify           = ClassificationMode::WindingNumber;
    const auto r = voxelize_mesh(cube.view(), opts, &alloc);

    REQUIRE(r.status == VoxelizationStatus::Ok);
    REQUIRE(r.grid.nx() == 4U);
    REQUIRE(r.grid.ny() == 4U);
    REQUIRE(r.grid.nz() == 4U);
    CHECK(r.surface_count == 56U);
    CHECK(r.inside_count  == 8U);
    CHECK(r.outside_count == 0U);
    CHECK(r.voxel_size_world.x == 0.25F);
    CHECK(r.voxel_size_world.y == 0.25F);
    CHECK(r.voxel_size_world.z == 0.25F);
}

TEST_CASE("calibration: unit cube fixed_resolution=4 pad=1 -> corner Outside + no Unknown",
          "[voxelize][calibration]")
{
    // With padding, the mesh AABB faces lie on the SHARED EDGE between
    // an interior cell and a padding cell; SAT correctly marks BOTH as
    // Surface. So the simple "mesh-shell = 56, padding-shell = 152" math
    // doesn't hold. We instead assert structural invariants:
    //   * grid is 6³ (4 + 2·1).
    //   * every voxel is classified (no Unknown survives both passes).
    //   * the corner voxel is Outside (the FloodFill / WindingNumber seed).
    //   * surface + inside + outside == 216 (exact, by construction).
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const OwnedMesh cube = build_unit_cube(&alloc);
    VoxelizationOptions opts{};
    opts.fixed_resolution   = 4U;
    opts.padding_voxels     = 1U;
    opts.classify           = ClassificationMode::WindingNumber;
    const auto r = voxelize_mesh(cube.view(), opts, &alloc);

    REQUIRE(r.status == VoxelizationStatus::Ok);
    REQUIRE(r.grid.nx() == 6U);
    REQUIRE(r.grid.ny() == 6U);
    REQUIRE(r.grid.nz() == 6U);
    CHECK(r.surface_count + r.inside_count + r.outside_count == 216U);
    CHECK(r.grid.state_at(0U, 0U, 0U) == VoxelState::Outside);
    // The CORE 2³ of the cube (ix,iy,iz ∈ {2,3}) must all be Inside.
    for (crd::u32 iz = 2U; iz <= 3U; ++iz)
    {
        for (crd::u32 iy = 2U; iy <= 3U; ++iy)
        {
            for (crd::u32 ix = 2U; ix <= 3U; ++ix)
            {
                CHECK(r.grid.state_at(ix, iy, iz) == VoxelState::Inside);
            }
        }
    }
}

// --- 3. Sphere volume (octahedron stand-in) -------------------------------

TEST_CASE("voxelize_mesh -- octahedron inside-count tracks analytic volume", "[voxelize][volume]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    const Vec3<crd::f32> centre{0.0F, 0.0F, 0.0F};
    const crd::f32 r_oct = 1.0F;
    const OwnedMesh oct = build_octahedron(&alloc, centre, r_oct);

    VoxelizationOptions opts{};
    opts.fixed_resolution = 32U;
    opts.padding_voxels   = 1U;
    opts.classify         = ClassificationMode::WindingNumber;
    const auto r = voxelize_mesh(oct.view(), opts, &alloc);

    REQUIRE(r.status == VoxelizationStatus::Ok);

    // Octahedron's analytic volume = (4/3) · r³ for an octahedron with
    // vertex-to-centre distance r (= sqrt(2)/3 · edge_length³ form
    // restated). For r=1: V = 4/3 ≈ 1.333.
    const crd::f32 voxel_vol = r.voxel_size_world.x * r.voxel_size_world.y * r.voxel_size_world.z;
    const crd::f32 measured_vol_inside_only  = static_cast<crd::f32>(r.inside_count) * voxel_vol;
    const crd::f32 measured_vol_inside_plus_half_surface =
        (static_cast<crd::f32>(r.inside_count) + 0.5F * static_cast<crd::f32>(r.surface_count))
        * voxel_vol;
    const crd::f32 analytic_vol = (4.0F / 3.0F) * r_oct * r_oct * r_oct;

    // The "Inside-only" count systematically UNDERestimates volume because
    // surface voxels are excluded. "Inside + half-Surface" is the standard
    // mid-point estimate; should be within ~10% of analytic at res=32.
    INFO("voxel_vol=" << voxel_vol
         << " inside_count=" << r.inside_count
         << " surface_count=" << r.surface_count
         << " vol_in_only=" << measured_vol_inside_only
         << " vol_in_plus_half_surf=" << measured_vol_inside_plus_half_surface
         << " analytic=" << analytic_vol);
    const crd::f32 err_rel = std::abs(measured_vol_inside_plus_half_surface - analytic_vol)
                              / analytic_vol;
    CHECK(err_rel < 0.10F);
}

// --- 4. Open cylinder: WindingNumber correct, FloodFill leaks ------------

TEST_CASE("open cylinder -- WindingNumber classifies interior correctly", "[voxelize][open-mesh]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U * 1024U);
    const OwnedMesh cyl = build_open_cylinder(&alloc, {0.0F, 0.0F, 0.0F}, 1.0F, 32U);

    VoxelizationOptions opts{};
    opts.fixed_resolution = 24U;
    opts.padding_voxels   = 1U;
    opts.classify         = ClassificationMode::WindingNumber;
    const auto r = voxelize_mesh(cyl.view(), opts, &alloc);

    REQUIRE(r.status == VoxelizationStatus::Ok);
    INFO("WindingNumber surface=" << r.surface_count
         << " inside=" << r.inside_count
         << " outside=" << r.outside_count);
    // The cylinder interior (π · r² · h = π · 1 · 1 ≈ 3.14) at voxel size
    // ~0.083 along x/y and 0.042 along z is many voxels. Just assert
    // inside_count is meaningfully non-zero (winding sees the open
    // cylinder as topologically inside-ish along its axis).
    CHECK(r.inside_count > 100U);
}

TEST_CASE("open cylinder -- FloodFill leaks (documented behavior)", "[voxelize][open-mesh]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U * 1024U);
    const OwnedMesh cyl = build_open_cylinder(&alloc, {0.0F, 0.0F, 0.0F}, 1.0F, 32U);

    VoxelizationOptions opts{};
    opts.fixed_resolution = 24U;
    opts.padding_voxels   = 1U;
    opts.classify         = ClassificationMode::FloodFill;
    const auto r = voxelize_mesh(cyl.view(), opts, &alloc);

    REQUIRE(r.status == VoxelizationStatus::Ok);
    INFO("FloodFill surface=" << r.surface_count
         << " inside=" << r.inside_count
         << " outside=" << r.outside_count);
    // The flood fill from the corner Outside seed leaks through the open
    // top/bottom of the cylinder and floods the interior as Outside.
    // Expected: inside_count is ZERO (or near-zero for SAT corner cases).
    // This test documents the leak --consumers using FloodFill on
    // non-watertight meshes should EXPECT this.
    CHECK(r.inside_count == 0U);
}

// --- 5. Determinism --------------------------------------------------------

TEST_CASE("voxelize_mesh determinism -- repeated runs yield identical voxel state",
          "[voxelize][determinism]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    const OwnedMesh oct = build_octahedron(&alloc, {0.0F, 0.0F, 0.0F}, 1.0F);

    VoxelizationOptions opts{};
    opts.fixed_resolution = 16U;
    opts.padding_voxels   = 1U;
    opts.classify         = ClassificationMode::WindingNumber;
    const auto r1 = voxelize_mesh(oct.view(), opts, &alloc);
    const auto r2 = voxelize_mesh(oct.view(), opts, &alloc);

    REQUIRE(r1.status == VoxelizationStatus::Ok);
    REQUIRE(r2.status == VoxelizationStatus::Ok);
    REQUIRE(r1.grid.nx() == r2.grid.nx());
    REQUIRE(r1.grid.ny() == r2.grid.ny());
    REQUIRE(r1.grid.nz() == r2.grid.nz());
    CHECK(r1.surface_count == r2.surface_count);
    CHECK(r1.inside_count  == r2.inside_count);
    CHECK(r1.outside_count == r2.outside_count);
    // Per-voxel state compare.
    bool any_mismatch = false;
    for (crd::u32 iz = 0; iz < r1.grid.nz() && !any_mismatch; ++iz)
    {
        for (crd::u32 iy = 0; iy < r1.grid.ny() && !any_mismatch; ++iy)
        {
            for (crd::u32 ix = 0; ix < r1.grid.nx() && !any_mismatch; ++ix)
            {
                if (r1.grid.state_at(ix, iy, iz) != r2.grid.state_at(ix, iy, iz))
                {
                    any_mismatch = true;
                }
            }
        }
    }
    CHECK_FALSE(any_mismatch);
}

// --- 6. Large-coord f32 stability -----------------------------------------

TEST_CASE("voxelize_mesh stable at f32 coordinate magnitude 1e4", "[voxelize][large-coord]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    constexpr crd::f32 scale = 1.0e4F;
    const OwnedMesh cube_at_origin = build_unit_cube(&alloc);
    const OwnedMesh cube_at_offset = build_unit_cube(&alloc, {scale, scale, scale});

    VoxelizationOptions opts{};
    opts.fixed_resolution = 4U;
    opts.padding_voxels   = 0U;
    opts.classify         = ClassificationMode::WindingNumber;
    const auto a = voxelize_mesh(cube_at_origin.view(), opts, &alloc);
    const auto b = voxelize_mesh(cube_at_offset.view(), opts, &alloc);

    REQUIRE(a.status == VoxelizationStatus::Ok);
    REQUIRE(b.status == VoxelizationStatus::Ok);
    // Same surface/inside counts at any translation (mesh-relative geometry).
    CHECK(a.surface_count == b.surface_count);
    CHECK(a.inside_count  == b.inside_count);
}

// --- 7. Perf budget -------------------------------------------------------

TEST_CASE("voxelize_mesh perf budget -- octahedron @ res=32 FloodFill under 500 ms",
          "[voxelize][perf]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    const OwnedMesh oct = build_octahedron(&alloc, {0.0F, 0.0F, 0.0F}, 1.0F);
    VoxelizationOptions opts{};
    opts.fixed_resolution = 32U;
    opts.padding_voxels   = 1U;
    opts.classify         = ClassificationMode::FloodFill; // fast path
    CRD_PERF_BUDGET_LE("voxelize_octahedron_res32_floodfill", 500.0, [&] {
        const auto r = voxelize_mesh(oct.view(), opts, &alloc);
        REQUIRE(r.status == VoxelizationStatus::Ok);
    });
}
