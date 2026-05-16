#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6d general polygon Boolean operations (Vatti 1992).
//
// Computes the four Boolean operations (union, intersection, difference, XOR)
// over arbitrary planar polygons. Each operand may be:
//
//   * Multipath:                       multiple disjoint outer rings.
//   * Holes:                           CW inner rings carved out of CCW outer rings.
//   * Self-intersecting:               edges crossing within a single ring.
//   * Vertex-coincident with the other operand: shared endpoints, shared edges.
//
// **Algorithm.** Scanbeam-sweep variant of Vatti 1992 ("A generic solution to
// polygon clipping", CACM 35(7):56–63). The plane is partitioned into
// horizontal strips ("scanbeams") by the Y-coordinates of every polygon
// vertex and every intersection point. The sweep processes strips bottom-up;
// at each strip boundary an Active Edge List (AEL) is maintained left-to-
// right. Output polygons are assembled by tracking which edges contribute
// to the boundary of the Boolean result.
//
// **Robustness.** Every orientation, "is-below", and "intersects-here"
// decision uses Shewchuk `orient2d` adaptive precision (Phase 3.1.7 v3a /
// ADR-0076 §18). No naive cross-product or epsilon fallback; degenerate
// cases (vertex-on-edge, collinear segments, three-edges-meeting-at-point)
// are handled in the adaptive path.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11).** All sort keys are lex-
// tuples (Y, X, edge-index); AEL re-insertions deterministic by edge-id
// tiebreak; output polygon walk follows the lowest-edge-id starting point.
// Bit-identical results across MSVC / GCC / clang on x64 / ARM64.
//
// **Two API layers.**
//
//   `polygon_boolean(subject, clip, op, alloc)` — full multi-polygon entry.
//     Returns a `Polygon2<T>` whose outer rings are CCW and hole rings CW
//     (the v6 winding convention pinned at v6a).
//
//   `polygon_union(...)` / `polygon_intersect(...)` / etc. — convenience
//     overloads selecting a single op.
//
// **Fill rule.** Two fill rules are supported:
//
//   * `EvenOdd`: a point is "inside" if a horizontal ray from it crosses an
//     ODD number of polygon edges. This is the SVG default.
//   * `NonZero`: a point is "inside" if the NET winding count is nonzero
//     (counting CCW crossings as +1 and CW as −1). This matches PDF / DXF
//     conventions and is friendly to self-intersecting polygons.
//
// Default: `EvenOdd` (matches PolygonView2's default convention).
//
// **Two-layer typed (ADR-0078 §5 D34).** Algorithm body operates on raw
// `MathScalar T` (`f32`/`f64`); typed `Vec2<Length32>` callers ride
// `polygon_boolean_typed.hpp` strip-compute-retag wrappers (added at slice
// close if a typed-surface consumer pulls).
//
// **Determinism + robustness for the common consumer:** PCB / EDA (Gerber
// trace clipping), navmesh polygon ops, lightmap UV charting, font glyph
// hole-cutout, vector-graphics export — all see bit-identical results.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::polygon
{

enum class BooleanOp : crd::u8
{
    Union        = 0, // subject ∪ clip
    Intersection = 1, // subject ∩ clip
    Difference   = 2, // subject \ clip
    Xor          = 3, // (subject ∪ clip) \ (subject ∩ clip)
};

enum class FillRule : crd::u8
{
    EvenOdd = 0,
    NonZero = 1,
};

enum class BooleanStatus : crd::u8
{
    Ok                  = 0,
    NonFiniteInput      = 1,
    EmptyOperand        = 2, // both subject and clip ring_count() == 0
    InternalInvariant   = 3, // unexpected algorithmic failure
};

struct BooleanOptions
{
    FillRule subject_fill = FillRule::EvenOdd;
    FillRule clip_fill    = FillRule::EvenOdd;

    // If true, output rings are sanitised: consecutive collinear vertices
    // are removed; zero-area rings dropped; consecutive duplicates merged.
    // Recommended ON for downstream consumers (cookers, fonts); OFF for
    // unit tests that want bit-identical raw output.
    bool clean_output = true;
};

template <crd::math::MathScalar T>
struct BooleanResult
{
    Polygon2<T>   output;
    BooleanStatus status = BooleanStatus::Ok;

    explicit BooleanResult(crd::memory::IAllocator* alloc) : output(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == BooleanStatus::Ok; }
};

// Full polygon Boolean.
template <crd::math::MathScalar T>
[[nodiscard]] BooleanResult<T>
polygon_boolean(PolygonView2<T> subject, PolygonView2<T> clip, BooleanOp op,
                crd::memory::IAllocator* alloc, BooleanOptions opts = {});

// Convenience overloads.
template <crd::math::MathScalar T>
[[nodiscard]] inline BooleanResult<T>
polygon_union(PolygonView2<T> subject, PolygonView2<T> clip, crd::memory::IAllocator* alloc,
              BooleanOptions opts = {})
{
    return polygon_boolean(subject, clip, BooleanOp::Union, alloc, opts);
}

template <crd::math::MathScalar T>
[[nodiscard]] inline BooleanResult<T>
polygon_intersect(PolygonView2<T> subject, PolygonView2<T> clip, crd::memory::IAllocator* alloc,
                  BooleanOptions opts = {})
{
    return polygon_boolean(subject, clip, BooleanOp::Intersection, alloc, opts);
}

template <crd::math::MathScalar T>
[[nodiscard]] inline BooleanResult<T>
polygon_difference(PolygonView2<T> subject, PolygonView2<T> clip, crd::memory::IAllocator* alloc,
                   BooleanOptions opts = {})
{
    return polygon_boolean(subject, clip, BooleanOp::Difference, alloc, opts);
}

template <crd::math::MathScalar T>
[[nodiscard]] inline BooleanResult<T>
polygon_xor(PolygonView2<T> subject, PolygonView2<T> clip, crd::memory::IAllocator* alloc,
            BooleanOptions opts = {})
{
    return polygon_boolean(subject, clip, BooleanOp::Xor, alloc, opts);
}

} // namespace crd::geometry::polygon
