#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype_chunk_storage.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/slot_map.hpp>
#include <crd/scene/sparse_set_storage.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/storage_event_sink.hpp>

#include <utility>

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
    World(World&&) = delete; // m_storage references *this; non-movable
    World& operator=(World&&) = delete;

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

    // ---- Component registry --------------------------------------------
    // Phase 3.0 v1b: registration grammar. Storage backends and indexes that
    // act on the registered metadata land in v1c–v1i. ADRs 0050, 0053, 0056.

    template <typename T, typename... Traits> ComponentId register_component(Traits&&... traits)
    {
        return m_components.register_type<T>(std::forward<Traits>(traits)...);
    }

    [[nodiscard]] const ComponentInfo* component_info(ComponentId id) const noexcept { return m_components.info(id); }

    template <typename T> [[nodiscard]] ComponentId component_id() const noexcept { return m_components.id_of<T>(); }

    [[nodiscard]] crd::u16 registered_component_count() const noexcept { return m_components.size(); }

    [[nodiscard]] const ComponentRegistry& components() const noexcept { return m_components; }

    // ---- Typed component API (Phase 3.0 v1c2; ADR-0050) ---------------
    //
    // add_component<T>(e, value)    UPSERT — value replaces any prior T on e.
    //                                Asserts T is registered + e is alive.
    // has_component<T>(e)           O(1) — archetype mask test.
    // get_component<T>(e)           const access; does not bump version.
    // get_component_mut<T>(e)       mutable access; bumps chunk version on entry.
    // remove_component<T>(e)        moves e to (mask & ~T) archetype.

    template <typename T> ComponentId require_component_id() const
    {
        ComponentId id = m_components.id_of<T>();
        CRD_ASSERT(!id.is_null());
        return id;
    }

    template <typename T> void add_component(EntityId e, T value)
    {
        CRD_ASSERT(is_alive(e));
        const ComponentId id = require_component_id<T>();
        // `value` is a local; storage may move-from it. Backend selection
        // routes by ComponentInfo::storage_hint (ADR-0050).
        backend_for(id).insert(e, id, static_cast<void*>(&value));
    }

    template <typename T> [[nodiscard]] bool has_component(EntityId e) const noexcept
    {
        if (!is_alive(e))
        {
            return false;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return false;
        }
        return backend_for_const(id).has(e, id);
    }

    template <typename T> [[nodiscard]] const T* get_component(EntityId e) const
    {
        if (!is_alive(e))
        {
            return nullptr;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return nullptr;
        }
        const ComponentInfo* info = m_components.info(id);
        CRD_ASSERT(info != nullptr);
        if (info->storage_hint == StorageHint::SparseSet)
        {
            return static_cast<const T*>(m_sparse_storage.get_const(e, id));
        }
        return static_cast<const T*>(m_storage.get_const(e, id));
    }

    template <typename T> [[nodiscard]] T* get_component_mut(EntityId e)
    {
        if (!is_alive(e))
        {
            return nullptr;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return nullptr;
        }
        return static_cast<T*>(backend_for(id).get_mut(e, id));
    }

    template <typename T> void remove_component(EntityId e)
    {
        if (!is_alive(e))
        {
            return;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return;
        }
        backend_for(id).remove(e, id);
    }

    [[nodiscard]] ArchetypeChunkStorage& storage() noexcept { return m_storage; }
    [[nodiscard]] const ArchetypeChunkStorage& storage() const noexcept { return m_storage; }

    [[nodiscard]] SparseSetStorage& sparse_storage() noexcept { return m_sparse_storage; }
    [[nodiscard]] const SparseSetStorage& sparse_storage() const noexcept { return m_sparse_storage; }

    [[nodiscard]] crd::u32 archetype_count() const noexcept { return m_storage.graph().archetype_count(); }

    // Mixed-backend chunk visitor (Phase 3.0 v1e, ADR-0050 §5).
    //
    // Yields chunks containing entities that satisfy `required` ACROSS both
    // storage backends. The DSL (v1g) sits on top of this primitive; callers
    // that already know `required` is pure-archetype or pure-SparseSet should
    // call the per-backend method directly to skip the dispatch.
    //
    // ChunkView semantics by path:
    //   - Pure-archetype path: chunk view forwarded from ArchetypeChunkStorage.
    //     `present_mask = archetype.mask` (a superset of required); `entities`
    //     points into the chunk's entity_id_array.
    //   - Pure-SparseSet single-bit path: forwarded from SparseSetStorage.
    //     `present_mask = {c}`; `entities` points into the pool's entities
    //     array.
    //   - Pure-SparseSet multi-bit OR mixed path: chunk view is constructed
    //     from a stack-local scratch buffer. `present_mask = required` (exact);
    //     `entities` points into the scratch and is valid ONLY for the
    //     duration of the visitor call.
    //
    // Visitors that compare `present_mask` should treat it as a superset of
    // `required` regardless of path. To access component data, prefer
    // `world.get_component_mut<T>(entity)` per entity in the mixed/multi-sparse
    // path — direct chunk-slot indexing only works for the pure-archetype
    // forwarded path.
    //
    // Threading: not thread-safe. par_each across yielded chunks is the
    // expected parallel path (the visitor dispatches one job per chunk).
    void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data);

    // Install one sink across both storage backends. World drives
    // on_entity_destroyed itself (once per destroy), so backends never fire
    // sink->on_entity_destroyed — they only emit per-component on_remove.
    void set_storage_event_sink(IStorageEventSink* sink) noexcept
    {
        m_event_sink = (sink != nullptr) ? sink : NullStorageEventSink::instance();
        m_storage.set_event_sink(m_event_sink);
        m_sparse_storage.set_event_sink(m_event_sink);
    }

private:
    [[nodiscard]] IStorageBackend& backend_for(ComponentId id) noexcept
    {
        const ComponentInfo* info = m_components.info(id);
        CRD_ASSERT(info != nullptr);
        return (info->storage_hint == StorageHint::SparseSet) ? static_cast<IStorageBackend&>(m_sparse_storage)
                                                              : static_cast<IStorageBackend&>(m_storage);
    }

    [[nodiscard]] const IStorageBackend& backend_for_const(ComponentId id) const noexcept
    {
        const ComponentInfo* info = m_components.info(id);
        CRD_ASSERT(info != nullptr);
        return (info->storage_hint == StorageHint::SparseSet) ? static_cast<const IStorageBackend&>(m_sparse_storage)
                                                              : static_cast<const IStorageBackend&>(m_storage);
    }

    SlotMap m_slots;
    crd::containers::Array<EntityId> m_pending_destroy;
    ComponentRegistry m_components;
    ArchetypeChunkStorage m_storage;
    SparseSetStorage m_sparse_storage;
    IStorageEventSink* m_event_sink;
};

} // namespace crd::scene
