#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype_chunk_storage.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/slot_map.hpp>
#include <crd/scene/sparse_set_storage.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/storage_event_sink.hpp>

#include <utility>

namespace crd::scene
{
// World — the root container for an ECS scene. Phase 3.0 v1a ships only the
// entity-identity layer (this file): a SlotMap plus a deferred-destroy queue.
//
// Subsequent v1b–v1n slices grow this class with component registry, storage
// backends, relations, query DSL, schedule, and indexes. All of those layers
// see a stable EntityId minted here.
//
// Lifecycle (per ADR-0049):
//   spawn()              — synchronously allocates a slot and returns the handle.
//   destroy(e)           — queues `e` for destruction; the slot stays alive
//                          until flush_destroys() runs.
//   destroy_immediate(e) — frees the slot synchronously. Caller asserts no
//                          parallel iteration is in flight.
//   flush_destroys()     — drains the queue once (typically end-of-frame).
//                          Stale handles in the queue are silently skipped, so
//                          a double-destroy is safe.
class World
{
public:
    explicit World(crd::memory::IAllocator* alloc = crd::memory::default_allocator());
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete; // m_storage references *this; non-movable
    World& operator=(World&&) = delete;

    // ---- Entity lifecycle ----------------------------------------------

    [[nodiscard]] EntityId spawn();

    void destroy(EntityId e);

    void destroy_immediate(EntityId e);

    void flush_destroys();

    // ---- Queries -------------------------------------------------------

    [[nodiscard]] bool is_alive(EntityId e) const noexcept { return m_slots.is_alive(e); }

    [[nodiscard]] crd::u32 entity_count() const noexcept { return m_slots.alive_count(); }

    [[nodiscard]] crd::u32 pending_destroy_count() const noexcept
    {
        return static_cast<crd::u32>(m_pending_destroy.size());
    }

    // Range over alive entities. Order matches slot index.
    [[nodiscard]] SlotMap::Iterator begin() const noexcept { return m_slots.begin(); }
    [[nodiscard]] SlotMap::Iterator end() const noexcept { return m_slots.end(); }

    // ---- Component registry --------------------------------------------
    // Phase 3.0 v1b: registration grammar. Storage backends and indexes that
    // act on the registered metadata land in v1c–v1i. ADRs 0050, 0053, 0056.

    template <typename T, typename... Traits> ComponentId register_component(Traits&&... traits)
    {
        return m_components.register_type<T>(std::forward<Traits>(traits)...);
    }

    [[nodiscard]] const ComponentInfo* component_info(ComponentId id) const noexcept { return m_components.info(id); }

    template <typename T> [[nodiscard]] ComponentId component_id() const noexcept { return m_components.id_of<T>(); }

    [[nodiscard]] crd::u16 registered_component_count() const noexcept { return m_components.size(); }

    [[nodiscard]] const ComponentRegistry& components() const noexcept { return m_components; }

    // ---- Typed component API (Phase 3.0 v1c2; ADR-0050) ---------------
    //
    // add_component<T>(e, value)    UPSERT — value replaces any prior T on e.
    //                                Asserts T is registered + e is alive.
    // has_component<T>(e)           O(1) — archetype mask test.
    // get_component<T>(e)           const access; does not bump version.
    // get_component_mut<T>(e)       mutable access; bumps chunk version on entry.
    // remove_component<T>(e)        moves e to (mask & ~T) archetype.

    template <typename T> ComponentId require_component_id() const
    {
        ComponentId id = m_components.id_of<T>();
        CRD_ASSERT(!id.is_null());
        return id;
    }

    template <typename T> void add_component(EntityId e, T value)
    {
        CRD_ASSERT(is_alive(e));
        const ComponentId id = require_component_id<T>();
        // `value` is a local; storage may move-from it. Backend selection
        // routes by ComponentInfo::storage_hint (ADR-0050).
        backend_for(id).insert(e, id, static_cast<void*>(&value));
    }

    template <typename T> [[nodiscard]] bool has_component(EntityId e) const noexcept
    {
        if (!is_alive(e))
        {
            return false;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return false;
        }
        return backend_for_const(id).has(e, id);
    }

    template <typename T> [[nodiscard]] const T* get_component(EntityId e) const
    {
        if (!is_alive(e))
        {
            return nullptr;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return nullptr;
        }
        const ComponentInfo* info = m_components.info(id);
        CRD_ASSERT(info != nullptr);
        if (info->storage_hint == StorageHint::SparseSet)
        {
            return static_cast<const T*>(m_sparse_storage.get_const(e, id));
        }
        return static_cast<const T*>(m_storage.get_const(e, id));
    }

    template <typename T> [[nodiscard]] T* get_component_mut(EntityId e)
    {
        if (!is_alive(e))
        {
            return nullptr;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return nullptr;
        }
        return static_cast<T*>(backend_for(id).get_mut(e, id));
    }

    template <typename T> void remove_component(EntityId e)
    {
        if (!is_alive(e))
        {
            return;
        }
        const ComponentId id = m_components.id_of<T>();
        if (id.is_null())
        {
            return;
        }
        backend_for(id).remove(e, id);
    }

    [[nodiscard]] ArchetypeChunkStorage& storage() noexcept { return m_storage; }
    [[nodiscard]] const ArchetypeChunkStorage& storage() const noexcept { return m_storage; }

    [[nodiscard]] SparseSetStorage& sparse_storage() noexcept { return m_sparse_storage; }
    [[nodiscard]] const SparseSetStorage& sparse_storage() const noexcept { return m_sparse_storage; }

    [[nodiscard]] crd::u32 archetype_count() const noexcept { return m_storage.graph().archetype_count(); }

    // Mixed-backend chunk visitor (Phase 3.0 v1e, ADR-0050 §5).
    //
    // Yields chunks containing entities that satisfy `required` ACROSS both
    // storage backends. The DSL (v1g) sits on top of this primitive; callers
    // that already know `required` is pure-archetype or pure-SparseSet should
    // call the per-backend method directly to skip the dispatch.
    //
    // ChunkView semantics by path:
    //   - Pure-archetype path: chunk view forwarded from ArchetypeChunkStorage.
    //     `present_mask = archetype.mask` (a superset of required); `entities`
    //     points into the chunk's entity_id_array.
    //   - Pure-SparseSet single-bit path: forwarded from SparseSetStorage.
    //     `present_mask = {c}`; `entities` points into the pool's entities
    //     array.
    //   - Pure-SparseSet multi-bit OR mixed path: chunk view is constructed
    //     from a stack-local scratch buffer. `present_mask = required` (exact);
    //     `entities` points into the scratch and is valid ONLY for the
    //     duration of the visitor call.
    //
    // Visitors that compare `present_mask` should treat it as a superset of
    // `required` regardless of path. To access component data, prefer
    // `world.get_component_mut<T>(entity)` per entity in the mixed/multi-sparse
    // path — direct chunk-slot indexing only works for the pure-archetype
    // forwarded path.
    //
    // Threading: not thread-safe. par_each across yielded chunks is the
    // expected parallel path (the visitor dispatches one job per chunk).
    void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data);

    // Install one sink across both storage backends. World drives
    // on_entity_destroyed itself (once per destroy), so backends never fire
    // sink->on_entity_destroyed — they only emit per-component on_remove.
    void set_storage_event_sink(IStorageEventSink* sink) noexcept
    {
        m_event_sink = (sink != nullptr) ? sink : NullStorageEventSink::instance();
        m_storage.set_event_sink(m_event_sink);
        m_sparse_storage.set_event_sink(m_event_sink);
    }

    // ---- Relation API (Phase 3.0 v1f, ADR-0051) ------------------------
    //
    // A relation is `(Tag, target)` modelled as a component of type
    // `Relation<Tag>` carrying the target EntityId. The full grammar:
    //
    //   register_relation<Tag>(traits...)        registers Relation<Tag> as a
    //                                            component with relation traits
    //                                            (ReverseIndex, Acyclic,
    //                                            OnTargetDestroyed); idempotent.
    //   add_relation<Tag>(src, target)           UPSERT — installs the relation;
    //                                            updates reverse index; debug-
    //                                            mode cycle assert when Acyclic.
    //   remove_relation<Tag>(src)                drops relation + reverse-index
    //                                            entry; no-op when absent.
    //   get_relation_target<Tag>(src)            current target or null.
    //   has_relation<Tag>(src)                   bool.
    //   would_form_cycle<Tag>(src, target)       public predicate; tests use
    //                                            this to verify cycle detection
    //                                            without tripping assertions.
    //   traverse_relation<Tag>(root, visitor)    DFS pre-order using the
    //                                            reverse index; visitor sees
    //                                            (entity, depth) starting at
    //                                            (root, 0). Requires
    //                                            ReverseIndex on the relation.
    //   register_builtin_relations()             registers all six built-ins
    //                                            (ChildOf / AttachedTo / Owns /
    //                                            Targets / DependsOn /
    //                                            PossessedBy) with canonical
    //                                            defaults. Idempotent — call
    //                                            override-registrations BEFORE
    //                                            this if you want non-default
    //                                            traits.
    //
    // `OnTargetDestroyed` fires inside `destroy_immediate` and `flush_destroys`
    // via an iterative worklist: when an entity is destroyed, every registered
    // relation looks up its reverse_sources entry and applies its policy
    // (Cascade / Detach / SetNull). A Cascade enqueues affected sources back
    // onto the worklist — recursion is iterative, so a 100-deep ChildOf tree
    // never overflows the stack.

    template <typename Tag, typename... Traits>
    ComponentId register_relation(Traits&&... traits)
    {
        // Forward to the component registry; trait dispatchers in
        // component_registry.hpp set the relation flags on ComponentInfo.
        // is_relation = true is stamped automatically because T = Relation<Tag>.
        const ComponentId id = m_components.register_type<Relation<Tag>>(std::forward<Traits>(traits)...);
        on_relation_registered(id);
        return id;
    }

    template <typename Tag> [[nodiscard]] ComponentId relation_id() const noexcept
    {
        return m_components.id_of<Relation<Tag>>();
    }

    template <typename Tag> void add_relation(EntityId src, EntityId target)
    {
        CRD_ASSERT(is_alive(src));
        const ComponentId id = require_component_id<Relation<Tag>>();
        add_relation_impl(id, src, target);
    }

    template <typename Tag> void remove_relation(EntityId src)
    {
        if (!is_alive(src))
        {
            return;
        }
        const ComponentId id = m_components.id_of<Relation<Tag>>();
        if (id.is_null())
        {
            return;
        }
        remove_relation_impl(id, src);
    }

    template <typename Tag> [[nodiscard]] EntityId get_relation_target(EntityId src) const
    {
        if (!is_alive(src))
        {
            return EntityId::null();
        }
        const ComponentId id = m_components.id_of<Relation<Tag>>();
        if (id.is_null())
        {
            return EntityId::null();
        }
        const Relation<Tag>* r = static_cast<const Relation<Tag>*>(get_relation_payload_const(id, src));
        return (r != nullptr) ? r->target : EntityId::null();
    }

    template <typename Tag> [[nodiscard]] bool has_relation(EntityId src) const
    {
        return !get_relation_target<Tag>(src).is_null();
    }

    template <typename Tag> [[nodiscard]] bool would_form_cycle(EntityId src, EntityId target) const
    {
        const ComponentId id = m_components.id_of<Relation<Tag>>();
        if (id.is_null())
        {
            return false;
        }
        return would_form_cycle_impl(id, src, target);
    }

    using RelationVisitorFn = void (*)(EntityId entity, crd::u32 depth, void* user_data);

    template <typename Tag, typename Visitor> void traverse_relation(EntityId root, Visitor&& visitor) const
    {
        const ComponentId id = m_components.id_of<Relation<Tag>>();
        if (id.is_null())
        {
            return;
        }
        // Tunnel the visitor through a stateless function pointer + ud.
        // Keeps the DFS body uninlined and avoids std::function overhead.
        struct Ctx
        {
            Visitor* vis;
        };
        Ctx ctx{&visitor};
        traverse_relation_impl(
            id, root,
            [](EntityId entity, crd::u32 depth, void* ud)
            {
                Ctx* c = static_cast<Ctx*>(ud);
                (*c->vis)(entity, depth);
            },
            &ctx);
    }

    // Built-in relations — registers all six with canonical defaults.
    // Idempotent: re-registering a built-in already explicitly registered
    // (e.g. with custom traits) is a no-op.
    void register_builtin_relations();

private:
    [[nodiscard]] IStorageBackend& backend_for(ComponentId id) noexcept
    {
        const ComponentInfo* info = m_components.info(id);
        CRD_ASSERT(info != nullptr);
        return (info->storage_hint == StorageHint::SparseSet) ? static_cast<IStorageBackend&>(m_sparse_storage)
                                                              : static_cast<IStorageBackend&>(m_storage);
    }

    [[nodiscard]] const IStorageBackend& backend_for_const(ComponentId id) const noexcept
    {
        const ComponentInfo* info = m_components.info(id);
        CRD_ASSERT(info != nullptr);
        return (info->storage_hint == StorageHint::SparseSet) ? static_cast<const IStorageBackend&>(m_sparse_storage)
                                                              : static_cast<const IStorageBackend&>(m_storage);
    }

    // Per-relation reverse-index payload. Lazy-allocated by
    // on_relation_registered() when ReverseIndex{} was passed at registration.
    // Indexed by ComponentId.raw in m_relations; null otherwise.
    struct RelationInfo
    {
        crd::containers::HashMap<EntityId, crd::containers::Array<EntityId>> reverse_sources;
        crd::memory::IAllocator* alloc = nullptr;

        explicit RelationInfo(crd::memory::IAllocator* a) noexcept : reverse_sources(a), alloc(a) {}

        // Add `src` to reverse_sources[target]. Lazy-creates the inner Array
        // with the World's allocator so we don't fall back to default_allocator.
        void add_reverse(EntityId target, EntityId src);
        // Remove `src` from reverse_sources[target]; erase the key if empty.
        void remove_reverse(EntityId target, EntityId src);
        // Pop and return the entire sources array for `target`, erasing the key.
        // Returns empty Array if the key is absent.
        [[nodiscard]] crd::containers::Array<EntityId> take_sources(EntityId target);
    };

    // Hooks called from the relation API templates; keep the heavy code out
    // of the header.
    void on_relation_registered(ComponentId id);
    void add_relation_impl(ComponentId id, EntityId src, EntityId target);
    void remove_relation_impl(ComponentId id, EntityId src);
    [[nodiscard]] bool would_form_cycle_impl(ComponentId id, EntityId src, EntityId target) const noexcept;
    void traverse_relation_impl(ComponentId id, EntityId root, RelationVisitorFn fn, void* user_data) const;
    [[nodiscard]] const void* get_relation_payload_const(ComponentId id, EntityId src) const;

    // Apply OnTargetDestroyed policy across every registered relation when an
    // entity is being destroyed. Pushes Cascade-affected sources onto the
    // worklist for iterative drain in destroy paths.
    void apply_on_target_destroyed(EntityId destroyed, crd::containers::Array<EntityId>& worklist);

    // Walk every registered relation; if `e` has Relation<Tag>{target}, remove
    // (target, e) from that relation's reverse_sources. Called once before
    // backend drain — after the backend tears the components down, the targets
    // are unrecoverable. The "outgoing" half of relation cleanup; the
    // "incoming" half is `apply_on_target_destroyed`.
    void cleanup_outgoing_relations(EntityId e);

    // Iterative destruction loop. Each iteration: alive-check (diamond
    // dedup), apply incoming on-destroy policy, scrub outgoing reverse-index
    // entries, fire sink, drain both backends, free the slot. Cascades push
    // new entities onto the worklist; loop terminates when worklist empties.
    void drain_destruction_worklist(crd::containers::Array<EntityId>& worklist);

    SlotMap m_slots;
    crd::containers::Array<EntityId> m_pending_destroy;
    ComponentRegistry m_components;
    ArchetypeChunkStorage m_storage;
    SparseSetStorage m_sparse_storage;
    IStorageEventSink* m_event_sink;
    // Per-relation info, indexed by ComponentId.raw. Pre-sized to
    // kMaxComponents; entries are nullptr until register_relation() lazily
    // allocates them.
    crd::containers::Array<RelationInfo*> m_relations;
};

} // namespace crd::scene
