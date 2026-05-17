// Tests for crd-geometry-delaunay v8a 2D Bowyer-Watson Delaunay.
//
// Covers:
//   - Diagnostic statuses (TooFewPoints, NonFiniteInput, DuplicatePoint)
//   - 3-point triangle (single triangle)
//   - 4-point square (2 triangles)
//   - N-point random cloud: every output triangle is CCW (orient2d > 0)
//     + empty-circumcircle invariant (incircle <= 0 for all other points)
//   - Triangle count formula for convex-position points (2n - 2 - h, where
//     h = hull-vertex count) — for square hull = 4, triangles = 4*2 - 2 - 4 = 2 (yes)
//   - Determinism: shuffled inputs produce the same triangulation (set
//     equivalence after canonicalisation)
//   - Cocircular degenerate (4 points on a circle) - any valid Delaunay
//     triangulation is acceptable (incircle adaptive resolves cocircular
//     via lex tiebreak deterministically)
//   - Large-coord (1e6) f32 stability
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec2;
using crd::geometry::delaunay::DelaunayStatus;
using crd::geometry::delaunay::delaunay_2d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{4U * 1024U * 1024U, nullptr, "delaunay-test-arena"};
};

// Verify (1) every triangle is CCW and (2) empty-circumcircle invariant.
template <typename T>
bool verify_delaunay(const crd::containers::Array<Vec2<T>>& pts,
                      const crd::containers::Array<u32>&     tris)
{
    const u32 tri_count = static_cast<u32>(tris.size() / 3U);
    for (u32 t = 0; t < tri_count; ++t)
    {
        const u32 a = tris[3U * t + 0U];
        const u32 b = tris[3U * t + 1U];
        const u32 c = tris[3U * t + 2U];
        // CCW check.
        const T o = crd::geometry::primitives::orient2d(pts[a], pts[b], pts[c]);
        if (o <= static_cast<T>(0)) { return false; }
        // Empty-circumcircle: every other input point has incircle <= 0.
        for (u32 p = 0; p < pts.size(); ++p)
        {
            if (p == a || p == b || p == c) { continue; }
            const T s = crd::geometry::primitives::incircle(pts[a], pts[b], pts[c], pts[p]);
            // Tolerate cocircular (s == 0); reject strict > 0.
            if (s > static_cast<T>(0)) { return false; }
        }
    }
    return true;
}

} // anonymous namespace

TEST_CASE("delaunay_2d: < 3 points -> TooFewPoints",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus::TooFewPoints);
    CHECK(r.triangle_count == 0U);
}

TEST_CASE("delaunay_2d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{std::numeric_limits<f32>::infinity(), 0});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus::NonFiniteInput);
}

TEST_CASE("delaunay_2d: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 0}); // duplicate
    pts.push_back(Vec2<f32>{0, 1});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus::DuplicatePoint);
}

TEST_CASE("delaunay_2d: single triangle (3 pts)",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, 1});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 1U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d: square (4 pts, 2 triangles)",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 1});
    pts.push_back(Vec2<f32>{0, 1});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 2U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d: regular pentagon (5 pts, 3 triangles)",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    // 5 pts at 72° intervals.
    pts.push_back(Vec2<f32>{ 1.0F,        0.0F});
    pts.push_back(Vec2<f32>{ 0.309017F,   0.951057F});
    pts.push_back(Vec2<f32>{-0.809017F,   0.587785F});
    pts.push_back(Vec2<f32>{-0.809017F,  -0.587785F});
    pts.push_back(Vec2<f32>{ 0.309017F,  -0.951057F});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 3U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d: 32-pt random cloud satisfies Delaunay invariants",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    // Deterministic "random" via a fixed seed: PCG mixer
    u32 state = 0xC0FFEEU;
    auto next_rand = [&]() -> f32 {
        state = state * 1103515245U + 12345U;
        return static_cast<f32>((state >> 8) & 0xFFFFFU) / static_cast<f32>(0x100000U);
    };
    for (u32 i = 0; i < 32U; ++i)
    {
        pts.push_back(Vec2<f32>{next_rand() * 10.0F, next_rand() * 10.0F});
    }
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count > 0U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d: insertion-order determinism (shuffled input)",
          "[geometry-delaunay][v8a][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts_a(&f.alloc);
    crd::containers::Array<Vec2<f32>> pts_b(&f.alloc);
    // Same set, different order.
    pts_a.push_back(Vec2<f32>{0, 0});
    pts_a.push_back(Vec2<f32>{1, 0});
    pts_a.push_back(Vec2<f32>{1, 1});
    pts_a.push_back(Vec2<f32>{0, 1});
    pts_a.push_back(Vec2<f32>{0.5F, 0.5F});

    pts_b.push_back(Vec2<f32>{0.5F, 0.5F});
    pts_b.push_back(Vec2<f32>{0, 0});
    pts_b.push_back(Vec2<f32>{1, 1});
    pts_b.push_back(Vec2<f32>{0, 1});
    pts_b.push_back(Vec2<f32>{1, 0});

    auto ra = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_a.data(), pts_a.size()}, &f.alloc);
    auto rb = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_b.data(), pts_b.size()}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK(ra.triangle_count == rb.triangle_count);
    // Triangle sets equivalent under vertex-position canonicalisation:
    // for each output triangle of A, find a triangle of B whose 3 vertex
    // POSITIONS (looked up in their own point arrays) match a, b, c
    // (modulo CCW rotation).
    auto sorted_triangle_positions = [](const crd::containers::Array<Vec2<f32>>& pts,
                                          const crd::containers::Array<u32>&     idx,
                                          u32                                     t) -> std::array<Vec2<f32>, 3> {
        std::array<Vec2<f32>, 3> tp = {pts[idx[3U * t + 0]], pts[idx[3U * t + 1]], pts[idx[3U * t + 2]]};
        std::sort(tp.begin(), tp.end(), [](const Vec2<f32>& l, const Vec2<f32>& r) {
            return l.x < r.x || (l.x == r.x && l.y < r.y);
        });
        return tp;
    };
    for (u32 t = 0; t < ra.triangle_count; ++t)
    {
        const auto ta = sorted_triangle_positions(pts_a, ra.triangle_indices, t);
        bool       found = false;
        for (u32 u = 0; u < rb.triangle_count && !found; ++u)
        {
            const auto tb = sorted_triangle_positions(pts_b, rb.triangle_indices, u);
            if (ta[0].x == tb[0].x && ta[0].y == tb[0].y
                && ta[1].x == tb[1].x && ta[1].y == tb[1].y
                && ta[2].x == tb[2].x && ta[2].y == tb[2].y)
            {
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("delaunay_2d: large-coord f32 stability (1e6 scale)",
          "[geometry-delaunay][v8a]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    constexpr f32 kS = 1.0e6F;
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{kS, 0});
    pts.push_back(Vec2<f32>{kS, kS});
    pts.push_back(Vec2<f32>{0, kS});
    pts.push_back(Vec2<f32>{0.5F * kS, 0.5F * kS});
    auto r = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 4U); // 4 triangles fan around centre
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d: f64 precision tier",
          "[geometry-delaunay][v8a][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{1, 0});
    pts.push_back(Vec2<f64>{1, 1});
    pts.push_back(Vec2<f64>{0, 1});
    pts.push_back(Vec2<f64>{0.5, 0.5});
    auto r = delaunay_2d<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 4U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}
