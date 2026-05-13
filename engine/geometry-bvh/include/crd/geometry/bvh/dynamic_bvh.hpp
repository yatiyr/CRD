#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — `DynamicBvh`, the incrementally-updatable AABB tree (v1c).
//
// The static `BvhTree` (a packed node array from a one-shot binned-SAH build)
// can't support arbitrary insert/erase without a rebuild — so the dynamic case
// gets its own structure: a binary tree of fat AABBs with parent pointers, a
// free list of recycled slots, height-balanced tree rotations on every
// insert/remove, and a fat-AABB margin so a primitive that moves a little
// doesn't touch the tree at all. This is the classic dynamic AABB tree from
// Catto's GDC 2019 *Dynamic Bounding Volume Hierarchies* / Box2D v3's
// `b2DynamicTree` — the form `crd-eylem`'s broadphase wraps (eylem v1c).
//
// Leaves carry a `u32 user_data` (the consumer's primitive id) and are
// addressed by a stable `DynamicBvhNodeId` handle (stable across insert/remove
// of *other* nodes; the slot is recycled only after `remove`). Queries report
// leaves whose *fat* AABB overlaps / is hit — the caller does the precise test
// against its own primitive data (this is broadphase, not narrowphase).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>      // AABB3, Ray3, intersects(AABB3,AABB3)
#include <crd/geometry/primitives/robust_ray_aabb.hpp> // precompute_ray_aabb / intersect_ray_aabb_robust
#include <crd/geometry/result_types.hpp>               // ClosestPointResult<P> — v1i-a

#include <limits>
#include <optional>

namespace crd::geometry::bvh
{
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;

// Stable handle for a leaf in a `DynamicBvh`.
struct DynamicBvhNodeId
{
    static constexpr crd::u32 k_invalid = 0xFFFFFFFFU;
    crd::u32 value{k_invalid};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != k_invalid; }
    [[nodiscard]] friend constexpr bool operator==(DynamicBvhNodeId a, DynamicBvhNodeId b) noexcept
    {
        return a.value == b.value;
    }
};

struct DynamicBvhConfig
{
    // Every inserted tight AABB is inflated by this absolute amount on each axis
    // when stored. A subsequent `update` whose new tight AABB still fits inside
    // the stored fat AABB is a no-op — the margin is the slack that lets small
    // motion go un-restructured. Box2D's `b2_aabbMargin` idea.
    crd::f32 fat_margin{0.1F};
};

// Maximum traversal-stack depth for queries (the tree is height-balanced, so
// real depths are ~2·log₂(n); 256 covers any practical scene). Asserted.
inline constexpr crd::usize k_max_dynamic_bvh_stack = 256;

// A self-overlap pair: two leaves' user_data with `a < b` (sorted for
// determinism — `find_overlapping_pairs` emits each unordered pair exactly
// once in the lower-then-higher form). Eylem v1c broadphase wraps this.
struct DynamicBvhPair
{
    crd::u32 a;
    crd::u32 b;

    [[nodiscard]] friend constexpr bool operator==(DynamicBvhPair x, DynamicBvhPair y) noexcept
    {
        return x.a == y.a && x.b == y.b;
    }
    [[nodiscard]] friend constexpr bool operator<(DynamicBvhPair x, DynamicBvhPair y) noexcept
    {
        return x.a != y.a ? x.a < y.a : x.b < y.b;
    }
};

// A work-pair on the cross-walk stack of `find_overlapping_pairs`: two
// internal node indices to test for overlap. Surfaced here so the
// caller-owned scratch struct below can hold a typed `Array` of them.
struct DynamicBvhPairWork
{
    crd::u32 a;
    crd::u32 b;
};

// Caller-owned scratch buffers for `find_overlapping_pairs`. The
// alloc-per-call overload makes one allocation per call from the tree's
// own allocator (TLSF O(1), amortised fine for non-hot-path callers).
// Broadphase consumers — eylem v1c will call this every physics tick at
// 60-1000 Hz — should construct one `DynamicBvhPairScratch` once and pass
// it in, avoiding the per-tick alloc/free churn. Capacity is retained
// across `clear()` so subsequent calls allocate only on growth.
struct DynamicBvhPairScratch
{
    crd::containers::Array<crd::u32> walk;
    crd::containers::Array<DynamicBvhPairWork> cross;

    explicit DynamicBvhPairScratch(crd::memory::IAllocator* alloc) noexcept : walk(alloc), cross(alloc) {}

    DynamicBvhPairScratch(const DynamicBvhPairScratch&) = delete;
    DynamicBvhPairScratch& operator=(const DynamicBvhPairScratch&) = delete;
    DynamicBvhPairScratch(DynamicBvhPairScratch&&) noexcept = default;
    DynamicBvhPairScratch& operator=(DynamicBvhPairScratch&&) noexcept = default;
    ~DynamicBvhPairScratch() = default;

    // Reset for the next call. Storage is retained — only `size()` resets
    // — so capacity grows monotonically up to the high-water mark.
    void clear() noexcept
    {
        walk.clear();
        cross.clear();
    }
};

class DynamicBvh
{
public:
    explicit DynamicBvh(crd::memory::IAllocator* alloc, const DynamicBvhConfig& cfg = {}) noexcept
        : m_nodes(alloc), m_cfg(cfg)
    {
    }

    DynamicBvh(const DynamicBvh&) = delete;
    DynamicBvh& operator=(const DynamicBvh&) = delete;
    DynamicBvh(DynamicBvh&&) noexcept = default;
    DynamicBvh& operator=(DynamicBvh&&) noexcept = default;
    ~DynamicBvh() = default;

    // ---- mutation -----------------------------------------------------------

    // Insert a leaf for `tight_aabb` carrying `user_data`. The stored fat AABB
    // is `tight_aabb` inflated by `cfg.fat_margin`. Returns a stable handle.
    [[nodiscard]] DynamicBvhNodeId insert(const AABB3<crd::f32>& tight_aabb, crd::u32 user_data);

    // Remove a leaf. The handle is invalid afterwards (its slot may be reused).
    void remove(DynamicBvhNodeId id);

    // Re-fit a leaf to `new_tight_aabb`. If the stored fat AABB already encloses
    // `new_tight_aabb`, this is a no-op and returns false. Otherwise the leaf is
    // removed and re-inserted with a fresh fat AABB (the handle stays valid) and
    // it returns true (the tree was restructured).
    bool update(DynamicBvhNodeId id, const AABB3<crd::f32>& new_tight_aabb);

    // ---- access -------------------------------------------------------------

    [[nodiscard]] bool is_empty() const noexcept { return m_root == k_null; }
    [[nodiscard]] crd::usize leaf_count() const noexcept { return m_leaf_count; }
    [[nodiscard]] crd::u32 user_data(DynamicBvhNodeId id) const noexcept
    {
        CRD_ASSERT(is_allocated_leaf(id.value));
        return m_nodes[id.value].user_data;
    }
    [[nodiscard]] AABB3<crd::f32> fat_aabb(DynamicBvhNodeId id) const noexcept
    {
        CRD_ASSERT(is_allocated_leaf(id.value));
        return m_nodes[id.value].aabb;
    }
    // Root fat AABB (encloses every leaf), or identity-empty (min = +∞, max = −∞).
    [[nodiscard]] AABB3<crd::f32> bounds() const noexcept;

    // ---- queries (visit leaves by fat AABB — broadphase) --------------------

    // Invoke `on_leaf(u32 user_data)` for every leaf whose fat AABB overlaps `box`.
    template <typename Fn> void query(const AABB3<crd::f32>& box, Fn&& on_leaf) const
    {
        if (m_root == k_null)
        {
            return;
        }
        crd::u32 stack[k_max_dynamic_bvh_stack];
        crd::usize sp = 0;
        stack[sp++] = m_root;
        while (sp > 0)
        {
            const Node& n = m_nodes[stack[--sp]];
            if (!crd::geometry::primitives::intersects(n.aabb, box))
            {
                continue;
            }
            if (n.is_leaf())
            {
                on_leaf(n.user_data);
            }
            else
            {
                CRD_ASSERT(sp + 2 <= k_max_dynamic_bvh_stack);
                stack[sp++] = n.child1;
                stack[sp++] = n.child2;
            }
        }
    }

    // Append every overlapping leaf's `user_data` to `out`.
    void query(const AABB3<crd::f32>& box, crd::containers::Array<crd::u32>& out) const;

    // Closest leaf (by *fat* AABB — broadphase) within `max_dist`. Reports the
    // leaf's `user_data` in the payload, the point on its fat AABB, and the
    // squared distance. Branch-and-bound: per-node AABB squared distance is a
    // lower bound on every leaf below it; the nearer child is descended first
    // so `best_d2` tightens before the far subtree is reached. Squared
    // throughout (no `sqrt` on the hot path); `max_dist²` stored as the cutoff.
    // For a *narrowphase* per-primitive closest-point, the caller refines
    // against its own tight prim AABBs / mesh / SDF afterwards (this is the
    // broadphase: the same shape as `query` and `raycast`).
    [[nodiscard]] std::optional<crd::geometry::ClosestPointResult<crd::u32>>
    closest_point(const crd::math::Vec3<crd::f32>& query,
                  crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity()) const;

    // ---- broadphase self-overlap (v1i-c) -----------------------------------

    // Invoke `on_pair(u32 ud_a, u32 ud_b)` for every pair of leaves whose fat
    // AABBs overlap, with `ud_a < ud_b` (each unordered pair emitted exactly
    // once in sorted order — deterministic). Dual-descent algorithm:
    // `find_pairs(node)` recurses into each subtree for its internal pairs,
    // then crosses `(child1, child2)` to find pairs spanning the two subtrees;
    // `cross(a, b)` skips when `a.aabb` and `b.aabb` don't overlap, otherwise
    // recurses into the larger side until both are leaves. Total work
    // `O(n + |pairs|)` for typical trees (vs. `O(n²)` brute force).
    //
    // **Scratch buffers.** The scratch-taking overload `find_overlapping_pairs
    // (Fn&&, DynamicBvhPairScratch&)` reuses caller-owned work stacks
    // (`scratch.walk` + `scratch.cross`) across calls — eylem v1c's broadphase
    // calls this every physics tick (~60-1000 Hz), and per-tick alloc churn
    // on the broadphase hot path is real overhead; caller-owned scratch
    // amortises capacity growth across calls. The `scratch.clear()` happens
    // inside the call. The convenience overload below allocates one
    // `DynamicBvhPairScratch` on the tree's allocator per call for non-hot-
    // path callers.
    template <typename Fn> void find_overlapping_pairs(Fn&& on_pair, DynamicBvhPairScratch& scratch) const
    {
        scratch.clear();
        if (m_root == k_null || m_nodes[m_root].is_leaf())
        {
            return;
        }
        scratch.walk.push_back(m_root);

        while (scratch.walk.size() > 0)
        {
            const crd::u32 ni = scratch.walk[scratch.walk.size() - 1];
            scratch.walk.resize(scratch.walk.size() - 1);
            const Node& n = m_nodes[ni];
            if (n.is_leaf())
            {
                continue;
            }

            // (1) Find pairs spanning child1 ⊗ child2.
            scratch.cross.push_back(DynamicBvhPairWork{n.child1, n.child2});
            while (scratch.cross.size() > 0)
            {
                const DynamicBvhPairWork cw = scratch.cross[scratch.cross.size() - 1];
                scratch.cross.resize(scratch.cross.size() - 1);
                const Node& a = m_nodes[cw.a];
                const Node& b = m_nodes[cw.b];
                if (!crd::geometry::primitives::intersects(a.aabb, b.aabb))
                {
                    continue;
                }
                const bool a_leaf = a.is_leaf();
                const bool b_leaf = b.is_leaf();
                if (a_leaf && b_leaf)
                {
                    const crd::u32 lo = a.user_data < b.user_data ? a.user_data : b.user_data;
                    const crd::u32 hi = a.user_data < b.user_data ? b.user_data : a.user_data;
                    on_pair(lo, hi);
                }
                else if (a_leaf)
                {
                    scratch.cross.push_back(DynamicBvhPairWork{cw.a, b.child1});
                    scratch.cross.push_back(DynamicBvhPairWork{cw.a, b.child2});
                }
                else if (b_leaf)
                {
                    scratch.cross.push_back(DynamicBvhPairWork{a.child1, cw.b});
                    scratch.cross.push_back(DynamicBvhPairWork{a.child2, cw.b});
                }
                else
                {
                    // Two interior nodes — descend into all 4 child-pair combos.
                    scratch.cross.push_back(DynamicBvhPairWork{a.child1, b.child1});
                    scratch.cross.push_back(DynamicBvhPairWork{a.child1, b.child2});
                    scratch.cross.push_back(DynamicBvhPairWork{a.child2, b.child1});
                    scratch.cross.push_back(DynamicBvhPairWork{a.child2, b.child2});
                }
            }

            // (2) Recurse into each child for its own internal pairs.
            scratch.walk.push_back(n.child1);
            scratch.walk.push_back(n.child2);
        }
    }

    // Convenience: allocates a `DynamicBvhPairScratch` per call from the
    // tree's own allocator. Use the scratch-taking overload above on the
    // broadphase hot path.
    template <typename Fn> void find_overlapping_pairs(Fn&& on_pair) const
    {
        DynamicBvhPairScratch scratch(m_nodes.allocator());
        find_overlapping_pairs(static_cast<Fn&&>(on_pair), scratch);
    }

    // Append every overlapping fat-AABB leaf pair to `out` in `(min, max)`
    // user_data order. The scratch-taking overload reuses caller-owned work
    // stacks; the alloc-per-call overload constructs a scratch on the tree's
    // allocator each call.
    void find_overlapping_pairs(crd::containers::Array<DynamicBvhPair>& out,
                                DynamicBvhPairScratch& scratch) const;
    void find_overlapping_pairs(crd::containers::Array<DynamicBvhPair>& out) const;

    // Invoke `on_leaf(u32 user_data)` for every leaf whose fat AABB the ray
    // (within [0, ∞)) enters. (No nearest-hit ordering — broadphase; the caller
    // refines. The static `bvh_raycast` does nearest-hit over tight boxes.)
    template <typename Fn> void raycast(const Ray3<crd::f32>& ray, Fn&& on_leaf) const
    {
        if (m_root == k_null)
        {
            return;
        }
        const crd::geometry::primitives::RayAABBPrecompute<crd::f32> pre =
            crd::geometry::primitives::precompute_ray_aabb(ray);
        crd::u32 stack[k_max_dynamic_bvh_stack];
        crd::usize sp = 0;
        stack[sp++] = m_root;
        while (sp > 0)
        {
            const Node& n = m_nodes[stack[--sp]];
            crd::f32 t = 0.0F;
            if (!crd::geometry::primitives::intersect_ray_aabb_robust(ray, pre, n.aabb, 0.0F,
                                                                      std::numeric_limits<crd::f32>::infinity(), t))
            {
                continue;
            }
            if (n.is_leaf())
            {
                on_leaf(n.user_data);
            }
            else
            {
                CRD_ASSERT(sp + 2 <= k_max_dynamic_bvh_stack);
                stack[sp++] = n.child1;
                stack[sp++] = n.child2;
            }
        }
    }

    // ---- diagnostics --------------------------------------------------------

    // Σ over interior nodes of halfArea(node.aabb), divided by halfArea(root.aabb)
    // — Catto's tree-quality metric (lower is better). 0 for an empty/single-leaf tree.
    [[nodiscard]] crd::f32 sah_cost() const noexcept;
    // Longest root→leaf path (edges). 0 for empty; 0 for a single leaf.
    [[nodiscard]] crd::usize max_depth() const noexcept;
    // Debug: full structural check (parent/child links, height invariant, AABB
    // enclosure, leaf count, free-list disjointness). CRD_ASSERTs on any breach.
    void validate() const noexcept;

private:
    static constexpr crd::u32 k_null = 0xFFFFFFFFU;

    struct Node
    {
        AABB3<crd::f32> aabb{};  // fat AABB (interior: union of children; leaf: inflated tight)
        crd::u32 parent{k_null}; // k_null for the root and for free slots
        crd::u32 child1{k_null}; // k_null ⇒ leaf
        crd::u32 child2{k_null};
        crd::u32 user_data{0};      // valid on allocated leaves
        crd::u32 next_free{k_null}; // free-list link on free slots
        crd::i32 height{-1};        // 0 = leaf; ≥1 = interior (max child height + 1); -1 = free slot

        [[nodiscard]] bool is_leaf() const noexcept { return child1 == k_null; }
    };

    [[nodiscard]] bool is_allocated_leaf(crd::u32 i) const noexcept
    {
        return i < m_nodes.size() && m_nodes[i].height == 0;
    }

    crd::u32 allocate_node();
    void free_node(crd::u32 i);
    void insert_leaf(crd::u32 leaf);
    void remove_leaf(crd::u32 leaf);
    // Height-based single rotation rooted at `i`; returns the new subtree root.
    crd::u32 balance(crd::u32 i);
    void fit_node_to_children(crd::u32 i) noexcept; // recompute aabb + height from children

    crd::containers::Array<Node> m_nodes;
    crd::u32 m_root{k_null};
    crd::u32 m_free_list{k_null};
    crd::usize m_leaf_count{0};
    DynamicBvhConfig m_cfg{};
};

} // namespace crd::geometry::bvh
