#include "bvh_build_internal.hpp"

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_build.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/math/vec.hpp>

#include <algorithm>
#include <limits>

namespace crd::geometry::bvh
{
namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::math::Vec3;
using detail::accumulate_histogram;
using detail::BinHistogram;
using detail::bounds_of_range;
using detail::centroid_bounds_of;
using detail::clear_histogram;
using detail::k_max_bins;
using detail::SplitChoice;
using detail::stable_partition_by_bin;
using detail::sweep_for_split;

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

// A node + the [first, count) slice of the leaf-order index array it owns + the
// recursion depth. The build is an explicit-stack DFS (no native recursion).
struct BuildFrame
{
    u32 node;
    u32 first;
    u32 count;
    u32 depth;
};

} // namespace

AABB3<f32> BvhTree::bounds() const noexcept
{
    if (is_empty())
    {
        return AABB3<f32>(Vec3<f32>(kInf, kInf, kInf), Vec3<f32>(-kInf, -kInf, -kInf));
    }
    return m_nodes[m_root].bounds;
}

BvhTree bvh_build(crd::containers::ConstSpan<AABB3<f32>> prims, crd::memory::IAllocator* alloc,
                  const BvhBuildOptions& opts)
{
    CRD_ASSERT(alloc != nullptr);
    // NaN/Inf contract (ADR-0076 §15): builders reject non-finite *caller* input
    // in debug — a NaN centroid silently corrupts the SAH split / the partition.
    CRD_ASSERT(crd::geometry::primitives::all_finite(prims));
    BvhTree tree(alloc);
    const usize n = prims.size();
    if (n == 0)
    {
        return tree;
    }
    const u32 bins = std::clamp<u32>(opts.sah_bins, 2U, k_max_bins);
    const u32 max_leaf = opts.max_leaf_prims < 1U ? 1U : static_cast<u32>(opts.max_leaf_prims);

    // Working data, all on the tree's allocator.
    crd::containers::Array<Vec3<f32>> centroids(alloc);
    centroids.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        centroids[i] = detail::aabb_centroid(prims[i]);
    }

    crd::containers::Array<u32>& idx = tree.prim_indices_mut();
    idx.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        idx[i] = static_cast<u32>(i);
    }
    crd::containers::Array<u32> partition_scratch(alloc);
    partition_scratch.resize(n);

    crd::containers::Array<BvhNode>& nodes = tree.nodes_mut();
    nodes.reserve(2 * n); // ≤ 2n−1 for a binary tree with capped leaves; no realloc during the build

    const AABB3<f32>* prim_ptr = prims.data();
    const Vec3<f32>* cent_ptr = centroids.data();
    u32* idx_ptr = idx.data();
    u32* scratch_ptr = partition_scratch.data();

    // Root node.
    BvhNode root{};
    root.bounds = bounds_of_range(prim_ptr, idx_ptr, 0, static_cast<u32>(n));
    nodes.push_back(root);
    tree.set_root(0);

    // Explicit DFS stack. Depth ≤ k_max_bvh_depth is the contract (binned SAH on
    // non-pathological input stays well under ~40 even for millions of prims).
    crd::containers::Array<BuildFrame> stack(alloc);
    stack.reserve(2 * k_max_bvh_depth);
    stack.push_back(BuildFrame{0, 0, static_cast<u32>(n), 0});

    while (stack.size() > 0)
    {
        const BuildFrame frame = stack[stack.size() - 1];
        stack.resize(stack.size() - 1);

        if (frame.count <= max_leaf) // leaf
        {
            BvhNode& node = nodes[frame.node];
            node.left_first = frame.first;
            node.prim_count = static_cast<crd::u16>(frame.count);
            node.split_axis = 0;
            continue; // node.bounds was set by whoever pushed this frame
        }

        const AABB3<f32> cbounds = centroid_bounds_of(idx_ptr, frame.first, frame.count, cent_ptr);
        BinHistogram hist;
        clear_histogram(hist, bins);
        accumulate_histogram(hist, idx_ptr, frame.first, frame.count, cent_ptr, prim_ptr, bins, cbounds);
        SplitChoice best;
        sweep_for_split(hist, bins, best);

        u32 mid = (best.axis < 0)
                      ? (frame.count / 2U) // all centroids coincide — deterministic median-by-index fallback
                      : stable_partition_by_bin(idx_ptr, scratch_ptr, frame.first, frame.count, cent_ptr, best.axis,
                                                best.bin, cbounds, bins);
        if (mid == 0 || mid >= frame.count)
        {
            mid = frame.count / 2U; // defensive — should not trigger given a real split
        }

        CRD_ASSERT(frame.depth + 1U < k_max_bvh_depth);
        const u32 left_idx = static_cast<u32>(nodes.size());
        BvhNode left_child{};
        BvhNode right_child{};
        left_child.bounds = bounds_of_range(prim_ptr, idx_ptr, frame.first, mid);
        right_child.bounds = bounds_of_range(prim_ptr, idx_ptr, frame.first + mid, frame.count - mid);
        nodes.push_back(left_child);
        nodes.push_back(right_child);
        CRD_ASSERT(nodes.size() <= 2U * n);

        BvhNode& parent = nodes[frame.node];
        parent.left_first = left_idx;
        parent.prim_count = 0;
        parent.split_axis = static_cast<crd::u8>(best.axis < 0 ? 0 : best.axis);

        // Push right then left so the left child is processed first (matches the
        // near-first traversal order the recorded split_axis enables).
        stack.push_back(BuildFrame{left_idx + 1U, frame.first + mid, frame.count - mid, frame.depth + 1U});
        stack.push_back(BuildFrame{left_idx, frame.first, mid, frame.depth + 1U});
    }

    return tree;
}

f32 bvh_sah_cost(const BvhTree& tree) noexcept
{
    if (tree.is_empty())
    {
        return 0.0F;
    }
    const crd::containers::ConstSpan<BvhNode> nodes = tree.nodes();
    const f32 root_ha = detail::aabb_half_area(nodes[tree.root()].bounds);
    if (root_ha <= 0.0F)
    {
        return 0.0F;
    }
    f32 sum = 0.0F;
    for (const BvhNode& node : nodes)
    {
        if (node.is_leaf())
        {
            sum += static_cast<f32>(node.prim_count) * detail::aabb_half_area(node.bounds);
        }
    }
    return sum / root_ha;
}

} // namespace crd::geometry::bvh
