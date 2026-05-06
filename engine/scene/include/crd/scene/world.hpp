#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/slot_map.hpp>

namespace crd::scene
{
// World — the root container for an ECS scene. Phase 3.0 v1a ships only the
// entity-identity layer (this file): a SlotMap plus a deferred-destroy queue.
//
// Subsequent v1b–v1n slices grow this class with component registry, storage
// backends, relations, query DSL, schedule, and indexes. All of those layers
// see a stable EntityId minted here.
//
// Lifecycle (per ADR-0049):
//   spawn()              — synchronously allocates a slot and returns the handle.
//   destroy(e)           — queues `e` for destruction; the slot stays alive
//                          until flush_destroys() runs.
//   destroy_immediate(e) — frees the slot synchronously. Caller asserts no
//                          parallel iteration is in flight.
//   flush_destroys()     — drains the queue once (typically end-of-frame).
//                          Stale handles in the queue are silently skipped, so
//                          a double-destroy is safe.
class World
{
public:
    explicit World(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = default;
    World& operator=(World&&) = default;

    // ---- Entity lifecycle ----------------------------------------------

    [[nodiscard]] EntityId spawn();

    void destroy(EntityId e);

    void destroy_immediate(EntityId e);

    void flush_destroys();

    // ---- Queries -------------------------------------------------------

    [[nodiscard]] bool is_alive(EntityId e) const noexcept { return m_slots.is_alive(e); }

    [[nodiscard]] crd::u32 entity_count() const noexcept { return m_slots.alive_count(); }

    [[nodiscard]] crd::u32 pending_destroy_count() const noexcept
    {
        return static_cast<crd::u32>(m_pending_destroy.size());
    }

    // Range over alive entities. Order matches slot index.
    [[nodiscard]] SlotMap::Iterator begin() const noexcept { return m_slots.begin(); }
    [[nodiscard]] SlotMap::Iterator end() const noexcept { return m_slots.end(); }

private:
    SlotMap m_slots;
    crd::containers::Array<EntityId> m_pending_destroy;
};

} // namespace crd::scene
