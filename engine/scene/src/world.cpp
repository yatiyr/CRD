#include <crd/scene/world.hpp>

namespace crd::scene
{

World::World(crd::memory::IAllocator* alloc) : m_slots(alloc), m_pending_destroy(alloc), m_components(alloc) {}

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
        m_slots.free(e);
    }
}

void World::flush_destroys()
{
    for (EntityId e : m_pending_destroy)
    {
        if (m_slots.is_alive(e))
        {
            m_slots.free(e);
        }
    }
    m_pending_destroy.clear();
}

} // namespace crd::scene
