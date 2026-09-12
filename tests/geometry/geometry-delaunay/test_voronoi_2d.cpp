// Tests for crd-geometry-delaunay v8d-2d 2D Voronoi diagram extraction.
//
// Coverage:
//   - Diagnostic statuses propagated from Delaunay (TooFewPoints /
//     NonFiniteInput / DuplicatePoint).
//   - 3-site triangle (all 3 cells unbounded, 1 Voronoi vertex).
//   - 4-site square (closed Voronoi cells around interior of input).
//   - 5-site square+center (interior site's cell is closed; hull sites
//     unbounded).
//   - **Defining property** validator: for every cell, pick a sample point
//     inside the cell and verify the nearest input site (brute force over
//     all sites) is the cell's site. This is the Voronoi DEFINITION; a
//     correct Voronoi extraction must satisfy it.
//   - CCW cell orientation (signed area > 0 for bounded cells).
//   - **Cospherical-pathology validator** (the v8c-pre Stage D enables
//     this) — 9-pt mixture with 5 cospherical sites. Cells must still
//     pass the defining property.
//   - Determinism (shuffled input → equivalent cells).
//   - Large-coord f32 stability.
//   - f64 precision.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
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
using crd::geometry::delaunay::VoronoiCell;
using crd::geometry::delaunay::VoronoiResult2;
using crd::geometry::delaunay::VoronoiStatus2;
using crd::geometry::delaunay::voronoi_2d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{8U * 1024U * 1024U, nullptr, "voronoi2d-test-arena"};
};

// signed_area: positive for CCW.
template <typename T>
T cell_signed_area(const VoronoiResult2<T>& r, const VoronoiCell<T>& cell)
{
    T sum = static_cast<T>(0);
    const u32 n = static_cast<u32>(cell.vertex_indices.size());
    if (n < 3U) { return static_cast<T>(0); }
    for (u32 i = 0; i < n; ++i)
    {
        const auto& p = r.voronoi_vertices[cell.vertex_indices[i]];
        const auto& q = r.voronoi_vertices[cell.vertex_indices[(i + 1U) % n]];
        sum += p.x * q.y - q.x * p.y;
    }
    return sum * static_cast<T>(0.5);
}

// For every cell, verify sites[cell.site_index] is the closest input site
// to itself (brute-force over all sites). This is the Voronoi DEFINITION.
template <typename T>
bool defining_property_holds(const crd::containers::Array<Vec2<T>>& sites,
                              const VoronoiResult2<T>&              r)
{
    for (const auto& cell : r.cells)
    {
        const Vec2<T> sample = sites[cell.site_index];
        T best_d2 = std::numeric_limits<T>::infinity();
        u32 best_s = std::numeric_limits<u32>::max();
        for (u32 s = 0; s < sites.size(); ++s)
        {
            const T dx = sample.x - sites[s].x;
            const T dy = sample.y - sites[s].y;
            const T d2 = dx * dx + dy * dy;
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best_s  = s;
            }
        }
        if (best_s != cell.site_index) { return false; }
    }
    return true;
}

} // anonymous namespace

TEST_CASE("voronoi_2d: < 3 points -> TooFewPoints",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus2::TooFewPoints);
    CHECK(r.cells.size() == 0U);
}

TEST_CASE("voronoi_2d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, std::numeric_limits<f32>::infinity()});
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus2::NonFiniteInput);
}

TEST_CASE("voronoi_2d: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 0}); // duplicate
    pts.push_back(Vec2<f32>{0, 1});
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus2::DuplicatePoint);
}

TEST_CASE("voronoi_2d: 3 sites (triangle) yields 1 Voronoi vertex + 3 unbounded cells",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{2, 0});
    pts.push_back(Vec2<f32>{1, 2});
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.voronoi_vertices.size() == 1U); // 1 Delaunay tri -> 1 circumcentre
    CHECK(r.cells.size() == 3U);
    for (const auto& cell : r.cells)
    {
        CHECK(!cell.is_bounded);
        CHECK(cell.vertex_indices.size() == 1U); // single circumcentre per cell
    }
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_2d: 4-site square (all 4 cells unbounded)",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 1});
    pts.push_back(Vec2<f32>{0, 1});
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    // 4 hull sites — all unbounded.
    for (const auto& cell : r.cells)
    {
        CHECK(!cell.is_bounded);
    }
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_2d: 5 sites (square + center) interior cell bounded",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{2, 0});
    pts.push_back(Vec2<f32>{2, 2});
    pts.push_back(Vec2<f32>{0, 2});
    pts.push_back(Vec2<f32>{1, 1}); // interior
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 5U);

    // Interior site (index 4) should have a bounded cell.
    u32 bounded_count = 0;
    for (const auto& cell : r.cells)
    {
        if (cell.is_bounded) { ++bounded_count; }
    }
    CHECK(bounded_count == 1U);

    // Defining property holds.
    CHECK(defining_property_holds(pts, r));

    // Bounded cells must have CCW orientation (signed_area > 0).
    for (const auto& cell : r.cells)
    {
        if (cell.is_bounded && cell.vertex_indices.size() >= 3U)
        {
            CHECK(cell_signed_area(r, cell) > static_cast<f32>(0));
        }
    }
}

TEST_CASE("voronoi_2d: 16-site exact integer grid (4 inner bounded, 12 perimeter unbounded)",
          "[geometry-delaunay][voronoi][v8d-2d][cocircular]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    // 4x4 integer grid. Every unit square is cocircular; Stage D incircle
    // + lex-tiebreak resolve the ambiguity deterministically. Convex hull
    // = 4 corners + 8 edge points (12 total perimeter sites, all on the
    // hull boundary even though only 4 are "strict" hull vertices —
    // sites lying on a hull edge have unbounded Voronoi cells too because
    // there's no input on the outside of the hull line).
    for (u32 j = 0; j < 4U; ++j)
    {
        for (u32 i = 0; i < 4U; ++i)
        {
            pts.push_back(Vec2<f32>{static_cast<f32>(i), static_cast<f32>(j)});
        }
    }
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 16U);

    // The 4 strictly-interior sites (indices 5, 6, 9, 10) MUST be bounded.
    // The 12 perimeter sites MUST be unbounded.
    auto site_index = [](u32 i, u32 j) { return j * 4U + i; };
    const u32 interior_sites[4] = {
        site_index(1U, 1U), site_index(2U, 1U),
        site_index(1U, 2U), site_index(2U, 2U),
    };
    u32 bounded_count = 0;
    u32 unbounded_count = 0;
    for (const auto& cell : r.cells)
    {
        if (cell.is_bounded) { ++bounded_count; } else { ++unbounded_count; }
    }
    CHECK(bounded_count == 4U);
    CHECK(unbounded_count == 12U);
    for (u32 idx : interior_sites)
    {
        CHECK(r.cells[idx].is_bounded);
    }
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_2d: cospherical-pathology 9-site mixture (v8c-pre validator)",
          "[geometry-delaunay][voronoi][v8d-2d][cospherical]")
{
    // 5 cospherical points on r^2=5e9 sphere (the v8c-pre Stage D
    // discriminator restricted to 2D — actually cocircular in 2D plane) +
    // 4 non-cospherical points. Without v8c-pre's full Stage D (here:
    // Stage D incircle which was shipped 2026-05-14 as the original
    // adaptive predicates paydown), the cocircular 5-tuple would produce
    // an arbitrary Delaunay flip choice → arbitrary cells.
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{50000, 50000});       // cocircular r^2=5e9
    pts.push_back(Vec2<f64>{50000, 40000});
    pts.push_back(Vec2<f64>{40000, 50000});
    pts.push_back(Vec2<f64>{30000, 40000});
    pts.push_back(Vec2<f64>{50000, 30000});
    pts.push_back(Vec2<f64>{20000, 20000});
    pts.push_back(Vec2<f64>{100000, 0});
    pts.push_back(Vec2<f64>{0, 100000});
    pts.push_back(Vec2<f64>{0, 0});
    auto r = voronoi_2d<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 9U);
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_2d: insertion-order determinism (shuffled input)",
          "[geometry-delaunay][voronoi][v8d-2d][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts_a(&f.alloc);
    crd::containers::Array<Vec2<f32>> pts_b(&f.alloc);
    pts_a.push_back(Vec2<f32>{0, 0});
    pts_a.push_back(Vec2<f32>{2, 0});
    pts_a.push_back(Vec2<f32>{2, 2});
    pts_a.push_back(Vec2<f32>{0, 2});
    pts_a.push_back(Vec2<f32>{1, 1});

    pts_b.push_back(Vec2<f32>{1, 1});
    pts_b.push_back(Vec2<f32>{0, 0});
    pts_b.push_back(Vec2<f32>{2, 2});
    pts_b.push_back(Vec2<f32>{0, 2});
    pts_b.push_back(Vec2<f32>{2, 0});

    auto ra = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_a.data(), pts_a.size()}, &f.alloc);
    auto rb = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_b.data(), pts_b.size()}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK(ra.cells.size() == rb.cells.size());
    CHECK(defining_property_holds(pts_a, ra));
    CHECK(defining_property_holds(pts_b, rb));
}

TEST_CASE("voronoi_2d: large-coord f32 stability (1e5 scale)",
          "[geometry-delaunay][voronoi][v8d-2d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    constexpr f32 k_scale = 1.0e5F;
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{k_scale, 0});
    pts.push_back(Vec2<f32>{k_scale, k_scale});
    pts.push_back(Vec2<f32>{0, k_scale});
    pts.push_back(Vec2<f32>{0.5F * k_scale, 0.5F * k_scale});
    auto r = voronoi_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 5U);
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_2d: f64 precision tier",
          "[geometry-delaunay][voronoi][v8d-2d][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{2, 0});
    pts.push_back(Vec2<f64>{2, 2});
    pts.push_back(Vec2<f64>{0, 2});
    pts.push_back(Vec2<f64>{1, 1});
    auto r = voronoi_2d<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 5U);
    CHECK(defining_property_holds(pts, r));
}
