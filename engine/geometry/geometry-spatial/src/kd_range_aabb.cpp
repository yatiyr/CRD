// crd-geometry-spatial — kd_range_aabb impl (Phase 3.1.7 v5a).
//
// Standard interval-prune DFS:
//   * If `box.min[axis] >  split_value`, left subtree is wholly outside.
//   * If `box.max[axis] <  split_value`, right subtree is wholly outside.
//   (Inclusive bounds — equality is "in window".)
// At a leaf, test every point against the box (componentwise inclusive).

#include <crd/geometry/spatial/kd_range_aabb.hpp>

#include <crd/core/assert.hpp>

namespace crd::geometry::spatial
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec3;

namespace
{

template <MathScalar T>
inline bool inside_aabb(const Vec3<T>& p, const AABB3<T>& b) noexcept
{
    return (p.x >= b.min.x) && (p.x <= b.max.x)
        && (p.y >= b.min.y) && (p.y <= b.max.y)
        && (p.z >= b.min.z) && (p.z <= b.max.z);
}

template <MathScalar T>
void kd_range_aabb_impl(const KdTree<T>&                  tree,
                        crd::containers::ConstSpan<Vec3<T>> points,
                        const AABB3<T>&                    box,
                        crd::containers::Array<u32>&       out) noexcept
{
    if (tree.is_empty())
    {
        return;
    }
    // Empty / inverted query box (min > max on any axis): no hits.
    if (box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z)
    {
        return;
    }

    const auto nodes  = tree.nodes();
    const auto pt_idx = tree.point_indices();

    u32 stack[k_max_kd_depth * 2U];
    usize sp = 0;
    stack[sp++] = tree.root();

    while (sp > 0U)
    {
        const u32 node_idx = stack[--sp];
        const KdNode<T>& n = nodes[node_idx];

        if (n.is_leaf())
        {
            for (u32 i = 0; i < n.prim_count; ++i)
            {
                const u32 pidx = pt_idx[n.child_first + i];
                if (inside_aabb<T>(points[pidx], box))
                {
                    out.push_back(pidx);
                }
            }
            continue;
        }

        const T lo = box.min[n.split_axis];
        const T hi = box.max[n.split_axis];
        const u32 left  = n.child_first;
        const u32 right = n.child_first + 1U;

        const bool visit_left  = (lo <= n.split_value);
        const bool visit_right = (hi >= n.split_value);

        CRD_ASSERT(sp + 2U <= k_max_kd_depth * 2U);
        if (visit_right) { stack[sp++] = right; }
        if (visit_left)  { stack[sp++] = left;  }
    }
}

} // namespace

template <MathScalar T>
void kd_range_aabb(const KdTree<T>&                  tree,
                    crd::containers::ConstSpan<Vec3<T>> points,
                    const AABB3<T>&                    box,
                    crd::containers::Array<u32>&       out) noexcept
{
    kd_range_aabb_impl<T>(tree, points, box, out);
}

template void kd_range_aabb<f32>(const KdTree<f32>&,
                                   crd::containers::ConstSpan<Vec3<f32>>,
                                   const AABB3<f32>&,
                                   crd::containers::Array<u32>&) noexcept;
template void kd_range_aabb<f64>(const KdTree<f64>&,
                                   crd::containers::ConstSpan<Vec3<f64>>,
                                   const AABB3<f64>&,
                                   crd::containers::Array<u32>&) noexcept;

} // namespace crd::geometry::spatial
