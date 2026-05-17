#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8b 2D Hilbert-sorted Bowyer-Watson Delaunay.
//
// **Alternative insertion strategy** to v8a's lex-sort. Same output (a
// Delaunay triangulation of `points` on `alloc`), same robustness contract,
// same status enum and result struct (re-used from `delaunay_2d.hpp`).
// The ONLY difference: insertion order.
//
// **v8a's lex-sort `(x, y, original_index)`** yields strictly-deterministic
// output but inserts points in scan-line order. The jump-walk for the i-th
// point has to traverse from the hint (the last new triangle) to the new
// point's location — for a wide sweep along x, that's often dozens of
// triangles. Total walk cost ~O(n^1.5).
//
// **v8b's Hilbert-sort** places consecutive points NEAR each other in 2D.
// The Hilbert curve (Hilbert 1891 space-filling curve) maps 2D coordinates
// onto a 1D index such that successive indices map to nearby cells. This is
// the trick Mapbox `delaunator` (Skinner & Agafonkin 2017), Sandia, libigl,
// and CGAL's `spatial_sort` all use. Effect: jump-walks from the hint
// converge in O(1) average steps. Total cost is O(n log n), dominated by
// the Hilbert sort itself; the BW phase is O(n).
//
// **Hilbert mapping** (Skilling 2004 iterative form):
//   - Map each point into a 2^k × 2^k integer grid (k = 16 → ~65 k × 65 k
//     buckets, covers the input bbox with sub-cell resolution).
//   - Compute Hilbert index = `xy2d(2^k, ix, iy)` via the standard bit-
//     interleave + rotation/flip iteration.
//   - Sort `(hilbert_index, original_index)` — original_index tiebreaks
//     coincident-grid-cell points, preserving full determinism.
//
// **Determinism preserved**: same input → same `(hilbert_index, idx)` →
// same insertion order → byte-identical output (modulo super-tri
// stripping order, which is also deterministic — triangle-id order).
//
// **Robustness preserved**: input validation (`TooFewPoints` /
// `NonFiniteInput` / `DuplicatePoint`) is identical. The Bowyer-Watson
// core is shared (`delaunay_2d_internal.hpp`).
//
// **When to use which:**
//   - v8a `delaunay_2d`: small N (< 1000), tests with exact byte-match
//     across implementations, when scan-line order is preferred.
//   - v8b `delaunay_2d_hilbert`: large N (1000+), production builds —
//     consistently 2-4× faster than v8a in micro-benchmarks for
//     `n ≥ 10 000`.
//
// **Output equivalence**: both produce a Delaunay triangulation of the same
// point set. Triangle SET is identical; triangle-INDEX-ORDER in the output
// array may differ because triangle-id allocation depends on insertion
// order. Tests verify "same Delaunay structure" by canonicalising (each
// triangle's vertex triple sorted, then triangle list sorted).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp> // DelaunayResult2 + DelaunayStatus
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

// Entry point. Builds the 2D Delaunay triangulation of `points` on `alloc`
// using Hilbert-curve insertion order (faster jump-walks for large N).
template <crd::math::MathScalar T>
[[nodiscard]] DelaunayResult2<T>
delaunay_2d_hilbert(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                    crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
