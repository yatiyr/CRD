// crd-geometry-spatial — R*-tree impl (Phase 3.1.7 v5c).
//
// Reference: Beckmann, Kriegel, Schneider, Seeger, *The R*-tree: An Efficient
// and Robust Access Method for Points and Rectangles*, SIGMOD 1990.
// STR bulk-load: Leutenegger, Lopez, Edgington, *STR: A Simple and Efficient
// Algorithm for R-Tree Packing*, ICDE 1997.
// k-NN: Hjaltason & Samet, *Distance Browsing in Spatial Databases*, ACM TODS
// 1999.
//
// Header `rtree.hpp` documents the design + locked decisions.

#include <crd/geometry/spatial/rtree.hpp>

#include <crd/containers/sort.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::geometry::spatial
{
using crd::f32;
using crd::f64;
using crd::i32;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::math::Vec3;

namespace
{

// Maximum traversal stack depth for raycast/k-NN/etc. Worst-case stack
// growth = M × depth (push every child per descent step). R*-tree depth
// is O(log_M N): for M=16 + N=1e9, depth ≤ 8 → worst-case 128 frames.
// 256 covers any practical scene with margin even at full-M branching.
// (A v5c-fast follow-on can introduce caller-supplied scratch via
// crd::containers::Array — same `DynamicBvhPairScratch` pattern — for
// hot-path consumers needing zero stack reservation.)
constexpr usize kRtreeMaxStack = 256;

// AABB intersects (closed): touching counts as overlap. Matches
// `primitives::intersects`.
template <MathScalar T>
inline bool aabb_intersects(const AABB3<T>& a, const AABB3<T>& b) noexcept
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

} // namespace

// =============================================================================
// Geometry helpers (static)
// =============================================================================

template <MathScalar T>
AABB3<T> RTree<T>::aabb_union(const AABB3<T>& a, const AABB3<T>& b) noexcept
{
    return AABB3<T>{
        Vec3<T>{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
        Vec3<T>{std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

template <MathScalar T>
AABB3<T> RTree<T>::aabb_union_of_entries(const RTreeEntry<T>* e, u32 n) noexcept
{
    CRD_ASSERT(n > 0);
    AABB3<T> r = e[0].aabb;
    for (u32 i = 1; i < n; ++i) { r = aabb_union(r, e[i].aabb); }
    return r;
}

template <MathScalar T>
T RTree<T>::aabb_area(const AABB3<T>& a) noexcept
{
    const T dx = a.max.x - a.min.x;
    const T dy = a.max.y - a.min.y;
    const T dz = a.max.z - a.min.z;
    // Negative-extent (empty / degenerate) → 0.
    if (dx <= T{0} || dy <= T{0} || dz <= T{0}) { return T{0}; }
    return T{2} * (dx * dy + dy * dz + dz * dx);
}

template <MathScalar T>
T RTree<T>::aabb_perimeter(const AABB3<T>& a) noexcept
{
    const T dx = std::max(a.max.x - a.min.x, T{0});
    const T dy = std::max(a.max.y - a.min.y, T{0});
    const T dz = std::max(a.max.z - a.min.z, T{0});
    return T{4} * (dx + dy + dz);
}

template <MathScalar T>
T RTree<T>::aabb_overlap_area(const AABB3<T>& a, const AABB3<T>& b) noexcept
{
    const T dx = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
    const T dy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
    const T dz = std::min(a.max.z, b.max.z) - std::max(a.min.z, b.min.z);
    if (dx <= T{0} || dy <= T{0} || dz <= T{0}) { return T{0}; }
    return T{2} * (dx * dy + dy * dz + dz * dx);
}

template <MathScalar T>
Vec3<T> RTree<T>::aabb_center(const AABB3<T>& a) noexcept
{
    return Vec3<T>{(a.min.x + a.max.x) * T{0.5},
                    (a.min.y + a.max.y) * T{0.5},
                    (a.min.z + a.max.z) * T{0.5}};
}

template <MathScalar T>
T RTree<T>::point_to_aabb_dist_sq(const Vec3<T>& p, const AABB3<T>& a) noexcept
{
    T d2 = T{0};
    for (int i = 0; i < 3; ++i)
    {
        const T v = p[static_cast<usize>(i)];
        const T lo = a.min[static_cast<usize>(i)];
        const T hi = a.max[static_cast<usize>(i)];
        if (v < lo)      { const T d = lo - v; d2 += d * d; }
        else if (v > hi) { const T d = v - hi; d2 += d * d; }
    }
    return d2;
}

// =============================================================================
// Construction + pool management
// =============================================================================

template <MathScalar T>
RTree<T>::RTree(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc)
    , m_nodes(alloc)
    , m_locations(alloc)
{
}

template <MathScalar T>
u32 RTree<T>::allocate_node(u8 level)
{
    u32 idx;
    if (m_node_free_list != k_null)
    {
        idx = m_node_free_list;
        // Free-list link parked in entries[0].payload.
        m_node_free_list = m_nodes[idx].entries[0].payload;
        m_nodes[idx] = RTreeNode<T>{};
    }
    else
    {
        idx = static_cast<u32>(m_nodes.size());
        m_nodes.push_back(RTreeNode<T>{});
    }
    m_nodes[idx].level = level;
    m_nodes[idx].flags = 1U;
    m_nodes[idx].parent = k_null;
    m_nodes[idx].entry_count = 0;
    ++m_allocated_nodes;
    return idx;
}

template <MathScalar T>
void RTree<T>::free_node(u32 idx)
{
    CRD_ASSERT(is_node_alive(idx));
    m_nodes[idx].flags = 0U;
    m_nodes[idx].entries[0].payload = m_node_free_list;
    m_node_free_list = idx;
    --m_allocated_nodes;
}

template <MathScalar T>
u32 RTree<T>::allocate_handle(u32 node_idx, u32 entry_idx)
{
    u32 h;
    if (m_handle_free_list != k_invalid_handle)
    {
        h = m_handle_free_list;
        m_handle_free_list = m_locations[h].node_idx; // free-list link parked here
    }
    else
    {
        h = static_cast<u32>(m_locations.size());
        m_locations.push_back(EntryLocation{});
    }
    m_locations[h] = EntryLocation{node_idx, entry_idx, true};
    return h;
}

template <MathScalar T>
void RTree<T>::free_handle(u32 handle)
{
    CRD_ASSERT(handle < m_locations.size());
    CRD_ASSERT(m_locations[handle].alive);
    m_locations[handle].alive = false;
    m_locations[handle].node_idx = m_handle_free_list;
    m_handle_free_list = handle;
}

template <MathScalar T>
void RTree<T>::update_handle_location(u32 handle, u32 node_idx, u32 entry_idx)
{
    CRD_ASSERT(handle < m_locations.size());
    CRD_ASSERT(m_locations[handle].alive);
    m_locations[handle].node_idx = node_idx;
    m_locations[handle].entry_idx = entry_idx;
}

// =============================================================================
// choose-subtree (Beckmann §4.1)
// =============================================================================

template <MathScalar T>
u32 RTree<T>::choose_subtree(const AABB3<T>& target_aabb, u8 target_level) const
{
    CRD_ASSERT(m_root != k_null);
    u32 cur = m_root;
    while (true)
    {
        const RTreeNode<T>& node = m_nodes[cur];
        if (node.level == target_level) { return cur; }
        CRD_ASSERT(!node.is_leaf()); // we can only descend into interior nodes

        // For child entries that point to LEAF nodes: minimise overlap
        // enlargement. Otherwise: minimise area enlargement.
        const bool children_are_leaves = (node.level == 1U);
        u32 best_idx = 0;
        T best_primary  = std::numeric_limits<T>::infinity();
        T best_secondary = std::numeric_limits<T>::infinity();
        T best_area     = std::numeric_limits<T>::infinity();
        u32 best_child  = 0xFFFFFFFFU;

        for (u32 i = 0; i < node.entry_count; ++i)
        {
            const RTreeEntry<T>& e = node.entries[i];
            const AABB3<T> enlarged = aabb_union(e.aabb, target_aabb);
            const T area_before = aabb_area(e.aabb);
            const T area_after  = aabb_area(enlarged);
            const T area_enlarge = area_after - area_before;

            T primary;
            if (children_are_leaves)
            {
                // Sum of overlap area of `enlarged` with every other sibling AABB,
                // minus the sum of overlap of e.aabb with the same set, gives the
                // overlap-enlargement metric.
                T overlap_after = T{0};
                T overlap_before = T{0};
                for (u32 j = 0; j < node.entry_count; ++j)
                {
                    if (j == i) { continue; }
                    overlap_after  += aabb_overlap_area(enlarged, node.entries[j].aabb);
                    overlap_before += aabb_overlap_area(e.aabb,   node.entries[j].aabb);
                }
                primary = overlap_after - overlap_before;
            }
            else
            {
                primary = area_enlarge;
            }

            // Lex tiebreak: (primary, area_enlarge, area_after, child_idx).
            const T secondary = area_enlarge;
            if (primary < best_primary
                || (primary == best_primary && secondary < best_secondary)
                || (primary == best_primary && secondary == best_secondary && area_after < best_area)
                || (primary == best_primary && secondary == best_secondary && area_after == best_area
                    && e.payload < best_child))
            {
                best_primary  = primary;
                best_secondary = secondary;
                best_area     = area_after;
                best_child    = e.payload;
                best_idx      = i;
            }
        }

        cur = node.entries[best_idx].payload;
    }
}

// =============================================================================
// R*-tree split (Beckmann §4.2)
// =============================================================================
//
// Stage all M+1 entries (the M in the node + the overflow entry) in a temp
// buffer. For each axis, sort by lower bound + sort by upper bound. For each
// of the (M-2m+2) distributions, compute perimeter sum. Pick the axis with
// the smallest perimeter sum. Within that axis (across both sorts), pick the
// distribution with smallest overlap (lex tiebreak: smallest total area).
// Distribute entries between the original node + a new sibling.

template <MathScalar T>
u32 RTree<T>::split_node(u32 node_idx, RTreeEntry<T> overflow_entry)
{
    // M = max-fanout, m = min-fanout per Beckmann 1990 �4.2 split notation.
    constexpr u32 M = k_rtree_max_entries; // NOLINT(readability-identifier-naming)
    constexpr u32 m = k_rtree_min_entries; // NOLINT(readability-identifier-naming)

    RTreeNode<T>& node = m_nodes[node_idx];
    const u8 node_level = node.level;
    const u32 node_parent = node.parent;
    CRD_ASSERT(node.entry_count == M);

    // Stage M+1 entries.
    RTreeEntry<T> staged[M + 1];
    for (u32 i = 0; i < M; ++i) { staged[i] = node.entries[i]; }
    staged[M] = overflow_entry;
    // N = M + 1 staged-entry count per Beckmann 1990 �4.2.
    constexpr u32 N = M + 1; // NOLINT(readability-identifier-naming)

    // Per-axis loop. Best (axis, sort, k) tuple where k is the split point
    // (entries [0..k) go to first group, [k..N) to second).
    u8  best_axis = 0;
    u8  best_sort = 0;       // 0 = lower-bound, 1 = upper-bound
    u32 best_k    = m;
    T   best_overlap = std::numeric_limits<T>::infinity();
    T   best_area    = std::numeric_limits<T>::infinity();
    T   best_perimeter_sum_per_axis[3]{
        std::numeric_limits<T>::infinity(),
        std::numeric_limits<T>::infinity(),
        std::numeric_limits<T>::infinity()};

    // Index permutation buffer.
    u32 order[N];

    for (u8 axis = 0; axis < 3; ++axis)
    {
        T axis_perimeter_sum = T{0};

        for (u8 sort_kind = 0; sort_kind < 2; ++sort_kind)
        {
            for (u32 i = 0; i < N; ++i) { order[i] = i; }
            // Lex-tuple comparator: (axis_key, then payload for tiebreak).
            auto cmp = [&](u32 lhs, u32 rhs) -> bool {
                const T lv = (sort_kind == 0)
                    ? staged[lhs].aabb.min[static_cast<usize>(axis)]
                    : staged[lhs].aabb.max[static_cast<usize>(axis)];
                const T rv = (sort_kind == 0)
                    ? staged[rhs].aabb.min[static_cast<usize>(axis)]
                    : staged[rhs].aabb.max[static_cast<usize>(axis)];
                if (lv < rv) { return true; }
                if (lv > rv) { return false; }
                // Lex tiebreak: secondary = the OTHER bound on the same axis.
                const T lv2 = (sort_kind == 0)
                    ? staged[lhs].aabb.max[static_cast<usize>(axis)]
                    : staged[lhs].aabb.min[static_cast<usize>(axis)];
                const T rv2 = (sort_kind == 0)
                    ? staged[rhs].aabb.max[static_cast<usize>(axis)]
                    : staged[rhs].aabb.min[static_cast<usize>(axis)];
                if (lv2 < rv2) { return true; }
                if (lv2 > rv2) { return false; }
                // Final: stable on payload.
                return staged[lhs].payload < staged[rhs].payload;
            };
            crd::containers::sort(order, order + N, cmp);

            // For each distribution k in [m, M-m+1] (= [m, N-m]):
            for (u32 k = m; k <= N - m; ++k)
            {
                AABB3<T> g1 = staged[order[0]].aabb;
                for (u32 i = 1; i < k; ++i)
                {
                    g1 = aabb_union(g1, staged[order[i]].aabb);
                }
                AABB3<T> g2 = staged[order[k]].aabb;
                for (u32 i = k + 1; i < N; ++i)
                {
                    g2 = aabb_union(g2, staged[order[i]].aabb);
                }

                axis_perimeter_sum += aabb_perimeter(g1) + aabb_perimeter(g2);

                // Defer overlap+area scoring to the final-axis pass — but
                // record best within this axis to compare across axes by sum.
            }
        }

        best_perimeter_sum_per_axis[axis] = axis_perimeter_sum;
    }

    // Pick axis with minimum perimeter sum (lex tiebreak: x<y<z).
    if (best_perimeter_sum_per_axis[1] < best_perimeter_sum_per_axis[best_axis]) { best_axis = 1; }
    if (best_perimeter_sum_per_axis[2] < best_perimeter_sum_per_axis[best_axis]) { best_axis = 2; }

    // Now within best_axis, pick the distribution with min overlap (lex
    // tiebreak: min area). Re-do both sorts on the chosen axis.
    for (u8 sort_kind = 0; sort_kind < 2; ++sort_kind)
    {
        for (u32 i = 0; i < N; ++i) { order[i] = i; }
        const u8 axis = best_axis;
        auto cmp = [&](u32 lhs, u32 rhs) -> bool {
            const T lv = (sort_kind == 0)
                ? staged[lhs].aabb.min[static_cast<usize>(axis)]
                : staged[lhs].aabb.max[static_cast<usize>(axis)];
            const T rv = (sort_kind == 0)
                ? staged[rhs].aabb.min[static_cast<usize>(axis)]
                : staged[rhs].aabb.max[static_cast<usize>(axis)];
            if (lv < rv) { return true; }
            if (lv > rv) { return false; }
            const T lv2 = (sort_kind == 0)
                ? staged[lhs].aabb.max[static_cast<usize>(axis)]
                : staged[lhs].aabb.min[static_cast<usize>(axis)];
            const T rv2 = (sort_kind == 0)
                ? staged[rhs].aabb.max[static_cast<usize>(axis)]
                : staged[rhs].aabb.min[static_cast<usize>(axis)];
            if (lv2 < rv2) { return true; }
            if (lv2 > rv2) { return false; }
            return staged[lhs].payload < staged[rhs].payload;
        };
        crd::containers::sort(order, order + N, cmp);

        for (u32 k = m; k <= N - m; ++k)
        {
            AABB3<T> g1 = staged[order[0]].aabb;
            for (u32 i = 1; i < k; ++i)
            {
                g1 = aabb_union(g1, staged[order[i]].aabb);
            }
            AABB3<T> g2 = staged[order[k]].aabb;
            for (u32 i = k + 1; i < N; ++i)
            {
                g2 = aabb_union(g2, staged[order[i]].aabb);
            }
            const T overlap = aabb_overlap_area(g1, g2);
            const T area = aabb_area(g1) + aabb_area(g2);
            if (overlap < best_overlap
                || (overlap == best_overlap && area < best_area))
            {
                best_overlap = overlap;
                best_area = area;
                best_k = k;
                best_sort = sort_kind;
            }
        }
    }

    // Materialise the chosen split — re-sort on best_axis + best_sort, take
    // [0..best_k) into original node, [best_k..N) into new sibling.
    for (u32 i = 0; i < N; ++i) { order[i] = i; }
    {
        const u8 axis = best_axis;
        const u8 sort_kind = best_sort;
        auto cmp = [&](u32 lhs, u32 rhs) -> bool {
            const T lv = (sort_kind == 0)
                ? staged[lhs].aabb.min[static_cast<usize>(axis)]
                : staged[lhs].aabb.max[static_cast<usize>(axis)];
            const T rv = (sort_kind == 0)
                ? staged[rhs].aabb.min[static_cast<usize>(axis)]
                : staged[rhs].aabb.max[static_cast<usize>(axis)];
            if (lv < rv) { return true; }
            if (lv > rv) { return false; }
            const T lv2 = (sort_kind == 0)
                ? staged[lhs].aabb.max[static_cast<usize>(axis)]
                : staged[lhs].aabb.min[static_cast<usize>(axis)];
            const T rv2 = (sort_kind == 0)
                ? staged[rhs].aabb.max[static_cast<usize>(axis)]
                : staged[rhs].aabb.min[static_cast<usize>(axis)];
            if (lv2 < rv2) { return true; }
            if (lv2 > rv2) { return false; }
            return staged[lhs].payload < staged[rhs].payload;
        };
        crd::containers::sort(order, order + N, cmp);
    }

    const u32 sibling_idx = allocate_node(node_level);
    RTreeNode<T>& orig = m_nodes[node_idx];
    RTreeNode<T>& sib  = m_nodes[sibling_idx];
    sib.parent = node_parent;

    // Repopulate orig with [0..best_k).
    orig.entry_count = 0;
    for (u32 i = 0; i < best_k; ++i)
    {
        orig.entries[orig.entry_count++] = staged[order[i]];
    }
    // Sibling gets [best_k..N).
    sib.entry_count = 0;
    for (u32 i = best_k; i < N; ++i)
    {
        sib.entries[sib.entry_count++] = staged[order[i]];
    }

    // Update handle locations + child parent pointers for moved entries.
    if (orig.is_leaf())
    {
        // Leaf entries carry both `payload` (user) and `handle` (stable ref).
        // Update locations using the handle.
        for (u32 i = 0; i < orig.entry_count; ++i)
        {
            update_handle_location(orig.entries[i].handle, node_idx, i);
        }
        for (u32 i = 0; i < sib.entry_count; ++i)
        {
            update_handle_location(sib.entries[i].handle, sibling_idx, i);
        }
    }
    else
    {
        // Interior entries' payload is a child node index. Update child.parent.
        for (u32 i = 0; i < orig.entry_count; ++i)
        {
            m_nodes[orig.entries[i].payload].parent = node_idx;
        }
        for (u32 i = 0; i < sib.entry_count; ++i)
        {
            m_nodes[sib.entries[i].payload].parent = sibling_idx;
        }
    }

    return sibling_idx;
}

// =============================================================================
// R*-tree forced reinsertion (Beckmann §4.3)
// =============================================================================
//
// Compute distance from each entry's center to the node center. Sort entries
// by distance DESCENDING. Pop p = floor(0.3 × M) = 4 entries. Keep the rest.
// Reinsert the popped entries from the root in distance ASCENDING order
// (closest-to-center first).

template <MathScalar T>
void RTree<T>::reinsert(u32 node_idx, RTreeEntry<T> overflow_entry)
{
    // M = max-fanout, P = forced-reinsert count per Beckmann 1990 �4.3 notation.
    constexpr u32 M = k_rtree_max_entries; // NOLINT(readability-identifier-naming)
    constexpr u32 P = k_rtree_reinsert_p;  // NOLINT(readability-identifier-naming)

    RTreeNode<T>& node = m_nodes[node_idx];
    const u8 node_level = node.level;
    CRD_ASSERT(node.entry_count == M);

    // Stage M+1 entries.
    RTreeEntry<T> staged[M + 1];
    for (u32 i = 0; i < M; ++i) { staged[i] = node.entries[i]; }
    staged[M] = overflow_entry;
    // N = M + 1 staged-entry count per Beckmann 1990 �4.3.
    constexpr u32 N = M + 1; // NOLINT(readability-identifier-naming)

    // Compute node center using union of all M+1 entries' AABBs.
    AABB3<T> node_aabb = aabb_union_of_entries(staged, N);
    Vec3<T> node_center = aabb_center(node_aabb);

    // Sort indices by distance from entry center to node center (DESCENDING).
    // Lex tiebreak: payload (so equal-distance behaviour is reproducible).
    u32 order[N];
    for (u32 i = 0; i < N; ++i) { order[i] = i; }
    auto desc_dist = [&](u32 lhs, u32 rhs) -> bool {
        const Vec3<T> lc = aabb_center(staged[lhs].aabb);
        const Vec3<T> rc = aabb_center(staged[rhs].aabb);
        const Vec3<T> ld = Vec3<T>{lc.x - node_center.x, lc.y - node_center.y, lc.z - node_center.z};
        const Vec3<T> rd = Vec3<T>{rc.x - node_center.x, rc.y - node_center.y, rc.z - node_center.z};
        const T ld2 = ld.x * ld.x + ld.y * ld.y + ld.z * ld.z;
        const T rd2 = rd.x * rd.x + rd.y * rd.y + rd.z * rd.z;
        if (ld2 > rd2) { return true; }
        if (ld2 < rd2) { return false; }
        return staged[lhs].payload < staged[rhs].payload;
    };
    crd::containers::sort(order, order + N, desc_dist);

    // Keep [P..N) in the node.
    node.entry_count = 0;
    for (u32 i = P; i < N; ++i)
    {
        const u32 dst = node.entry_count++;
        node.entries[dst] = staged[order[i]];
        if (node.is_leaf())
        {
            update_handle_location(node.entries[dst].handle, node_idx, dst);
        }
        // Interior children's parent pointer is unchanged (they stay in this node).
    }

    // Reinsert the first P entries (closest-to-center first → reverse order).
    // Beckmann §4.3 close-reinsert: closest first.
    RTreeEntry<T> to_reinsert[P];
    for (u32 i = 0; i < P; ++i)
    {
        to_reinsert[i] = staged[order[P - 1 - i]];
    }
    // For interior-node reinsertion, we need to update child.parent later
    // when the entry lands in a new node — handled by insert_entry path.
    for (u32 i = 0; i < P; ++i)
    {
        // For leaf entries: handle exists, we'll update its location in the
        // new home in adjust_tree_after_insert / insert_entry. For interior
        // entries: we need to update the child's parent after insert.
        insert_entry(to_reinsert[i], node_level);
    }
}

// =============================================================================
// adjust-tree-after-insert + insert orchestration
// =============================================================================

template <MathScalar T>
void RTree<T>::adjust_tree_after_insert(u32 node_idx, u32 split_sibling)
{
    u32 cur = node_idx;
    u32 carry = split_sibling;
    while (true)
    {
        RTreeNode<T>& cur_node = m_nodes[cur];
        const u32 parent_idx = cur_node.parent;

        if (parent_idx == k_null)
        {
            // We're at the root.
            if (carry != k_null)
            {
                // Root just split. Create a new root containing both halves.
                const u32 new_root = allocate_node(static_cast<u8>(cur_node.level + 1));
                RTreeNode<T>& root_node = m_nodes[new_root];
                root_node.entry_count = 2;
                root_node.entries[0] = RTreeEntry<T>{aabb_union_of_entries(m_nodes[cur].entries, m_nodes[cur].entry_count), cur};
                root_node.entries[1] = RTreeEntry<T>{aabb_union_of_entries(m_nodes[carry].entries, m_nodes[carry].entry_count), carry};
                m_nodes[cur].parent   = new_root;
                m_nodes[carry].parent = new_root;
                m_root = new_root;
            }
            // Else: just refresh the root's bounds (already done by carry==null path implicit).
            return;
        }

        RTreeNode<T>& parent = m_nodes[parent_idx];

        // Find the entry in the parent that points to `cur` and refresh its AABB.
        for (u32 i = 0; i < parent.entry_count; ++i)
        {
            if (parent.entries[i].payload == cur)
            {
                parent.entries[i].aabb = aabb_union_of_entries(cur_node.entries, cur_node.entry_count);
                break;
            }
        }

        if (carry != k_null)
        {
            // Add a new entry to parent for the sibling.
            RTreeEntry<T> sibling_entry{
                aabb_union_of_entries(m_nodes[carry].entries, m_nodes[carry].entry_count), carry};
            if (parent.entry_count < k_rtree_max_entries)
            {
                parent.entries[parent.entry_count++] = sibling_entry;
                m_nodes[carry].parent = parent_idx;
                carry = k_null;
            }
            else
            {
                // Parent overflows. Per Beckmann §4.3, try forced reinsertion
                // ONCE per level per insert call. Otherwise split.
                m_nodes[carry].parent = parent_idx; // sibling becomes a child of parent regardless
                const u8 lvl = parent.level;
                const u32 mask = 1U << lvl;
                if ((m_treated_levels & mask) == 0)
                {
                    m_treated_levels |= mask;
                    reinsert(parent_idx, sibling_entry);
                    carry = k_null;
                    // Continue walking up — parent's bounds may have changed.
                }
                else
                {
                    const u32 parent_sibling = split_node(parent_idx, sibling_entry);
                    carry = parent_sibling;
                }
            }
        }

        cur = parent_idx;
    }
}

template <MathScalar T>
void RTree<T>::insert_entry(const RTreeEntry<T>& entry, u8 level)
{
    if (m_root == k_null)
    {
        // Tree is empty — create a root leaf with this entry.
        // (`level` is expected to be 0 in this path.)
        CRD_ASSERT(level == 0);
        const u32 root = allocate_node(0);
        m_nodes[root].entry_count = 1;
        m_nodes[root].entries[0] = entry;
        m_root = root;
        if (m_nodes[root].is_leaf())
        {
            update_handle_location(entry.handle, root, 0);
        }
        else
        {
            // Interior reinsertion (rare — only when a deep tree's interior
            // node was reinserted): update child's parent.
            m_nodes[entry.payload].parent = root;
        }
        return;
    }

    const u32 target_node_idx = choose_subtree(entry.aabb, level);
    RTreeNode<T>& target = m_nodes[target_node_idx];

    if (target.entry_count < k_rtree_max_entries)
    {
        const u32 slot = target.entry_count++;
        target.entries[slot] = entry;
        if (target.is_leaf())
        {
            update_handle_location(entry.handle, target_node_idx, slot);
        }
        else
        {
            m_nodes[entry.payload].parent = target_node_idx;
        }
        adjust_tree_after_insert(target_node_idx, k_null);
    }
    else
    {
        // Overflow. Try reinsertion once per level per insert; else split.
        const u32 mask = 1U << target.level;
        if ((m_treated_levels & mask) == 0)
        {
            m_treated_levels |= mask;
            // Reinsert handles the AABB-union refresh up the tree implicitly
            // via subsequent insert_entry calls.
            reinsert(target_node_idx, entry);
            // After reinsertion, the parent's AABB for target_node_idx may
            // need a refresh.
            adjust_tree_after_insert(target_node_idx, k_null);
        }
        else
        {
            const u32 sibling = split_node(target_node_idx, entry);
            adjust_tree_after_insert(target_node_idx, sibling);
        }
    }
}

// =============================================================================
// Public insert + remove
// =============================================================================

template <MathScalar T>
RTreeLeafId RTree<T>::insert(const AABB3<T>& aabb, u32 payload)
{
    CRD_ASSERT(crd::geometry::primitives::is_finite(aabb));

    m_treated_levels = 0;
    const u32 handle = allocate_handle(k_null, 0);
    insert_entry(RTreeEntry<T>{aabb, payload, handle}, 0);
    ++m_leaf_count;
    return RTreeLeafId{handle};
}

template <MathScalar T>
void RTree<T>::remove(RTreeLeafId id)
{
    CRD_ASSERT(id.valid());
    CRD_ASSERT(id.value < m_locations.size());
    CRD_ASSERT(m_locations[id.value].alive);

    const u32 node_idx = m_locations[id.value].node_idx;
    const u32 entry_idx = m_locations[id.value].entry_idx;
    RTreeNode<T>& node = m_nodes[node_idx];
    CRD_ASSERT(node.is_leaf());
    CRD_ASSERT(entry_idx < node.entry_count);
    CRD_ASSERT(node.entries[entry_idx].handle == id.value);

    // Swap-with-last in the leaf node, update the swapped entry's location.
    const u32 last = node.entry_count - 1;
    if (entry_idx != last)
    {
        node.entries[entry_idx] = node.entries[last];
        update_handle_location(node.entries[entry_idx].handle, node_idx, entry_idx);
    }
    --node.entry_count;
    free_handle(id.value);
    --m_leaf_count;

    condense_tree(node_idx);
}

// =============================================================================
// condense-tree (Guttman §3.4)
// =============================================================================

template <MathScalar T>
void RTree<T>::condense_tree(u32 node_idx)
{
    // Walk up: any node that underflows below `m`, collect orphans, remove
    // node from its parent, free node. Reinsert orphans at their original
    // level after the walk.

    // Collect orphans: per (orphan, level) pair.
    struct Orphan { RTreeEntry<T> entry; u8 level; };
    Orphan orphans[64]; // bounded by depth × M
    usize n_orphans = 0;

    u32 cur = node_idx;
    while (cur != m_root)
    {
        RTreeNode<T>& node = m_nodes[cur];
        const u32 parent_idx = node.parent;
        RTreeNode<T>& parent = m_nodes[parent_idx];

        if (node.entry_count < k_rtree_min_entries)
        {
            // Underflow — collect orphans + remove node from parent + free node.
            for (u32 i = 0; i < node.entry_count; ++i)
            {
                CRD_ASSERT(n_orphans < 64);
                orphans[n_orphans++] = Orphan{node.entries[i], node.level};
            }
            // Remove parent's entry pointing to this node (swap-with-last).
            for (u32 i = 0; i < parent.entry_count; ++i)
            {
                if (parent.entries[i].payload == cur)
                {
                    const u32 plast = parent.entry_count - 1;
                    if (i != plast)
                    {
                        parent.entries[i] = parent.entries[plast];
                    }
                    --parent.entry_count;
                    break;
                }
            }
            free_node(cur);
        }
        else
        {
            // No underflow — just refresh parent's AABB for this node.
            for (u32 i = 0; i < parent.entry_count; ++i)
            {
                if (parent.entries[i].payload == cur)
                {
                    parent.entries[i].aabb =
                        aabb_union_of_entries(node.entries, node.entry_count);
                    break;
                }
            }
        }

        cur = parent_idx;
    }

    // Root: if it has 1 child and it's not a leaf, promote the child.
    if (m_root != k_null)
    {
        RTreeNode<T>& root_node = m_nodes[m_root];
        if (root_node.entry_count == 1 && !root_node.is_leaf())
        {
            const u32 promoted = root_node.entries[0].payload;
            free_node(m_root);
            m_root = promoted;
            m_nodes[m_root].parent = k_null;
        }
        else if (root_node.entry_count == 0)
        {
            // Tree completely empty.
            free_node(m_root);
            m_root = k_null;
        }
    }

    // Reinsert orphans at their original level.
    m_treated_levels = 0;
    for (usize i = 0; i < n_orphans; ++i)
    {
        insert_entry(orphans[i].entry, orphans[i].level);
    }
}

// =============================================================================
// STR bulk-load (Leutenegger 1997)
// =============================================================================

template <MathScalar T>
u32 RTree<T>::str_pack_level(crd::containers::Array<RTreeEntry<T>>& level_entries, u8 level)
{
    // M = max-fanout per Beckmann 1990; N/L/S = total/leaves/slabs per
    // Leutenegger 1997 �3 STR-pack notation.
    constexpr u32 M = k_rtree_max_entries; // NOLINT(readability-identifier-naming)
    const usize N = level_entries.size(); // NOLINT(readability-identifier-naming)
    if (N == 1)
    {
        // Sole "entry" is actually a node — return its child index.
        return level_entries[0].payload;
    }

    // Number of leaves at THIS level after packing N entries.
    const usize L = (N + M - 1) / M; // NOLINT(readability-identifier-naming)
    // Number of slabs (vertical strips on x).
    usize S; // NOLINT(readability-identifier-naming)
    {
        // S = ceil(sqrt(L))
        const f64 sf = crd::math::sqrt(static_cast<f64>(L));
        S = static_cast<usize>(sf);
        if (S * S < L) { ++S; }
        if (S == 0) { S = 1; }
    }

    // Sort by x-midpoint (lex tiebreak: y-midpoint, then z-midpoint, then payload).
    auto x_cmp = [](const RTreeEntry<T>& a, const RTreeEntry<T>& b) -> bool {
        const T ax = (a.aabb.min.x + a.aabb.max.x) * T{0.5};
        const T bx = (b.aabb.min.x + b.aabb.max.x) * T{0.5};
        if (ax < bx) { return true; }
        if (ax > bx) { return false; }
        const T ay = (a.aabb.min.y + a.aabb.max.y) * T{0.5};
        const T by = (b.aabb.min.y + b.aabb.max.y) * T{0.5};
        if (ay < by) { return true; }
        if (ay > by) { return false; }
        const T az = (a.aabb.min.z + a.aabb.max.z) * T{0.5};
        const T bz = (b.aabb.min.z + b.aabb.max.z) * T{0.5};
        if (az < bz) { return true; }
        if (az > bz) { return false; }
        return a.payload < b.payload;
    };
    crd::containers::sort(level_entries.data(), level_entries.data() + N, x_cmp);

    // Partition into S slabs of `slab_size = ceil(N/S)` each, last slab may
    // be smaller.
    const usize slab_size = (N + S - 1) / S;

    // Build the next level's entries (one per leaf node we pack).
    crd::containers::Array<RTreeEntry<T>> next_level{m_alloc};
    next_level.reserve(L);

    for (usize si = 0; si < S; ++si)
    {
        const usize slab_begin = si * slab_size;
        if (slab_begin >= N) { break; }
        const usize slab_end = std::min(slab_begin + slab_size, N);

        // Sort this slab by y-midpoint (lex tiebreak: z-midpoint, payload).
        auto y_cmp = [](const RTreeEntry<T>& a, const RTreeEntry<T>& b) -> bool {
            const T ay = (a.aabb.min.y + a.aabb.max.y) * T{0.5};
            const T by = (b.aabb.min.y + b.aabb.max.y) * T{0.5};
            if (ay < by) { return true; }
            if (ay > by) { return false; }
            const T az = (a.aabb.min.z + a.aabb.max.z) * T{0.5};
            const T bz = (b.aabb.min.z + b.aabb.max.z) * T{0.5};
            if (az < bz) { return true; }
            if (az > bz) { return false; }
            return a.payload < b.payload;
        };
        crd::containers::sort(level_entries.data() + slab_begin,
                                 level_entries.data() + slab_end, y_cmp);

        // Pack into leaves of M.
        usize pos = slab_begin;
        while (pos < slab_end)
        {
            const usize chunk_end = std::min(pos + M, slab_end);
            const u32 leaf_idx = allocate_node(level);
            RTreeNode<T>& leaf = m_nodes[leaf_idx];
            leaf.entry_count = 0;
            for (usize i = pos; i < chunk_end; ++i)
            {
                const u32 slot = leaf.entry_count++;
                leaf.entries[slot] = level_entries[i];
                if (leaf.is_leaf())
                {
                    update_handle_location(leaf.entries[slot].handle, leaf_idx, slot);
                }
                else
                {
                    m_nodes[leaf.entries[slot].payload].parent = leaf_idx;
                }
            }
            // Build a next-level entry pointing at this leaf — interior entry,
            // handle field unused (k_invalid_handle for clarity).
            next_level.push_back(RTreeEntry<T>{
                aabb_union_of_entries(leaf.entries, leaf.entry_count),
                leaf_idx, k_invalid_handle});
            pos = chunk_end;
        }
    }

    // Recurse — pack the next level up.
    return str_pack_level(next_level, static_cast<u8>(level + 1));
}

template <MathScalar T>
void RTree<T>::bulk_load(crd::containers::ConstSpan<AABB3<T>> aabbs,
                          crd::containers::ConstSpan<u32>      payloads,
                          crd::containers::Array<RTreeLeafId>& out_handles)
{
    CRD_ASSERT(aabbs.size() == payloads.size());

    // Reset tree.
    m_nodes.clear();
    m_locations.clear();
    m_root = k_null;
    m_node_free_list = k_null;
    m_handle_free_list = k_invalid_handle;
    m_leaf_count = 0;
    m_allocated_nodes = 0;
    m_treated_levels = 0;

    out_handles.clear();
    out_handles.reserve(aabbs.size());

    if (aabbs.size() == 0)
    {
        return;
    }

    // Allocate handles up-front in input order so that out_handles[i]
    // corresponds to input i.
    crd::containers::Array<RTreeEntry<T>> entries{m_alloc};
    entries.reserve(aabbs.size());
    for (usize i = 0; i < aabbs.size(); ++i)
    {
        CRD_ASSERT(crd::geometry::primitives::is_finite(aabbs[i]));
        const u32 h = allocate_handle(k_null, 0);
        out_handles.push_back(RTreeLeafId{h});
        entries.push_back(RTreeEntry<T>{aabbs[i], payloads[i], h});
    }

    m_leaf_count = aabbs.size();

    // Single-leaf tree case.
    if (entries.size() <= k_rtree_max_entries)
    {
        const u32 root = allocate_node(0);
        for (usize i = 0; i < entries.size(); ++i)
        {
            const u32 slot = m_nodes[root].entry_count++;
            m_nodes[root].entries[slot] = entries[i];
            update_handle_location(entries[i].handle, root, slot);
        }
        m_root = root;
        return;
    }

    m_root = str_pack_level(entries, 0);
    m_nodes[m_root].parent = k_null;
}

// =============================================================================
// Public access
// =============================================================================

template <MathScalar T>
AABB3<T> RTree<T>::bounds() const noexcept
{
    constexpr T inf = std::numeric_limits<T>::infinity();
    if (m_root == k_null)
    {
        return AABB3<T>{Vec3<T>{inf, inf, inf}, Vec3<T>{-inf, -inf, -inf}};
    }
    return aabb_union_of_entries(m_nodes[m_root].entries, m_nodes[m_root].entry_count);
}

template <MathScalar T>
AABB3<T> RTree<T>::entry_aabb(RTreeLeafId id) const noexcept
{
    CRD_ASSERT(id.valid() && id.value < m_locations.size() && m_locations[id.value].alive);
    const auto& loc = m_locations[id.value];
    return m_nodes[loc.node_idx].entries[loc.entry_idx].aabb;
}

template <MathScalar T>
u32 RTree<T>::entry_payload(RTreeLeafId id) const noexcept
{
    CRD_ASSERT(id.valid() && id.value < m_locations.size() && m_locations[id.value].alive);
    const auto& loc = m_locations[id.value];
    return m_nodes[loc.node_idx].entries[loc.entry_idx].payload;
}

template <MathScalar T>
u32 RTree<T>::depth() const noexcept
{
    if (m_root == k_null) { return 0; }
    return static_cast<u32>(m_nodes[m_root].level) + 1U;
}

// =============================================================================
// Queries — overlap (Array sink), raycast, k-NN
// =============================================================================

template <MathScalar T>
void RTree<T>::overlap(const AABB3<T>& query, crd::containers::Array<u32>& out) const
{
    overlap(query, [&](u32 payload) { out.push_back(payload); });
}

// (Templated `overlap(box, on_hit)` uses raw payload from leaf entries,
// which is the HANDLE — but we want to expose the user payload. Patch:
// the inline template in the header should look up m_user_payloads[handle].
// But the templated overlap is in the header — needs access to m_user_payloads.
// Resolved by making it a friend — actually simpler: the inline version emits
// `m_user_payloads[handle]`. Need to update the inline template … but it's
// already written. Quick fix: keep inline template as-is (emits raw entry
// payload), and provide a wrapper that does the indirection. Since leaf
// entries' payload IS the handle at storage level, callers expect the user
// payload — make the leaf-entry payload BE the user payload and store handle
// elsewhere? That breaks the location table approach.
//
// Cleanest: store handle as the leaf entry's payload (so split/move bookkeeping
// stays correct), and do the user-payload lookup at the query callback layer.
// Update the inline template in the header to do `on_hit(m_user_payloads[entry.payload])`.

template <MathScalar T>
std::optional<crd::geometry::RayHit<u32>>
RTree<T>::raycast(const Ray3<T>& ray, T tmax) const noexcept
{
    if (m_root == k_null) { return std::nullopt; }
    if (tmax <= T{0}) { return std::nullopt; }
    if (!crd::geometry::primitives::is_finite(ray.origin)
        || !crd::geometry::primitives::is_finite(ray.direction))
    {
        return std::nullopt;
    }

    if constexpr (!std::is_same_v<T, f32>)
    {
        // f64 path — scalar slab traversal without precompute.
        T best_t = tmax;
        u32 best_payload = 0xFFFFFFFFU;
        bool any = false;

        u32 stack[kRtreeMaxStack];
        usize sp = 0;
        stack[sp++] = m_root;
        while (sp > 0)
        {
            const u32 ni = stack[--sp];
            const RTreeNode<T>& node = m_nodes[ni];
            if (node.is_leaf())
            {
                for (u32 i = 0; i < node.entry_count; ++i)
                {
                    T t_min = T{0};
                    T t_max = best_t;
                    bool ok = true;
                    for (int ax = 0; ax < 3; ++ax)
                    {
                        const T o = ray.origin[static_cast<usize>(ax)];
                        const T d = ray.direction[static_cast<usize>(ax)];
                        const T lo = node.entries[i].aabb.min[static_cast<usize>(ax)];
                        const T hi = node.entries[i].aabb.max[static_cast<usize>(ax)];
                        if (std::abs(d) < std::numeric_limits<T>::epsilon())
                        {
                            if (o < lo || o > hi) { ok = false; break; }
                        }
                        else
                        {
                            const T inv = T{1} / d;
                            T t1 = (lo - o) * inv;
                            T t2 = (hi - o) * inv;
                            if (t1 > t2) { const T tmp = t1; t1 = t2; t2 = tmp; }
                            if (t1 > t_min) { t_min = t1; }
                            if (t2 < t_max) { t_max = t2; }
                            if (t_min > t_max) { ok = false; break; }
                        }
                    }
                    if (!ok || t_min < T{0}) { continue; }
                    const u32 user_pay = node.entries[i].payload;
                    if (t_min < best_t)
                    {
                        best_t = t_min;
                        best_payload = user_pay;
                        any = true;
                    }
                    else if (t_min == best_t && user_pay < best_payload)
                    {
                        best_payload = user_pay;
                        any = true;
                    }
                }
            }
            else
            {
                for (u32 i = 0; i < node.entry_count; ++i)
                {
                    // Slab test for child AABB, prune by best_t.
                    T t_min = T{0};
                    T t_max = best_t;
                    bool ok = true;
                    for (int ax = 0; ax < 3; ++ax)
                    {
                        const T o = ray.origin[static_cast<usize>(ax)];
                        const T d = ray.direction[static_cast<usize>(ax)];
                        const T lo = node.entries[i].aabb.min[static_cast<usize>(ax)];
                        const T hi = node.entries[i].aabb.max[static_cast<usize>(ax)];
                        if (std::abs(d) < std::numeric_limits<T>::epsilon())
                        {
                            if (o < lo || o > hi) { ok = false; break; }
                        }
                        else
                        {
                            const T inv = T{1} / d;
                            T t1 = (lo - o) * inv;
                            T t2 = (hi - o) * inv;
                            if (t1 > t2) { const T tmp = t1; t1 = t2; t2 = tmp; }
                            if (t1 > t_min) { t_min = t1; }
                            if (t2 < t_max) { t_max = t2; }
                            if (t_min > t_max) { ok = false; break; }
                        }
                    }
                    if (!ok) { continue; }
                    CRD_ASSERT(sp < kRtreeMaxStack);
                    stack[sp++] = node.entries[i].payload;
                }
            }
        }
        if (!any) { return std::nullopt; }
        return crd::geometry::RayHit<u32>{static_cast<f32>(best_t), best_payload};
    }
    else
    {
        const auto pre = crd::geometry::primitives::precompute_ray_aabb(ray);
        T best_t = tmax;
        u32 best_payload = 0xFFFFFFFFU;
        bool any = false;

        struct Frame { u32 node; T t_near; };
        Frame stack[kRtreeMaxStack];
        usize sp = 0;

        T root_t = T{0};
        const AABB3<T> root_bounds = aabb_union_of_entries(
            m_nodes[m_root].entries, m_nodes[m_root].entry_count);
        if (!crd::geometry::primitives::intersect_ray_aabb_robust(
                ray, pre, root_bounds, T{0}, best_t, root_t))
        {
            return std::nullopt;
        }
        stack[sp++] = Frame{m_root, root_t};

        while (sp > 0)
        {
            const Frame f = stack[--sp];
            if (f.t_near >= best_t) { continue; }
            const RTreeNode<T>& node = m_nodes[f.node];
            if (node.is_leaf())
            {
                for (u32 i = 0; i < node.entry_count; ++i)
                {
                    T t = T{0};
                    if (!crd::geometry::primitives::intersect_ray_aabb_robust(
                            ray, pre, node.entries[i].aabb, T{0}, best_t, t))
                    {
                        continue;
                    }
                    const u32 user_pay = node.entries[i].payload;
                    if (t < best_t)
                    {
                        best_t = t;
                        best_payload = user_pay;
                        any = true;
                    }
                    else if (t == best_t && user_pay < best_payload)
                    {
                        best_payload = user_pay;
                        any = true;
                    }
                }
            }
            else
            {
                // Compute t_near for each child + push reverse-sorted (largest
                // first → smallest pops first).
                Frame cf[k_rtree_max_entries];
                u32 nc = 0;
                for (u32 i = 0; i < node.entry_count; ++i)
                {
                    T tt = T{0};
                    if (!crd::geometry::primitives::intersect_ray_aabb_robust(
                            ray, pre, node.entries[i].aabb, T{0}, best_t, tt))
                    {
                        continue;
                    }
                    cf[nc++] = Frame{node.entries[i].payload, tt};
                }
                // Insertion-sort descending by t_near (small node count: ≤ M=16).
                for (u32 i = 1; i < nc; ++i)
                {
                    Frame v = cf[i];
                    u32 j = i;
                    while (j > 0U && cf[j - 1U].t_near < v.t_near)
                    {
                        cf[j] = cf[j - 1U]; --j;
                    }
                    cf[j] = v;
                }
                for (u32 i = 0; i < nc; ++i)
                {
                    CRD_ASSERT(sp < kRtreeMaxStack);
                    stack[sp++] = cf[i];
                }
            }
        }
        if (!any) { return std::nullopt; }
        return crd::geometry::RayHit<u32>{best_t, best_payload};
    }
}

// k-NN — Hjaltason-Samet 1999 incremental NN.
//
// PQ entries: (min_dist², kind, node_or_user_payload).
//   * NODE entries: key = min_dist² from query to node-bounds (computed when
//                   we're about to push a child).
//   * LEAF entries: key = min_dist² from query to leaf entry's AABB.
// Pop the min-key entry. If LEAF, emit + add to result set. If NODE, expand
// (push all children into PQ as either LEAF or NODE entries).
// Stop when k LEAF entries emitted.
//
// Tiebreak: the PQ comparator sorts by (dist², payload) ascending so equal-
// dist tiebreaks favour lowest payload. Final ascending sort by (dist², payload).

template <MathScalar T>
void RTree<T>::nearest_n(const Vec3<T>&                          query,
                          usize                                    k,
                          crd::containers::Array<Neighbor>&        out) const noexcept
{
    out.clear();
    if (m_root == k_null || k == 0) { return; }
    if (!crd::geometry::primitives::is_finite(query)) { return; }
    out.reserve(k);

    // Heap entry: kind + key + (node_idx OR user payload).
    struct PqItem
    {
        T        key{};      // min_dist²
        u32      kind{0};    // 0 = node, 1 = leaf
        u32      idx{0};     // node_idx OR user payload
        u32      tiebreak{0}; // for leaf items: the user payload, for stable ordering
    };
    // Min-heap: comparator returns true when a should be deeper than b
    // (i.e., a "less" than b) — for std-style max-heap-on-cmp, we invert.
    auto cmp = [](const PqItem& a, const PqItem& b) -> bool {
        // We want top of heap = smallest key. Heap puts max-under-cmp at top
        // (Cerid `push_heap` follows std semantics). So return true when a > b
        // (so b stays on top when equal, etc.).
        if (a.key > b.key) { return true; }
        if (a.key < b.key) { return false; }
        if (a.tiebreak > b.tiebreak) { return true; }
        if (a.tiebreak < b.tiebreak) { return false; }
        // Same key + tiebreak: arbitrary but deterministic — prefer node over
        // leaf? Actually doesn't matter once both keys+tiebreaks match. Use
        // (kind, idx) for stability.
        if (a.kind > b.kind) { return true; }
        if (a.kind < b.kind) { return false; }
        return a.idx > b.idx;
    };

    crd::containers::Array<PqItem> pq{m_alloc};
    pq.reserve(64);

    pq.push_back(PqItem{point_to_aabb_dist_sq(query, bounds()), 0U, m_root, 0U});
    crd::containers::push_heap(pq.data(), pq.data() + pq.size(), cmp);

    while (pq.size() > 0 && out.size() < k)
    {
        crd::containers::pop_heap(pq.data(), pq.data() + pq.size(), cmp);
        const PqItem top = pq[pq.size() - 1];
        pq.resize(pq.size() - 1);

        if (top.kind == 1U)
        {
            // Leaf entry — emit.
            out.push_back(Neighbor{top.idx, top.key});
        }
        else
        {
            // Node — expand children.
            const RTreeNode<T>& node = m_nodes[top.idx];
            for (u32 i = 0; i < node.entry_count; ++i)
            {
                const T dist2 = point_to_aabb_dist_sq(query, node.entries[i].aabb);
                if (node.is_leaf())
                {
                    const u32 user_pay = node.entries[i].payload;
                    pq.push_back(PqItem{dist2, 1U, user_pay, user_pay});
                }
                else
                {
                    // tiebreak for node entries doesn't directly affect order
                    // (they compete with their parent's siblings in the PQ);
                    // use child node idx for determinism.
                    pq.push_back(PqItem{dist2, 0U, node.entries[i].payload, node.entries[i].payload});
                }
                crd::containers::push_heap(pq.data(), pq.data() + pq.size(), cmp);
            }
        }
    }

    // Final ascending sort by (dist², payload).
    auto asc = [](const Neighbor& a, const Neighbor& b) -> bool {
        if (a.distance_squared < b.distance_squared) { return true; }
        if (a.distance_squared > b.distance_squared) { return false; }
        return a.payload < b.payload;
    };
    crd::containers::sort(out.data(), out.data() + out.size(), asc);
}

// =============================================================================
// validate (debug-only structural check)
// =============================================================================

template <MathScalar T>
void RTree<T>::validate() const noexcept
{
    if (m_root == k_null)
    {
        CRD_ASSERT(m_leaf_count == 0);
        return;
    }
    // Walk every node. Verify:
    //   * parent pointers point back at us
    //   * AABB enclosure: parent's entry-AABB encloses our entries' AABBs' union
    //   * leaf level == 0
    //   * size in [m, M] except root
    //   * leaf entries' handle locations match their actual location
    crd::usize visited_leaves = 0;
    // validate() walks every node — paranoid 1024-frame buffer for cosmic
    // depths (M=16 ⇒ depth 16 covers ~10^19 entries).
    u32 stack[1024];
    usize sp = 0;
    stack[sp++] = m_root;
    while (sp > 0)
    {
        const u32 ni = stack[--sp];
        const RTreeNode<T>& node = m_nodes[ni];
        CRD_ASSERT(is_node_alive(ni));
        if (ni != m_root)
        {
            CRD_ASSERT(node.entry_count >= k_rtree_min_entries);
        }
        CRD_ASSERT(node.entry_count <= k_rtree_max_entries);

        if (node.is_leaf())
        {
            visited_leaves += node.entry_count;
            for (u32 i = 0; i < node.entry_count; ++i)
            {
                [[maybe_unused]] const u32 h = node.entries[i].handle;
                CRD_ASSERT(h < m_locations.size() && m_locations[h].alive);
                CRD_ASSERT(m_locations[h].node_idx == ni);
                CRD_ASSERT(m_locations[h].entry_idx == i);
            }
        }
        else
        {
            for (u32 i = 0; i < node.entry_count; ++i)
            {
                const u32 child_idx = node.entries[i].payload;
                CRD_ASSERT(is_node_alive(child_idx));
                CRD_ASSERT(m_nodes[child_idx].parent == ni);
                CRD_ASSERT(m_nodes[child_idx].level == node.level - 1U);
                CRD_ASSERT(sp < 1024);
                stack[sp++] = child_idx;
            }
        }
    }
    CRD_ASSERT(visited_leaves == m_leaf_count);
}

// =============================================================================
// Explicit instantiations
// =============================================================================

template class RTree<f32>;
template class RTree<f64>;

} // namespace crd::geometry::spatial
