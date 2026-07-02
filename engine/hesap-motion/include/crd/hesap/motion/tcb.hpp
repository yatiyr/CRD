#pragma once

// crd-hesap-motion v13-q — KOCHANEK-BARTELS (TCB) spline: a Hermite spline whose tangents are shaped by three
// intuitive knobs — Tension, Continuity, Bias — giving an artist/animator direct control over how a keyframed path
// eases through each control point. Catmull-Rom is the special case T=C=B=0. The camera/keyframe-animation primitive
// (extends the parametric Catmull-Rom that geometry-curves ships, adding the TCB shaping). Scalar per-axis; call once
// per component. Gate: endpoint interpolation + the Catmull-Rom special case. Moat: determinism + allocation-free.

#include <crd/core/types.hpp>

namespace crd::hesap::motion
{

// Kochanek-Bartels interpolation on the segment [p0, p1] with neighbours p_{-1} (pm1) and p2, at u ∈ [0,1]. tension /
// continuity / bias in [−1,1] (all 0 = Catmull-Rom). Returns the interpolated value.
template <typename T>
[[nodiscard]] T kb_eval(T pm1, T p0, T p1, T p2, T tension, T continuity, T bias, T u)
{
    const T k = (T{1} - tension) / T{2};
    // outgoing tangent at p0, incoming tangent at p1 (the KB weights)
    const T m0 = k * ((T{1} + bias) * (T{1} + continuity) * (p0 - pm1) + (T{1} - bias) * (T{1} - continuity) * (p1 - p0));
    const T m1 = k * ((T{1} + bias) * (T{1} - continuity) * (p1 - p0) + (T{1} - bias) * (T{1} + continuity) * (p2 - p1));
    const T u2  = u * u;
    const T u3  = u2 * u;
    const T h00 = T{2} * u3 - T{3} * u2 + T{1};
    const T h10 = u3 - T{2} * u2 + u;
    const T h01 = -T{2} * u3 + T{3} * u2;
    const T h11 = u3 - u2;
    return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
}

} // namespace crd::hesap::motion
