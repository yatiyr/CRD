// ---------------------------------------------------------------------------
// crd-geometry-curves — Validators. Phase 3.1.7 v10a (2026-05-19).
//
// Per-kind well-formedness checks. Templated on `T ∈ {f32, f64}`; explicit
// instantiations at the bottom of the TU so the linker has exactly the
// symbols the test corpus needs without dragging the implementations into
// every consumer's TU.
// ---------------------------------------------------------------------------

#include <crd/geometry/curves/validate.hpp>

#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::curves
{
namespace
{

template <crd::math::MathScalar T>
[[nodiscard]] bool is_finite_vec3(const crd::math::Vec3<T>& v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

template <crd::math::MathScalar T>
[[nodiscard]] bool is_finite_vec2(const crd::math::Vec2<T>& v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y);
}

template <crd::math::MathScalar T>
[[nodiscard]] bool approx_unit(const crd::math::Vec3<T>& v) noexcept
{
    const T len_sq = crd::math::length_squared(v);
    // Allow 1e-3 in len_sq (≈ 1e-3 / 2 in length error) — generous to
    // accommodate hand-authored axes from designers that aren't exactly
    // normalised.
    return std::abs(len_sq - static_cast<T>(1)) <= static_cast<T>(1e-3);
}

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate_arc_axes(const crd::math::Vec3<T>& u, const crd::math::Vec3<T>& v) noexcept
{
    if (!is_finite_vec3(u) || !is_finite_vec3(v)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!approx_unit(u)) { return {CurveValidationStatus::AxisNotUnit, 0U}; }
    if (!approx_unit(v)) { return {CurveValidationStatus::AxisNotUnit, 1U}; }
    const T uv = crd::math::dot(u, v);
    if (std::abs(uv) > static_cast<T>(1e-3)) { return {CurveValidationStatus::AxesNotOrthogonal, 0U}; }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
[[nodiscard]] bool valid_sweep(T sweep_radians) noexcept
{
    if (!std::isfinite(sweep_radians)) { return false; }
    constexpr T kTwoPi = static_cast<T>(6.28318530717958647692);
    return std::abs(sweep_radians) <= kTwoPi + static_cast<T>(1e-5);
}

} // namespace

// ---------------------------------------------------------------------------
// Polyline.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const Polyline3View<T>& curve) noexcept
{
    if (curve.points.size() < 2U) { return {CurveValidationStatus::NotEnoughPoints, 0U}; }
    for (crd::usize i = 0U; i < curve.points.size(); ++i)
    {
        if (!is_finite_vec3(curve.points[i]))
        {
            return {CurveValidationStatus::NonFinitePoint, static_cast<crd::u32>(i)};
        }
    }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const Polyline3<T>& curve) noexcept
{
    return validate(curve.view());
}

// ---------------------------------------------------------------------------
// Bezier.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const QuadBezier3<T>& curve) noexcept
{
    if (!is_finite_vec3(curve.p0)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!is_finite_vec3(curve.p1)) { return {CurveValidationStatus::NonFinitePoint, 1U}; }
    if (!is_finite_vec3(curve.p2)) { return {CurveValidationStatus::NonFinitePoint, 2U}; }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const CubicBezier3<T>& curve) noexcept
{
    if (!is_finite_vec3(curve.p0)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!is_finite_vec3(curve.p1)) { return {CurveValidationStatus::NonFinitePoint, 1U}; }
    if (!is_finite_vec3(curve.p2)) { return {CurveValidationStatus::NonFinitePoint, 2U}; }
    if (!is_finite_vec3(curve.p3)) { return {CurveValidationStatus::NonFinitePoint, 3U}; }
    return {CurveValidationStatus::Ok, 0U};
}

// ---------------------------------------------------------------------------
// Hermite.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const CubicHermite3<T>& curve) noexcept
{
    if (!is_finite_vec3(curve.p0)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!is_finite_vec3(curve.p1)) { return {CurveValidationStatus::NonFinitePoint, 1U}; }
    if (!is_finite_vec3(curve.t0)) { return {CurveValidationStatus::NonFiniteTangent, 0U}; }
    if (!is_finite_vec3(curve.t1)) { return {CurveValidationStatus::NonFiniteTangent, 1U}; }
    return {CurveValidationStatus::Ok, 0U};
}

// ---------------------------------------------------------------------------
// CatmullRom.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const CatmullRom3<T>& curve) noexcept
{
    if (curve.points.size() < 2U) { return {CurveValidationStatus::NotEnoughPoints, 0U}; }
    for (crd::usize i = 0U; i < curve.points.size(); ++i)
    {
        if (!is_finite_vec3(curve.points[i]))
        {
            return {CurveValidationStatus::NonFinitePoint, static_cast<crd::u32>(i)};
        }
    }
    // Centripetal cannot have adjacent colocated points (chord = 0 →
    // division by zero in the knot computation).
    if (curve.param == CatmullRomParam::Centripetal)
    {
        const auto n = curve.points.size();
        for (crd::usize i = 1U; i < n; ++i)
        {
            if (crd::math::length_squared(curve.points[i] - curve.points[i - 1U]) <= static_cast<T>(0))
            {
                return {CurveValidationStatus::AdjacentColocated, static_cast<crd::u32>(i)};
            }
        }
        // Closed curve: also check the wrap pair (last → first).
        if (curve.closed && n >= 2U
            && crd::math::length_squared(curve.points[0] - curve.points[n - 1U]) <= static_cast<T>(0))
        {
            return {CurveValidationStatus::AdjacentColocated, static_cast<crd::u32>(n - 1U)};
        }
    }
    return {CurveValidationStatus::Ok, 0U};
}

// ---------------------------------------------------------------------------
// BSpline.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const BSpline3<T>& curve) noexcept
{
    constexpr crd::u32 kDegree = BSpline3<T>::k_degree;
    if (curve.points.size() < kDegree + 1U) { return {CurveValidationStatus::NotEnoughPoints, 0U}; }
    for (crd::usize i = 0U; i < curve.points.size(); ++i)
    {
        if (!is_finite_vec3(curve.points[i]))
        {
            return {CurveValidationStatus::NonFinitePoint, static_cast<crd::u32>(i)};
        }
    }
    const crd::u32 expected_n_knots = static_cast<crd::u32>(curve.points.size()) + kDegree + 1U;
    if (static_cast<crd::u32>(curve.knots.size()) != expected_n_knots)
    {
        return {CurveValidationStatus::KnotCountMismatch, 0U};
    }
    // Knots must be non-decreasing.
    for (crd::usize i = 1U; i < curve.knots.size(); ++i)
    {
        if (curve.knots[i] < curve.knots[i - 1U])
        {
            return {CurveValidationStatus::KnotNonMonotonic, static_cast<crd::u32>(i)};
        }
    }
    // Multiplicity check — no single value appears more than degree + 1 times.
    crd::u32 run = 1U;
    for (crd::usize i = 1U; i < curve.knots.size(); ++i)
    {
        if (curve.knots[i] == curve.knots[i - 1U])
        {
            ++run;
            if (run > kDegree + 1U)
            {
                return {CurveValidationStatus::KnotMultiplicityExceeded, static_cast<crd::u32>(i)};
            }
        }
        else { run = 1U; }
    }
    return {CurveValidationStatus::Ok, 0U};
}

// ---------------------------------------------------------------------------
// Arcs.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const CircularArc3<T>& curve) noexcept
{
    if (!is_finite_vec3(curve.center)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    const auto axes = validate_arc_axes(curve.axis_u, curve.axis_v);
    if (axes.status != CurveValidationStatus::Ok) { return axes; }
    if (!std::isfinite(curve.radius) || curve.radius <= static_cast<T>(0))
    {
        return {CurveValidationStatus::InvalidRadius, 0U};
    }
    if (!valid_sweep(curve.sweep_radians)) { return {CurveValidationStatus::SweepOutOfRange, 0U}; }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const EllipseArc3<T>& curve) noexcept
{
    if (!is_finite_vec3(curve.center)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    const auto axes = validate_arc_axes(curve.axis_u, curve.axis_v);
    if (axes.status != CurveValidationStatus::Ok) { return axes; }
    if (!std::isfinite(curve.radius_u) || curve.radius_u <= static_cast<T>(0))
    {
        return {CurveValidationStatus::InvalidRadius, 0U};
    }
    if (!std::isfinite(curve.radius_v) || curve.radius_v <= static_cast<T>(0))
    {
        return {CurveValidationStatus::InvalidRadius, 1U};
    }
    if (!valid_sweep(curve.sweep_radians)) { return {CurveValidationStatus::SweepOutOfRange, 0U}; }
    return {CurveValidationStatus::Ok, 0U};
}

// ---------------------------------------------------------------------------
// 2D peers.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
CurveValidationResult validate(const Polyline2View<T>& curve) noexcept
{
    if (curve.points.size() < 2U) { return {CurveValidationStatus::NotEnoughPoints, 0U}; }
    for (crd::usize i = 0U; i < curve.points.size(); ++i)
    {
        if (!is_finite_vec2(curve.points[i]))
        {
            return {CurveValidationStatus::NonFinitePoint, static_cast<crd::u32>(i)};
        }
    }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const Polyline2<T>& curve) noexcept
{
    return validate(curve.view());
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const QuadBezier2<T>& curve) noexcept
{
    if (!is_finite_vec2(curve.p0)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!is_finite_vec2(curve.p1)) { return {CurveValidationStatus::NonFinitePoint, 1U}; }
    if (!is_finite_vec2(curve.p2)) { return {CurveValidationStatus::NonFinitePoint, 2U}; }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const CubicBezier2<T>& curve) noexcept
{
    if (!is_finite_vec2(curve.p0)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!is_finite_vec2(curve.p1)) { return {CurveValidationStatus::NonFinitePoint, 1U}; }
    if (!is_finite_vec2(curve.p2)) { return {CurveValidationStatus::NonFinitePoint, 2U}; }
    if (!is_finite_vec2(curve.p3)) { return {CurveValidationStatus::NonFinitePoint, 3U}; }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const CircularArc2<T>& curve) noexcept
{
    if (!is_finite_vec2(curve.center)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!std::isfinite(curve.radius) || curve.radius <= static_cast<T>(0))
    {
        return {CurveValidationStatus::InvalidRadius, 0U};
    }
    if (!std::isfinite(curve.start_radians) || !valid_sweep(curve.sweep_radians))
    {
        return {CurveValidationStatus::SweepOutOfRange, 0U};
    }
    return {CurveValidationStatus::Ok, 0U};
}

template <crd::math::MathScalar T>
CurveValidationResult validate(const EllipseArc2<T>& curve) noexcept
{
    if (!is_finite_vec2(curve.center)) { return {CurveValidationStatus::NonFinitePoint, 0U}; }
    if (!std::isfinite(curve.radius_u) || curve.radius_u <= static_cast<T>(0))
    {
        return {CurveValidationStatus::InvalidRadius, 0U};
    }
    if (!std::isfinite(curve.radius_v) || curve.radius_v <= static_cast<T>(0))
    {
        return {CurveValidationStatus::InvalidRadius, 1U};
    }
    if (!std::isfinite(curve.start_radians) || !valid_sweep(curve.sweep_radians))
    {
        return {CurveValidationStatus::SweepOutOfRange, 0U};
    }
    return {CurveValidationStatus::Ok, 0U};
}

// ---------------------------------------------------------------------------
// Explicit instantiations for f32 + f64.
// ---------------------------------------------------------------------------

// Macro is the standard idiom for explicit template instantiation across
// many types — a `constexpr template function` cannot perform explicit
// instantiation at namespace scope. NOLINT(cppcoreguidelines-macro-usage).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRD_CURVES_INSTANTIATE_VALIDATE(TYPE_TEMPLATE)                                                                   \
    template CurveValidationResult validate(const TYPE_TEMPLATE<crd::f32>&) noexcept;                                    \
    template CurveValidationResult validate(const TYPE_TEMPLATE<crd::f64>&) noexcept;

CRD_CURVES_INSTANTIATE_VALIDATE(Polyline3View)
CRD_CURVES_INSTANTIATE_VALIDATE(Polyline3)
CRD_CURVES_INSTANTIATE_VALIDATE(QuadBezier3)
CRD_CURVES_INSTANTIATE_VALIDATE(CubicBezier3)
CRD_CURVES_INSTANTIATE_VALIDATE(CubicHermite3)
CRD_CURVES_INSTANTIATE_VALIDATE(CatmullRom3)
CRD_CURVES_INSTANTIATE_VALIDATE(BSpline3)
CRD_CURVES_INSTANTIATE_VALIDATE(CircularArc3)
CRD_CURVES_INSTANTIATE_VALIDATE(EllipseArc3)
CRD_CURVES_INSTANTIATE_VALIDATE(Polyline2View)
CRD_CURVES_INSTANTIATE_VALIDATE(Polyline2)
CRD_CURVES_INSTANTIATE_VALIDATE(QuadBezier2)
CRD_CURVES_INSTANTIATE_VALIDATE(CubicBezier2)
CRD_CURVES_INSTANTIATE_VALIDATE(CircularArc2)
CRD_CURVES_INSTANTIATE_VALIDATE(EllipseArc2)

#undef CRD_CURVES_INSTANTIATE_VALIDATE

} // namespace crd::geometry::curves
