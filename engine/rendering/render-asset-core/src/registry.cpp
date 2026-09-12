#include <crd/renderasset/registry.hpp>

#include <utility> // std::move, std::swap

namespace crd::renderasset
{
usize AssetRegistry::lower_bound(AssetId id) const noexcept
{
    usize lo = 0;
    usize hi = m_entries.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (m_entries[mid].id < id)
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

bool AssetRegistry::contains(AssetId id) const noexcept
{
    const usize idx = lower_bound(id);
    return idx < m_entries.size() && m_entries[idx].id == id;
}

bool AssetRegistry::lookup(AssetId id, StringView& out_canonical) const noexcept
{
    const usize idx = lower_bound(id);
    if (idx < m_entries.size() && m_entries[idx].id == id)
    {
        const String& c = m_entries[idx].canonical;
        out_canonical = StringView{c.data(), c.size()};
        return true;
    }
    return false;
}

bool AssetRegistry::register_id(AssetId id, StringView canonical, DiagnosticList& diags)
{
    if (!id.valid())
    {
        return false;
    }
    const usize idx = lower_bound(id);
    if (idx < m_entries.size() && m_entries[idx].id == id)
    {
        const String& existing = m_entries[idx].canonical;
        if (existing == canonical)
        {
            return true; // idempotent: same id, same path
        }
        diags.emit(Severity::Error, DiagCode::IdCollision, "asset id maps to two different canonical paths", canonical,
                   {}, StringView{existing.data(), existing.size()}, canonical);
        return false;
    }

    // Insert at idx keeping the array sorted by id: append, then bubble down.
    m_entries.push_back(Entry{id, String(canonical, m_alloc)});
    for (usize j = m_entries.size() - 1; j > idx; --j)
    {
        std::swap(m_entries[j], m_entries[j - 1]);
    }
    return true;
}

bool AssetRegistry::register_ref(const AssetRef& ref, DiagnosticList& diags)
{
    if (!ref.valid())
    {
        return false;
    }
    return register_id(ref.id(), ref.canonical(), diags);
}

bool AssetRegistry::register_path(StringView raw, DiagnosticList& diags)
{
    const AssetRef ref = AssetRef::parse(raw, diags, m_alloc);
    if (!ref.valid())
    {
        return false;
    }
    return register_id(ref.id(), ref.canonical(), diags);
}
} // namespace crd::renderasset
