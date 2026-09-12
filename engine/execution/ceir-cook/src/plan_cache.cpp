#include <crd/ceir/cook/plan_cache.hpp>

namespace crd::ceir::cook
{
crd::u64 plan_target(containers::StringView provider_name) noexcept
{
    crd::u64 h = 0xcbf29ce484222325ULL; // FNV-1a (the CapabilityId pattern) over the provider name bytes
    for (crd::usize i = 0; i < provider_name.size(); ++i)
    {
        h ^= static_cast<crd::u8>(provider_name[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

namespace
{
[[nodiscard]] bool key_eq(const PlanKey& a, const PlanKey& b) noexcept
{
    return a.content_hash == b.content_hash && a.target == b.target && a.compiler_version == b.compiler_version;
}
} // namespace

PlanCache::PlanCache(memory::IAllocator* alloc, InterfaceResolver resolver, void* user)
    : m_alloc(alloc), m_resolver(resolver), m_user(user), m_entries(alloc)
{
}

PlanCache::~PlanCache() { clear(); }

void PlanCache::free_entry(Entry& e) noexcept
{
    if (e.artifact != nullptr) { m_alloc->deallocate(e.artifact); }
    if (e.deps != nullptr) { m_alloc->deallocate(e.deps); }
    e.artifact      = nullptr;
    e.artifact_size = 0U;
    e.deps          = nullptr;
    e.dep_count     = 0U;
}

crd::usize PlanCache::find(const PlanKey& key) const noexcept
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (key_eq(m_entries[i].key, key)) { return i; }
    }
    return m_entries.size();
}

bool PlanCache::deps_valid(const Entry& e) const
{
    for (crd::usize i = 0; i < e.dep_count; ++i)
    {
        const crd::u64 current = m_resolver(e.deps[i].id, m_user);
        // ⛔ EMPTY≠UNKNOWN: a resolver return of 0 (gone/unresolvable) is STALE, never valid.
        if (current == 0U || current != e.deps[i].interface_hash) { return false; }
    }
    return true;
}

PlanLookup PlanCache::get(const PlanKey& key)
{
    const crd::usize idx = find(key);
    if (idx == m_entries.size())
    {
        ++m_misses;
        return PlanLookup{PlanStatus::Miss, nullptr, 0U};
    }
    if (!deps_valid(m_entries[idx]))
    {
        // a recorded dep drifted → the cached plan is stale; drop it (lazy eviction) and report a recompile.
        free_entry(m_entries[idx]);
        const crd::usize last = m_entries.size() - 1U;
        if (idx != last) { m_entries[idx] = m_entries[last]; }
        m_entries.pop_back();
        ++m_misses;
        return PlanLookup{PlanStatus::StaleDeps, nullptr, 0U};
    }
    ++m_hits;
    return PlanLookup{PlanStatus::Hit, m_entries[idx].artifact, m_entries[idx].artifact_size};
}

void PlanCache::put(AssetId owner, const PlanKey& key, containers::ConstSpan<crd::u8> artifact,
                    containers::ConstSpan<PlanDep> deps)
{
    const crd::usize idx    = find(key);
    Entry*           target = nullptr;
    if (idx != m_entries.size())
    {
        free_entry(m_entries[idx]); // replace an existing same-key entry
        target = &m_entries[idx];
    }
    else
    {
        m_entries.push_back(Entry{}); // POD entry; buffers filled below
        target = &m_entries[m_entries.size() - 1U];
    }
    target->owner = owner;
    target->key   = key;
    // copy the artifact bytes into a cache-owned heap buffer (stable across entry-vector growth).
    if (artifact.size() > 0U)
    {
        auto* const buf = static_cast<crd::u8*>(m_alloc->allocate(artifact.size(), alignof(crd::u8)));
        for (crd::usize i = 0; i < artifact.size(); ++i) { buf[i] = artifact[i]; }
        target->artifact      = buf;
        target->artifact_size = artifact.size();
    }
    // copy the dep records.
    if (deps.size() > 0U)
    {
        auto* const dbuf = static_cast<PlanDep*>(m_alloc->allocate(sizeof(PlanDep) * deps.size(), alignof(PlanDep)));
        for (crd::usize i = 0; i < deps.size(); ++i) { dbuf[i] = deps[i]; }
        target->deps      = dbuf;
        target->dep_count = deps.size();
    }
}

void PlanCache::evict(AssetId owner)
{
    crd::usize i = 0;
    while (i < m_entries.size())
    {
        if (m_entries[i].owner == owner)
        {
            free_entry(m_entries[i]);
            const crd::usize last = m_entries.size() - 1U;
            if (i != last) { m_entries[i] = m_entries[last]; }
            m_entries.pop_back(); // do NOT advance i — a swapped-in entry now occupies slot i
        }
        else
        {
            ++i;
        }
    }
}

void PlanCache::clear()
{
    for (crd::usize i = 0; i < m_entries.size(); ++i) { free_entry(m_entries[i]); }
    m_entries.clear();
}
} // namespace crd::ceir::cook
