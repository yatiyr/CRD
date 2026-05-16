// crd-geometry-polygon v6a — point-in-polygon classification tests.
//
// Inside / Outside / OnBoundary correctness across:
//   - convex polygons (sanity)
//   - concave polygons (the L-shape that ray-casting must handle correctly)
//   - polygons with holes (inside outer + inside hole ⇒ OUTSIDE the polygon)
//   - boundary points (vertex hits, edge interior hits)
//   - non-finite query points (defensive Outside, no crash)
//
// Robustness driver: Shewchuk `orient2d` adaptive — these tests include
// near-collinear cases that fail naive cross-product fallback.

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using crd::f32;
using crd::math::Vec2;
using crd::geometry::polygon::Polygon2;
using crd::geometry::polygon::PointInPolygon;
using crd::geometry::polygon::Ring2;
using crd::geometry::polygon::point_in_polygon;
using crd::geometry::polygon::point_in_ring;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

Ring2<f32> ring_of(const crd::containers::Array<Vec2<f32>>& v)
{
    return Ring2<f32>{crd::containers::ConstSpan<Vec2<f32>>{v.data(), v.size()}};
}
} // namespace

TEST_CASE("point_in_ring: classification on unit square",
          "[geometry-polygon][pip]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> sq(&f.alloc);
    sq.push_back(Vec2<f32>{0.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 1.F});
    sq.push_back(Vec2<f32>{0.F, 1.F});

    // Strict interior.
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.5F, 0.5F}) == PointInPolygon::Inside);
    // Strict exterior in each direction.
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{-0.1F, 0.5F}) == PointInPolygon::Outside);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{1.1F, 0.5F})  == PointInPolygon::Outside);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.5F, -0.1F}) == PointInPolygon::Outside);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.5F, 1.1F})  == PointInPolygon::Outside);
}

TEST_CASE("point_in_ring: boundary detection on vertices + edge interiors",
          "[geometry-polygon][pip][boundary]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> sq(&f.alloc);
    sq.push_back(Vec2<f32>{0.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 1.F});
    sq.push_back(Vec2<f32>{0.F, 1.F});

    // Vertex hits.
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.F, 0.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{1.F, 0.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{1.F, 1.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.F, 1.F}) == PointInPolygon::OnBoundary);

    // Edge interiors.
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.5F, 0.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{1.F, 0.5F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.5F, 1.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.F, 0.5F}) == PointInPolygon::OnBoundary);
}

TEST_CASE("point_in_ring: L-shape concavity",
          "[geometry-polygon][pip][concave]")
{
    AllocFixture f{};
    // L-shape: outer corner cut out of unit 2x2 at top-right quadrant.
    crd::containers::Array<Vec2<f32>> l_shape(&f.alloc);
    l_shape.push_back(Vec2<f32>{0.F, 0.F});
    l_shape.push_back(Vec2<f32>{2.F, 0.F});
    l_shape.push_back(Vec2<f32>{2.F, 1.F});
    l_shape.push_back(Vec2<f32>{1.F, 1.F});
    l_shape.push_back(Vec2<f32>{1.F, 2.F});
    l_shape.push_back(Vec2<f32>{0.F, 2.F});

    // Top-right CUT-OUT corner — should be Outside even though it's
    // inside the bounding box.
    CHECK(point_in_ring(ring_of(l_shape), Vec2<f32>{1.5F, 1.5F}) == PointInPolygon::Outside);
    // Bottom-left interior.
    CHECK(point_in_ring(ring_of(l_shape), Vec2<f32>{0.5F, 0.5F}) == PointInPolygon::Inside);
    // Right arm.
    CHECK(point_in_ring(ring_of(l_shape), Vec2<f32>{1.5F, 0.5F}) == PointInPolygon::Inside);
    // Top arm.
    CHECK(point_in_ring(ring_of(l_shape), Vec2<f32>{0.5F, 1.5F}) == PointInPolygon::Inside);
}

TEST_CASE("point_in_polygon: inside-outer + inside-hole = Outside",
          "[geometry-polygon][pip][holes]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};

    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{0.F, 0.F});
    outer.push_back(Vec2<f32>{4.F, 0.F});
    outer.push_back(Vec2<f32>{4.F, 4.F});
    outer.push_back(Vec2<f32>{0.F, 4.F});
    p.add_ring(outer);

    crd::containers::Array<Vec2<f32>> hole(&f.alloc);
    hole.push_back(Vec2<f32>{1.F, 1.F});
    hole.push_back(Vec2<f32>{1.F, 3.F});
    hole.push_back(Vec2<f32>{3.F, 3.F});
    hole.push_back(Vec2<f32>{3.F, 1.F});
    p.add_ring(hole); // CW

    auto view = p.view();
    // Inside outer + inside hole ⇒ Outside polygon.
    CHECK(point_in_polygon(view, Vec2<f32>{2.F, 2.F}) == PointInPolygon::Outside);
    // Inside outer + outside hole ⇒ Inside polygon.
    CHECK(point_in_polygon(view, Vec2<f32>{0.5F, 0.5F}) == PointInPolygon::Inside);
    CHECK(point_in_polygon(view, Vec2<f32>{3.5F, 3.5F}) == PointInPolygon::Inside);
    // Outside outer ⇒ Outside.
    CHECK(point_in_polygon(view, Vec2<f32>{-1.F, -1.F}) == PointInPolygon::Outside);
    // Boundary of hole ⇒ OnBoundary.
    CHECK(point_in_polygon(view, Vec2<f32>{1.F, 1.F}) == PointInPolygon::OnBoundary);
}

TEST_CASE("point_in_ring: non-finite query point tolerates (returns Outside)",
          "[geometry-polygon][pip][nan]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> sq(&f.alloc);
    sq.push_back(Vec2<f32>{0.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 1.F});
    sq.push_back(Vec2<f32>{0.F, 1.F});

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{nan, 0.5F}) == PointInPolygon::Outside);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{0.5F, nan}) == PointInPolygon::Outside);
    CHECK(point_in_ring(ring_of(sq), Vec2<f32>{inf, 0.5F}) == PointInPolygon::Outside);
}

TEST_CASE("point_in_ring: empty ring returns Outside",
          "[geometry-polygon][pip][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    CHECK(point_in_ring(ring_of(v), Vec2<f32>{0.5F, 0.5F}) == PointInPolygon::Outside);
}

TEST_CASE("point_in_ring: triangle classification",
          "[geometry-polygon][pip][triangle]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> tri(&f.alloc);
    tri.push_back(Vec2<f32>{0.F, 0.F});
    tri.push_back(Vec2<f32>{4.F, 0.F});
    tri.push_back(Vec2<f32>{2.F, 4.F});

    // Centroid is inside.
    CHECK(point_in_ring(ring_of(tri), Vec2<f32>{2.F, 1.0F}) == PointInPolygon::Inside);
    // Corners on boundary.
    CHECK(point_in_ring(ring_of(tri), Vec2<f32>{0.F, 0.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(tri), Vec2<f32>{4.F, 0.F}) == PointInPolygon::OnBoundary);
    CHECK(point_in_ring(ring_of(tri), Vec2<f32>{2.F, 4.F}) == PointInPolygon::OnBoundary);
    // Outside.
    CHECK(point_in_ring(ring_of(tri), Vec2<f32>{-1.F, -1.F}) == PointInPolygon::Outside);
    CHECK(point_in_ring(ring_of(tri), Vec2<f32>{2.F, 5.F})  == PointInPolygon::Outside);
}

TEST_CASE("point_in_polygon: nested-classification stress (10x10 grid sampling)",
          "[geometry-polygon][pip][grid]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};

    // 10x10 square with two non-overlapping 2x2 holes.
    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{0.F, 0.F});
    outer.push_back(Vec2<f32>{10.F, 0.F});
    outer.push_back(Vec2<f32>{10.F, 10.F});
    outer.push_back(Vec2<f32>{0.F, 10.F});
    p.add_ring(outer);

    crd::containers::Array<Vec2<f32>> h1(&f.alloc);
    h1.push_back(Vec2<f32>{2.F, 2.F});
    h1.push_back(Vec2<f32>{2.F, 4.F});
    h1.push_back(Vec2<f32>{4.F, 4.F});
    h1.push_back(Vec2<f32>{4.F, 2.F});
    p.add_ring(h1);

    crd::containers::Array<Vec2<f32>> h2(&f.alloc);
    h2.push_back(Vec2<f32>{6.F, 6.F});
    h2.push_back(Vec2<f32>{6.F, 8.F});
    h2.push_back(Vec2<f32>{8.F, 8.F});
    h2.push_back(Vec2<f32>{8.F, 6.F});
    p.add_ring(h2);

    auto view = p.view();

    // Strict-interior point per quadrant.
    CHECK(point_in_polygon(view, Vec2<f32>{1.F, 1.F}) == PointInPolygon::Inside);
    CHECK(point_in_polygon(view, Vec2<f32>{9.F, 9.F}) == PointInPolygon::Inside);
    CHECK(point_in_polygon(view, Vec2<f32>{5.F, 1.F}) == PointInPolygon::Inside);
    CHECK(point_in_polygon(view, Vec2<f32>{1.F, 5.F}) == PointInPolygon::Inside);

    // In each hole.
    CHECK(point_in_polygon(view, Vec2<f32>{3.F, 3.F}) == PointInPolygon::Outside);
    CHECK(point_in_polygon(view, Vec2<f32>{7.F, 7.F}) == PointInPolygon::Outside);

    // Far exterior.
    CHECK(point_in_polygon(view, Vec2<f32>{-1.F, -1.F})  == PointInPolygon::Outside);
    CHECK(point_in_polygon(view, Vec2<f32>{11.F, 11.F}) == PointInPolygon::Outside);
}
