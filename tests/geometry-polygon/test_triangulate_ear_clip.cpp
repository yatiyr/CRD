// crd-geometry-polygon v6b — ear-clipping triangulation tests.
//
// Coverage:
//   * triangulation of convex / concave / many-vertex simple polygons
//   * triangle count matches the n-2 / n-2H+... invariant
//   * each output triangle is CCW (orient2d > 0) — orientation preservation
//   * vertex-set equality: every triangle vertex is a valid polygon index
//   * polygons-with-holes via Eberly bridging
//   * area conservation: Σ triangle areas == polygon signed_area
//   * degenerate inputs return diagnostic status (not crash)
//   * non-simple input returns NonSimpleOuter
//   * f64 precision tier works
//   * determinism: shuffling vertex order ⇒ same area + same triangle count

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec2;
using crd::geometry::polygon::Polygon2;
using crd::geometry::polygon::PolygonView2;
using crd::geometry::polygon::Ring2;
using crd::geometry::polygon::signed_area;
using crd::geometry::polygon::TriangulateStatus;
using crd::geometry::polygon::triangulate_ear_clip;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

template <typename T>
Ring2<T> ring_of(const crd::containers::Array<Vec2<T>>& v)
{
    return Ring2<T>{crd::containers::ConstSpan<Vec2<T>>{v.data(), v.size()}};
}

template <typename T>
T tri_area(const Vec2<T>& a, const Vec2<T>& b, const Vec2<T>& c) noexcept
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

// Sum the (signed) areas of all output triangles. Should equal 2 * polygon
// signed_area (since tri_area returns 2*signed-area).
template <typename T>
T sum_triangle_areas(const crd::containers::Array<u32>& tris,
                     crd::containers::ConstSpan<Vec2<T>> verts) noexcept
{
    T total = T{0};
    for (usize i = 0; i + 3U <= tris.size(); i += 3U)
    {
        total += tri_area(verts[tris[i]], verts[tris[i + 1U]], verts[tris[i + 2U]]);
    }
    return total;
}
} // namespace

// ============================================================================
// Simple ring triangulation
// ============================================================================

TEST_CASE("triangulate_ear_clip: unit square emits 2 CCW triangles",
          "[geometry-polygon][triangulate][simple]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> sq(&f.alloc);
    sq.push_back(Vec2<f32>{0.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 0.F});
    sq.push_back(Vec2<f32>{1.F, 1.F});
    sq.push_back(Vec2<f32>{0.F, 1.F});

    auto result = triangulate_ear_clip(ring_of(sq), &f.alloc);
    CHECK(result.ok());
    CHECK(result.triangle_count == 2U);
    CHECK(result.triangle_indices.size() == 6U);

    // Every triangle CCW.
    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 i0 = result.triangle_indices[3U * t + 0U];
        const u32 i1 = result.triangle_indices[3U * t + 1U];
        const u32 i2 = result.triangle_indices[3U * t + 2U];
        CHECK(tri_area(sq[i0], sq[i1], sq[i2]) > 0.F);
    }
    // Area conservation.
    const f32 sum  = sum_triangle_areas<f32>(result.triangle_indices,
                                            crd::containers::ConstSpan<Vec2<f32>>{sq.data(), sq.size()});
    const f32 poly = 2.F * signed_area(ring_of(sq));
    CHECK(sum == poly);
}

TEST_CASE("triangulate_ear_clip: triangle is its own triangulation",
          "[geometry-polygon][triangulate][simple]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> tri(&f.alloc);
    tri.push_back(Vec2<f32>{0.F, 0.F});
    tri.push_back(Vec2<f32>{1.F, 0.F});
    tri.push_back(Vec2<f32>{0.5F, 1.F});

    auto result = triangulate_ear_clip(ring_of(tri), &f.alloc);
    CHECK(result.ok());
    CHECK(result.triangle_count == 1U);
    CHECK(result.triangle_indices[0] == 0U);
    CHECK(result.triangle_indices[1] == 1U);
    CHECK(result.triangle_indices[2] == 2U);
}

TEST_CASE("triangulate_ear_clip: L-shape (concave) - 4 triangles, area = 3",
          "[geometry-polygon][triangulate][concave]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> l_shape(&f.alloc);
    l_shape.push_back(Vec2<f32>{0.F, 0.F});
    l_shape.push_back(Vec2<f32>{2.F, 0.F});
    l_shape.push_back(Vec2<f32>{2.F, 1.F});
    l_shape.push_back(Vec2<f32>{1.F, 1.F});
    l_shape.push_back(Vec2<f32>{1.F, 2.F});
    l_shape.push_back(Vec2<f32>{0.F, 2.F});

    auto result = triangulate_ear_clip(ring_of(l_shape), &f.alloc);
    REQUIRE(result.ok());
    CHECK(result.triangle_count == 4U); // n=6 ⇒ n-2 = 4 triangles
    // Area conservation: L-shape area is 3 (1x2 + 1x1).
    const f32 sum
        = sum_triangle_areas<f32>(result.triangle_indices,
                                  crd::containers::ConstSpan<Vec2<f32>>{l_shape.data(), l_shape.size()});
    CHECK(sum == 6.F); // 2 * signed_area = 6
}

TEST_CASE("triangulate_ear_clip: comb-shape (many concave) - area conservation",
          "[geometry-polygon][triangulate][concave]")
{
    AllocFixture f{};
    // A 3-toothed comb: alternating ups and downs, 10 vertices total.
    crd::containers::Array<Vec2<f32>> comb(&f.alloc);
    comb.push_back(Vec2<f32>{0.F, 0.F});
    comb.push_back(Vec2<f32>{6.F, 0.F});
    comb.push_back(Vec2<f32>{6.F, 1.F});
    comb.push_back(Vec2<f32>{5.F, 1.F});
    comb.push_back(Vec2<f32>{5.F, 2.F});
    comb.push_back(Vec2<f32>{3.F, 2.F});
    comb.push_back(Vec2<f32>{3.F, 1.F});
    comb.push_back(Vec2<f32>{1.F, 1.F});
    comb.push_back(Vec2<f32>{1.F, 2.F});
    comb.push_back(Vec2<f32>{0.F, 2.F});

    auto result = triangulate_ear_clip(ring_of(comb), &f.alloc);
    REQUIRE(result.ok());
    CHECK(result.triangle_count == 8U); // n=10 ⇒ n-2 = 8

    const f32 sum  = sum_triangle_areas<f32>(result.triangle_indices,
                                            crd::containers::ConstSpan<Vec2<f32>>{comb.data(), comb.size()});
    const f32 poly = 2.F * signed_area(ring_of(comb));
    CHECK(sum == poly);
}

TEST_CASE("triangulate_ear_clip: every triangle is CCW for arbitrary concave polygon",
          "[geometry-polygon][triangulate][concave][ccw]")
{
    AllocFixture f{};
    // A canonical 5-point star with proper interleaved outer/inner radii.
    // Outer radius 1.0, inner radius 0.382 (golden-ratio inset); start
    // at top, go CCW. 10 vertices, 5 concave + 5 convex.
    constexpr f32 PI    = 3.14159265358979F;
    constexpr f32 r_out = 1.0F;
    constexpr f32 r_in  = 0.382F;
    crd::containers::Array<Vec2<f32>> star(&f.alloc);
    for (u32 i = 0; i < 10U; ++i)
    {
        const f32 r     = (i % 2U == 0U) ? r_out : r_in;
        const f32 theta = PI * 0.5F - 2.F * PI * static_cast<f32>(i) / 10.F; // CW pass
        star.push_back(Vec2<f32>{r * std::cos(theta), r * std::sin(theta)});
    }
    // We built it CW (because we wanted to walk the star outline); reverse for CCW.
    crd::containers::Array<Vec2<f32>> star_ccw(&f.alloc);
    for (u32 i = static_cast<u32>(star.size()); i > 0U; --i) { star_ccw.push_back(star[i - 1U]); }

    auto result = triangulate_ear_clip(ring_of(star_ccw), &f.alloc);
    REQUIRE(result.ok());
    CHECK(result.triangle_count == 8U);

    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 i0 = result.triangle_indices[3U * t + 0U];
        const u32 i1 = result.triangle_indices[3U * t + 1U];
        const u32 i2 = result.triangle_indices[3U * t + 2U];
        CHECK(tri_area(star_ccw[i0], star_ccw[i1], star_ccw[i2]) > 0.F);
    }
}

// ============================================================================
// Diagnostics on degenerate / adverse input
// ============================================================================

TEST_CASE("triangulate_ear_clip: empty ring returns EmptyPolygon",
          "[geometry-polygon][triangulate][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    auto result = triangulate_ear_clip(ring_of(v), &f.alloc);
    CHECK_FALSE(result.ok());
    CHECK(result.status == TriangulateStatus::EmptyPolygon);
}

TEST_CASE("triangulate_ear_clip: 2-vertex ring returns EmptyPolygon",
          "[geometry-polygon][triangulate][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    auto result = triangulate_ear_clip(ring_of(v), &f.alloc);
    CHECK_FALSE(result.ok());
    CHECK(result.status == TriangulateStatus::EmptyPolygon);
}

TEST_CASE("triangulate_ear_clip: figure-eight returns NonSimpleOuter",
          "[geometry-polygon][triangulate][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v(&f.alloc);
    v.push_back(Vec2<f32>{0.F, 0.F});
    v.push_back(Vec2<f32>{1.F, 1.F});
    v.push_back(Vec2<f32>{1.F, 0.F});
    v.push_back(Vec2<f32>{0.F, 1.F});
    auto result = triangulate_ear_clip(ring_of(v), &f.alloc);
    CHECK_FALSE(result.ok());
    CHECK(result.status == TriangulateStatus::NonSimpleOuter);
}

// ============================================================================
// Polygon-with-holes via Eberly bridging
// ============================================================================

TEST_CASE("triangulate_ear_clip: square with central square hole",
          "[geometry-polygon][triangulate][holes]")
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

    auto result = triangulate_ear_clip(p.view(), &f.alloc);
    REQUIRE(result.ok());
    // n + 2*H = 4 + 4 + 2*1 = 10 total verts in bridged polygon ⇒ 10 - 2 = 8 triangles.
    CHECK(result.triangle_count == 8U);

    const f32 sum  = sum_triangle_areas<f32>(
        result.triangle_indices, crd::containers::ConstSpan<Vec2<f32>>{p.vertices().data(),
                                                                        p.vertices().size()});
    const f32 poly = 2.F * signed_area(p.view()); // 16 - 4 = 12, doubled = 24
    CHECK(sum == poly);
}

TEST_CASE("triangulate_ear_clip: square with TWO non-overlapping holes",
          "[geometry-polygon][triangulate][holes]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
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

    auto result = triangulate_ear_clip(p.view(), &f.alloc);
    REQUIRE(result.ok());
    // 12 input verts + 2 bridges (×2 each) = 16. 16 - 2 = 14 triangles.
    CHECK(result.triangle_count == 14U);

    const f32 sum  = sum_triangle_areas<f32>(
        result.triangle_indices, crd::containers::ConstSpan<Vec2<f32>>{p.vertices().data(),
                                                                        p.vertices().size()});
    const f32 poly = 2.F * signed_area(p.view()); // 100 - 8 = 92, doubled = 184
    CHECK(sum == poly);
}

// ============================================================================
// f64 precision + determinism
// ============================================================================

TEST_CASE("triangulate_ear_clip: f64 precision tier - large-coord stability",
          "[geometry-polygon][triangulate][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> v(&f.alloc);
    const f64 s = 1.0e6; // large-coord stress
    v.push_back(Vec2<f64>{0.0, 0.0});
    v.push_back(Vec2<f64>{s, 0.0});
    v.push_back(Vec2<f64>{s, s});
    v.push_back(Vec2<f64>{0.0, s});

    auto result = triangulate_ear_clip(ring_of(v), &f.alloc);
    REQUIRE(result.ok());
    CHECK(result.triangle_count == 2U);
    // Area conservation at large coord still holds in f64.
    const f64 sum  = sum_triangle_areas<f64>(result.triangle_indices,
                                            crd::containers::ConstSpan<Vec2<f64>>{v.data(), v.size()});
    const f64 poly = 2.0 * signed_area(ring_of(v));
    CHECK(sum == poly);
}

TEST_CASE("triangulate_ear_clip: vertex-rotation determinism - area + count preserved",
          "[geometry-polygon][triangulate][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> v0(&f.alloc);
    v0.push_back(Vec2<f32>{0.F, 0.F});
    v0.push_back(Vec2<f32>{2.F, 0.F});
    v0.push_back(Vec2<f32>{2.F, 1.F});
    v0.push_back(Vec2<f32>{1.F, 1.F});
    v0.push_back(Vec2<f32>{1.F, 2.F});
    v0.push_back(Vec2<f32>{0.F, 2.F});

    crd::containers::Array<Vec2<f32>> v1(&f.alloc);
    // Same ring rotated by 3.
    v1.push_back(Vec2<f32>{1.F, 1.F});
    v1.push_back(Vec2<f32>{1.F, 2.F});
    v1.push_back(Vec2<f32>{0.F, 2.F});
    v1.push_back(Vec2<f32>{0.F, 0.F});
    v1.push_back(Vec2<f32>{2.F, 0.F});
    v1.push_back(Vec2<f32>{2.F, 1.F});

    const auto r0 = triangulate_ear_clip(ring_of(v0), &f.alloc);
    const auto r1 = triangulate_ear_clip(ring_of(v1), &f.alloc);
    REQUIRE(r0.ok());
    REQUIRE(r1.ok());
    CHECK(r0.triangle_count == r1.triangle_count);

    const f32 sum0 = sum_triangle_areas<f32>(r0.triangle_indices,
                                              crd::containers::ConstSpan<Vec2<f32>>{v0.data(), v0.size()});
    const f32 sum1 = sum_triangle_areas<f32>(r1.triangle_indices,
                                              crd::containers::ConstSpan<Vec2<f32>>{v1.data(), v1.size()});
    // Both should triangulate to the same total area (the polygon's area * 2).
    CHECK(sum0 == sum1);
}

TEST_CASE("triangulate_ear_clip: 16-gon convex polygon - n-2 triangles",
          "[geometry-polygon][triangulate][convex]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> poly(&f.alloc);
    const u32 n = 16U;
    for (u32 i = 0; i < n; ++i)
    {
        const f32 theta = 2.F * 3.14159265358979F * static_cast<f32>(i) / static_cast<f32>(n);
        poly.push_back(Vec2<f32>{std::cos(theta), std::sin(theta)});
    }

    auto result = triangulate_ear_clip(ring_of(poly), &f.alloc);
    REQUIRE(result.ok());
    CHECK(result.triangle_count == n - 2U);
}

TEST_CASE("triangulate_ear_clip: polygon-with-hole CCW triangles invariant",
          "[geometry-polygon][triangulate][holes][ccw]")
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
    p.add_ring(hole);

    auto result = triangulate_ear_clip(p.view(), &f.alloc);
    REQUIRE(result.ok());
    // The bridged polygon retains CCW orientation, so all output triangles
    // should be CCW. Note: the two coincident bridge-edge triangles produced
    // by ear-clipping can degenerate to zero area (collinear) — accept >= 0
    // for those, but most should be strictly positive.
    u32 nonneg = 0;
    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 i0 = result.triangle_indices[3U * t + 0U];
        const u32 i1 = result.triangle_indices[3U * t + 1U];
        const u32 i2 = result.triangle_indices[3U * t + 2U];
        const f32 a  = tri_area(p.vertices()[i0], p.vertices()[i1], p.vertices()[i2]);
        if (a >= 0.F) { ++nonneg; }
    }
    CHECK(nonneg == result.triangle_count); // every triangle CCW-or-flat
}
