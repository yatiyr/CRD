// crd-geometry-spatial — kd_build impl (Phase 3.1.7 v5a).
//
// Iterative DFS builder. The "child slots are adjacent in the node array"
// invariant is maintained by pre-allocating BOTH child nodes before pushing
// either subtree's build frame (same trick `bvh_build` uses): when we expand
// an interior node, we
//   1. allocate two empty child node slots (left at L, right at L+1),
//   2. point parent.child_first = L,
//   3. push two build frames carrying those node indices.
// The DFS order is then: pop left frame → fully build left subtree (which
// may grow `nodes`) → pop right frame → build right subtree. The right
// subtree's root sits at L+1 regardless of left subtree depth.
//
// Per node:
//   1. Compute current AABB over the slice's points.
//   2. If `count <= leaf_threshold`, finalize as a leaf.
//   3. Otherwise, pick split_axis = widest extent (X<Y<Z tiebreak on equal
//      extent — keeps tree topology canonical across builds).
//   4. Median-pick via crd::containers::nth_element on a lex-tuple comparator
//      `(coord_value, original_input_index)` — no two elements compare equal,
//      so the partition is byte-identical across MSVC / GCC / clang.
//   5. Record the splitting coord on the interior node + emit two child
//      build frames into pre-allocated child slots.

#include <crd/geometry/spatial/kd_tree.hpp>

#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/geometry/primitives/is_finite.hpp>

#include <limits>

namespace crd::geometry::spatial
{

using crd::f32;
using crd::f64;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::math::Vec3;

namespace
{

struct BuildFrame
{
    u32 node;    // pre-allocated node index this frame fills in
    u32 first;   // start of slice in point_indices
    u32 count;   // number of points in slice
    u32 depth;   // for k_max_kd_depth assert
};

// AABB enclosing every point in the slice [first, first+count) of `idx`.
template <MathScalar T>
AABB3<T> compute_slice_bounds(crd::containers::ConstSpan<Vec3<T>> points,
                               const u32* idx, u32 first, u32 count) noexcept
{
    constexpr T inf = std::numeric_limits<T>::infinity();
    AABB3<T> b{Vec3<T>{inf, inf, inf}, Vec3<T>{-inf, -inf, -inf}};
    for (u32 i = 0; i < count; ++i)
    {
        const Vec3<T>& p = points[idx[first + i]];
        if (p.x < b.min.x) b.min.x = p.x;
        if (p.y < b.min.y) b.min.y = p.y;
        if (p.z < b.min.z) b.min.z = p.z;
        if (p.x > b.max.x) b.max.x = p.x;
        if (p.y > b.max.y) b.max.y = p.y;
        if (p.z > b.max.z) b.max.z = p.z;
    }
    return b;
}

// Widest-extent split axis. Tiebreak X < Y < Z on equal extent.
template <MathScalar T>
u8 pick_split_axis(const AABB3<T>& bounds) noexcept
{
    const T ex = bounds.max.x - bounds.min.x;
    const T ey = bounds.max.y - bounds.min.y;
    const T ez = bounds.max.z - bounds.min.z;
    if (ex >= ey && ex >= ez) return 0;
    if (ey >= ez)              return 1;
    return 2;
}

// Lex-tuple comparator: (coord, original_index). No two elements compare
// equal — `idx` carries unique original-input indices by construction.
template <MathScalar T>
struct LexCompare
{
    const Vec3<T>* points;
    u8 axis;
    [[nodiscard]] bool operator()(u32 lhs, u32 rhs) const noexcept
    {
        const T lv = points[lhs][axis];
        const T rv = points[rhs][axis];
        if (lv < rv) return true;
        if (lv > rv) return false;
        return lhs < rhs;
    }
};

template <MathScalar T>
KdTree<T> build_impl(crd::containers::ConstSpan<Vec3<T>> points,
                     crd::memory::IAllocator* alloc,
                     KdBuildOptions opts)
{
    KdTree<T> tree(alloc);

    constexpr T inf_t = std::numeric_limits<T>::infinity();
    const AABB3<T> empty_bounds{Vec3<T>{inf_t, inf_t, inf_t},
                                  Vec3<T>{-inf_t, -inf_t, -inf_t}};

    if (points.size() == 0U)
    {
        tree.set_root_bounds(empty_bounds);
        return tree;
    }

    // Builder-reject contract (ADR-0076 §15) — caller-supplied data must be
    // finite. Queries tolerate non-finite query points (return no hits).
    for (usize i = 0; i < points.size(); ++i)
    {
        CRD_ASSERT(crd::geometry::primitives::is_finite(points[i]));
    }

    const u32 n = static_cast<u32>(points.size());
    const u32 leaf_threshold = (opts.leaf_threshold == 0U) ? k_kd_leaf_threshold : opts.leaf_threshold;

    auto& point_idx = tree.point_indices_mut();
    point_idx.resize(n);
    for (u32 i = 0; i < n; ++i) { point_idx[i] = i; }

    // Pessimistic node-count reserve: at leaf=L the tree has at most
    // 2*ceil(N/L)-1 nodes. Reserve generously so the inner loop never
    // reallocates (would not break correctness — child indices are by node
    // number not pointer — but kills perf).
    auto& nodes = tree.nodes_mut();
    const u32 reserve_count = (2U * n / leaf_threshold) + 8U;
    nodes.reserve(reserve_count);

    // Allocate root node.
    const u32 root_index = static_cast<u32>(nodes.size());
    nodes.push_back(KdNode<T>{});
    tree.set_root(root_index);

    const AABB3<T> root_bounds = compute_slice_bounds<T>(
        points, point_idx.data(), 0U, n);
    tree.set_root_bounds(root_bounds);

    // DFS stack of pending node-build frames. Bounded by ~2 × max depth.
    BuildFrame stack[k_max_kd_depth * 2U];
    usize sp = 0;
    stack[sp++] = BuildFrame{root_index, 0U, n, 0U};

    while (sp > 0U)
    {
        const BuildFrame f = stack[--sp];
        CRD_ASSERT(f.depth < k_max_kd_depth);

        // Leaf path.
        if (f.count <= leaf_threshold)
        {
            KdNode<T>& leaf = nodes[f.node];
            leaf.split_axis  = 0;
            leaf.split_value = T{0};
            leaf.child_first = f.first;
            leaf.prim_count  = static_cast<crd::u16>(f.count);
            continue;
        }

        // Interior path.
        const AABB3<T> b = compute_slice_bounds<T>(
            points, point_idx.data(), f.first, f.count);
        const u8 axis = pick_split_axis<T>(b);
        const u32 mid_offset = f.count / 2U;
        u32* slice_begin = point_idx.data() + f.first;
        u32* slice_mid   = slice_begin + mid_offset;
        u32* slice_end   = slice_begin + f.count;

        crd::containers::nth_element(
            slice_begin, slice_mid, slice_end,
            LexCompare<T>{points.data(), axis});

        const T pivot_coord = points[*slice_mid][axis];

        // Pre-allocate BOTH children adjacent in the node array, then push
        // their frames. Right pushed first → left popped first → left subtree
        // is fully built before we touch right. Right's root index = left+1
        // regardless of left subtree depth (`bvh_build` uses the same trick).
        const u32 left_node  = static_cast<u32>(nodes.size());
        nodes.push_back(KdNode<T>{});
        const u32 right_node = static_cast<u32>(nodes.size());
        nodes.push_back(KdNode<T>{});

        KdNode<T>& interior = nodes[f.node];
        interior.split_axis  = axis;
        interior.split_value = pivot_coord;
        interior.child_first = left_node;
        interior.prim_count  = 0;

        const u32 left_count  = mid_offset;
        const u32 right_count = f.count - mid_offset;
        const u32 right_first = f.first + mid_offset;

        CRD_ASSERT(sp + 2U <= k_max_kd_depth * 2U);
        stack[sp++] = BuildFrame{right_node, right_first, right_count, f.depth + 1U};
        stack[sp++] = BuildFrame{left_node,  f.first,     left_count,  f.depth + 1U};
    }

    return tree;
}

} // namespace

template <MathScalar T>
KdTree<T> kd_build(crd::containers::ConstSpan<Vec3<T>> points,
                    crd::memory::IAllocator* alloc,
                    KdBuildOptions opts)
{
    return build_impl<T>(points, alloc, opts);
}

template KdTree<f32> kd_build<f32>(
    crd::containers::ConstSpan<Vec3<f32>>, crd::memory::IAllocator*, KdBuildOptions);
template KdTree<f64> kd_build<f64>(
    crd::containers::ConstSpan<Vec3<f64>>, crd::memory::IAllocator*, KdBuildOptions);

} // namespace crd::geometry::spatial
