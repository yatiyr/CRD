#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — `LooseOctree<T>`, the dynamic AABB index (Phase 3.1.7
// v5b). Reference: Thatcher Ulrich, "Loose Octrees" in *Game Programming Gems*
// vol. 1, 2000.
//
// **Workhorse for scene spatial culling** + eylem broadphase + editor selection.
//
// The classical octree's recurring failure: an object straddling a midplane
// must be promoted to the parent, hurting query cost. Ulrich's fix: each cell
// has a *loose AABB* (tight bounds × `loosening`, default 2.0) shared with its
// parent. Property: an object whose tight AABB has extent ≤ loosening × cell_size
// fits within the loose AABB of the *single* cell containing its center —
// regardless of midplane proximity. No straddling, single-cell residency,
// O(log N) insert.
//
// ── Storage ─────────────────────────────────────────────────────────────────
//   * `m_nodes`   — 64 B `OctreeNode` pool with free-list (DynamicBvh pattern).
//                   Each interior cell has up to 8 children (sparse — `k_null`
//                   in unused octant slots). Tight cell bounds stored; loose
//                   bounds computed via `loose_aabb_of(node)` (constexpr-cheap).
//   * `m_objects` — `ObjectEntry` pool with free-list. `{aabb, payload,
//                   cell_node}`. Stable `u32` handle = pool slot index.
//                   Removed handle's slot recycled on next insert.
//   * Per-cell objects: each cell owns `Array<u32 obj_idx>` (allocated only
//                   for non-empty cells; first insert allocates, threshold
//                   `KdBuildOptions::leaf_object_threshold = 8` triggers split
//                   when an interior-non-leaf cell would otherwise carry
//                   "objects too big to push down").
//
// ── Insert ──────────────────────────────────────────────────────────────────
//   1. **Reject** non-finite AABB, center outside root, extent > loosening ×
//      root extent (debug `CRD_ASSERT` — ADR-0076 §15 builder-reject pin).
//   2. Compute target depth = deepest cell where object fits. With loosening
//      `k`: extent ≤ `k * cell_size_at_depth_d` ⇒ `d ≤ log2(k * root_extent /
//      max(extent, ε))`. Deepest `≤ max_depth` cap (default 8 — `2^8 = 256³`
//      cells fits any practical scene, ~16 MB worst case fully populated).
//   3. Walk down from root. At each depth, compute octant of `center` via
//      `octant_of_center(node, center)` (bit i = `center[i] >= midplane[i]`,
//      `>=` not `>` — "lower octant wins" lex tiebreak). Lazily allocate the
//      child cell.
//   4. Append `obj_idx` to terminal cell's object list. Object's `cell_node`
//      back-pointer = terminal cell index (used by `remove` / `update`).
//
// ── Update fast-path ────────────────────────────────────────────────────────
// Per Ulrich's correctness invariant: an object is correctly findable by every
// query touching its cell as long as its tight AABB fits within the cell's
// **loose AABB**. The fast-path predicate is therefore *AABB-fit only*, NOT
// "center still inside cell" — center can drift out of the cell as long as
// the loose box still encloses the object. Wider window, ~90%+ of small motion.
//
// Slow path (loose AABB no longer encloses): remove object from old cell,
// reinsert via the standard descent. Handle stays valid (the pool slot is
// not freed — we mutate the AABB in place).
//
// ── Remove ──────────────────────────────────────────────────────────────────
// Remove from the cell's object array (swap-with-last, O(cell_count)); free
// the object pool slot. Cells are *not* automatically collapsed when emptied
// (cheap in steady-state churn; would need recursive parent-empty checks). A
// `compact()` follow-on can land in v5b-fast if a consumer surfaces.
//
// ── Queries ─────────────────────────────────────────────────────────────────
//   * `overlap(box, on_hit)` — Morton-order child traversal (000, 001, …, 111
//     deterministic). Per cell: prune by query-vs-loose-AABB; scan local
//     objects + recurse into allocated children.
//   * `raycast(ray)` — **t-near-first** child descent with `best_t` pruning.
//     Emission order *is not* part of the API contract (only the nearest hit
//     is) — this is the BVH pattern (ADR-0076 §16 pin #2). Lowest-payload-
//     index tiebreak on equal `t`.
//
// Queries TOLERATE non-finite query input — every finite-vs-NaN comparison
// is false → loose-AABB-vs-query prune kills every subtree at root → empty
// result. Symmetric with v5a `kd_*` + ADR-0076 §15 BVH pin.
//
// ── Two-layer typing (ADR-0078 §5 D34) ─────────────────────────────────────
// Algorithm body is raw `<MathScalar T>`. Typed wrappers in
// `octree_queries_typed.hpp` strip-compute-retag at the API surface. Same
// pattern as `kd_queries_typed.hpp` (v5a) and `mesh_queries_typed.hpp` (v4).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>      // AABB3, Ray3, intersects
#include <crd/geometry/primitives/robust_ray_aabb.hpp> // ray-AABB precompute
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

// Stable handle for an object in a `LooseOctree`.
struct OctreeObjectId
{
    static constexpr crd::u32 k_invalid = 0xFFFFFFFFU;
    crd::u32 value{k_invalid};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != k_invalid; }
    [[nodiscard]] friend constexpr bool operator==(OctreeObjectId a, OctreeObjectId b) noexcept
    {
        return a.value == b.value;
    }
};

// Default split trigger — grow a child cell when an *interior* cell already
// has more local objects than this. Non-leaf cells at `max_depth` ignore the
// threshold (no deeper to push to — capped purely by the depth cap). Configure
// via `OctreeBuildOptions::leaf_object_threshold`.
inline constexpr crd::u32 k_octree_leaf_object_threshold = 8;

// Default ceiling on cell depth. 2^8 = 256³ uniform-grid worst case (16 M cells
// fully populated; rarely realised because the tree is sparse). The depth that
// makes sense is `log2(loosening * root_extent / smallest_object_extent)`;
// 8 is plenty for typical scene scales (1024-unit world + 0.125-unit smallest
// object hits depth ~13 only in pathological clustering — and those clusters
// just retain extra objects per cell, not a correctness issue).
inline constexpr crd::u8 k_octree_max_depth = 8;

// Default loosening factor. 2.0 = Ulrich 2000's recommendation (loose AABB
// edge length is twice the tight cell edge). Other values change the depth-
// vs-cell-overlap tradeoff: 1.5 = tighter culling, fewer objects per cell;
// 3.0+ = wider loose boxes, more cells touched per query but more objects
// fit deeper. 2.0 is the canonical pin.
template <MathScalar T>
[[nodiscard]] constexpr T k_octree_loosening() noexcept
{
    return T{2};
}

template <MathScalar T>
struct OctreeBuildOptions
{
    AABB3<T> root_bounds{};
    T        loosening{k_octree_loosening<T>()};
    crd::u32 leaf_object_threshold{k_octree_leaf_object_threshold};
    crd::u8  max_depth{k_octree_max_depth};
};

// 64-byte cell node (one per cache line). Layout pinned via `static_assert`.
template <MathScalar T>
struct OctreeNode
{
    AABB3<T>   bounds{};       // tight cell bounds; loose = `loose_aabb_of(node, loosening)`
    crd::u32   children[8]{};  // k_null in unused octants
    crd::u32   parent{0};      // k_null on root
    crd::u8    depth{0};       // 0 = root
    crd::u8    flags{0};       // bit 0 = allocated; (free if 0)
    crd::u8    pad_[2]{};
};
// f32 cell-node pin: 24 (AABB) + 32 (8×u32 children) + 4 (parent) + 1+1+2 (depth+flags+pad) = 64 B.
static_assert(sizeof(OctreeNode<crd::f32>) == 64,
              "OctreeNode<f32> sizing pinned — 64 B (one per cache line); accidental field bloat is a CI fail");

template <MathScalar T>
class LooseOctree
{
public:
    static constexpr crd::u32 k_null = 0xFFFFFFFFU;

    // The user must supply finite root bounds with positive extent on every axis.
    LooseOctree(crd::memory::IAllocator* alloc, const OctreeBuildOptions<T>& opts);

    LooseOctree(const LooseOctree&) = delete;
    LooseOctree& operator=(const LooseOctree&) = delete;
    LooseOctree(LooseOctree&&) noexcept = default;
    LooseOctree& operator=(LooseOctree&&) noexcept = default;
    ~LooseOctree() = default;

    // ---- mutation -----------------------------------------------------------

    // Insert an object. Returns a stable handle. Preconditions (debug `CRD_ASSERT`):
    //   * `is_finite(aabb)` — non-finite is REJECTED at the boundary
    //   * AABB center inside `root_bounds`
    //   * AABB extent ≤ `loosening` × root extent on every axis
    [[nodiscard]] OctreeObjectId insert(const AABB3<T>& aabb, crd::u32 payload);

    // Remove an object. Handle is invalid afterwards (slot may be reused).
    void remove(OctreeObjectId id);

    // Update an object's AABB. If the new AABB still fits within its cell's
    // loose AABB (Ulrich correctness invariant), the call is a no-op and
    // returns false. Otherwise the object is removed from its cell and
    // re-inserted (handle stays valid; pool slot retained).
    bool update(OctreeObjectId id, const AABB3<T>& new_aabb);

    // ---- access -------------------------------------------------------------

    [[nodiscard]] bool is_empty() const noexcept { return m_object_count == 0U; }
    [[nodiscard]] crd::usize node_count() const noexcept { return m_allocated_nodes; }
    [[nodiscard]] crd::usize object_count() const noexcept { return m_object_count; }
    [[nodiscard]] crd::u8 max_depth_used() const noexcept { return m_max_depth_used; }
    [[nodiscard]] AABB3<T> bounds() const noexcept { return m_root_bounds; }
    [[nodiscard]] T loosening() const noexcept { return m_loosening; }
    [[nodiscard]] AABB3<T> object_aabb(OctreeObjectId id) const noexcept
    {
        CRD_ASSERT(is_object_alive(id.value));
        return m_objects[id.value].aabb;
    }
    [[nodiscard]] crd::u32 object_payload(OctreeObjectId id) const noexcept
    {
        CRD_ASSERT(is_object_alive(id.value));
        return m_objects[id.value].payload;
    }

    // ---- queries ------------------------------------------------------------

    // Invoke `on_hit(crd::u32 payload)` for every object whose stored tight
    // AABB overlaps `query`. Cells are traversed in Morton octant order (000,
    // 001, …, 111) — deterministic emission. Local-object scan order is
    // insertion order within each cell.
    template <typename Fn> void overlap(const AABB3<T>& query, Fn&& on_hit) const;

    // Append every overlapping object's payload to `out` (deterministic order).
    void overlap(const AABB3<T>& query, crd::containers::Array<crd::u32>& out) const;

    // Nearest-hit raycast against object tight AABBs. t-near-first child
    // descent + `best_t` pruning. Lowest-payload-index tiebreak on equal `t`
    // (ADR-0076 §4 pin #11). Emission order is NOT part of the API — only
    // the nearest result is. (BVH pattern.)
    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast(const Ray3<T>& ray,
            T tmax = std::numeric_limits<T>::infinity()) const noexcept;

    // ---- diagnostics --------------------------------------------------------

    // Loose AABB of a cell — tight bounds expanded by `(loosening - 1)/2 ×
    // extent` on each side around the cell center. Public so tests + viz can
    // visualise the loose extents (they're the actual prune surface).
    [[nodiscard]] AABB3<T> loose_aabb_of(const OctreeNode<T>& node) const noexcept;

private:
    static constexpr crd::u32 k_invalid_node = 0xFFFFFFFFU;

    struct ObjectEntry
    {
        AABB3<T> aabb{};
        crd::u32 payload{0};
        crd::u32 cell_node{k_invalid_node};   // back-pointer to cell containing this object
        crd::u32 next_free{k_invalid_node};   // free-list link when alive == false
        bool     alive{false};
    };

    // Cell structure — one Array per cell (allocated lazily on first insert).
    struct CellObjects
    {
        crd::containers::Array<crd::u32> ids;
        explicit CellObjects(crd::memory::IAllocator* a) : ids(a) {}
    };

    // ---- node pool helpers --------------------------------------------------
    [[nodiscard]] crd::u32 allocate_node(const AABB3<T>& bounds, crd::u32 parent, crd::u8 depth);
    void free_node(crd::u32 idx);
    [[nodiscard]] bool is_node_alive(crd::u32 idx) const noexcept
    {
        return idx < m_nodes.size() && (m_nodes[idx].flags & 1U) != 0U;
    }

    // ---- object pool helpers ------------------------------------------------
    [[nodiscard]] crd::u32 allocate_object(const AABB3<T>& aabb, crd::u32 payload, crd::u32 cell_node);
    void free_object(crd::u32 idx);
    [[nodiscard]] bool is_object_alive(crd::u32 idx) const noexcept
    {
        return idx < m_objects.size() && m_objects[idx].alive;
    }

    // ---- octree algorithms --------------------------------------------------

    // Octant index of `center` relative to a cell. bit i = (center[i] >=
    // midplane[i] ? 1 : 0). `>=` not `>` — "lower octant wins" lex tiebreak
    // when center sits exactly on a midplane.
    [[nodiscard]] static crd::u8 octant_of_center(const AABB3<T>& cell, const Vec3<T>& center) noexcept;

    // Tight bounds of `octant`-th child of `parent_bounds`.
    [[nodiscard]] static AABB3<T> child_bounds_of(const AABB3<T>& parent_bounds, crd::u8 octant) noexcept;

    // Compute the deepest cell-depth at which `extent` fits within the loose
    // bound (loosening × cell extent at that depth). Uses root extent as
    // reference. Capped at `m_max_depth`.
    [[nodiscard]] crd::u8 target_depth_for(const Vec3<T>& extent) const noexcept;

    // Insert `obj_idx` into the cell at the given position. Walks from root,
    // lazily allocates intermediate cells. Returns the terminal cell index.
    crd::u32 descend_and_insert(crd::u32 obj_idx, const Vec3<T>& center, crd::u8 target_depth);

    // Append `obj_idx` to a cell's object list. Allocates the CellObjects on
    // first insert into that cell.
    void cell_add_object(crd::u32 cell_idx, crd::u32 obj_idx);
    // Remove `obj_idx` from a cell's list. Swap-with-last, O(cell_count).
    void cell_remove_object(crd::u32 cell_idx, crd::u32 obj_idx);

    // Inline overlap recursion + raycast recursion bodies (templated; called
    // from the `overlap` member template). Defined out-of-line in `loose_octree.cpp`
    // ... actually no, the overlap query is templated on user callback so it
    // must live inline. The raycast (non-templated body) lives in `.cpp`.
    template <typename Fn> void overlap_recursive(crd::u32 cell_idx, const AABB3<T>& query, Fn& on_hit) const;

    // ---- storage ------------------------------------------------------------
    crd::memory::IAllocator*               m_alloc{nullptr};
    crd::containers::Array<OctreeNode<T>>  m_nodes;
    crd::containers::Array<ObjectEntry>    m_objects;
    crd::containers::Array<CellObjects>    m_cells; // index parallel to m_nodes; ids = m_cells[idx].ids

    crd::u32 m_root{k_null};
    crd::u32 m_node_free_list{k_null};
    crd::u32 m_object_free_list{k_null};
    crd::usize m_allocated_nodes{0};
    crd::usize m_object_count{0};
    crd::u8    m_max_depth_used{0};

    AABB3<T> m_root_bounds{};
    T        m_loosening{k_octree_loosening<T>()};
    crd::u32 m_leaf_object_threshold{k_octree_leaf_object_threshold};
    crd::u8  m_max_depth{k_octree_max_depth};
};

// ---- inline templates -------------------------------------------------------

template <MathScalar T>
template <typename Fn>
void LooseOctree<T>::overlap(const AABB3<T>& query, Fn&& on_hit) const
{
    if (m_root == k_null) { return; }
    Fn on_hit_ref = static_cast<Fn&&>(on_hit);
    overlap_recursive<Fn>(m_root, query, on_hit_ref);
}

template <MathScalar T>
template <typename Fn>
void LooseOctree<T>::overlap_recursive(crd::u32 cell_idx, const AABB3<T>& query, Fn& on_hit) const
{
    const OctreeNode<T>& node = m_nodes[cell_idx];
    const AABB3<T> loose = loose_aabb_of(node);
    if (!crd::geometry::primitives::intersects(loose, query)) { return; }

    // Local objects of this cell.
    if (cell_idx < m_cells.size())
    {
        const auto& list = m_cells[cell_idx].ids;
        for (crd::usize i = 0; i < list.size(); ++i)
        {
            const crd::u32 obj_idx = list[i];
            const ObjectEntry& obj = m_objects[obj_idx];
            if (crd::geometry::primitives::intersects(obj.aabb, query))
            {
                on_hit(obj.payload);
            }
        }
    }

    // Children in Morton order (000…111). Determinism: octant index walk.
    for (crd::u8 oct = 0; oct < 8U; ++oct)
    {
        const crd::u32 child = node.children[oct];
        if (child != k_null)
        {
            overlap_recursive<Fn>(child, query, on_hit);
        }
    }
}

using LooseOctreef = LooseOctree<crd::f32>;
using LooseOctreed = LooseOctree<crd::f64>;

extern template class LooseOctree<crd::f32>;
extern template class LooseOctree<crd::f64>;

} // namespace crd::geometry::spatial
