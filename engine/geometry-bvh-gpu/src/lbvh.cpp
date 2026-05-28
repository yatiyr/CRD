// ---------------------------------------------------------------------------
// CPU LBVH (Karras 2012 + KittenGpuLBVH-style fat-node layout). Phase 3.1.7
// v9a-c-followon "Track A elite rewrite" (2026-05-18).
//
// THE ALGORITHM DEFINITION. GPU `LbvhGpuPipeline::dispatch_build_lbvh` is
// the mechanical translation; conformance via byte-identical topology
// + ULP-identical bounds (D162).
//
// Two phases (NO canonical reorder — fat-node layout is the canonical
// output; siblings are NOT consecutive in the array):
//
//   Phase A — Karras §2.2 binary-tree-from-sorted-codes. For each internal
//     node i ∈ [0, N-1): compute range, find split, set
//     nodes[i].parent_idx (in children via parent_idx fixup), left_idx /
//     right_idx (with MSB = isLeaf), fence. NO bounds written yet.
//
//   Phase B — Carry-register upsweep (KittenGpuLBVH §"mergeUpKernel"
//     pattern). For each leaf k in [0, N): start at the leaf carrying the
//     leaf's AABB in a register. Walk UP via parent_idx; at each level
//     write the carried bounds into the parent's `bounds[isRight]` slot.
//     Atomic-coordinated: the second arriver at a parent reads the
//     sibling's slot (already written by the first arriver — SAME cache
//     line as our write), unions with the register-carried bounds, and
//     continues walking with the union as the new register value.
//
// **Why this beats compact-node (32 B) LBVH builds:**
//   - No random tree-walk reads. Each upsweep step touches one cache line
//     (the parent node — 64 bytes = one cache line).
//   - Bounds carried in register. Zero global-memory reads for the running
//     accumulator.
//   - No separate AABB buffer (bounds live in the nodes themselves).
//   - No finalize/reorder pass (the nodes are written in their final
//     format during the build).
//
// Expected pure-CPU build: ~5-10 ms / 1 M on a modern dev box (CPU bound
// by tree-walk dependency chains; serial DFS-equivalent walk).
//
// Degenerate cases:
//   N = 0 → empty tree
//   N = 1 → 1-leaf special case: no internal nodes. The LbvhTree carries
//           an EMPTY `nodes` array; `prim_indices` has one entry; the
//           consumer treats this as a "lone leaf" via `is_empty()` or by
//           checking `internal_count() == 0`. (Karras's algorithm has no
//           "root" for a single-leaf input; there's nothing to build.)
//   N ≥ 2 → N-1 internal nodes + N leaves embedded in their parents.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/lbvh.hpp>

#include <crd/core/assert.hpp>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdint>
#include <limits>

namespace crd::geometry::bvh_gpu
{

namespace
{

using crd::geometry::primitives::AABB3;

// δ(i, j) — count of common high bits between (code, index) augmented keys.
// Returns -1 when j is out of [0, n). For equal Morton codes (after sort
// stability), the augmented key trick (D164) breaks ties via the indices.
template <typename KeyT>
[[nodiscard]] int
delta(const MortonPair<KeyT>* pairs, int n, int i, int j) noexcept
{
    if (j < 0 || j >= n) { return -1; }
    const KeyT ki = pairs[i].code;
    const KeyT kj = pairs[j].code;
    if (ki != kj)
    {
        if constexpr (sizeof(KeyT) == 4U)
        {
            return std::countl_zero(static_cast<crd::u32>(ki ^ kj));
        }
        else
        {
            static_assert(sizeof(KeyT) == 8U);
            return std::countl_zero(static_cast<std::uint64_t>(ki ^ kj));
        }
    }
    constexpr int code_bits = static_cast<int>(sizeof(KeyT)) * CHAR_BIT;
    return code_bits + std::countl_zero(static_cast<crd::u32>(i) ^ static_cast<crd::u32>(j));
}

template <typename KeyT>
[[nodiscard]] std::pair<int, int>
determine_range(const MortonPair<KeyT>* pairs, int n, int i) noexcept
{
    const int d_pos = delta(pairs, n, i, i + 1);
    const int d_neg = delta(pairs, n, i, i - 1);
    const int d     = (d_pos > d_neg) ? 1 : -1;
    const int delta_min = (d == 1) ? d_neg : d_pos;

    int l_max = 2;
    while (delta(pairs, n, i, i + l_max * d) > delta_min) { l_max *= 2; }

    int l = 0;
    for (int t = l_max / 2; t >= 1; t /= 2)
    {
        if (delta(pairs, n, i, i + (l + t) * d) > delta_min) { l += t; }
    }
    const int j = i + l * d;
    return (d == 1) ? std::pair<int, int>{i, j} : std::pair<int, int>{j, i};
}

template <typename KeyT>
[[nodiscard]] int
find_split(const MortonPair<KeyT>* pairs, int n, int first, int last) noexcept
{
    const int delta_node = delta(pairs, n, first, last);
    int s = 0;
    int t = last - first;
    while (true)
    {
        t = (t + 1) / 2;
        if (delta(pairs, n, first, first + (s + t)) > delta_node) { s += t; }
        if (t <= 1) { break; }
    }
    return first + s;
}

[[nodiscard]] AABB3<crd::f32>
aabb_union(const AABB3<crd::f32>& a, const AABB3<crd::f32>& b) noexcept
{
    AABB3<crd::f32> r;
    r.min.x = std::min(a.min.x, b.min.x);
    r.min.y = std::min(a.min.y, b.min.y);
    r.min.z = std::min(a.min.z, b.min.z);
    r.max.x = std::max(a.max.x, b.max.x);
    r.max.y = std::max(a.max.y, b.max.y);
    r.max.z = std::max(a.max.z, b.max.z);
    return r;
}

// Sentinel AABB for unused leaf-slot in a leaf-parent's bounds[].
[[nodiscard]] constexpr AABB3<crd::f32>
sentinel_aabb() noexcept
{
    constexpr crd::f32 inf = std::numeric_limits<crd::f32>::infinity();
    return {{inf, inf, inf}, {-inf, -inf, -inf}};
}

// MSB encoding helpers.
constexpr crd::u32 kLeafBit  = 0x80000000U;
constexpr crd::u32 kRightBit = 0x80000000U;
constexpr crd::u32 kIndexMask = 0x7FFFFFFFU;

template <typename KeyT>
LbvhTree
build_lbvh_cpu_impl(crd::containers::ConstSpan<MortonPair<KeyT>>      sorted_pairs,
                    crd::containers::ConstSpan<AABB3<crd::f32>>        leaf_aabbs,
                    crd::memory::IAllocator*                           alloc) noexcept
{
    LbvhTree tree(alloc);

    const crd::usize n_usize = sorted_pairs.size();
    CRD_ASSERT_MSG(n_usize <= static_cast<crd::usize>(INT_MAX),
                   "build_lbvh_cpu: N exceeds INT_MAX (Karras's signed-index arithmetic)");

    if (n_usize == 0U) { return tree; }

    const int n = static_cast<int>(n_usize);

    auto& nodes        = tree.nodes_mut();
    auto& prim_indices = tree.prim_indices_mut();

    // ---- prim_indices always populated (one entry per leaf, sorted order)
    prim_indices.resize(static_cast<crd::usize>(n));
    for (int k = 0; k < n; ++k)
    {
        prim_indices[static_cast<crd::usize>(k)] = sorted_pairs[static_cast<crd::usize>(k)].index;
    }

    // ---- Singleton case: no internal nodes; consumer queries via prim_indices[0]
    if (n == 1)
    {
        tree.set_root(0U);
        return tree;
    }

    // ---- N-1 internal nodes
    const crd::usize n_int = static_cast<crd::usize>(n - 1);
    nodes.resize(n_int);

    // ---- Phase A: Karras tree-build into fat-node format.
    for (int i = 0; i < n - 1; ++i)
    {
        const auto [first, last] = determine_range<KeyT>(sorted_pairs.data(), n, i);
        const int  split         = find_split<KeyT>(sorted_pairs.data(), n, first, last);
        const bool left_is_leaf  = (split == first);
        const bool right_is_leaf = (split + 1 == last);

        LbvhFatNode& node = nodes[static_cast<crd::usize>(i)];
        node.left_idx  = (left_is_leaf  ? (kLeafBit | static_cast<crd::u32>(split))
                                         : static_cast<crd::u32>(split));
        node.right_idx = (right_is_leaf ? (kLeafBit | static_cast<crd::u32>(split + 1))
                                         : static_cast<crd::u32>(split + 1));
        node.fence     = (i == first) ? static_cast<crd::u32>(last) : static_cast<crd::u32>(first);
        // bounds[] will be filled by the upsweep below.
        node.bounds[0] = sentinel_aabb();
        node.bounds[1] = sentinel_aabb();

        // Fix up parent_idx for child internal nodes.
        if (!left_is_leaf)
        {
            nodes[static_cast<crd::usize>(split)].parent_idx =
                static_cast<crd::u32>(i);                       // MSB=0 → left child
        }
        if (!right_is_leaf)
        {
            nodes[static_cast<crd::usize>(split) + 1U].parent_idx =
                kRightBit | static_cast<crd::u32>(i);            // MSB=1 → right child
        }
    }
    // Root (index 0) has no parent: parent_idx already initialised to
    // 0xFFFFFFFFU in the LbvhFatNode default constructor.
    nodes[0].parent_idx = 0xFFFFFFFFU;

    // ---- Phase B: carry-register upsweep (CPU mimic of KittenGpuLBVH).
    // For each leaf k, walk UP carrying the leaf's bounds in a register.
    // At each level, write into parent.bounds[isRight]; if first arriver,
    // stop. If second arriver (sibling already wrote), read sibling's slot,
    // union, continue.
    //
    // The "did sibling finish?" flag here is the parent's children_done
    // counter (one bit suffices; we use a u8 array for simplicity).
    crd::containers::Array<crd::u8> done(alloc);
    done.resize(n_int, 0U);

    // Helper: get the parent_idx field for a leaf at sorted position `k`.
    // The leaf is referenced as a child of SOME internal node via the
    // MSB-flagged left_idx or right_idx. We need to find which internal
    // refers to leaf k. Easiest: in Phase A, we set parent_idx for INTERNAL
    // children. For LEAVES, we didn't set anything because leaves aren't in
    // nodes[]. So we need to find each leaf's parent here.
    //
    // Quick scan: for each internal i, if its left/right child is a leaf,
    // its child's sorted index is (i.left_idx & kIndexMask) / (i.right_idx
    // & kIndexMask). Build a leaf-parent lookup table.
    crd::containers::Array<crd::u32> leaf_parent(alloc);
    crd::containers::Array<crd::u8>  leaf_is_right(alloc);
    leaf_parent.resize(static_cast<crd::usize>(n), 0U);
    leaf_is_right.resize(static_cast<crd::usize>(n), 0U);
    for (int i = 0; i < n - 1; ++i)
    {
        const LbvhFatNode& node = nodes[static_cast<crd::usize>(i)];
        if ((node.left_idx & kLeafBit) != 0U)
        {
            const crd::u32 leaf_sorted = node.left_idx & kIndexMask;
            leaf_parent[leaf_sorted]   = static_cast<crd::u32>(i);
            leaf_is_right[leaf_sorted] = 0U;
        }
        if ((node.right_idx & kLeafBit) != 0U)
        {
            const crd::u32 leaf_sorted = node.right_idx & kIndexMask;
            leaf_parent[leaf_sorted]   = static_cast<crd::u32>(i);
            leaf_is_right[leaf_sorted] = 1U;
        }
    }

    // Carry-register walk.
    for (int k = 0; k < n; ++k)
    {
        // Initial leaf bounds carried in the "register" variable.
        const crd::u32 prim_idx = sorted_pairs[static_cast<crd::usize>(k)].index;
        AABB3<crd::f32> carried = leaf_aabbs[prim_idx];

        crd::u32 parent  = leaf_parent[static_cast<crd::usize>(k)];
        bool     is_right = leaf_is_right[static_cast<crd::usize>(k)] != 0U;

        while (true)
        {
            // Write carried bounds into parent's slot.
            nodes[parent].bounds[is_right ? 1U : 0U] = carried;

            // "Arrived?" check via done[parent].
            const crd::u8 prior = done[parent]++;
            if (prior == 0U)
            {
                // First arriver — stop this walk.
                break;
            }

            // Second arriver: union with sibling's slot (already written).
            carried = aabb_union(carried, nodes[parent].bounds[is_right ? 0U : 1U]);

            // Walk up.
            if (nodes[parent].is_root()) { break; }
            const bool next_is_right = nodes[parent].i_am_right_child();
            parent   = nodes[parent].parent();
            is_right = next_is_right;
        }
    }

    tree.set_root(0U);
    return tree;
}

} // namespace

template <typename KeyT>
LbvhTree
build_lbvh_cpu(crd::containers::ConstSpan<MortonPair<KeyT>>      sorted_pairs,
               crd::containers::ConstSpan<AABB3<crd::f32>>        leaf_aabbs,
               crd::memory::IAllocator*                           alloc) noexcept
{
    return build_lbvh_cpu_impl<KeyT>(sorted_pairs, leaf_aabbs, alloc);
}

template LbvhTree
build_lbvh_cpu<crd::u32>(crd::containers::ConstSpan<MortonPair<crd::u32>>,
                          crd::containers::ConstSpan<AABB3<crd::f32>>,
                          crd::memory::IAllocator*) noexcept;

template LbvhTree
build_lbvh_cpu<std::uint64_t>(crd::containers::ConstSpan<MortonPair<std::uint64_t>>,
                                crd::containers::ConstSpan<AABB3<crd::f32>>,
                                crd::memory::IAllocator*) noexcept;

} // namespace crd::geometry::bvh_gpu
