// crd-geometry-mesh — mesh_closest_point impl (v4a).

#include <crd/geometry/mesh/mesh_closest_point.hpp>

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/primitives.hpp>

namespace crd::geometry::mesh
{

using crd::geometry::bvh::BvhNode;
using crd::geometry::bvh::BvhTree;
using crd::geometry::bvh::k_max_bvh_depth;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::closest_point;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3;

namespace
{

// Squared distance from a point to an AABB. The classic clamp-to-bounds
// formula — zero inside, otherwise the squared length of the (per-axis)
// excess outside the box. We re-implement here instead of routing through
// `primitives::closest_point(AABB3, p)` to keep this hot loop branch-light.
inline crd::f32 aabb_dist_sq(const AABB3<crd::f32>& box, const Vec3<crd::f32>& p) noexcept
{
    crd::f32 dsq = 0.0F;
    for (int ax = 0; ax < 3; ++ax)
    {
        const crd::f32 v = p[static_cast<crd::usize>(ax)];
        const crd::f32 lo = box.min[static_cast<crd::usize>(ax)];
        const crd::f32 hi = box.max[static_cast<crd::usize>(ax)];
        if (v < lo) { const crd::f32 d = lo - v; dsq += d * d; }
        else if (v > hi) { const crd::f32 d = v - hi; dsq += d * d; }
    }
    return dsq;
}

inline crd::f32 length_sq(const Vec3<crd::f32>& v) noexcept
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

} // namespace

std::optional<MeshClosestPoint>
mesh_closest_point(const TriangleMeshViewf& view,
                   const TriangleMeshBvh&    bvh,
                   const Vec3<crd::f32>&     query,
                   crd::f32                  max_dist) noexcept
{
    if (view.is_empty() || bvh.is_empty())
    {
        return std::nullopt;
    }

    const auto nodes     = bvh.tree.nodes();
    const auto prim_idx  = bvh.tree.prim_indices();
    const auto vertices  = view.vertices;
    const auto indices   = view.indices;

    // Best so far. `best_dsq` is the squared distance to beat; init at
    // `max_dist^2` so anything farther is pruned at the AABB lower-bound
    // check. `best_tri = UINT32_MAX` doubles as "no hit" sentinel.
    crd::f32 best_dsq = (max_dist == std::numeric_limits<crd::f32>::infinity())
                            ? std::numeric_limits<crd::f32>::infinity()
                            : max_dist * max_dist;
    Vec3<crd::f32> best_point{};
    crd::u32       best_tri = 0xFFFFFFFFU;

    // Stack of (node_index, lower_bound_dsq). Sorted on push: nearer child
    // pushed last → popped first. Manual stack to avoid heap traffic;
    // BVH depth bounded by `k_max_bvh_depth`.
    struct Frame { crd::u32 node; crd::f32 lower_dsq; };
    Frame stack[k_max_bvh_depth];
    crd::usize sp = 0;

    const crd::f32 root_lower = aabb_dist_sq(nodes[bvh.tree.root()].bounds, query);
    if (root_lower >= best_dsq)
    {
        return std::nullopt;
    }
    stack[sp++] = Frame{bvh.tree.root(), root_lower};

    while (sp > 0)
    {
        const Frame f = stack[--sp];
        if (f.lower_dsq >= best_dsq)
        {
            continue; // pruned by an improvement that happened after push
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
                const Vec3<crd::f32> cp = closest_point(tri, query);
                const crd::f32 dsq = length_sq(cp - query);
                if (dsq < best_dsq)
                {
                    best_dsq   = dsq;
                    best_point = cp;
                    best_tri   = ti;
                }
                else if (dsq == best_dsq && ti < best_tri)
                {
                    // Determinism tiebreak — lowest triangle index wins.
                    best_point = cp;
                    best_tri   = ti;
                }
            }
            continue;
        }

        // Interior — push children. Nearer last so it's popped first.
        const crd::u32 left  = node.left_first;
        const crd::u32 right = node.left_first + 1U;
        const crd::f32 ld = aabb_dist_sq(nodes[left].bounds, query);
        const crd::f32 rd = aabb_dist_sq(nodes[right].bounds, query);
        CRD_ASSERT(sp + 2U <= k_max_bvh_depth);
        if (ld <= rd)
        {
            if (rd < best_dsq) { stack[sp++] = Frame{right, rd}; }
            if (ld < best_dsq) { stack[sp++] = Frame{left,  ld}; }
        }
        else
        {
            if (ld < best_dsq) { stack[sp++] = Frame{left,  ld}; }
            if (rd < best_dsq) { stack[sp++] = Frame{right, rd}; }
        }
    }

    if (best_tri == 0xFFFFFFFFU)
    {
        return std::nullopt;
    }
    return MeshClosestPoint{best_point, best_dsq, best_tri};
}

} // namespace crd::geometry::mesh
