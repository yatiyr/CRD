#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — convex hull simplification (Phase 3.1.7 v3d;
// ADR-0076 §18 amendment / renewed-scope §11).
//
// Given a `QuickhullResult<T>` produced by v3c, reduce its vertex count to
// a target or until a per-vertex error threshold is reached, while:
//   (1) preserving the **locked-vertex** set (`keep_vertex_indices`), which
//       are never removed — multi-domain pin for CAD remap / FEA attachment
//       point / robotics gripper finger / eylem warm-start contact ID.
//   (2) keeping the result **strictly convex**: every output face's outward
//       plane has all remaining hull vertices strictly below within v3a
//       Stage D adaptive `orient3d` tolerance.
//   (3) keeping the result **inside the input AABB**: trivially satisfied
//       because output vertices are a subset of input vertices — verified
//       by the test corpus.
//
// **Algorithm (greedy vertex removal with shrinkage-cost heuristic)**:
//
//   1. Build per-vertex incident-face index + ring-of-neighbors via two
//      passes over the input face_vertex_indices.
//   2. For each non-locked vertex `v`, compute the **shrinkage cost**:
//          cost(v) = max over { fan triangles that would replace v's star }
//                    of |signed_distance(v, fan_triangle_plane)|
//      Fan triangulation uses the lowest-index ring vertex as the pivot
//      (deterministic per ADR-0076 §4 pin #11). Cost has units of length —
//      directly meaningful for the user's `max_error_threshold`.
//   3. Maintain a min-heap of (cost, vertex_index) pairs. Pop the cheapest.
//   4. Verify removal is admissible:
//        a. Build the would-be fan triangulation of the ring (CCW from
//           outside, via the existing outward face normals — the average of
//           v's incident face normals gives the outward direction of the
//           cap, which determines fan orientation).
//        b. For each new fan face, check that all OTHER live hull vertices
//           have signed distance ≤ k_distance_epsilon (strict-convex check
//           via v3a Stage D `orient3d`). If any check fails, reject this
//           removal (the new fan would extend outside the existing hull) —
//           push v back into the heap with a tombstone marker, continue.
//        c. The ring must be a simple cycle (no repeated vertices) — the
//           topological invariant of Quickhull output guarantees this for
//           non-degenerate triangle hulls.
//   5. On admit: drop v's incident faces, append the new fan faces, update
//      adjacency, mark v as removed. Re-cost the ring vertices (their
//      neighborhoods changed); push updated costs into the heap.
//   6. Stop when:
//        - `live_vertex_count <= opts.target_vertex_count` (if non-zero), OR
//        - `next_cost > opts.max_error_threshold` (if non-zero), OR
//        - the heap is empty (no more admissible removals).
//   7. Compact: emit a fresh `QuickhullResult<T>` whose `vertices` are the
//      surviving vertices in **increasing original-input-index order**
//      (same convention as v3c), with face arrays renumbered accordingly.
//
// **Degenerate inputs** (each is a no-op):
//   - Empty / single-vertex / two-vertex / three-vertex hull → returned
//     unchanged.
//   - `is_coplanar` / `is_colinear` / `is_coincident` flag set on source →
//     returned unchanged (flat hulls already minimal; v3b 2D-hull-on-plane
//     is the right simplification path for those).
//   - `target_vertex_count >= source.vertices.size()` AND `max_error_threshold == 0`
//     → identity copy.
//   - All vertices in `keep_vertex_indices` → identity copy (nothing to remove).
//
// **Determinism (ADR-0076 §4 pin #11, §18; carries over from v3c)**:
//   - Fan triangulation pivot: lowest-index ring vertex.
//   - Cost ties: lowest vertex-index wins.
//   - All sign / convexity decisions via v3a `orient3d` Stage D adaptive
//     predicate (bit-exact across compilers / SIMD widths / OSes).
//   - Output vertex order: increasing original-input-index.
//
// **Builder-reject contract (ADR-0076 §15)**:
//   - `CRD_ASSERT(source.vertices.size() == 0 || all source vertices finite)`
//     inherited from v3c — v3c rejects non-finite input. v3d inherits.
//   - `CRD_ASSERT` on `keep_vertex_indices` bounds (each index < source.vertices.size()).
//
// **Templated on `T ∈ {f32, f64}`**. f32 path goes through f64 adaptive
// `orient3d` for the convexity check.
//
// **Multi-domain consumers** (the `keep_vertex_indices` pin):
//   - **eylem stable contact**: warm-start contact persistence depends on
//     vertex-ID stability across frames. Setting a vertex budget cap (e.g.
//     max 32 vertices for contact-side hulls) AND requiring stable IDs
//     means simplification preserves the identity of the un-collapsed
//     vertices — caller passes `keep_vertex_indices = {}` for full
//     greedy reduction, or pins specific contact-critical vertices.
//   - **CAD remap** (future 3.1.8 `crd-brep`): vertices on B-rep feature
//     edges must survive simplification (boundary preservation across
//     LOD generation).
//   - **FEA attachment-point preservation** (future 3.1.12 `crd-fea`):
//     bolt-hole / weld-point / load-application vertices must survive
//     across mesh decimation; loss would change the FEA constraint set.
//   - **Robotics gripper-finger hull**: fingertip + contact-pad vertices
//     are contact-critical; cannot be removed by aggressive vertex
//     budget reduction.
//
// **API surface**:
//   - `simplify_hull(source, alloc, opts)` → `QuickhullResult<T>`.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/convex/quickhull.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::convex
{
using crd::math::MathScalar;

// ===========================================================================
// Options
// ===========================================================================

template <MathScalar T> struct HullSimplifyOptions
{
    // Target vertex count for the output. The algorithm removes vertices
    // (cheapest first) until live_vertex_count <= target_vertex_count, OR
    // the next-cheapest removal exceeds max_error_threshold (if non-zero),
    // OR no further admissible removal exists.
    //
    // 0 → no target; rely entirely on max_error_threshold to stop. With
    // both 0, the call is a no-op (returns identity copy).
    crd::u32 target_vertex_count = 0;

    // Maximum per-vertex shrinkage cost (length units) admitted before the
    // algorithm stops. The cost is the maximum signed distance from the
    // removed vertex to any new fan-face plane that would replace it — so
    // it has direct geometric meaning ("how far does the hull surface move
    // inward").
    //
    // 0 → no threshold; rely entirely on target_vertex_count to stop. With
    // both 0, the call is a no-op (returns identity copy).
    T max_error_threshold = static_cast<T>(0);

    // Vertices that MUST survive simplification (indices into source.vertices).
    // Empty by default (full greedy reduction allowed). Consumers pin
    // contact-critical / boundary / attachment-point vertices here.
    crd::containers::ConstSpan<crd::u32> keep_vertex_indices = {};
};

// ===========================================================================
// Public API
// ===========================================================================

// Simplify a Quickhull-built convex hull by greedy vertex removal. See
// header comment for algorithm + invariants. Output is a fresh
// `QuickhullResult<T>` whose arrays bind to `alloc`. The source is read-
// only; the caller continues to own it.
//
// Degenerate sources (empty / 1-vertex / 2-vertex / 3-vertex / coplanar /
// colinear / coincident) are returned as identity copies (deep copy of the
// source onto the new allocator).
//
// `alloc` outlives the returned `QuickhullResult`. The result's arrays
// move with it.
template <MathScalar T>
[[nodiscard]] QuickhullResult<T> simplify_hull(const QuickhullResult<T>& source,
                                                crd::memory::IAllocator* alloc,
                                                const HullSimplifyOptions<T>& opts = {}) noexcept;

} // namespace crd::geometry::convex
