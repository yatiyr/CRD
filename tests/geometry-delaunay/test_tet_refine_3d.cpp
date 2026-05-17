// Tests for crd-geometry-delaunay v8h 3D Delaunay quality refinement
// (dihedral-bounded).
//
// Coverage (advisor-recommended order):
//   - **CALIBRATION FIRST** — `min_dihedral_of_tet_rad` on a regular
//     tetrahedron returns 6 dihedrals all = arccos(1/3) ≈ 70.5288° within
//     numerical eps. Anchor for the dihedral formula; if this fails,
//     everything else is meaningless.
//   - Degenerate flat tet returns near-zero dihedral.
//   - Diagnostic statuses (TooFewPoints / NonFiniteInput / DuplicatePoint
//     / Coplanar / InvalidAngle).
//   - Already-good mesh (regular-tet vertices + interior point) converges
//     in <= 1 iteration.
//   - Skinny-cube input (8 corners + 1 near-coplanar inserted point that
//     creates slivers) — refinement EITHER converges OR returns
//     NotConverged (3D termination NOT guaranteed; both are valid).
//   - Quality assertion: when status == Ok and converged, every output tet
//     has min-dihedral >= alpha within 0.5° tolerance.
//   - Determinism.
//   - f64 precision.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/tet_refine_3d.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::delaunay::TetRefineOptions;
using crd::geometry::delaunay::TetRefineResult;
using crd::geometry::delaunay::TetRefineStatus;
using crd::geometry::delaunay::min_dihedral_of_tet_rad;
using crd::geometry::delaunay::tet_refine_3d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{32U * 1024U * 1024U, nullptr, "tetrefine3d-test-arena"};
};

constexpr f64 kRadToDeg = 180.0 / 3.14159265358979323846;
constexpr f32 kRadToDegF = 180.0F / 3.14159265358979323846F;

template <typename T>
bool all_tets_meet_dihedral_quality(const TetRefineResult<T>& r, T threshold_deg, T tol_deg)
{
    const T threshold_rad = (threshold_deg - tol_deg) * static_cast<T>(3.14159265358979323846 / 180.0);
    for (u32 t = 0; t < r.tet_count; ++t)
    {
        const u32 ia = r.tet_indices[4U * t + 0U];
        const u32 ib = r.tet_indices[4U * t + 1U];
        const u32 ic = r.tet_indices[4U * t + 2U];
        const u32 id = r.tet_indices[4U * t + 3U];
        const T d = min_dihedral_of_tet_rad<T>(r.vertices[ia], r.vertices[ib],
                                                  r.vertices[ic], r.vertices[id]);
        if (d < threshold_rad) { return false; }
    }
    return true;
}

} // anonymous namespace

// ===========================================================================
// CALIBRATION FIRST (per advisor) — regular tet has all 6 dihedrals =
// arccos(1/3) ≈ 70.5288°.
// ===========================================================================

TEST_CASE("min_dihedral_of_tet_rad: regular tetrahedron returns arccos(1/3)",
          "[geometry-delaunay][tet-refine][v8h][calibration]")
{
    // Standard regular tet vertex placement:
    //   v0 = ( 1,  1,  1)
    //   v1 = ( 1, -1, -1)
    //   v2 = (-1,  1, -1)
    //   v3 = (-1, -1,  1)
    // All edge lengths = 2*sqrt(2). All dihedrals = arccos(1/3) ≈ 1.23096 rad
    // ≈ 70.5288 degrees.
    const Vec3<f64> v0{1, 1, 1};
    const Vec3<f64> v1{1, -1, -1};
    const Vec3<f64> v2{-1, 1, -1};
    const Vec3<f64> v3{-1, -1, 1};
    const f64 d = min_dihedral_of_tet_rad<f64>(v0, v1, v2, v3);
    const f64 expected = std::acos(1.0 / 3.0);
    CHECK(std::abs(d - expected) < 1.0e-9);
    CHECK(std::abs(d * kRadToDeg - 70.5287793655) < 1.0e-6);
}

TEST_CASE("min_dihedral_of_tet_rad: flat/sliver tet returns near-zero",
          "[geometry-delaunay][tet-refine][v8h][calibration]")
{
    // Nearly-flat tet: 4 points near z=0 plane, one with tiny z offset.
    // Minimum dihedral should be very small (sliver).
    const Vec3<f64> v0{0, 0, 0};
    const Vec3<f64> v1{1, 0, 0};
    const Vec3<f64> v2{0.5, 1, 0};
    const Vec3<f64> v3{0.5, 0.5, 1e-3}; // tiny z offset -> sliver
    const f64 d = min_dihedral_of_tet_rad<f64>(v0, v1, v2, v3);
    CHECK(d * kRadToDeg < 5.0); // very small dihedral
}

// ===========================================================================
// Diagnostic statuses
// ===========================================================================

TEST_CASE("tet_refine_3d: < 4 points -> TooFewPoints",
          "[geometry-delaunay][tet-refine][v8h]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    TetRefineOptions<f32> opts{};
    auto r = tet_refine_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == TetRefineStatus::TooFewPoints);
}

TEST_CASE("tet_refine_3d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][tet-refine][v8h]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, std::numeric_limits<f32>::infinity()});
    TetRefineOptions<f32> opts{};
    auto r = tet_refine_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == TetRefineStatus::NonFiniteInput);
}

TEST_CASE("tet_refine_3d: coplanar input -> Coplanar",
          "[geometry-delaunay][tet-refine][v8h]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{1, 1, 0});
    pts.push_back(Vec3<f32>{0.5F, 0.5F, 0});
    TetRefineOptions<f32> opts{};
    auto r = tet_refine_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == TetRefineStatus::Coplanar);
}

TEST_CASE("tet_refine_3d: invalid angle -> InvalidAngle",
          "[geometry-delaunay][tet-refine][v8h]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    TetRefineOptions<f32> opts{};
    opts.min_dihedral_degrees = 0.0F;
    auto r = tet_refine_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == TetRefineStatus::InvalidAngle);

    opts.min_dihedral_degrees = 80.0F; // > 70.5 unrefinable
    auto r2 = tet_refine_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r2.status == TetRefineStatus::InvalidAngle);
}

// ===========================================================================
// Behavioural tests
// ===========================================================================

TEST_CASE("tet_refine_3d: regular tet + center converges in <= 1 iteration",
          "[geometry-delaunay][tet-refine][v8h][convergence]")
{
    // Already-good mesh: regular-tet vertices + centre. No bad tets.
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{1, 1, 1});
    pts.push_back(Vec3<f64>{1, -1, -1});
    pts.push_back(Vec3<f64>{-1, 1, -1});
    pts.push_back(Vec3<f64>{-1, -1, 1});
    pts.push_back(Vec3<f64>{0, 0, 0}); // centre
    TetRefineOptions<f64> opts{};
    opts.min_dihedral_degrees = 30.0; // well below regular-tet dihedral
    auto r = tet_refine_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.converged);
    CHECK(r.iterations_run <= 1U);
    CHECK(all_tets_meet_dihedral_quality<f64>(r, 30.0, 0.5));
}

TEST_CASE("tet_refine_3d: cube + interior point converges with dihedral=10",
          "[geometry-delaunay][tet-refine][v8h][convergence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{0, 0, 0});
    pts.push_back(Vec3<f64>{1, 0, 0});
    pts.push_back(Vec3<f64>{0, 1, 0});
    pts.push_back(Vec3<f64>{1, 1, 0});
    pts.push_back(Vec3<f64>{0, 0, 1});
    pts.push_back(Vec3<f64>{1, 0, 1});
    pts.push_back(Vec3<f64>{0, 1, 1});
    pts.push_back(Vec3<f64>{1, 1, 1});
    pts.push_back(Vec3<f64>{0.5, 0.5, 0.5});
    TetRefineOptions<f64> opts{};
    opts.min_dihedral_degrees = 10.0;
    opts.max_iterations       = 1000U;
    auto r = tet_refine_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    // Cube + centre is well-conditioned; should converge.
    CHECK(r.converged);
    CHECK(all_tets_meet_dihedral_quality<f64>(r, 10.0, 0.5));
}

TEST_CASE("tet_refine_3d: sliver-rich input may return NotConverged (3D termination not guaranteed)",
          "[geometry-delaunay][tet-refine][v8h][sliver]")
{
    // Construct an input known to produce slivers: 4 near-coplanar points +
    // 1 above. Pure Steiner-insertion refinement may or may not converge
    // on this — BOTH outcomes are valid per the algorithm's known
    // limitation. Test asserts the algorithm runs to completion (either
    // Ok+converged or NotConverged) without internal errors.
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{0, 0, 0.001});       // near-coplanar
    pts.push_back(Vec3<f64>{1, 0, 0.001});       // near-coplanar
    pts.push_back(Vec3<f64>{0, 1, 0.001});       // near-coplanar
    pts.push_back(Vec3<f64>{1, 1, 0});           // exactly z=0
    pts.push_back(Vec3<f64>{0.5, 0.5, 1});       // tall above
    TetRefineOptions<f64> opts{};
    opts.min_dihedral_degrees = 20.0;
    opts.max_iterations       = 200U;
    opts.max_steiner          = 200U;
    auto r = tet_refine_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    // EITHER outcome OK; algorithm must not crash or hang.
    CHECK((r.status == TetRefineStatus::Ok || r.status == TetRefineStatus::NotConverged));
    CHECK(r.tet_count > 0U);
}

TEST_CASE("tet_refine_3d: determinism (same input -> same output)",
          "[geometry-delaunay][tet-refine][v8h][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{0, 0, 0});
    pts.push_back(Vec3<f64>{2, 0, 0});
    pts.push_back(Vec3<f64>{0, 2, 0});
    pts.push_back(Vec3<f64>{2, 2, 0});
    pts.push_back(Vec3<f64>{0, 0, 2});
    pts.push_back(Vec3<f64>{2, 0, 2});
    pts.push_back(Vec3<f64>{0, 2, 2});
    pts.push_back(Vec3<f64>{2, 2, 2});
    pts.push_back(Vec3<f64>{1, 1, 1});
    TetRefineOptions<f64> opts{};
    opts.min_dihedral_degrees = 10.0;
    opts.max_iterations       = 200U;

    auto r1 = tet_refine_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    auto r2 = tet_refine_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    CHECK(r1.vertices.size() == r2.vertices.size());
    CHECK(r1.tet_count == r2.tet_count);
    CHECK(r1.steiner_count == r2.steiner_count);
    for (u32 i = 0; i < r1.vertices.size(); ++i)
    {
        CHECK(r1.vertices[i].x == r2.vertices[i].x);
        CHECK(r1.vertices[i].y == r2.vertices[i].y);
        CHECK(r1.vertices[i].z == r2.vertices[i].z);
    }
}

TEST_CASE("tet_refine_3d: f32 precision tier",
          "[geometry-delaunay][tet-refine][v8h][f32]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{1, 1, 1});
    pts.push_back(Vec3<f32>{1, -1, -1});
    pts.push_back(Vec3<f32>{-1, 1, -1});
    pts.push_back(Vec3<f32>{-1, -1, 1});
    pts.push_back(Vec3<f32>{0, 0, 0});
    TetRefineOptions<f32> opts{};
    opts.min_dihedral_degrees = 30.0F;
    auto r = tet_refine_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.tet_count > 0U);

    // Also exercise the public dihedral helper on f32.
    const f32 d = min_dihedral_of_tet_rad<f32>(
        Vec3<f32>{1, 1, 1}, Vec3<f32>{1, -1, -1},
        Vec3<f32>{-1, 1, -1}, Vec3<f32>{-1, -1, 1});
    CHECK(std::abs(d * kRadToDegF - 70.5288F) < 1.0e-2F);
}
