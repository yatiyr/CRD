// crd-geometry-convex v3b — 2D convex hull (Andrew's monotone chain).
//
// Coverage:
//   (1) Closed-form hulls of known shapes (square, triangle, regular pentagon).
//   (2) Degenerate inputs: 0/1/2/3 points, all-collinear, all-coincident.
//   (3) Output is CCW.
//   (4) Output indices form == output points form (cross-check).
//   (5) Hull contains every input point (membership invariant — input is
//       inside-or-on the convex hull polygon).
//   (6) Determinism: replay produces bit-identical hull indices.
//   (7) Adversarial: collinear-with-outlier (the classic Quickhull torture
//       — three nearly-collinear points + one slightly off-axis).
//   (8) Large-coordinate stability (input at scale 1e6).
//   (9) f32 + f64 both produce correct hulls.

#include <crd/geometry/convex/convex_hull_2d.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <numbers>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::geometry::convex::convex_hull_2d_indices;
using crd::geometry::convex::convex_hull_2d_points;
using crd::geometry::primitives::orient2d;
using crd::math::Vec2;

// Check that the indices form a CCW polygon (every consecutive triple is a
// left turn or collinear).
template <typename T>
bool is_ccw_polygon(crd::containers::ConstSpan<Vec2<T>> points,
                    crd::containers::ConstSpan<u32> hull_indices)
{
    const usize n = hull_indices.size();
    if (n < 3)
    {
        return true; // degenerate (single point / segment) — trivially "CCW"
    }
    for (usize i = 0; i < n; ++i)
    {
        const Vec2<T>& a = points[hull_indices[i]];
        const Vec2<T>& b = points[hull_indices[(i + 1) % n]];
        const Vec2<T>& c = points[hull_indices[(i + 2) % n]];
        if (orient2d(a, b, c) < 0)
        {
            return false;
        }
    }
    return true;
}

// Check that every input point is inside-or-on the hull polygon (the
// containment invariant — equivalent to: every input point is on the LEFT
// side or ON every hull edge).
template <typename T>
bool hull_contains_all_inputs(crd::containers::ConstSpan<Vec2<T>> points,
                              crd::containers::ConstSpan<u32> hull_indices)
{
    const usize n_hull = hull_indices.size();
    if (n_hull < 3)
    {
        return true; // degenerate
    }
    for (usize p_idx = 0; p_idx < points.size(); ++p_idx)
    {
        const Vec2<T>& p = points[p_idx];
        for (usize i = 0; i < n_hull; ++i)
        {
            const Vec2<T>& a = points[hull_indices[i]];
            const Vec2<T>& b = points[hull_indices[(i + 1) % n_hull]];
            if (orient2d(a, b, p) < 0)
            {
                return false; // p is to the right of edge a->b -> outside hull
            }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) Closed-form hulls
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: square (4 corners)", "[v3b][hull2d]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    points.push_back(Vec2<f64>(0, 0));
    points.push_back(Vec2<f64>(1, 0));
    points.push_back(Vec2<f64>(1, 1));
    points.push_back(Vec2<f64>(0, 1));

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);

    REQUIRE(hull.size() == 4);
    CHECK(is_ccw_polygon<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
    CHECK(hull_contains_all_inputs<f64>(
        crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
        crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}

TEST_CASE("convex_hull_2d: square with interior point (1 point excluded)", "[v3b][hull2d]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    points.push_back(Vec2<f64>(0, 0));
    points.push_back(Vec2<f64>(1, 0));
    points.push_back(Vec2<f64>(1, 1));
    points.push_back(Vec2<f64>(0, 1));
    points.push_back(Vec2<f64>(0.5, 0.5)); // interior — must NOT appear in hull

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);

    REQUIRE(hull.size() == 4);
    // Interior point (index 4) must not appear.
    for (usize i = 0; i < hull.size(); ++i)
    {
        CHECK(hull[i] != 4);
    }
    CHECK(is_ccw_polygon<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
    CHECK(hull_contains_all_inputs<f64>(
        crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
        crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}

TEST_CASE("convex_hull_2d: triangle", "[v3b][hull2d]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    points.push_back(Vec2<f64>(0, 0));
    points.push_back(Vec2<f64>(2, 0));
    points.push_back(Vec2<f64>(1, 2));

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);

    REQUIRE(hull.size() == 3);
    CHECK(is_ccw_polygon<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}

TEST_CASE("convex_hull_2d: hexagon with interior cluster", "[v3b][hull2d]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    // 6 hexagon vertices (a regular hexagon on the unit circle).
    for (int i = 0; i < 6; ++i)
    {
        const f64 theta = static_cast<f64>(i) * std::numbers::pi_v<f64> / 3.0;
        points.push_back(Vec2<f64>(std::cos(theta), std::sin(theta)));
    }
    // 10 interior points (random-ish but deterministic).
    for (int i = 0; i < 10; ++i)
    {
        const f64 t = static_cast<f64>(i) / 10.0;
        points.push_back(Vec2<f64>(0.1 + 0.3 * t, 0.2 + 0.4 * t));
    }

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);

    REQUIRE(hull.size() == 6);
    CHECK(is_ccw_polygon<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
    CHECK(hull_contains_all_inputs<f64>(
        crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
        crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}

// ---------------------------------------------------------------------------
// (2) Degenerate inputs
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: empty input -> empty hull", "[v3b][hull2d][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>{}, hull);
    CHECK(hull.empty());
}

TEST_CASE("convex_hull_2d: single point -> 1-vertex hull", "[v3b][hull2d][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    points.push_back(Vec2<f64>(3.14, 2.71));
    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
    REQUIRE(hull.size() == 1);
    CHECK(hull[0] == 0);
}

TEST_CASE("convex_hull_2d: two distinct points -> 2-vertex hull", "[v3b][hull2d][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    points.push_back(Vec2<f64>(0, 0));
    points.push_back(Vec2<f64>(1, 1));
    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
    REQUIRE(hull.size() == 2);
    // Output should be lex-min first, lex-max second.
    CHECK(points[hull[0]].x <= points[hull[1]].x);
}

TEST_CASE("convex_hull_2d: collinear input -> 2-vertex hull (lex extremes)",
          "[v3b][hull2d][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    // 5 points on the line y = 2x + 1.
    for (int i = 0; i < 5; ++i)
    {
        points.push_back(Vec2<f64>(static_cast<f64>(i), 2.0 * i + 1.0));
    }
    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
    // Collinear input: the lex-min and lex-max are the only "extremes".
    REQUIRE(hull.size() == 2);
}

TEST_CASE("convex_hull_2d: all-coincident input -> 1-vertex hull", "[v3b][hull2d][degen]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    for (int i = 0; i < 7; ++i)
    {
        points.push_back(Vec2<f64>(2.5, -1.0));
    }
    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
    REQUIRE(hull.size() == 1);
}

// ---------------------------------------------------------------------------
// (4) Indices form vs points form cross-check
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: indices form and points form agree", "[v3b][hull2d]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    points.push_back(Vec2<f64>(0, 0));
    points.push_back(Vec2<f64>(3, 0));
    points.push_back(Vec2<f64>(3, 4));
    points.push_back(Vec2<f64>(0, 4));
    points.push_back(Vec2<f64>(1.5, 2));

    crd::containers::Array<u32> hull_indices(&alloc);
    crd::containers::Array<Vec2<f64>> hull_points(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull_indices);
    convex_hull_2d_points<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull_points);

    REQUIRE(hull_indices.size() == hull_points.size());
    for (usize i = 0; i < hull_indices.size(); ++i)
    {
        CHECK(hull_points[i].x == points[hull_indices[i]].x);
        CHECK(hull_points[i].y == points[hull_indices[i]].y);
    }
}

// ---------------------------------------------------------------------------
// (6) Determinism
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: replay produces bit-identical hull", "[v3b][hull2d][determinism]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    // Deterministic 20-point cloud.
    for (int i = 0; i < 20; ++i)
    {
        const f64 x = std::cos(0.31 * i + 0.7);
        const f64 y = std::sin(0.31 * i + 0.7);
        points.push_back(Vec2<f64>(x, y));
    }

    crd::containers::Array<u32> hull1(&alloc);
    crd::containers::Array<u32> hull2(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull1);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull2);

    REQUIRE(hull1.size() == hull2.size());
    for (usize i = 0; i < hull1.size(); ++i)
    {
        CHECK(hull1[i] == hull2[i]);
    }
}

// ---------------------------------------------------------------------------
// (7) Adversarial collinear-with-outlier
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: near-collinear with adaptive predicate", "[v3b][hull2d][adversarial]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    // 4 points nearly collinear on y=0, plus 1 outlier slightly above:
    points.push_back(Vec2<f64>(0, 0));
    points.push_back(Vec2<f64>(1, 0));
    points.push_back(Vec2<f64>(2, 0));
    points.push_back(Vec2<f64>(3, 0));
    points.push_back(Vec2<f64>(1.5, 1e-14)); // very-slightly above — adaptive orient2d must catch this

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);

    // Outlier MUST be on the hull (Shewchuk adaptive predicate detects the
    // tiny tilt; naïve float predicate would have missed it).
    bool found_outlier = false;
    for (usize i = 0; i < hull.size(); ++i)
    {
        if (hull[i] == 4)
        {
            found_outlier = true;
            break;
        }
    }
    CHECK(found_outlier);
    CHECK(is_ccw_polygon<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}

// ---------------------------------------------------------------------------
// (8) Large-coordinate stability
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: square at scale 1e6 produces correct hull",
          "[v3b][hull2d][large-coord]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    constexpr f64 origin = 1.0e6;
    points.push_back(Vec2<f64>(origin, origin));
    points.push_back(Vec2<f64>(origin + 1, origin));
    points.push_back(Vec2<f64>(origin + 1, origin + 1));
    points.push_back(Vec2<f64>(origin, origin + 1));
    points.push_back(Vec2<f64>(origin + 0.5, origin + 0.5)); // interior

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
    REQUIRE(hull.size() == 4);
    CHECK(is_ccw_polygon<f64>(crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}

// ---------------------------------------------------------------------------
// (9) f32 + f64 both work
// ---------------------------------------------------------------------------

TEST_CASE("convex_hull_2d: f32 square produces 4-vertex hull", "[v3b][hull2d][f32]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec2<f32>> points(&alloc);
    points.push_back(Vec2<f32>(0, 0));
    points.push_back(Vec2<f32>(1, 0));
    points.push_back(Vec2<f32>(1, 1));
    points.push_back(Vec2<f32>(0, 1));
    points.push_back(Vec2<f32>(0.5F, 0.5F));

    crd::containers::Array<u32> hull(&alloc);
    convex_hull_2d_indices<f32>(crd::containers::ConstSpan<Vec2<f32>>(points.data(), points.size()), hull);
    REQUIRE(hull.size() == 4);
    CHECK(is_ccw_polygon<f32>(crd::containers::ConstSpan<Vec2<f32>>(points.data(), points.size()),
                              crd::containers::ConstSpan<u32>(hull.data(), hull.size())));
}
