#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6e Bentley-Ottmann 1979 line-segment intersection.
//
// Reports every pairwise intersection of an input set of 2D line segments
// in `O((n + k) log n)` time, where n is segment count and k is intersection
// count. Two segments "intersect" if any interior or endpoint of one lies
// on the other (transverse crossings, T-junctions, vertex-on-vertex, and
// collinear overlap endpoints all reported).
//
// **Algorithm.** Sweep-line over Y (bottom-to-top). An event queue holds
// segment-start, segment-end, and computed intersection events in lex-
// sorted (y, x, event-kind, segment-id) order. A status structure
// (currently-active segments, sorted left-to-right at the sweep line)
// supports neighbour lookup at each event:
//
//   * Segment START: insert into status; test newly-adjacent pairs for
//     intersection above the current sweep y; enqueue any found.
//   * Segment END:   remove from status; test the pair that newly becomes
//     adjacent for intersection; enqueue if found.
//   * Intersection:  emit; swap the two segments' order in status; test
//     the new adjacent pairs.
//
// **Robustness.** Every orientation, sign, and "is-this-segment-end?"
// decision routes through Shewchuk `orient2d` adaptive precision (Phase
// 3.1.7 v3a / ADR-0076 §18). No naive cross-product, no epsilon. Vertical
// segments + horizontal segments + collinear-overlap pairs flow through
// the adaptive path with exact-sign results.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11).** Event ordering is
// lex-tuple (y, x, kind, seg-id, secondary-seg-id). Status-structure
// ties broken by current-x then segment-id. Output intersections sorted
// by (y, x, seg-a, seg-b) before return. Bit-identical results across
// MSVC / GCC / clang on x64 / ARM64 for any given input.
//
// **Multi-domain consumers** (LOCKED at v6 close):
//   - v6a `is_simple` O(n log n) replacement (v6a ships O(n²) brute force;
//     v6e becomes the dispatch path when n ≥ 64).
//   - v6d brute-force intersection finder replacement for very-large inputs.
//   - Editor / cooker: polyline self-intersection detection (font glyph,
//     vector graphics import sanity).
//   - Future `crd-geometry-mesh-processing` (v7e) edge self-intersection
//     scan for mesh-repair pipelines.
//
// **API form.**
//
//   `bentley_ottmann(segments, alloc)` — returns a sorted list of
//   intersections. Each intersection records the two segment indices that
//   met and the intersection point.
//
//   `bentley_ottmann_any(segments, alloc)` — short-circuit: returns true
//   on the FIRST intersection found (useful for `is_simple` checks).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::polygon
{

// A line segment for the Bentley-Ottmann input. Endpoints in any order;
// the algorithm internally canonicalises by lex (y, x).
template <crd::math::MathScalar T>
struct BOSegment
{
    crd::math::Vec2<T> a;
    crd::math::Vec2<T> b;
};

// An intersection record. `segment_a` and `segment_b` are indices into the
// input segment array; `point` is the intersection coordinate (exact when
// representable; nearest f32/f64 otherwise).
template <crd::math::MathScalar T>
struct BOIntersection
{
    crd::u32           segment_a;
    crd::u32           segment_b;
    crd::math::Vec2<T> point;
};

enum class BOStatus : crd::u8
{
    Ok               = 0,
    NonFiniteInput   = 1,
    DegenerateSegment = 2, // a segment with coincident endpoints
    InternalInvariant = 3,
};

template <crd::math::MathScalar T>
struct BOResult
{
    crd::containers::Array<BOIntersection<T>> intersections;
    BOStatus                                    status = BOStatus::Ok;

    explicit BOResult(crd::memory::IAllocator* alloc) : intersections(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == BOStatus::Ok; }
};

// Find every pairwise intersection of the input segments. Returns a sorted
// list (by lex (y, x, seg-a, seg-b)).
template <crd::math::MathScalar T>
[[nodiscard]] BOResult<T>
bentley_ottmann(crd::containers::ConstSpan<BOSegment<T>> segments,
                crd::memory::IAllocator*                  alloc);

// Short-circuit variant. Returns true on the FIRST intersection. The
// `out_first` parameter (if non-null) is filled with the intersection
// record. Useful for `is_simple` checks where any intersection is
// disqualifying.
template <crd::math::MathScalar T>
[[nodiscard]] bool
bentley_ottmann_any(crd::containers::ConstSpan<BOSegment<T>> segments,
                    crd::memory::IAllocator*                  alloc,
                    BOIntersection<T>*                         out_first = nullptr);

} // namespace crd::geometry::polygon
