#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype_chunk_storage.hpp>
#include <crd/scene/async_aware_index.hpp>
#include <crd/scene/change_detect_index.hpp>
#include <crd/scene/commands.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/query.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/reserved_indexes.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/slot_map.hpp>
#include <crd/scene/sparse_set_storage.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/storage_event_sink.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>

#include <memory>
#include <utility>

// Forward-declare crd::jobs::Counter for the (currently unused) job-system
// hooks reserved on World. `Counter` is a type alias of
// `detail::Counter` in <crd/jobs/jobs.hpp>; matching that shape here lets
// TUs that include both world.hpp and jobs.hpp coexist without the alias
// conflicting with a class-style forward declaration.
namespace crd::jobs::detail
{
struct Counter;
}
namespace crd::jobs
{
using Counter = detail::Counter;
}

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
        const ComponentId id = m_components.register_type<T>(std::forward<Traits>(traits)...);
        // v1i: auto-register the indexes implied by the trait flags
        // stamped onto ComponentInfo by register_type. Honours ADR-0053's
        // "trait registration is enough" contract — users opt in once
        // via register_component(History{60}) and the corresponding
        // index appears automatically.
        auto_register_indexes_for(id);
        return id;
    }

    [[nodiscard]] const ComponentInfo* component_info(ComponentId id) const noexcept { return m_components.info(id); }

    template <typename T> [[nodiscard]] ComponentId component_id() const noexcept { return m_components.id_of<T>(); }

    [[nodiscard]] crd::u16 registered_component_count() const noexcept { return m_components.size(); }

    [[nodiscard]] const ComponentRegistry& components() const noexcept { return m_components; }

    // Allocator the World was constructed with — every Array / HashMap /
    // backend allocates through this. Exposed for higher-level constructs
    // (Query, future System helpers) that want to extend the chain rather
    // than fall back to default_allocator.
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_pending_destroy.allocator(); }

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

    // REN-36.3-b: the RUNTIME (non-template) archetype test. An authored frame-graph draw list names its
    // components as STRINGS, so a renderer resolves them to `ComponentId`s and must test them WITHOUT a
    // compile-time type. Same O(1) mask test as `has_component<T>`; it simply skips the type -> id lookup the
    // caller has already done.
    [[nodiscard]] bool has_component_id(EntityId e, ComponentId id) const noexcept
    {
        if (id.is_null() || !is_alive(e)) { return false; }
        return backend_for_const(id).has(e, id);
    }

    // REN-36.3-b: resolve a component's AUTHORED name to its id. The registry stores `typeid(T).name()`, which is
    // DECORATED DIFFERENTLY PER ABI, so the match must understand both spellings:
    //   · MSVC:    "struct crd::scene::MeshRenderer"   — the plain name, so the decorated string ENDS with it.
    //   · Itanium: "N3crd5scene12MeshRendererE"        — LENGTH-PREFIXED components and a trailing 'E'.
    // ⛔⛔ REN-38 (2026-07-27): the original matcher only did the MSVC trailing test, so on gcc/clang the name
    // ended in 'E' and NOTHING ever matched — every authored `all`/`any`/`none` component filter rejected every
    // group, silently, on every Linux/macOS build. The renderer's draw list then resolved EMPTY and the frame
    // drew nothing. It looked like a Vulkan-implementation bug (it surfaced on llvmpipe) and was a MANGLING bug.
    // ⛔ Returns null for an unknown name. The caller must REPORT that, not treat it as "matches everything" — a
    // silently-ignored filter is worse than an unsupported one, because it reads as working.
    [[nodiscard]] static bool name_char_is_ident(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }
    // Does `decorated` name exactly the component `want`, under either ABI's decoration?
    [[nodiscard]] static bool decorated_names(crd::containers::StringView decorated,
                                              crd::containers::StringView want) noexcept
    {
        if (want.size() == 0U || decorated.size() < want.size()) { return false; }
        // ── MSVC / plain: the decorated name ENDS with `want`, on an identifier boundary. ──
        {
            const crd::usize off = decorated.size() - want.size();
            bool             eq  = true;
            for (crd::usize k = 0; k < want.size() && eq; ++k) { eq = decorated[off + k] == want[k]; }
            if (eq && (off == 0U || !name_char_is_ident(decorated[off - 1U]))) { return true; }
        }
        // ── Itanium: the component appears as "<decimal length><name>", e.g. "12MeshRenderer". Matching the
        //    LENGTH PREFIX is exact — it cannot collide with a longer name that merely ends the same way. ──
        char       digits[8]{};
        crd::u32   ndig = 0U;
        crd::usize n    = want.size();
        while (n > 0U && ndig < 8U) { digits[ndig++] = static_cast<char>('0' + (n % 10U)); n /= 10U; }
        if (ndig == 0U || n != 0U) { return false; } // unrepresentable length ⇒ no Itanium form to look for
        for (crd::usize i = 0; i + ndig + want.size() <= decorated.size(); ++i)
        {
            bool eq = true;
            for (crd::u32 d = 0; d < ndig && eq; ++d) { eq = decorated[i + d] == digits[ndig - 1U - d]; } // MSB first
            if (!eq) { continue; }
            if (i > 0U && decorated[i - 1U] >= '0' && decorated[i - 1U] <= '9') { continue; } // mid-number, not a prefix
            for (crd::usize k = 0; k < want.size() && eq; ++k) { eq = decorated[i + ndig + k] == want[k]; }
            if (!eq) { continue; }
            // the component must END here: next is the mangling's terminator/another length, never more identifier
            const crd::usize after = i + ndig + want.size();
            if (after == decorated.size()) { return true; }
            const char c = decorated[after];
            if (c == 'E' || (c >= '0' && c <= '9') || !name_char_is_ident(c)) { return true; }
        }
        return false;
    }
    [[nodiscard]] ComponentId component_id_by_name(crd::containers::StringView want) const noexcept
    {
        for (crd::u16 i = 0; i < m_components.size(); ++i)
        {
            const ComponentInfo* info = m_components.info(ComponentId{i});
            if (info == nullptr) { continue; }
            if (decorated_names(info->name, want)) { return info->id; }
        }
        return ComponentId{};
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

    // Install an EXTERNAL test/debug sink that runs alongside any
    // registered indexes. Pre-v1i contract: this sink received every
    // event. v1i contract: this sink runs ALONGSIDE indexes; in code
    // that doesn't register any indexes (matches every test prior to
    // v1i), behaviour is identical. Code that mixes registered indexes
    // and a test sink: the sink sees every event the fan-out dispatches.
    //
    // World drives on_entity_destroyed itself (once per destroy across
    // both backends — see destroy_immediate / flush_destroys). Storage
    // backends still fire per-component on_remove during their drain.
    void set_storage_event_sink(IStorageEventSink* sink) noexcept
    {
        m_external_sink = (sink != nullptr) ? sink : NullStorageEventSink::instance();
    }

    // ---- Scene serialisation (Phase 3.0 v1k, ADR-0055) -----------------
    //
    // Instantiate a loaded SceneResource into this World. Spawns one
    // EntityId per file-local index, restores components by FourCC
    // lookup, and installs relations.
    //
    // Forward-compatibility (advisor pin #1):
    //   - Unknown FourCC on a component-or-relation → skipped (counted
    //     in `components_skipped` / `relations_skipped`); the entity
    //     itself is still spawned with whatever known components it has.
    //   - Known FourCC + size or alignment mismatch with the registered
    //     type → CRD_FATAL during instantiation. Silent best-effort
    //     would corrupt downstream state; loaders sit at the trust
    //     boundary, fail loudly.
    //   - Known FourCC + version mismatch with registered ComponentSerialize
    //     → CRD_FATAL. Versions are explicit migration opt-ins.
    //
    // Result: a SceneInstantiation move-only struct mapping file-local
    // index → live EntityId. Caller code uses this to find spawned
    // entities by name, hand them to systems, etc.
    [[nodiscard]] SceneInstantiation instantiate_scene(const SceneResource& res);

    // ---- Öbek instantiation (Phase 3.0 v1m, ADR-0058) ------------------
    //
    // Spawn one EntityId per file-local index in the öbek; restore
    // components by FourCC lookup; install relations. If `parent` is
    // alive, every öbek root (an entity with no ChildOf relation in the
    // source) gets a Relation<ChildOf> to `parent` installed during
    // instantiation. Pass EntityId::null() to disable reparenting (the
    // öbek roots become top-level entities in this World).
    //
    // Forward-compat + hard-fail rules match instantiate_scene:
    //   - Unknown FourCC → skipped (counted in components_skipped /
    //     relations_skipped); entity still spawned.
    //   - Known FourCC + size/alignment/version mismatch → CRD_FATAL.
    //
    // v1m1 ships the basic restore + reparent path; v1m2 layers override
    // patches; v1m3 lights up nested öbek sub-instance tracking; v1m5
    // adds unpack / revert APIs.
    [[nodiscard]] ObekInstantiation instantiate_obek(const ObekResource& res, EntityId parent = EntityId::null(),
                                                     crd::containers::ConstSpan<ObekOverride> overrides = {});

    // ---- v1m5a — revert / unpack / enumerate APIs (ADR-0058 pillar 7) ---
    //
    // These rebuild entity component bytes from the öbek's stored source
    // payload + cook-time OOVR overrides, ignoring any runtime mutations
    // the caller has made through `get_component_mut`. The instance's
    // `source` pointer must be valid (set by `instantiate_obek`); calling
    // any revert API on an unpacked instance is a no-op.
    //
    // All APIs are idempotent and safe to call repeatedly.

    // Revert a specific byte range within a component on the entity at
    // `file_idx` to the value baked at instantiation time (source +
    // cook-time overrides). Out-of-range or unknown component → no-op.
    void revert_field(ObekInstantiation& inst, crd::u32 file_idx, crd::u32 component_fourcc, crd::u32 field_offset,
                      crd::u32 field_size);

    // Revert the whole component (every byte) on the entity at `file_idx`.
    void revert_component(ObekInstantiation& inst, crd::u32 file_idx, crd::u32 component_fourcc);

    // Revert every component on the entity at `file_idx`.
    void revert_entity(ObekInstantiation& inst, crd::u32 file_idx);

    // Revert every entity in the instance.
    void revert_all(ObekInstantiation& inst);

    // Sever the instance ↔ source link AND revert all entities to their
    // post-instantiate state. After this call, the instance's entities are
    // plain World data; the öbek source can be safely unloaded.
    void unpack_obek(ObekInstantiation& inst);

    // Sever the link WITHOUT reverting. Entities keep whatever state they
    // currently have (including any runtime mutations). After this call,
    // the öbek source can be safely unloaded.
    void unpack_obek_keep_overrides(ObekInstantiation& inst);

    // Return the cook-time override records that were baked into the
    // öbek's OOVR chunk (i.e. authored in the `.obek.toml`'s
    // `overrides = [...]` block). Returns an empty span if the instance
    // has been unpacked. Used by editor "override window" UIs to display
    // what overrides exist on a given instance.
    [[nodiscard]] crd::containers::ConstSpan<ObekOverrideRecord>
    enumerate_overrides(const ObekInstantiation& inst) const noexcept;

    // ---- v1m5b — AAAA-tier batch instantiation (ADR-0058 pillar 15a) ---
    //
    // Spawn `count` instances of `res` at once. Returns an `ObekBatchHandle`
    // that the renderer (Phase 3.5+) reads to detect shared-draw eligibility.
    //
    // For each slot i in [0, count):
    //   1. instantiate_obek(res, parent) is called once per slot.
    //   2. If `BatchInstanceTag` is registered on this World, every spawned
    //      entity for slot i is tagged with `BatchInstanceTag{batch, i}`.
    //
    // GEO-7 (D-007 row 72) delivers the reserved transforms path: `transforms[slot]` is the slot's LOCAL TRS,
    // UPSERTED onto every ROOT entity of that slot's instance (source-roots — reparented to `parent` when given)
    // and the subtree marked dirty, so one propagation pass places the whole batch (one asset → N placed
    // instances — the renderer's instanced-draw feed). Fewer transforms than slots: the tail spawns unplaced
    // (source-authored transforms). Empty span = the v1m5b behavior, unchanged.
    //
    // Each call returns a unique `ObekBatchHandle`. v1m5b uses a monotonic
    // per-World counter; no persistence concerns.
    [[nodiscard]] ObekBatchHandle instantiate_obek_batch(const ObekResource& res, crd::u32 count,
                                                         EntityId parent = EntityId::null(), BatchHints hints = {},
                                                         crd::containers::ConstSpan<Transform> transforms = {});

    // ---- Transform writer API (Phase 3.0 v1j, ADR-0054) ----------------
    //
    // Six rotation-set entry points covering the cross-domain space:
    //   - quat: direct (auto-normalised) — the default.
    //   - quat_unnormalized: explicit opt-out for precision-preserving
    //     domains (robotics control loops, aerospace IMU integration).
    //   - axis_angle: servo / joint controller input.
    //   - euler: explicit-order Euler input — pass an EulerOrder enum to
    //     avoid Tait-Bryan ambiguity.
    //   - from_to: shortest-arc rotation (look-at semantics for arrows /
    //     missile lock-on).
    //   - look_at: forward + up convention.
    //
    // All writers:
    //   1. Mutate the Transform via get_component_mut<Transform>(e) →
    //      fires ChangeDetect on_update for downstream consumers.
    //   2. Add TransformDirtyFlag to `e` AND every ChildOf descendant
    //      via mark_transform_subtree_dirty (debug-asserts depth <
    //      kMaxTransformDepth = 256).
    //
    // Propagation system in PreRender phase consumes the dirty flags,
    // recomputes world matrices, removes the flags via Commands.
    //
    // Robustness pin: write APIs CRD_ASSERT(is_alive(e)) in debug.
    // Releasing on a dead entity is a programmer error — silent failure
    // would corrupt downstream state.

    void set_translation(EntityId e, crd::math::Vec3f t);
    void set_rotation_quat(EntityId e, crd::math::Quatf q);
    void set_rotation_quat_unnormalized(EntityId e, crd::math::Quatf q);
    void set_rotation_axis_angle(EntityId e, crd::math::Vec3f axis, crd::f32 radians);
    void set_rotation_euler(EntityId e, crd::f32 x, crd::f32 y, crd::f32 z,
                            crd::math::EulerOrder order = crd::math::EulerOrder::XYZ_Intrinsic);
    void set_rotation_from_to(EntityId e, crd::math::Vec3f from_dir, crd::math::Vec3f to_dir);
    void set_rotation_look_at(EntityId e, crd::math::Vec3f forward,
                              crd::math::Vec3f up = crd::math::Vec3f{static_cast<crd::f32>(0), static_cast<crd::f32>(1),
                                                                     static_cast<crd::f32>(0)});
    void set_scale(EntityId e, crd::math::Vec3f s);

    // Whole-transform setters.
    void set_local(EntityId e, const crd::math::Vec3f& translation, const crd::math::Quatf& rotation,
                   const crd::math::Vec3f& scale = crd::math::Vec3f{static_cast<crd::f32>(1), static_cast<crd::f32>(1),
                                                                    static_cast<crd::f32>(1)});

    // set_world(world) — best-effort decompose; CRD_ASSERT in debug on
    // singular matrix. Negative-determinant succeeds with X-scale negated
    // (mirror handedness preserved per advisor decision #4).
    void set_world(EntityId e, const crd::math::Mat4f& world);

    // try_set_world — validates the matrix; returns false WITHOUT mutating
    // the Transform on degenerate input. Same negative-determinant
    // semantic as set_world.
    [[nodiscard]] bool try_set_world(EntityId e, const crd::math::Mat4f& world);

    // Mark `e` and every ChildOf descendant dirty for the next
    // TransformPropagation pass. Public so user systems that bypass the
    // typed setters above (e.g. raw get_component_mut<Transform>) can
    // notify propagation explicitly.
    void mark_transform_subtree_dirty(EntityId e);

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

    template <typename Tag, typename... Traits> ComponentId register_relation(Traits&&... traits)
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

    // The visitor is tunnelled by ADDRESS through a stateless fn-ptr + user-data (never invoked as a forwarded
    // value); forwarding would dangle the Ctx pointer — hence the suppression.
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
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

    // ---- Query DSL factory (Phase 3.0 v1g, ADR-0052) -------------------
    //
    // Constructs a Query<Cs...> over this world. The Cs... pack defines
    // the per-entity tuple yielded by range-for iteration. Add filters via
    // .with<>() / .without<>() / .with_relation<>() / .filter() — see
    // query.hpp.
    //
    // Returned by value via guaranteed copy elision (C++17+). The chain
    // syntax `world.query<A>().with<B>().without<C>()` makes ZERO copies
    // — each .with<>() returns Query& and the final assignment binds to
    // the elided rvalue.
    template <typename... Cs> [[nodiscard]] Query<Cs...> query() { return Query<Cs...>{*this}; }

    // ---- Index framework (Phase 3.0 v1i, ADR-0053) ---------------------
    //
    // Layer 5 plug-point. Indexes register with the World; the storage
    // backends fan events out to every registered index whose observed()
    // mask matches the touched component. v1i ships ChangeDetectIndex and
    // AsyncAwareIndex; ADR-0053's reserved indexes (History, SpatialBVH,
    // GpuResident, Replication, Reflection) ship as auto-registered no-op
    // shells that round-trip the trait grammar.
    //
    // Auto-registration: register_component<T>(traits...) inspects the
    // resulting ComponentInfo and lazy-creates the indexes implied by the
    // trait flags (AsyncAware{} → AsyncAwareIndex, History{N} →
    // HistoryIndex, etc.). ChangeDetectIndex auto-registers on first
    // component registration.
    //
    // Manual registration (for user-defined indexes):
    //   auto* metrics = world.register_index<MetricsIndex>(args...);
    //
    // The unique_ptr<IComponentIndex> is owned by World; the raw return
    // pointer is non-owning and remains valid for the World's lifetime.

    template <typename Idx, typename... Args> Idx* register_index(Args&&... args)
    {
        auto owned = std::make_unique<Idx>(std::forward<Args>(args)...);
        Idx* raw = owned.get();
        m_indexes.push_back(std::move(owned));
        return raw;
    }

    // Find a registered index by exact dynamic type. Returns nullptr if
    // not registered. Used by query operators (.changed<T>(), etc.) to
    // discover their backing index.
    template <typename Idx> [[nodiscard]] Idx* find_index() noexcept
    {
        for (auto& slot : m_indexes)
        {
            if (Idx* casted = dynamic_cast<Idx*>(slot.get()); casted != nullptr)
            {
                return casted;
            }
        }
        return nullptr;
    }
    template <typename Idx> [[nodiscard]] const Idx* find_index() const noexcept
    {
        for (const auto& slot : m_indexes)
        {
            if (const Idx* casted = dynamic_cast<const Idx*>(slot.get()); casted != nullptr)
            {
                return casted;
            }
        }
        return nullptr;
    }

    // Number of registered indexes. Diagnostics / tests.
    [[nodiscard]] crd::usize index_count() const noexcept { return m_indexes.size(); }

    // Current frame index. Incremented at the START of every step() and
    // step_fixed() call. ChangeDetectIndex consumes this to drive the
    // ".changed<T>() = modified during current frame" semantic.
    [[nodiscard]] crd::u32 current_frame() const noexcept { return m_frame_index; }

    // Leftover seconds in the fixed-step accumulator AFTER the most
    // recent step_fixed call ran its substeps. Variable-rate render-
    // path systems consume this for "fixed timestep with interpolation"
    // (Glenn Fiedler, "Fix Your Timestep") — render frames between two
    // fixed substeps interpolate via:
    //
    //     alpha = world.fixed_step_alpha(fixed_dt);
    //     render_state = lerp(prev_state, curr_state, alpha);
    //
    // where prev_state is the body state BEFORE the most recent integrate
    // and curr_state is the state AFTER. Returns 0.0 if step_fixed has
    // never been called (or the leftover happens to be exactly 0).
    [[nodiscard]] crd::f64 fixed_step_accumulator() const noexcept { return m_fixed_accumulator; }
    [[nodiscard]] crd::f64 fixed_step_alpha(crd::f64 fixed_dt) const noexcept
    {
        if (fixed_dt <= 0.0)
        {
            return 0.0;
        }
        const crd::f64 a = m_fixed_accumulator / fixed_dt;
        if (a < 0.0) { return 0.0; }
        return a > 1.0 ? 1.0 : a;
    }

    // ---- Schedule + Commands (Phase 3.0 v1h, ADR-0052 §3-§5) -----------

    // Register a system into the fixed 7-phase schedule. The system runs
    // in its declared phase (per ISystem::phase()), in registration order
    // among other systems in the same phase. Mid-frame registration is
    // allowed: a system registered from another system's run() runs from
    // the next phase the schedule visits.
    void register_system(std::unique_ptr<ISystem> system);

    // Run all 7 phases once with `dt`. Variable-rate systems run with this
    // `dt`; fixed-step systems are also invoked once (with the same dt).
    // Use step_fixed for determinism-mode dispatch.
    //
    // After each phase's systems run, the World waits on the phase fence
    // (no-op if no jobs were submitted) and flushes the command buffer.
    void step(crd::f64 dt);

    // Run all 7 phases under deterministic fixed-step semantics. Each
    // phase runs:
    //   1. Fixed-step systems N times where N = floor(accum / fixed_dt),
    //      clamped to max_substeps. Accumulator persists across calls so
    //      remainders carry forward.
    //   2. Variable-rate systems exactly once with the real `dt`.
    //
    // Standard Bevy `FixedUpdate` shape. v1h uses a SINGLE GLOBAL
    // fixed_dt (the function argument); per-system fixed_dt is reserved
    // for v1h+1 along with per-system accumulators.
    void step_fixed(crd::f64 dt, crd::f64 fixed_dt, crd::u32 max_substeps);

    // World's deferred-mutation buffer. Used by systems iterating in
    // parallel; v1h serial users may also use it for ergonomic batched
    // entity construction. Drained at every phase boundary.
    [[nodiscard]] Commands& commands() noexcept { return m_commands_buffer; }

    // Compute the ComponentMask of a ComponentSet<Ts...>. Used by tooling
    // (and Phase 3.5 auto-parallel scheduling). Reads the registry — must
    // be called after every Cs in Set is registered.
    template <typename Set> [[nodiscard]] ComponentMask component_set_mask() const noexcept;

    // ---- Type-erased access for Commands / future tooling --------------
    //
    // These mirror the private template paths but take a runtime
    // ComponentId. Used by Commands' flush path to apply queued mutations
    // without re-instantiating templates per command. Public so any
    // tooling code that builds id-driven mutations (editor undo/redo,
    // serialisation, scripting) can call them.

    [[nodiscard]] IStorageBackend& backend_for_public(ComponentId id) noexcept { return backend_for(id); }

    void add_relation_via_id(ComponentId relation_id, EntityId src, EntityId target)
    {
        CRD_ASSERT(is_alive(src));
        add_relation_impl(relation_id, src, target);
    }

    void remove_relation_via_id(ComponentId relation_id, EntityId src)
    {
        if (!is_alive(src))
        {
            return;
        }
        remove_relation_impl(relation_id, src);
    }

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
    // Per-relation info, indexed by ComponentId.raw. Pre-sized to
    // kMaxComponents; entries are nullptr until register_relation() lazily
    // allocates them.
    crd::containers::Array<RelationInfo*> m_relations;

    // Schedule (v1h). Per-phase array of owned systems. Iteration order
    // within a phase = registration order.
    crd::containers::Array<std::unique_ptr<ISystem>> m_systems[kSchedulePhaseCount];

    // Single-allocator command buffer drained at every phase boundary.
    // (v1h ships single-threaded; v1h+1 will introduce per-fiber stripes
    // for parallel par_each.) Initialised AFTER m_pending_destroy because
    // Commands' allocator-aware Arrays read World::allocator() which
    // reads m_pending_destroy.allocator().
    Commands m_commands_buffer;

    // Accumulator for step_fixed. Carries the unused fraction of the
    // previous frame's dt forward.
    crd::f64 m_fixed_accumulator = 0.0;

    // ---- v1i: Index framework members ----------------------------------

    // Internal fan-out sink — installed on both storage backends. Routes
    // every storage event to every registered index whose observed() mask
    // includes the touched component, AND forwards to m_external_sink for
    // backward-compatible test sinks.
    class IndexFanOutSink : public IStorageEventSink
    {
    public:
        explicit IndexFanOutSink(World& w) noexcept : m_world(&w) {}

        void on_insert(EntityId e, ComponentId c, const void* data) override;
        void on_update(EntityId e, ComponentId c, const void* old_data, const void* new_data) override;
        void on_remove(EntityId e, ComponentId c, const void* data) override;
        void on_entity_destroyed(EntityId e) override;

    private:
        World* m_world;
    };

    // Indexes registered with the World, fan-out targets in registration
    // order. Auto-population for ADR-0053 reserved-slot traits happens
    // inside register_component<T>(traits...) post-stamp.
    crd::containers::Array<std::unique_ptr<IComponentIndex>> m_indexes;

    // Fan-out sink instance. Always installed on both backends (set in
    // ctor); never replaced. The external test sink coexists.
    IndexFanOutSink m_fanout_sink;

    // External test/debug sink. Runs alongside indexes via the fan-out's
    // forward path. Default: NullStorageEventSink::instance().
    IStorageEventSink* m_external_sink;

    // Auto-registration helper for ADR-0053 reserved-slot traits. Called
    // by register_component<T>(traits...) AFTER ComponentInfo is stamped.
    // Inspects the new component's flags (async_aware, history_window,
    // spatial_bvh, gpu_resident, replication, reflection) and lazy-
    // creates the corresponding global index (if not already present),
    // then extends the index's observed mask with the new ComponentId.
    void auto_register_indexes_for(ComponentId id);

    // Per-frame counter incremented at start of step() / step_fixed().
    // Drives ChangeDetectIndex's "modified during current frame" semantic.
    crd::u32 m_frame_index = 0;

    // Frame lifecycle dispatch — fired before / after the 7-phase loop.
    void notify_frame_begin();
    void notify_frame_end();
};

// ---- Query<Cs...> template method bodies --------------------------------
//
// These live below the World definition so they can name World::-prefixed
// template methods (require_component_id, get_component_mut, etc.). The
// forward declaration in query.hpp is enough for the class skeleton; the
// bodies below complete the picture for any TU that includes world.hpp.

template <typename... Cs>
Query<Cs...>::Query(World& world)
    : m_world(&world), m_relations(world.allocator()), m_predicates(world.allocator()),
      m_change_filters(world.allocator()), m_skip_pending_filters(world.allocator()), m_match_cache(world.allocator())
{
    // Stamp Cs... into the required mask. Each Cs must be a registered
    // component type by the time the query is constructed; the DSL is
    // a runtime tool, not a registration-time one.
    (m_required.set(world.components().template id_of<Cs>()), ...);
    if constexpr (sizeof...(Cs) > 0)
    {
        ComponentId ids[] = {world.components().template id_of<Cs>()...};
        for ([[maybe_unused]] ComponentId id : ids)
        {
            CRD_ASSERT(!id.is_null() && "Query<Cs...>: every Cs must be a registered component type");
        }
    }
}

template <typename... Cs> template <typename T> Query<Cs...>& Query<Cs...>::with() &
{
    const ComponentId id = m_world->components().template id_of<T>();
    CRD_ASSERT(!id.is_null() && "Query::with<T>: T must be a registered component type");
    m_required.set(id);
    invalidate_cache();
    return *this;
}

template <typename... Cs> template <typename T> Query<Cs...> Query<Cs...>::with() &&
{
    this->template with<T>();
    return std::move(*this);
}

template <typename... Cs> template <typename T> Query<Cs...>& Query<Cs...>::without() &
{
    const ComponentId id = m_world->components().template id_of<T>();
    CRD_ASSERT(!id.is_null() && "Query::without<T>: T must be a registered component type");
    m_forbidden.set(id);
    invalidate_cache();
    return *this;
}

template <typename... Cs> template <typename T> Query<Cs...> Query<Cs...>::without() &&
{
    this->template without<T>();
    return std::move(*this);
}

template <typename... Cs> template <typename Tag> Query<Cs...>& Query<Cs...>::with_relation(EntityId target) &
{
    const ComponentId id = m_world->components().template id_of<Relation<Tag>>();
    CRD_ASSERT(!id.is_null() && "Query::with_relation<Tag>: Relation<Tag> must be a registered relation");
    m_relations.push_back(RelationFilter{id, target});
    invalidate_cache();
    return *this;
}

template <typename... Cs> template <typename Tag> Query<Cs...> Query<Cs...>::with_relation(EntityId target) &&
{
    this->template with_relation<Tag>(target);
    return std::move(*this);
}

template <typename... Cs> Query<Cs...>& Query<Cs...>::filter(FilterPredicateFn fn, void* user_data) &
{
    if (fn != nullptr)
    {
        m_predicates.push_back(PredicateFilter{fn, user_data});
        invalidate_cache();
    }
    return *this;
}

template <typename... Cs> Query<Cs...> Query<Cs...>::filter(FilterPredicateFn fn, void* user_data) &&
{
    this->filter(fn, user_data);
    return std::move(*this);
}

template <typename... Cs> template <typename T> Query<Cs...>& Query<Cs...>::changed() &
{
    const ComponentId id = m_world->components().template id_of<T>();
    CRD_ASSERT(!id.is_null() && "Query::changed<T>: T must be a registered component type");
    const ChangeDetectIndex* index = m_world->template find_index<ChangeDetectIndex>();
    // ChangeDetectIndex is auto-registered on the first register_component;
    // it should always exist by query-construction time. If it doesn't,
    // the predicate naturally returns false (no entries match).
    m_change_filters.push_back(ChangeDetectFilter{index, id, m_world->current_frame()});
    invalidate_cache();
    return *this;
}

template <typename... Cs> template <typename T> Query<Cs...> Query<Cs...>::changed() &&
{
    this->template changed<T>();
    return std::move(*this);
}

template <typename... Cs> template <typename T> Query<Cs...>& Query<Cs...>::skip_pending() &
{
    const ComponentId id = m_world->components().template id_of<T>();
    CRD_ASSERT(!id.is_null() && "Query::skip_pending<T>: T must be a registered component type");
    const AsyncAwareIndex* index = m_world->template find_index<AsyncAwareIndex>();
    // If no AsyncAwareIndex is registered (T wasn't tagged with AsyncAware{}),
    // the predicate falls back to "pass everything" (the index's null check
    // in run_query_pipeline). That's the documented v1i contract: the
    // operator works as a query no-op on non-async components, matching the
    // ADR-0053 §2 reserved-slot grammar.
    m_skip_pending_filters.push_back(SkipPendingFilter{index, id});
    invalidate_cache();
    return *this;
}

template <typename... Cs> template <typename T> Query<Cs...> Query<Cs...>::skip_pending() &&
{
    this->template skip_pending<T>();
    return std::move(*this);
}

// ---- v1p: Reserved spatial DSL operators (ADR-0053 §6) -------------------
//
// `.in_aabb(box)` and `.within_radius(center, radius)` are formally frozen
// as Query<>'s spatial filters in Phase 3.0 v1p. The backing
// SpatialBVHIndex is a no-op shell — the operators currently pass every
// entity matching the required components (no per-entity bounds test).
// When Phase 3.5 ships the real BVH, the operators start filtering
// without any caller code change. The argument types
// (`crd::geometry::primitives::AABB3<f32>` / `Vec3<f32>` + `f32`) are FROZEN: a
// different bounding shape ships as a new operator, never as a signature change.
// (The AABB3 type moved out of crd-math into crd-geometry-primitives in Phase
// 3.1.7 v0a, ADR-0076 §13 — namespace change only, not a signature change.)

template <typename... Cs>
Query<Cs...>& Query<Cs...>::in_aabb(const crd::geometry::primitives::AABB3<crd::f32>& /*box*/) &
{
    // v1p reservation: SpatialBVHIndex is a no-op stub; passing through
    // every match is the contracted behaviour. No per-entity filter is
    // appended. The chain still composes (range-for + count() etc. all
    // work) so caller code written today continues to compile + iterate
    // unchanged once Phase 3.5 wires the BVH.
    return *this;
}

template <typename... Cs> Query<Cs...> Query<Cs...>::in_aabb(const crd::geometry::primitives::AABB3<crd::f32>& box) &&
{
    this->in_aabb(box);
    return std::move(*this);
}

template <typename... Cs>
Query<Cs...>& Query<Cs...>::within_radius(const crd::math::Vec3<crd::f32>& /*center*/, crd::f32 /*radius*/) &
{
    // Same passthrough contract as in_aabb — see comment above.
    return *this;
}

template <typename... Cs>
Query<Cs...> Query<Cs...>::within_radius(const crd::math::Vec3<crd::f32>& center, crd::f32 radius) &&
{
    this->within_radius(center, radius);
    return std::move(*this);
}

template <typename... Cs> void Query<Cs...>::for_each_chunk(ChunkVisitor fn, void* user_data)
{
    drive_filtered_chunks(fn, user_data);
}

template <typename... Cs> void Query<Cs...>::drive_filtered_chunks(ChunkVisitor fn, void* user_data)
{
    run_query_pipeline(*m_world, m_required, m_forbidden, m_relations.data(), static_cast<crd::u32>(m_relations.size()),
                       m_change_filters.data(), static_cast<crd::u32>(m_change_filters.size()),
                       m_skip_pending_filters.data(), static_cast<crd::u32>(m_skip_pending_filters.size()),
                       m_predicates.data(), static_cast<crd::u32>(m_predicates.size()), fn, user_data);
}

template <typename... Cs> void Query<Cs...>::materialise()
{
    m_match_cache.clear();
    drive_filtered_chunks(
        [](const ChunkView& view, void* ud)
        {
            auto* arr = static_cast<crd::containers::Array<EntityId>*>(ud);
            for (crd::u32 i = 0; i < view.entity_count; ++i)
            {
                arr->push_back(view.entities[i]);
            }
        },
        &m_match_cache);
    m_materialised = true;
}

template <typename... Cs> typename Query<Cs...>::Iterator Query<Cs...>::begin()
{
    if (!m_materialised)
    {
        materialise();
    }
    return Iterator{this, 0U};
}

template <typename... Cs> typename Query<Cs...>::Iterator Query<Cs...>::end()
{
    if (!m_materialised)
    {
        materialise();
    }
    return Iterator{this, m_match_cache.size()};
}

template <typename... Cs> crd::u32 Query<Cs...>::count()
{
    if (!m_materialised)
    {
        materialise();
    }
    return static_cast<crd::u32>(m_match_cache.size());
}

template <typename... Cs> const crd::containers::Array<EntityId>& Query<Cs...>::matches()
{
    if (!m_materialised)
    {
        materialise();
    }
    return m_match_cache;
}

template <typename... Cs> auto Query<Cs...>::Iterator::operator*() const
{
    EntityId e = m_query->m_match_cache[m_index];
    return std::tuple<EntityId, Cs&...>{e, *m_query->m_world->template get_component_mut<Cs>(e)...};
}

// ---- Commands<T> templated mutations + ComponentSet helpers -------------
//
// These bodies live below World because they call into World's templated
// methods (require_component_id<T>, components(), etc.). The same
// header-bottom pattern that Query<Cs...>'s methods use.

namespace detail
{
template <typename Set> struct ComponentSetMaskHelper;
template <typename... Ts> struct ComponentSetMaskHelper<ComponentSet<Ts...>>
{
    static ComponentMask compute(const ComponentRegistry& reg) noexcept
    {
        ComponentMask m{};
        ((void)((reg.template id_of<Ts>().is_null() ? false : (m.set(reg.template id_of<Ts>()), true))), ...);
        return m;
    }
};
} // namespace detail

template <typename Set> ComponentMask World::component_set_mask() const noexcept
{
    return detail::ComponentSetMaskHelper<Set>::compute(m_components);
}

template <typename T> void Commands::add_component(EntityId e, T value)
{
    const ComponentId id = m_world->components().template id_of<T>();
    CRD_ASSERT(!id.is_null() && "Commands::add_component<T>: T must be registered");
    const ComponentInfo* info = m_world->components().info(id);
    Command cmd{};
    cmd.kind = CommandKind::AddComponent;
    cmd.component = id;
    cmd.entity = e;
    enqueue_with_payload(cmd, &value, info);
}

template <typename T> void Commands::remove_component(EntityId e)
{
    const ComponentId id = m_world->components().template id_of<T>();
    if (id.is_null())
    {
        return; // un-registered T → noop, matches World::remove_component
    }
    Command cmd{};
    cmd.kind = CommandKind::RemoveComponent;
    cmd.component = id;
    cmd.entity = e;
    enqueue(cmd);
}

template <typename T> void Commands::set_component(EntityId e, T value)
{
    // UPSERT semantics — same as add_component for v1h (storage already
    // upserts internally). Kept as a separate API name for caller intent.
    add_component<T>(e, std::move(value));
}

template <typename Tag> void Commands::add_relation(EntityId src, EntityId target)
{
    const ComponentId id = m_world->components().template id_of<Relation<Tag>>();
    CRD_ASSERT(!id.is_null() && "Commands::add_relation<Tag>: Relation<Tag> must be registered");
    Command cmd{};
    cmd.kind = CommandKind::AddRelation;
    cmd.component = id;
    cmd.entity = src;
    cmd.relation_target = target;
    enqueue(cmd);
}

template <typename Tag> void Commands::remove_relation(EntityId src)
{
    const ComponentId id = m_world->components().template id_of<Relation<Tag>>();
    if (id.is_null())
    {
        return;
    }
    Command cmd{};
    cmd.kind = CommandKind::RemoveRelation;
    cmd.component = id;
    cmd.entity = src;
    enqueue(cmd);
}

// ---- v1i: Auto-registration of reserved-slot indexes ---------------------
//
// register_component<T>(traits...) ends by calling this hook with the
// freshly-stamped ComponentInfo. Each trait flag triggers a lazy
// instantiation of the corresponding global index (one per kind), and
// adds the new ComponentId to that index's observed mask.
//
// ChangeDetectIndex auto-registers on EVERY component registration
// (every component is "watchable"). The other reserved indexes register
// only when their trait flag is set.

inline void World::auto_register_indexes_for(ComponentId id)
{
    const ComponentInfo* info = m_components.info(id);
    if (info == nullptr)
    {
        return;
    }

    // Helper lambda — finds an index by exact dynamic type or lazy-creates it.
    auto ensure_and_watch = [&]<typename Idx>(ComponentId watched)
    {
        Idx* existing = nullptr;
        for (auto& slot : m_indexes)
        {
            if (Idx* casted = dynamic_cast<Idx*>(slot.get()); casted != nullptr)
            {
                existing = casted;
                break;
            }
        }
        if (existing == nullptr)
        {
            // Indexes that need an allocator (ChangeDetect, AsyncAware)
            // construct with the World's allocator; the no-op shells take
            // no ctor args.
            if constexpr (requires { Idx{allocator()}; })
            {
                existing = register_index<Idx>(allocator());
            }
            else
            {
                existing = register_index<Idx>();
            }
        }
        existing->watch(watched);
    };

    // ChangeDetect — every component is observable.
    ensure_and_watch.template operator()<ChangeDetectIndex>(id);

    if (info->async_aware)
    {
        ensure_and_watch.template operator()<AsyncAwareIndex>(id);
    }
    if (info->history_window > 0)
    {
        ensure_and_watch.template operator()<HistoryIndex>(id);
    }
    if (info->spatial_bvh)
    {
        ensure_and_watch.template operator()<SpatialBVHIndex>(id);
    }
    if (info->gpu_resident)
    {
        ensure_and_watch.template operator()<GpuResidentIndex>(id);
    }
    if (info->replication != Replication::Local)
    {
        ensure_and_watch.template operator()<ReplicationIndex>(id);
    }
    if (info->reflection.fields != nullptr)
    {
        ensure_and_watch.template operator()<ReflectionIndex>(id);
    }
}

// ---- v1i: Query DSL operators that consume indexes -----------------------

namespace detail
{
struct ChangedSinceCtx
{
    const ChangeDetectIndex* index;
    ComponentId component;
    crd::u32 since_frame;
};

inline bool changed_since_predicate(EntityId e, const World*, void* ud)
{
    const auto* ctx = static_cast<const ChangedSinceCtx*>(ud);
    return ctx->index != nullptr && ctx->index->changed_since(e, ctx->component, ctx->since_frame);
}

struct SkipPendingCtx
{
    const AsyncAwareIndex* index;
    ComponentId component;
};

inline bool skip_pending_predicate(EntityId e, const World*, void* ud)
{
    const auto* ctx = static_cast<const SkipPendingCtx*>(ud);
    if (ctx->index == nullptr)
    {
        return true; // no index registered → can't filter; pass everything
    }
    return !ctx->index->is_pending(e, ctx->component);
}
} // namespace detail

} // namespace crd::scene
