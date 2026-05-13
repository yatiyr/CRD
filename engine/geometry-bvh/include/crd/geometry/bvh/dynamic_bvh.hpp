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

#include <limits>

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
