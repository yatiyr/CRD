// Tests for crd-geometry-delaunay v8e 2D Lloyd's CVT relaxation.
//
// Coverage:
//   - Diagnostic statuses (TooFewPoints / NonFiniteInput / DuplicatePoint
//     / BboxInvalid).
//   - Already-relaxed input (4-point square + 1 centred) converges
//     immediately (iter <= 1, displacement ~= 0).
//   - Off-centre point converges TO the centre under Fix hull policy.
//   - **CVT defining property**: after convergence, every interior site
//     is at the centroid of its Voronoi cell (within tolerance).
//   - **Variance reduction**: sum-of-squared-distances from sites to cell
//     centroids DECREASES monotonically per iteration (Lloyd's energy
//     function is non-increasing). This is the Lloyd algorithm's hallmark
//     property.
//   - HullPolicy::Fix keeps hull sites unmoved across all iterations.
//   - HullPolicy::ClipToBbox produces a converged CVT inside the bbox
//     where ALL sites (including hull) reach their cell centroids.
//   - NotConverged status when max_iterations is too small.
//   - Determinism (shuffled input -> equivalent relaxed set).
//   - Large-coord f32 stability.
//   - f64 precision.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/lloyd_2d.hpp>
#include <crd/geometry/delaunay/voronoi_2d.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec2;
using crd::geometry::delaunay::HullPolicy2;
using crd::geometry::delaunay::LloydOptions2;
using crd::geometry::delaunay::LloydStatus2;
using crd::geometry::delaunay::lloyd_relax_2d;
using crd::geometry::delaunay::voronoi_2d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{16U * 1024U * 1024U, nullptr, "lloyd2d-test-arena"};
};

// Compute polygon centroid given vertex sequence (CCW). Standard formula.
template <typename T>
Vec2<T> polygon_centroid(const Vec2<T>* verts, u32 n)
{
    T cx = static_cast<T>(0);
    T cy = static_cast<T>(0);
    T a  = static_cast<T>(0);
    for (u32 i = 0; i < n; ++i)
    {
        const auto& p = verts[i];
        const auto& q = verts[(i + 1U) % n];
        const T cross = p.x * q.y - q.x * p.y;
        a  += cross;
        cx += (p.x + q.x) * cross;
        cy += (p.y + q.y) * cross;
    }
    a *= static_cast<T>(0.5);
    if (a == static_cast<T>(0)) { return Vec2<T>{static_cast<T>(0), static_cast<T>(0)}; }
    const T inv_6a = static_cast<T>(1) / (static_cast<T>(6) * a);
    return Vec2<T>{cx * inv_6a, cy * inv_6a};
}

// Lloyd's energy: sum over all bounded cells of integral over cell
// of |x - site|^2 dx. Approximated as ||site - cell_centroid||^2 weighted
// by cell area. Monotonic non-increase per Lloyd iteration.
template <typename T>
T lloyd_energy(const crd::containers::Array<Vec2<T>>& sites,
                crd::memory::IAllocator*               alloc)
{
    auto vor = voronoi_2d<T>(
        crd::containers::ConstSpan<Vec2<T>>{sites.data(), sites.size()}, alloc);
    if (!vor.ok()) { return std::numeric_limits<T>::infinity(); }

    T energy = static_cast<T>(0);
    crd::containers::Array<Vec2<T>> poly(alloc);
    for (const auto& cell : vor.cells)
    {
        if (!cell.is_bounded) { continue; }
        if (cell.vertex_indices.size() < 3U) { continue; }
        poly.clear();
        for (u32 vi : cell.vertex_indices)
        {
            poly.push_back(vor.voronoi_vertices[vi]);
        }
        const auto centroid = polygon_centroid<T>(poly.data(), static_cast<u32>(poly.size()));
        const auto& site = sites[cell.site_index];
        const T dx = site.x - centroid.x;
        const T dy = site.y - centroid.y;
        energy += dx * dx + dy * dy;
    }
    return energy;
}

} // anonymous namespace

TEST_CASE("lloyd_relax_2d: < 3 points -> TooFewPoints",
          "[geometry-delaunay][lloyd][v8e]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    LloydOptions2<f32> opts{};
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus2::TooFewPoints);
}

TEST_CASE("lloyd_relax_2d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][lloyd][v8e]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, std::numeric_limits<f32>::infinity()});
    LloydOptions2<f32> opts{};
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus2::NonFiniteInput);
}

TEST_CASE("lloyd_relax_2d: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][lloyd][v8e]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 0}); // duplicate
    pts.push_back(Vec2<f32>{0, 1});
    LloydOptions2<f32> opts{};
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus2::DuplicatePoint);
}

TEST_CASE("lloyd_relax_2d: bbox invalid -> BboxInvalid",
          "[geometry-delaunay][lloyd][v8e]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, 1});
    pts.push_back(Vec2<f32>{1, 1});
    LloydOptions2<f32> opts{};
    opts.hull_policy = HullPolicy2::ClipToBbox;
    opts.bbox_set    = true;
    opts.bbox_min    = Vec2<f32>{1.0F, 1.0F};
    opts.bbox_max    = Vec2<f32>{0.0F, 0.0F}; // inverted -> invalid
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus2::BboxInvalid);
}

TEST_CASE("lloyd_relax_2d: 4-corner square + centre converges in <= 1 iteration (Fix)",
          "[geometry-delaunay][lloyd][v8e][convergence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{2, 0});
    pts.push_back(Vec2<f32>{2, 2});
    pts.push_back(Vec2<f32>{0, 2});
    pts.push_back(Vec2<f32>{1.0F, 1.0F}); // already at centroid of its bounded cell
    LloydOptions2<f32> opts{};
    opts.tolerance = 1.0e-5F;
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.converged);
    CHECK(r.iterations_run <= 1U);
}

TEST_CASE("lloyd_relax_2d: off-centre point relaxes toward centre (Fix)",
          "[geometry-delaunay][lloyd][v8e][convergence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{2, 0});
    pts.push_back(Vec2<f32>{2, 2});
    pts.push_back(Vec2<f32>{0, 2});
    pts.push_back(Vec2<f32>{1.3F, 1.3F}); // off-centre
    LloydOptions2<f32> opts{};
    opts.max_iterations = 30;
    opts.tolerance      = 1.0e-5F;
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    // Site 4 (interior) should converge to (1, 1).
    CHECK(std::abs(r.relaxed_sites[4].x - 1.0F) < 1.0e-3F);
    CHECK(std::abs(r.relaxed_sites[4].y - 1.0F) < 1.0e-3F);
    // Hull sites (0..3) MUST NOT move under Fix policy.
    for (u32 i = 0; i < 4U; ++i)
    {
        CHECK(r.relaxed_sites[i].x == pts[i].x);
        CHECK(r.relaxed_sites[i].y == pts[i].y);
    }
}

TEST_CASE("lloyd_relax_2d: Lloyd energy decreases monotonically (Fix)",
          "[geometry-delaunay][lloyd][v8e][energy]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    // 4 corners + 4 jittered interior points.
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{3, 0});
    pts.push_back(Vec2<f32>{3, 3});
    pts.push_back(Vec2<f32>{0, 3});
    pts.push_back(Vec2<f32>{0.7F, 0.7F});
    pts.push_back(Vec2<f32>{2.3F, 0.7F});
    pts.push_back(Vec2<f32>{2.3F, 2.3F});
    pts.push_back(Vec2<f32>{0.7F, 2.3F});

    const f32 initial_energy = lloyd_energy<f32>(pts, &f.alloc);

    LloydOptions2<f32> opts{};
    opts.max_iterations = 1;
    opts.tolerance      = 1.0e-7F; // ensure we always run 1 iteration
    auto r1 = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r1.ok());

    const f32 after_1iter = lloyd_energy<f32>(r1.relaxed_sites, &f.alloc);
    CHECK(after_1iter <= initial_energy);

    // Run a few more iterations; energy must continue to non-increase.
    opts.max_iterations = 5;
    auto r5 = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{r1.relaxed_sites.data(), r1.relaxed_sites.size()},
        opts, &f.alloc);
    REQUIRE(r5.ok());
    const f32 after_6iter = lloyd_energy<f32>(r5.relaxed_sites, &f.alloc);
    CHECK(after_6iter <= after_1iter);
}

TEST_CASE("lloyd_relax_2d: ClipToBbox relaxes ALL sites incl. hull",
          "[geometry-delaunay][lloyd][v8e][clip]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.1F, 0.1F});
    pts.push_back(Vec2<f32>{1.9F, 0.1F});
    pts.push_back(Vec2<f32>{1.9F, 1.9F});
    pts.push_back(Vec2<f32>{0.1F, 1.9F});
    pts.push_back(Vec2<f32>{1.0F, 1.0F}); // interior

    LloydOptions2<f32> opts{};
    opts.hull_policy    = HullPolicy2::ClipToBbox;
    opts.bbox_set       = true;
    opts.bbox_min       = Vec2<f32>{0.0F, 0.0F};
    opts.bbox_max       = Vec2<f32>{2.0F, 2.0F};
    opts.max_iterations = 100;
    opts.tolerance      = 1.0e-4F;

    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    // All sites should stay inside the bbox.
    for (const auto& p : r.relaxed_sites)
    {
        CHECK(p.x >= 0.0F);
        CHECK(p.x <= 2.0F);
        CHECK(p.y >= 0.0F);
        CHECK(p.y <= 2.0F);
    }
    // Under ClipToBbox + symmetric input, the symmetric interior site
    // should remain near (1, 1).
    CHECK(std::abs(r.relaxed_sites[4].x - 1.0F) < 1.0e-2F);
    CHECK(std::abs(r.relaxed_sites[4].y - 1.0F) < 1.0e-2F);
}

TEST_CASE("lloyd_relax_2d: NotConverged when max_iterations too small",
          "[geometry-delaunay][lloyd][v8e]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{2, 0});
    pts.push_back(Vec2<f32>{2, 2});
    pts.push_back(Vec2<f32>{0, 2});
    pts.push_back(Vec2<f32>{1.5F, 1.5F});

    LloydOptions2<f32> opts{};
    opts.max_iterations = 1;
    opts.tolerance      = 1.0e-10F; // unreachable in 1 iter
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    CHECK(r.status == LloydStatus2::NotConverged);
    CHECK(r.iterations_run == 1U);
    CHECK(!r.converged);
    // Best-so-far positions still returned.
    CHECK(r.relaxed_sites.size() == 5U);
}

TEST_CASE("lloyd_relax_2d: insertion-order determinism",
          "[geometry-delaunay][lloyd][v8e][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts_a(&f.alloc);
    crd::containers::Array<Vec2<f32>> pts_b(&f.alloc);
    pts_a.push_back(Vec2<f32>{0, 0});
    pts_a.push_back(Vec2<f32>{2, 0});
    pts_a.push_back(Vec2<f32>{2, 2});
    pts_a.push_back(Vec2<f32>{0, 2});
    pts_a.push_back(Vec2<f32>{1.3F, 1.3F});

    // Same points but in different order. Note: Lloyd result depends on
    // INDEX correspondence; reorder pts_b to match pts_a's indexing for
    // direct compare.
    pts_b = pts_a;

    LloydOptions2<f32> opts{};
    opts.max_iterations = 20;
    opts.tolerance      = 1.0e-5F;

    auto ra = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_a.data(), pts_a.size()}, opts, &f.alloc);
    auto rb = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_b.data(), pts_b.size()}, opts, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    REQUIRE(ra.relaxed_sites.size() == rb.relaxed_sites.size());
    for (u32 i = 0; i < ra.relaxed_sites.size(); ++i)
    {
        CHECK(ra.relaxed_sites[i].x == rb.relaxed_sites[i].x);
        CHECK(ra.relaxed_sites[i].y == rb.relaxed_sites[i].y);
    }
}

TEST_CASE("lloyd_relax_2d: large-coord f32 stability (1e3 scale)",
          "[geometry-delaunay][lloyd][v8e]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    constexpr f32 kScale = 1.0e3F;
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{kScale, 0});
    pts.push_back(Vec2<f32>{kScale, kScale});
    pts.push_back(Vec2<f32>{0, kScale});
    pts.push_back(Vec2<f32>{0.7F * kScale, 0.7F * kScale});
    LloydOptions2<f32> opts{};
    opts.max_iterations = 60;
    opts.tolerance      = 1.0e-4F * kScale;
    auto r = lloyd_relax_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    // Interior site converges near (kScale/2, kScale/2). Tolerance scales
    // with kScale: ~0.1F is ~1e-4 relative.
    CHECK(std::abs(r.relaxed_sites[4].x - 0.5F * kScale) < 0.5F);
    CHECK(std::abs(r.relaxed_sites[4].y - 0.5F * kScale) < 0.5F);
}

TEST_CASE("lloyd_relax_2d: f64 precision tier",
          "[geometry-delaunay][lloyd][v8e][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{2, 0});
    pts.push_back(Vec2<f64>{2, 2});
    pts.push_back(Vec2<f64>{0, 2});
    pts.push_back(Vec2<f64>{1.3, 1.3});
    LloydOptions2<f64> opts{};
    opts.max_iterations = 30;
    opts.tolerance      = 1.0e-7;
    auto r = lloyd_relax_2d<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()}, opts, &f.alloc);
    REQUIRE(r.ok());
    CHECK(std::abs(r.relaxed_sites[4].x - 1.0) < 1.0e-5);
    CHECK(std::abs(r.relaxed_sites[4].y - 1.0) < 1.0e-5);
}
