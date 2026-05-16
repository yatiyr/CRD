// crd-geometry-polygon v6a — predicate tests.
//
// Covers signed_area, centroid, aabb, is_ccw, is_simple, ensure_orientation.
// Determinism + adverse-input + degenerate-input + winding-convention.

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec2;
using crd::geometry::polygon::Polygon2;
using crd::geometry::polygon::Ring2;
using crd::geometry::polygon::aabb;
using crd::geometry::polygon::centroid;
using crd::geometry::polygon::ensure_orientation;
using crd::geometry::polygon::is_ccw;
using crd::geometry::polygon::is_cw;
using crd::geometry::polygon::is_simple;
using crd::geometry::polygon::signed_area;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

template <typename T> Ring2<T> ring_of(const crd::containers::Array<Vec2<T>>& v)
{
    return Ring2<T>{crd::containers::ConstSpan<Vec2<T>>{v.data(), v.size()}};
}
} // namespace

TEST_CASE("signed_area: CCW unit square returns +1.0", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{0.F, 1.F});

    const f32 a = signed_area(ring_of(v));
    CHECK(a == 1.0F);
    CHECK(is_ccw(ring_of(v)));
    CHECK_FALSE(is_cw(ring_of(v)));
}

TEST_CASE("signed_area: CW unit square returns -1.0", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{0.F, 1.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{1.F, 0.F});

    const f32 a = signed_area(ring_of(v));
    CHECK(a == -1.0F);
    CHECK_FALSE(is_ccw(ring_of(v)));
    CHECK(is_cw(ring_of(v)));
}

TEST_CASE("signed_area: degenerate (n<3) returns zero", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    CHECK(signed_area(ring_of(v)) == 0.F);
    v.push_back(Vec2<f32>{0.F, 0.F});
    CHECK(signed_area(ring_of(v)) == 0.F);
    v.push_back(Vec2<f32>{1.F, 0.F});
    CHECK(signed_area(ring_of(v)) == 0.F);
}

TEST_CASE("signed_area: collinear ring returns zero", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    v.push_back(Vec2<f32>{2.F, 0.F});
    v.push_back(Vec2<f32>{3.F, 0.F});

    CHECK(signed_area(ring_of(v)) == 0.F);
    CHECK_FALSE(is_ccw(ring_of(v)));
    CHECK_FALSE(is_cw(ring_of(v)));
}

TEST_CASE("signed_area: scales with polygon size (f64 precision)", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> v(&f.alloc);
    const f64 s = 100.0;
    v.push_back(Vec2<f64>{0.0, 0.0});
    v.push_back(Vec2<f64>{s, 0.0});
    v.push_back(Vec2<f64>{s, s});
    v.push_back(Vec2<f64>{0.0, s});

    CHECK(signed_area(ring_of(v)) == s * s);
}

TEST_CASE("signed_area: polygon with hole subtracts hole area", "[geometry-polygon][predicates]")
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

    // 4*4 - 2*2 = 12.
    CHECK(signed_area(p.view()) == 12.0F);
}

TEST_CASE("centroid: unit square centred at (0.5, 0.5)", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{0.F, 1.F});

    const auto c = centroid(ring_of(v));
    CHECK(c.x == 0.5F);
    CHECK(c.y == 0.5F);
}

TEST_CASE("centroid: degenerate empty ring returns origin", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    const auto c = centroid(ring_of(v));
    CHECK(c.x == 0.F);
    CHECK(c.y == 0.F);
}

TEST_CASE("centroid: collinear ring falls back to vertex mean", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{2.F, 0.F});
    v.push_back(Vec2<f32>{4.F, 0.F});

    const auto c = centroid(ring_of(v));
    // (0+2+4)/3 = 2, (0+0+0)/3 = 0.
    CHECK(c.x == 2.F);
    CHECK(c.y == 0.F);
}

TEST_CASE("aabb: ring computes tight bounds", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{-2.F, -3.F});
    v.push_back(Vec2<f32>{5.F, -3.F});
    v.push_back(Vec2<f32>{5.F, 7.F});
    v.push_back(Vec2<f32>{-2.F, 7.F});

    const auto b = aabb(ring_of(v));
    CHECK(b.min.x == -2.F);
    CHECK(b.min.y == -3.F);
    CHECK(b.max.x == 5.F);
    CHECK(b.max.y == 7.F);
}

TEST_CASE("aabb: empty ring returns inverted sentinel", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    const auto b = aabb(ring_of(v));
    // Sentinel: min = +inf, max = -inf.
    CHECK(std::isinf(b.min.x));
    CHECK(b.min.x > 0.F);
    CHECK(b.max.x < 0.F);
}

TEST_CASE("aabb: polygon view = union over rings", "[geometry-polygon][predicates]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{-1.F, -1.F});
    outer.push_back(Vec2<f32>{5.F, -1.F});
    outer.push_back(Vec2<f32>{5.F, 5.F});
    outer.push_back(Vec2<f32>{-1.F, 5.F});
    p.add_ring(outer);

    crd::containers::Array<Vec2<f32>> hole(&f.alloc);
    hole.push_back(Vec2<f32>{1.F, 1.F});
    hole.push_back(Vec2<f32>{1.F, 3.F});
    hole.push_back(Vec2<f32>{3.F, 3.F});
    hole.push_back(Vec2<f32>{3.F, 1.F});
    p.add_ring(hole);

    const auto b = aabb(p.view());
    CHECK(b.min.x == -1.F);
    CHECK(b.min.y == -1.F);
    CHECK(b.max.x == 5.F);
    CHECK(b.max.y == 5.F);
}

TEST_CASE("is_simple: convex square is simple", "[geometry-polygon][predicates][simple]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{0.F, 1.F});
    CHECK(is_simple(ring_of(v)));
}

TEST_CASE("is_simple: figure-eight (self-intersecting) is NOT simple",
          "[geometry-polygon][predicates][simple]")
{
    AllocFixture f{};
    // (0,0) -> (1,1) -> (1,0) -> (0,1) -> close — the two diagonals cross.
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    v.push_back(Vec2<f32>{0.F, 1.F});
    CHECK_FALSE(is_simple(ring_of(v)));
}

TEST_CASE("is_simple: complex but valid concave polygon stays simple",
          "[geometry-polygon][predicates][simple]")
{
    AllocFixture f{};
    // A simple L-shape — concave but non-self-intersecting.
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{2.F, 0.F});
    v.push_back(Vec2<f32>{2.F, 1.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{1.F, 2.F});
    v.push_back(Vec2<f32>{0.F, 2.F});
    CHECK(is_simple(ring_of(v)));
}

TEST_CASE("ensure_orientation: flips CW outer to CCW + CCW hole to CW",
          "[geometry-polygon][predicates][orient]")
{
    AllocFixture f{};
    Polygon2<f32> wrong{&f.alloc};
    // OUTER stored CW (wrong) — ensure_orientation should flip to CCW.
    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{0.F, 0.F});
    outer.push_back(Vec2<f32>{0.F, 1.F});
    outer.push_back(Vec2<f32>{1.F, 1.F});
    outer.push_back(Vec2<f32>{1.F, 0.F});
    wrong.add_ring(outer);
    // HOLE stored CCW (wrong) — should flip to CW.
    crd::containers::Array<Vec2<f32>> hole(&f.alloc);
    hole.push_back(Vec2<f32>{0.25F, 0.25F});
    hole.push_back(Vec2<f32>{0.75F, 0.25F});
    hole.push_back(Vec2<f32>{0.75F, 0.75F});
    hole.push_back(Vec2<f32>{0.25F, 0.75F});
    wrong.add_ring(hole);

    REQUIRE(signed_area(wrong.outer()) < 0.F); // CW
    REQUIRE(signed_area(wrong.ring(1)) > 0.F); // CCW

    auto fixed = ensure_orientation(wrong.view(), &f.alloc, true);
    CHECK(signed_area(fixed.outer()) > 0.F); // CCW
    CHECK(signed_area(fixed.ring(1)) < 0.F); // CW
    // Total filled area = 1 - 0.25 = 0.75.
    CHECK(signed_area(fixed.view()) == 0.75F);
}

TEST_CASE("ensure_orientation: preserves already-correctly-oriented input",
          "[geometry-polygon][predicates][orient]")
{
    AllocFixture f{};
    Polygon2<f32> good{&f.alloc};
    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{0.F, 0.F});
    outer.push_back(Vec2<f32>{1.F, 0.F});
    outer.push_back(Vec2<f32>{1.F, 1.F});
    outer.push_back(Vec2<f32>{0.F, 1.F});
    good.add_ring(outer);

    auto fixed = ensure_orientation(good.view(), &f.alloc, true);
    // Vertex order preserved.
    CHECK(fixed.outer()[0].x == 0.F);
    CHECK(fixed.outer()[1].x == 1.F);
    CHECK(fixed.outer()[1].y == 0.F);
    CHECK(fixed.outer()[3].x == 0.F);
    CHECK(fixed.outer()[3].y == 1.F);
}

TEST_CASE("signed_area determinism: vertex-order-rotation reproducibility",
          "[geometry-polygon][predicates][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v0(&f.alloc);
    v0.push_back(Vec2<f32>{0.F, 0.F});
    v0.push_back(Vec2<f32>{1.F, 0.F});
    v0.push_back(Vec2<f32>{1.F, 1.F});
    v0.push_back(Vec2<f32>{0.F, 1.F});

    crd::containers::Array<Vec2<f32>> v1(&f.alloc);
    // Same ring rotated by 1.
    v1.push_back(Vec2<f32>{1.F, 0.F});
    v1.push_back(Vec2<f32>{1.F, 1.F});
    v1.push_back(Vec2<f32>{0.F, 1.F});
    v1.push_back(Vec2<f32>{0.F, 0.F});

    // Areas should be identical for any rotation of the same ring.
    const f32 a0 = signed_area(ring_of(v0));
    const f32 a1 = signed_area(ring_of(v1));
    CHECK(a0 == a1);
}
