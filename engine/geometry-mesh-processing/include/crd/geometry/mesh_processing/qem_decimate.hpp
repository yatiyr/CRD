#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7b Quadric Edge Collapse Decimation
//                                  (Garland-Heckbert 1997 + Garland 1998).
//
// Mesh decimation: produce a lower-poly mesh that approximates the input
// surface as closely as possible. The standard algorithm:
//
//   1. Per-vertex: accumulate fundamental error quadrics from incident
//      face planes (Garland-Heckbert 1997 §3).
//
//   2. Per-edge: combined quadric `Q = Q_a + Q_b`. Closed-form optimal
//      position `v_opt = argmin_v (v^T Q v)` via 3x3 inverse of the
//      gradient-zero system; falls back to midpoint when singular.
//      Collapse cost = `Q(v_opt)`.
//
//   3. Greedy: min-heap of (cost, edge). Pop cheapest, apply
//      `HalfEdgeMesh::collapse_edge` (link-condition check inside).
//      Re-evaluate all edges in the merged vertex's new 1-ring;
//      lazy-invalidation via per-edge generation counter.
//
//   4. **Boundary preservation (Garland 1998 §3.1):** for each boundary
//      edge, add a perpendicular-to-face plane through the edge with a
//      large weight; prevents the silhouette from collapsing inward.
//
//   5. **Locked vertices:** caller can mark vertices that must survive
//      decimation (cooker LOD anchor points, attachment-point UVs,
//      feature corners). Collapses constrain `v_opt` to the locked
//      endpoint's position; both-locked edges are rejected.
//
//   6. **Inversion prevention:** before applying a collapse, simulate
//      replacing the merged vertex with `v_opt` and check that no
//      surviving face flips orientation. Rejects v_opt placements that
//      would create self-intersection.
//
// **Output:** input mesh untouched; returns a new `HalfEdgeMesh<T>` on
// the requested allocator.
//
// **Stop conditions** (at least one required):
//   - `target_face_count`     stop when alive faces ≤ target.
//   - `max_error_threshold`   stop when next cheapest collapse exceeds.
//
// **Determinism contract:** heap order is lex `(cost, canonical_he_id)`
// — byte-identical output across compilers given byte-identical input
// (per ADR-0063 + ADR-0076 §4 pin #11).
//
// **Two-layer typing:** raw `<MathScalar T>` algorithm body. Typed
// (`Vec3<Length32>`) consumers ride wrappers in `qem_decimate_typed.hpp`
// added at slice close on first typed-surface consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::mesh_processing
{

enum class QemDecimateStatus : crd::u8
{
    Ok                   = 0,
    NoStopCondition      = 1, // neither target_face_count nor max_error_threshold provided
    NonManifoldInput     = 2, // input mesh fails is_manifold()
    EmptyMesh            = 3, // input has 0 faces
    TargetUnreachable    = 4, // requested target_face_count not reached (link/inversion/error stopped us early)
};

template <crd::math::MathScalar T>
struct QemDecimateOptions
{
    // At least ONE of these must be set (else status = NoStopCondition).
    crd::u32 target_face_count   = 0;
    T        max_error_threshold = std::numeric_limits<T>::infinity();

    // Vertex indices (into the INPUT mesh's vertex slot space) that must
    // survive decimation. Locked vertices are never merged away; collapses
    // touching exactly one locked endpoint constrain v_opt to the locked
    // position; both-locked edges are rejected.
    crd::containers::ConstSpan<crd::u32> locked_vertices = {};

    // Garland 1998 boundary-preservation weight. Zero disables. Default
    // (1000) holds the silhouette firmly while letting interior simplify.
    T boundary_weight = T{1000};

    // Singular-system threshold for `optimal_position`. Below this, the
    // 3x3 inverse is treated as singular and the midpoint fallback runs.
    T singular_det_epsilon = static_cast<T>(1e-10);

    // Allocator for the OUTPUT mesh (and intermediate work arrays). If
    // null, the input mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct QemDecimateReport
{
    QemDecimateStatus status                  = QemDecimateStatus::Ok;
    crd::u32          collapses_applied       = 0;
    crd::u32          collapses_rejected_link = 0; // HalfEdgeMesh::collapse_edge returned false
    crd::u32          collapses_rejected_flip = 0; // inversion-prevention check rejected
    crd::u32          singular_fallbacks      = 0; // optimal_position singular → midpoint
};

// Entry point. Builds a fresh decimated mesh; the input is not modified.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> qem_decimate(const HalfEdgeMesh<T>&         input,
                              const QemDecimateOptions<T>&  opts,
                              QemDecimateReport*            out_report = nullptr);

} // namespace crd::geometry::mesh_processing
