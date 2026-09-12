#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype_chunk.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// Per-World archetype identifier. Stable for the life of the World; allocated
// monotonically by ArchetypeGraph in registration order. Index 0xFFFFFFFFU
// is the null sentinel — entities with no archetype-stored components carry
// this value in their EntityLocation.
struct ArchetypeId
{
    crd::u32 raw = 0xFFFFFFFFU;

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0xFFFFFFFFU; }
    [[nodiscard]] constexpr bool operator==(const ArchetypeId&) const noexcept = default;
};

inline constexpr ArchetypeId kInvalidArchetypeId{0xFFFFFFFFU};

// Per-entity location into archetype-storage. Indexed by EntityId::index() in
// ArchetypeChunkStorage's m_locations array. An entity with no
// archetype-stored components has archetype == kInvalidArchetypeId.
//
// 16 bytes (12 logical + 4 padding); cache-friendly for the lookup hot path.
struct EntityLocation
{
    ArchetypeId archetype = kInvalidArchetypeId;
    crd::u32 chunk_index = 0;
    crd::u16 slot_in_chunk = 0;
    crd::u16 _reserved = 0;
};

// Archetype — owns the chunks for one unique ComponentMask.
//
// Edge tables (`add_edges`, `remove_edges`) are dense `Array<ArchetypeId>` of
// size kMaxComponents, indexed by ComponentId.raw. 1 KB per archetype on a
// 64-bit system. The trade-off vs HashMap<ComponentId, ArchetypeId>: the array
// gives a guaranteed O(1) indexed load on the hot add/remove-component path
// with no allocation, no hashing, no probe loop. At 1000 archetypes (a large
// scene), 1 MB of edge metadata — invisible.
//
// The empty entry sentinel is `kInvalidArchetypeId` (matches default).
//
// Chunks: dense Array, never sparse. Insertion always goes to the last chunk;
// when the last chunk fills, a new one is allocated and pushed back. swap_remove
// from any chunk; if the *last* chunk drops to 0 entities, it's freed back to
// the ChunkAllocator (matches Bevy's strategy — avoids fragmentation, doesn't
// thrash mid-archetype chunks).
struct Archetype
{
    ArchetypeId id{};
    ComponentMask mask{};
    ChunkLayout layout;

    crd::containers::Array<Chunk> chunks;
    crd::containers::Array<ArchetypeId> add_edges;    // size = kMaxComponents
    crd::containers::Array<ArchetypeId> remove_edges; // size = kMaxComponents

    explicit Archetype(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    // Number of entities currently held across all chunks.
    [[nodiscard]] crd::u32 entity_count() const noexcept;
};

} // namespace crd::scene
