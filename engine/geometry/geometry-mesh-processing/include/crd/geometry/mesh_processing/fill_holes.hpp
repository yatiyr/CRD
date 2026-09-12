#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7e Liepa 2003 hole filling.
//
// Detect every boundary loop of the input mesh; for each loop, compute the
// triangulation that lex-minimises the Liepa weight
//
//     ω(triangulation) = Σ_{T ∈ triangulation} ( area(T) + λ · dihedral(T) )
//
// where `dihedral(T)` is the sum over T's three edges of the dihedral
// penalty `1 - cos(θ)` between T and the OTHER triangle on that edge — one
// of:
//   * Another patch triangle (interior patch edge — known via DP backpointer
//     to the phantom-edge triangle of the relevant sub-DP), or
//   * The existing input-mesh face on the opposite side of a hole boundary
//     edge (boundary-edge case).
//
// The optimal triangulation is solved by Barequet-Sharir / Liepa dynamic
// programming over the loop's `(i, k)` sub-intervals, O(N³) time + O(N²)
// space (memoised). The phantom-edge dihedral of each W[i, k] entry is
// DEFERRED — added when the parent merge fixes which triangle sits on the
// (b_i, b_k) chord. For the top-level call W[0, N-1] the chord is itself
// a boundary edge (the closing edge of the loop), so its phantom dihedral
// resolves against the input-mesh outside face.
//
// Triangle vertex order: each patch triangle (b_i, b_m, b_k) is emitted in
// CCW order WHEN VIEWED FROM OUTSIDE THE SURROUNDING MESH — i.e., the same
// orientation as the input-mesh faces. The boundary loop walk via
// `HalfEdgeMesh` boundary-HE `.next` returns the loop in the direction
// OPPOSITE to the surrounding faces' CCW (per v7a invariants); the DP
// triangle order `(b_i, b_m, b_k)` with `i < m < k` gives the correct
// outward-facing patch normal.
//
// **Output**: input untouched; returns a fresh mesh with all small-enough
// holes filled. Loops of size > `max_hole_size` are skipped (reported via
// `holes_skipped_too_large` counter) — protects against pathological
// boundary loops with thousands of vertices that would OOM the O(N²) DP
// table.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11):** boundary loop
// detection walks the HE pool in slot order; loops emitted in
// first-touched order. DP min-comparator uses lex-tuple `(composite
// weight, split-index)` so ties resolve deterministically across compilers.
//
// **Builder-reject / query-tolerate (ADR-0076 §15):** input must be
// 2-manifold (else status `NonManifoldInput`); empty mesh → `EmptyMesh`;
// mesh with no boundary loops → status `NoHolesToFill`. `max_hole_size`
// must be ≥ 3.
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` body;
// typed wrappers in `fill_holes_typed.hpp` ship at slice close on first
// typed consumer.
//
// **Scope (v7e + v7e-refine):** full Liepa 2003 §3 DP triangulation + §4
// Steiner-point refinement (density-match the surrounding mesh) + §5
// Laplacian fairing of Steiner-point positions (boundary loop clamped).
// All three phases enabled by default; disable individually via options.
//
// **Refinement (§4):** per loop vertex, precompute σ_v = average length of
// incident edges in the INPUT mesh. Iterate:
//   1. For each patch triangle T = (v_a, v_b, v_c): compute centroid c and
//      σ_avg = (σ_a + σ_b + σ_c) / 3. T is "too coarse" iff for some
//      v ∈ T, α · |v - c| > σ_v AND α · |v - c| > σ_avg, where α = √2.
//   2. Split too-coarse triangles at their centroids — each split inserts
//      a new Steiner vertex v_m with σ_m = σ_avg and replaces T with three
//      sub-triangles (T_ab_m, T_bc_m, T_ca_m).
//   3. Run a local Delaunay-flip pass on patch interior edges. Edge (a,b)
//      with apex vertices c, d flips iff ∠acb + ∠adb > π (interior angles
//      via `crd::math::deterministic::acos`).
//   4. Repeat until no splits OR `max_refine_iterations` reached.
//
// **Fairing (§5):** Laplacian smoothing of Steiner vertices only (loop
// vertices are CLAMPED). Jacobi update: new position = mean of one-ring
// neighbours. Repeat `fairing_iterations` times.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

enum class FillHolesStatus : crd::u8
{
    Ok               = 0,
    EmptyMesh        = 1, // input has 0 faces
    NonManifoldInput = 2, // input fails is_manifold()
    NoHolesToFill    = 3, // input is already closed
};

template <crd::math::MathScalar T>
struct FillHolesOptions
{
    // Largest boundary-loop length the DP will attempt. Loops with more
    // vertices are skipped (counted in `holes_skipped_too_large`).
    // Default 256 ≈ 8 MB DP table — comfortable on every realistic
    // hole. Increase only if you're filling truly enormous loops.
    crd::u32 max_hole_size = 256;

    // Weight of the dihedral-angle penalty relative to area in the
    // Liepa lex-min objective (Phase §3 DP). Larger values prefer flatter
    // patches (matching local mesh planarity) at the cost of larger
    // triangles. Smaller values prefer small-area triangles regardless of
    // normal consistency. Default 1.0 = balanced.
    T dihedral_lambda = T{1};

    // Liepa §4 Steiner-point refinement controls.
    bool refine = true;                          // run §4 at all
    // α scale ratio for the too-coarse test (`α · |v-centroid| > σ`).
    // Liepa 2003 uses √2 ≈ 1.4142136; smaller values trigger more
    // aggressive subdivision, larger values trigger less.
    T        refine_alpha           = static_cast<T>(1.41421356); // √2 (Liepa default)
    crd::u32 max_refine_iterations  = 10;        // hard cap on §4 loop

    // Liepa §5 Laplacian fairing controls. Fairing is applied to STEINER
    // vertices only; loop boundary vertices stay clamped. With `refine =
    // false`, no Steiner points exist → fairing is a no-op.
    crd::u32 fairing_iterations = 5;

    // Allocator for the OUTPUT mesh + scratch. If null, the input
    // mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct FillHolesReport
{
    FillHolesStatus status                   = FillHolesStatus::Ok;
    crd::u32        holes_detected           = 0;
    crd::u32        holes_filled             = 0;
    crd::u32        holes_skipped_too_large  = 0;
    crd::u32        triangles_added          = 0;
    crd::u32        steiner_points_added     = 0; // §4 refinement output
    crd::u32        refine_iterations_run    = 0; // §4 iterations actually executed
    crd::u32        delaunay_flips_applied   = 0; // §4 flip pass total
    crd::u32        fairing_iterations_run   = 0; // §5 iterations actually executed
    crd::u32        output_vertices          = 0;
    crd::u32        output_faces             = 0;
};

// Entry point. Builds a fresh mesh with all holes filled; input unchanged.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> fill_holes(const HalfEdgeMesh<T>&        input,
                            const FillHolesOptions<T>&   opts,
                            FillHolesReport*             out_report = nullptr);

} // namespace crd::geometry::mesh_processing
