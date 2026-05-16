#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — generalised winding number (Phase 3.1.7 v4c).
//
// Jacobson, Kavan, Sorkine-Hornung 2013: "Robust inside-outside segmentation
// using generalized winding numbers." ACM TOG / SIGGRAPH 2013.
//
// The winding number w(p) of a point p with respect to a triangle mesh is
// the sum of solid angles subtended by each triangle at p, scaled by 1/(4π).
// For a closed watertight manifold, w ∈ {0 (outside), 1 (inside)} exactly.
// Jacobson's key insight: for a NON-watertight or self-intersecting mesh,
// the same formula yields a continuous real value that rounds to the
// "topological" inside/outside the user intended — so the inside/outside
// query is robust on imperfect input. This is THE inside/outside test for
// real-world meshes (the kind that come out of glTF / scans / authoring
// tools), where ray-cast parity counts fail on T-junctions and edges-cracks.
//
// Per-triangle solid angle (Van Oosterom & Strackee 1983):
//
//   Let a = v0 - p, b = v1 - p, c = v2 - p, and let |·| be the L2 norm.
//   Numerator   = a · (b × c)                  (signed scalar triple product)
//   Denominator = |a||b||c|
//                 + (a·b)|c|
//                 + (b·c)|a|
//                 + (c·a)|b|
//   Ω = 2 · atan2(numerator, denominator)
//
// The atan2 form is numerically robust — denominator stays positive except
// when p is coincident with a vertex (handled below).
//
// Algorithm (v4c-base, direct O(N)):
//   for each triangle (v0, v1, v2):
//       Ω = van_oosterom_strackee(v0 - p, v1 - p, v2 - p)
//       w += Ω
//   return w / (4π)
//
// **Degenerate inputs**:
//   * p exactly coincident with a vertex → atan2(num, denom) → atan2(0, 0)
//     in the limit. We early-return `0.0F` from that contribution (the
//     other triangles around the vertex sum to the local solid angle).
//   * Zero-area triangle → numerator = 0, denominator > 0 → atan2(0, +) = 0.
//     Contributes 0; harmless.
//   * Empty mesh → returns `0.0F`.
//
// **Determinism (ADR-0076 §4 pin #11 with caveat)**:
//   * Summation order: ascending triangle index. Naive sum (Kahan
//     compensation is a v4c-precision follow-on).
//   * atan2 + sqrt may drift 1-2 ULPs cross-compiler. The threshold
//     check at 0.5 has comfortable margin from the {0, 1} attractors;
//     the inside/outside answer is invariant to this drift in practice.
//
// **Two-layer typing (ADR-0078 §5)**: raw `<MathScalar T>` algorithm here;
// typed `Vec3<Length32>` query consumers go through `mesh_queries_typed.hpp`.
// Return is dimensionless `T` regardless.
//
// **Performance**: v4c-base is O(N). Jacobson 2013 §4 describes a
// hierarchical treecode that uses per-BVH-node dipole moments + an
// adaptive descent criterion for O(log N) average queries — reserved
// for v4c-fast as a follow-on slice once a consumer needs it (eylem
// volumetric inside-checks at hundreds of bodies × many queries / step,
// or the editor "fill" tool).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::mesh
{

// Generalised winding number of `query` w.r.t. `view`. Dimensionless
// (rotations / 4π). Returns `0.0F` for an empty mesh.
[[nodiscard]] crd::f32 mesh_winding_number(const TriangleMeshViewf& view,
                                            const crd::math::Vec3<crd::f32>& query) noexcept;

// Convenience: is `query` "inside" the mesh by the winding-number test?
// Threshold defaults to `0.5` per Jacobson 2013 — closed manifolds give
// w ∈ {0, 1}, non-watertight meshes give a continuous value rounding to
// the topological inside/outside at 0.5.
[[nodiscard]] inline bool mesh_is_inside(const TriangleMeshViewf& view,
                                          const crd::math::Vec3<crd::f32>& query,
                                          crd::f32 threshold = 0.5F) noexcept
{
    return mesh_winding_number(view, query) > threshold;
}

} // namespace crd::geometry::mesh
