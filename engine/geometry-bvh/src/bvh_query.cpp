#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_query.hpp>

#include <limits>

namespace crd::geometry::bvh
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::closest_point;
using crd::geometry::primitives::intersect_ray_aabb_robust;
using crd::geometry::primitives::precompute_ray_aabb;
using crd::geometry::primitives::RayAABBPrecompute;
using crd::math::Vec3;

namespace
{
// Squared distance from `q` to box `b` (0 if inside) — via `closest_point`
// (`primitives.hpp` ships that for AABB3), kept local so this TU stays
// independent of `closest_point.hpp`.
[[nodiscard]] f32 aabb_dist2(const AABB3<f32>& b, const Vec3<f32>& q) noexcept
{
    const Vec3<f32> d = closest_point(b, q) - q;
    return crd::math::dot(d, d);
}
} // namespace

std::optional<BvhRayHit> bvh_raycast(const BvhTree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                     const Ray3<f32>& ray, f32 tmax)
{
    if (tree.is_empty())
    {
        return std::nullopt;
    }
    const crd::containers::ConstSpan<BvhNode> nodes = tree.nodes();
    const crd::containers::ConstSpan<u32> prim_idx = tree.prim_indices();
    const RayAABBPrecompute<f32> pre = precompute_ray_aabb(ray);

    u32 stack[k_max_bvh_depth];
    usize sp = 0;
    stack[sp++] = tree.root();

    f32 best_t = tmax;
    u32 best_prim = 0;
    bool hit = false;

    while (sp > 0)
    {
        const BvhNode& node = nodes[stack[--sp]];
        // Reject the node (and prune if its entry is already past the best hit)
        // — window [0, best_t] handles both: lo > hi ⇒ false.
        f32 t_enter = 0.0F;
        if (!intersect_ray_aabb_robust(ray, pre, node.bounds, 0.0F, best_t, t_enter))
        {
            continue;
        }
        if (node.is_leaf())
        {
            for (u32 i = node.left_first; i < node.left_first + node.prim_count; ++i)
            {
                const u32 p = prim_idx[i];
                f32 t = 0.0F;
                if (intersect_ray_aabb_robust(ray, pre, prims[p], 0.0F, best_t, t) && t < best_t)
                {
                    best_t = t;
                    best_prim = p;
                    hit = true;
                }
            }
        }
        else
        {
            // Visit the child on the side the ray enters first (per split_axis) last
            // so it is popped first — its hit then prunes the far child.
            const u32 left = node.left_first;
            const u32 right = node.left_first + 1U;
            const bool near_is_left =
                pre.sign[node.split_axis] == 0; // dir on this axis ≥ 0 ⇒ enter the lower (left) side first
            const u32 near = near_is_left ? left : right;
            const u32 far = near_is_left ? right : left;
            CRD_ASSERT(sp + 2 <= k_max_bvh_depth);
            stack[sp++] = far;
            stack[sp++] = near;
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return BvhRayHit{best_prim, best_t};
}

void bvh_overlap(const BvhTree& tree, crd::containers::ConstSpan<AABB3<f32>> prims, const AABB3<f32>& box,
                 crd::containers::Array<u32>& out)
{
    bvh_overlap(tree, prims, box, [&out](u32 p) { out.push_back(p); });
}

std::optional<BvhClosestPoint> bvh_closest_point(const BvhTree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                                 const Vec3<f32>& query, f32 max_dist)
{
    if (tree.is_empty())
    {
        return std::nullopt;
    }
    const crd::containers::ConstSpan<BvhNode> nodes = tree.nodes();
    const crd::containers::ConstSpan<u32> prim_idx = tree.prim_indices();

    // `best_d2` is the squared distance to beat. Storing the cutoff squared lets
    // every comparison stay in squared space (no sqrt on the hot path).
    f32 best_d2 =
        (max_dist >= std::numeric_limits<f32>::infinity()) ? std::numeric_limits<f32>::infinity() : max_dist * max_dist;
    u32 best_prim = 0;
    Vec3<f32> best_point{};
    bool hit = false;

    u32 stack[k_max_bvh_depth];
    usize sp = 0;
    stack[sp++] = tree.root();
    while (sp > 0)
    {
        const BvhNode& node = nodes[stack[--sp]];
        // The node AABB distance is a lower bound on every leaf below it; re-check
        // against the current best (which may have tightened since this was pushed).
        if (aabb_dist2(node.bounds, query) >= best_d2)
        {
            continue;
        }
        if (node.is_leaf())
        {
            for (u32 i = node.left_first; i < node.left_first + node.prim_count; ++i)
            {
                const u32 p = prim_idx[i];
                const Vec3<f32> cp = closest_point(prims[p], query);
                const Vec3<f32> d = cp - query;
                const f32 d2 = crd::math::dot(d, d);
                if (d2 < best_d2)
                {
                    best_d2 = d2;
                    best_prim = p;
                    best_point = cp;
                    hit = true;
                }
            }
        }
        else
        {
            const u32 left = node.left_first;
            const u32 right = node.left_first + 1U;
            const f32 dl = aabb_dist2(nodes[left].bounds, query);
            const f32 dr = aabb_dist2(nodes[right].bounds, query);
            // Push the far child first, the near child last (so the near one pops
            // first and tightens `best_d2` before the far subtree is reached);
            // skip a child already known not to beat the best.
            CRD_ASSERT(sp + 2 <= k_max_bvh_depth);
            if (dl <= dr)
            {
                if (dr < best_d2)
                {
                    stack[sp++] = right;
                }
                if (dl < best_d2)
                {
                    stack[sp++] = left;
                }
            }
            else
            {
                if (dl < best_d2)
                {
                    stack[sp++] = left;
                }
                if (dr < best_d2)
                {
                    stack[sp++] = right;
                }
            }
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return BvhClosestPoint{best_prim, best_point, best_d2};
}

} // namespace crd::geometry::bvh
