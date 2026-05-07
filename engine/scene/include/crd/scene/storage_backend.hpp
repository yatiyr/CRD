#pragma once

#include <crd/core/types.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// One chunk of components handed to a query visitor.
//
// v1b declares the type only. The actual layout (per-component SoA pointer
// table, entity-id array, version counters) is populated by the storage
// backends when they ship in v1c (ArchetypeChunkStorage) and v1d
// (SparseSetStorage). Visitors that target a single backend may downcast
// for backend-specific optimisations; the abstract view here is what the
// query DSL (v1g) walks generically.
struct ChunkView
{
    const EntityId* entities = nullptr;
    crd::u32 entity_count = 0;
    ComponentMask present_mask{};

    // v1b reserves only the abstract interface — backends populate
    // per-component pointer tables in their respective slices.
};

using ChunkVisitor = void (*)(const ChunkView& chunk, void* user_data);

// IStorageBackend — the unified interface behind which Archetype and SparseSet
// backends live. ADR-0050.
//
// v1b declares the interface; no implementations exist yet. The query layer
// (v1g) and the index dispatcher (v1i) are written against this interface
// and will work transparently against either backend. v1c lands
// ArchetypeChunkStorage; v1d lands SparseSetStorage; v1e lands the mixed-backend
// chunk visitor that walks both uniformly.
class IStorageBackend
{
public:
    virtual ~IStorageBackend() = default;

    // Component lifecycle
    virtual void insert(EntityId e, ComponentId c, const void* data) = 0;
    virtual void remove(EntityId e, ComponentId c) = 0;
    [[nodiscard]] virtual bool has(EntityId e, ComponentId c) const = 0;
    [[nodiscard]] virtual void* get_mut(EntityId e, ComponentId c) = 0;

    // Bulk iteration. Visitor sees one storage chunk at a time; the query layer
    // (v1g) hands chunks to par_each / range-for / vectorised loops.
    virtual void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data) = 0;

    // Driven by SlotMap::flush_destroys (ADR-0049). Storage is responsible for
    // releasing all components owned by `e`. Indexes (Layer 5) receive the same
    // notification through a separate dispatch.
    virtual void on_entity_destroyed(EntityId e) = 0;
};

} // namespace crd::scene
