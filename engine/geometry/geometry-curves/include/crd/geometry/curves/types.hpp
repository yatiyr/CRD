#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — shared type catalogue. Phase 3.1.7 v10a (2026-05-19).
//
// Kind enums + shared aliases used across every curve header in this module.
// Pulled to its own header so a consumer can `#include <crd/geometry/curves/
// types.hpp>` for a forward-declarable kind switch without dragging in the
// full storage of every curve type.
//
// **D183 (planned for v10-close, ADR-0076 §27)** — type set is FIXED at
// v10a-close: Polyline3 + 7 curve kinds (QuadBezier3, CubicBezier3,
// CubicHermite3, CatmullRom3, BSpline3, CircularArc3, EllipseArc3) + 5 2D
// peers (Polyline2 + QuadBezier2 + CubicBezier2 + CircularArc2 +
// EllipseArc2). Extensions are additive (append new enum values), never
// reorder — mirrors the vtable-stability append-only discipline that
// load-bears v9e D168 + Phase 3.1.7.6 v0-close case study.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>

namespace crd::geometry::curves
{

// 3D curve kinds. The values are part of the storage contract once a curve
// is cooked or serialised — append only, never reorder.
enum class CurveKind3 : crd::u8
{
    Polyline      = 0,
    QuadBezier    = 1,
    CubicBezier   = 2,
    CubicHermite  = 3,
    CatmullRom    = 4,
    BSpline       = 5,
    CircularArc   = 6,
    EllipseArc    = 7,
};

// 2D curve kinds. Same append-only discipline.
enum class CurveKind2 : crd::u8
{
    Polyline      = 0,
    QuadBezier    = 1,
    CubicBezier   = 2,
    CircularArc   = 3,
    EllipseArc    = 4,
};

// CatmullRom3 supports two parameterisations selected at construction time.
//   - Uniform: classical; sensitive to non-uniform control-point spacing
//     (can produce self-intersections / wiggle).
//   - Centripetal: Yuksel-Schaefer-Keyser 2011, the cinematic-camera default.
//     Robust to non-uniform spacing; never produces self-intersections;
//     parameter spacing follows √chord-length.
//
// Naming pin (D190 for v10-close): the construction-time enum lives ON the
// CurveKind3::CatmullRom flag, not as a separate kind value, so cooked-data
// and runtime stays uniform.
enum class CatmullRomParam : crd::u8
{
    Uniform     = 0,
    Centripetal = 1,
};

// Curve parameter type. `t` runs `[0, 1]` for every kind regardless of how
// the underlying spline parameterises internally (the evaluator remaps).
//
// For closed curves, `evaluate(curve, 0.0)` and `evaluate(curve, 1.0)` are
// guaranteed bit-equal — the closed-flag is a per-instance property, NOT a
// separate type (D188 for v10-close). Same evaluator handles t-wrap by
// modular reduction at the curve boundary.
template <typename T>
using CurveParam = T;

} // namespace crd::geometry::curves
