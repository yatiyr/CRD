#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Bezier curves. Phase 3.1.7 v10a (2026-05-19).
//
// Quadratic + cubic Bezier in 3D and 2D. de Casteljau evaluation is the
// canonical numerically-stable form — repeated linear interpolation
// produces the same answer the Bernstein-polynomial form does, but with
// strictly bounded intermediate values + no catastrophic cancellation.
//
// Closed-curve flag is intentionally NOT supported on Bezier — closing a
// cubic requires forcing P3 == P0 (and typically P2-P0 == -(P1-P0) for
// C1). Callers wanting a closed Bezier loop should compose multiple
// cubics into a Polyline-of-Beziers (a `MultiCubicBezier3` — filed as a
// v10-followon if a consumer needs it).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::curves
{

// ---------------------------------------------------------------------------
// 3D Bezier.
// ---------------------------------------------------------------------------

template <crd::math::MathValue T> struct QuadBezier3
{
    using scalar_t = T;

    // Bezier is never closed by type (closing requires P_n == P_0 + tangent
    // constraints; consumers wanting a closed Bezier compose a MultiBezier).
    // Static-constexpr so the generic sampler `curve.closed` access works
    // uniformly for ALL curve kinds.
    static constexpr bool closed = false;

    crd::math::Vec3<T> p0{};
    crd::math::Vec3<T> p1{};
    crd::math::Vec3<T> p2{};

    constexpr QuadBezier3() noexcept = default;
    constexpr QuadBezier3(const crd::math::Vec3<T>& p0_in,
                          const crd::math::Vec3<T>& p1_in,
                          const crd::math::Vec3<T>& p2_in) noexcept
        : p0(p0_in), p1(p1_in), p2(p2_in)
    {
    }
};

template <crd::math::MathValue T> struct CubicBezier3
{
    using scalar_t = T;

    static constexpr bool closed = false;

    crd::math::Vec3<T> p0{};
    crd::math::Vec3<T> p1{};
    crd::math::Vec3<T> p2{};
    crd::math::Vec3<T> p3{};

    constexpr CubicBezier3() noexcept = default;
    constexpr CubicBezier3(const crd::math::Vec3<T>& p0_in,
                           const crd::math::Vec3<T>& p1_in,
                           const crd::math::Vec3<T>& p2_in,
                           const crd::math::Vec3<T>& p3_in) noexcept
        : p0(p0_in), p1(p1_in), p2(p2_in), p3(p3_in)
    {
    }
};

// ---------------------------------------------------------------------------
// 2D Bezier peers.
// ---------------------------------------------------------------------------

template <crd::math::MathValue T> struct QuadBezier2
{
    using scalar_t = T;

    static constexpr bool closed = false;

    crd::math::Vec2<T> p0{};
    crd::math::Vec2<T> p1{};
    crd::math::Vec2<T> p2{};

    constexpr QuadBezier2() noexcept = default;
    constexpr QuadBezier2(const crd::math::Vec2<T>& p0_in,
                          const crd::math::Vec2<T>& p1_in,
                          const crd::math::Vec2<T>& p2_in) noexcept
        : p0(p0_in), p1(p1_in), p2(p2_in)
    {
    }
};

template <crd::math::MathValue T> struct CubicBezier2
{
    using scalar_t = T;

    static constexpr bool closed = false;

    crd::math::Vec2<T> p0{};
    crd::math::Vec2<T> p1{};
    crd::math::Vec2<T> p2{};
    crd::math::Vec2<T> p3{};

    constexpr CubicBezier2() noexcept = default;
    constexpr CubicBezier2(const crd::math::Vec2<T>& p0_in,
                           const crd::math::Vec2<T>& p1_in,
                           const crd::math::Vec2<T>& p2_in,
                           const crd::math::Vec2<T>& p3_in) noexcept
        : p0(p0_in), p1(p1_in), p2(p2_in), p3(p3_in)
    {
    }
};

} // namespace crd::geometry::curves
