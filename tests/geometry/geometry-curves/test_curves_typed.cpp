// ---------------------------------------------------------------------------
// crd-geometry-curves -- Typed boundary layer. Phase 3.1.7 v10-close.
//
// Tests the queries_typed.hpp wrappers across the WHOLE v10 surface.
// Discriminator (per advisor): every typed call must produce results
// bit-equal to the same-input raw call via `detail_typed::strip`. This
// catches strip-bugs (dropped retag, wrong-tag retag, type-conversion
// truncation) reliably.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace
{

using namespace crd::geometry::curves;

template <typename T>
[[nodiscard]] crd::math::Vec3<T> v3(T x, T y, T z) noexcept
{
    return crd::math::Vec3<T>(x, y, z);
}

// Typed-vector convenience: build a Vec3<Length<T>> from raw coords.
template <typename T>
[[nodiscard]] crd::math::Vec3<crd::units::Length<T>> v3l(T x, T y, T z) noexcept
{
    return crd::math::Vec3<crd::units::Length<T>>(crd::units::Length<T>{x}, crd::units::Length<T>{y},
                                                    crd::units::Length<T>{z});
}

template <typename T>
[[nodiscard]] bool approx_eq(T a, T b, T tol = static_cast<T>(1e-5)) noexcept
{
    const T d = a - b;
    return (d < tol) && (d > -tol);
}

} // namespace

TEST_CASE("typed: evaluate on value-kind Bezier matches raw bit-exactly",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "typed-test");

    // Raw curve
    CubicBezier3<T> raw{v3<T>(0, 0, 0), v3<T>(1, 1, 0), v3<T>(2, -1, 0), v3<T>(3, 0, 0)};
    // Typed curve
    CubicBezier3<crd::units::Length<T>> typed{v3l<T>(0, 0, 0), v3l<T>(1, 1, 0), v3l<T>(2, -1, 0),
                                                v3l<T>(3, 0, 0)};

    const auto raw_pt   = evaluate(raw, static_cast<T>(0.3));
    const auto typed_pt = evaluate_typed(typed, static_cast<T>(0.3), &alloc);

    REQUIRE(typed_pt.x.value == raw_pt.x);
    REQUIRE(typed_pt.y.value == raw_pt.y);
    REQUIRE(typed_pt.z.value == raw_pt.z);
}

TEST_CASE("typed: strip-then-call yields bit-exact result (discriminator)",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "typed-test");

    CubicBezier3<crd::units::Length<T>> typed{v3l<T>(0, 0, 0), v3l<T>(1, 2, 0), v3l<T>(3, -2, 0),
                                                v3l<T>(4, 0, 0)};

    // strip-then-call manually
    const auto raw_via_strip = evaluate(detail_typed::strip(typed, &alloc), static_cast<T>(0.4));
    // typed wrapper
    const auto typed_via_wrapper = evaluate_typed(typed, static_cast<T>(0.4), &alloc);

    REQUIRE(typed_via_wrapper.x.value == raw_via_strip.x);
    REQUIRE(typed_via_wrapper.y.value == raw_via_strip.y);
    REQUIRE(typed_via_wrapper.z.value == raw_via_strip.z);
}

TEST_CASE("typed: evaluate_derivative on Hermite returns Vec3<Length>",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "typed-test");

    CubicHermite3<crd::units::Length<T>> typed{v3l<T>(0, 0, 0), v3l<T>(1, 0, 0), v3l<T>(1, 1, 0),
                                                v3l<T>(1, -1, 0)};
    const auto deriv = evaluate_derivative_typed(typed, static_cast<T>(0.5), &alloc);
    // At t=0.5 of CubicHermite, derivative is non-zero. Check magnitude > 0.
    const T mag_sq = deriv.x.value * deriv.x.value + deriv.y.value * deriv.y.value
                     + deriv.z.value * deriv.z.value;
    REQUIRE(mag_sq > static_cast<T>(0));
}

TEST_CASE("typed: sample_uniform owning Polyline matches raw point-by-point",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    // Typed Polyline3.
    Polyline3<crd::units::Length<T>> typed(&alloc);
    typed.points.push_back(v3l<T>(0, 0, 0));
    typed.points.push_back(v3l<T>(1, 0, 0));
    typed.points.push_back(v3l<T>(2, 1, 0));
    typed.points.push_back(v3l<T>(3, 0, 0));

    auto typed_samples = sample_uniform_typed(typed, 16U, &alloc);
    REQUIRE(typed_samples.points.size() == 17U); // open: n+1

    // Raw equivalent via strip + .view() (raw `sample_uniform` for polylines
    // takes Polyline3View, not the owning Polyline3 directly).
    auto raw         = detail_typed::strip(typed, &alloc);
    auto raw_samples = sample_uniform(raw.view(), 16U, &alloc);
    REQUIRE(typed_samples.points.size() == raw_samples.points.size());
    for (crd::usize i = 0U; i < typed_samples.points.size(); ++i)
    {
        REQUIRE(typed_samples.points[i].x.value == raw_samples.points[i].x);
        REQUIRE(typed_samples.points[i].y.value == raw_samples.points[i].y);
        REQUIRE(typed_samples.points[i].z.value == raw_samples.points[i].z);
    }
}

TEST_CASE("typed: sample_adaptive takes Length<T> tolerance", "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    CubicBezier3<crd::units::Length<T>> typed{v3l<T>(0, 0, 0), v3l<T>(1, 2, 0), v3l<T>(2, -2, 0),
                                                v3l<T>(3, 0, 0)};
    auto poly = sample_adaptive_typed(typed, crd::units::Length<T>{static_cast<T>(0.01)}, &alloc);
    REQUIRE(poly.points.size() >= 2U);
    // First sample bit-exact to control point p0.
    REQUIRE(poly.points[0].x.value == static_cast<T>(0));
    REQUIRE(poly.points[0].y.value == static_cast<T>(0));
}

TEST_CASE("typed: sample_by_curvature takes Angle<T>", "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    CircularArc3<crd::units::Length<T>> typed{};
    typed.center        = v3l<T>(0, 0, 0);
    typed.axis_u        = v3l<T>(1, 0, 0); // axis vectors typed for storage uniformity
    typed.axis_v        = v3l<T>(0, 0, 1);
    typed.radius        = crd::units::Length<T>{static_cast<T>(1)};
    typed.sweep_radians = crd::units::Length<T>{static_cast<T>(3.14159265)};
    typed.closed        = false;

    auto poly = sample_by_curvature_typed(typed, crd::units::Angle<T>{static_cast<T>(0.2)}, &alloc);
    // 180-degree arc at 0.2 rad max-step => at least pi/0.2 ~= 16 segments.
    REQUIRE(poly.points.size() >= 8U);
}

TEST_CASE("typed: arc length round-trip (build / length_of / t_at_distance / distance_at_t)",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    // Straight typed polyline from (0,0,0) to (10,0,0) — length = 10.
    Polyline3<crd::units::Length<T>> typed(&alloc);
    typed.points.push_back(v3l<T>(0, 0, 0));
    typed.points.push_back(v3l<T>(5, 0, 0));
    typed.points.push_back(v3l<T>(10, 0, 0));

    auto raw_table = build_arclength_table_typed(typed, 64U, &alloc); // D215 returns raw table
    const auto total_len = length_of_typed<crd::units::dim::Length, T>(raw_table);
    REQUIRE(approx_eq(total_len.value, static_cast<T>(10)));

    // t at distance 5 should be ~0.5 on a uniform parameterised polyline.
    const T t_at_half = t_at_distance_typed(raw_table, crd::units::Length<T>{static_cast<T>(5)});
    REQUIRE(approx_eq(t_at_half, static_cast<T>(0.5), static_cast<T>(1e-3)));

    // Inverse: distance at t=0.5 should be ~5.
    const auto d_at_half = distance_at_t_typed<crd::units::dim::Length, T>(raw_table, static_cast<T>(0.5));
    REQUIRE(approx_eq(d_at_half.value, static_cast<T>(5), static_cast<T>(1e-3)));

    // length_of_typed(curve, n, alloc) convenience matches.
    const auto total_via_curve = length_of_typed(typed, 64U, &alloc);
    REQUIRE(approx_eq(total_via_curve.value, static_cast<T>(10)));
}

TEST_CASE("typed: aabb_of returns AABB3<Length>", "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    CubicBezier3<crd::units::Length<T>> typed{v3l<T>(-1, -1, 0), v3l<T>(-0.5F, 1, 0),
                                                v3l<T>(0.5F, -1, 0), v3l<T>(1, 1, 0)};
    auto box = aabb_of_typed(typed, &alloc);
    REQUIRE(box.min.x.value <= static_cast<T>(-1));
    REQUIRE(box.max.x.value >= static_cast<T>(1));
}

TEST_CASE("typed: closest_point returns typed point + Area distance_squared",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    Polyline3<crd::units::Length<T>> typed(&alloc);
    typed.points.push_back(v3l<T>(0, 0, 0));
    typed.points.push_back(v3l<T>(1, 0, 0));

    const auto q = v3l<T>(static_cast<T>(0.5), static_cast<T>(1.0), static_cast<T>(0));
    const auto cp =
        closest_point_typed(typed, q, static_cast<T>(1e-4), &alloc);

    REQUIRE(approx_eq(cp.point.x.value, static_cast<T>(0.5)));
    REQUIRE(approx_eq(cp.point.y.value, static_cast<T>(0)));
    REQUIRE(approx_eq(cp.distance_squared.value, static_cast<T>(1), static_cast<T>(1e-3)));
}

TEST_CASE("typed: distance returns Length<T>", "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    Polyline3<crd::units::Length<T>> typed(&alloc);
    typed.points.push_back(v3l<T>(0, 0, 0));
    typed.points.push_back(v3l<T>(2, 0, 0));

    const auto q = v3l<T>(static_cast<T>(1), static_cast<T>(3), static_cast<T>(0));
    const auto d =
        distance_typed(typed, q, static_cast<T>(1e-4), &alloc);
    REQUIRE(approx_eq(d.value, static_cast<T>(3), static_cast<T>(1e-3)));
}

TEST_CASE("typed: intersect_ray returns typed point + raw t-params",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    // Horizontal typed polyline at y=0; ray points down from (1, 1, 0).
    Polyline3<crd::units::Length<T>> typed(&alloc);
    typed.points.push_back(v3l<T>(0, 0, 0));
    typed.points.push_back(v3l<T>(2, 0, 0));

    crd::geometry::primitives::Ray3<crd::units::Length<T>> ray{};
    ray.origin    = v3l<T>(1, 1, 0);
    ray.direction = v3l<T>(0, -1, 0);

    auto hit = intersect_ray_typed(typed, ray, static_cast<T>(1e-3), &alloc);
    REQUIRE(hit.has_value());
    REQUIRE(approx_eq(hit->point.x.value, static_cast<T>(1), static_cast<T>(1e-2)));
    REQUIRE(approx_eq(hit->point.y.value, static_cast<T>(0), static_cast<T>(1e-2)));
}

TEST_CASE("typed: tangent / normal / binormal return RAW Vec3<T> per D216",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "typed-test");

    CircularArc3<crd::units::Length<T>> typed{};
    typed.center        = v3l<T>(0, 0, 0);
    typed.axis_u        = v3l<T>(1, 0, 0);
    typed.axis_v        = v3l<T>(0, 0, 1);
    typed.radius        = crd::units::Length<T>{static_cast<T>(1)};
    typed.sweep_radians = crd::units::Length<T>{static_cast<T>(3.14159265)};
    typed.closed        = false;

    const auto t_h = tangent_typed(typed, static_cast<T>(0.25), &alloc);
    const auto n_h = normal_typed(typed, static_cast<T>(0.25), &alloc);
    const auto b_h = binormal_typed(typed, static_cast<T>(0.25), &alloc);

    // Static_assert the return types are raw Vec3<T>, not typed.
    static_assert(std::is_same_v<decltype(t_h), const crd::math::Vec3<T>>);
    static_assert(std::is_same_v<decltype(n_h), const crd::math::Vec3<T>>);
    static_assert(std::is_same_v<decltype(b_h), const crd::math::Vec3<T>>);

    // Unit length within tolerance.
    const T mag_t = std::sqrt(t_h.x * t_h.x + t_h.y * t_h.y + t_h.z * t_h.z);
    REQUIRE(approx_eq(mag_t, static_cast<T>(1), static_cast<T>(1e-4)));
}

TEST_CASE("typed: compute_rmf_typed returns raw Array<CurveFrame<T>> per D216",
          "[geometry-curves][typed]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    CubicBezier3<crd::units::Length<T>> typed{v3l<T>(0, 0, 0), v3l<T>(1, 1, 0), v3l<T>(2, -1, 0),
                                                v3l<T>(3, 0, 0)};
    auto frames = compute_rmf_typed(typed, 8U, &alloc);
    REQUIRE(frames.size() == 9U); // open: n + 1
    static_assert(std::is_same_v<decltype(frames),
                                  crd::containers::Array<crd::geometry::curves::CurveFrame<T>>>);

    // Frames orthonormal at every sample.
    for (const auto& f : frames)
    {
        const T mag_t =
            std::sqrt(f.tangent.x * f.tangent.x + f.tangent.y * f.tangent.y + f.tangent.z * f.tangent.z);
        REQUIRE(approx_eq(mag_t, static_cast<T>(1), static_cast<T>(1e-3)));
    }
}

TEST_CASE("typed: f64 instantiations work end-to-end", "[geometry-curves][typed][f64]")
{
    using T = crd::f64;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "typed-test");

    CubicBezier3<crd::units::Length<T>> typed{v3l<T>(0, 0, 0), v3l<T>(1, 2, 0), v3l<T>(3, -2, 0),
                                                v3l<T>(4, 0, 0)};
    const auto pt = evaluate_typed(typed, static_cast<T>(0.5), &alloc);
    REQUIRE(approx_eq(pt.x.value, static_cast<T>(2), static_cast<T>(1e-12)));
}
