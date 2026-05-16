// crd-geometry-polygon v6a — substrate-type tests.
//
// Covers: Polygon2 incremental construction, Ring2 / PolygonView2 access
// patterns, ring_offsets prefix-sum invariant, the implicit-closure
// (no-duplicate-last-vertex) contract, and the Polygon2 -> PolygonView2
// implicit-conversion shortcut.

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec2;
using crd::geometry::polygon::Polygon2;
using crd::geometry::polygon::PolygonView2;
using crd::geometry::polygon::Ring2;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

crd::containers::Array<Vec2<f32>> ccw_square(crd::memory::IAllocator* a)
{
    crd::containers::Array<Vec2<f32>> r(a);
    r.push_back(Vec2<f32>{0.F, 0.F});
    r.push_back(Vec2<f32>{1.F, 0.F});
    r.push_back(Vec2<f32>{1.F, 1.F});
    r.push_back(Vec2<f32>{0.F, 1.F});
    return r;
}

crd::containers::Array<Vec2<f32>> cw_hole(crd::memory::IAllocator* a)
{
    // Centred hole, CW per convention.
    crd::containers::Array<Vec2<f32>> r(a);
    r.push_back(Vec2<f32>{0.25F, 0.25F});
    r.push_back(Vec2<f32>{0.25F, 0.75F});
    r.push_back(Vec2<f32>{0.75F, 0.75F});
    r.push_back(Vec2<f32>{0.75F, 0.25F});
    return r;
}
} // namespace

TEST_CASE("Polygon2: empty polygon has zero rings + zero vertices", "[geometry-polygon][types]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};

    CHECK(p.ring_count() == 0U);
    CHECK(p.hole_count() == 0U);
    CHECK(p.vertex_count() == 0U);
    CHECK(p.vertices().empty());
    CHECK(p.ring_offsets().empty());
}

TEST_CASE("Polygon2: add_ring builds prefix-sum offsets", "[geometry-polygon][types]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};

    auto outer = ccw_square(&f.alloc);
    p.add_ring(outer);

    CHECK(p.ring_count() == 1U);
    CHECK(p.hole_count() == 0U);
    CHECK(p.vertex_count() == 4U);

    auto offsets = p.ring_offsets();
    REQUIRE(offsets.size() == 2U);
    CHECK(offsets[0] == 0U);
    CHECK(offsets[1] == 4U);

    auto hole = cw_hole(&f.alloc);
    p.add_ring(hole);

    CHECK(p.ring_count() == 2U);
    CHECK(p.hole_count() == 1U);
    CHECK(p.vertex_count() == 8U);

    offsets = p.ring_offsets();
    REQUIRE(offsets.size() == 3U);
    CHECK(offsets[0] == 0U);
    CHECK(offsets[1] == 4U);
    CHECK(offsets[2] == 8U);
}

TEST_CASE("Polygon2: ring(r) extracts the correct vertex span", "[geometry-polygon][types]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
    p.add_ring(ccw_square(&f.alloc));
    p.add_ring(cw_hole(&f.alloc));

    const Ring2<f32> r0 = p.ring(0);
    REQUIRE(r0.size() == 4U);
    CHECK(r0[0].x == 0.F);
    CHECK(r0[0].y == 0.F);
    CHECK(r0[3].x == 0.F);
    CHECK(r0[3].y == 1.F);

    const Ring2<f32> r1 = p.ring(1);
    REQUIRE(r1.size() == 4U);
    CHECK(r1[0].x == 0.25F);
    CHECK(r1[0].y == 0.25F);
    CHECK(r1[2].x == 0.75F);
    CHECK(r1[2].y == 0.75F);

    CHECK(p.outer().size() == 4U);
    CHECK(p.outer()[0].y == 0.F);
}

TEST_CASE("Ring2: next/prev wrap correctly", "[geometry-polygon][types]")
{
    AllocFixture f{};
    auto verts = ccw_square(&f.alloc);
    Ring2<f32> r{crd::containers::ConstSpan<Vec2<f32>>{verts.data(), verts.size()}};

    CHECK(r.next(0) == 1U);
    CHECK(r.next(1) == 2U);
    CHECK(r.next(2) == 3U);
    CHECK(r.next(3) == 0U); // wraparound

    CHECK(r.prev(0) == 3U); // wraparound
    CHECK(r.prev(1) == 0U);
    CHECK(r.prev(2) == 1U);
    CHECK(r.prev(3) == 2U);
}

TEST_CASE("PolygonView2: implicit conversion from Polygon2", "[geometry-polygon][types]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
    p.add_ring(ccw_square(&f.alloc));
    p.add_ring(cw_hole(&f.alloc));

    // Test the implicit-conversion shortcut to PolygonView2 — the view path
    // every v6 algorithm consumer takes.
    PolygonView2<f32> view = p;
    CHECK(view.ring_count() == 2U);
    CHECK(view.hole_count() == 1U);
    REQUIRE(view.ring_offsets.size() == 3U);
    CHECK(view.ring_offsets[0] == 0U);
    CHECK(view.ring_offsets[1] == 4U);
    CHECK(view.ring_offsets[2] == 8U);

    CHECK(view.outer().size() == 4U);
    CHECK(view.ring(1).size() == 4U);
}

TEST_CASE("Polygon2: clear preserves capacity but resets state", "[geometry-polygon][types]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
    p.add_ring(ccw_square(&f.alloc));
    p.add_ring(cw_hole(&f.alloc));

    REQUIRE(p.ring_count() == 2U);
    p.clear();
    CHECK(p.ring_count() == 0U);
    CHECK(p.vertex_count() == 0U);

    // Re-add rings — should work cleanly.
    p.add_ring(ccw_square(&f.alloc));
    CHECK(p.ring_count() == 1U);
    CHECK(p.vertex_count() == 4U);
}

TEST_CASE("Polygon2<f64>: 64-bit precision tier exists + works", "[geometry-polygon][types]")
{
    AllocFixture f{};
    Polygon2<f64> p{&f.alloc};

    crd::containers::Array<Vec2<f64>> ring(&f.alloc);
    ring.push_back(Vec2<f64>{0.0, 0.0});
    ring.push_back(Vec2<f64>{1.0e9, 0.0});       // large-coord stress
    ring.push_back(Vec2<f64>{1.0e9, 1.0e9});
    ring.push_back(Vec2<f64>{0.0, 1.0e9});
    p.add_ring(ring);

    CHECK(p.ring_count() == 1U);
    CHECK(p.outer()[2].x == 1.0e9);
}
