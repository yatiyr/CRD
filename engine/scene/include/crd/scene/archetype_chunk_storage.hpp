#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype.hpp>
#include <crd/scene/archetype_chunk.hpp>
#include <crd/scene/archetype_graph.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/storage_event_sink.hpp>

namespace crd::scene
{
// ArchetypeChunkStorage — the primary L2 backend per ADR-0050. Implements
// IStorageBackend.
//
// Owns:
//   - one ChunkAllocator (16 KB blocks, 64-byte aligned)
//   - one ArchetypeGraph (memoised mask → archetype + add/remove edges)
//   - one m_locations array (entity index → EntityLocation), lazy-resized
//
// Mutation contracts:
//   insert(e, c, data)     — UPSERT: if e already has c, the existing slot
//                            is overwritten via destruct + move_construct.
//                            Otherwise e moves to the (mask | c) archetype
//                            and the new slot receives the value. Bumps the
//                            new chunk's version counter for c.
//   remove(e, c)           — Move e to (mask & ~c) archetype; old chunk's
//                            slot is destructed; swap-remove fills the slot
//                            with the chunk's last entity (whose location
//                            updates).
//   has(e, c)              — O(1): location.archetype.mask.test(c).
//   get_mut(e, c)          — Returns void* into the SoA array. Bumps version
//                            (caller-declared write).
//   for_each_chunk(req, ...) — Walks every archetype whose mask is a SUPERSET
//                              of `req` and visits each of its chunks. Visitor
//                              sees ChunkView{entities, count, mask}.
//   on_entity_destroyed(e) — Tears down components in e's current archetype
//                            via swap_remove; clears m_locations[e.idx].
//
// Lifecycle event dispatch (the Cerid signature, ADR-0053):
//   Every mutation calls into m_sink (default = NullStorageEventSink, swap
//   in v1i). Storage never calls IComponentIndex directly — the indirection
//   is what makes adding a new index a one-day job in its consumer phase.
class ArchetypeChunkStorage : public IStorageBackend
{
public:
    ArchetypeChunkStorage(crd::memory::IAllocator* alloc, const ComponentRegistry& registry);
    ~ArchetypeChunkStorage() override = default;

    ArchetypeChunkStorage(const ArchetypeChunkStorage&) = delete;
    ArchetypeChunkStorage& operator=(const ArchetypeChunkStorage&) = delete;
    ArchetypeChunkStorage(ArchetypeChunkStorage&&) = delete; // referenced by World
    ArchetypeChunkStorage& operator=(ArchetypeChunkStorage&&) = delete;

    // ---- IStorageBackend -----------------------------------------------

    void insert(EntityId e, ComponentId c, void* data) override;
    void remove(EntityId e, ComponentId c) override;
    [[nodiscard]] bool has(EntityId e, ComponentId c) const override;
    [[nodiscard]] void* get_mut(EntityId e, ComponentId c) override;
    void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data) override;
    void on_entity_destroyed(EntityId e) override;

    // ---- v1c2 read accessor (read-only; does NOT bump version) --------

    [[nodiscard]] const void* get_const(EntityId e, ComponentId c) const;

    // ---- Diagnostics + introspection ----------------------------------

    [[nodiscard]] EntityLocation location(EntityId e) const noexcept;

    [[nodiscard]] ArchetypeGraph& graph() noexcept { return m_graph; }
    [[nodiscard]] const ArchetypeGraph& graph() const noexcept { return m_graph; }

    // ---- Event sink wiring (v1i replaces with the IComponentIndex fan-out) --

    void set_event_sink(IStorageEventSink* sink) noexcept;
    [[nodiscard]] IStorageEventSink* event_sink() noexcept { return m_sink; }

private:
    // Location-array maintenance
    EntityLocation& ensure_location(EntityId e);
    EntityLocation location_or_invalid(EntityId e) const noexcept;
    void clear_location(EntityId e) noexcept;

    // Place a fresh slot in `dst` and return (chunk_index, slot_in_chunk).
    // Allocates a new chunk if the last chunk is full.
    EntityLocation acquire_slot(Archetype& dst, EntityId entity);

    // swap_remove an entity from a chunk. If a non-trailing entity is removed,
    // the trailing entity moves into its slot — call site updates that
    // entity's location.
    void release_slot(Archetype& src, crd::u32 chunk_index, crd::u16 slot_in_chunk);

    // Move all components shared between src and dst from
    // (src_chunk, src_slot) → (dst_chunk, dst_slot). Source slots are
    // destructed; do not read their bytes after this call.
    void move_shared_components(Archetype& src, Chunk& src_chunk, crd::u16 src_slot, Archetype& dst, Chunk& dst_chunk,
                                crd::u16 dst_slot);

    // Locate the layout-local index of `c` within an archetype's
    // components_sorted. Returns kMaxComponents on miss.
    [[nodiscard]] static crd::u32 layout_index(const Archetype& a, ComponentId c) noexcept;

    ChunkAllocator m_chunks;
    ArchetypeGraph m_graph;
    const ComponentRegistry* m_registry;
    crd::containers::Array<EntityLocation> m_locations;
    IStorageEventSink* m_sink;
};

} // namespace crd::scene
