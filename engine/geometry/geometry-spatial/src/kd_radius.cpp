// crd-geometry-spatial — kd_radius impl (Phase 3.1.7 v5a).
//
// Stack-based DFS with squared-distance prune at each interior node.
// At an interior node split on axis A with split_value S:
//   * `query[A] - radius` is the leftmost coord that could possibly hit the
//     right subtree → only descend right when `query[A] + radius >= S`.
//   * `query[A] + radius` is the rightmost coord that could possibly hit the
//     left subtree  → only descend left  when `query[A] - radius <= S`.
// At a leaf, scan every point and emit hits whose squared-distance ≤ r².

#include <crd/geometry/spatial/kd_radius.hpp>

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
inline T length_sq(const Vec3<T>& v) noexcept
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

template <MathScalar T>
void kd_radius_impl(const KdTree<T>&                       tree,
                    crd::containers::ConstSpan<Vec3<T>>    points,
                    const Vec3<T>&                          query,
                    T                                       radius,
                    crd::containers::Array<KdRadiusHit<T>>& out) noexcept
{
    if (tree.is_empty() || radius < T{0})
    {
        return;
    }
    const T r2 = radius * radius;

    const auto nodes  = tree.nodes();
    const auto pt_idx = tree.point_indices();

    // Stack of node indices to visit. Tree-DFS emission order: deterministic
    // given a fixed tree, but yields *coordinate*-ordered leaves (not payload-
    // ordered). Callers needing a specific order sort `out` post-call.
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
                const T d2 = length_sq<T>(points[pidx] - query);
                if (d2 <= r2)
                {
                    out.push_back(KdRadiusHit<T>{pidx, d2});
                }
            }
            continue;
        }

        // Interior — push the FAR child first so the NEAR child pops next.
        // Since "in-order" emission requires left-to-right node index order,
        // we determine which side is near via the query coordinate vs split.
        const T qa = query[n.split_axis];
        const u32 left  = n.child_first;
        const u32 right = n.child_first + 1U;

        // (qa - radius) > split_value → query window is strictly right of split → left subtree pruned.
        // (qa + radius) < split_value → query window is strictly left  of split → right subtree pruned.
        const bool visit_left  = (qa - radius) <= n.split_value;
        const bool visit_right = (qa + radius) >= n.split_value;

        // Push so RIGHT pops AFTER LEFT (LIFO): push right first.
        CRD_ASSERT(sp + 2U <= k_max_kd_depth * 2U);
        if (visit_right) { stack[sp++] = right; }
        if (visit_left)  { stack[sp++] = left;  }
    }
}

} // namespace

template <MathScalar T>
void kd_radius(const KdTree<T>&                       tree,
                crd::containers::ConstSpan<Vec3<T>>    points,
                const Vec3<T>&                          query,
                T                                       radius,
                crd::containers::Array<KdRadiusHit<T>>& out) noexcept
{
    kd_radius_impl<T>(tree, points, query, radius, out);
}

template void kd_radius<f32>(const KdTree<f32>&,
                              crd::containers::ConstSpan<Vec3<f32>>,
                              const Vec3<f32>&, f32,
                              crd::containers::Array<KdRadiusHit<f32>>&) noexcept;
template void kd_radius<f64>(const KdTree<f64>&,
                              crd::containers::ConstSpan<Vec3<f64>>,
                              const Vec3<f64>&, f64,
                              crd::containers::Array<KdRadiusHit<f64>>&) noexcept;

} // namespace crd::geometry::spatial
