#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — 2D convex hull via Andrew's monotone chain (Phase
// 3.1.7 v3b; ADR-0076 §18).
//
// Computes the convex hull of a 2D point set in O(n log n). Output is the
// CCW polygon of hull vertices (boundary).
//
// **Algorithm** (Andrew 1979, "Another Efficient Algorithm for Convex Hulls
// in Two Dimensions"):
//
//   1. Sort the input points lexicographically by (x, y) using
//      `crd::containers::stable_sort` (deterministic per ADR-0063).
//   2. Build the lower hull: iterate left-to-right, maintain a stack;
//      pop while the last three points don't make a left turn (orient2d > 0
//      means CCW = left turn = keep).
//   3. Build the upper hull: iterate right-to-left, same rule.
//   4. Concatenate (lower + upper minus duplicate endpoints).
//
// Output is CCW from outside (standard convention for outward-facing
// polygons).
//
// **Robustness**: uses the v3a `orient2d` exact predicate (Shewchuk 1997
// adaptive). Coplanar/collinear input is handled correctly:
//   - 0 points → empty hull.
//   - 1 point → 1-vertex hull (the point).
//   - 2 distinct points → 2-vertex hull (segment).
//   - 3+ exactly-collinear points → 2-vertex hull (lex-min and lex-max).
//   - All-coincident points → 1-vertex hull.
//
// **Determinism (ADR-0076 §4)**:
//   - Sort tiebreak: stable sort with (x, y) lex comparison; on equal (x, y),
//     lower input index wins (stable sort preserves input order).
//   - "Left turn" decision: `orient2d > 0` (strict positive) keeps the point.
//     Collinear points (`orient2d == 0` from the adaptive predicate) are
//     POPPED — the resulting hull has no degenerate collinear segments on
//     its boundary. This is the published Andrew monotone-chain convention.
//   - Output is bit-exact across compilers / SIMD widths / OSes.
//
// **Builder-reject contract (ADR-0076 §15)**: `CRD_ASSERT(all_finite(points))`
// at function entry. Release path produces a valid-but-degenerate result for
// NaN/Inf input (the adaptive `orient2d` returns 0 on non-finite input — the
// hull "drops" non-finite points).
//
// **API surface** (two forms, same underlying algorithm):
//
//   - `convex_hull_2d_indices(points, out_hull_indices)` — primary. Output
//     is indices into the input `points` array, CCW order. Caller-supplied
//     `Array<u32>&` — the caller owns storage.
//   - `convex_hull_2d_points(points, out_hull_points)` — convenience.
//     Output is the actual `Vec2<T>` positions, CCW order.
//
// Both share the same algorithm and the same determinism guarantees; the
// points form is just `points[indices[i]]` materialized.
//
// **Templated on `T` ∈ {f32, f64}**: f32 callers go through f64 adaptive
// `orient2d` internally (Shewchuk f64 predicate); output type matches input.
//
// **Consumers**:
//   - v3c 3D Quickhull coplanar fallback (degenerate 3D hull → 2D hull on
//     dominant plane).
//   - v6 Vatti polygon Boolean (polygon convex envelope).
//   - v8 2D Delaunay (convex-hull boundary of the point set).
//   - Editor / cooker: visualizing 2D point sets, picking convex shells.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Vec2;

namespace hull_2d_detail
{
// Comparator: lex order on (x, y). Stable sort preserves input index order
// on equal keys, which gives the lowest-input-index tiebreak naturally.
template <MathScalar T> struct LexCompare
{
    [[nodiscard]] bool operator()(const Vec2<T>& a, const Vec2<T>& b) const noexcept
    {
        if (a.x != b.x)
        {
            return a.x < b.x;
        }
        return a.y < b.y;
    }
};

// Comparator wrapping (point, original_index) pairs, so we can recover the
// input index after sorting (the index form requires this).
template <MathScalar T> struct IndexedPoint
{
    Vec2<T> p;
    crd::u32 idx;
};

template <MathScalar T> struct IndexedLexCompare
{
    [[nodiscard]] bool operator()(const IndexedPoint<T>& a, const IndexedPoint<T>& b) const noexcept
    {
        if (a.p.x != b.p.x)
        {
            return a.p.x < b.p.x;
        }
        if (a.p.y != b.p.y)
        {
            return a.p.y < b.p.y;
        }
        // Total-order tiebreak: lower input index wins. This is what stable
        // sort would give us anyway, but making it explicit means we work
        // with non-stable sort too if Cerid's sort impl ever changes.
        return a.idx < b.idx;
    }
};

// Is the turn (a → b → c) a strict left turn (CCW)? Uses the v3a Shewchuk
// adaptive `orient2d`. Returns false on collinear input — collinear runs are
// not part of the hull boundary in Andrew's convention.
template <MathScalar T>
[[nodiscard]] inline bool is_left_turn(const Vec2<T>& a, const Vec2<T>& b, const Vec2<T>& c) noexcept
{
    return crd::geometry::primitives::orient2d(a, b, c) > static_cast<T>(0);
}

} // namespace hull_2d_detail

// ===========================================================================
// PUBLIC API — indices form
// ===========================================================================

// Compute the 2D convex hull of `points`. Output is indices into the input
// array, CCW order. `out_hull_indices` is cleared before writing.
//
// Empty input → empty output. Single point → single-index output. Two
// distinct points → two-index output (segment). All-collinear (>= 3 points)
// → two-index output (lex-min and lex-max — the segment endpoints). All-
// coincident → single-index output (lex-min, which is the first by stable
// sort).
template <MathScalar T>
inline void convex_hull_2d_indices(crd::containers::ConstSpan<Vec2<T>> points,
                                    crd::containers::Array<crd::u32>& out_hull_indices) noexcept
{
    out_hull_indices.clear();
    const crd::usize n = points.size();
    if (n == 0)
    {
        return;
    }

    // Builder-reject contract (ADR-0076 §15): assert finite input in debug.
    // The adaptive orient2d returns 0 on non-finite anyway, so release path
    // produces a valid-but-degenerate hull on Inf/NaN.
    for (crd::usize i = 0; i < n; ++i)
    {
        CRD_ASSERT(std::isfinite(points[i].x) && std::isfinite(points[i].y));
    }

    if (n == 1)
    {
        out_hull_indices.push_back(0);
        return;
    }

    // Sort indexed points by (x, y) lex.
    crd::containers::Array<hull_2d_detail::IndexedPoint<T>> sorted(out_hull_indices.allocator());
    sorted.reserve(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        sorted.push_back({points[i], static_cast<crd::u32>(i)});
    }
    crd::containers::stable_sort(sorted.begin(), sorted.end(),
                                  hull_2d_detail::IndexedLexCompare<T>{});

    // Collapse coincident points: the hull never has two consecutive
    // identical vertices.
    if (n >= 2)
    {
        crd::usize unique_count = 1;
        for (crd::usize i = 1; i < n; ++i)
        {
            if (sorted[i].p.x != sorted[unique_count - 1].p.x ||
                sorted[i].p.y != sorted[unique_count - 1].p.y)
            {
                sorted[unique_count++] = sorted[i];
            }
        }
        sorted.resize(unique_count);
    }

    const crd::usize m = sorted.size();
    if (m == 1)
    {
        out_hull_indices.push_back(sorted[0].idx);
        return;
    }
    if (m == 2)
    {
        out_hull_indices.push_back(sorted[0].idx);
        out_hull_indices.push_back(sorted[1].idx);
        return;
    }

    // Build lower hull: left-to-right pass.
    crd::containers::Array<crd::u32> lower(out_hull_indices.allocator());
    lower.reserve(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        while (lower.size() >= 2)
        {
            const crd::u32 idx_back2 = lower[lower.size() - 2];
            const crd::u32 idx_back1 = lower[lower.size() - 1];
            if (!hull_2d_detail::is_left_turn(points[idx_back2], points[idx_back1], sorted[i].p))
            {
                lower.pop_back();
            }
            else
            {
                break;
            }
        }
        lower.push_back(sorted[i].idx);
    }

    // Build upper hull: right-to-left pass.
    crd::containers::Array<crd::u32> upper(out_hull_indices.allocator());
    upper.reserve(m);
    for (crd::usize i = m; i-- > 0;)
    {
        while (upper.size() >= 2)
        {
            const crd::u32 idx_back2 = upper[upper.size() - 2];
            const crd::u32 idx_back1 = upper[upper.size() - 1];
            if (!hull_2d_detail::is_left_turn(points[idx_back2], points[idx_back1], sorted[i].p))
            {
                upper.pop_back();
            }
            else
            {
                break;
            }
        }
        upper.push_back(sorted[i].idx);
    }

    // Concatenate: drop the last point of each (lower[-1] duplicates upper[0],
    // upper[-1] duplicates lower[0]).
    out_hull_indices.reserve(lower.size() + upper.size() - 2);
    for (crd::usize i = 0; i + 1 < lower.size(); ++i)
    {
        out_hull_indices.push_back(lower[i]);
    }
    for (crd::usize i = 0; i + 1 < upper.size(); ++i)
    {
        out_hull_indices.push_back(upper[i]);
    }
}

// ===========================================================================
// PUBLIC API — points (convenience) form
// ===========================================================================

// Convenience overload. Computes the hull and writes the actual Vec2<T>
// positions (not indices) into `out_hull_points`, CCW order.
template <MathScalar T>
inline void convex_hull_2d_points(crd::containers::ConstSpan<Vec2<T>> points,
                                   crd::containers::Array<Vec2<T>>& out_hull_points) noexcept
{
    crd::containers::Array<crd::u32> indices(out_hull_points.allocator());
    convex_hull_2d_indices<T>(points, indices);
    out_hull_points.clear();
    out_hull_points.reserve(indices.size());
    for (crd::usize i = 0; i < indices.size(); ++i)
    {
        out_hull_points.push_back(points[indices[i]]);
    }
}

} // namespace crd::geometry::convex
