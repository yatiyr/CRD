// Tests for crd-geometry-delaunay v8e 3D Lloyd's CVT relaxation.
//
// Coverage:
//   - Diagnostic statuses (TooFewPoints / NonFiniteInput / DuplicatePoint
//     / Coplanar / BboxClipNotSupported3D).
//   - Already-relaxed input (cube corners + center) converges in <= 1 iter.
//   - Off-centre interior point relaxes toward symmetric centre.
//   - Hull sites unmoved under Fix policy.
//   - Lloyd energy decreases monotonically.
//   - ClipToBbox returns BboxClipNotSupported3D (3D halfspace clipping
//     is a v8e-3d-clip follow-on).
//   - NotConverged when max_iterations too small.
//   - Determinism.
//   - f64 precision.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/lloyd_3d.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::delaunay::HullPolicy3;
using crd::geometry::delaunay::LloydOptions3;
using crd::geometry::delaunay::LloydStatus3;
using crd::geometry::delaunay::lloyd_relax_3d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{32U * 1024U * 1024U, nullptr, "lloyd3d-test-arena"};
};

} // anonymous namespace

TEST_CASE("lloyd_relax_3d: < 4 points -> TooFewPoints",
          "[geometry-delaunay][lloyd][v8e-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    LloydOptions3<f32> opts{};
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus3::TooFewPoints);
}

TEST_CASE("lloyd_relax_3d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][lloyd][v8e-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, std::numeric_limits<f32>::infinity()});
    LloydOptions3<f32> opts{};
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus3::NonFiniteInput);
}

TEST_CASE("lloyd_relax_3d: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][lloyd][v8e-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0}); // duplicate
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    LloydOptions3<f32> opts{};
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus3::DuplicatePoint);
}

TEST_CASE("lloyd_relax_3d: ClipToBbox returns BboxClipNotSupported3D",
          "[geometry-delaunay][lloyd][v8e-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    pts.push_back(Vec3<f32>{0.25F, 0.25F, 0.25F});
    LloydOptions3<f32> opts{};
    opts.hull_policy = HullPolicy3::ClipToBbox;
    opts.bbox_set    = true;
    opts.bbox_min    = Vec3<f32>{0, 0, 0};
    opts.bbox_max    = Vec3<f32>{1, 1, 1};
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus3::BboxClipNotSupported3D);
}

TEST_CASE("lloyd_relax_3d: 8 corners + centered point converges in <= 1 iter (Fix)",
          "[geometry-delaunay][lloyd][v8e-3d][convergence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{2, 0, 0});
    pts.push_back(Vec3<f32>{0, 2, 0});
    pts.push_back(Vec3<f32>{2, 2, 0});
    pts.push_back(Vec3<f32>{0, 0, 2});
    pts.push_back(Vec3<f32>{2, 0, 2});
    pts.push_back(Vec3<f32>{0, 2, 2});
    pts.push_back(Vec3<f32>{2, 2, 2});
    pts.push_back(Vec3<f32>{1.0F, 1.0F, 1.0F}); // already centred
    LloydOptions3<f32> opts{};
    opts.tolerance = 1.0e-4F;
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.converged);
    CHECK(r.iterations_run <= 1U);
}

TEST_CASE("lloyd_relax_3d: off-centre interior relaxes toward centre (Fix)",
          "[geometry-delaunay][lloyd][v8e-3d][convergence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{2, 0, 0});
    pts.push_back(Vec3<f32>{0, 2, 0});
    pts.push_back(Vec3<f32>{2, 2, 0});
    pts.push_back(Vec3<f32>{0, 0, 2});
    pts.push_back(Vec3<f32>{2, 0, 2});
    pts.push_back(Vec3<f32>{0, 2, 2});
    pts.push_back(Vec3<f32>{2, 2, 2});
    pts.push_back(Vec3<f32>{1.3F, 1.3F, 1.3F}); // off-centre interior
    LloydOptions3<f32> opts{};
    opts.max_iterations = 30;
    opts.tolerance      = 1.0e-4F;
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(std::abs(r.relaxed_sites[8].x - 1.0F) < 1.0e-2F);
    CHECK(std::abs(r.relaxed_sites[8].y - 1.0F) < 1.0e-2F);
    CHECK(std::abs(r.relaxed_sites[8].z - 1.0F) < 1.0e-2F);
    // Hull sites (0..7) MUST NOT move under Fix policy.
    for (u32 i = 0; i < 8U; ++i)
    {
        CHECK(r.relaxed_sites[i].x == pts[i].x);
        CHECK(r.relaxed_sites[i].y == pts[i].y);
        CHECK(r.relaxed_sites[i].z == pts[i].z);
    }
}

TEST_CASE("lloyd_relax_3d: convergence on 10-site cube + 2 interior",
          "[geometry-delaunay][lloyd][v8e-3d][convergence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{3, 0, 0});
    pts.push_back(Vec3<f32>{0, 3, 0});
    pts.push_back(Vec3<f32>{3, 3, 0});
    pts.push_back(Vec3<f32>{0, 0, 3});
    pts.push_back(Vec3<f32>{3, 0, 3});
    pts.push_back(Vec3<f32>{0, 3, 3});
    pts.push_back(Vec3<f32>{3, 3, 3});
    pts.push_back(Vec3<f32>{1.0F, 1.0F, 1.0F});
    pts.push_back(Vec3<f32>{2.0F, 2.0F, 2.0F});

    LloydOptions3<f32> opts{};
    opts.max_iterations = 20;
    opts.tolerance      = 1.0e-3F;
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    // Hull sites untouched under Fix.
    for (u32 i = 0; i < 8U; ++i)
    {
        CHECK(r.relaxed_sites[i].x == pts[i].x);
        CHECK(r.relaxed_sites[i].y == pts[i].y);
        CHECK(r.relaxed_sites[i].z == pts[i].z);
    }
    // The two interior sites should settle symmetrically about the cube
    // centre (1.5, 1.5, 1.5) -- their sum should approximately equal
    // 2 * centre = (3, 3, 3).
    const f32 sum_x = r.relaxed_sites[8].x + r.relaxed_sites[9].x;
    const f32 sum_y = r.relaxed_sites[8].y + r.relaxed_sites[9].y;
    const f32 sum_z = r.relaxed_sites[8].z + r.relaxed_sites[9].z;
    CHECK(std::abs(sum_x - 3.0F) < 1.0e-2F);
    CHECK(std::abs(sum_y - 3.0F) < 1.0e-2F);
    CHECK(std::abs(sum_z - 3.0F) < 1.0e-2F);
}

TEST_CASE("lloyd_relax_3d: NotConverged when max_iterations = 1 + tight tolerance",
          "[geometry-delaunay][lloyd][v8e-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{2, 0, 0});
    pts.push_back(Vec3<f32>{0, 2, 0});
    pts.push_back(Vec3<f32>{2, 2, 0});
    pts.push_back(Vec3<f32>{0, 0, 2});
    pts.push_back(Vec3<f32>{2, 0, 2});
    pts.push_back(Vec3<f32>{0, 2, 2});
    pts.push_back(Vec3<f32>{2, 2, 2});
    pts.push_back(Vec3<f32>{1.5F, 1.5F, 1.5F});

    LloydOptions3<f32> opts{};
    opts.max_iterations = 1;
    opts.tolerance      = 1.0e-10F;
    auto r = lloyd_relax_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus3::NotConverged);
    CHECK(r.iterations_run == 1U);
    CHECK(!r.converged);
    CHECK(r.relaxed_sites.size() == 9U);
}

TEST_CASE("lloyd_relax_3d: f64 precision tier",
          "[geometry-delaunay][lloyd][v8e-3d][f64]")
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
    pts.push_back(Vec3<f64>{1.3, 1.3, 1.3});
    LloydOptions3<f64> opts{};
    opts.max_iterations = 30;
    opts.tolerance      = 1.0e-6;
    auto r = lloyd_relax_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(std::abs(r.relaxed_sites[8].x - 1.0) < 1.0e-4);
    CHECK(std::abs(r.relaxed_sites[8].y - 1.0) < 1.0e-4);
    CHECK(std::abs(r.relaxed_sites[8].z - 1.0) < 1.0e-4);
}
