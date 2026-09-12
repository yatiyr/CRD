#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — query helpers (v1a): nearest-hit raycast + AABB overlap.
//
//   * `bvh_raycast(tree, prims, ray, tmax)` — the nearest primitive whose AABB
//     the ray enters within `[0, tmax]`. Uses the v0f precomputed Williams/Ize
//     robust slab (`precompute_ray_aabb` + `intersect_ray_aabb_robust`):
//     hole-free, NaN/∞-direction-safe, conservative `tmax` widening. Ordered
//     traversal — the child on the side the ray enters first (per the node's
//     recorded `split_axis`) is visited first, so the running `best_t` prunes
//     the far subtree. Returns the *primitive's* AABB hit; per-triangle ray-tri
//     refinement inside a leaf is `crd-geometry-mesh` (v4).
//   * `bvh_overlap(tree, prims, box, on_prim)` — invokes `on_prim(u32 prim)`
//     for every primitive whose AABB overlaps `box`. The `Array<u32>&`
//     convenience appends the hits.
//   * `bvh_closest_point(tree, prims, query, max_dist)` — the primitive AABB
//     closest to `query` (within `max_dist`), via branch-and-bound (v1e).
// ---------------------------------------------------------------------------

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/primitives/primitives.hpp>      // intersects(AABB3, AABB3)
#include <crd/geometry/primitives/robust_ray_aabb.hpp> // precompute_ray_aabb / intersect_ray_aabb_robust
#include <crd/geometry/result_types.hpp>               // RayHit<P> / ClosestPointResult<P> — v1i-a

#include <limits>
#include <optional>

namespace crd::geometry::bvh
{
using crd::geometry::primitives::Ray3;

// `BvhRayHit::payload` is the leaf-prim index (a `u32` into the caller's prims
// span). `RayHit{t, payload}` field order pinned by ADR-0076 §16 pin #2.
using BvhRayHit = crd::geometry::RayHit<crd::u32>;

// Nearest-hit raycast. `nullopt` if the ray (within `[0, tmax]`) misses every
// primitive AABB, or the tree is empty.
[[nodiscard]] std::optional<BvhRayHit> bvh_raycast(const BvhTree& tree,
                                                   crd::containers::ConstSpan<AABB3<crd::f32>> prims,
                                                   const Ray3<crd::f32>& ray,
                                                   crd::f32 tmax = std::numeric_limits<crd::f32>::infinity());

// AABB-overlap query — callback form. `on_prim` is invoked once per overlapping
// primitive, in traversal order (deterministic for a given tree).
template <typename Fn>
inline void bvh_overlap(const BvhTree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims,
                        const AABB3<crd::f32>& box, Fn&& on_prim)
{
    if (tree.is_empty())
    {
        return;
    }
    const crd::containers::ConstSpan<BvhNode> nodes = tree.nodes();
    const crd::containers::ConstSpan<crd::u32> prim_idx = tree.prim_indices();

    crd::u32 stack[k_max_bvh_depth];
    crd::usize sp = 0;
    stack[sp++] = tree.root();
    while (sp > 0)
    {
        const BvhNode& node = nodes[stack[--sp]];
        if (!crd::geometry::primitives::intersects(node.bounds, box))
        {
            continue;
        }
        if (node.is_leaf())
        {
            for (crd::u32 i = node.left_first; i < node.left_first + node.prim_count; ++i)
            {
                const crd::u32 p = prim_idx[i];
                if (crd::geometry::primitives::intersects(prims[p], box))
                {
                    on_prim(p);
                }
            }
        }
        else
        {
            CRD_ASSERT(sp + 2 <= k_max_bvh_depth);
            stack[sp++] = node.left_first;
            stack[sp++] = node.left_first + 1U;
        }
    }
}

// AABB-overlap query — appends every overlapping primitive index to `out`.
void bvh_overlap(const BvhTree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims, const AABB3<crd::f32>& box,
                 crd::containers::Array<crd::u32>& out);

// ---- closest point (v1e) --------------------------------------------------

// `BvhClosestPoint::payload` is the leaf-prim index; `point` is on that prim's
// AABB. Field order pinned by ADR-0076 §16 pin #2: {point, distance_squared, payload}.
using BvhClosestPoint = crd::geometry::ClosestPointResult<crd::u32>;

// The primitive (and the point on its AABB) closest to `query`, considering
// only primitives within `max_dist`. `nullopt` if the tree is empty or nothing
// is within `max_dist`. Branch-and-bound: the per-node AABB distance is a lower
// bound on every leaf below it, so a node whose distance ≥ the current best is
// pruned; the nearer child is descended first so the best tightens before the
// far subtree is reached. (Closest point on the primitive *AABB* — per-triangle
// closest-point inside a leaf is `crd-geometry-mesh`, v4.)
[[nodiscard]] std::optional<BvhClosestPoint>
bvh_closest_point(const BvhTree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims,
                  const crd::math::Vec3<crd::f32>& query,
                  crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity());

} // namespace crd::geometry::bvh
