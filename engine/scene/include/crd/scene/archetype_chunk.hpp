#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// 16 KB chunks, 64-byte SoA alignment per ADR-0050 §2. Sized to fit comfortably
// inside an L1 cache line block — header + currently-iterated component arrays
// stay hot during a chunk-grain visitor pass.
inline constexpr crd::usize kChunkSize = 16 * 1024;
inline constexpr crd::usize kChunkAlignment = 64;

// Cap on archetype width. Per-archetype, not per-World — a small ceiling keeps
// the per-chunk version-counter array bounded (256 B at 32 entries × u64).
// Most archetypes carry 2–8 components in practice; 32 is comfortable headroom.
// Layouts that exceed this return invalid (entity_capacity == 0).
inline constexpr crd::u32 kMaxComponentsPerArchetype = 32;

// ChunkHeader — at byte 0 of every 16 KB chunk. Followed by SoA arrays:
//   [ChunkHeader] [pad to 64B] [EntityId[N]] [pad] [Component0[N]] [pad] ... [pad] [ComponentK-1[N]]
//
// `version_counter` is indexed by *layout-local* component index (0..K-1 where K
// is this archetype's component count). The mapping (layout-local index ↔
// ComponentId) lives in `ChunkLayout::components_sorted`. ChangeDetectIndex
// (Phase 3.0 v1i) reads these counters to gate `.changed<T>()` queries.
struct ChunkHeader
{
    crd::u16 entity_count = 0;
    crd::u16 entity_capacity = 0;
    crd::u32 archetype_id = 0; // back-reference; populated by Archetype in v1c2
    crd::u64 version_counter[kMaxComponentsPerArchetype]{};
};

// ChunkLayout — once-computed plan for an archetype's in-chunk byte arrangement.
//
//   components_sorted : ComponentIds in ascending order (canonical archetype identity)
//   sizes[i]          : sizeof(component) for components_sorted[i]
//   alignments[i]     : alignof(component) for components_sorted[i]
//   offsets[i]        : byte offset within the chunk where the i-th SoA array starts
//   entity_id_offset  : byte offset of the EntityId[] array (always present)
//   entity_capacity   : N entities a single chunk holds. 0 ⇒ invalid (caller error)
//
// Sentinel `entity_capacity == 0` covers two failure modes:
//   - components_sorted.size() exceeds kMaxComponentsPerArchetype
//   - sum of per-entity bytes exceeds the chunk body (one entity does not fit)
//
// Caller (Archetype in v1c2) decides what to do — registration-time rejection
// or runtime CRD_FATAL.
struct ChunkLayout
{
    crd::containers::Array<ComponentId> components_sorted;
    crd::containers::Array<crd::u32> sizes;
    crd::containers::Array<crd::u32> alignments;
    crd::containers::Array<crd::u32> offsets;
    crd::u32 entity_id_offset = 0;
    crd::u32 entity_capacity = 0;

    explicit ChunkLayout(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    [[nodiscard]] bool is_valid() const noexcept { return entity_capacity > 0; }

    [[nodiscard]] crd::u32 component_count() const noexcept { return static_cast<crd::u32>(components_sorted.size()); }
};

// Compute a chunk layout for an archetype identified by `mask`. Components are
// pulled from the registry (size + alignment). The returned layout is owned by
// the caller. Failure paths (too many components, single entity too big) return
// a layout with entity_capacity == 0; caller checks `is_valid()`.
[[nodiscard]] ChunkLayout compute_chunk_layout(const ComponentMask& mask, const ComponentRegistry& registry,
                                               crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// Chunk — owns a 16 KB aligned memory block. Pure data + accessors. Entity
// lifecycle (add/remove, swap-with-last, version bump) lives on Archetype in
// v1c2; Chunk is intentionally minimal so that v1c2 doesn't have to refactor
// it.
struct Chunk
{
    void* memory = nullptr; // 64-byte aligned; null when default-constructed

    [[nodiscard]] ChunkHeader* header() const noexcept { return static_cast<ChunkHeader*>(memory); }

    // Pointer to the EntityId[] array (offset baked into the layout).
    [[nodiscard]] EntityId* entity_id_array(const ChunkLayout& layout) const noexcept
    {
        return reinterpret_cast<EntityId*>(static_cast<crd::u8*>(memory) + layout.entity_id_offset);
    }

    // Pointer to the i-th SoA component array (i = layout-local index, 0..K-1).
    [[nodiscard]] void* component_array(const ChunkLayout& layout, crd::u32 layout_index) const noexcept
    {
        return static_cast<crd::u8*>(memory) + layout.offsets[layout_index];
    }
};

// ChunkAllocator — owns the lifetime of every Chunk it hands out. Aligned
// allocate-per-chunk in v1c1; a heap-pooled backing allocator can land later
// without changing the API.
//
// Construction of a chunk zero-initialises the header (so consumers can rely
// on entity_count == 0 and version_counter[]==0 from day one).
class ChunkAllocator
{
public:
    explicit ChunkAllocator(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    ChunkAllocator(const ChunkAllocator&) = delete;
    ChunkAllocator& operator=(const ChunkAllocator&) = delete;
    ChunkAllocator(ChunkAllocator&&) noexcept;
    ChunkAllocator& operator=(ChunkAllocator&&) noexcept;

    ~ChunkAllocator();

    [[nodiscard]] Chunk allocate();

    // Free a chunk obtained from this allocator. The chunk's memory is returned
    // to the underlying IAllocator. After `free`, the Chunk's `memory` is
    // cleared to nullptr.
    void free(Chunk& chunk) noexcept;

    [[nodiscard]] crd::u32 outstanding() const noexcept;

private:
    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<void*> m_blocks; // book-keeping for dtor cleanup
};

} // namespace crd::scene
