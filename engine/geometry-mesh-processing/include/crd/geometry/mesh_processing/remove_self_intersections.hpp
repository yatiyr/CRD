#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7g self-intersection removal.
//
// Take a triangle mesh that may have triangle pairs which intersect each
// other in 3D (a "self-intersection"), and produce an output where every
// intersection segment becomes an EDGE of the mesh — the self-intersection
// is replaced by a clean cut that retriangulates the locally-affected
// triangles. The mesh's surface topology stays the same; the tessellation
// resolves the geometric inconsistency.
//
// **Algorithm (3 stages):**
//
//   1. **Broadphase**: build a `TriangleMeshBvh` (v4a) over the input
//      mesh's per-triangle AABBs; walk pairs of triangles whose AABBs
//      overlap. O(n log n + k) where k is the candidate pair count.
//
//   2. **Narrowphase**: for each candidate pair `(T₁, T₂)`, run a
//      robust Möller 1997 triangle-triangle intersection test driven
//      by Shewchuk `orient3d` predicates (exact-sign side determination
//      for whether each triangle straddles the other's plane). When
//      the triangles intersect transversally, the test emits the
//      intersection SEGMENT (two 3D endpoints on the intersection line
//      of the two planes, clipped to the overlap of the two triangle
//      intervals). Coplanar pairs (`orient3d == 0` for all 4 vertices)
//      emit no segment and are deferred to v7f manifoldness repair.
//
//   3. **Per-triangle retriangulation**: every triangle accumulates a
//      list of intersection segments it participates in. For each such
//      triangle T:
//        a. Project T's 3 vertices + all segment endpoints to 2D by
//           dropping the largest-magnitude component of T's plane
//           normal (the "drop axis" gives the most-orthogonal 2D
//           projection, minimising distortion).
//        b. Dedup the 2D points (epsilon-tolerant — two segment
//           endpoints from different pairs may land on the same point
//           when 3 triangles meet at a common edge).
//        c. Build CDT input: deduped points + T's boundary edges +
//           all intersection segments (as constraints into the deduped
//           point array).
//        d. Call `crd::geometry::polygon::constrained_delaunay` → 2D
//           sub-triangulation.
//        e. Lift back to 3D using T's plane equation (interpolate the
//           dropped coordinate).
//        f. Emit sub-triangles to the output mesh.
//
// **Cross-triangle vertex stitching**: every intersection event creates
// segment endpoints ONCE and shares the global vertex indices between
// both incident triangles. This keeps the output 2-manifold along the
// cut — neighbouring sub-triangles in T₁ and T₂ share the same
// intersection vertices.
//
// **Output**: input untouched; a fresh mesh on the requested allocator.
// Triangles that did not intersect anything are emitted unchanged
// (same vertex indices in the output positions array, which prepends
// the original vertices in slot order). Self-intersection vertices
// are appended to the end of the positions array.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11):** broadphase walks
// pairs in `(i, j)` lex order with `i < j`; intersection segment
// endpoint creation is per-pair-deterministic (same pair → same
// endpoints in same order); per-triangle CDT calls receive constraints
// in slot order. Byte-identical output given byte-identical input.
//
// **Builder-reject / query-tolerate (ADR-0076 §15):** input must have
// `face_count > 0` (else status `EmptyMesh`). Self-intersections are
// the primary use case — no `is_manifold` guard. If a per-triangle CDT
// returns a non-Ok status (e.g., `ConstraintsCrossing` — segments
// crossing INSIDE the triangle, which requires Bentley-Ottmann segment
// intersection to resolve), that triangle's original tessellation is
// kept and the report's `triangles_skipped_cdt_failure` counter
// increments. This degrades gracefully on pathological input rather
// than producing garbage.
//
// **Pinned deferrals (v7g-followon, NOT silently dropped):**
//   - Bentley-Ottmann pre-pass to insert segment-segment intersection
//     points as Steiner vertices BEFORE the per-triangle CDT call.
//     Required for the "3+ triangles meet at a non-manifold edge"
//     case where multiple intersection segments meet inside a single
//     triangle. Without it, those CDT calls return `ConstraintsCrossing`
//     and the triangle keeps its original tessellation. Phase doc
//     references this as v6e Bentley-Ottmann's role; the per-triangle
//     dedup + CDT already handles most non-degenerate cases.
//   - Coplanar triangle pair resolution. Möller's test returns "no
//     segment" for coplanar pairs (the intersection is a 2D region
//     not a 1D segment). The v7g spec defers this to v7f
//     manifoldness repair, which handles the resulting non-manifold
//     edge classes.
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` body;
// typed wrappers in `remove_self_intersections_typed.hpp` ship at
// slice close on first typed consumer.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

enum class RemoveSelfIntersectionsStatus : crd::u8
{
    Ok                       = 0, // input had self-intersections; output is cut
    EmptyMesh                = 1, // input has 0 faces
    NoSelfIntersections      = 2, // input was clean; output is a faithful clone
};

template <crd::math::MathScalar T>
struct RemoveSelfIntersectionsOptions
{
    // Epsilon (in input units) for deduping 2D points within a per-triangle
    // CDT input. Two endpoints closer than this are merged into a single
    // CDT vertex — handles the "3 triangles meet at a common edge" case
    // where multiple pair intersections produce coincident endpoints.
    T dedup_epsilon = static_cast<T>(1e-6);

    // Allocator for the OUTPUT mesh + scratch. If null, the input
    // mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct RemoveSelfIntersectionsReport
{
    RemoveSelfIntersectionsStatus status                       = RemoveSelfIntersectionsStatus::Ok;
    crd::u32                       candidate_pairs_tested      = 0;
    crd::u32                       intersection_pairs_detected = 0;
    crd::u32                       intersection_vertices_added = 0;
    crd::u32                       triangles_retriangulated    = 0;
    crd::u32                       triangles_skipped_cdt_failure = 0;
    crd::u32                       output_vertices              = 0;
    crd::u32                       output_faces                 = 0;
};

// Entry point. Builds a fresh self-intersection-free copy of input;
// input is unmodified.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> remove_self_intersections(const HalfEdgeMesh<T>&                       input,
                                            const RemoveSelfIntersectionsOptions<T>&   opts,
                                            RemoveSelfIntersectionsReport*             out_report = nullptr);

} // namespace crd::geometry::mesh_processing
