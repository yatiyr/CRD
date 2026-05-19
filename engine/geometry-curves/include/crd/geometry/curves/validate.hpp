#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Validation. Phase 3.1.7 v10a (2026-05-19).
//
// **D191 (planned for v10-close)** — `validate(curve) -> CurveValidationResult`
// per-kind validators that report well-formedness with first-offending-index
// granularity. Mirrors `IrValidationResult` (v9e-a D169 analog): cooker-
// friendly because a bad input fails as a cooker error (caller-side
// recovery) instead of an assertion deep inside the evaluator.
//
// Per-kind contracts:
//
//   Polyline3 / Polyline2:
//     - points.size() >= 2.
//     - every point is finite (no NaN / Inf).
//
//   QuadBezier3 / 2, CubicBezier3 / 2:
//     - every control point is finite.
//     - degenerate Bezier (all colocated) is VALID — produces a constant
//       curve, useful for animation start/end pauses.
//
//   CubicHermite3:
//     - p0, p1 finite.
//     - t0, t1 finite (zero tangent is permitted — produces sharp corner).
//
//   CatmullRom3:
//     - points.size() >= 2.
//     - every point is finite.
//     - For centripetal: no two adjacent points colocated (chord-length
//       knot increment would be zero, division by zero in evaluator).
//
//   BSpline3:
//     - points.size() >= 4 (degree + 1).
//     - knots.size() == points.size() + degree + 1.
//     - knots non-decreasing.
//     - knot multiplicity at any single value <= degree + 1 (here: 4).
//
//   CircularArc3:
//     - axis_u, axis_v finite + unit-length (|axis_u| ≈ 1, |axis_v| ≈ 1).
//     - axis_u · axis_v ≈ 0 (orthogonal).
//     - radius > 0.
//     - sweep_radians finite, |sweep_radians| <= 2π (one full revolution
//       max; multi-turn arcs ship via `MultiCircularArc3` follow-on if
//       a consumer asks).
//
//   EllipseArc3 / 2:
//     - same axis constraints as CircularArc.
//     - radius_u > 0, radius_v > 0.
//
//   CircularArc2 / 2D Ellipse arcs:
//     - center finite, radius > 0, start + sweep finite.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/curves/arc.hpp>
#include <crd/geometry/curves/bezier.hpp>
#include <crd/geometry/curves/bspline.hpp>
#include <crd/geometry/curves/catmull_rom.hpp>
#include <crd/geometry/curves/hermite.hpp>
#include <crd/geometry/curves/polyline.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::curves
{

enum class CurveValidationStatus : crd::u8
{
    Ok = 0,
    NotEnoughPoints,        // Polyline / CatmullRom / BSpline below min count
    NonFinitePoint,         // NaN / Inf in a control point
    NonFiniteTangent,       // NaN / Inf in a Hermite tangent
    AdjacentColocated,      // Centripetal CatmullRom: two adjacent points equal
    KnotCountMismatch,      // BSpline: knots.size() != points.size() + degree + 1
    KnotNonMonotonic,       // BSpline: knot vector decreasing
    KnotMultiplicityExceeded, // BSpline: multiplicity > degree + 1
    AxisNotUnit,            // Arc: axis_u or axis_v not unit-length
    AxesNotOrthogonal,      // Arc: axis_u · axis_v not zero
    InvalidRadius,          // Arc: radius <= 0
    SweepOutOfRange,        // Arc: |sweep_radians| > 2π or non-finite
};

struct CurveValidationResult
{
    CurveValidationStatus status        = CurveValidationStatus::Ok;
    crd::u32              offending_index = 0U; // index of first offending element (when applicable)
};

// 3D validators.
template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const Polyline3View<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const Polyline3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const QuadBezier3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const CubicBezier3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const CubicHermite3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const CatmullRom3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const BSpline3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const CircularArc3<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const EllipseArc3<T>& curve) noexcept;

// 2D validators.
template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const Polyline2View<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const Polyline2<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const QuadBezier2<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const CubicBezier2<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const CircularArc2<T>& curve) noexcept;

template <crd::math::MathScalar T>
[[nodiscard]] CurveValidationResult validate(const EllipseArc2<T>& curve) noexcept;

} // namespace crd::geometry::curves
