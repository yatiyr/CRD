#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7d isotropic remeshing
//                                  (Botsch-Kobbelt 2004 §4).
//
// Goal: turn an arbitrary triangle mesh into one of approximately UNIFORM
// triangle size, with all interior valences ≈ 6 and all boundary valences
// ≈ 4 — the canonical "well-conditioned mesh" property that FEA solvers,
// shading-rate sampling, and GPU-friendly vertex-shader pipelines want.
//
// **Algorithm** (Botsch-Kobbelt 2004 §4 verbatim; `L` = target edge length):
//
//   For each of `n_iterations` (typically 5-10):
//
//     1. SPLIT  each edge longer than (4/3)·L at its midpoint.
//     2. COLLAPSE each edge shorter than (4/5)·L, GATED by
//          (a) link condition (manifold preservation, via
//              `HalfEdgeMesh::collapse_edge` internal check), and
//          (b) post-collapse safety: no new edge incident to the merged
//              vertex may exceed (4/3)·L (else we'd just immediately
//              re-split it on the next pass — wasted work).
//     3. FLIP   each interior edge whose flip reduces total valence
//          deviation from the target (6 for interior, 4 for boundary).
//          Deviation = Σ |valence_i - target_i| over the 4 surrounding
//          vertices; the flip moves -1 / -1 / +1 / +1 across those four.
//     4. SMOOTH each vertex toward the area-weighted centroid of its
//          one-ring, then PROJECT the result back onto the ORIGINAL input
//          surface via closest-point query on a per-input-mesh BVH. The
//          projection preserves surface shape while the smoothing
//          eliminates local non-uniformity. Boundary vertices are held
//          fixed by default (Botsch-Kobbelt §4 footnote: boundary
//          smoothing wants the cubic-B-spline mask; future enhancement).
//
// **BVH used for projection is built from the INPUT mesh**, not from the
// currently-being-remeshed mesh. This is the Botsch-Kobbelt
// surface-preservation property: the silhouette + curvature of the input
// is held throughout, only the tessellation pattern changes.
//
// **f32 / f64 both supported.** The BVH is internally `f32` (consistent
// with `crd-geometry-mesh::TriangleMeshBvh`); when called with `f64`
// positions we cast at the BVH query boundary (precision loss bounded by
// `f32` ulp × surface scale — acceptable for tangential surface
// projection where the smoothing offset already dwarfs the cast error).
//
// **Determinism contract (ADR-0063 + ADR-0076 §4 pin #11):** every pass
// snapshots the canonical-HE list in slot order before mutating. New HEs
// created by splits in pass 1 are NOT re-split in the same pass (we
// iterate the snapshot, not the live pool). The smoothing pass is
// Jacobi-style (all new positions computed against OLD; applied
// atomically) — order-independent.
//
// **Builder-reject / query-tolerate (ADR-0076 §15):** input must be
// 2-manifold (`is_manifold == true`); else status `NonManifoldInput`.
// `target_edge_length <= 0` rejected as `InvalidTargetLength`. Empty
// input → `EmptyMesh`.
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` body;
// typed `Vec3<Length32>` consumers ride wrappers in
// `isotropic_remesh_typed.hpp` added at slice close on first typed
// consumer.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

enum class IsotropicRemeshStatus : crd::u8
{
    Ok                  = 0,
    EmptyMesh           = 1, // input has 0 faces
    NonManifoldInput    = 2, // input fails is_manifold()
    InvalidTargetLength = 3, // target_edge_length <= 0
};

template <crd::math::MathScalar T>
struct IsotropicRemeshOptions
{
    // Target edge length (in input units). Edges within
    // [collapse_factor·L, split_factor·L] are considered "well-sized" and
    // not touched. Required (must be > 0).
    T target_edge_length = T{0};

    // Number of remesh iterations (split + collapse + flip + smooth).
    // 5-10 is the standard range; convergence is empirically rapid in
    // the first 3 passes and refines thereafter.
    crd::u32 n_iterations = 5;

    // Edge-length thresholds, expressed as multiples of target_edge_length.
    // The defaults are Botsch-Kobbelt 2004 §4 canonical values; do not
    // change unless you understand the convergence implications.
    T split_factor    = T{4} / T{3}; // edges > factor·L → split
    T collapse_factor = T{4} / T{5}; // edges < factor·L → collapse

    // Tangential smoothing strength. 0 = no movement; 1 = move all the
    // way to the centroid (typical). Larger values overshoot.
    T smoothing_lambda = T{1};

    // If true, boundary vertices are held in place during the smoothing
    // pass. Recommended (Botsch-Kobbelt §4 footnote — smoothing along the
    // boundary curve requires the cubic-B-spline mask, which is future
    // work, see v7c boundary vertex update for the same mask).
    bool keep_boundary_fixed = true;

    // If true, after Laplacian smoothing each vertex is projected back
    // onto the ORIGINAL input surface via closest-point on a BVH. This is
    // the Botsch-Kobbelt surface-preservation step. Disable for purely
    // Laplacian smoothing (mesh will shrink).
    bool project_to_input = true;

    // Allocator for the OUTPUT mesh, the input-BVH, and all intermediate
    // scratch. If null, the input mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct IsotropicRemeshReport
{
    IsotropicRemeshStatus status              = IsotropicRemeshStatus::Ok;
    crd::u32              iterations_run      = 0;
    crd::u32              splits_applied      = 0;
    crd::u32              collapses_applied   = 0;
    crd::u32              flips_applied       = 0;
    crd::u32              vertices_smoothed   = 0;
    crd::u32              output_vertices     = 0;
    crd::u32              output_faces        = 0;
};

// Entry point. Builds a fresh remeshed copy of input; input is unmodified.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> isotropic_remesh(const HalfEdgeMesh<T>&             input,
                                  const IsotropicRemeshOptions<T>&  opts,
                                  IsotropicRemeshReport*            out_report = nullptr);

} // namespace crd::geometry::mesh_processing
