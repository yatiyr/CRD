#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Uniform / non-uniform cubic B-spline. Phase 3.1.7
// v10a (2026-05-19).
//
// A B-spline is a piecewise polynomial whose continuity (typically C2 for
// degree 3) is automatic by construction. Each segment is influenced by
// `degree + 1` control points; moving a control point only affects the
// segments where it's in the local support.
//
// **v10a ships DEGREE 3** (cubic) as the only supported degree. Higher
// degrees (degree 5 for cinematic surfaces, degree 1 = polyline) are
// filed as `v10a-bspline-arbitrary-degree` follow-on if a consumer asks.
//
// Storage (D189 planned for v10-close):
//   - `points`: owned `Array<Vec3<T>>` of n control points.
//   - `knots`:  owned `Array<T>` of n+4 knot values (n + degree + 1).
//
// Validation contract (v10a-substrate):
//   - `n_control >= 4` (need at least degree+1 control points for a single
//      span).
//   - `n_knots == n_control + degree + 1` (here: n + 4).
//   - knots non-decreasing.
//   - knot multiplicity at any single value <= degree + 1 (here: 4).
//
// Closed-curve flag: when set, the spline wraps mod n. v10a ships closed
// as a per-instance flag — the t-wrap is handled at evaluator time
// (modular reduction at the parameter boundary).
//
// The `evaluate` algorithm uses Cox-de-Boor recursion (v10a:
// implementation in `bspline.cpp`). Cox-de-Boor is the numerically-stable
// canonical form — same role here that de Casteljau plays for Bezier.
// Anchor `bspline.cpp` plus the inline header storage keep the
// instantiation cost low.
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

template <crd::math::MathValue T> struct BSpline3
{
    using scalar_t = T;

    // Fixed at degree 3 for v10a. Higher degrees ship later.
    static constexpr crd::u32 k_degree = 3U;

    crd::containers::Array<crd::math::Vec3<T>> points;
    crd::containers::Array<T>                  knots;
    bool                                       closed = false;

    explicit BSpline3(crd::memory::IAllocator* alloc) noexcept : points(alloc), knots(alloc) {}

    BSpline3(crd::memory::IAllocator*                       alloc,
             crd::containers::ConstSpan<crd::math::Vec3<T>> control_points,
             crd::containers::ConstSpan<T>                  knot_vector,
             bool                                           closed_in = false)
        : points(alloc), knots(alloc), closed(closed_in)
    {
        points.reserve(control_points.size());
        for (const auto& p : control_points) { points.push_back(p); }
        knots.reserve(knot_vector.size());
        for (const auto& k : knot_vector) { knots.push_back(k); }
    }

    // Factory: produce a uniform open B-spline. Knot vector is
    //   [0, 0, 0, 0, 1, 2, ..., (n-3), (n-3), (n-3), (n-3)]
    // for `n` control points (the standard clamped-endpoint convention —
    // first and last knot multiplicities are degree+1 = 4 so the curve
    // passes through p0 and p_{n-1}).
    [[nodiscard]] static BSpline3<T> make_uniform_open(
        crd::memory::IAllocator*                       alloc,
        crd::containers::ConstSpan<crd::math::Vec3<T>> control_points)
    {
        const auto n = static_cast<crd::u32>(control_points.size());
        CRD_ASSERT(n >= k_degree + 1U);

        BSpline3<T> result(alloc);
        result.points.reserve(n);
        for (const auto& p : control_points) { result.points.push_back(p); }

        // Knot vector size = n + degree + 1.
        const crd::u32 n_knots = n + k_degree + 1U;
        result.knots.reserve(n_knots);

        // First (degree + 1) = 4 knots are 0.
        for (crd::u32 i = 0U; i <= k_degree; ++i) { result.knots.push_back(static_cast<T>(0)); }
        // Interior knots go from 1 .. (n - degree - 1) — one less than the
        // number of interior segments. For n=4 there are 0 interior knots
        // beyond the boundary multiplicity; for n=5 there's 1; etc.
        const crd::u32 n_interior = (n > k_degree + 1U) ? (n - k_degree - 1U) : 0U;
        for (crd::u32 i = 1U; i <= n_interior; ++i) { result.knots.push_back(static_cast<T>(i)); }
        // Last (degree + 1) = 4 knots clamp to (n - degree).
        const T end_value = static_cast<T>(n - k_degree);
        for (crd::u32 i = 0U; i <= k_degree; ++i) { result.knots.push_back(end_value); }

        return result;
    }
};

} // namespace crd::geometry::curves
