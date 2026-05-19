// ---------------------------------------------------------------------------
// crd-geometry-curves — Type API surface. Phase 3.1.7 v10a.
//
// Smoke-level coverage of every curve type: constructible, fields accessible,
// view ↔ owning round-trip for Polyline, factories work, both `f32` and `f64`
// instantiate cleanly.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

namespace
{

using namespace crd::geometry::curves;

template <typename T>
[[nodiscard]] crd::math::Vec3<T> v3(T x, T y, T z) noexcept
{
    return crd::math::Vec3<T>(x, y, z);
}

template <typename T>
[[nodiscard]] crd::math::Vec2<T> v2(T x, T y) noexcept
{
    return crd::math::Vec2<T>(x, y);
}

} // namespace

TEST_CASE("v10a Polyline3 owning + view round-trip", "[curves][polyline][types]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> points[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(1.0F, 1.0F, 0.0F),
    };

    Polyline3<float> owned(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{points, 3U}, /*closed_in=*/true);
    REQUIRE(owned.points.size() == 3U);
    REQUIRE(owned.closed);

    const auto view = owned.view();
    REQUIRE(view.points.size() == 3U);
    REQUIRE(view.closed);
    REQUIRE(view.points[0].x == 0.0F);
    REQUIRE(view.points[2].y == 1.0F);
}

TEST_CASE("v10a Polyline2 owning + view round-trip", "[curves][polyline][types]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec2<double> points[] = {v2(0.0, 0.0), v2(1.0, 1.0)};

    Polyline2<double> owned(&alloc, crd::containers::ConstSpan<crd::math::Vec2<double>>{points, 2U});
    const auto        view = owned.view();
    REQUIRE(view.points.size() == 2U);
    REQUIRE_FALSE(view.closed);
}

TEST_CASE("v10a Bezier types are constructible (3D + 2D, f32 + f64)", "[curves][bezier][types]")
{
    {
        QuadBezier3<float> q(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 0.0F), v3(2.0F, 0.0F, 0.0F));
        REQUIRE(q.p1.x == 1.0F);

        CubicBezier3<float> c(v3(0.0F, 0.0F, 0.0F),
                              v3(1.0F, 2.0F, 0.0F),
                              v3(2.0F, 2.0F, 0.0F),
                              v3(3.0F, 0.0F, 0.0F));
        REQUIRE(c.p3.x == 3.0F);
    }
    {
        QuadBezier3<double> q(v3(0.0, 0.0, 0.0), v3(1.0, 1.0, 0.0), v3(2.0, 0.0, 0.0));
        CubicBezier3<double> c(v3(0.0, 0.0, 0.0), v3(1.0, 2.0, 0.0), v3(2.0, 2.0, 0.0), v3(3.0, 0.0, 0.0));
        REQUIRE(q.p2.x == 2.0);
        REQUIRE(c.p2.x == 2.0);
    }
    {
        QuadBezier2<float> q(v2(0.0F, 0.0F), v2(1.0F, 1.0F), v2(2.0F, 0.0F));
        CubicBezier2<float> c(v2(0.0F, 0.0F), v2(1.0F, 1.0F), v2(2.0F, 1.0F), v2(3.0F, 0.0F));
        REQUIRE(q.p0.y == 0.0F);
        REQUIRE(c.p2.x == 2.0F);
    }
}

TEST_CASE("v10a CubicHermite3 is constructible", "[curves][hermite][types]")
{
    CubicHermite3<float> h(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F), v3(1.0F, 0.0F, 0.0F));
    REQUIRE(h.p1.x == 1.0F);
    REQUIRE(h.t0.x == 1.0F);
}

TEST_CASE("v10a CatmullRom3 stores points and parameterisation", "[curves][catmull_rom][types]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> points[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(2.0F, 1.0F, 0.0F),
        v3(3.0F, 0.0F, 0.0F),
    };

    CatmullRom3<float> cr(&alloc,
                          crd::containers::ConstSpan<crd::math::Vec3<float>>{points, 4U},
                          CatmullRomParam::Centripetal,
                          /*closed_in=*/false);
    REQUIRE(cr.points.size() == 4U);
    REQUIRE(cr.param == CatmullRomParam::Centripetal);
    REQUIRE_FALSE(cr.closed);
}

TEST_CASE("v10a BSpline3 uniform-open factory produces a correct knot vector",
          "[curves][bspline][types][factory]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> points[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(1.0F, 1.0F, 0.0F),
        v3(0.0F, 1.0F, 0.0F),
        v3(-1.0F, 1.0F, 0.0F),
    };

    const auto bspline = BSpline3<float>::make_uniform_open(
        &alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{points, 5U});

    REQUIRE(bspline.points.size() == 5U);
    // For n=5, knots.size() = n + degree + 1 = 9.
    REQUIRE(bspline.knots.size() == 9U);
    // First 4 knots clamped to 0.
    REQUIRE(bspline.knots[0] == 0.0F);
    REQUIRE(bspline.knots[1] == 0.0F);
    REQUIRE(bspline.knots[2] == 0.0F);
    REQUIRE(bspline.knots[3] == 0.0F);
    // 1 interior knot at value 1.
    REQUIRE(bspline.knots[4] == 1.0F);
    // Last 4 knots clamped to (n - degree) = 2.
    REQUIRE(bspline.knots[5] == 2.0F);
    REQUIRE(bspline.knots[6] == 2.0F);
    REQUIRE(bspline.knots[7] == 2.0F);
    REQUIRE(bspline.knots[8] == 2.0F);
}

TEST_CASE("v10a CircularArc3 + EllipseArc3 are constructible", "[curves][arc][types]")
{
    CircularArc3<float> c(v3(0.0F, 0.0F, 0.0F),
                           v3(1.0F, 0.0F, 0.0F),
                           v3(0.0F, 1.0F, 0.0F),
                           /*radius_in=*/2.0F,
                           /*sweep_radians_in=*/3.14159265F);
    REQUIRE(c.radius == 2.0F);

    EllipseArc3<double> e(v3(0.0, 0.0, 0.0),
                           v3(1.0, 0.0, 0.0),
                           v3(0.0, 1.0, 0.0),
                           /*radius_u_in=*/3.0,
                           /*radius_v_in=*/1.5,
                           /*sweep_radians_in=*/1.0);
    REQUIRE(e.radius_u == 3.0);
    REQUIRE(e.radius_v == 1.5);
}

TEST_CASE("v10a CircularArc2 + EllipseArc2 (2D peers) are constructible",
          "[curves][arc][types][peers2d]")
{
    CircularArc2<float> c(v2(0.0F, 0.0F), /*radius_in=*/2.0F, /*start_radians_in=*/0.0F, /*sweep_radians_in=*/3.14F);
    REQUIRE(c.radius == 2.0F);

    EllipseArc2<double> e(v2(0.0, 0.0), 3.0, 1.5, 0.0, 1.0);
    REQUIRE(e.radius_v == 1.5);
}

TEST_CASE("v10a CurveKind enums use append-only ordering", "[curves][types][stability]")
{
    // D183 — the enum values are part of the cooked-data contract. The
    // numeric order is fixed at v10a; any future kind appends to the end.
    REQUIRE(static_cast<crd::u8>(CurveKind3::Polyline)     == 0U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::QuadBezier)   == 1U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::CubicBezier)  == 2U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::CubicHermite) == 3U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::CatmullRom)   == 4U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::BSpline)      == 5U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::CircularArc)  == 6U);
    REQUIRE(static_cast<crd::u8>(CurveKind3::EllipseArc)   == 7U);

    REQUIRE(static_cast<crd::u8>(CurveKind2::Polyline)    == 0U);
    REQUIRE(static_cast<crd::u8>(CurveKind2::EllipseArc)  == 4U);
}
