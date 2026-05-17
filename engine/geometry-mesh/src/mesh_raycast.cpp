// crd-geometry-mesh — mesh_raycast impl (v4b).

#include <crd/geometry/mesh/mesh_raycast.hpp>

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/geometry/primitives/watertight_ray_tri.hpp>

namespace crd::geometry::mesh
{

using crd::geometry::bvh::BvhNode;
using crd::geometry::bvh::k_max_bvh_depth;
using crd::geometry::primitives::intersect_ray_aabb_robust;
using crd::geometry::primitives::intersect_ray_triangle_watertight;
using crd::geometry::primitives::precompute_ray_aabb;
using crd::geometry::primitives::precompute_ray_tri;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::RayAABBPrecompute;
using crd::geometry::primitives::RayTriShear;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3;

std::optional<MeshRayHit>
mesh_raycast(const TriangleMeshViewf& view,
             const TriangleMeshBvh&    bvh,
             const Ray3<crd::f32>&     ray,
             crd::f32                  tmax,
             bool                      cull_back) noexcept
{
    if (view.is_empty() || bvh.is_empty())
    {
        return std::nullopt;
    }

    const auto nodes    = bvh.tree.nodes();
    const auto prim_idx = bvh.tree.prim_indices();
    const auto vertices = view.vertices;
    const auto indices  = view.indices;

    // Per-ray precomputes — amortise across the traversal.
    const RayAABBPrecompute<crd::f32> aabb_pc = precompute_ray_aabb(ray);
    const RayTriShear<crd::f32>       tri_pc  = precompute_ray_tri(ray);

    crd::f32       best_t   = tmax;
    crd::u32       best_tri = 0xFFFFFFFFU;
    Vec3<crd::f32> best_bary{};

    // Manual stack of (node_index, lower_bound_t). The lower_bound is the
    // entry-t the slab test recorded — if it later exceeds best_t, prune.
    struct Frame { crd::u32 node; crd::f32 enter_t; };
    Frame stack[k_max_bvh_depth];
    crd::usize sp = 0;

    {
        crd::f32 root_t = 0.0F;
        if (!intersect_ray_aabb_robust(ray, aabb_pc, nodes[bvh.tree.root()].bounds,
                                       0.0F, best_t, root_t))
        {
            return std::nullopt;
        }
        stack[sp++] = Frame{bvh.tree.root(), root_t};
    }

    while (sp > 0)
    {
        const Frame f = stack[--sp];
        if (f.enter_t >= best_t)
        {
            continue; // best_t tightened after this frame was pushed
        }
        const BvhNode& node = nodes[f.node];

        if (node.is_leaf())
        {
            for (crd::u32 i = node.left_first; i < node.left_first + node.prim_count; ++i)
            {
                const crd::u32 ti = prim_idx[i];
                const crd::u32 i0 = indices[ti * 3U + 0U];
                const crd::u32 i1 = indices[ti * 3U + 1U];
                const crd::u32 i2 = indices[ti * 3U + 2U];
                const Triangle3<crd::f32> tri{vertices[i0], vertices[i1], vertices[i2]};

                crd::f32       t{};
                Vec3<crd::f32> bary{};
                if (!intersect_ray_triangle_watertight(ray, tri_pc, tri, t, bary,
                                                        cull_back, /*tnear=*/0.0F))
                {
                    continue;
                }
                if (t < best_t)
                {
                    best_t    = t;
                    best_tri  = ti;
                    best_bary = bary;
                }
                else if (t == best_t && ti < best_tri)
                {
                    // Determinism tiebreak — lowest triangle index wins.
                    best_tri  = ti;
                    best_bary = bary;
                }
            }
            continue;
        }

        // Interior — slab test both children, descend nearer-first.
        const crd::u32 left  = node.left_first;
        const crd::u32 right = node.left_first + 1U;
        crd::f32 ltl = 0.0F;
        crd::f32 ltr = 0.0F;
        const bool lhit = intersect_ray_aabb_robust(ray, aabb_pc, nodes[left].bounds,
                                                     0.0F, best_t, ltl);
        const bool rhit = intersect_ray_aabb_robust(ray, aabb_pc, nodes[right].bounds,
                                                     0.0F, best_t, ltr);
        CRD_ASSERT(sp + 2U <= k_max_bvh_depth);
        if (lhit && rhit)
        {
            // Push far first → near popped first.
            if (ltl <= ltr)
            {
                stack[sp++] = Frame{right, ltr};
                stack[sp++] = Frame{left,  ltl};
            }
            else
            {
                stack[sp++] = Frame{left,  ltl};
                stack[sp++] = Frame{right, ltr};
            }
        }
        else if (lhit)
        {
            stack[sp++] = Frame{left, ltl};
        }
        else if (rhit)
        {
            stack[sp++] = Frame{right, ltr};
        }
    }

    if (best_tri == 0xFFFFFFFFU)
    {
        return std::nullopt;
    }
    MeshRayHit out{};
    out.t       = best_t;
    out.payload = MeshHitPayload{best_tri, best_bary};
    return out;
}

} // namespace crd::geometry::mesh
