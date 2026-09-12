// node.cpp — the CHIR-0 source model (CEIR-32b, ADR-0128 D5). See node.hpp.

#include <crd/chir/node.hpp>

namespace crd::chir
{
namespace
{
// Fold a byte range into a running FNV-1a-64 accumulator (the SAME constants as containers::fnv1a_64, so a one-shot over
// a concatenated buffer and this incremental fold agree — used to combine an id's constituent fields without a scratch
// buffer). offset 0xcbf29ce484222325, prime 0x100000001B3 (containers/hash.hpp).
inline void fnv_fold(crd::u64& h, const void* data, crd::usize n) noexcept
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (crd::usize i = 0; i < n; ++i) { h = (h ^ p[i]) * 0x00000100000001B3ULL; }
}
inline void fnv_fold_u64(crd::u64& h, crd::u64 v) noexcept { fnv_fold(h, &v, sizeof v); }
inline void fnv_fold_u32(crd::u64& h, crd::u32 v) noexcept { fnv_fold(h, &v, sizeof v); }
inline void fnv_fold_u8(crd::u64& h, crd::u8 v) noexcept { fnv_fold(h, &v, sizeof v); }
inline constexpr crd::u64 kFnvOffset = 0xcbf29ce484222325ULL;
} // namespace

StringView node_kind_name(NodeKind k) noexcept
{
    switch (k)
    {
    case NodeKind::Program: return StringView("program");
    case NodeKind::EventHandler: return StringView("event_handler");
    case NodeKind::StateDecl: return StringView("state_decl");
    case NodeKind::Query: return StringView("query");
    case NodeKind::ParallelFor: return StringView("parallel_for");
    case NodeKind::Await: return StringView("await");
    case NodeKind::StateUpdate: return StringView("state_update");
    }
    return StringView("?");
}

bool node_kind_from_name(StringView s, NodeKind& out) noexcept
{
    if (s == StringView("program")) { out = NodeKind::Program; return true; }
    if (s == StringView("event_handler")) { out = NodeKind::EventHandler; return true; }
    if (s == StringView("state_decl")) { out = NodeKind::StateDecl; return true; }
    if (s == StringView("query")) { out = NodeKind::Query; return true; }
    if (s == StringView("parallel_for")) { out = NodeKind::ParallelFor; return true; }
    if (s == StringView("await")) { out = NodeKind::Await; return true; }
    if (s == StringView("state_update")) { out = NodeKind::StateUpdate; return true; }
    return false;
}

StringRef SourceModel::intern(StringView s)
{
    const auto off = static_cast<crd::u32>(m_strings.size());
    for (crd::usize i = 0; i < s.size(); ++i) { m_strings.push_back(s[i]); }
    return StringRef{off, static_cast<crd::u32>(s.size())};
}

StringView SourceModel::str(StringRef r) const noexcept { return StringView(m_strings.data() + r.off, r.len); }

crd::u32 SourceModel::add_node(NodeKind kind, StringView name, crd::u32 parent, SourceLoc loc)
{
    const auto idx = static_cast<crd::u32>(m_nodes.size());
    ChirNode&  n   = m_nodes.emplace_back(m_alloc);
    n.kind         = kind;
    n.loc          = loc;
    n.name         = intern(name); // NOTE: intern may reallocate m_strings; `n` is a live ref into m_nodes (unaffected)
    n.parent       = parent;
    if (parent != kInvalidNode) { m_nodes[parent].children.push_back(idx); }
    return idx;
}

void SourceModel::add_pin(crd::u32 node, PinDir dir, StringView name, StringView type)
{
    const StringRef nm = intern(name);
    const StringRef ty = intern(type);
    m_nodes[node].pins.push_back(Pin{nm, ty, dir});
}

void SourceModel::add_attr(crd::u32 node, StringView key, StringView val)
{
    const StringRef k = intern(key);
    const StringRef v = intern(val);
    m_nodes[node].attrs.push_back(Attr{k, v});
}

void SourceModel::add_edge(crd::u32 from_node, crd::u32 from_pin, crd::u32 to_node, crd::u32 to_pin)
{
    // ⛔ edges are a SET keyed by the CONSUMER pin (to_node, to_pin) — an in-pin has exactly ONE source (single-writer).
    // Stored in canonical (to_node, to_pin) order so semantic_hash + print_schema are AUTHORING-ORDER-INDEPENDENT: the
    // text projection (32c) wires each edge at its consumer in-pin (pin order, node pre-order), which is exactly this
    // order — so both projections produce byte-identical edge sections + equal semantic_hash (the 32c/32d parity
    // precondition). Linear insert (edge counts are tiny; no sort call, no std container).
    const Edge e{from_node, from_pin, to_node, to_pin};
    crd::u32   pos = 0;
    while (pos < m_edges.size())
    {
        const Edge& x        = m_edges[pos];
        const bool  x_before = (x.to_node < to_node) || (x.to_node == to_node && x.to_pin < to_pin);
        if (!x_before) { break; } // insert BEFORE the first edge whose consumer key is >= e's
        ++pos;
    }
    m_edges.insert(pos, e);
}

void SourceModel::set_layout(StableId id, crd::f32 x, crd::f32 y, crd::u32 group)
{
    for (Layout& l : m_layout)
    {
        if (l.id == id) { l.x = x; l.y = y; l.group = group; return; } // last-write-wins per id
    }
    m_layout.push_back(Layout{id, x, y, group});
}

void SourceModel::derive_ids() noexcept
{
    // Parents precede children in m_nodes (add order), so a single forward pass sees each node's parent id already set.
    for (crd::u32 i = 0; i < m_nodes.size(); ++i)
    {
        ChirNode&      n  = m_nodes[i];
        const crd::u64 pid = (n.parent != kInvalidNode) ? m_nodes[n.parent].id.value : 0ULL;
        crd::u64       h  = kFnvOffset;
        fnv_fold_u64(h, pid);
        fnv_fold_u8(h, static_cast<crd::u8>(n.kind));
        if (n.name.len != 0U)
        {
            // NAMED: id = fnv(parent ‖ kind ‖ name) — position-INDEPENDENT (reorder siblings -> id unchanged).
            const StringView nm = str(n.name);
            fnv_fold(h, nm.data(), nm.size());
        }
        else
        {
            // ANONYMOUS: id = fnv(parent ‖ kind ‖ ordinal). ⛔ the ordinal is the count of same-kind anonymous siblings
            // BEFORE this node in the PARENT'S children order — the SEMANTIC sibling order, NOT the global construction
            // index (ADR-0128 ruling 3: position-independent). The root (no parent) has ordinal 0.
            crd::u32 ordinal = 0;
            if (n.parent != kInvalidNode)
            {
                const ChirNode& par = m_nodes[n.parent];
                for (crd::u32 k = 0; k < par.children.size(); ++k)
                {
                    const crd::u32 sib = par.children[k];
                    if (sib == i) { break; }
                    if (m_nodes[sib].kind == n.kind && m_nodes[sib].name.len == 0U) { ++ordinal; }
                }
            }
            fnv_fold_u32(h, ordinal);
        }
        n.id = StableId{h};
    }
}

crd::u64 SourceModel::semantic_hash() const noexcept
{
    // Canonical fold over SEMANTIC content only — kind, name, pins, attrs, child stable-ids, and edges (by endpoint
    // STABLE IDS, so it is position-independent). ⛔ IGNORES layout AND SourceLoc (§180 #10). Requires derive_ids first.
    crd::u64 h = kFnvOffset;
    for (crd::u32 i = 0; i < m_nodes.size(); ++i)
    {
        const ChirNode& n = m_nodes[i];
        fnv_fold_u64(h, n.id.value);
        fnv_fold_u8(h, static_cast<crd::u8>(n.kind));
        const StringView nm = str(n.name);
        fnv_fold(h, nm.data(), nm.size());
        for (const Pin& p : n.pins)
        {
            fnv_fold_u8(h, static_cast<crd::u8>(p.dir));
            const StringView pn = str(p.name);
            const StringView pt = str(p.type);
            fnv_fold(h, pn.data(), pn.size());
            fnv_fold(h, pt.data(), pt.size());
        }
        for (const Attr& a : n.attrs)
        {
            const StringView ak = str(a.key);
            const StringView av = str(a.val);
            fnv_fold(h, ak.data(), ak.size());
            fnv_fold(h, av.data(), av.size());
        }
        for (const crd::u32 c : n.children) { fnv_fold_u64(h, m_nodes[c].id.value); }
    }
    for (const Edge& e : m_edges)
    {
        fnv_fold_u64(h, m_nodes[e.from_node].id.value);
        fnv_fold_u32(h, e.from_pin);
        fnv_fold_u64(h, m_nodes[e.to_node].id.value);
        fnv_fold_u32(h, e.to_pin);
    }
    return h;
}

crd::u32 SourceModel::root() const noexcept
{
    for (crd::u32 i = 0; i < m_nodes.size(); ++i)
    {
        if (m_nodes[i].kind == NodeKind::Program) { return i; }
    }
    return kInvalidNode;
}

} // namespace crd::chir
