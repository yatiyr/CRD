#include <crd/scene/world.hpp>

namespace crd::scene
{

World::World(crd::memory::IAllocator* alloc)
    : m_slots(alloc), m_pending_destroy(alloc), m_components(alloc), m_storage(alloc, m_components),
      m_sparse_storage(alloc, m_components), m_event_sink(NullStorageEventSink::instance())
{
}

EntityId World::spawn()
{
    return m_slots.allocate();
}

void World::destroy(EntityId e)
{
    // Stale handles are silently dropped by flush_destroys; queueing one is
    // harmless. Skipping the queue push here would also be valid, but defer
    // is cheaper than a generation check on the hot path.
    m_pending_destroy.push_back(e);
}

void World::destroy_immediate(EntityId e)
{
    if (m_slots.is_alive(e))
    {
        // World fires the singular sink->on_entity_destroyed event ONCE so
        // observers (Layer-5 indexes) see exactly one event per destroy
        // regardless of how many backends hold the entity's components.
        // Both backends then drain their own components, emitting per-
        // component on_remove events through the same sink.
        m_event_sink->on_entity_destroyed(e);
        m_storage.on_entity_destroyed(e);
        m_sparse_storage.on_entity_destroyed(e);
        m_slots.free(e);
    }
}

void World::flush_destroys()
{
    for (EntityId e : m_pending_destroy)
    {
        if (m_slots.is_alive(e))
        {
            m_event_sink->on_entity_destroyed(e);
            m_storage.on_entity_destroyed(e);
            m_sparse_storage.on_entity_destroyed(e);
            m_slots.free(e);
        }
    }
    m_pending_destroy.clear();
}

} // namespace crd::scene
