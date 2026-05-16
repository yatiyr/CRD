#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — closest_point(TriangleMeshView, point) over BVH (v4a).
//
// Returns the point on the triangle mesh nearest to `query`, plus its squared
// distance and the triangle index it lies on. Branch-and-bound traversal of
// the per-triangle BVH:
//
//   * Per node, compute lower bound = `distance_squared(query, node.bounds)`
//     via `closest_point(AABB3, p)`. If ≥ best, prune.
//   * Descend the nearer child first so `best` tightens before the far
//     subtree is reached. (Children stored adjacent in the BVH; `nearer`
//     determined by per-node `split_axis` + which side `query` falls on.)
//   * At leaves, run Ericson §5.1.5 Voronoi-region cascade
//     (`crd-geometry-primitives::closest_point(Triangle3, p)`) for each
//     triangle and update `best` on improvement.
//
// Determinism (ADR-0076 §4 pin #11):
//   * Tie-break on equal squared distance: lowest triangle index wins.
//   * Traversal order is deterministic given a built tree.
//
// Two-layer typing (ADR-0078 §5 D32-D36): this header ships the raw-`f32`
// algorithm body. Typed `Vec3<Length32>` / `Length32` returns ride one
// layer above via `triangle_mesh_typed.hpp` strip-compute-retag wrappers.
//
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh/mesh_bvh.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/math/vec.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::mesh
{

// `MeshClosestPoint::payload` is the triangle index — the index into
// `view.indices` / 3 for the winning triangle. Field order pinned to
// `ClosestPointResult{point, distance_squared, payload}` per ADR-0076 §16
// pin #2.
using MeshClosestPoint = crd::geometry::ClosestPointResult<crd::u32>;

[[nodiscard]] std::optional<MeshClosestPoint>
mesh_closest_point(const TriangleMeshViewf& view,
                   const TriangleMeshBvh&    bvh,
                   const crd::math::Vec3<crd::f32>& query,
                   crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity()) noexcept;

} // namespace crd::geometry::mesh
