#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — shape-cast (sweep) queries (Phase 3.1.7 v1i-b).
//
// "Sweep a moving shape along `dir` from t=0 to t=tmax; report the nearest
// primitive AABB the moving shape first touches." Two moving shapes ship in
// v1i-b — a `Sphere` and an `AABB3`. Both reduce to **ray-vs-inflated-AABB**:
//
//   * sphere-cast: inflate each node + leaf-prim AABB by `sphere.radius` on
//     every axis (conservative — true Minkowski sum of a sphere and an AABB
//     has rounded corners + edges; this approximation produces square corners
//     so the worst case is a "hit" reported at a corner that the rounded
//     Minkowski form would have skirted; the broadphase precondition every
//     shipped physics engine accepts).
//   * box-cast: inflate each node + leaf-prim AABB by the moving box's
//     half-extents — Minkowski sum of two AABBs is an AABB, so this case is
//     **exact**, not conservative.
//
// The traversal is the same ordered DFS as `bvh_raycast` (near-child-first per
// `split_axis`, with the running `best_t` pruning the far subtree); the inner
// slab test is the v0f precomputed Williams/Ize robust form
// (`intersect_ray_aabb_robust`) on the inflated bounds. Result type is
// `BvhRayHit` — same `{t, payload}` alias the raycast surface ships.
//
// `dir` need not be unit (matches `Ray3` convention — `t` is the parameter
// along `dir`). Already-overlapping moving shape at t=0 returns t=0 (the slab
// test naturally gives `tmin = 0` when origin is inside the inflated box).
//
// The general convex shapecast (sweep an arbitrary `ConvexHullView`) is the
// GJK-cast in `-convex` v2 — `bvh_shapecast_convex` is its broadphase wrapper.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh4.hpp>
#include <crd/geometry/bvh/bvh_query.hpp> // BvhRayHit
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/primitives/primitives.hpp> // AABB3, Sphere, Ray3
#include <crd/math/vec.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::bvh
{
// ---- sphere-cast vs static BVH --------------------------------------------

// Sweep `moving` from its current `center` along `dir` for `t ∈ [0, tmax]`;
// return the nearest prim whose AABB the *swept sphere* first enters. Each
// node/leaf AABB is inflated by `moving.radius` before the slab test — the
// conservative Minkowski reduction described in the file header.
[[nodiscard]] std::optional<BvhRayHit>
bvh_shapecast_sphere(const BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                     const primitives::Sphere<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
                     crd::f32 tmax = std::numeric_limits<crd::f32>::infinity());

// Same, over a `Bvh4Tree`. Scalar four-sequential inflate-and-slab per node —
// the `Vec4f` ray-vs-4-AABB SIMD kernel reuse for shapecast is a follow-up
// (mirrors the v1g pattern; v4g per-leaf-SIMD is also a follow-up).
[[nodiscard]] std::optional<BvhRayHit>
bvh4_shapecast_sphere(const Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                      const primitives::Sphere<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
                      crd::f32 tmax = std::numeric_limits<crd::f32>::infinity());

// ---- box-cast vs static BVH -----------------------------------------------

// Sweep `moving` (an `AABB3` — center + half-extents implicit in its min/max)
// along `dir` for `t ∈ [0, tmax]`. Each node/leaf AABB is inflated by the
// moving box's half-extents — **exact** Minkowski reduction (sum of two AABBs
// is an AABB). The trajectory's origin is the moving box's center.
[[nodiscard]] std::optional<BvhRayHit>
bvh_shapecast_box(const BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                  const primitives::AABB3<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
                  crd::f32 tmax = std::numeric_limits<crd::f32>::infinity());

[[nodiscard]] std::optional<BvhRayHit>
bvh4_shapecast_box(const Bvh4Tree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                   const primitives::AABB3<crd::f32>& moving, const crd::math::Vec3<crd::f32>& dir,
                   crd::f32 tmax = std::numeric_limits<crd::f32>::infinity());

} // namespace crd::geometry::bvh
