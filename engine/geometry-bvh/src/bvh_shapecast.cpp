#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh4_simd.hpp>
#include <crd/geometry/bvh/bvh_shapecast.hpp>
#include <crd/geometry/primitives/is_finite.hpp>

#include <limits>

namespace crd::geometry::bvh
{
namespace
{
using crd::f32;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::intersect_ray_aabb_robust;
using crd::geometry::primitives::precompute_ray_aabb;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::RayAABBPrecompute;
using crd::geometry::primitives::Sphere;
using crd::math::Vec3;

// Inflate an AABB outward by a per-axis amount on every face. For sphere-cast
// `pad = (r, r, r)`; for box-cast `pad = half_extents`. Conservative when one
// of the two summands is non-AABB-shaped (sphere); exact when both are AABBs.
[[nodiscard]] AABB3<f32> inflate(const AABB3<f32>& a, const Vec3<f32>& pad) noexcept
{
    return AABB3<f32>(Vec3<f32>(a.min.x - pad.x, a.min.y - pad.y, a.min.z - pad.z),
                      Vec3<f32>(a.max.x + pad.x, a.max.y + pad.y, a.max.z + pad.z));
}

// The shared inner traversal: walk the binary tree near-child-first and ray-
// cast against every node + leaf-prim AABB inflated by `pad`. Same shape as
// `bvh_raycast` in `bvh_query.cpp`, the only diff is the per-test inflation.
[[nodiscard]] std::optional<BvhRayHit> binary_inflated_raycast(const BvhTree& tree,
                                                               crd::containers::ConstSpan<AABB3<f32>> prims,
                                                               const Vec3<f32>& origin, const Vec3<f32>& dir,
                                                               const Vec3<f32>& pad, f32 tmax)
{
    if (tree.is_empty())
    {
        return std::nullopt;
    }
    CRD_ASSERT(crd::geometry::primitives::is_finite(origin));
    CRD_ASSERT(crd::geometry::primitives::is_finite(dir));
    CRD_ASSERT(crd::geometry::primitives::is_finite(pad));

    const crd::containers::ConstSpan<BvhNode> nodes = tree.nodes();
    const crd::containers::ConstSpan<u32> prim_idx = tree.prim_indices();
    const Ray3<f32> ray(origin, dir);
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
        const AABB3<f32> node_inflated = inflate(node.bounds, pad);
        f32 t_enter = 0.0F;
        if (!intersect_ray_aabb_robust(ray, pre, node_inflated, 0.0F, best_t, t_enter))
        {
            continue;
        }
        if (node.is_leaf())
        {
            for (u32 i = node.left_first; i < node.left_first + node.prim_count; ++i)
            {
                const u32 p = prim_idx[i];
                const AABB3<f32> prim_inflated = inflate(prims[p], pad);
                f32 t = 0.0F;
                if (intersect_ray_aabb_robust(ray, pre, prim_inflated, 0.0F, best_t, t) && t < best_t)
                {
                    best_t = t;
                    best_prim = p;
                    hit = true;
                }
            }
        }
        else
        {
            const u32 left = node.left_first;
            const u32 right = node.left_first + 1U;
            const bool near_is_left = pre.sign[node.split_axis] == 0;
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
    return BvhRayHit{best_t, best_prim};
}

[[nodiscard]] std::optional<BvhRayHit> bvh4_inflated_raycast(const Bvh4Tree& tree,
                                                              crd::containers::ConstSpan<AABB3<f32>> prims,
                                                              const Vec3<f32>& origin, const Vec3<f32>& dir,
                                                              const Vec3<f32>& pad, f32 tmax)
{
    if (tree.is_empty())
    {
        return std::nullopt;
    }
    CRD_ASSERT(crd::geometry::primitives::is_finite(origin));
    CRD_ASSERT(crd::geometry::primitives::is_finite(dir));
    CRD_ASSERT(crd::geometry::primitives::is_finite(pad));

    const crd::containers::ConstSpan<Bvh4Node> nodes = tree.nodes();
    const crd::containers::ConstSpan<u32> prim_idx = tree.prim_indices();
    const Ray3<f32> ray(origin, dir);
    const RayAABBPrecompute<f32> pre = precompute_ray_aabb(ray);

    u32 stack[k_max_bvh4_stack];
    usize sp = 0;
    stack[sp++] = tree.root();

    f32 best_t = tmax;
    u32 best_prim = 0;
    bool hit = false;

    using crd::math::simd::Vec4f;
    while (sp > 0)
    {
        const Bvh4Node& node = nodes[stack[--sp]];

        // Transpose ≤4 children's tight bounds into SoA `Vec4f` columns
        // (unused lanes duplicate child 0 — harmless, we only read lanes
        // < child_count). Then one `Vec4f` inflate-and-slab kernel instead
        // of four scalar `inflate` + `intersect_ray_aabb_robust` calls. The
        // pad is broadcast inside the kernel. Bit-identical for finite /
        // well-formed inputs to the scalar form it replaces (v1i-b post-
        // pay): the `min`/`max` chain is associative + commutative on
        // finites, and inflating by ±pad before the slab is the same
        // arithmetic the scalar form did.
        f32 minx[4];
        f32 miny[4];
        f32 minz[4];
        f32 maxx[4];
        f32 maxy[4];
        f32 maxz[4];
        for (u8 c = 0; c < 4U; ++c)
        {
            const Bvh4Child& src = (c < node.child_count) ? node.children[c] : node.children[0];
            minx[c] = src.bounds.min.x;
            miny[c] = src.bounds.min.y;
            minz[c] = src.bounds.min.z;
            maxx[c] = src.bounds.max.x;
            maxy[c] = src.bounds.max.y;
            maxz[c] = src.bounds.max.z;
        }
        const Ray4AabbResult r =
            ray_vs_4_aabb_inflated(ray, pre, Vec4f::load(minx), Vec4f::load(miny), Vec4f::load(minz),
                                   Vec4f::load(maxx), Vec4f::load(maxy), Vec4f::load(maxz), pad.x, pad.y, pad.z, 0.0F,
                                   best_t);
        f32 hit4[4];
        f32 t4[4];
        r.hit_mask.store(hit4);
        r.t_enter.store(t4);

        for (u8 c = 0; c < node.child_count; ++c)
        {
            if (hit4[c] == 0.0F || t4[c] >= best_t)
            {
                continue;
            }
            const Bvh4Child& ch = node.children[c];
            if (ch.is_leaf())
            {
                for (u32 i = ch.first; i < ch.first + ch.count; ++i)
                {
                    const u32 p = prim_idx[i];
                    const AABB3<f32> prim_inflated = inflate(prims[p], pad);
                    f32 t = 0.0F;
                    if (intersect_ray_aabb_robust(ray, pre, prim_inflated, 0.0F, best_t, t) && t < best_t)
                    {
                        best_t = t;
                        best_prim = p;
                        hit = true;
                    }
                }
            }
            else
            {
                CRD_ASSERT(sp + 1 <= k_max_bvh4_stack);
                stack[sp++] = ch.first;
            }
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return BvhRayHit{best_t, best_prim};
}

} // namespace

std::optional<BvhRayHit> bvh_shapecast_sphere(const BvhTree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                              const Sphere<f32>& moving, const Vec3<f32>& dir, f32 tmax)
{
    return binary_inflated_raycast(tree, prims, moving.center, dir,
                                   Vec3<f32>(moving.radius, moving.radius, moving.radius), tmax);
}

std::optional<BvhRayHit> bvh4_shapecast_sphere(const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                                const Sphere<f32>& moving, const Vec3<f32>& dir, f32 tmax)
{
    return bvh4_inflated_raycast(tree, prims, moving.center, dir,
                                  Vec3<f32>(moving.radius, moving.radius, moving.radius), tmax);
}

std::optional<BvhRayHit> bvh_shapecast_box(const BvhTree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                            const AABB3<f32>& moving, const Vec3<f32>& dir, f32 tmax)
{
    const Vec3<f32> half = (moving.max - moving.min) * 0.5F;
    const Vec3<f32> center = (moving.max + moving.min) * 0.5F;
    return binary_inflated_raycast(tree, prims, center, dir, half, tmax);
}

std::optional<BvhRayHit> bvh4_shapecast_box(const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                             const AABB3<f32>& moving, const Vec3<f32>& dir, f32 tmax)
{
    const Vec3<f32> half = (moving.max - moving.min) * 0.5F;
    const Vec3<f32> center = (moving.max + moving.min) * 0.5F;
    return bvh4_inflated_raycast(tree, prims, center, dir, half, tmax);
}

} // namespace crd::geometry::bvh
