#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Polyline3 / Polyline2. Phase 3.1.7 v10a (2026-05-19).
//
// A polyline is a chain of straight segments through a sequence of control
// points. It's the simplest "curve" and the canonical *output* of every
// other curve's `to_polyline` / `sample_*` (v10b). It's also a first-class
// curve in its own right — animations frequently use a hand-authored
// polyline path.
//
// **D184 (planned for v10-close)** — Polyline gets BOTH a non-owning view +
// an owning variant. Mirrors `TriangleMeshView` from `crd-geometry-mesh`
// where the non-owning form is the runtime hot-path (sampled curves, cooked
// asset slices) and the owning form is the editor/cooker authoring path.
// The other curve kinds in v10a are small fixed-size value structs (no
// array allocations except BSpline knots) so a single owning form is
// sufficient for them.
//
// **Closed-curve flag (D188)** — a per-instance `bool closed`. When set,
// the last point connects back to the first, and `evaluate(curve, t=1.0)`
// is bit-equal to `evaluate(curve, t=0.0)`. Same evaluator handles both
// open + closed polylines via modular reduction at the boundary.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::curves
{

template <crd::math::MathValue T>
using Vec3 = crd::math::Vec3<T>;

template <crd::math::MathValue T>
using Vec2 = crd::math::Vec2<T>;

// ---------------------------------------------------------------------------
// Polyline3 — non-owning view + owning variant. 3D.
// ---------------------------------------------------------------------------

// Non-owning view over a contiguous run of control points. Stable handle
// for runtime evaluation. Does NOT own — caller guarantees lifetime.
template <crd::math::MathValue T> struct Polyline3View
{
    using scalar_t = T; // D185/D193 — generic samplers deduce T from this alias.

    crd::containers::ConstSpan<Vec3<T>> points;
    bool                                closed = false;

    constexpr Polyline3View() noexcept = default;
    constexpr Polyline3View(crd::containers::ConstSpan<Vec3<T>> p, bool closed_in = false) noexcept
        : points(p), closed(closed_in)
    {
    }
};

// Owning variant. Used by cookers + editors + tests that need to build a
// polyline at runtime and pass it around without lifetime gymnastics.
template <crd::math::MathValue T> struct Polyline3
{
    using scalar_t = T;

    crd::containers::Array<Vec3<T>> points;
    bool                            closed = false;

    explicit Polyline3(crd::memory::IAllocator* alloc) noexcept : points(alloc) {}

    Polyline3(crd::memory::IAllocator*            alloc,
              crd::containers::ConstSpan<Vec3<T>> initial,
              bool                                closed_in = false)
        : points(alloc), closed(closed_in)
    {
        points.reserve(initial.size());
        for (const auto& p : initial) { points.push_back(p); }
    }

    [[nodiscard]] Polyline3View<T> view() const noexcept
    {
        return Polyline3View<T>{crd::containers::ConstSpan<Vec3<T>>{points.data(), points.size()}, closed};
    }
};

// ---------------------------------------------------------------------------
// Polyline2 — 2D peer. Same shape, Vec2.
// ---------------------------------------------------------------------------

template <crd::math::MathValue T> struct Polyline2View
{
    using scalar_t = T;

    crd::containers::ConstSpan<Vec2<T>> points;
    bool                                closed = false;

    constexpr Polyline2View() noexcept = default;
    constexpr Polyline2View(crd::containers::ConstSpan<Vec2<T>> p, bool closed_in = false) noexcept
        : points(p), closed(closed_in)
    {
    }
};

template <crd::math::MathValue T> struct Polyline2
{
    using scalar_t = T;

    crd::containers::Array<Vec2<T>> points;
    bool                            closed = false;

    explicit Polyline2(crd::memory::IAllocator* alloc) noexcept : points(alloc) {}

    Polyline2(crd::memory::IAllocator*            alloc,
              crd::containers::ConstSpan<Vec2<T>> initial,
              bool                                closed_in = false)
        : points(alloc), closed(closed_in)
    {
        points.reserve(initial.size());
        for (const auto& p : initial) { points.push_back(p); }
    }

    [[nodiscard]] Polyline2View<T> view() const noexcept
    {
        return Polyline2View<T>{crd::containers::ConstSpan<Vec2<T>>{points.data(), points.size()}, closed};
    }
};

} // namespace crd::geometry::curves
