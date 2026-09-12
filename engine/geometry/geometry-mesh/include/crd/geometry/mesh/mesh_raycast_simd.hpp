#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — per-leaf SIMD Möller-Trumbore raycast (v4d).
//
// Alternative entry point to v4b's `mesh_raycast`. Same BVH traversal +
// AABB slab pruning; differs only at the leaf inner loop, where it processes
// 8 triangles at a time via AVX2 SoA-batched Möller-Trumbore (Möller &
// Trumbore 1997).
//
// **When to use**:
//   * Hot raycast workloads against meshes with average-leaf-size ≥ 4
//     (typical Wald-2007 SAH builds with `kSahBins=16` produce 4-8 tris/leaf).
//   * Cases where every ULP of v4b's Woop watertight contract is not
//     load-bearing — picking, broadphase culling, navmesh height queries.
//
// **When NOT to use**:
//   * Watertight contract IS load-bearing (CSG, robust mesh booleans,
//     winding-via-rays). v4b's Woop test is the correct path.
//   * Leaves with <4 triangles dominate — gather overhead negates SIMD
//     win. v4b is faster on small-leaf BVHs.
//
// **Algorithm (per leaf)**:
//   1. Read up to 8 triangle indices from the leaf's prim_idx range.
//      Pad-replicate the LAST valid triangle to fill to 8 (the padded
//      lanes report a hit on the same triangle; lowest-tri-index
//      tiebreak picks the real one).
//   2. Gather v0/v1/v2 vertex positions via `view.indices` →
//      `view.vertices`. Transpose into SoA: 9 × `Vec8f` registers
//      (v0x/y/z, v1x/y/z, v2x/y/z).
//   3. SIMD Möller-Trumbore: edge1/edge2, pvec=cross(dir, edge2),
//      det=dot(edge1, pvec). Lanes with `|det| < ε` (and `det < ε` if
//      `cull_back`) get masked out.
//   4. inv_det, tvec, u, qvec, v, t — same MT pipeline.
//   5. Reduce: scan 8 lanes for the lowest `t` strictly less than
//      `best_t`, with lowest-tri-index tiebreak on equal-t.
//
// **Determinism vs v4b**:
//   * MT uses a strict-sign det test, not Woop's watertight exact-edge
//     promotion. Rays passing EXACTLY through a shared edge may hit
//     neither adjacent triangle (vs Woop hitting both). The
//     lowest-tri-index tiebreak still picks deterministically when
//     either lane reports a hit.
//   * Within a single ULP-of-precision, v4b and v4d agree to within
//     1-2 ULPs on `t`. The tests cross-validate per a tolerance.
//
// **Two-layer typing (ADR-0078 §5)**: raw `<f32>` algorithm here; typed
// `Ray3T<Length32>` consumers reuse the same `mesh_queries_typed.hpp`
// wrapper via a `Backend::Simd` selector (TBD in `mesh_queries_typed.hpp`).
//
// **Platform**: SIMD path is AVX2 (`Vec8f`). Non-AVX2 builds fall back
// to two `Vec4f`-batched halves under the hood (the same `Vec8f`
// abstraction); on win-debug-scalar (no SIMD at all) the test still
// runs correctly via the scalar fallback in the Vec8f shim.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh/mesh_bvh.hpp>
#include <crd/geometry/mesh/mesh_raycast.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::mesh
{

// Same result shape as v4b — interchangeable downstream.
[[nodiscard]] std::optional<MeshRayHit>
mesh_raycast_simd(const TriangleMeshViewf&                 view,
                  const TriangleMeshBvh&                    bvh,
                  const crd::geometry::primitives::Ray3<crd::f32>& ray,
                  crd::f32                                  tmax      = std::numeric_limits<crd::f32>::infinity(),
                  bool                                      cull_back = false) noexcept;

} // namespace crd::geometry::mesh
