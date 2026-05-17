#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7f manifoldness repair.
//
// Take a triangle mesh that may have either (or both) of the two classical
// non-manifold pathologies, and produce a strictly 2-manifold result. The
// downstream consumers (collision-mesh generation, signed-distance bake,
// finite-element-method tessellation, photogrammetry pipelines, etc.) all
// REQUIRE 2-manifold input; this is the substrate's "make it 2-manifold"
// primitive.
//
// **Pathology A — non-manifold EDGES (>2 incident triangles):**
// An undirected edge `(u, v)` with N > 2 incident triangles cannot be
// represented in a 2-manifold mesh (each interior edge has EXACTLY two
// adjacent faces). Repair: orientation-pair the incident triangles
// (`forward` = triangle's CCW order has `(u, v)` directed `u → v`;
// `backward` = directed `v → u`). The first matched pair keeps the
// original edge endpoints. For each ADDITIONAL pair, duplicate the
// `min(u, v)` endpoint — both triangles in the pair now share the
// duplicated `(u_new, v)` edge instead, which becomes its own manifold
// edge in the output. Leftover unpaired triangles (orientation imbalance
// — odd extras) each get their own duplicated endpoint, becoming
// boundary edges in the output.
//
// **Pathology B — non-manifold VERTICES ("bowties", >1 fan):**
// A vertex whose one-ring is multiple DISCONNECTED triangle fans —
// often visualised as an hourglass / bowtie shape where two surface
// patches meet at a single point. Detected via the v7a CW fan-walk
// returning fewer outgoing HEs than the vertex's slot scan reports
// (= the walk closes early on one fan and never reaches the others).
// Repair: identify the fans via triangle-to-triangle adjacency at the
// vertex (BFS with "two triangles share an edge through v" as the
// adjacency relation); duplicate the vertex once per fan beyond the
// first; rewrite each duplicated fan's triangles to use the duplicate
// vertex.
//
// **Output**: input untouched; a fresh mesh on the requested allocator
// with all detected pathologies repaired. Phase A runs before Phase B
// (Phase B's detection depends on a temp half-edge mesh which itself
// requires manifold-edge input). Both phases are individually
// enable-able via options.
//
// **No information loss for already-manifold input**: if `is_manifold()`
// returns true on the input, neither phase changes anything; the output
// is a faithful clone (same vertex count, same triangle indices) and
// status reports `AlreadyManifold`.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11):** edge grouping uses
// lex-tuple `(min(u, v), max(u, v))` sort; intra-group triangle ordering
// follows slot order; bowtie-fan BFS visits triangles in slot order
// from the lowest-index seed. Output triangle order = input triangle
// order with edits applied in place (no reordering).
//
// **Builder-reject / query-tolerate (ADR-0076 §15):** input must have
// `face_count > 0` (else status `EmptyMesh`). Non-manifold input is
// the PRIMARY use case — no `is_manifold` guard at entry.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

enum class RepairManifoldnessStatus : crd::u8
{
    Ok               = 0, // input had defects; output is repaired
    EmptyMesh        = 1, // input has 0 faces
    AlreadyManifold  = 2, // input was already 2-manifold; output identical
};

struct RepairManifoldnessOptions
{
    // Phase A — detect + fix non-manifold edges (>2 incident faces).
    bool repair_non_manifold_edges = true;

    // Phase B — detect + fix bowtie (multi-fan) vertices.
    bool repair_bowtie_vertices = true;

    // Allocator for the OUTPUT mesh + scratch. If null, the input
    // mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct RepairManifoldnessReport
{
    RepairManifoldnessStatus status                       = RepairManifoldnessStatus::Ok;
    crd::u32                 non_manifold_edges_detected  = 0;
    crd::u32                 non_manifold_edges_repaired  = 0;
    crd::u32                 bowtie_vertices_detected     = 0;
    crd::u32                 bowtie_vertices_repaired     = 0;
    crd::u32                 duplicated_vertices_added    = 0;
    crd::u32                 output_vertices              = 0;
    crd::u32                 output_faces                 = 0;
};

// Entry point. Builds a fresh strictly-2-manifold copy of input; input
// is unmodified.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> repair_manifoldness(const HalfEdgeMesh<T>&               input,
                                     const RepairManifoldnessOptions&     opts,
                                     RepairManifoldnessReport*            out_report = nullptr);

} // namespace crd::geometry::mesh_processing
