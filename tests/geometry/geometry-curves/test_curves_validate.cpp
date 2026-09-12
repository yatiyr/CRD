//:-------------------------------------------------------------------------
// crd-geometry-curves:Validation. Phase 3.1.7 v10a.
//
// Every `validate(curve)` returns Ok on well-formed input + the right status
// on each malformed-input class.
//:-------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

namespace
{

using namespace crd::geometry::curves;

template <typename T>
[[nodiscard]] crd::math::Vec3<T> v3(T x, T y, T z) noexcept
{
    return crd::math::Vec3<T>(x, y, z);
}

} // namespace

TEST_CASE("v10a validate Polyline3: Ok / NotEnoughPoints / NonFinitePoint",
          "[curves][validate][polyline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    {
        const crd::math::Vec3<float> points[] = {v3(0.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 1.0F)};
        Polyline3<float> p(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{points, 2U});
        REQUIRE(validate(p).status == CurveValidationStatus::Ok);
    }
    {
        const crd::math::Vec3<float> single[] = {v3(0.0F, 0.0F, 0.0F)};
        Polyline3<float> p(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{single, 1U});
        REQUIRE(validate(p).status == CurveValidationStatus::NotEnoughPoints);
    }
    {
        const float nan_val = std::nanf("");
        const crd::math::Vec3<float> bad[] = {v3(0.0F, 0.0F, 0.0F), v3(nan_val, 0.0F, 0.0F)};
        Polyline3<float> p(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{bad, 2U});
        const auto result = validate(p);
        REQUIRE(result.status == CurveValidationStatus::NonFinitePoint);
        REQUIRE(result.offending_index == 1U);
    }
}

TEST_CASE("v10a validate Bezier: Ok on finite, NonFinitePoint on NaN/Inf",
          "[curves][validate][bezier]")
{
    const float nan_val = std::nanf("");
    {
        CubicBezier3<float> ok(v3(0.0F, 0.0F, 0.0F), v3(1.0F, 1.0F, 0.0F), v3(2.0F, 1.0F, 0.0F), v3(3.0F, 0.0F, 0.0F));
        REQUIRE(validate(ok).status == CurveValidationStatus::Ok);
    }
    {
        CubicBezier3<float> bad(v3(0.0F, 0.0F, 0.0F),
                                 v3(nan_val, 1.0F, 0.0F),
                                 v3(2.0F, 1.0F, 0.0F),
                                 v3(3.0F, 0.0F, 0.0F));
        const auto result = validate(bad);
        REQUIRE(result.status == CurveValidationStatus::NonFinitePoint);
        REQUIRE(result.offending_index == 1U);
    }
}

TEST_CASE("v10a validate CubicHermite3: NonFiniteTangent kind", "[curves][validate][hermite]")
{
    const float nan_val = std::nanf("");
    CubicHermite3<float> bad(v3(0.0F, 0.0F, 0.0F),
                              v3(nan_val, 0.0F, 0.0F),
                              v3(1.0F, 0.0F, 0.0F),
                              v3(1.0F, 0.0F, 0.0F));
    const auto result = validate(bad);
    REQUIRE(result.status == CurveValidationStatus::NonFiniteTangent);
    REQUIRE(result.offending_index == 0U);
}

TEST_CASE("v10a validate CatmullRom3 centripetal: AdjacentColocated catches duplicate",
          "[curves][validate][catmull_rom]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> dup[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F), // colocated with previous
        v3(2.0F, 1.0F, 0.0F),
    };
    CatmullRom3<float> cr(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{dup, 4U},
                          CatmullRomParam::Centripetal);
    const auto result = validate(cr);
    REQUIRE(result.status == CurveValidationStatus::AdjacentColocated);
    REQUIRE(result.offending_index == 2U);
}

TEST_CASE("v10a validate CatmullRom3 uniform: adjacent colocated points are ALLOWED",
          "[curves][validate][catmull_rom]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> dup[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(2.0F, 1.0F, 0.0F),
    };
    CatmullRom3<float> cr(&alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{dup, 4U},
                          CatmullRomParam::Uniform);
    REQUIRE(validate(cr).status == CurveValidationStatus::Ok);
}

TEST_CASE("v10a validate BSpline3: knot vector contracts", "[curves][validate][bspline]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<float> points[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(1.0F, 1.0F, 0.0F),
        v3(0.0F, 1.0F, 0.0F),
    };

    SECTION("uniform-open factory is Ok")
    {
        const auto bspline = BSpline3<float>::make_uniform_open(
            &alloc, crd::containers::ConstSpan<crd::math::Vec3<float>>{points, 4U});
        REQUIRE(validate(bspline).status == CurveValidationStatus::Ok);
    }

    SECTION("too few control points -> NotEnoughPoints")
    {
        // 3 control points < degree+1 = 4
        BSpline3<float> b(&alloc);
        b.points.push_back(v3(0.0F, 0.0F, 0.0F));
        b.points.push_back(v3(1.0F, 0.0F, 0.0F));
        b.points.push_back(v3(2.0F, 0.0F, 0.0F));
        REQUIRE(validate(b).status == CurveValidationStatus::NotEnoughPoints);
    }

    SECTION("wrong knot count -> KnotCountMismatch")
    {
        BSpline3<float> b(&alloc);
        b.points.reserve(4U);
        for (const auto& p : points) { b.points.push_back(p); }
        b.knots.push_back(0.0F);
        b.knots.push_back(0.0F);
        // Should be 8 knots; we provide 2.
        REQUIRE(validate(b).status == CurveValidationStatus::KnotCountMismatch);
    }

    SECTION("non-monotonic knot vector -> KnotNonMonotonic")
    {
        BSpline3<float> b(&alloc);
        b.points.reserve(4U);
        for (const auto& p : points) { b.points.push_back(p); }
        const float knots[] = {0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 1.0F, 1.0F, 1.0F};
        for (float k : knots) { b.knots.push_back(k); }
        const auto result = validate(b);
        REQUIRE(result.status == CurveValidationStatus::KnotNonMonotonic);
        REQUIRE(result.offending_index == 4U);
    }
}

TEST_CASE("v10a validate CircularArc3: axis constraints + radius + sweep",
          "[curves][validate][arc]")
{
    SECTION("standard arc is Ok")
    {
        CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F),
                                  v3(0.0F, 1.0F, 0.0F),
                                  /*radius_in=*/1.0F,
                                  /*sweep_radians_in=*/1.0F);
        REQUIRE(validate(arc).status == CurveValidationStatus::Ok);
    }

    SECTION("non-unit axis -> AxisNotUnit")
    {
        CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                  v3(2.0F, 0.0F, 0.0F), // |u| = 2
                                  v3(0.0F, 1.0F, 0.0F),
                                  1.0F,
                                  1.0F);
        REQUIRE(validate(arc).status == CurveValidationStatus::AxisNotUnit);
    }

    SECTION("non-orthogonal axes -> AxesNotOrthogonal")
    {
        CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F), // == u, not orthogonal
                                  1.0F,
                                  1.0F);
        // First catches as non-unit because |v| = 1 here, axes are equal so dot=1.
        // Actually |v|=1 so AxisNotUnit doesn't fire; we expect AxesNotOrthogonal.
        REQUIRE(validate(arc).status == CurveValidationStatus::AxesNotOrthogonal);
    }

    SECTION("zero radius -> InvalidRadius")
    {
        CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F),
                                  v3(0.0F, 1.0F, 0.0F),
                                  /*radius_in=*/0.0F,
                                  1.0F);
        REQUIRE(validate(arc).status == CurveValidationStatus::InvalidRadius);
    }

    SECTION("sweep > 2π -> SweepOutOfRange")
    {
        CircularArc3<float> arc(v3(0.0F, 0.0F, 0.0F),
                                  v3(1.0F, 0.0F, 0.0F),
                                  v3(0.0F, 1.0F, 0.0F),
                                  1.0F,
                                  /*sweep_radians_in=*/10.0F);
        REQUIRE(validate(arc).status == CurveValidationStatus::SweepOutOfRange);
    }
}

TEST_CASE("v10a validate EllipseArc3: radius_u and radius_v",
          "[curves][validate][arc][ellipse]")
{
    EllipseArc3<double> bad(v3(0.0, 0.0, 0.0),
                              v3(1.0, 0.0, 0.0),
                              v3(0.0, 1.0, 0.0),
                              /*radius_u_in=*/0.0, // bad
                              /*radius_v_in=*/1.0,
                              1.0);
    const auto r = validate(bad);
    REQUIRE(r.status == CurveValidationStatus::InvalidRadius);
    REQUIRE(r.offending_index == 0U);
}

TEST_CASE("v10a validate 2D peers", "[curves][validate][peers2d]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec2<float> pts[] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {2.0F, 1.0F}};
    Polyline2<float> p(&alloc, crd::containers::ConstSpan<crd::math::Vec2<float>>{pts, 3U});
    REQUIRE(validate(p).status == CurveValidationStatus::Ok);

    QuadBezier2<float> q(crd::math::Vec2<float>{0.0F, 0.0F},
                          crd::math::Vec2<float>{1.0F, 1.0F},
                          crd::math::Vec2<float>{2.0F, 0.0F});
    REQUIRE(validate(q).status == CurveValidationStatus::Ok);

    CircularArc2<float> arc(crd::math::Vec2<float>{0.0F, 0.0F}, /*radius_in=*/1.0F, 0.0F, 1.0F);
    REQUIRE(validate(arc).status == CurveValidationStatus::Ok);
}

TEST_CASE("v10a validate works for f64 instantiations", "[curves][validate][f64]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const crd::math::Vec3<double> pts[] = {v3(0.0, 0.0, 0.0), v3(1.0, 1.0, 1.0)};
    Polyline3<double> p(&alloc, crd::containers::ConstSpan<crd::math::Vec3<double>>{pts, 2U});
    REQUIRE(validate(p).status == CurveValidationStatus::Ok);

    CubicBezier3<double> c(v3(0.0, 0.0, 0.0), v3(1.0, 1.0, 0.0), v3(2.0, 1.0, 0.0), v3(3.0, 0.0, 0.0));
    REQUIRE(validate(c).status == CurveValidationStatus::Ok);
}
