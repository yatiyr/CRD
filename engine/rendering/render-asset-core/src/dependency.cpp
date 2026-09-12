#include <crd/renderasset/dependency.hpp>

#include <utility> // std::swap

namespace crd::renderasset
{
namespace
{
// Binary search: index where `id` is or would be inserted in a sorted list (DependencyRecord's deps).
usize lower_bound_id(const Array<AssetId>& a, AssetId id) noexcept
{
    usize lo = 0;
    usize hi = a.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (a[mid] < id) { lo = mid + 1; }
        else { hi = mid; }
    }
    return lo;
}

void insert_sorted_unique(Array<AssetId>& a, AssetId id)
{
    const usize idx = lower_bound_id(a, id);
    if (idx < a.size() && a[idx] == id) { return; } // already present
    a.push_back(id);
    for (usize j = a.size() - 1; j > idx; --j) { std::swap(a[j], a[j - 1]); }
}

// "0x" + 16 lowercase hex digits into buf[19]; returns a view over it.
StringView id_hex(u64 v, char (&buf)[19]) noexcept
{
    buf[0] = '0';
    buf[1] = 'x';
    for (usize i = 0; i < 16; ++i)
    {
        const u32 nyb = static_cast<u32>((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i]    = static_cast<char>(nyb < 10 ? ('0' + nyb) : ('a' + (nyb - 10)));
    }
    return StringView{buf, 18};
}
} // namespace

void DependencyRecord::add(AssetId dep)
{
    if (!dep.valid() || dep == m_owner) { return; }
    insert_sorted_unique(m_deps, dep);
}

// ── CEIR-8h (ADR-0118): every method below DELEGATES to the one engine (m_dag). AssetId ↔ NodeId (its u64 `value`) is
// lossless + order-preserving, so the emitted order is BYTE-IDENTICAL to the pre-refactor implementation. ──
void DependencyGraph::add_node(AssetId id) { m_dag.add_node(id.value); } // invalid (value 0) ignored by the engine

void DependencyGraph::add_edge(AssetId from, AssetId to) { m_dag.add_edge(from.value, to.value); } // self/invalid ignored

bool DependencyGraph::topo_order(Array<AssetId>& out, DiagnosticList& diags) const
{
    out.clear();
    containers::Array<u64> ids(m_alloc);
    if (!m_dag.topo_order(ids))
    {
        diags.error(DiagCode::CyclicDependency, "dependency graph contains a cycle");
        return false;
    }
    for (usize i = 0; i < ids.size(); ++i) { out.push_back(AssetId{ids[i]}); }
    return true;
}

bool DependencyGraph::affected_by(AssetId changed, Array<AssetId>& out, DiagnosticList& diags) const
{
    out.clear();
    containers::Array<u64> ids(m_alloc);
    if (!m_dag.affected_by(changed.value, ids)) // false ONLY on a cycle (an invalid/absent `changed` ⇒ true + empty)
    {
        diags.error(DiagCode::CyclicDependency, "dependency graph contains a cycle");
        return false;
    }
    for (usize i = 0; i < ids.size(); ++i) { out.push_back(AssetId{ids[i]}); }
    return true;
}

bool DependencyGraph::validate_against(const AssetRegistry& registry, DiagnosticList& diags) const
{
    bool ok = true;
    for (usize i = 0; i < m_dag.node_count(); ++i) // engine nodes are sorted by id — the same iteration order as before
    {
        const AssetId owner{m_dag.node_id_at(i)};
        StringView    owner_path{};
        const bool    owner_known = registry.lookup(owner, owner_path);
        const auto    deps        = m_dag.deps_at(i);
        for (usize d = 0; d < deps.size(); ++d)
        {
            const AssetId dep{deps[d]};
            if (!registry.contains(dep))
            {
                ok = false;
                char buf[19];
                diags.emit(Severity::Error, DiagCode::MissingDependency, "asset depends on an unregistered asset id",
                           owner_known ? owner_path : StringView{}, StringView{}, StringView{}, id_hex(dep.value, buf));
            }
        }
    }
    return ok;
}
} // namespace crd::renderasset
