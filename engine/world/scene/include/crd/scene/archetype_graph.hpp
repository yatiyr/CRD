#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/scene/archetype.hpp>
#include <crd/scene/archetype_chunk.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>

namespace crd::scene
{
// Hash functor for ComponentMask (256-bit). Folds the four u64 words with a
// boost-style mix (golden ratio + rotation) so distinct masks differing only
// in the higher words still spread across buckets. ComponentMask has
// value-semantic equality so std::equal_to<> suffices for the HashMap's
// key-equal predicate.
struct ComponentMaskHash
{
    [[nodiscard]] crd::u64 operator()(const ComponentMask& m) const noexcept
    {
        crd::u64 h = crd::containers::hash_u64(m.bits[0]);
        for (crd::u32 i = 1; i < 4; ++i)
        {
            const crd::u64 wh = crd::containers::hash_u64(m.bits[i]);
            h ^= wh + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// ArchetypeGraph — owns every Archetype for one World. Lazy-built.
//
// archetype_for(mask): O(1) average — hash lookup + compare. Miss path
//                      allocates a fresh Archetype, computes its ChunkLayout,
//                      registers it. CRD_FATAL on layout invalid.
// after_add(arch, c):  O(1) — checks arch.add_edges[c.raw]. On miss, computes
//                      target mask + archetype_for + caches the edge.
// after_remove(arch,c) O(1) — symmetric to after_add.
//
// Allocator integration:
//   - Member arrays / hash maps go through `m_alloc` (the World's IAllocator).
//   - The Archetype STRUCTS themselves come from a dedicated
//     `GrowablePoolAllocator m_archetype_pool` whose parent is `m_alloc`.
//     This pools Archetype allocations (1000 archetypes → ~32 pages) and
//     keeps every byte of the graph routed through the World's allocator
//     chain — no `std::make_unique` / global `operator new`.
//
// Pointer stability: archetypes live in `m_archetypes`, an Array of raw
// `Archetype*`. Each pointer is allocated from the pool and never moves.
// `ArchetypeId.raw` is the index into m_archetypes; ids never recycle.
class ArchetypeGraph
{
public:
    ArchetypeGraph(crd::memory::IAllocator* alloc, ChunkAllocator& chunks, const ComponentRegistry& registry);

    ~ArchetypeGraph();

    ArchetypeGraph(const ArchetypeGraph&) = delete;
    ArchetypeGraph& operator=(const ArchetypeGraph&) = delete;
    ArchetypeGraph(ArchetypeGraph&&) noexcept;
    ArchetypeGraph& operator=(ArchetypeGraph&&) noexcept;

    // Find or create the archetype with `mask`. Allocates a new chunk for
    // empty archetypes.
    [[nodiscard]] Archetype& archetype_for(const ComponentMask& mask);

    // Edge navigation. Memoised — second call to the same edge is O(1) hash-free.
    [[nodiscard]] Archetype& after_add(Archetype& src, ComponentId added);
    [[nodiscard]] Archetype& after_remove(Archetype& src, ComponentId removed);

    [[nodiscard]] Archetype* by_id(ArchetypeId id) noexcept;
    [[nodiscard]] const Archetype* by_id(ArchetypeId id) const noexcept;

    [[nodiscard]] crd::u32 archetype_count() const noexcept;

    [[nodiscard]] ChunkAllocator& chunks() noexcept { return *m_chunks; }
    [[nodiscard]] const ComponentRegistry& registry() const noexcept { return *m_registry; }

    // Pool diagnostics (mostly useful for tests / smokes).
    [[nodiscard]] crd::usize archetype_pool_pages() const noexcept;

private:
    void destroy_all_archetypes() noexcept;

    crd::memory::IAllocator* m_alloc;
    ChunkAllocator* m_chunks;
    const ComponentRegistry* m_registry;

    // Pool for the Archetype structs. Allocated lazily on first archetype.
    // Parent allocator = m_alloc, so the chain stays intact end-to-end.
    crd::memory::GrowablePoolAllocator m_archetype_pool;
    crd::containers::Array<Archetype*> m_archetypes;
    crd::containers::HashMap<ComponentMask, ArchetypeId, ComponentMaskHash> m_by_mask;
};

} // namespace crd::scene
