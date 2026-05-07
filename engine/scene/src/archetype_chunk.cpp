#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/scene/archetype_chunk.hpp>

#include <cstring>
#include <utility>

namespace crd::scene
{

namespace
{
constexpr crd::usize align_up(crd::usize off, crd::usize alignment) noexcept
{
    return (off + alignment - 1) & ~(alignment - 1);
}

// Test whether a candidate `capacity` fits inside one chunk for the given
// component plan. Writes offsets[] and entity_id_offset into `out` only on
// success; on failure `out` is left in an indeterminate state and the caller
// reduces capacity.
[[nodiscard]] bool layout_fits(crd::u32 capacity, crd::containers::ConstSpan<crd::u32> sizes,
                               crd::u32& out_entity_id_offset, crd::containers::Array<crd::u32>& out_offsets) noexcept
{
    crd::usize off = sizeof(ChunkHeader);
    off = align_up(off, kChunkAlignment);
    if (off > kChunkSize)
    {
        return false;
    }

    out_entity_id_offset = static_cast<crd::u32>(off);
    off += static_cast<crd::usize>(capacity) * sizeof(EntityId);
    if (off > kChunkSize)
    {
        return false;
    }

    out_offsets.clear();
    for (crd::u32 size_bytes : sizes)
    {
        off = align_up(off, kChunkAlignment);
        if (off > kChunkSize)
        {
            return false;
        }
        out_offsets.push_back(static_cast<crd::u32>(off));
        off += static_cast<crd::usize>(capacity) * size_bytes;
        if (off > kChunkSize)
        {
            return false;
        }
    }

    return true;
}
} // namespace

// ---- ChunkLayout ---------------------------------------------------------

ChunkLayout::ChunkLayout(crd::memory::IAllocator* alloc)
    : components_sorted(alloc), sizes(alloc), alignments(alloc), offsets(alloc)
{
}

// ---- compute_chunk_layout ------------------------------------------------

ChunkLayout compute_chunk_layout(const ComponentMask& mask, const ComponentRegistry& registry,
                                 crd::memory::IAllocator* alloc)
{
    ChunkLayout layout{alloc};

    // Walk ComponentIds in ascending order; emit only those present in `mask`.
    // This guarantees the canonical sorted order regardless of registration
    // order.
    for (crd::u16 raw = 0; raw < static_cast<crd::u16>(kMaxComponents); ++raw)
    {
        ComponentId id{raw};
        if (!mask.test(id))
        {
            continue;
        }
        const ComponentInfo* info = registry.info(id);
        if (info == nullptr)
        {
            // mask references a never-registered ComponentId. Caller bug;
            // signal by returning an invalid layout (capacity 0).
            return ChunkLayout{alloc};
        }
        layout.components_sorted.push_back(id);
        layout.sizes.push_back(static_cast<crd::u32>(info->size));
        layout.alignments.push_back(static_cast<crd::u32>(info->alignment));
    }

    if (layout.components_sorted.size() > kMaxComponentsPerArchetype)
    {
        return ChunkLayout{alloc};
    }

    // Conservative initial estimate: assume worst-case 63 bytes alignment
    // padding before each SoA array. Decrement until the layout actually
    // fits inside `kChunkSize`.
    const crd::u32 comp_count = static_cast<crd::u32>(layout.components_sorted.size());
    crd::u32 per_entity = static_cast<crd::u32>(sizeof(EntityId));
    for (crd::u32 s : layout.sizes)
    {
        per_entity += s;
    }

    if (per_entity == 0)
    {
        // No components and we still have entity ids — sized purely by the
        // entity-id array.
        per_entity = static_cast<crd::u32>(sizeof(EntityId));
    }

    const crd::usize header_padded = align_up(sizeof(ChunkHeader), kChunkAlignment);
    const crd::usize alignment_overhead = static_cast<crd::usize>(comp_count) * (kChunkAlignment - 1);
    const crd::usize body_budget =
        kChunkSize > (header_padded + alignment_overhead) ? kChunkSize - header_padded - alignment_overhead : 0;

    crd::u32 capacity = static_cast<crd::u32>(body_budget / per_entity);

    while (capacity > 0)
    {
        if (layout_fits(capacity, layout.sizes, layout.entity_id_offset, layout.offsets))
        {
            layout.entity_capacity = capacity;
            return layout;
        }
        --capacity;
    }

    // entity_capacity stays 0 → invalid layout
    return ChunkLayout{alloc};
}

// ---- ChunkAllocator ------------------------------------------------------

ChunkAllocator::ChunkAllocator(crd::memory::IAllocator* alloc) : m_alloc(alloc), m_blocks(alloc) {}

ChunkAllocator::ChunkAllocator(ChunkAllocator&& other) noexcept
    : m_alloc(other.m_alloc), m_blocks(std::move(other.m_blocks))
{
    other.m_alloc = nullptr;
}

ChunkAllocator& ChunkAllocator::operator=(ChunkAllocator&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    // Free anything we still hold before adopting the rhs's blocks.
    for (void* p : m_blocks)
    {
        if (m_alloc != nullptr && p != nullptr)
        {
            m_alloc->deallocate(p);
        }
    }
    m_blocks.clear();

    m_alloc = other.m_alloc;
    m_blocks = std::move(other.m_blocks);
    other.m_alloc = nullptr;
    return *this;
}

ChunkAllocator::~ChunkAllocator()
{
    for (void* p : m_blocks)
    {
        if (m_alloc != nullptr && p != nullptr)
        {
            m_alloc->deallocate(p);
        }
    }
}

Chunk ChunkAllocator::allocate()
{
    CRD_ASSERT(m_alloc != nullptr);
    void* mem = m_alloc->allocate(kChunkSize, kChunkAlignment);
    CRD_ASSERT(mem != nullptr);

    // Zero header so consumers can rely on entity_count == 0 and version
    // counters == 0 from day one. We zero only the header — SoA bytes stay
    // uninitialised until populated.
    std::memset(mem, 0, sizeof(ChunkHeader));

    m_blocks.push_back(mem);
    return Chunk{mem};
}

void ChunkAllocator::free(Chunk& chunk) noexcept
{
    if (chunk.memory == nullptr)
    {
        return;
    }
    for (crd::usize i = 0; i < m_blocks.size(); ++i)
    {
        if (m_blocks[i] == chunk.memory)
        {
            m_alloc->deallocate(m_blocks[i]);
            m_blocks.swap_remove(i);
            chunk.memory = nullptr;
            return;
        }
    }
    // Caller passed a chunk we don't own — programming error.
    CRD_ASSERT(false);
}

crd::u32 ChunkAllocator::outstanding() const noexcept
{
    return static_cast<crd::u32>(m_blocks.size());
}

} // namespace crd::scene
