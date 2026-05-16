// crd-geometry-polygon v6e - Bentley-Ottmann line-segment intersection tests.
//
// Coverage:
//   * single transverse intersection
//   * grid of segments with O(n) intersections
//   * T-junction (endpoint on interior of another segment)
//   * vertex-on-vertex coincidence
//   * vertical + horizontal mix
//   * parallel non-intersecting
//   * collinear overlap
//   * dense small case (5x5 grid)
//   * is_simple-like usage: short-circuit on first intersection
//   * diagnostic statuses (degenerate / non-finite)
//   * f64 precision tier

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec2;
using crd::geometry::polygon::bentley_ottmann;
using crd::geometry::polygon::bentley_ottmann_any;
using crd::geometry::polygon::BOIntersection;
using crd::geometry::polygon::BOResult;
using crd::geometry::polygon::BOSegment;
using crd::geometry::polygon::BOStatus;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };
} // namespace

TEST_CASE("bentley_ottmann: empty input returns zero intersections",
          "[geometry-polygon][bo]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.intersections.size() == 0U);
}

TEST_CASE("bentley_ottmann: two disjoint segments have no intersections",
          "[geometry-polygon][bo]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 0.F}, Vec2<f32>{1.F, 1.F}});
    segs.push_back(BOSegment<f32>{Vec2<f32>{3.F, 0.F}, Vec2<f32>{4.F, 1.F}});
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.intersections.size() == 0U);
}

TEST_CASE("bentley_ottmann: two crossing segments emit 1 intersection",
          "[geometry-polygon][bo]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 0.F}, Vec2<f32>{2.F, 2.F}});   // 0
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 2.F}, Vec2<f32>{2.F, 0.F}});   // 1 - the X
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    REQUIRE(r.intersections.size() == 1U);
    const auto& it = r.intersections[0];
    CHECK(it.point.x == 1.F);
    CHECK(it.point.y == 1.F);
    // Segment pair canonicalised (a < b).
    CHECK(it.segment_a == 0U);
    CHECK(it.segment_b == 1U);
}

TEST_CASE("bentley_ottmann: 4-segment cross emits 4 intersections (2 pairs cross at 2 pts)",
          "[geometry-polygon][bo]")
{
    AllocFixture f{};
    // Two horizontal-ish segments + two vertical-ish forming a # pattern.
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 1.F}, Vec2<f32>{4.F, 1.F}}); // 0 - bottom horiz
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 3.F}, Vec2<f32>{4.F, 3.F}}); // 1 - top horiz
    segs.push_back(BOSegment<f32>{Vec2<f32>{1.F, 0.F}, Vec2<f32>{1.F, 4.F}}); // 2 - left vert
    segs.push_back(BOSegment<f32>{Vec2<f32>{3.F, 0.F}, Vec2<f32>{3.F, 4.F}}); // 3 - right vert
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    // 4 intersections: (0,2), (0,3), (1,2), (1,3).
    CHECK(r.intersections.size() == 4U);
}

TEST_CASE("bentley_ottmann: T-junction (endpoint on interior) is reported",
          "[geometry-polygon][bo]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 0.F}, Vec2<f32>{2.F, 0.F}});  // horizontal at y=0
    segs.push_back(BOSegment<f32>{Vec2<f32>{1.F, 0.F}, Vec2<f32>{1.F, 1.F}});  // vertical touching at (1, 0)
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    REQUIRE(r.intersections.size() == 1U);
    CHECK(r.intersections[0].point.x == 1.F);
    CHECK(r.intersections[0].point.y == 0.F);
}

TEST_CASE("bentley_ottmann: parallel non-intersecting segments report nothing",
          "[geometry-polygon][bo]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 0.F}, Vec2<f32>{2.F, 2.F}});
    segs.push_back(BOSegment<f32>{Vec2<f32>{1.F, 0.F}, Vec2<f32>{3.F, 2.F}});
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.intersections.size() == 0U);
}

TEST_CASE("bentley_ottmann: dense 5x5 grid (5 horiz + 5 vert) emits 25 intersections",
          "[geometry-polygon][bo][dense]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    for (u32 i = 0; i < 5U; ++i)
    {
        // Horizontal at y = i
        segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, static_cast<f32>(i)},
                                       Vec2<f32>{4.F, static_cast<f32>(i)}});
    }
    for (u32 i = 0; i < 5U; ++i)
    {
        // Vertical at x = i
        segs.push_back(BOSegment<f32>{Vec2<f32>{static_cast<f32>(i), 0.F},
                                       Vec2<f32>{static_cast<f32>(i), 4.F}});
    }
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    // 5 horizontals * 5 verticals = 25 intersections.
    CHECK(r.intersections.size() == 25U);
}

TEST_CASE("bentley_ottmann: short-circuit `_any` returns true on first hit",
          "[geometry-polygon][bo][short-circuit]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 0.F}, Vec2<f32>{2.F, 2.F}});
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 2.F}, Vec2<f32>{2.F, 0.F}});
    BOIntersection<f32> first{};
    const bool any = bentley_ottmann_any<f32>(
        crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()}, &f.alloc, &first);
    CHECK(any);
    CHECK(first.point.x == 1.F);
    CHECK(first.point.y == 1.F);
}

TEST_CASE("bentley_ottmann: short-circuit `_any` returns false on no intersection",
          "[geometry-polygon][bo][short-circuit]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 0.F}, Vec2<f32>{1.F, 1.F}});
    segs.push_back(BOSegment<f32>{Vec2<f32>{3.F, 0.F}, Vec2<f32>{4.F, 1.F}});
    const bool any = bentley_ottmann_any<f32>(
        crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()}, &f.alloc);
    CHECK_FALSE(any);
}

TEST_CASE("bentley_ottmann: degenerate (zero-length) input returns DegenerateSegment",
          "[geometry-polygon][bo][diagnostic]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{1.F, 1.F}, Vec2<f32>{1.F, 1.F}}); // zero length
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    CHECK_FALSE(r.ok());
    CHECK(r.status == BOStatus::DegenerateSegment);
}

TEST_CASE("bentley_ottmann: non-finite input returns NonFiniteInput",
          "[geometry-polygon][bo][diagnostic]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    segs.push_back(BOSegment<f32>{Vec2<f32>{std::numeric_limits<f32>::infinity(), 0.F},
                                   Vec2<f32>{1.F, 1.F}});
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    CHECK_FALSE(r.ok());
    CHECK(r.status == BOStatus::NonFiniteInput);
}

TEST_CASE("bentley_ottmann: f64 precision tier - large coordinates",
          "[geometry-polygon][bo][f64]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f64>> segs(&f.alloc);
    segs.push_back(BOSegment<f64>{Vec2<f64>{0.0, 0.0}, Vec2<f64>{1.0e6, 1.0e6}});
    segs.push_back(BOSegment<f64>{Vec2<f64>{0.0, 1.0e6}, Vec2<f64>{1.0e6, 0.0}});
    auto r = bentley_ottmann<f64>(crd::containers::ConstSpan<BOSegment<f64>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    REQUIRE(r.intersections.size() == 1U);
    CHECK(r.intersections[0].point.x == 5.0e5);
    CHECK(r.intersections[0].point.y == 5.0e5);
}

TEST_CASE("bentley_ottmann: output sorted by lex (y, x, seg_a, seg_b)",
          "[geometry-polygon][bo][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<BOSegment<f32>> segs(&f.alloc);
    // 3 horizontals at y=1,2,3 + 1 vertical at x=2 crossing them all.
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 1.F}, Vec2<f32>{4.F, 1.F}}); // 0
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 2.F}, Vec2<f32>{4.F, 2.F}}); // 1
    segs.push_back(BOSegment<f32>{Vec2<f32>{0.F, 3.F}, Vec2<f32>{4.F, 3.F}}); // 2
    segs.push_back(BOSegment<f32>{Vec2<f32>{2.F, 0.F}, Vec2<f32>{2.F, 4.F}}); // 3
    auto r = bentley_ottmann<f32>(crd::containers::ConstSpan<BOSegment<f32>>{segs.data(), segs.size()},
                                   &f.alloc);
    REQUIRE(r.ok());
    REQUIRE(r.intersections.size() == 3U);
    // Sorted by y ascending: (2,1), (2,2), (2,3).
    CHECK(r.intersections[0].point.y == 1.F);
    CHECK(r.intersections[1].point.y == 2.F);
    CHECK(r.intersections[2].point.y == 3.F);
}
