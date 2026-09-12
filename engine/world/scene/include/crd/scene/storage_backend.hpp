#pragma once

#include <crd/core/types.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// One chunk of components handed to a query visitor.
//
// GEO-7 (D-007 row 72) delivers the SoA surface v1b promised: backends populate the per-component pointer table +
// per-component chunk version counters, so chunk-grain systems (transform extract, culling, draw submission) walk
// component arrays DIRECTLY — never per-entity handle chasing. The archetype backend fills the table from its
// ChunkLayout; the sparse backend fills a single-entry table from the pool's dense array (except CoW-shared pools,
// whose dense bytes are not authoritative — those yield an empty table and consumers fall back per-entity).
inline constexpr crd::u32 kMaxChunkViewComponents = 32; // == kMaxComponentsPerArchetype (archetype_chunk.hpp)

struct ChunkView
{
    const EntityId* entities = nullptr;
    crd::u32 entity_count = 0;
    ComponentMask present_mask{};

    // ── the SoA table (GEO-7) ────────────────────────────────────────────────────────────────────────────────
    // Parallel arrays, layout order. `component_versions[i]` is the chunk-grain change counter for that array
    // (bumped on declared writes — the ChangeDetect signal at chunk grain; the partial-re-upload driver).
    crd::u32    component_count = 0;
    ComponentId component_ids[kMaxChunkViewComponents]{};
    void*       component_arrays[kMaxChunkViewComponents]{};
    crd::u64    component_versions[kMaxChunkViewComponents]{};

    // The SoA array for component `c`, or nullptr when the backend did not surface one (CoW-shared sparse pool,
    // or `c` absent). O(component_count) linear scan — tables are 2..8 entries in practice.
    [[nodiscard]] void* array_of(ComponentId c) const noexcept
    {
        for (crd::u32 i = 0; i < component_count; ++i)
        {
            if (component_ids[i] == c) { return component_arrays[i]; }
        }
        return nullptr;
    }

    // The chunk-grain version counter for component `c` (0 when absent from the table).
    [[nodiscard]] crd::u64 version_of(ComponentId c) const noexcept
    {
        for (crd::u32 i = 0; i < component_count; ++i)
        {
            if (component_ids[i] == c) { return component_versions[i]; }
        }
        return 0;
    }

    // Typed convenience over `array_of`.
    template <typename T> [[nodiscard]] T* array(ComponentId c) const noexcept
    {
        return static_cast<T*>(array_of(c));
    }
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
    // `data` points to a fully-constructed component value. Storage
    // implementations may move-from it (e.g. via the registry's
    // move_construct callback). Caller's source value is left in a
    // moved-from state and must not be read afterwards.
    virtual void insert(EntityId e, ComponentId c, void* data) = 0;
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
