#include <crd/core/assert.hpp>
#include <crd/scene/archetype_chunk_storage.hpp>

#include <cstring>

namespace crd::scene
{

namespace
{
inline crd::u8* component_slot(Chunk& chunk, const ChunkLayout& layout, crd::u32 layout_index, crd::u16 slot)
{
    return static_cast<crd::u8*>(chunk.component_array(layout, layout_index)) +
           static_cast<crd::usize>(slot) * static_cast<crd::usize>(layout.sizes[layout_index]);
}

inline const crd::u8* component_slot(const Chunk& chunk, const ChunkLayout& layout, crd::u32 layout_index,
                                     crd::u16 slot)
{
    return static_cast<const crd::u8*>(chunk.component_array(layout, layout_index)) +
           static_cast<crd::usize>(slot) * static_cast<crd::usize>(layout.sizes[layout_index]);
}
} // namespace

// ---- Construction --------------------------------------------------------

ArchetypeChunkStorage::ArchetypeChunkStorage(crd::memory::IAllocator* alloc, const ComponentRegistry& registry)
    : m_chunks(alloc), m_graph(alloc, m_chunks, registry), m_registry(&registry), m_locations(alloc),
      m_sink(NullStorageEventSink::instance())
{
}

void ArchetypeChunkStorage::set_event_sink(IStorageEventSink* sink) noexcept
{
    m_sink = (sink != nullptr) ? sink : NullStorageEventSink::instance();
}

// ---- Location-array maintenance ------------------------------------------

EntityLocation& ArchetypeChunkStorage::ensure_location(EntityId e)
{
    const crd::u32 idx = e.index();
    if (idx >= m_locations.size())
    {
        m_locations.resize(static_cast<crd::usize>(idx) + 1, EntityLocation{});
    }
    return m_locations[idx];
}

EntityLocation ArchetypeChunkStorage::location_or_invalid(EntityId e) const noexcept
{
    const crd::u32 idx = e.index();
    if (idx >= m_locations.size())
    {
        return EntityLocation{};
    }
    return m_locations[idx];
}

void ArchetypeChunkStorage::clear_location(EntityId e) noexcept
{
    const crd::u32 idx = e.index();
    if (idx < m_locations.size())
    {
        m_locations[idx] = EntityLocation{};
    }
}

EntityLocation ArchetypeChunkStorage::location(EntityId e) const noexcept
{
    return location_or_invalid(e);
}

// ---- layout_index helper -------------------------------------------------

crd::u32 ArchetypeChunkStorage::layout_index(const Archetype& a, ComponentId c) noexcept
{
    // components_sorted is ascending; linear scan is fine at archetype widths
    // ≤ kMaxComponentsPerArchetype = 32.
    for (crd::u32 i = 0; i < a.layout.component_count(); ++i)
    {
        if (a.layout.components_sorted[i] == c)
        {
            return i;
        }
    }
    return kMaxComponents; // sentinel — caller treats as "not present"
}

// ---- Slot acquire / release ---------------------------------------------

EntityLocation ArchetypeChunkStorage::acquire_slot(Archetype& dst, EntityId entity)
{
    // Always insert into the last chunk if it has room; else allocate one.
    if (dst.chunks.size() == 0 || dst.chunks.back().header()->entity_count >= dst.layout.entity_capacity)
    {
        Chunk fresh = m_chunks.allocate();
        fresh.header()->entity_capacity = static_cast<crd::u16>(dst.layout.entity_capacity);
        fresh.header()->archetype_id = dst.id.raw;
        dst.chunks.push_back(fresh);
    }

    Chunk& chunk = dst.chunks.back();
    ChunkHeader* hdr = chunk.header();
    const crd::u16 slot = hdr->entity_count;
    const crd::u32 chunk_idx = static_cast<crd::u32>(dst.chunks.size() - 1);

    // Place entity id; component bytes are populated by caller.
    chunk.entity_id_array(dst.layout)[slot] = entity;
    ++hdr->entity_count;

    return EntityLocation{dst.id, chunk_idx, slot, 0};
}

void ArchetypeChunkStorage::release_slot(Archetype& src, crd::u32 chunk_index, crd::u16 slot_in_chunk)
{
    Chunk& chunk = src.chunks[chunk_index];
    ChunkHeader* hdr = chunk.header();
    const crd::u16 last_slot = static_cast<crd::u16>(hdr->entity_count - 1);

    // Destruct the components living in the slot we're releasing. Source
    // bytes must NOT be read after destruct — anything that needs them
    // (event sinks) has been called earlier in the mutation path.
    for (crd::u32 li = 0; li < src.layout.component_count(); ++li)
    {
        const ComponentInfo* info = m_registry->info(src.layout.components_sorted[li]);
        CRD_ASSERT(info != nullptr);
        crd::u8* slot_bytes = component_slot(chunk, src.layout, li, slot_in_chunk);
        if (info->destruct != nullptr)
        {
            info->destruct(slot_bytes);
        }
    }

    if (slot_in_chunk != last_slot)
    {
        // Move the trailing entity into the freed slot, byte-by-byte per
        // component. Move-construct from the trailing slot into the freed
        // slot; then destruct the trailing slot.
        //
        // Pattern: pass-1 destruct above leaves slot_in_chunk's bytes
        // uninitialised. `move_construct` here constructs *into* those bytes
        // via placement-new — the destination must not be read by the
        // user's move ctor. This is the standard C++ contract for
        // placement-new but worth pinning given the byte-level interface.
        for (crd::u32 li = 0; li < src.layout.component_count(); ++li)
        {
            const ComponentInfo* info = m_registry->info(src.layout.components_sorted[li]);
            CRD_ASSERT(info != nullptr);
            crd::u8* dst_bytes = component_slot(chunk, src.layout, li, slot_in_chunk);
            crd::u8* last_bytes = component_slot(chunk, src.layout, li, last_slot);
            if (info->move_construct != nullptr)
            {
                info->move_construct(dst_bytes, last_bytes);
            }
            else
            {
                // No move-construct registered — copy bytes verbatim. Acceptable
                // for trivially-movable types where the registry chose not to
                // capture an op (shouldn't happen in v1c2 since we capture
                // unconditionally for movable types, but keep the fallback safe).
                std::memcpy(dst_bytes, last_bytes, info->size);
            }
            if (info->destruct != nullptr)
            {
                info->destruct(last_bytes);
            }
        }

        // Update the trailing entity's id at the freed slot.
        EntityId* eids = chunk.entity_id_array(src.layout);
        EntityId moved = eids[last_slot];
        eids[slot_in_chunk] = moved;

        // Patch the moved entity's location to point at the freed slot.
        EntityLocation& moved_loc = ensure_location(moved);
        moved_loc.slot_in_chunk = slot_in_chunk;
    }

    --hdr->entity_count;

    // If the LAST chunk dropped to 0 entities, free it back. Mid-archetype
    // empty chunks are not reclaimed (matches Bevy — avoids fragmentation
    // and chunk-allocator churn under steady-state churn).
    if (chunk_index == src.chunks.size() - 1 && hdr->entity_count == 0)
    {
        m_chunks.free(src.chunks[chunk_index]);
        src.chunks.swap_remove(chunk_index);
    }
}

// ---- move_shared_components ---------------------------------------------

void ArchetypeChunkStorage::move_shared_components(Archetype& src, Chunk& src_chunk, crd::u16 src_slot, Archetype& dst,
                                                   Chunk& dst_chunk, crd::u16 dst_slot)
{
    // Walk components shared between src and dst (intersection of masks).
    // Both layouts are sorted by ComponentId, so a merge-style two-pointer
    // walk suffices.
    crd::u32 i_src = 0;
    crd::u32 i_dst = 0;
    while (i_src < src.layout.component_count() && i_dst < dst.layout.component_count())
    {
        const ComponentId src_c = src.layout.components_sorted[i_src];
        const ComponentId dst_c = dst.layout.components_sorted[i_dst];
        if (src_c.raw < dst_c.raw)
        {
            ++i_src;
        }
        else if (dst_c.raw < src_c.raw)
        {
            ++i_dst;
        }
        else
        {
            // Shared component — move-construct from src slot into dst slot.
            const ComponentInfo* info = m_registry->info(src_c);
            CRD_ASSERT(info != nullptr);
            crd::u8* src_bytes = component_slot(src_chunk, src.layout, i_src, src_slot);
            crd::u8* dst_bytes = component_slot(dst_chunk, dst.layout, i_dst, dst_slot);
            if (info->move_construct != nullptr)
            {
                info->move_construct(dst_bytes, src_bytes);
            }
            else
            {
                std::memcpy(dst_bytes, src_bytes, info->size);
            }
            // Source slot is destructed by release_slot; do not read its bytes
            // after this point.

            // Bump dst chunk's version counter for this component (it was
            // written via the move).
            dst_chunk.header()->version_counter[i_dst] += 1;
            ++i_src;
            ++i_dst;
        }
    }
}

// ---- IStorageBackend impl -------------------------------------------------

void ArchetypeChunkStorage::insert(EntityId e, ComponentId c, void* data)
{
    CRD_ASSERT(data != nullptr);
    const ComponentInfo* info = m_registry->info(c);
    CRD_ASSERT(info != nullptr);

    EntityLocation& loc = ensure_location(e);

    // Path 1: entity already has c — UPSERT in place, destruct old + move new.
    if (!loc.archetype.is_null())
    {
        Archetype& arch = *m_graph.by_id(loc.archetype);
        if (arch.mask.test(c))
        {
            const crd::u32 li = layout_index(arch, c);
            CRD_ASSERT(li != kMaxComponents);
            Chunk& chunk = arch.chunks[loc.chunk_index];
            crd::u8* slot_b = component_slot(chunk, arch.layout, li, loc.slot_in_chunk);

            if (info->destruct != nullptr)
            {
                info->destruct(slot_b);
            }
            if (info->move_construct != nullptr)
            {
                // `data` is caller-owned; we move from a non-const view of it.
                // Since storage receives `const void*`, cast away — caller
                // contract: data is owned and may be moved-from.
                info->move_construct(slot_b, data);
            }
            else
            {
                std::memcpy(slot_b, data, info->size);
            }

            chunk.header()->version_counter[li] += 1;
            m_sink->on_update(e, c, slot_b, slot_b);
            return;
        }
    }

    // Path 2: entity does not have c — move to (current_mask | c) archetype.
    Archetype* src_arch = loc.archetype.is_null() ? nullptr : m_graph.by_id(loc.archetype);

    ComponentMask new_mask = (src_arch != nullptr) ? src_arch->mask : ComponentMask{};
    new_mask.set(c);
    Archetype& dst_arch = (src_arch != nullptr) ? m_graph.after_add(*src_arch, c) : m_graph.archetype_for(new_mask);

    EntityLocation new_loc = acquire_slot(dst_arch, e);

    if (src_arch != nullptr)
    {
        Chunk& src_chunk = src_arch->chunks[loc.chunk_index];
        Chunk& dst_chunk = dst_arch.chunks[new_loc.chunk_index];
        move_shared_components(*src_arch, src_chunk, loc.slot_in_chunk, dst_arch, dst_chunk, new_loc.slot_in_chunk);
    }

    // Place the new component value into its slot in the destination chunk.
    {
        const crd::u32 dst_li = layout_index(dst_arch, c);
        CRD_ASSERT(dst_li != kMaxComponents);
        Chunk& dst_chunk = dst_arch.chunks[new_loc.chunk_index];
        crd::u8* slot_b = component_slot(dst_chunk, dst_arch.layout, dst_li, new_loc.slot_in_chunk);
        if (info->move_construct != nullptr)
        {
            info->move_construct(slot_b, data);
        }
        else
        {
            std::memcpy(slot_b, data, info->size);
        }
        dst_chunk.header()->version_counter[dst_li] += 1;
        m_sink->on_insert(e, c, slot_b);
    }

    // Now drain the source archetype if there was one. release_slot handles
    // swap-remove + destruct + chunk-free if applicable.
    if (src_arch != nullptr)
    {
        release_slot(*src_arch, loc.chunk_index, loc.slot_in_chunk);
    }

    // Persist the new location.
    EntityLocation& slot_loc = ensure_location(e);
    slot_loc = new_loc;
}

void ArchetypeChunkStorage::remove(EntityId e, ComponentId c)
{
    EntityLocation loc = location_or_invalid(e);
    if (loc.archetype.is_null())
    {
        return; // entity has no archetype-stored components — nothing to do
    }
    Archetype& src_arch = *m_graph.by_id(loc.archetype);
    if (!src_arch.mask.test(c))
    {
        return; // entity lacks c — no-op
    }

    // Fire on_remove before move/destruct so the sink may inspect the value.
    {
        const crd::u32 li = layout_index(src_arch, c);
        CRD_ASSERT(li != kMaxComponents);
        Chunk& chunk = src_arch.chunks[loc.chunk_index];
        const crd::u8* slot_b = component_slot(chunk, src_arch.layout, li, loc.slot_in_chunk);
        m_sink->on_remove(e, c, slot_b);
    }

    Archetype& dst_arch = m_graph.after_remove(src_arch, c);

    // Special case: dst is the empty archetype — the entity has no remaining
    // archetype-stored components. release source slot and clear location.
    if (dst_arch.mask.popcount() == 0)
    {
        release_slot(src_arch, loc.chunk_index, loc.slot_in_chunk);
        clear_location(e);
        return;
    }

    EntityLocation new_loc = acquire_slot(dst_arch, e);
    Chunk& src_chunk = src_arch.chunks[loc.chunk_index];
    Chunk& dst_chunk = dst_arch.chunks[new_loc.chunk_index];
    move_shared_components(src_arch, src_chunk, loc.slot_in_chunk, dst_arch, dst_chunk, new_loc.slot_in_chunk);

    release_slot(src_arch, loc.chunk_index, loc.slot_in_chunk);

    EntityLocation& slot_loc = ensure_location(e);
    slot_loc = new_loc;
}

bool ArchetypeChunkStorage::has(EntityId e, ComponentId c) const
{
    const EntityLocation loc = location_or_invalid(e);
    if (loc.archetype.is_null())
    {
        return false;
    }
    const Archetype* arch = m_graph.by_id(loc.archetype);
    if (arch == nullptr)
    {
        return false;
    }
    return arch->mask.test(c);
}

void* ArchetypeChunkStorage::get_mut(EntityId e, ComponentId c)
{
    const EntityLocation loc = location_or_invalid(e);
    if (loc.archetype.is_null())
    {
        return nullptr;
    }
    Archetype* arch = m_graph.by_id(loc.archetype);
    if (arch == nullptr || !arch->mask.test(c))
    {
        return nullptr;
    }
    const crd::u32 li = layout_index(*arch, c);
    CRD_ASSERT(li != kMaxComponents);

    Chunk& chunk = arch->chunks[loc.chunk_index];
    crd::u8* slot_b = component_slot(chunk, arch->layout, li, loc.slot_in_chunk);
    chunk.header()->version_counter[li] += 1; // declared write — chunk-grain change detect
    m_sink->on_update(e, c, slot_b, slot_b);
    return slot_b;
}

const void* ArchetypeChunkStorage::get_const(EntityId e, ComponentId c) const
{
    const EntityLocation loc = location_or_invalid(e);
    if (loc.archetype.is_null())
    {
        return nullptr;
    }
    const Archetype* arch = m_graph.by_id(loc.archetype);
    if (arch == nullptr || !arch->mask.test(c))
    {
        return nullptr;
    }
    const crd::u32 li = layout_index(*arch, c);
    CRD_ASSERT(li != kMaxComponents);

    const Chunk& chunk = arch->chunks[loc.chunk_index];
    const crd::u8* slot_b = component_slot(chunk, arch->layout, li, loc.slot_in_chunk);
    return slot_b;
}

void ArchetypeChunkStorage::for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data)
{
    if (fn == nullptr)
    {
        return;
    }
    for (crd::u32 i = 0; i < m_graph.archetype_count(); ++i)
    {
        Archetype* arch = m_graph.by_id(ArchetypeId{i});
        CRD_ASSERT(arch != nullptr);
        // Superset match: archetype.mask & required == required
        if (((arch->mask & required).bits[0] != required.bits[0]) ||
            ((arch->mask & required).bits[1] != required.bits[1]) ||
            ((arch->mask & required).bits[2] != required.bits[2]) ||
            ((arch->mask & required).bits[3] != required.bits[3]))
        {
            continue;
        }
        for (Chunk& chunk : arch->chunks)
        {
            ChunkHeader* hdr = chunk.header();
            ChunkView view{};
            view.entities = chunk.entity_id_array(arch->layout);
            view.entity_count = hdr->entity_count;
            view.present_mask = arch->mask;
            fn(view, user_data);
        }
    }
}

void ArchetypeChunkStorage::on_entity_destroyed(EntityId e)
{
    m_sink->on_entity_destroyed(e);

    const EntityLocation loc = location_or_invalid(e);
    if (loc.archetype.is_null())
    {
        return;
    }
    Archetype& arch = *m_graph.by_id(loc.archetype);

    // Fire per-component on_remove events (in mask order) before destruction.
    for (crd::u32 li = 0; li < arch.layout.component_count(); ++li)
    {
        const ComponentId c = arch.layout.components_sorted[li];
        Chunk& chunk = arch.chunks[loc.chunk_index];
        const crd::u8* slot_b = component_slot(chunk, arch.layout, li, loc.slot_in_chunk);
        m_sink->on_remove(e, c, slot_b);
    }

    release_slot(arch, loc.chunk_index, loc.slot_in_chunk);
    clear_location(e);
}

} // namespace crd::scene
