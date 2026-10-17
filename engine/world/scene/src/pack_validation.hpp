#pragma once

// Pack checks shared by SceneLoader / ObekLoader and World::instantiate_scene / instantiate_obek (ADR-0055 §5,
// ADR-0058). SCEN and OBEK share one record layout per kind (component descriptor, payload view, relation record), so
// each check is a template over the record types. A loader refuses a pack these checks reject, so a corrupt pack is
// never published; instantiation re-checks the views (a hand-built resource can still contradict itself), then the
// records against the World's registry, and spawns nothing when either fails.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/construct.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/serialize.hpp>

#include <cstring>
#include <type_traits>

namespace crd::scene::pack_detail
{
// The null entity index of every pack record (kSceneNullTargetIdx, kObekNullTargetIdx, kObekNullParentIdx).
inline constexpr crd::u32 kNullIndex = 0xFFFFFFFFU;

// A record is staged in place when its component fits this many bytes at this alignment.
inline constexpr crd::usize kInlineStagingBytes = 1024U;
inline constexpr crd::usize kInlineStagingAlignment = 16U;

// Copies the `count` records of T a chunk holds, and nothing else, into typed storage, so no record is read at the
// alignment the chunk's bytes happen to have (a compressed chunk decodes into a packed backing, so its payload can sit
// at any offset). False when the chunk holds anything but exactly `count` records.
template <typename T>
[[nodiscard]] bool copy_records(const crd::resources::CrdrChunk& chunk, crd::usize count,
                                crd::containers::Array<T>& out)
{
    static_assert(std::is_trivially_copyable_v<T>, "pack records are copied as bytes");
    crd::usize bytes = 0U;
    if (!crd::memory::checked_mul(count, sizeof(T), &bytes) || chunk.payload.size() != bytes)
    {
        return false;
    }
    out.resize(count);
    if (bytes > 0U)
    {
        std::memcpy(out.data(), chunk.payload.data(), bytes);
    }
    return true;
}

// A descriptor a pack may carry: a non-zero FourCC, a non-zero size that is a multiple of its power-of-two
// alignment, and a known storage hint. (Empty components are never persisted.)
template <typename Descriptor> [[nodiscard]] bool descriptor_is_valid(const Descriptor& d) noexcept
{
    return d.fourcc != 0U && d.size != 0U && crd::memory::is_pow2(d.alignment) && d.size % d.alignment == 0U &&
           d.storage_hint <= static_cast<crd::u8>(StorageHint::SparseSet);
}

// Where a payload chunk's entity indices start (after u32 record_count and u32 reserved).
inline constexpr crd::usize kPayloadIndicesOffset = 2U * sizeof(crd::u32);

// The layout of a payload chunk (SCEN 'C###', OBEK 'D###'): u32 record_count, u32 reserved, u32 indices[record_count],
// padding to the descriptor's alignment, then record_count records of the descriptor's size, and nothing after them.
// `d` must satisfy descriptor_is_valid. True, with where the records start and their byte size, when the chunk is
// exactly that and its record_count is the descriptor's.
template <typename Descriptor>
[[nodiscard]] bool payload_chunk_layout(const crd::resources::CrdrChunk& chunk, const Descriptor& d,
                                        crd::usize* payloads_start, crd::usize* payloads_bytes) noexcept
{
    const auto bytes = chunk.payload;
    if (bytes.size() < kPayloadIndicesOffset)
    {
        return false;
    }
    crd::u32 record_count = 0U;
    std::memcpy(&record_count, bytes.data(), sizeof(record_count));
    if (record_count != d.record_count)
    {
        return false;
    }
    crd::usize indices_bytes = 0U;
    crd::usize indices_end = 0U;
    crd::usize end = 0U;
    const bool fits = crd::memory::checked_mul(record_count, sizeof(crd::u32), &indices_bytes) &&
                      crd::memory::checked_add(kPayloadIndicesOffset, indices_bytes, &indices_end) &&
                      crd::memory::checked_align_up(indices_end, d.alignment, payloads_start) &&
                      crd::memory::checked_mul(record_count, d.size, payloads_bytes) &&
                      crd::memory::checked_add(*payloads_start, *payloads_bytes, &end);
    return fits && end == bytes.size();
}

// Reads every payload chunk of a pack whose descriptors are valid: the records of descriptor ci stay a view into the
// chunk (copied out with memcpy at instantiation), and all entity indices are copied into one typed table that the
// views' `indices` point into. `chunk_for(ci)` is the descriptor's chunk or null; a descriptor has a chunk exactly
// when it has records. False when a chunk is missing, unexpected or not exactly its layout.
template <typename Descriptor, typename Payload, typename ChunkFor>
[[nodiscard]] bool read_payload_chunks(crd::containers::ConstSpan<Descriptor> descriptors, ChunkFor chunk_for,
                                       crd::containers::Array<Payload>& payloads,
                                       crd::containers::Array<crd::u32>& indices)
{
    if (descriptors.size() > kMaxComponents)
    {
        return false;
    }
    const crd::resources::CrdrChunk* chunks[kMaxComponents] = {};
    crd::usize starts[kMaxComponents] = {};
    crd::usize sizes[kMaxComponents] = {};
    crd::usize total_records = 0U;
    for (crd::usize ci = 0U; ci < descriptors.size(); ++ci)
    {
        const Descriptor& d = descriptors[ci];
        chunks[ci] = chunk_for(static_cast<crd::u32>(ci));
        if (chunks[ci] == nullptr ? d.record_count != 0U
                                  : !payload_chunk_layout(*chunks[ci], d, &starts[ci], &sizes[ci]))
        {
            return false;
        }
        total_records += d.record_count; // each count is backed by its chunk's bytes, so the sum cannot wrap
    }
    payloads.resize(descriptors.size());
    indices.resize(total_records);
    crd::usize next = 0U;
    for (crd::usize ci = 0U; ci < descriptors.size(); ++ci)
    {
        const crd::u32 count = descriptors[ci].record_count;
        const crd::resources::CrdrChunk* const chunk = chunks[ci];
        if (chunk == nullptr || count == 0U)
        {
            continue;
        }
        std::memcpy(indices.data() + next, chunk->payload.data() + kPayloadIndicesOffset,
                    static_cast<crd::usize>(count) * sizeof(crd::u32));
        payloads[ci].indices = crd::containers::ConstSpan<crd::u32>{indices.data() + next, count};
        payloads[ci].payloads = crd::containers::ConstSpan<crd::u8>{chunk->payload.data() + starts[ci], sizes[ci]};
        next += count;
    }
    return true;
}

// The component views instantiation reads: at most kMaxComponents valid descriptors with pairwise distinct FourCCs,
// one payload view per descriptor holding exactly its records, whose entity indices are strictly ascending (so no
// entity gets one component twice) and below `entity_count`.
template <typename Descriptor, typename Payload>
[[nodiscard]] bool components_are_consistent(crd::u32 entity_count, crd::containers::ConstSpan<Descriptor> descriptors,
                                             crd::containers::ConstSpan<Payload> payloads) noexcept
{
    if (descriptors.size() > kMaxComponents || payloads.size() != descriptors.size())
    {
        return false;
    }
    for (crd::usize ci = 0U; ci < descriptors.size(); ++ci)
    {
        const Descriptor& d = descriptors[ci];
        if (!descriptor_is_valid(d))
        {
            return false;
        }
        for (crd::usize prior = 0U; prior < ci; ++prior)
        {
            if (descriptors[prior].fourcc == d.fourcc)
            {
                return false;
            }
        }
        const Payload& p = payloads[ci];
        crd::usize payload_bytes = 0U;
        if (p.indices.size() != d.record_count || !crd::memory::checked_mul(d.record_count, d.size, &payload_bytes) ||
            p.payloads.size() != payload_bytes)
        {
            return false;
        }
        for (crd::usize r = 0U; r < p.indices.size(); ++r)
        {
            if (p.indices[r] >= entity_count || (r > 0U && p.indices[r] <= p.indices[r - 1U]))
            {
                return false;
            }
        }
    }
    return true;
}

// Every relation record names a non-zero FourCC, a source below `entity_count` and a target that is null or below it;
// the records ascend by source, and one source names each relation FourCC once (an entity carries one Relation<Tag>
// per tag, so at most kMaxComponents records), as the builders write them. Without a repeated source a restore never
// replaces an edge, so an acyclic final graph is acyclic at every step of the restore.
template <typename Relation>
[[nodiscard]] bool relations_are_consistent(crd::u32 entity_count,
                                            crd::containers::ConstSpan<Relation> relations) noexcept
{
    crd::usize run_start = 0U;
    for (crd::usize i = 0U; i < relations.size(); ++i)
    {
        const Relation& rec = relations[i];
        if (rec.relation_fourcc == 0U || rec.src_file_idx >= entity_count ||
            (rec.target_file_idx != kNullIndex && rec.target_file_idx >= entity_count))
        {
            return false;
        }
        if (i > 0U && rec.src_file_idx != relations[i - 1U].src_file_idx)
        {
            if (rec.src_file_idx < relations[i - 1U].src_file_idx)
            {
                return false;
            }
            run_start = i;
        }
        if (i - run_start >= kMaxComponents)
        {
            return false;
        }
        for (crd::usize prior = run_start; prior < i; ++prior)
        {
            if (relations[prior].relation_fourcc == rec.relation_fourcc)
            {
                return false;
            }
        }
    }
    return true;
}

// The registered component whose ComponentSerialize FourCC is `fourcc`, or the null id; 0, the FourCC of every
// component registered without a serialize trait, names none.
[[nodiscard]] ComponentId find_by_fourcc(const ComponentRegistry& registry, crd::u32 fourcc) noexcept;

// The registered component a payload record restores, or the null id: an unknown FourCC (forward compatibility), a
// relation component (relations are restored from the relation records) and, when `skip_dont_inherit`, a DontInherit
// component restore nothing.
[[nodiscard]] ComponentId restored_component(const ComponentRegistry& registry, crd::u32 fourcc,
                                             bool skip_dont_inherit) noexcept;

// The first descriptor whose restored component differs from it in size, alignment or ComponentSerialize::version.
template <typename Descriptor>
[[nodiscard]] PackRefusal check_layouts(const ComponentRegistry& registry,
                                        crd::containers::ConstSpan<Descriptor> descriptors, bool skip_dont_inherit,
                                        crd::u32* refused_fourcc) noexcept
{
    for (const Descriptor& d : descriptors)
    {
        const ComponentId id = restored_component(registry, d.fourcc, skip_dont_inherit);
        if (id.is_null())
        {
            continue;
        }
        const ComponentInfo& info = registry.require_info(id);
        PackRefusal refusal = PackRefusal::None;
        if (info.size != d.size)
        {
            refusal = PackRefusal::ComponentSizeMismatch;
        }
        else if (info.alignment != d.alignment)
        {
            refusal = PackRefusal::ComponentAlignmentMismatch;
        }
        else if (info.serialize.version != d.version)
        {
            refusal = PackRefusal::ComponentVersionMismatch;
        }
        if (refusal != PackRefusal::None)
        {
            *refused_fourcc = d.fourcc;
            return refusal;
        }
    }
    return PackRefusal::None;
}

// RelationCycle when the records of an Acyclic relation registered in `registry` form a cycle among the pack's
// entities (a self-relation included). The records must satisfy relations_are_consistent, so each source has at most
// one edge per relation: a functional graph, walked once. Each Acyclic relation present costs one pass over the
// records and one walk over the entities, in two scratch arrays from `alloc`.
template <typename Relation>
[[nodiscard]] PackRefusal check_cycles(const ComponentRegistry& registry, crd::u32 entity_count,
                                       crd::containers::ConstSpan<Relation> relations, crd::memory::IAllocator* alloc,
                                       crd::u32* refused_fourcc)
{
    constexpr crd::u8 unvisited = 0U;
    constexpr crd::u8 on_walk = 1U;
    constexpr crd::u8 settled = 2U;

    crd::u64 checked[kMaxComponents / 64U] = {};
    crd::containers::Array<crd::u32> next{alloc};
    crd::containers::Array<crd::u8> state{alloc};
    for (const Relation& first : relations)
    {
        const ComponentId id = find_by_fourcc(registry, first.relation_fourcc);
        if (id.is_null())
        {
            continue;
        }
        crd::u64& word = checked[id.raw / 64U];
        const crd::u64 bit = crd::u64{1} << (id.raw % 64U);
        if ((word & bit) != 0U)
        {
            continue;
        }
        word |= bit;
        const ComponentInfo& info = registry.require_info(id);
        if (!info.is_relation || !info.acyclic)
        {
            continue;
        }
        next.clear();
        next.resize(entity_count, kNullIndex);
        for (const Relation& rec : relations)
        {
            if (rec.relation_fourcc == first.relation_fourcc)
            {
                next[rec.src_file_idx] = rec.target_file_idx;
            }
        }
        state.clear();
        state.resize(entity_count, unvisited);
        for (crd::u32 start = 0U; start < entity_count; ++start)
        {
            crd::u32 at = start;
            while (at != kNullIndex && state[at] == unvisited)
            {
                state[at] = on_walk;
                at = next[at];
            }
            if (at != kNullIndex && state[at] == on_walk)
            {
                *refused_fourcc = first.relation_fourcc;
                return PackRefusal::RelationCycle;
            }
            for (crd::u32 v = start; v != kNullIndex && state[v] == on_walk; v = next[v])
            {
                state[v] = settled;
            }
        }
    }
    return PackRefusal::None;
}

// One record staged for insertion (the storage backends take a mutable pointer and move from it): in place when the
// component fits kInlineStagingBytes at kInlineStagingAlignment, otherwise in one block from the World's allocator
// that lives as long as the staging; out of memory there is fatal.
class ComponentStaging
{
public:
    ComponentStaging(crd::memory::IAllocator& alloc, crd::usize size, crd::usize alignment) : m_alloc(&alloc)
    {
        if (size <= kInlineStagingBytes && alignment <= kInlineStagingAlignment)
        {
            m_data = m_inline;
        }
        else
        {
            m_heap = static_cast<crd::u8*>(crd::memory::allocate_or_fatal(
                alloc, size, alignment, "World: out of memory for a component's instantiation staging"));
            m_data = m_heap;
        }
    }

    ~ComponentStaging()
    {
        if (m_heap != nullptr)
        {
            m_alloc->deallocate(m_heap);
        }
    }

    ComponentStaging(const ComponentStaging&) = delete;
    ComponentStaging& operator=(const ComponentStaging&) = delete;
    ComponentStaging(ComponentStaging&&) = delete;
    ComponentStaging& operator=(ComponentStaging&&) = delete;

    [[nodiscard]] crd::u8* data() noexcept { return m_data; }

private:
    alignas(kInlineStagingAlignment) crd::u8 m_inline[kInlineStagingBytes]{};
    crd::memory::IAllocator* m_alloc;
    crd::u8* m_heap = nullptr;
    crd::u8* m_data = nullptr;
};
} // namespace crd::scene::pack_detail
