#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — raycast(TriangleMeshView, Ray3) over BVH (v4b).
//
// Nearest-hit raycast against an indexed triangle mesh. Hole-free, NaN-safe.
// The on-leaf test is the Woop 2013 watertight ray-triangle predicate
// (`crd-geometry-primitives::intersect_ray_triangle_watertight`) — rays
// passing exactly through a shared edge / vertex hit BOTH adjacent triangles
// (no leak), with the lowest-triangle-index tiebreak picking the winner
// deterministically.
//
// Algorithm:
//   * Precompute Williams/Ize ray-AABB slab + Woop shear once per call.
//   * Walk the per-mesh BVH. At each interior node, run
//     `intersect_ray_aabb_robust` against the node bounds; descend the
//     nearer child first (ordered by entry-t) so `best_t` tightens before
//     the far subtree.
//   * At leaves, run the precomputed-shear Woop test per triangle.
//     Update `best` on improvement; lowest triangle index wins on equal t.
//
// Determinism (ADR-0076 §4 pin #11):
//   * AABB slab uses Williams/Ize precompute — branchless, bit-exact.
//   * Woop edge predicates re-evaluated in `double` on exact zero — same
//     watertight contract for f32 and f64 rays.
//   * Tie-break on equal hit t: lowest triangle index wins.
//
// Two-layer typing (ADR-0078 §5): raw `<f32>` algorithm here; typed
// `Ray3<Length32>` (origin = Vec3<Length32>, direction = Vec3f, max_t =
// Length32) consumers go through `mesh_queries_typed.hpp`.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh/mesh_bvh.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/math/vec.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::mesh
{

// MeshHitPayload — triangle index + Woop barycentric `(1−u−v, u, v)`. Field
// order pinned: `tri`, `bary`. Caller reconstructs the world-space hit point
// as `bary.x * v0 + bary.y * v1 + bary.z * v2` or `ray.origin + t * ray.direction`.
struct MeshHitPayload
{
    crd::u32                       tri{0xFFFFFFFFU};
    crd::math::Vec3<crd::f32>      bary{};
};

// `MeshRayHit::t` is the parametric distance along the ray; `payload`
// carries triangle index + barycentric.
using MeshRayHit = crd::geometry::RayHit<MeshHitPayload>;

// Nearest-hit raycast against the indexed mesh. Returns `nullopt` if the
// ray (within `[0, tmax]`) misses every triangle.
//
// `tmax` defaults to +inf — caller may clamp to reduce work. Backface
// triangles count by default (set `cull_back=true` to ignore them — useful
// for closed surfaces where only the outer hit matters).
[[nodiscard]] std::optional<MeshRayHit>
mesh_raycast(const TriangleMeshViewf&                 view,
             const TriangleMeshBvh&                    bvh,
             const crd::geometry::primitives::Ray3<crd::f32>& ray,
             crd::f32                                  tmax      = std::numeric_limits<crd::f32>::infinity(),
             bool                                      cull_back = false) noexcept;

} // namespace crd::geometry::mesh
