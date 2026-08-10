#pragma once

// crd-containers — IncrementalDag (CEIR-8h, ADR-0118): the ONE generic dependency/dirty engine the tree's ≥2 models
// converge on (render-asset-core::DependencyGraph is a thin wrapper over THIS; the cook's content/interface hash pair
// is its revision model). A DAG keyed by `NodeId = u64` (a content hash, an asset id, any stable id; 0 = invalid) with
// a DETERMINISTIC topological order (deps-first, ties by ascending id — the RAF-11 reproducibility contract) and a
// per-node (content, interface) revision pair driving the §107 dirty rule: a content change recomputes the node; an
// INTERFACE change also recomputes its transitive dependents; a content-ONLY change hot-swaps (dependents stay valid).
// ⛔ NO diagnostics here (containers cannot name a diagnostic type) — cycle ⇒ `false`, the caller reports it. ⛔ Holds
// STRUCTURE + REVISIONS + dirty propagation, never the consumer's cached RESULTS (those are keyed by the content hash).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <utility> // std::swap (the DependencyGraph precedent this absorbs)

namespace crd::containers
{
class IncrementalDag
{
public:
    using NodeId = u64;

    explicit IncrementalDag(memory::IAllocator* alloc) noexcept : m_nodes(alloc), m_alloc(alloc) {}

    // Ensure a node exists even with no edges (participates in ordering). id 0 (invalid) is ignored.
    void add_node(NodeId id)
    {
        if (id != 0U) { (void)ensure_node(id); }
    }
    // Record "from depends on to" (so `to` orders before `from`). Self-edges + invalid ids are ignored.
    void add_edge(NodeId from, NodeId to)
    {
        if (from == 0U || to == 0U || from == to) { return; }
        (void)ensure_node(to);
        const usize fi = ensure_node(from); // last, so fi stays valid
        insert_sorted_unique(m_nodes[fi].deps, to);
    }

    [[nodiscard]] usize            node_count() const noexcept { return m_nodes.size(); }
    [[nodiscard]] NodeId           node_id_at(usize i) const noexcept { return m_nodes[i].id; }
    [[nodiscard]] ConstSpan<NodeId> deps_at(usize i) const noexcept
    {
        return ConstSpan<NodeId>(m_nodes[i].deps.data(), m_nodes[i].deps.size());
    }

    // Deterministic topological order (deps first, ties by ascending id). Cycle ⇒ clears `out`, returns false.
    [[nodiscard]] bool topo_order(Array<NodeId>& out) const
    {
        out.clear();
        const usize n = m_nodes.size();
        Array<usize> remaining(m_alloc);
        Array<u8>    emitted(m_alloc);
        for (usize i = 0; i < n; ++i)
        {
            remaining.push_back(m_nodes[i].deps.size());
            emitted.push_back(0U);
        }
        for (usize produced = 0; produced < n; ++produced)
        {
            usize pick = n; // nodes are sorted ascending by id ⇒ the first ready+unemitted is the minimum id (det. tie-break)
            for (usize i = 0; i < n; ++i)
            {
                if (emitted[i] == 0U && remaining[i] == 0U)
                {
                    pick = i;
                    break;
                }
            }
            if (pick == n)
            {
                out.clear();
                return false; // a cycle — the caller emits its own CyclicDependency
            }
            const NodeId pid = m_nodes[pick].id;
            out.push_back(pid);
            emitted[pick] = 1U;
            for (usize j = 0; j < n; ++j)
            {
                if (emitted[j] == 0U && remaining[j] > 0U && contains(m_nodes[j].deps, pid)) { --remaining[j]; }
            }
        }
        return true;
    }

    // The transitive DEPENDENTS of `changed` (every node that, directly or through a chain, depends on it), in the
    // graph's own deterministic topo order, EXCLUDING `changed`. Empty when nothing depends on it. Cycle ⇒ false.
    [[nodiscard]] bool affected_by(NodeId changed, Array<NodeId>& out) const
    {
        out.clear();
        if (changed == 0U) { return true; }
        Array<NodeId> affected(m_alloc);
        Array<NodeId> frontier(m_alloc);
        frontier.push_back(changed);
        while (frontier.size() > 0U)
        {
            const NodeId cur = frontier[frontier.size() - 1U];
            frontier.pop_back();
            for (usize i = 0; i < m_nodes.size(); ++i)
            {
                const Node& node = m_nodes[i];
                if (node.id == changed) { continue; } // never fold `changed` into its own dependent set
                if (contains(node.deps, cur) && !contains(affected, node.id))
                {
                    insert_sorted_unique(affected, node.id);
                    frontier.push_back(node.id);
                }
            }
        }
        if (affected.size() == 0U) { return true; }
        Array<NodeId> order(m_alloc);
        if (!topo_order(order)) { out.clear(); return false; } // reuse the deterministic order (a cyclic graph rejects)
        for (usize i = 0; i < order.size(); ++i)
        {
            if (contains(affected, order[i])) { out.push_back(order[i]); }
        }
        return true;
    }

    // ── the incremental layer (the §107 dirty rule, generalized) ──
    // Set/update a node's (content, interface) revision pair (materializes the node). ⛔ the field is `interface_rev`,
    // NOT `interface` — Windows COM headers `#define interface struct`, and this is a foundational header a COM-pulling
    // module may include.
    void set_revision(NodeId id, u64 content, u64 interface_rev)
    {
        if (id == 0U) { return; }
        const usize i          = ensure_node(id);
        m_nodes[i].content      = content;
        m_nodes[i].interface_rev = interface_rev;
    }
    [[nodiscard]] u64 content_of(NodeId id) const noexcept
    {
        const usize i = find_node(id);
        return (i < m_nodes.size() && m_nodes[i].id == id) ? m_nodes[i].content : 0U;
    }
    [[nodiscard]] u64 interface_of(NodeId id) const noexcept
    {
        const usize i = find_node(id);
        return (i < m_nodes.size() && m_nodes[i].id == id) ? m_nodes[i].interface_rev : 0U;
    }

    // ⛔ THE headline rule: update `changed`'s revisions and fill `out` (topo-ordered) with the nodes that must
    // RECOMPUTE — `changed` itself iff its CONTENT changed, PLUS its transitive dependents iff its INTERFACE changed
    // (a content-only change hot-swaps: dependents stay valid). Cycle ⇒ false. This is the cook §107 semantics as a
    // generic engine rule — the one property that distinguishes this from a naive dirty-flag graph.
    [[nodiscard]] bool recompute_after_change(NodeId changed, u64 new_content, u64 new_interface, Array<NodeId>& out)
    {
        out.clear();
        if (changed == 0U) { return true; }
        const usize i                 = ensure_node(changed);
        const bool  content_changed   = m_nodes[i].content != new_content;
        const bool  interface_changed = m_nodes[i].interface_rev != new_interface;
        m_nodes[i].content      = new_content;
        m_nodes[i].interface_rev = new_interface;
        // ⛔ self recomputes on EITHER change: an interface change without a content change is incoherent under
        // content-addressing (interface is a projection of content), so recompute self conservatively rather than let a
        // dependent recompute against a node that never did (over-recompute, never stale — the EMPTY≠UNKNOWN direction).
        if (content_changed || interface_changed) { out.push_back(changed); } // topo-first — it is its dependents' dep
        if (interface_changed)
        {
            Array<NodeId> deps(m_alloc);
            if (!affected_by(changed, deps)) { out.clear(); return false; }
            for (usize k = 0; k < deps.size(); ++k) { out.push_back(deps[k]); } // dependents, topo-ordered
        }
        return true;
    }

private:
    struct Node
    {
        NodeId        id;
        Array<NodeId> deps;             // sorted ascending, unique
        u64           content       = 0U;
        u64           interface_rev = 0U; // ⛔ NOT `interface` (the Windows COM `#define interface struct` landmine)
    };

    [[nodiscard]] static usize lower_bound(const Array<NodeId>& a, NodeId id) noexcept
    {
        usize lo = 0;
        usize hi = a.size();
        while (lo < hi)
        {
            const usize mid = lo + (hi - lo) / 2U;
            if (a[mid] < id) { lo = mid + 1U; }
            else { hi = mid; }
        }
        return lo;
    }
    [[nodiscard]] static bool contains(const Array<NodeId>& a, NodeId id) noexcept
    {
        const usize idx = lower_bound(a, id);
        return idx < a.size() && a[idx] == id;
    }
    static void insert_sorted_unique(Array<NodeId>& a, NodeId id)
    {
        const usize idx = lower_bound(a, id);
        if (idx < a.size() && a[idx] == id) { return; }
        a.push_back(id);
        for (usize j = a.size() - 1U; j > idx; --j) { std::swap(a[j], a[j - 1U]); }
    }
    [[nodiscard]] usize find_node(NodeId id) const noexcept
    {
        usize lo = 0;
        usize hi = m_nodes.size();
        while (lo < hi)
        {
            const usize mid = lo + (hi - lo) / 2U;
            if (m_nodes[mid].id < id) { lo = mid + 1U; }
            else { hi = mid; }
        }
        return lo;
    }
    usize ensure_node(NodeId id)
    {
        const usize idx = find_node(id);
        if (idx < m_nodes.size() && m_nodes[idx].id == id) { return idx; }
        m_nodes.push_back(Node{id, Array<NodeId>(m_alloc), 0U, 0U});
        for (usize j = m_nodes.size() - 1U; j > idx; --j) { std::swap(m_nodes[j], m_nodes[j - 1U]); }
        return idx;
    }

    Array<Node>         m_nodes; // sorted ascending by id
    memory::IAllocator* m_alloc;
};
} // namespace crd::containers
