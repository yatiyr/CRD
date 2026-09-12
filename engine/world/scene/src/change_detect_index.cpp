// Phase 3.0 v1i — ChangeDetectIndex (ADR-0053 §3).

#include <crd/scene/change_detect_index.hpp>

namespace crd::scene
{

ChangeDetectIndex::ChangeDetectIndex(crd::memory::IAllocator* alloc) : m_last_change_frame(alloc) {}

void ChangeDetectIndex::record(ComponentId c, EntityId e) noexcept
{
    const crd::u64 key = encode_key(c, e);
    if (auto* existing = m_last_change_frame.find(key); existing != nullptr)
    {
        *existing = m_current_frame;
    }
    else
    {
        m_last_change_frame.emplace(key, m_current_frame);
    }
}

void ChangeDetectIndex::on_insert(EntityId e, ComponentId c, const void* /*data*/)
{
    record(c, e);
}

void ChangeDetectIndex::on_update(EntityId e, ComponentId c, const void* /*old_data*/, const void* /*new_data*/)
{
    record(c, e);
}

void ChangeDetectIndex::on_remove(EntityId e, ComponentId c, const void* /*data*/)
{
    // Removal counts as a change — consumers of `.changed<T>()` may want
    // to see entities whose T was just removed (e.g., to clean up GPU
    // resources tied to T). Last-modified-frame is bumped; the next
    // on_entity_destroyed (if any) clears the entry.
    record(c, e);
}

void ChangeDetectIndex::on_entity_destroyed(EntityId e)
{
    // Walk only the components this index actually observes — at most
    // kMaxComponents (256) erase probes, but typically just the few
    // components the entity carried. Each erase is O(1) average.
    const crd::u64 idx = static_cast<crd::u64>(e.index());
    const crd::u64 gen = static_cast<crd::u64>(e.generation()) & 0xFFFFULL;
    const crd::u64 entity_bits = (gen << 32) | idx;
    for (crd::u32 c_raw = 0; c_raw < kMaxComponents; ++c_raw)
    {
        const ComponentId c{static_cast<crd::u16>(c_raw)};
        if (!m_observed.test(c))
        {
            continue;
        }
        const crd::u64 key = (static_cast<crd::u64>(c_raw) << 48) | entity_bits;
        m_last_change_frame.erase(key);
    }
}

bool ChangeDetectIndex::changed_since(EntityId e, ComponentId c, crd::u32 since_frame) const noexcept
{
    const crd::u64 key = encode_key(c, e);
    const auto* slot = m_last_change_frame.find(key);
    if (slot == nullptr)
    {
        return false;
    }
    return *slot >= since_frame;
}

} // namespace crd::scene
