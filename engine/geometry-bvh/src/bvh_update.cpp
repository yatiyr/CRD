#include "aabb_ops.hpp"

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_update.hpp>

namespace crd::geometry::bvh
{
using crd::f32;
using crd::usize;
using detail::aabb_empty;
using detail::aabb_merge;

void bvh_refit(BvhTree& tree, crd::containers::ConstSpan<AABB3<f32>> prims)
{
    if (tree.is_empty())
    {
        return;
    }
    CRD_ASSERT(prims.size() == tree.prim_count() && "bvh_refit: prim count changed since build — rebuild instead");

    crd::containers::Array<BvhNode>& nodes = tree.nodes_mut();
    const crd::containers::ConstSpan<crd::u32> prim_idx = tree.prim_indices();
    const AABB3<f32>* prim_ptr = prims.data();
    const crd::u32* idx_ptr = prim_idx.data();

    // A node's children always have a higher array index than the node itself
    // (`bvh_build` pushes the parent, then its two children) — so a single pass
    // from the last node to the first processes children before parents.
    for (usize ni = nodes.size(); ni-- > 0;)
    {
        BvhNode& node = nodes[ni];
        if (node.is_leaf())
        {
            AABB3<f32> b = aabb_empty();
            for (crd::u32 i = node.left_first; i < node.left_first + node.prim_count; ++i)
            {
                aabb_merge(b, prim_ptr[idx_ptr[i]]);
            }
            node.bounds = b;
        }
        else
        {
            CRD_ASSERT(node.left_first > ni && node.left_first + 1U < nodes.size());
            AABB3<f32> b = nodes[node.left_first].bounds;
            aabb_merge(b, nodes[node.left_first + 1U].bounds);
            node.bounds = b;
        }
    }
}

} // namespace crd::geometry::bvh
