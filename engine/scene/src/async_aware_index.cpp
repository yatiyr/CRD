// Phase 3.0 v1i — AsyncAwareIndex (ADR-0053 §4).

#include <crd/scene/async_aware_index.hpp>

namespace crd::scene
{

AsyncAwareIndex::AsyncAwareIndex(crd::memory::IAllocator* alloc) : m_state(alloc) {}

void AsyncAwareIndex::set_state(EntityId e, ComponentId c, LoadState s) noexcept
{
    const crd::u64 key = encode_key(c, e);
    if (auto* slot = m_state.find(key); slot != nullptr)
    {
        *slot = s;
    }
    else
    {
        m_state.emplace(key, s);
    }
}

void AsyncAwareIndex::on_insert(EntityId e, ComponentId c, const void* /*data*/)
{
    // Default on insert: Loading. Caller flips to Loaded via mark_loaded
    // when their async work completes.
    set_state(e, c, LoadState::Loading);
}

void AsyncAwareIndex::on_update(EntityId e, ComponentId c, const void* /*old_data*/, const void* /*new_data*/)
{
    // UPSERT replays as a fresh insert from the index's POV — the new
    // payload may be a re-stream of the same asset, restart the
    // Loading lifecycle. Callers that want to keep the previous Loaded
    // state across in-place updates should NOT mutate AsyncAware
    // components in place.
    set_state(e, c, LoadState::Loading);
}

void AsyncAwareIndex::on_remove(EntityId e, ComponentId c, const void* /*data*/)
{
    m_state.erase(encode_key(c, e));
}

void AsyncAwareIndex::on_entity_destroyed(EntityId e)
{
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
        m_state.erase((static_cast<crd::u64>(c_raw) << 48) | entity_bits);
    }
}

void AsyncAwareIndex::mark_loading(EntityId e, ComponentId c) noexcept
{
    set_state(e, c, LoadState::Loading);
}

void AsyncAwareIndex::mark_loaded(EntityId e, ComponentId c) noexcept
{
    set_state(e, c, LoadState::Loaded);
}

void AsyncAwareIndex::mark_failed(EntityId e, ComponentId c) noexcept
{
    set_state(e, c, LoadState::Failed);
}

LoadState AsyncAwareIndex::load_state(EntityId e, ComponentId c) const noexcept
{
    const auto* slot = m_state.find(encode_key(c, e));
    if (slot == nullptr)
    {
        // No tracked state → caller treats as Loading by default (component
        // not present, or never had an event fire).
        return LoadState::Loading;
    }
    return *slot;
}

bool AsyncAwareIndex::is_pending(EntityId e, ComponentId c) const noexcept
{
    return load_state(e, c) == LoadState::Loading;
}

bool AsyncAwareIndex::is_loaded(EntityId e, ComponentId c) const noexcept
{
    return load_state(e, c) == LoadState::Loaded;
}

} // namespace crd::scene
