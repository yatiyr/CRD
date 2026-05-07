#include <crd/core/assert.hpp>
#include <crd/scene/world.hpp>

#include <utility>

namespace crd::scene
{

namespace
{
// Walk every set bit of `mask`; invoke fn(ComponentId{bit}) for each.
// Linear bit-scan over kMaxComponents (256) — for_each_chunk setup, not the
// hot iteration path. Keeps the implementation self-contained without <bit>.
template <typename Fn> void for_each_set_bit(const ComponentMask& mask, Fn&& fn)
{
    for (crd::u32 word = 0; word < 4; ++word)
    {
        const crd::u64 bits = mask.bits[word];
        if (bits == 0)
        {
            continue;
        }
        for (crd::u32 b = 0; b < 64; ++b)
        {
            if ((bits & (crd::u64{1} << b)) != 0)
            {
                const crd::u32 c_raw = (word * 64U) + b;
                std::forward<Fn>(fn)(ComponentId{static_cast<crd::u16>(c_raw)});
            }
        }
    }
}

// True if `subset.bits` is a bitmask subset of `superset.bits`.
[[nodiscard]] bool mask_subset_of(const ComponentMask& subset, const ComponentMask& superset) noexcept
{
    for (crd::u32 i = 0; i < 4; ++i)
    {
        if ((subset.bits[i] & ~superset.bits[i]) != 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace

World::World(crd::memory::IAllocator* alloc)
    : m_slots(alloc), m_pending_destroy(alloc), m_components(alloc), m_storage(alloc, m_components),
      m_sparse_storage(alloc, m_components), m_event_sink(NullStorageEventSink::instance())
{
}

EntityId World::spawn()
{
    return m_slots.allocate();
}

void World::destroy(EntityId e)
{
    // Stale handles are silently dropped by flush_destroys; queueing one is
    // harmless. Skipping the queue push here would also be valid, but defer
    // is cheaper than a generation check on the hot path.
    m_pending_destroy.push_back(e);
}

void World::destroy_immediate(EntityId e)
{
    if (m_slots.is_alive(e))
    {
        // World fires the singular sink->on_entity_destroyed event ONCE so
        // observers (Layer-5 indexes) see exactly one event per destroy
        // regardless of how many backends hold the entity's components.
        // Both backends then drain their own components, emitting per-
        // component on_remove events through the same sink.
        m_event_sink->on_entity_destroyed(e);
        m_storage.on_entity_destroyed(e);
        m_sparse_storage.on_entity_destroyed(e);
        m_slots.free(e);
    }
}

void World::flush_destroys()
{
    for (EntityId e : m_pending_destroy)
    {
        if (m_slots.is_alive(e))
        {
            m_event_sink->on_entity_destroyed(e);
            m_storage.on_entity_destroyed(e);
            m_sparse_storage.on_entity_destroyed(e);
            m_slots.free(e);
        }
    }
    m_pending_destroy.clear();
}

// ---- v1e: mixed-backend chunk visitor ------------------------------------

void World::for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data)
{
    if (fn == nullptr)
    {
        return;
    }

    // Split `required` by the registered storage hint. Components that have
    // no registered info (raw bits set on an unregistered slot) fall into the
    // archetype side — they will simply never match an archetype mask and
    // yield zero chunks, which is the desired "yield nothing" behavior.
    ComponentMask archetype_bits{};
    ComponentMask sparse_bits{};
    for_each_set_bit(required,
                     [&](ComponentId c)
                     {
                         const ComponentInfo* info = m_components.info(c);
                         if (info != nullptr && info->storage_hint == StorageHint::SparseSet)
                         {
                             sparse_bits.set(c);
                         }
                         else
                         {
                             archetype_bits.set(c);
                         }
                     });

    const crd::u32 sparse_pop = sparse_bits.popcount();
    const crd::u32 archetype_pop = archetype_bits.popcount();

    // Pure-archetype path — zero overhead vs the per-backend method. Includes
    // the empty-required case (yields every archetype chunk).
    if (sparse_pop == 0)
    {
        m_storage.for_each_chunk(required, fn, user_data);
        // Symmetric reading for an entirely empty `required`: also yield
        // every non-empty SparseSet pool. The behavior matches "yield every
        // chunk regardless of required" — both backends contribute. Pinned
        // 2026-05-07 during v1e planning so visitors see all entities of
        // any backend when required is empty.
        if (archetype_pop == 0)
        {
            m_sparse_storage.for_each_chunk(required, fn, user_data);
        }
        return;
    }

    // Pure-SparseSet single-bit path — forward to SparseSetStorage's optimised
    // single-pool yield.
    if (archetype_pop == 0 && sparse_pop == 1)
    {
        m_sparse_storage.for_each_chunk(required, fn, user_data);
        return;
    }

    // From here on the path constructs ChunkViews into a stack-local scratch
    // buffer. `m_alloc` is the World's allocator chain; per-call allocate +
    // dtor-free is acceptable — visitor body cost dwarfs it. Stack-local also
    // makes nested for_each_chunk safe (recursive queries from debug overlays
    // / editor tools don't corrupt scratch).
    crd::memory::IAllocator* alloc = m_pending_destroy.allocator();
    crd::containers::Array<EntityId> scratch{alloc};

    // Pure-SparseSet multi-bit path — anchor on the smallest pool, sparse-check
    // every other bit per entity. Sparse-as-anchor here is correct: pure-sparse
    // intersection has no cache-coherent path (no chunked SoA), so smallest-
    // anchor minimises probes per entity.
    if (archetype_pop == 0)
    {
        // Resolve every named sparse_bit's pool. If ANY is missing or empty,
        // the intersection is empty — yield nothing immediately.
        ComponentId anchor_id{};
        crd::u32 anchor_count = 0xFFFFFFFFU;
        bool any_missing = false;
        for_each_set_bit(sparse_bits,
                         [&](ComponentId c)
                         {
                             const crd::u32 cnt = m_sparse_storage.entity_count(c);
                             if (cnt == 0U)
                             {
                                 any_missing = true;
                             }
                             if (cnt < anchor_count)
                             {
                                 anchor_count = cnt;
                                 anchor_id = c;
                             }
                         });
        if (any_missing || anchor_id.is_null())
        {
            return;
        }

        // Walk anchor pool's entities directly via has/get from SparseSetStorage.
        // We don't go through m_sparse_storage.for_each_chunk here because we
        // need the ComponentId-of-anchor to map back to its pool's entity list,
        // and entity_count(anchor_id) plus per-entity has() probes do that
        // without needing a context-passing trampoline through ChunkVisitor.
        //
        // Strategy: use SparseSetStorage's single-bit for_each_chunk path to
        // get the anchor's dense entity array, then walk it in a dedicated
        // visitor invocation that fills `scratch`.
        ComponentMask anchor_only{};
        anchor_only.set(anchor_id);

        struct AnchorCtx
        {
            const SparseSetStorage* sparse_storage;
            ComponentMask sparse_bits;
            ComponentId anchor;
            crd::containers::Array<EntityId>* out;
        };
        AnchorCtx ctx{&m_sparse_storage, sparse_bits, anchor_id, &scratch};

        m_sparse_storage.for_each_chunk(
            anchor_only,
            [](const ChunkView& view, void* ud)
            {
                auto* c = static_cast<AnchorCtx*>(ud);
                for (crd::u32 i = 0; i < view.entity_count; ++i)
                {
                    const EntityId e = view.entities[i];
                    bool ok = true;
                    for_each_set_bit(c->sparse_bits,
                                     [&](ComponentId b)
                                     {
                                         if (b == c->anchor)
                                         {
                                             return;
                                         }
                                         if (!c->sparse_storage->has(e, b))
                                         {
                                             ok = false;
                                         }
                                     });
                    if (ok)
                    {
                        c->out->push_back(e);
                    }
                }
            },
            &ctx);

        if (scratch.size() == 0)
        {
            return;
        }
        ChunkView view{};
        view.entities = scratch.data();
        view.entity_count = static_cast<crd::u32>(scratch.size());
        view.present_mask = required;
        fn(view, user_data);
        return;
    }

    // Mixed path — walk archetypes ⊇ archetype_bits, sparse-check sparse_bits
    // per entity. Archetype-as-anchor is the cache-coherent default for v1e;
    // sparse-as-anchor when the smallest sparse pool is much smaller than the
    // matching archetype is a future profile-driven optimisation.
    for (crd::u32 ai = 0; ai < m_storage.graph().archetype_count(); ++ai)
    {
        const Archetype* arch = m_storage.graph().by_id(ArchetypeId{ai});
        CRD_ASSERT(arch != nullptr);
        if (!mask_subset_of(archetype_bits, arch->mask))
        {
            continue;
        }
        for (const Chunk& chunk : arch->chunks)
        {
            const ChunkHeader* hdr = chunk.header();
            const EntityId* chunk_entities = chunk.entity_id_array(arch->layout);
            scratch.clear();
            for (crd::u16 slot = 0; slot < hdr->entity_count; ++slot)
            {
                const EntityId e = chunk_entities[slot];
                bool ok = true;
                for_each_set_bit(sparse_bits,
                                 [&](ComponentId c)
                                 {
                                     if (!m_sparse_storage.has(e, c))
                                     {
                                         ok = false;
                                     }
                                 });
                if (ok)
                {
                    scratch.push_back(e);
                }
            }
            if (scratch.size() == 0)
            {
                continue;
            }
            ChunkView view{};
            view.entities = scratch.data();
            view.entity_count = static_cast<crd::u32>(scratch.size());
            view.present_mask = required;
            fn(view, user_data);
        }
    }
}

} // namespace crd::scene
