#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Catmull-Rom splines. Phase 3.1.7 v10a (2026-05-19).
//
// A Catmull-Rom spline is a piecewise cubic that interpolates a sequence of
// control points (UNLIKE Bezier which only interpolates the endpoints).
// Each segment uses 4 successive control points P[i-1], P[i], P[i+1],
// P[i+2] to define a curve between P[i] and P[i+1] with tangents derived
// from the neighbours. The boundary segments (segment 0 and the last)
// require phantom points which v10a generates via reflection.
//
// **D190 (planned for v10-close)** — CatmullRom3 supports BOTH `Uniform`
// AND `Centripetal` parameterisation, selected via the `param` field at
// construction time:
//
//   - Uniform: classical knots `t_i = i`. Sensitive to non-uniform control
//     spacing — can produce self-intersections / wiggle when adjacent
//     control points are unevenly placed.
//   - Centripetal: Yuksel-Schaefer-Keyser 2011, knots `t_i+1 = t_i +
//     |P_i+1 - P_i|^0.5`. The cinematic-camera default — robust to
//     non-uniform spacing, never produces self-intersections, parameter
//     spacing follows √chord-length.
//
// Closed-curve flag: when set, the spline wraps mod n (last point's
// neighbour is the first). Tangent generation at the boundary uses the
// modular neighbours instead of reflection.
//
// Stores the control points as an owned `Array<Vec3<T>>` since the segment
// count depends on the control-point count at runtime.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::curves
{

template <crd::math::MathValue T> struct CatmullRom3
{
    using scalar_t = T;

    crd::containers::Array<crd::math::Vec3<T>> points;
    CatmullRomParam                            param  = CatmullRomParam::Centripetal;
    bool                                       closed = false;

    explicit CatmullRom3(crd::memory::IAllocator* alloc) noexcept : points(alloc) {}

    CatmullRom3(crd::memory::IAllocator*                       alloc,
                crd::containers::ConstSpan<crd::math::Vec3<T>> control_points,
                CatmullRomParam                                param_in  = CatmullRomParam::Centripetal,
                bool                                           closed_in = false)
        : points(alloc), param(param_in), closed(closed_in)
    {
        points.reserve(control_points.size());
        for (const auto& p : control_points) { points.push_back(p); }
    }
};

} // namespace crd::geometry::curves
