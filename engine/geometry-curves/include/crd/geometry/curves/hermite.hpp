#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Cubic Hermite. Phase 3.1.7 v10a (2026-05-19).
//
// Cubic curve specified by position + tangent (velocity) at each endpoint.
// The most common animation/keyframe curve form — Maya / Blender / Unity
// all use Hermite under the hood. Equivalent to a CubicBezier with the
// basis-change identity:
//     CubicBezier(P0, P0 + T0/3, P3 - T1/3, P3)  ≡  CubicHermite(P0, T0, P3, T1)
// Verified bit-by-bit in the v10a test corpus.
//
// 3D-only in v10a. A `CubicHermite2` 2D peer ships when an authoring
// consumer asks — 2D Hermite is a straight `Vec2`-substitution of this
// code; no algorithm change. Filed as `v10a-hermite2` follow-on.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::curves
{

template <crd::math::MathValue T> struct CubicHermite3
{
    using scalar_t = T;

    // Hermite is never closed by type (closing requires p0 == p1 + tangent
    // continuity which the type doesn't enforce). Static-constexpr for
    // uniform `curve.closed` access in the generic sampler.
    static constexpr bool closed = false;

    crd::math::Vec3<T> p0{}; // position at t=0
    crd::math::Vec3<T> t0{}; // tangent (velocity) at t=0
    crd::math::Vec3<T> p1{}; // position at t=1
    crd::math::Vec3<T> t1{}; // tangent (velocity) at t=1

    constexpr CubicHermite3() noexcept = default;
    constexpr CubicHermite3(const crd::math::Vec3<T>& p0_in,
                            const crd::math::Vec3<T>& t0_in,
                            const crd::math::Vec3<T>& p1_in,
                            const crd::math::Vec3<T>& t1_in) noexcept
        : p0(p0_in), t0(t0_in), p1(p1_in), t1(t1_in)
    {
    }
};

} // namespace crd::geometry::curves
