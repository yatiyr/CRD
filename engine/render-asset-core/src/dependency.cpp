#include <crd/renderasset/dependency.hpp>

#include <utility> // std::swap

namespace crd::renderasset
{
namespace
{
// Binary search: index where `id` is or would be inserted in a sorted list.
usize lower_bound_id(const Array<AssetId>& a, AssetId id) noexcept
{
    usize lo = 0;
    usize hi = a.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (a[mid] < id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

bool contains_id(const Array<AssetId>& a, AssetId id) noexcept
{
    const usize idx = lower_bound_id(a, id);
    return idx < a.size() && a[idx] == id;
}

void insert_sorted_unique(Array<AssetId>& a, AssetId id)
{
    const usize idx = lower_bound_id(a, id);
    if (idx < a.size() && a[idx] == id)
    {
        return; // already present
    }
    a.push_back(id);
    for (usize j = a.size() - 1; j > idx; --j)
    {
        std::swap(a[j], a[j - 1]);
    }
}

// "0x" + 16 lowercase hex digits into buf[19]; returns a view over it.
StringView id_hex(u64 v, char (&buf)[19]) noexcept
{
    buf[0] = '0';
    buf[1] = 'x';
    for (usize i = 0; i < 16; ++i)
    {
        const u32 nyb = static_cast<u32>((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = static_cast<char>(nyb < 10 ? ('0' + nyb) : ('a' + (nyb - 10)));
    }
    return StringView{buf, 18};
}
} // namespace

void DependencyRecord::add(AssetId dep)
{
    if (!dep.valid() || dep == m_owner)
    {
        return;
    }
    insert_sorted_unique(m_deps, dep);
}

usize DependencyGraph::find_node(AssetId id) const noexcept
{
    usize lo = 0;
    usize hi = m_nodes.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (m_nodes[mid].id < id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

usize DependencyGraph::ensure_node(AssetId id)
{
    const usize idx = find_node(id);
    if (idx < m_nodes.size() && m_nodes[idx].id == id)
    {
        return idx;
    }
    m_nodes.push_back(Node{id, Array<AssetId>(m_alloc)});
    for (usize j = m_nodes.size() - 1; j > idx; --j)
    {
        std::swap(m_nodes[j], m_nodes[j - 1]);
    }
    return idx;
}

void DependencyGraph::add_node(AssetId id)
{
    if (!id.valid())
    {
        return;
    }
    ensure_node(id);
}

void DependencyGraph::add_edge(AssetId from, AssetId to)
{
    if (!from.valid() || !to.valid() || from == to)
    {
        return;
    }
    ensure_node(to);
    const usize fi = ensure_node(from); // last, so fi stays valid
    insert_sorted_unique(m_nodes[fi].deps, to);
}

bool DependencyGraph::topo_order(Array<AssetId>& out, DiagnosticList& diags) const
{
    out.clear();
    const usize n = m_nodes.size();

    Array<usize> remaining(m_alloc);
    Array<u8> emitted(m_alloc);
    for (usize i = 0; i < n; ++i)
    {
        remaining.push_back(m_nodes[i].deps.size());
        emitted.push_back(0);
    }

    for (usize produced = 0; produced < n; ++produced)
    {
        // Nodes are sorted ascending by id, so the first ready+unemitted node is
        // the minimum id — deterministic tie-breaking, no allocation.
        usize pick = n;
        for (usize i = 0; i < n; ++i)
        {
            if (emitted[i] == 0 && remaining[i] == 0)
            {
                pick = i;
                break;
            }
        }
        if (pick == n)
        {
            diags.error(DiagCode::CyclicDependency, "dependency graph contains a cycle");
            out.clear();
            return false;
        }

        const AssetId pid = m_nodes[pick].id;
        out.push_back(pid);
        emitted[pick] = 1;

        for (usize j = 0; j < n; ++j)
        {
            if (emitted[j] == 0 && remaining[j] > 0 && contains_id(m_nodes[j].deps, pid))
            {
                --remaining[j];
            }
        }
    }
    return true;
}

bool DependencyGraph::validate_against(const AssetRegistry& registry, DiagnosticList& diags) const
{
    bool ok = true;
    for (usize i = 0; i < m_nodes.size(); ++i)
    {
        const Node& node = m_nodes[i];
        StringView owner_path{};
        const bool owner_known = registry.lookup(node.id, owner_path);
        for (usize d = 0; d < node.deps.size(); ++d)
        {
            const AssetId dep = node.deps[d];
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
