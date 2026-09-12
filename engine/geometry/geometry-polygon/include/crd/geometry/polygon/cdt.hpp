#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6c Constrained Delaunay Triangulation (CDT).
//
// Two-pass CDT (Anglada 1997):
//   1) Bowyer-Watson incremental Delaunay over the input point set.
//      Adverse decisions use Shewchuk `incircle` adaptive predicate; every
//      orientation test uses Shewchuk `orient2d` adaptive. Adverse +
//      degenerate input is handled by promoting to the exact path, not by
//      epsilon nudges.
//   2) Constraint-edge recovery via the Anglada 1997 edge-flip strategy:
//      for each input constraint edge (a, b) not already present, locate
//      the strip of triangles crossed by (a, b), flip the crossing edges
//      one at a time until (a, b) is realised, then flag it as
//      "constrained" so the rest of the algorithm never flips it.
//
// **Determinism contract (ADR-0063 + ADR-0076 §4 pin #11).** Input points
// are sorted lex-by-(x, y, original-index) before insertion; Bowyer-Watson
// cavity expansion BFS visits triangles in monotonic ID order; edge-flip
// resolution picks the lex-smallest crossing-edge triangle index when
// multiple are candidates. Result: bit-identical triangulation across
// MSVC / GCC / clang on x64 / ARM64 for any given input.
//
// **Two API forms.**
//
//   `constrained_delaunay(points, constraints, ...)`
//     Generic PSLG input. Returns ALL triangles in the convex hull of the
//     points; caller filters by domain if needed.
//
//   `constrained_delaunay(PolygonView2, ...)`
//     Convenience for v6 callers (fonts / UI / navmesh / lightmap-UV).
//     Adds each ring's edges as constraints; final result KEEPS only
//     triangles INSIDE the polygon (outer-CCW + holes-CW per v6 winding
//     convention). Even-odd ring-classification via centroid point-in-
//     polygon test.
//
// **Builder reject / query tolerate.** Non-finite input ⇒ status =
// NonFiniteInput (no assert — query-tolerate, since the boundary between
// "input gathered from cooker" and "input from user data" is fuzzy at
// this layer). Self-intersecting constraint edges ⇒ status =
// ConstraintsCrossing (well-defined diagnostic, not crash). Empty point
// set or fewer than 3 points ⇒ status = TooFewPoints.
//
// **Output triangle convention.** Each triangle's three vertex indices
// reference the INPUT `points` array (NOT the internal super-triangle
// vertices, which are stripped before return). Order is CCW.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::polygon
{

// Constraint edge — two indices into the input point array. The edge MUST
// appear in the final triangulation. Endpoints are unordered (the algorithm
// canonicalises to lex-smallest first).
struct CdtEdge
{
    crd::u32 a;
    crd::u32 b;
};

enum class CdtStatus : crd::u8
{
    Ok                  = 0,
    TooFewPoints        = 1, // < 3 input points
    NonFiniteInput      = 2, // a point or constraint endpoint is non-finite
    DuplicatePoint      = 3, // two input points have identical coordinates (boundary
                             // case — Bowyer-Watson's incircle test can't distinguish
                             // them; caller dedups upstream)
    ConstraintOutOfBounds = 4, // a constraint edge index is >= points.size()
    ConstraintsCrossing = 5,  // two constraint edges cross interior-to-interior
    InternalInvariant   = 6,  // unexpected algorithmic failure (should never trip)
};

struct CdtOptions
{
    // If `true`, triangles that lie INSIDE a hole (per even-odd ring-fill)
    // are removed from the output. Only meaningful for the polygon overload.
    bool keep_only_inside_polygon = true;
    // Reserved — Chew's quality refinement (refinement Steiner points to
    // hit a min-angle bound) ships as v6c-quality follow-on.
    bool reserved_refine_quality = false;
};

template <crd::math::MathScalar T>
struct CdtResult
{
    // Triangle list — 3 indices per triangle (CCW). Indices reference the
    // INPUT point array (super-triangle vertices are stripped).
    crd::containers::Array<crd::u32> triangle_indices;
    crd::u32                          triangle_count = 0;
    CdtStatus                         status         = CdtStatus::Ok;

    explicit CdtResult(crd::memory::IAllocator* alloc) : triangle_indices(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == CdtStatus::Ok; }
};

// Generic PSLG entry — points + constraints.
template <crd::math::MathScalar T>
[[nodiscard]] CdtResult<T>
constrained_delaunay(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                     crd::containers::ConstSpan<CdtEdge>             constraints,
                     crd::memory::IAllocator*                        alloc,
                     CdtOptions                                       opts = {});

// Polygon convenience entry — adds every ring's edges as constraints,
// optionally keeps only triangles inside the outer-minus-holes domain.
template <crd::math::MathScalar T>
[[nodiscard]] CdtResult<T>
constrained_delaunay(PolygonView2<T> polygon, crd::memory::IAllocator* alloc,
                     CdtOptions opts = {});

} // namespace crd::geometry::polygon
