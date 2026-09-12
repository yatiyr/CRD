#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Circular + elliptical arcs. Phase 3.1.7 v10a
// (2026-05-19).
//
// **3D arcs (CircularArc3 / EllipseArc3)** — analytic curves on a user-
// specified plane. Storage:
//   - `center`: 3D position of the arc centre.
//   - `axis_u`: in-plane unit vector defining the t=0 direction (the
//     "x-axis" of the local plane).
//   - `axis_v`: in-plane unit vector orthogonal to axis_u (the "y-axis").
//   - `radius` (CircularArc3) or `radius_u`/`radius_v` (EllipseArc3).
//   - `sweep_radians`: signed sweep angle in radians. Positive = CCW
//     around the plane normal (`axis_u × axis_v`).
//
// **2D peers (CircularArc2 / EllipseArc2)** — same idea but parameterised
// directly by `start_radians` + `sweep_radians` since 2D plane is the
// canonical plane.
//
// Closed-flag: a CircularArc with `sweep_radians == 2π` is geometrically
// a full circle. Per D188 (closed = per-instance flag, not separate
// type), v10a treats a full circle as a CircularArc with the standard
// closed-flag set; consumers reading `evaluate(arc, 1.0)` get
// bit-equal output to `evaluate(arc, 0.0)`. The implementation just
// applies `t mod 1.0` at the evaluator boundary.
//
// Angle math uses `crd::math::deterministic::sin/cos` per the
// `feedback_always_units` discipline — GPU/CPU bit-portable sin/cos
// across compilers (Cephes-poly approximation; same one v9e ships into
// the GLSL/HLSL preludes for the `_twist` / `_bend` ops).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::curves
{

// ---------------------------------------------------------------------------
// 3D arcs.
// ---------------------------------------------------------------------------

template <crd::math::MathValue T> struct CircularArc3
{
    using scalar_t = T;

    crd::math::Vec3<T> center{};
    crd::math::Vec3<T> axis_u{};        // in-plane t=0 direction (unit)
    crd::math::Vec3<T> axis_v{};        // in-plane orthogonal (unit)
    T                  radius        = T{};
    T                  sweep_radians = T{}; // signed; CCW positive around axis_u × axis_v
    bool               closed        = false;

    constexpr CircularArc3() noexcept = default;
    constexpr CircularArc3(const crd::math::Vec3<T>& center_in,
                            const crd::math::Vec3<T>& axis_u_in,
                            const crd::math::Vec3<T>& axis_v_in,
                            T                         radius_in,
                            T                         sweep_radians_in,
                            bool                      closed_in = false) noexcept
        : center(center_in),
          axis_u(axis_u_in),
          axis_v(axis_v_in),
          radius(radius_in),
          sweep_radians(sweep_radians_in),
          closed(closed_in)
    {
    }
};

template <crd::math::MathValue T> struct EllipseArc3
{
    using scalar_t = T;

    crd::math::Vec3<T> center{};
    crd::math::Vec3<T> axis_u{};
    crd::math::Vec3<T> axis_v{};
    T                  radius_u      = T{};
    T                  radius_v      = T{};
    T                  sweep_radians = T{};
    bool               closed        = false;

    constexpr EllipseArc3() noexcept = default;
    constexpr EllipseArc3(const crd::math::Vec3<T>& center_in,
                           const crd::math::Vec3<T>& axis_u_in,
                           const crd::math::Vec3<T>& axis_v_in,
                           T                         radius_u_in,
                           T                         radius_v_in,
                           T                         sweep_radians_in,
                           bool                      closed_in = false) noexcept
        : center(center_in),
          axis_u(axis_u_in),
          axis_v(axis_v_in),
          radius_u(radius_u_in),
          radius_v(radius_v_in),
          sweep_radians(sweep_radians_in),
          closed(closed_in)
    {
    }
};

// ---------------------------------------------------------------------------
// 2D peers.
// ---------------------------------------------------------------------------

template <crd::math::MathValue T> struct CircularArc2
{
    using scalar_t = T;

    crd::math::Vec2<T> center{};
    T                  radius        = T{};
    T                  start_radians = T{};
    T                  sweep_radians = T{};
    bool               closed        = false;

    constexpr CircularArc2() noexcept = default;
    constexpr CircularArc2(const crd::math::Vec2<T>& center_in,
                            T                         radius_in,
                            T                         start_radians_in,
                            T                         sweep_radians_in,
                            bool                      closed_in = false) noexcept
        : center(center_in),
          radius(radius_in),
          start_radians(start_radians_in),
          sweep_radians(sweep_radians_in),
          closed(closed_in)
    {
    }
};

template <crd::math::MathValue T> struct EllipseArc2
{
    using scalar_t = T;

    crd::math::Vec2<T> center{};
    T                  radius_u      = T{};
    T                  radius_v      = T{};
    T                  start_radians = T{};
    T                  sweep_radians = T{};
    bool               closed        = false;

    constexpr EllipseArc2() noexcept = default;
    constexpr EllipseArc2(const crd::math::Vec2<T>& center_in,
                           T                         radius_u_in,
                           T                         radius_v_in,
                           T                         start_radians_in,
                           T                         sweep_radians_in,
                           bool                      closed_in = false) noexcept
        : center(center_in),
          radius_u(radius_u_in),
          radius_v(radius_v_in),
          start_radians(start_radians_in),
          sweep_radians(sweep_radians_in),
          closed(closed_in)
    {
    }
};

} // namespace crd::geometry::curves
