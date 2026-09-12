#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — `RTree<T>`, the R*-tree dynamic AABB index.
// Phase 3.1.7 v5c. Reference: Beckmann, Kriegel, Schneider, Seeger, *The R*-
// tree: An Efficient and Robust Access Method for Points and Rectangles*,
// SIGMOD 1990. Bulk-load: Leutenegger, Lopez, Edgington, *STR: A Simple and
// Efficient Algorithm for R-Tree Packing*, ICDE 1997.
//
// Compared to LooseOctree (v5b): R*-tree is the right index for **static or
// slow-change** AABB data — denser packing (no loose factor inflating cell
// extents), better culling (every node's AABB is the tight union of its
// children), and supports bulk-load via STR for ~7× faster construction
// from cooked level data. LooseOctree wins on highly dynamic broadphase
// (per-tick update fast-path); R*-tree wins on static culling +
// k-nearest-neighbour queries.
//
// ── Algorithm — full Beckmann 1990 R*-tree (NOT Guttman's base R-tree) ────
//
//   * **choose-subtree (§4.1)**: at the level above leaves, pick the child
//     entry minimising **overlap enlargement** (lex tiebreak: area
//     enlargement → area → lowest child index). At higher levels, pick by
//     **area enlargement** (lex tiebreak: area → lowest child index). The
//     overlap-enlargement metric is what distinguishes R*-tree from Guttman.
//
//   * **split (§4.2)**: for each axis, sort entries twice (by lower bound,
//     then by upper bound). For each sorted ordering, enumerate distributions
//     of size {m..M-m+1}. Sum the perimeters of the two resulting bounding
//     rectangles across all distributions. Pick the axis minimising the
//     perimeter sum. Within that axis, pick the distribution minimising
//     overlap (lex tiebreak: area). This is canonical R*-tree split.
//
//   * **forced reinsertion (§4.3)**: on insertion overflow, if this LEVEL has
//     not yet been "treated" during this insert call, remove the
//     `floor(M × 0.3) = 4` entries farthest from the node centre, sort them
//     by distance ASCENDING, and re-insert them from the root. Mark the
//     level as treated (one-shot per insert). This globally compacts the
//     tree on inserts and avoids cascading splits.
//
//   * **delete + condense-tree (Guttman §3.4)**: find leaf, remove entry,
//     walk up: if a node underflows (size < m), collect its orphan entries,
//     remove the node from its parent, and reinsert the orphans at their
//     ORIGINAL level. Root special case: if root has 1 child, promote.
//
//   * **STR bulk load (Leutenegger 1997)**: for N entries with capacity M,
//     L=ceil(N/M) leaves, S=ceil(sqrt(L)) vertical slabs. Sort by x-midpoint,
//     divide into S slabs; sort each slab by y-midpoint, divide into leaves
//     of M. Recursively pack levels above. Optimal packing for cooked-level
//     data; the right tool when consumer authors a static scene.
//
// ── Queries ─────────────────────────────────────────────────────────────────
//   * `overlap(box, on_hit)` — recursive descent (no traversal-order
//     contract; cells visited per child-entry order).
//   * `raycast(ray)` — t-near-first child descent + best_t pruning.
//     Emission order NOT part of API (BVH pattern, ADR-0076 §16 pin #2).
//     Lowest-payload-index tiebreak on equal t.
//   * `nearest_n(query, k)` — Hjaltason-Samet 1999 incremental priority-queue
//     k-NN. Priority queue ordered by min-distance from query point to AABB
//     (lex tiebreak on entry id). Pop closest item; if leaf entry, emit;
//     if node, push children. Stops when k results emitted. Optimal — no
//     extra work beyond what's needed.
//
// ── Stable leaf-entry handle ───────────────────────────────────────────────
// R*-tree splits move entries between nodes — `{node_idx, entry_idx}` pairs
// drift over the tree's lifetime. To keep handles stable across splits, we
// maintain an indirection table `m_handle_to_location[handle] -> {node, slot}`
// updated whenever an entry moves. Free-list of recycled handles.
//
// ── Two-layer typing (ADR-0078 §5 D34) ─────────────────────────────────────
// Algorithm body is raw `<MathScalar T>`. Typed wrappers in
// `rtree_queries_typed.hpp` strip-compute-retag at the API surface. Same
// pattern as `kd_queries_typed.hpp` (v5a) and `octree_queries_typed.hpp` (v5b).
//
// Builder REJECTS non-finite AABBs in debug (`CRD_ASSERT(is_finite(aabb))`);
// queries TOLERATE non-finite query inputs (defensive `is_finite` short-
// circuits at the API surface).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::spatial
{
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::math::MathScalar;
using crd::math::Vec3;

// Stable handle for a leaf entry in an `RTree`. Survives splits / reinsertions
// (the indirection table inside the tree is updated whenever the entry moves).
struct RTreeLeafId
{
    static constexpr crd::u32 k_invalid = 0xFFFFFFFFU;
    crd::u32 value{k_invalid};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != k_invalid; }
    [[nodiscard]] friend constexpr bool operator==(RTreeLeafId a, RTreeLeafId b) noexcept
    {
        return a.value == b.value;
    }
};

// Branching factor M (max entries per node) and min fill m. Beckmann §6's
// recommended defaults: M=16 (cache-friendly node size), m=5 (≈30% of M).
inline constexpr crd::u32 k_rtree_max_entries = 16;
inline constexpr crd::u32 k_rtree_min_entries = 5;

// Forced-reinsertion ratio (Beckmann §4.3) — reinsert this fraction of a
// node's entries on first overflow per level per insert call.
inline constexpr crd::u32 k_rtree_reinsert_p = 4; // floor(0.3 × 16) = 4

// One entry in an R-tree node. Carries both the user payload (leaf only) and
// the stable handle (leaf only — used to update `m_locations` when split /
// reinsert / condense moves the entry between nodes). For interior entries,
// `payload` is the child node index and `handle` is unused.
//
// The two-field layout costs 4 B per entry (32 B vs 28 B) but lets the
// inline `overlap` callback emit `entry.payload` directly without a side-
// table indirection (= zero-overhead query path).
template <MathScalar T>
struct RTreeEntry
{
    AABB3<T> aabb{};
    crd::u32 payload{0}; // child node index (interior) OR caller user payload (leaf)
    crd::u32 handle{0};  // stable handle (leaf only); 0xFFFFFFFFU on interior entries
};

template <MathScalar T>
struct RTreeNode
{
    RTreeEntry<T> entries[k_rtree_max_entries]{};
    crd::u32      entry_count{0};
    crd::u32      parent{0xFFFFFFFFU}; // parent node index (root = invalid)
    crd::u8       level{0};            // 0 = leaf; >0 = interior; root has the largest level
    crd::u8       flags{0};            // bit 0 = allocated
    crd::u8       pad_[2]{};

    [[nodiscard]] constexpr bool is_leaf() const noexcept { return level == 0; }
};

template <MathScalar T>
class RTree
{
public:
    static constexpr crd::u32 k_null = 0xFFFFFFFFU;

    explicit RTree(crd::memory::IAllocator* alloc) noexcept;

    RTree(const RTree&) = delete;
    RTree& operator=(const RTree&) = delete;
    RTree(RTree&&) noexcept = default;
    RTree& operator=(RTree&&) noexcept = default;
    ~RTree() = default;

    // ---- mutation -----------------------------------------------------------

    // Insert a leaf entry. Returns a stable handle that survives subsequent
    // splits / reinsertions / deletes of OTHER entries.
    [[nodiscard]] RTreeLeafId insert(const AABB3<T>& aabb, crd::u32 payload);

    // Remove an entry. Handle invalid afterwards (slot may be reused).
    void remove(RTreeLeafId id);

    // STR bulk-load constructor (Leutenegger 1997). Replaces tree contents
    // with an optimally-packed tree built from the given entries. Returns
    // an Array of stable handles parallel to the input order.
    void bulk_load(crd::containers::ConstSpan<AABB3<T>>          aabbs,
                    crd::containers::ConstSpan<crd::u32>          payloads,
                    crd::containers::Array<RTreeLeafId>&          out_handles);

    // ---- access -------------------------------------------------------------

    [[nodiscard]] bool is_empty() const noexcept { return m_leaf_count == 0; }
    [[nodiscard]] crd::usize leaf_count() const noexcept { return m_leaf_count; }
    [[nodiscard]] crd::usize node_count() const noexcept { return m_allocated_nodes; }
    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }
    [[nodiscard]] AABB3<T> bounds() const noexcept;
    [[nodiscard]] AABB3<T> entry_aabb(RTreeLeafId id) const noexcept;
    [[nodiscard]] crd::u32 entry_payload(RTreeLeafId id) const noexcept;

    // Tree depth — root level + 1 (a 1-leaf tree has depth 1).
    [[nodiscard]] crd::u32 depth() const noexcept;

    // ---- queries ------------------------------------------------------------

    // Invoke `on_hit(crd::u32 payload)` for every leaf entry whose AABB
    // overlaps `query`. Traversal order: child-entry order per node
    // (deterministic given a fixed tree).
    template <typename Fn> void overlap(const AABB3<T>& query, Fn&& on_hit) const;

    // Append every overlapping leaf payload to `out`.
    void overlap(const AABB3<T>& query, crd::containers::Array<crd::u32>& out) const;

    // Nearest-hit raycast against leaf entries. t-near-first child descent.
    // Lowest-payload-index tiebreak on equal `t` (ADR-0076 §4 pin #11).
    // Emission order is NOT part of the API contract.
    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast(const Ray3<T>& ray,
            T tmax = std::numeric_limits<T>::infinity()) const noexcept;

    // k-nearest-neighbour query (Hjaltason-Samet 1999 incremental NN).
    // `out`'s capacity does NOT define `k` — pass it explicitly. `out` is
    // CLEARED + sorted ascending by `(distance², payload)` on return.
    struct Neighbor
    {
        crd::u32 payload{};
        T        distance_squared{0};
    };
    void nearest_n(const Vec3<T>&                          query,
                    crd::usize                              k,
                    crd::containers::Array<Neighbor>&       out) const noexcept;

    // ---- diagnostics --------------------------------------------------------

    // Whole-tree structural validation. Asserts on any invariant breach.
    // (Used by tests + debug builds; not on the hot path.)
    void validate() const noexcept;

private:
    static constexpr crd::u32 k_invalid_handle = 0xFFFFFFFFU;

    // Stable-handle indirection — m_locations[handle] = (node_idx, entry_idx).
    // Free-list of recycled handle slots: m_loc_free_list head + per-slot
    // `next_free` parked in node_idx when alive == false.
    struct EntryLocation
    {
        crd::u32 node_idx{k_null};
        crd::u32 entry_idx{0};
        bool     alive{false};
    };

    // ---- node pool ----------------------------------------------------------
    [[nodiscard]] crd::u32 allocate_node(crd::u8 level);
    void free_node(crd::u32 idx);
    [[nodiscard]] bool is_node_alive(crd::u32 idx) const noexcept
    {
        return idx < m_nodes.size() && (m_nodes[idx].flags & 1U) != 0U;
    }

    // ---- handle pool --------------------------------------------------------
    [[nodiscard]] crd::u32 allocate_handle(crd::u32 node_idx, crd::u32 entry_idx);
    void free_handle(crd::u32 handle);
    void update_handle_location(crd::u32 handle, crd::u32 node_idx, crd::u32 entry_idx);

    // ---- R*-tree algorithms -------------------------------------------------
    // choose-subtree: descend from root to the level just above target_level,
    // picking child entries by R*-tree's overlap/area heuristics. Returns the
    // node at target_level into which the entry should be inserted.
    [[nodiscard]] crd::u32 choose_subtree(const AABB3<T>& target_aabb, crd::u8 target_level) const;

    // R*-tree split (Beckmann §4.2). Splits `node` (which has M+1 entries
    // staged via the temp buffer) into TWO nodes: the original `node` keeps
    // one half, the returned node index gets the other half. Updates handle
    // locations for all moved leaf entries.
    [[nodiscard]] crd::u32 split_node(crd::u32 node_idx, RTreeEntry<T> overflow_entry);

    // R*-tree forced reinsertion (Beckmann §4.3). Removes the 30% entries
    // farthest from the node center and re-inserts them from the root. Used
    // on first overflow per level per insert call (tracked via
    // m_treated_levels bit-set).
    void reinsert(crd::u32 node_idx, RTreeEntry<T> overflow_entry);

    // After an insert (entry added to a leaf node, possibly splitting),
    // walk up adjusting parent AABBs + propagating splits.
    void adjust_tree_after_insert(crd::u32 node_idx, crd::u32 split_sibling);

    // Insert helper — orchestrates choose_subtree + insert + reinsert/split.
    // `level` parameter supports reinsertion (orphans inserted at their
    // original level, not always at leaves).
    void insert_entry(const RTreeEntry<T>& entry, crd::u8 level);

    // Delete helper — walks up from the modified leaf, refreshing parent
    // AABBs and condensing underflow nodes (orphans reinserted at their
    // original level).
    void condense_tree(crd::u32 node_idx);

    // STR bulk-load helper (recursive level packing).
    crd::u32 str_pack_level(crd::containers::Array<RTreeEntry<T>>& level_entries, crd::u8 level);

    // Geometry helpers.
    [[nodiscard]] static AABB3<T> aabb_union(const AABB3<T>& a, const AABB3<T>& b) noexcept;
    [[nodiscard]] static AABB3<T> aabb_union_of_entries(const RTreeEntry<T>* e, crd::u32 n) noexcept;
    [[nodiscard]] static T aabb_area(const AABB3<T>& a) noexcept;
    [[nodiscard]] static T aabb_perimeter(const AABB3<T>& a) noexcept;
    [[nodiscard]] static T aabb_overlap_area(const AABB3<T>& a, const AABB3<T>& b) noexcept;
    [[nodiscard]] static Vec3<T> aabb_center(const AABB3<T>& a) noexcept;
    [[nodiscard]] static T point_to_aabb_dist_sq(const Vec3<T>& p, const AABB3<T>& a) noexcept;

    // ---- storage ------------------------------------------------------------
    crd::memory::IAllocator*               m_alloc{nullptr};
    crd::containers::Array<RTreeNode<T>>   m_nodes;
    crd::containers::Array<EntryLocation>  m_locations;

    crd::u32 m_root{k_null};
    crd::u32 m_node_free_list{k_null};
    crd::u32 m_handle_free_list{k_invalid_handle};
    crd::usize m_leaf_count{0};
    crd::usize m_allocated_nodes{0};

    // Forced-reinsertion bookkeeping. Bit `i` set ⇒ level `i` already
    // reinserted during the current insert call (Beckmann §4.3 — once per
    // level per insert). Reset on every public insert / bulk_load entry.
    crd::u32 m_treated_levels{0};
};

// ---- inline templates -------------------------------------------------------

template <MathScalar T>
template <typename Fn>
void RTree<T>::overlap(const AABB3<T>& query, Fn&& on_hit) const
{
    if (m_root == k_null) { return; }
    if (!crd::geometry::primitives::is_finite(query)) { return; }

    // Iterative DFS. Worst-case sp = M × depth (push every child per
    // descent). M=16, depth ≤ 16 ⇒ 256 frames covers any practical scene.
    constexpr crd::usize stack_max = 256;
    crd::u32 stack[stack_max];
    crd::usize sp = 0;
    stack[sp++] = m_root;
    while (sp > 0)
    {
        const crd::u32 ni = stack[--sp];
        const RTreeNode<T>& node = m_nodes[ni];
        if (node.is_leaf())
        {
            for (crd::u32 i = 0; i < node.entry_count; ++i)
            {
                if (crd::geometry::primitives::intersects(node.entries[i].aabb, query))
                {
                    on_hit(node.entries[i].payload);
                }
            }
        }
        else
        {
            for (crd::u32 i = 0; i < node.entry_count; ++i)
            {
                if (crd::geometry::primitives::intersects(node.entries[i].aabb, query))
                {
                    CRD_ASSERT(sp < stack_max);
                    stack[sp++] = node.entries[i].payload;
                }
            }
        }
    }
}

using RTreef = RTree<crd::f32>;
using RTreed = RTree<crd::f64>;

extern template class RTree<crd::f32>;
extern template class RTree<crd::f64>;

} // namespace crd::geometry::spatial
