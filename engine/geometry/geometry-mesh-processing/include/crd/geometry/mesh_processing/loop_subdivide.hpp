#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7c Loop subdivision (Loop 1987).
//
// Loop's classical triangle subdivision scheme: each face splits 1 → 4 via
// edge-midpoint insertion, then existing vertex positions update with a
// smoothing mask. The limit surface is C² almost everywhere (C¹ at
// extraordinary vertices, the only exception). It is the canonical
// "smooth a coarse triangle mesh" operation for cinematic, procgen,
// scanner-cleanup, and CAD viz pipelines.
//
// **Algorithm (per level):**
//
//   1. Compute UPDATED positions for every existing vertex using the
//      Loop weight mask:
//
//        Interior vertex V with valence n + neighbours u₀..u_{n-1}:
//          β = (1/n) · (5/8 - (3/8 + 1/4·cos(2π/n))²)
//          V' = (1 - n·β)·V + β·Σ u_i
//
//        Boundary vertex V with boundary neighbours u_left, u_right:
//          V' = 3/4·V + 1/8·u_left + 1/8·u_right        (cubic B-spline mask)
//
//   2. For every UNDIRECTED EDGE, insert a new midpoint vertex:
//
//        Interior edge (A,B) shared by faces (A,B,C) and (B,A,D):
//          M = 3/8·A + 3/8·B + 1/8·C + 1/8·D
//
//        Boundary edge (A,B):
//          M = (A + B)/2                                 (cubic B-spline limit)
//
//   3. Replace each face (v₀, v₁, v₂) with FOUR sub-triangles emitted CCW:
//        Corner @ v₀: (v₀, m₀₁, m₂₀)
//        Corner @ v₁: (v₁, m₁₂, m₀₁)
//        Corner @ v₂: (v₂, m₂₀, m₁₂)
//        Central:     (m₀₁, m₁₂, m₂₀)
//      where m_ij is the midpoint vertex created on the edge (v_i, v_j).
//      The 3 midpoints form an inner triangle; the 3 corner sub-triangles
//      sit between each original vertex and the inner triangle's near edge.
//      CCW preserved so surface orientation propagates.
//
// **Multi-level**: apply once per `n_levels`. F_new = 4·F_old per level.
// V_new = V_old + E_old per level. Boundary loop count is invariant
// (boundary edges split 1 → 2; the loop topology is preserved).
//
// **Determinism contract (ADR-0063):** transcendentals through
// `crd::math::deterministic::cos` for bit-identical β across compilers
// and architectures. Vertex emission order = input slot order →
// edge midpoint emission order = canonical-HE-id order → byte-identical
// output indices given byte-identical input.
//
// **Builder-reject / query-tolerate (ADR-0076 §15):** the input mesh is
// required to be 2-manifold; `is_manifold() == false` → status
// `NonManifoldInput` (no output produced). Non-finite vertex positions
// are rejected by the underlying `HalfEdgeMesh::build_from`.
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` algorithm
// body; typed `Vec3<Length32>` consumers ride wrappers in
// `loop_subdivide_typed.hpp` added at slice close on first typed-surface
// consumer.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

enum class LoopSubdivideStatus : crd::u8
{
    Ok               = 0,
    EmptyMesh        = 1, // input has 0 faces
    NonManifoldInput = 2, // input fails is_manifold() at some level
};

struct LoopSubdivideOptions
{
    // Number of refinement passes. 0 = no-op (output is a clone of input).
    crd::u32 n_levels = 1;

    // Allocator for the OUTPUT mesh (and intermediate work arrays). If
    // null, the input mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct LoopSubdivideReport
{
    LoopSubdivideStatus status            = LoopSubdivideStatus::Ok;
    crd::u32            levels_applied    = 0; // useful when input has 0 faces or non-manifold mid-level
    crd::u32            output_vertices   = 0;
    crd::u32            output_faces      = 0;
};

// Entry point. Returns a fresh subdivided mesh; the input is not modified.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> loop_subdivide(const HalfEdgeMesh<T>&        input,
                                const LoopSubdivideOptions&  opts,
                                LoopSubdivideReport*         out_report = nullptr);

} // namespace crd::geometry::mesh_processing
