#include "aabb_ops.hpp"

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/dynamic_bvh.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/math/vec.hpp>

#include <algorithm>
#include <limits>

namespace crd::geometry::bvh
{
namespace
{
using crd::f32;
using crd::i32;
using crd::u32;
using crd::usize;
using crd::math::Vec3;
using detail::aabb_half_area;

[[nodiscard]] AABB3<f32> aabb_union(const AABB3<f32>& a, const AABB3<f32>& b) noexcept
{
    return AABB3<f32>(Vec3<f32>(std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)),
                      Vec3<f32>(std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)));
}

[[nodiscard]] AABB3<f32> aabb_inflate(const AABB3<f32>& a, f32 m) noexcept
{
    return AABB3<f32>(Vec3<f32>(a.min.x - m, a.min.y - m, a.min.z - m),
                      Vec3<f32>(a.max.x + m, a.max.y + m, a.max.z + m));
}

[[nodiscard]] bool aabb_contains(const AABB3<f32>& outer, const AABB3<f32>& inner) noexcept
{
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

} // namespace

// ---- node pool ------------------------------------------------------------

u32 DynamicBvh::allocate_node()
{
    if (m_free_list != k_null)
    {
        const u32 i = m_free_list;
        m_free_list = m_nodes[i].next_free;
        m_nodes[i] = Node{};
        return i;
    }
    m_nodes.push_back(Node{});
    return static_cast<u32>(m_nodes.size() - 1);
}

void DynamicBvh::free_node(u32 i)
{
    m_nodes[i] = Node{}; // height stays -1 (free), parent/children k_null
    m_nodes[i].next_free = m_free_list;
    m_free_list = i;
}

void DynamicBvh::fit_node_to_children(u32 i) noexcept
{
    Node& n = m_nodes[i];
    const Node& c1 = m_nodes[n.child1];
    const Node& c2 = m_nodes[n.child2];
    n.aabb = aabb_union(c1.aabb, c2.aabb);
    n.height = 1 + (c1.height > c2.height ? c1.height : c2.height);
}

// ---- balance: height-based single rotation rooted at iA -------------------

u32 DynamicBvh::balance(u32 i_a)
{
    Node& a = m_nodes[i_a];
    if (a.is_leaf() || a.height < 2)
    {
        return i_a;
    }
    const u32 i_b = a.child1;
    const u32 i_c = a.child2;
    const i32 delta = m_nodes[i_c].height - m_nodes[i_b].height;

    // Rotate child C up.
    if (delta > 1)
    {
        Node& c = m_nodes[i_c];
        const u32 i_f = c.child1;
        const u32 i_g = c.child2;
        c.child1 = i_a;
        c.parent = a.parent;
        a.parent = i_c;
        if (c.parent != k_null)
        {
            if (m_nodes[c.parent].child1 == i_a)
            {
                m_nodes[c.parent].child1 = i_c;
            }
            else
            {
                m_nodes[c.parent].child2 = i_c;
            }
        }
        else
        {
            m_root = i_c;
        }
        if (m_nodes[i_f].height > m_nodes[i_g].height)
        {
            c.child2 = i_f;
            a.child2 = i_g;
            m_nodes[i_g].parent = i_a;
            a.aabb = aabb_union(m_nodes[i_b].aabb, m_nodes[i_g].aabb);
            c.aabb = aabb_union(a.aabb, m_nodes[i_f].aabb);
            a.height = 1 + (m_nodes[i_b].height > m_nodes[i_g].height ? m_nodes[i_b].height : m_nodes[i_g].height);
            c.height = 1 + (a.height > m_nodes[i_f].height ? a.height : m_nodes[i_f].height);
        }
        else
        {
            c.child2 = i_g;
            a.child2 = i_f;
            m_nodes[i_f].parent = i_a;
            a.aabb = aabb_union(m_nodes[i_b].aabb, m_nodes[i_f].aabb);
            c.aabb = aabb_union(a.aabb, m_nodes[i_g].aabb);
            a.height = 1 + (m_nodes[i_b].height > m_nodes[i_f].height ? m_nodes[i_b].height : m_nodes[i_f].height);
            c.height = 1 + (a.height > m_nodes[i_g].height ? a.height : m_nodes[i_g].height);
        }
        return i_c;
    }

    // Rotate child B up (mirror).
    if (delta < -1)
    {
        Node& b = m_nodes[i_b];
        const u32 i_d = b.child1;
        const u32 i_e = b.child2;
        b.child1 = i_a;
        b.parent = a.parent;
        a.parent = i_b;
        if (b.parent != k_null)
        {
            if (m_nodes[b.parent].child1 == i_a)
            {
                m_nodes[b.parent].child1 = i_b;
            }
            else
            {
                m_nodes[b.parent].child2 = i_b;
            }
        }
        else
        {
            m_root = i_b;
        }
        if (m_nodes[i_d].height > m_nodes[i_e].height)
        {
            b.child2 = i_d;
            a.child1 = i_e;
            m_nodes[i_e].parent = i_a;
            a.aabb = aabb_union(m_nodes[i_c].aabb, m_nodes[i_e].aabb);
            b.aabb = aabb_union(a.aabb, m_nodes[i_d].aabb);
            a.height = 1 + (m_nodes[i_c].height > m_nodes[i_e].height ? m_nodes[i_c].height : m_nodes[i_e].height);
            b.height = 1 + (a.height > m_nodes[i_d].height ? a.height : m_nodes[i_d].height);
        }
        else
        {
            b.child2 = i_e;
            a.child1 = i_d;
            m_nodes[i_d].parent = i_a;
            a.aabb = aabb_union(m_nodes[i_c].aabb, m_nodes[i_d].aabb);
            b.aabb = aabb_union(a.aabb, m_nodes[i_e].aabb);
            a.height = 1 + (m_nodes[i_c].height > m_nodes[i_d].height ? m_nodes[i_c].height : m_nodes[i_d].height);
            b.height = 1 + (a.height > m_nodes[i_e].height ? a.height : m_nodes[i_e].height);
        }
        return i_b;
    }

    return i_a;
}

// ---- insert / remove ------------------------------------------------------

void DynamicBvh::insert_leaf(u32 leaf)
{
    if (m_root == k_null)
    {
        m_root = leaf;
        m_nodes[leaf].parent = k_null;
        return;
    }

    const AABB3<f32> leaf_aabb = m_nodes[leaf].aabb;

    // Greedy best-sibling descent (Box2D-style cost; deterministic c1-on-tie).
    u32 sibling = m_root;
    while (!m_nodes[sibling].is_leaf())
    {
        const u32 c1 = m_nodes[sibling].child1;
        const u32 c2 = m_nodes[sibling].child2;
        const f32 area = aabb_half_area(m_nodes[sibling].aabb);
        const f32 combined = aabb_half_area(aabb_union(m_nodes[sibling].aabb, leaf_aabb));
        const f32 cost_here = 2.0F * combined;        // make a new parent right here
        const f32 inherit = 2.0F * (combined - area); // extra cost pushed onto all ancestors
        const auto descend_cost = [&](u32 c) noexcept -> f32
        {
            const f32 nu = aabb_half_area(aabb_union(m_nodes[c].aabb, leaf_aabb));
            return (m_nodes[c].is_leaf() ? nu : (nu - aabb_half_area(m_nodes[c].aabb))) + inherit;
        };
        const f32 cost1 = descend_cost(c1);
        const f32 cost2 = descend_cost(c2);
        if (cost_here < cost1 && cost_here < cost2)
        {
            break;
        }
        sibling = (cost1 <= cost2) ? c1 : c2;
    }

    const u32 old_parent = m_nodes[sibling].parent;
    const u32 new_parent = allocate_node(); // may realloc m_nodes — only indices held past here
    {
        Node& np = m_nodes[new_parent];
        np.parent = old_parent;
        np.child1 = sibling;
        np.child2 = leaf;
        np.user_data = 0;
        np.aabb = aabb_union(leaf_aabb, m_nodes[sibling].aabb);
        np.height = 1 + m_nodes[sibling].height; // leaf height is 0
    }
    m_nodes[sibling].parent = new_parent;
    m_nodes[leaf].parent = new_parent;
    if (old_parent != k_null)
    {
        if (m_nodes[old_parent].child1 == sibling)
        {
            m_nodes[old_parent].child1 = new_parent;
        }
        else
        {
            m_nodes[old_parent].child2 = new_parent;
        }
    }
    else
    {
        m_root = new_parent;
    }

    // Refit + rebalance up to the root.
    u32 i = new_parent;
    while (i != k_null)
    {
        i = balance(i);
        fit_node_to_children(i);
        i = m_nodes[i].parent;
    }
}

void DynamicBvh::remove_leaf(u32 leaf)
{
    if (leaf == m_root)
    {
        m_root = k_null;
        return;
    }
    const u32 parent = m_nodes[leaf].parent;
    const u32 grandparent = m_nodes[parent].parent;
    const u32 sibling = (m_nodes[parent].child1 == leaf) ? m_nodes[parent].child2 : m_nodes[parent].child1;

    if (grandparent != k_null)
    {
        if (m_nodes[grandparent].child1 == parent)
        {
            m_nodes[grandparent].child1 = sibling;
        }
        else
        {
            m_nodes[grandparent].child2 = sibling;
        }
        m_nodes[sibling].parent = grandparent;
        free_node(parent);
        u32 i = grandparent;
        while (i != k_null)
        {
            i = balance(i);
            fit_node_to_children(i);
            i = m_nodes[i].parent;
        }
    }
    else
    {
        m_root = sibling;
        m_nodes[sibling].parent = k_null;
        free_node(parent);
    }
}

// ---- public mutation ------------------------------------------------------

DynamicBvhNodeId DynamicBvh::insert(const AABB3<f32>& tight_aabb, u32 user_data)
{
    CRD_ASSERT(crd::geometry::primitives::is_finite(tight_aabb)); // NaN/Inf contract — ADR-0076 §15
    const u32 leaf = allocate_node();
    {
        Node& n = m_nodes[leaf];
        n.aabb = aabb_inflate(tight_aabb, m_cfg.fat_margin);
        n.user_data = user_data;
        n.child1 = k_null;
        n.child2 = k_null;
        n.parent = k_null;
        n.height = 0;
    }
    insert_leaf(leaf);
    ++m_leaf_count;
    return DynamicBvhNodeId{leaf};
}

void DynamicBvh::remove(DynamicBvhNodeId id)
{
    CRD_ASSERT(is_allocated_leaf(id.value));
    remove_leaf(id.value);
    free_node(id.value);
    --m_leaf_count;
}

bool DynamicBvh::update(DynamicBvhNodeId id, const AABB3<f32>& new_tight_aabb)
{
    CRD_ASSERT(is_allocated_leaf(id.value));
    CRD_ASSERT(crd::geometry::primitives::is_finite(new_tight_aabb)); // NaN/Inf contract — ADR-0076 §15
    if (aabb_contains(m_nodes[id.value].aabb, new_tight_aabb))
    {
        return false; // still inside the fat AABB — nothing to do
    }
    const u32 leaf = id.value;
    remove_leaf(leaf);
    m_nodes[leaf].aabb = aabb_inflate(new_tight_aabb, m_cfg.fat_margin);
    m_nodes[leaf].child1 = k_null;
    m_nodes[leaf].child2 = k_null;
    m_nodes[leaf].parent = k_null;
    m_nodes[leaf].height = 0;
    insert_leaf(leaf);
    return true;
}

// ---- access / queries -----------------------------------------------------

AABB3<f32> DynamicBvh::bounds() const noexcept
{
    if (m_root == k_null)
    {
        constexpr f32 inf = std::numeric_limits<f32>::infinity();
        return AABB3<f32>(Vec3<f32>(inf, inf, inf), Vec3<f32>(-inf, -inf, -inf));
    }
    return m_nodes[m_root].aabb;
}

void DynamicBvh::query(const AABB3<f32>& box, crd::containers::Array<u32>& out) const
{
    query(box, [&out](u32 ud) { out.push_back(ud); });
}

// ---- closest point (v1i-a) ------------------------------------------------
//
// Branch-and-bound over the *fat* AABB tree — broadphase semantics. Per-node
// AABB squared distance is a lower bound on every leaf below it; nearer child
// descended first so `best_d2` tightens before the far subtree. Squared
// throughout; `max_dist²` stored as the cutoff. Same algorithmic shape as
// `bvh_closest_point` and `bvh4_closest_point`, just over the dynamic tree's
// fat-AABB internal nodes (no external prims span — the leaves *are* the
// primitives in this structure).

namespace
{
[[nodiscard]] f32 aabb_dist2(const AABB3<f32>& b, const Vec3<f32>& q) noexcept
{
    const Vec3<f32> d = crd::geometry::primitives::closest_point(b, q) - q;
    return crd::math::dot(d, d);
}
} // namespace

std::optional<crd::geometry::ClosestPointResult<u32>>
DynamicBvh::closest_point(const Vec3<f32>& query, f32 max_dist) const
{
    if (m_root == k_null)
    {
        return std::nullopt;
    }
    f32 best_d2 =
        (max_dist >= std::numeric_limits<f32>::infinity()) ? std::numeric_limits<f32>::infinity() : max_dist * max_dist;
    u32 best_ud = 0;
    Vec3<f32> best_point{};
    bool hit = false;

    u32 stack[k_max_dynamic_bvh_stack];
    usize sp = 0;
    stack[sp++] = m_root;
    while (sp > 0)
    {
        const Node& n = m_nodes[stack[--sp]];
        // Re-check the node's AABB against the (possibly tightened) best.
        if (aabb_dist2(n.aabb, query) >= best_d2)
        {
            continue;
        }
        if (n.is_leaf())
        {
            const Vec3<f32> cp = crd::geometry::primitives::closest_point(n.aabb, query);
            const Vec3<f32> d = cp - query;
            const f32 d2 = crd::math::dot(d, d);
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best_ud = n.user_data;
                best_point = cp;
                hit = true;
            }
        }
        else
        {
            // Push far child first, near child last (popped first to tighten
            // `best_d2`). Skip a child already known not to beat the best.
            const u32 c1 = n.child1;
            const u32 c2 = n.child2;
            const f32 d1 = aabb_dist2(m_nodes[c1].aabb, query);
            const f32 d2 = aabb_dist2(m_nodes[c2].aabb, query);
            CRD_ASSERT(sp + 2 <= k_max_dynamic_bvh_stack);
            if (d1 <= d2)
            {
                if (d2 < best_d2)
                {
                    stack[sp++] = c2;
                }
                if (d1 < best_d2)
                {
                    stack[sp++] = c1;
                }
            }
            else
            {
                if (d1 < best_d2)
                {
                    stack[sp++] = c1;
                }
                if (d2 < best_d2)
                {
                    stack[sp++] = c2;
                }
            }
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return crd::geometry::ClosestPointResult<u32>{best_point, best_d2, best_ud};
}

// ---- broadphase self-overlap (v1i-c) --------------------------------------

void DynamicBvh::find_overlapping_pairs(crd::containers::Array<DynamicBvhPair>& out,
                                        DynamicBvhPairScratch& scratch) const
{
    find_overlapping_pairs([&out](u32 a, u32 b) { out.push_back(DynamicBvhPair{a, b}); }, scratch);
}

void DynamicBvh::find_overlapping_pairs(crd::containers::Array<DynamicBvhPair>& out) const
{
    find_overlapping_pairs([&out](u32 a, u32 b) { out.push_back(DynamicBvhPair{a, b}); });
}

// ---- diagnostics ----------------------------------------------------------

f32 DynamicBvh::sah_cost() const noexcept
{
    if (m_root == k_null || m_nodes[m_root].is_leaf())
    {
        return 0.0F;
    }
    const f32 root_ha = aabb_half_area(m_nodes[m_root].aabb);
    if (root_ha <= 0.0F)
    {
        return 0.0F;
    }
    f32 sum = 0.0F;
    for (const Node& n : m_nodes)
    {
        if (n.height > 0) // interior, allocated
        {
            sum += aabb_half_area(n.aabb);
        }
    }
    return sum / root_ha;
}

usize DynamicBvh::max_depth() const noexcept
{
    if (m_root == k_null)
    {
        return 0;
    }
    // Iterative DFS over (index, depth).
    struct Frame
    {
        u32 idx;
        usize depth;
    };
    Frame stack[k_max_dynamic_bvh_stack];
    usize sp = 0;
    stack[sp++] = Frame{m_root, 0};
    usize best = 0;
    while (sp > 0)
    {
        const Frame f = stack[--sp];
        if (f.depth > best)
        {
            best = f.depth;
        }
        const Node& n = m_nodes[f.idx];
        if (!n.is_leaf())
        {
            CRD_ASSERT(sp + 2 <= k_max_dynamic_bvh_stack);
            stack[sp++] = Frame{n.child1, f.depth + 1};
            stack[sp++] = Frame{n.child2, f.depth + 1};
        }
    }
    return best;
}

void DynamicBvh::validate() const noexcept
{
    if (m_root == k_null)
    {
        CRD_ASSERT(m_leaf_count == 0);
        return;
    }
    CRD_ASSERT(m_nodes[m_root].parent == k_null);
    u32 stack[k_max_dynamic_bvh_stack];
    usize sp = 0;
    stack[sp++] = m_root;
    [[maybe_unused]] usize leaves_seen = 0;
    while (sp > 0)
    {
        const u32 i = stack[--sp];
        const Node& n = m_nodes[i];
        if (n.is_leaf())
        {
            CRD_ASSERT(n.child2 == k_null);
            CRD_ASSERT(n.height == 0);
            ++leaves_seen;
            continue;
        }
        CRD_ASSERT(n.child1 != k_null && n.child2 != k_null);
        [[maybe_unused]] const Node& c1 = m_nodes[n.child1];
        [[maybe_unused]] const Node& c2 = m_nodes[n.child2];
        CRD_ASSERT(c1.parent == i && c2.parent == i);
        CRD_ASSERT(n.height == 1 + (c1.height > c2.height ? c1.height : c2.height));
        CRD_ASSERT(aabb_contains(n.aabb, c1.aabb) && aabb_contains(n.aabb, c2.aabb));
        CRD_ASSERT(sp + 2 <= k_max_dynamic_bvh_stack);
        stack[sp++] = n.child1;
        stack[sp++] = n.child2;
    }
    CRD_ASSERT(leaves_seen == m_leaf_count);
}

} // namespace crd::geometry::bvh
