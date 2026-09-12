// v1f: relations + iterative destruction worklist
// v1h: schedule + commands flush
// v1j: transform writers + dirty subtree marking
// v1k: serialize traits attached to built-in relations
#include <crd/core/assert.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/world.hpp>

#include <new>
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
      m_sparse_storage(alloc, m_components), m_relations(alloc), m_commands_buffer(*this),
      m_indexes(alloc), m_fanout_sink(*this), m_external_sink(NullStorageEventSink::instance())
{
    // Lazy slots indexed by ComponentId.raw. Always nullptr until
    // register_relation() allocates a RelationInfo for that slot.
    m_relations.resize(kMaxComponents, nullptr);

    // Pre-construct the per-phase system arrays with the World allocator.
    for (auto& bucket : m_systems)
    {
        bucket = crd::containers::Array<std::unique_ptr<ISystem>>{alloc};
    }

    // v1i: install the index fan-out on both storage backends. The
    // backends never see anything else — external sinks reach the events
    // through the fan-out's forward path.
    m_storage.set_event_sink(&m_fanout_sink);
    m_sparse_storage.set_event_sink(&m_fanout_sink);
}

World::~World()
{
    crd::memory::IAllocator* alloc = m_pending_destroy.allocator();
    for (RelationInfo*& slot : m_relations)
    {
        if (slot != nullptr)
        {
            slot->~RelationInfo();
            alloc->deallocate(slot);
            slot = nullptr;
        }
    }
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

namespace
{
// Drain a worklist of entities, applying full destruction (cascade + sink +
// backends + slot free) iteratively. Cascades from OnTargetDestroyed::Cascade
// push new entities onto the worklist — recursion is iterative, so a 100+
// deep ChildOf tree never overflows the stack. Diamond shapes (an entity
// reachable via two cascades) are deduped by the alive-check at the top of
// each iteration.
} // namespace

void World::destroy_immediate(EntityId e)
{
    if (!m_slots.is_alive(e))
    {
        return;
    }
    crd::containers::Array<EntityId> worklist{m_pending_destroy.allocator()};
    worklist.push_back(e);
    drain_destruction_worklist(worklist);
}

void World::flush_destroys()
{
    crd::containers::Array<EntityId> worklist{m_pending_destroy.allocator()};
    for (EntityId e : m_pending_destroy)
    {
        if (m_slots.is_alive(e))
        {
            worklist.push_back(e);
        }
    }
    m_pending_destroy.clear();
    drain_destruction_worklist(worklist);
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

// ---- v1f: Relations -----------------------------------------------------

void World::RelationInfo::add_reverse(EntityId target, EntityId src)
{
    if (target.is_null())
    {
        return;
    }
    crd::containers::Array<EntityId>* arr = reverse_sources.find(target);
    if (arr == nullptr)
    {
        // Lazy-create the inner Array with the World's allocator. operator[]
        // would default-construct with default_allocator — wrong for memory
        // budgeting. Emplace explicitly with `alloc`.
        reverse_sources.emplace(target, alloc);
        arr = reverse_sources.find(target);
        CRD_ASSERT(arr != nullptr);
    }
    arr->push_back(src);
}

void World::RelationInfo::remove_reverse(EntityId target, EntityId src)
{
    if (target.is_null())
    {
        return;
    }
    crd::containers::Array<EntityId>* arr = reverse_sources.find(target);
    if (arr == nullptr)
    {
        return;
    }
    // Linear scan + swap-with-last. Source arrays are typically small
    // (children of one parent); n^2 in the rare degenerate case is fine.
    for (crd::usize i = 0; i < arr->size(); ++i)
    {
        if ((*arr)[i].raw == src.raw)
        {
            (*arr)[i] = arr->back();
            arr->pop_back();
            break;
        }
    }
    // Erase empty entries so HashMap probe length stays bounded under churn.
    if (arr->size() == 0)
    {
        reverse_sources.erase(target);
    }
}

crd::containers::Array<EntityId> World::RelationInfo::take_sources(EntityId target)
{
    crd::containers::Array<EntityId> result{alloc};
    crd::containers::Array<EntityId>* arr = reverse_sources.find(target);
    if (arr == nullptr)
    {
        return result;
    }
    // Move-out + erase. Avoids a copy of the entity-id array under load.
    result = std::move(*arr);
    reverse_sources.erase(target);
    return result;
}

void World::on_relation_registered(ComponentId id)
{
    CRD_ASSERT(!id.is_null() && id.raw < m_relations.size());
    const ComponentInfo* info = m_components.info(id);
    CRD_ASSERT(info != nullptr && info->is_relation);

    // v1f invariant: OnTargetDestroyed requires ReverseIndex. Without the
    // reverse index, "find every source pointing at the dying target" is
    // an O(N) scan of every entity — out of scope for v1f. v1g+ may relax.
    if (info->has_on_target_destroyed)
    {
        CRD_ASSERT(info->has_reverse_index &&
                   "OnTargetDestroyed policy requires ReverseIndex on the same relation");
    }

    // Lazy-allocate RelationInfo only when ReverseIndex is set. Relations
    // without it pay zero memory tax for the unused HashMap.
    if (info->has_reverse_index && m_relations[id.raw] == nullptr)
    {
        crd::memory::IAllocator* alloc = m_pending_destroy.allocator();
        void* mem = alloc->allocate(sizeof(RelationInfo), alignof(RelationInfo));
        CRD_ASSERT(mem != nullptr);
        m_relations[id.raw] = ::new (mem) RelationInfo(alloc);
    }
}

void World::add_relation_impl(ComponentId id, EntityId src, EntityId target)
{
    CRD_ASSERT(!id.is_null());
    const ComponentInfo* info = m_components.info(id);
    CRD_ASSERT(info != nullptr && info->is_relation);

    // Read the prior target (if any) before the storage UPSERT.
    EntityId old_target = EntityId::null();
    if (const void* old_payload = get_relation_payload_const(id, src); old_payload != nullptr)
    {
        old_target = *static_cast<const EntityId*>(old_payload);
    }

    // UPSERT short-circuit: re-targeting to the same target is a no-op.
    // Avoids spurious storage events (on_remove + on_insert) that v1i
    // ChangeDetect would otherwise see as a real change.
    if (old_target.raw == target.raw && !old_target.is_null())
    {
        return;
    }

    // Acyclic check (debug only). Enforced via CRD_ASSERT — release builds
    // trust the caller. The public `would_form_cycle<Tag>(src, target)`
    // predicate is the testability lever for callers that want to verify
    // before attempting the add.
    if (info->acyclic)
    {
        CRD_ASSERT(!would_form_cycle_impl(id, src, target) &&
                   "add_relation: would form a cycle on an Acyclic relation");
    }

    RelationInfo* ri = m_relations[id.raw];

    // Reverse-index maintenance: drop the old (old_target, src) edge before
    // installing the new one. Idempotent if old_target was null.
    if (ri != nullptr && !old_target.is_null())
    {
        ri->remove_reverse(old_target, src);
    }

    // Storage UPSERT. Relation<Tag>'s payload is layout-equivalent to a
    // single EntityId, so we pass &target directly — the registered
    // move_construct callback for Relation<Tag> will placement-new at the
    // slot, copying the EntityId byte-identically.
    EntityId payload_target = target;
    backend_for(id).insert(src, id, &payload_target);

    // Install the new (target, src) edge.
    if (ri != nullptr && !target.is_null())
    {
        ri->add_reverse(target, src);
    }
}

void World::remove_relation_impl(ComponentId id, EntityId src)
{
    CRD_ASSERT(!id.is_null());
    const ComponentInfo* info = m_components.info(id);
    if (info == nullptr || !info->is_relation)
    {
        return;
    }
    // Read current target before the storage path destroys it.
    EntityId current_target = EntityId::null();
    if (const void* payload = get_relation_payload_const(id, src); payload != nullptr)
    {
        current_target = *static_cast<const EntityId*>(payload);
    }
    if (current_target.is_null())
    {
        return; // src has no Relation<Tag> — no-op
    }

    if (RelationInfo* ri = m_relations[id.raw]; ri != nullptr)
    {
        ri->remove_reverse(current_target, src);
    }
    backend_for(id).remove(src, id);
}

const void* World::get_relation_payload_const(ComponentId id, EntityId src) const
{
    if (id.is_null() || !is_alive(src))
    {
        return nullptr;
    }
    const ComponentInfo* info = m_components.info(id);
    if (info == nullptr || !info->is_relation)
    {
        return nullptr;
    }
    if (info->storage_hint == StorageHint::SparseSet)
    {
        return m_sparse_storage.get_const(src, id);
    }
    return m_storage.get_const(src, id);
}

bool World::would_form_cycle_impl(ComponentId id, EntityId src, EntityId target) const noexcept
{
    if (src.raw == target.raw && !src.is_null())
    {
        return true;
    }
    // Walk target → target's target → ... up the chain, looking for src.
    // Bound the walk at kMaxComponents (defensive against pre-existing
    // cycles in untyped inputs); legitimate trees never approach this depth.
    EntityId current = target;
    for (crd::u32 step = 0; step < 4096; ++step)
    {
        if (current.is_null())
        {
            return false;
        }
        const void* payload = get_relation_payload_const(id, current);
        if (payload == nullptr)
        {
            return false; // chain ends — no cycle
        }
        current = *static_cast<const EntityId*>(payload);
        if (current.raw == src.raw && !current.is_null())
        {
            return true;
        }
    }
    return false;
}

void World::traverse_relation_impl(ComponentId id, EntityId root, RelationVisitorFn fn, void* user_data) const
{
    if (fn == nullptr || id.is_null())
    {
        return;
    }
    const ComponentInfo* info = m_components.info(id);
    if (info == nullptr || !info->is_relation || !info->has_reverse_index)
    {
        // No reverse index → can't walk parent → children. v1g+ may add a
        // query-and-filter fallback; v1f requires reverse index.
        return;
    }
    const RelationInfo* ri = m_relations[id.raw];
    if (ri == nullptr)
    {
        return;
    }

    // Iterative DFS pre-order. Stack-local frame buffer avoids recursion
    // and keeps deep trees safe (UI hierarchies, particle attachment chains).
    struct Frame
    {
        EntityId entity;
        crd::u32 depth;
    };
    crd::containers::Array<Frame> stack{m_pending_destroy.allocator()};
    stack.push_back(Frame{root, 0U});

    while (stack.size() > 0)
    {
        const Frame f = stack.back();
        stack.pop_back();

        fn(f.entity, f.depth, user_data);

        // Push children in reverse order so DFS visits them in insertion order.
        const crd::containers::Array<EntityId>* children = ri->reverse_sources.find(f.entity);
        if (children == nullptr)
        {
            continue;
        }
        for (crd::usize i = children->size(); i > 0; --i)
        {
            stack.push_back(Frame{(*children)[i - 1U], f.depth + 1U});
        }
    }
}

void World::cleanup_outgoing_relations(EntityId e)
{
    // For every registered reverse-indexed relation that `e` has, remove
    // (target, e) from its reverse_sources. Must run BEFORE backend drain;
    // once backends destroy the components, the targets are unrecoverable.
    for (crd::u32 i = 0; i < m_relations.size(); ++i)
    {
        RelationInfo* ri = m_relations[i];
        if (ri == nullptr)
        {
            continue;
        }
        const ComponentId id{static_cast<crd::u16>(i)};
        const void* payload = get_relation_payload_const(id, e);
        if (payload == nullptr)
        {
            continue;
        }
        const EntityId target = *static_cast<const EntityId*>(payload);
        ri->remove_reverse(target, e);
    }
}

void World::apply_on_target_destroyed(EntityId destroyed, crd::containers::Array<EntityId>& worklist)
{
    for (crd::u32 i = 0; i < m_relations.size(); ++i)
    {
        RelationInfo* ri = m_relations[i];
        if (ri == nullptr)
        {
            continue;
        }
        const ComponentId id{static_cast<crd::u16>(i)};
        const ComponentInfo* info = m_components.info(id);
        if (info == nullptr || !info->is_relation || !info->has_on_target_destroyed)
        {
            continue;
        }

        // Drain (and erase) the sources entry for `destroyed`. The bulk
        // move-out avoids walking the array twice and frees the HashMap
        // slot immediately.
        crd::containers::Array<EntityId> sources = ri->take_sources(destroyed);
        if (sources.size() == 0)
        {
            continue;
        }

        const auto policy = static_cast<OnTargetDestroyed::Policy>(info->on_target_destroyed_policy);

        for (EntityId source : sources)
        {
            if (!is_alive(source))
            {
                continue; // already destroyed (diamond shape) — no-op
            }
            switch (policy)
            {
                case OnTargetDestroyed::Policy::Cascade:
                    worklist.push_back(source);
                    break;
                case OnTargetDestroyed::Policy::Detach:
                    backend_for(id).remove(source, id);
                    break;
                case OnTargetDestroyed::Policy::SetNull:
                {
                    void* p = backend_for(id).get_mut(source, id);
                    if (p != nullptr)
                    {
                        *static_cast<EntityId*>(p) = EntityId::null();
                    }
                    break;
                }
            }
        }
    }
}

void World::drain_destruction_worklist(crd::containers::Array<EntityId>& worklist)
{
    while (worklist.size() > 0)
    {
        const EntityId current = worklist.back();
        worklist.pop_back();

        if (!m_slots.is_alive(current))
        {
            continue; // diamond dedup — second visit is a no-op
        }

        // Phase 1: incoming relation policy (Cascade may push onto worklist).
        apply_on_target_destroyed(current, worklist);

        // Phase 2: outgoing reverse-index cleanup.
        cleanup_outgoing_relations(current);

        // Phase 3: backends drain (fire per-component on_remove), then
        // sink fan-out fires the singular on_entity_destroyed AFTER the
        // drain. Pinned 2026-05-07 v1i: this order lets indexes record
        // per-component on_remove during teardown without their
        // on_entity_destroyed clears being clobbered by trailing
        // on_remove events.
        m_storage.on_entity_destroyed(current);
        m_sparse_storage.on_entity_destroyed(current);
        m_fanout_sink.on_entity_destroyed(current);
        m_slots.free(current);
    }
}

// ---- v1i: Index framework — fan-out sink + auto-registration ------------

void World::IndexFanOutSink::on_insert(EntityId e, ComponentId c, const void* data)
{
    for (auto& slot : m_world->m_indexes)
    {
        if (slot != nullptr && slot->observed().test(c))
        {
            slot->on_insert(e, c, data);
        }
    }
    m_world->m_external_sink->on_insert(e, c, data);
}

void World::IndexFanOutSink::on_update(EntityId e, ComponentId c, const void* old_data, const void* new_data)
{
    for (auto& slot : m_world->m_indexes)
    {
        if (slot != nullptr && slot->observed().test(c))
        {
            slot->on_update(e, c, old_data, new_data);
        }
    }
    m_world->m_external_sink->on_update(e, c, old_data, new_data);
}

void World::IndexFanOutSink::on_remove(EntityId e, ComponentId c, const void* data)
{
    for (auto& slot : m_world->m_indexes)
    {
        if (slot != nullptr && slot->observed().test(c))
        {
            slot->on_remove(e, c, data);
        }
    }
    m_world->m_external_sink->on_remove(e, c, data);
}

void World::IndexFanOutSink::on_entity_destroyed(EntityId e)
{
    // Entity destruction is fan-out unconditionally — every index gets
    // the chance to clean up its per-entity state regardless of which
    // components it observes.
    for (auto& slot : m_world->m_indexes)
    {
        if (slot != nullptr)
        {
            slot->on_entity_destroyed(e);
        }
    }
    m_world->m_external_sink->on_entity_destroyed(e);
}

void World::notify_frame_begin()
{
    for (auto& slot : m_indexes)
    {
        if (slot != nullptr)
        {
            slot->on_frame_begin(m_frame_index);
        }
    }
}

void World::notify_frame_end()
{
    for (auto& slot : m_indexes)
    {
        if (slot != nullptr)
        {
            slot->on_frame_end(m_frame_index);
        }
    }
}

// auto_register_indexes_for is defined in world.hpp's inline section
// because it dispatches by trait flags into make_unique<...> calls
// (templates that need the concrete index types). See the bottom of
// world.hpp.

// ---- v1h: Schedule + step / step_fixed ----------------------------------

void World::register_system(std::unique_ptr<ISystem> system)
{
    CRD_ASSERT(system != nullptr);
    const SchedulePhase ph = system->phase();
    const auto idx = static_cast<crd::usize>(ph);
    CRD_ASSERT(idx < kSchedulePhaseCount);
    m_systems[idx].push_back(std::move(system));
}

namespace
{
// Run one phase of systems. Variable-rate systems run only on the
// variable pass; fixed-step systems only on fixed passes. step_fixed()
// runs each phase as N fixed passes followed by one variable pass.
void run_phase_pass(World& world, crd::containers::Array<std::unique_ptr<ISystem>>& bucket,
                    bool is_variable_pass)
{
    for (crd::usize i = 0; i < bucket.size(); ++i)
    {
        ISystem* sys = bucket[i].get();
        if (sys == nullptr)
        {
            continue;
        }
        const bool is_fixed = sys->fixed_step();
        if (is_variable_pass != !is_fixed)
        {
            continue; // wrong pass for this system
        }
        sys->run(world);
    }
    world.commands().flush();
}
} // namespace

void World::step(crd::f64 dt)
{
    // step() runs every system exactly once. Variable-rate AND fixed-step
    // systems are dispatched together — fixed-step opt-in only matters
    // under step_fixed.
    ++m_frame_index;
    notify_frame_begin();
    for (crd::u8 phase = 0; phase < kSchedulePhaseCount; ++phase)
    {
        auto& bucket = m_systems[phase];
        for (crd::usize i = 0; i < bucket.size(); ++i)
        {
            ISystem* sys = bucket[i].get();
            if (sys != nullptr)
            {
                sys->run(*this);
            }
        }
        m_commands_buffer.flush();
    }
    notify_frame_end();
    (void)dt; // v1h ISystem::run is dt-agnostic; systems pull dt from their own state
}

void World::step_fixed(crd::f64 dt, crd::f64 fixed_dt, crd::u32 max_substeps)
{
    CRD_ASSERT(fixed_dt > 0.0);

    m_fixed_accumulator += dt;
    crd::u32 substeps = 0;
    if (fixed_dt > 0.0)
    {
        const crd::f64 raw = m_fixed_accumulator / fixed_dt;
        substeps = static_cast<crd::u32>(raw < 0.0 ? 0.0 : raw);
    }
    if (substeps > max_substeps)
    {
        // Spiral-of-death clamp. Drop the excess accumulator instead of
        // running unbounded work this frame.
        m_fixed_accumulator -= static_cast<crd::f64>(substeps - max_substeps) * fixed_dt;
        substeps = max_substeps;
    }
    if (substeps > 0)
    {
        m_fixed_accumulator -= static_cast<crd::f64>(substeps) * fixed_dt;
    }

    // Phase order: each phase runs its fixed-step systems N times, then
    // its variable-rate systems once. This interleaving matches Bevy's
    // FixedUpdate semantics — variable systems see the world AFTER the
    // current phase's fixed substeps have settled.
    //
    // Frame lifecycle hooks fire ONCE per step_fixed call (not per
    // substep) — ChangeDetectIndex's "modified during current frame"
    // semantic takes the whole step_fixed as a single frame.
    ++m_frame_index;
    notify_frame_begin();
    for (crd::u8 phase = 0; phase < kSchedulePhaseCount; ++phase)
    {
        auto& bucket = m_systems[phase];
        for (crd::u32 substep = 0; substep < substeps; ++substep)
        {
            run_phase_pass(*this, bucket, /*is_variable_pass=*/false);
        }
        run_phase_pass(*this, bucket, /*is_variable_pass=*/true);
    }
    notify_frame_end();
}

// ---- v1j: Transform writer API ------------------------------------------

namespace
{
// Mark dirty by adding a TransformDirtyFlag (SparseSet) component if the
// entity doesn't already carry one. add_component on a SparseSet pool is
// O(1) and idempotent on UPSERT (replaces with the same payload).
void mark_one_dirty(World& w, EntityId e)
{
    if (!w.has_component<TransformDirtyFlag>(e))
    {
        w.add_component<TransformDirtyFlag>(e, TransformDirtyFlag{});
    }
}

// DFS subtree marking via traverse_relation<ChildOf>. CRD_ASSERTs depth
// in debug builds (advisor pin #7); release skips the check.
void mark_subtree_via_childof(World& w, EntityId e)
{
    crd::u32 max_observed_depth = 0;
    w.traverse_relation<crd::scene::relations::ChildOf>(e,
                                                        [&](EntityId visited, crd::u32 depth)
                                                        {
                                                            if (depth > max_observed_depth)
                                                            {
                                                                max_observed_depth = depth;
                                                            }
                                                            mark_one_dirty(w, visited);
                                                        });
    CRD_ASSERT(max_observed_depth < kMaxTransformDepth &&
               "Transform subtree depth exceeded kMaxTransformDepth (256). Split via Owns or use a flat propagation.");
}
} // namespace

void World::mark_transform_subtree_dirty(EntityId e)
{
    if (!is_alive(e))
    {
        return;
    }
    mark_subtree_via_childof(*this, e);
}

void World::set_translation(EntityId e, crd::math::Vec3f t)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr && "set_translation: entity has no Transform component");
    // v0b-3: raw Vec3f from public API tagged as Length at the assignment boundary.
    tr->translation = crd::math::from_raw_vec<crd::units::dim::Length>(t);
    mark_transform_subtree_dirty(e);
}

void World::set_rotation_quat(EntityId e, crd::math::Quatf q)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    (void)crd::math::try_normalize(q);
    tr->rotation = q;
    mark_transform_subtree_dirty(e);
}

void World::set_rotation_quat_unnormalized(EntityId e, crd::math::Quatf q)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    tr->rotation = q;
    mark_transform_subtree_dirty(e);
}

void World::set_rotation_axis_angle(EntityId e, crd::math::Vec3f axis, crd::f32 radians)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    tr->rotation = crd::math::from_axis_angle(axis, radians);
    mark_transform_subtree_dirty(e);
}

void World::set_rotation_euler(EntityId e, crd::f32 x, crd::f32 y, crd::f32 z, crd::math::EulerOrder order)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    tr->rotation = crd::math::from_euler(x, y, z, order);
    mark_transform_subtree_dirty(e);
}

void World::set_rotation_from_to(EntityId e, crd::math::Vec3f from_dir, crd::math::Vec3f to_dir)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    tr->rotation = crd::math::from_to_rotation(from_dir, to_dir);
    mark_transform_subtree_dirty(e);
}

void World::set_rotation_look_at(EntityId e, crd::math::Vec3f forward, crd::math::Vec3f up)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    // Convert look_at view-matrix orientation back to a quat. The view
    // matrix's upper-left 3x3 is the inverse rotation; we pull it out via
    // from_mat3 of the transposed columns. Cleaner: recompose directly.
    const crd::math::Vec3f f = crd::math::normalized(forward);
    const crd::math::Vec3f r = crd::math::normalized(crd::math::cross(f, up));
    const crd::math::Vec3f u = crd::math::cross(r, f);
    // Object-space rotation: columns are (right, up, -forward) — matches
    // the right-handed convention. f points OUT of the camera/object so
    // we negate it for the Z column to make it the FORWARD-FACING axis.
    const crd::math::Mat3f rot_mat(r, u, -f);
    tr->rotation = crd::math::from_mat3(rot_mat);
    mark_transform_subtree_dirty(e);
}

void World::set_scale(EntityId e, crd::math::Vec3f s)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    tr->scale = s;
    mark_transform_subtree_dirty(e);
}

void World::set_local(EntityId e, const crd::math::Vec3f& translation, const crd::math::Quatf& rotation,
                      const crd::math::Vec3f& scale)
{
    CRD_ASSERT(is_alive(e));
    Transform* tr = get_component_mut<Transform>(e);
    CRD_ASSERT(tr != nullptr);
    // Boundary: raw Vec3f from the public API is tagged as Length at the
    // assignment site (Phase 3.1.7.5 v0b-3). Callers that pass typed
    // Vec3<Length32> can use a future `set_local` overload (v0c+).
    tr->translation = crd::math::from_raw_vec<crd::units::dim::Length>(translation);
    tr->rotation    = rotation;
    (void)crd::math::try_normalize(tr->rotation);
    tr->scale = scale;
    mark_transform_subtree_dirty(e);
}

void World::set_world(EntityId e, const crd::math::Mat4f& world)
{
    crd::math::Vec3f t{};
    crd::math::Quatf r{};
    crd::math::Vec3f s{};
    const bool ok = crd::math::to_trs(world, t, r, s);
    CRD_ASSERT(ok && "set_world: input matrix is singular (zero column). Use try_set_world for validation.");
    if (!ok)
    {
        return;
    }
    set_local(e, t, r, s);
}

bool World::try_set_world(EntityId e, const crd::math::Mat4f& world)
{
    if (!is_alive(e) || get_component_mut<Transform>(e) == nullptr)
    {
        return false;
    }
    crd::math::Vec3f t{};
    crd::math::Quatf r{};
    crd::math::Vec3f s{};
    if (!crd::math::to_trs(world, t, r, s))
    {
        return false;
    }
    set_local(e, t, r, s);
    return true;
}

void World::register_builtin_relations()
{
    using namespace crd::scene::relations;

    // Canonical defaults — see relation.hpp for the rationale table.
    // ComponentSerialize traits attached so SCEN round-trip works for the
    // built-in hierarchy/attachment/ownership/etc relations (v1k).
    register_relation<ChildOf>(StorageHint::Archetype, ReverseIndex{}, Acyclic{},
                               OnTargetDestroyed{OnTargetDestroyed::Policy::Cascade},
                               relation_serialize_trait(kFourCC_RelChildOf));

    register_relation<AttachedTo>(StorageHint::Archetype, ReverseIndex{}, Acyclic{},
                                  OnTargetDestroyed{OnTargetDestroyed::Policy::Detach},
                                  relation_serialize_trait(kFourCC_RelAttachedTo));

    register_relation<Owns>(StorageHint::Archetype, ReverseIndex{}, Acyclic{},
                            OnTargetDestroyed{OnTargetDestroyed::Policy::Cascade},
                            relation_serialize_trait(kFourCC_RelOwns));

    register_relation<Targets>(StorageHint::SparseSet, ReverseIndex{},
                               OnTargetDestroyed{OnTargetDestroyed::Policy::SetNull},
                               relation_serialize_trait(kFourCC_RelTargets));

    register_relation<DependsOn>(StorageHint::SparseSet, ReverseIndex{}, Acyclic{},
                                 OnTargetDestroyed{OnTargetDestroyed::Policy::SetNull},
                                 relation_serialize_trait(kFourCC_RelDependsOn));

    register_relation<PossessedBy>(StorageHint::SparseSet, ReverseIndex{},
                                   OnTargetDestroyed{OnTargetDestroyed::Policy::Detach},
                                   relation_serialize_trait(kFourCC_RelPossessedBy));
}

} // namespace crd::scene
