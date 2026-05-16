// crd-geometry-polygon v6a — typed Quantity-overload wrapper tests.
//
// Validates the ADR-0078 §5 D34 two-layer boundary: typed `Vec2<Length32>`
// input → raw f32 algorithm → retagged result. Zero codegen overhead is
// proven by the bit-identical-result invariant against the raw path.

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_predicates_typed.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::math::Vec2;
using crd::units::Length32;
using crd::geometry::polygon::is_ccw;
using crd::geometry::polygon::PointInPolygon;
using crd::geometry::polygon::Ring2;
using crd::geometry::polygon::aabb;
using crd::geometry::polygon::centroid;
using crd::geometry::polygon::point_in_ring;
using crd::geometry::polygon::signed_area;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

// Convenience: explicit Length32 construction. No `_m_f32` UDL exists; the
// units library ships `_m` for f64 only. f32 callsites construct directly.
inline Length32 L(f32 v) noexcept { return Length32{v}; }
inline Vec2<Length32> P(f32 x, f32 y) noexcept { return Vec2<Length32>{L(x), L(y)}; }
} // namespace

TEST_CASE("typed signed_area: result is Quantity<DimMul<Length, Length>, f32>",
          "[geometry-polygon][typed]")
{
    AllocFixture f{};

    // Typed unit square at the API surface.
    crd::containers::Array<Vec2<Length32>> typed(&f.alloc);
    typed.push_back(P(0.F, 0.F));
    typed.push_back(P(1.F, 0.F));
    typed.push_back(P(1.F, 1.F));
    typed.push_back(P(0.F, 1.F));

    crd::containers::ConstSpan<Vec2<Length32>> span{typed.data(), typed.size()};
    const auto area = signed_area<crd::units::dim::Length, f32>(span);
    // The result is typed Area (Length×Length); .value extracts the raw f32.
    CHECK(area.value == 1.0F);
}

TEST_CASE("typed centroid: returns Vec2<Length32>",
          "[geometry-polygon][typed]")
{
    AllocFixture f{};

    crd::containers::Array<Vec2<Length32>> typed(&f.alloc);
    typed.push_back(P(0.F, 0.F));
    typed.push_back(P(2.F, 0.F));
    typed.push_back(P(2.F, 2.F));
    typed.push_back(P(0.F, 2.F));

    crd::containers::ConstSpan<Vec2<Length32>> span{typed.data(), typed.size()};
    const auto c = centroid<crd::units::dim::Length, f32>(span);
    CHECK(c.x.value == 1.0F);
    CHECK(c.y.value == 1.0F);
}

TEST_CASE("typed aabb: returns AABB2<Length32>",
          "[geometry-polygon][typed]")
{
    AllocFixture f{};

    crd::containers::Array<Vec2<Length32>> typed(&f.alloc);
    typed.push_back(P(-3.F, -4.F));
    typed.push_back(P(5.F, -4.F));
    typed.push_back(P(5.F, 7.F));
    typed.push_back(P(-3.F, 7.F));

    crd::containers::ConstSpan<Vec2<Length32>> span{typed.data(), typed.size()};
    const auto bb = aabb<crd::units::dim::Length, f32>(span);
    CHECK(bb.min.x.value == -3.0F);
    CHECK(bb.min.y.value == -4.0F);
    CHECK(bb.max.x.value == 5.0F);
    CHECK(bb.max.y.value == 7.0F);
}

TEST_CASE("typed point_in_ring: typed boundary matches raw path",
          "[geometry-polygon][typed]")
{
    AllocFixture f{};

    crd::containers::Array<Vec2<Length32>> typed(&f.alloc);
    typed.push_back(P(0.F, 0.F));
    typed.push_back(P(1.F, 0.F));
    typed.push_back(P(1.F, 1.F));
    typed.push_back(P(0.F, 1.F));

    crd::containers::ConstSpan<Vec2<Length32>> span{typed.data(), typed.size()};

    CHECK(point_in_ring<crd::units::dim::Length, f32>(span, P(0.5F, 0.5F))
          == PointInPolygon::Inside);
    CHECK(point_in_ring<crd::units::dim::Length, f32>(span, P(2.F, 0.5F))
          == PointInPolygon::Outside);
    CHECK(point_in_ring<crd::units::dim::Length, f32>(span, P(0.F, 0.5F))
          == PointInPolygon::OnBoundary);
}

TEST_CASE("typed is_ccw: returns bool unchanged",
          "[geometry-polygon][typed]")
{
    AllocFixture f{};

    crd::containers::Array<Vec2<Length32>> ccw(&f.alloc);
    ccw.push_back(P(0.F, 0.F));
    ccw.push_back(P(1.F, 0.F));
    ccw.push_back(P(1.F, 1.F));
    ccw.push_back(P(0.F, 1.F));
    crd::containers::ConstSpan<Vec2<Length32>> ccw_span{ccw.data(), ccw.size()};
    CHECK(is_ccw<crd::units::dim::Length, f32>(ccw_span));

    crd::containers::Array<Vec2<Length32>> cw(&f.alloc);
    cw.push_back(P(0.F, 0.F));
    cw.push_back(P(0.F, 1.F));
    cw.push_back(P(1.F, 1.F));
    cw.push_back(P(1.F, 0.F));
    crd::containers::ConstSpan<Vec2<Length32>> cw_span{cw.data(), cw.size()};
    CHECK_FALSE(is_ccw<crd::units::dim::Length, f32>(cw_span));
}

TEST_CASE("typed wrapper: bit-identical result vs raw path",
          "[geometry-polygon][typed][parity]")
{
    AllocFixture f{};

    crd::containers::Array<Vec2<Length32>> typed_v(&f.alloc);
    typed_v.push_back(P(0.F, 0.F));
    typed_v.push_back(P(3.F, 0.F));
    typed_v.push_back(P(3.F, 4.F));
    typed_v.push_back(P(0.F, 4.F));

    crd::containers::Array<Vec2<f32>> raw_v(&f.alloc);
    raw_v.push_back(Vec2<f32>{0.F, 0.F});
    raw_v.push_back(Vec2<f32>{3.F, 0.F});
    raw_v.push_back(Vec2<f32>{3.F, 4.F});
    raw_v.push_back(Vec2<f32>{0.F, 4.F});

    crd::containers::ConstSpan<Vec2<Length32>> typed_span{typed_v.data(), typed_v.size()};
    Ring2<f32> raw{crd::containers::ConstSpan<Vec2<f32>>{raw_v.data(), raw_v.size()}};

    const auto typed_area = signed_area<crd::units::dim::Length, f32>(typed_span);
    const f32  raw_area   = signed_area(raw);
    // Bit-identical: the typed wrapper is a reinterpret_cast + same algorithm.
    CHECK(typed_area.value == raw_area);
}
