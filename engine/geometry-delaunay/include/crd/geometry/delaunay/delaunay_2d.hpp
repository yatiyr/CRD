#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8a 2D Bowyer-Watson Delaunay (Bowyer 1981 /
//                                                          Watson 1981).
//
// Pure 2D Delaunay triangulation of a point set. For every input point set
// (with at least 3 non-collinear, finite, distinct points), the algorithm
// produces the unique Delaunay triangulation — the triangulation whose
// triangles' circumcircles contain NO other input point ("empty
// circumcircle" property). The dual of this triangulation is the 2D
// Voronoi diagram (v8d-2d).
//
// **Algorithm** (Bowyer 1981 / Watson 1981 incremental insertion):
//   1. Build a "super-triangle" containing all input points (1000× bbox
//      scale — matches Triangle / CGAL empirically-stable scaling).
//   2. Lex-sort input points by `(x, y, original_index)` for determinism.
//   3. For each point in sorted order:
//      a. Find the triangle containing it via jump-walk from the last
//         inserted triangle's index (apex-side `orient2d` decides which
//         edge to cross when point is outside; converges in O(√n) average).
//      b. Find all "bad" triangles whose circumcircle contains the point
//         via BFS from the containing triangle. Uses Shewchuk adaptive
//         `incircle` (already full Stage D — v8a paydown done per
//         `docs/debt.md`).
//      c. Identify the cavity polygon boundary = edges of bad-triangle
//         cluster that are NOT shared with another bad triangle.
//      d. Delete bad triangles. Re-triangulate cavity by fanning new
//         triangles from the inserted point to each cavity-boundary edge.
//      e. Wire neighbour-triangle pointers between new triangles + the
//         outer ring of unchanged triangles.
//   4. After all points inserted, strip every triangle that references a
//      super-triangle vertex.
//
// **Output triangle convention.** Each output triangle's three vertex
// indices reference the INPUT `points` array (super-triangle vertices
// stripped). Order is CCW (left-of edge by `orient2d > 0`). Triangle
// count = 2N - 2 - boundary_vertex_count (for points in general position).
//
// **Determinism contract (ADR-0063 + ADR-0076 §4 pin #11):**
//   - Lex-sort `(x, y, original_index)` on input.
//   - Jump-walk picks the cross-edge in fixed corner order (HE 0, 1, 2)
//     when multiple are negative-side.
//   - Cavity BFS pops in monotonic triangle-id order (low-id first).
//   - Output triangles emitted in triangle-id order at strip-time.
//   Byte-identical output across compilers / SIMD widths / OSes given
//   byte-identical input.
//
// **Robustness contract (ADR-0076 §15):**
//   - All input point coordinates must be finite (else `NonFiniteInput`).
//   - Input must have ≥ 3 points (else `TooFewPoints`).
//   - Two input points with EXACTLY the same coordinates ⇒ `DuplicatePoint`
//     (caller must dedup upstream — the empty-circumcircle test can't
//     distinguish them).
//   - Collinear inputs: still triangulate (degenerate cases handled by
//     adaptive `incircle` — returns 0 for cocircular and the lex-tiebreak
//     picks the deterministic outcome).
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` body;
// typed `Vec2<Length32>` consumers ride wrappers in `delaunay_2d_typed.hpp`
// added at slice close on first typed consumer.
//
// **Architectural note vs `crd-geometry-polygon` v6c.** v6c's
// `constrained_delaunay(points, constraints)` is a higher-level operation
// that adds constraint-edge recovery (Phase 2 + Phase 3 Lawson restoration)
// on top of the Bowyer-Watson core. v6c shipped before v8a and currently
// re-implements the Bowyer-Watson core locally. An optional v8-close
// follow-on slice can refactor v6c to consume `delaunay_2d` from this
// module, eliminating the duplication. Until then, both implementations
// co-exist by design — they share the same algorithmic basis (Shewchuk
// incircle + lex-sort + jump-walk + cavity BFS) so output is the same.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class DelaunayStatus : crd::u8
{
    Ok               = 0,
    TooFewPoints     = 1, // < 3 input points
    NonFiniteInput   = 2, // a point coordinate is non-finite
    DuplicatePoint   = 3, // two points have identical coordinates
    InternalInvariant = 4, // unexpected algorithmic failure (should never trip)
};

template <crd::math::MathScalar T>
struct DelaunayResult2
{
    // Output triangles — 3 indices per triangle, CCW. Indices reference
    // the INPUT `points` array (super-triangle vertices stripped).
    crd::containers::Array<crd::u32> triangle_indices;
    crd::u32                          triangle_count = 0;
    DelaunayStatus                    status         = DelaunayStatus::Ok;

    explicit DelaunayResult2(crd::memory::IAllocator* alloc) : triangle_indices(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == DelaunayStatus::Ok; }
};

// Entry point. Builds the 2D Delaunay triangulation of `points` on `alloc`.
template <crd::math::MathScalar T>
[[nodiscard]] DelaunayResult2<T>
delaunay_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
            crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
