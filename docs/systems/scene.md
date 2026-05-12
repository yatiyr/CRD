# crd-scene

The Scene / ECS foundation. Entities, components, relations, queries, systems, and the index slot framework that makes the architecture extensible. Every Phase 3.x onwards consumes it.

**Phase 3.0 active.** v1a–v1m all shipped 2026-05-06 / 07 / 08. **v1m (Öbek system) FULLY DELIVERED 2026-05-08** across 12 sub-slices (~2700 LOC, 58 öbek tests; ADR-0058 fully realised). Phase 3.0 expanded from 14 to 17 slices on 2026-05-08 to land the elite-tier authoring substrate (Öbek + Preset + Profile). 3 slices remaining: v1n (Preset + Profile, ADRs 0059/0060), v1o (sandbox integration), v1p (reserved-slot + API freeze).

Depends on `crd-core` + `crd-containers` + `crd-memory`. Links `crd-resources` (SceneLoader / ObekLoader); `crd-cooker` extended with SceneCooker + ObekCooker. Future slices link `crd-jobs` (parallel queries) and feed `crd-renderer` (extract via query). ADR-0020 cornerstone; ADRs 0049–0057 lock the eight layers; ADRs 0058 (Öbek) / 0059 (Preset) / 0060 (Profile) lock the authoring substrate.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | `EntityId` + `SlotMap` + `World` shell | ✅ shipped 2026-05-06 |
| v1b | `ComponentRegistry` + `IStorageBackend` + storage-hint registration grammar | ✅ shipped 2026-05-07 |
| v1c1 | Chunk allocator + SoA layout + per-chunk version-counter array | ✅ shipped 2026-05-07 |
| v1c2 | Archetype + ArchetypeGraph + ArchetypeChunkStorage + IStorageEventSink + typed World API | ✅ shipped 2026-05-07 |
| v1d | `SparseSetStorage` (escape hatch for high-churn / sparse / lookup-dominated components) | ✅ shipped 2026-05-07 |
| v1e | Mixed-backend chunk visitor (queries walk Archetype + SparseSet uniformly) | ✅ shipped 2026-05-07 |
| v1f | Relations + 6 built-ins (ChildOf / AttachedTo / Owns / Targets / DependsOn / PossessedBy) + ReverseIndex / Acyclic / OnTargetDestroyed traits | ✅ shipped 2026-05-07 |
| v1g | Query DSL (`world.query<Cs...>().with<>().without<>().with_relation<>().filter()`, range-for, chunk visitor) | ✅ shipped 2026-05-07 |
| v1h | System + Schedule + Commands (`ISystem`, 7-phase fixed schedule, `step` / `step_fixed`, deferred-mutation `Commands` flushed at phase boundaries) | ✅ shipped 2026-05-07 |
| v1i | Index framework + `ChangeDetectIndex` + `AsyncAwareIndex` + 5 reserved no-op shells (History / SpatialBVH / GpuResident / Replication / Reflection); `.changed<T>()` / `.skip_pending<T>()` operators | ✅ shipped 2026-05-07 |
| v1j | `Transform` (TRS + cached world) + `TransformPropagation` (PreRender phase) + 6 rotation-set APIs + `set_world` / `try_set_world` + cross-domain robust + bit-exact deterministic | ✅ shipped 2026-05-07 |
| v1k | `SceneResource` + `SceneLoader` (FourCC `'SCEN'`) + `SceneArtifactBuilder` + `World::instantiate_scene` + 6 built-in relations get serialize traits | ✅ shipped 2026-05-07 |
| v1l | `cook_scene` cooker handler (`.scene.toml` → SCEN CRDR; `crd-cooker` SceneCooker; cooker-side TransformPropagation bakes world matrices) | ✅ shipped 2026-05-08 |
| v1m | **Öbek system** — full ADR-0058 surface (12 sub-slices: substrate / overrides + OCHN / full ObekCooker / InheritPolicy + DontInherit / Inherit transparent CoW backend / revert + unpack + AAAA reservations) | ✅ shipped 2026-05-08 |
| v1n | **Preset + Profile system** — `PresetResource` + per-type `PresetLoader` + `QualityPreset` (`'PRQL'`) + `CameraPreset` (`'PRCM'`); `ProfileResolver` with closed predicates + additive composition; ADRs 0059 / 0060 | ⏳ next |
| v1o | **Sandbox renderer integration with Öbek + Preset + Profile** — sandbox loads `.scene.toml` referencing öbeks; profile auto-resolves at boot; ImGui live override panel | ⏳ |
| v1p | **Reserved-slot freeze** — registration grammar test for L6/L7/L8 reserved indexes; öbek/preset/profile API surface frozen; closes Phase 3.0 | ⏳ |

## The eight-layer architecture

```
L8 │ Reflection / Editor       (Phase 7)             │ ADR-0056
L7 │ Scripting & Behaviors     (Phase 4.0+)          │ ADR-0056
L6 │ Replication / Networking  (Phase 4.2)           │ ADR-0056
L5 │ Indexes                   (Phase 3.0+ as needed)│ ADR-0053
   │   ChangeDetect · AsyncAware  (impl 3.0 v1i)
   │   History · SpatialBVH · GpuResident (API only) │
L4 │ Query · System · Schedule (Phase 3.0)           │ ADR-0052
L3 │ Relations                 (Phase 3.0)           │ ADR-0051
L2 │ Storage backends          (Phase 3.0)           │ ADR-0050
   │   ArchetypeChunk (primary) · SparseSet (escape) │
L1 │ Entity / SlotMap          (Phase 3.0)           │ ADR-0049
L0 │ Memory · Containers · Jobs (already shipped)    │
```

**The Cerid signature** lives at L5: a uniform `IComponentIndex` extension framework where every novel ECS extension (history, change detect, spatial, GPU-mirror, async, replication, scripts, reflection) is a registered slot consuming the same component-lifecycle event stream. Adding the next extension is a one-day job.

## Core decisions

- **Hybrid storage, not pure ECS, not naive scene graph.** ArchetypeChunk + SparseSet behind a single `IStorageBackend` interface; per-component-type hint at registration. Two backends because pure-archetype loses on high-churn / sparse components, and pure-sparse-set loses 5–10× on million-entity multi-component iteration. (ADR-0050)
- **64-bit generational `EntityId` with `[gen:32 | idx:32]` layout.** 4 G slots × 4 G generations per slot. 16/16 was rejected (caps at 64 K live entities — inadequate for agent simulations); 8/24 was rejected (256 reuses per slot is a footgun under steady recycling). (ADR-0049)
- **Slot 0 reserved permanently as the null sentinel.** `EntityId{}` default-initialises to `null()`; `is_alive(EntityId{})` is `false` by construction. No special-case branch.
- **Deferred destroy with synchronous escape hatch.** `world.destroy(e)` queues for end-of-frame; `flush_destroys()` drains. Required for safe parallel iteration (`par_each` ships v1g). `world.destroy_immediate(e)` is the synchronous escape, lenient on stale handles so double-destroy is safe.
- **Relations are first class** — not synthesized from `Parent` components. `Relation<Tag>` template + reverse-index trait + cycle-detection in debug + `OnTargetDestroyed` policy. Built-in `ChildOf`, `AttachedTo`. (ADR-0051)
- **Phase-ordered scheduler with command-buffer mutation.** Systems declare `Reads<>` / `Writes<>` and a `SchedulePhase`. Mutation during iteration goes through a per-fiber command buffer flushed at phase boundary. Auto-parallel within a phase is API-reserved, impl deferred. (ADR-0052)
- **Authoring TOML, runtime SCEN CRDR.** Closes ADR-0020's "FlatBuffers vs Cap'n Proto" deferral with "neither — a CRDR `SCEN` artifact". Reuses CRDR's chunking / zstd / manifest / hot-reload / mounting machinery. (ADR-0055)
- **UI in the scene tree.** `ControlNodeTag` reserved at L3 boundary; UI components live in `crd-ui` (Phase 5) and never leak engine concerns. (ADR-0057)

## Public API (v1a + v1b — what's there now)

```cpp
namespace crd::scene {

// 64-bit generational handle. 8 bytes, trivially copyable, default-zero is null().
struct EntityId
{
    crd::u64 raw = 0;
    [[nodiscard]] constexpr crd::u32 index()      const noexcept;
    [[nodiscard]] constexpr crd::u32 generation() const noexcept;
    [[nodiscard]] constexpr bool     is_null()    const noexcept;
    [[nodiscard]] static constexpr EntityId null() noexcept;
    [[nodiscard]] static constexpr EntityId make(crd::u32 idx, crd::u32 gen) noexcept;
    [[nodiscard]] constexpr bool operator==(const EntityId&) const noexcept = default;
};

// Dense slot table with free-list reuse and slot-0 sentinel. Owned by World.
class SlotMap
{
public:
    explicit SlotMap(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    [[nodiscard]] EntityId  allocate();        // O(1); never returns index 0
    void                    free(EntityId);    // O(1); precondition is_alive(e)
    [[nodiscard]] bool      is_alive(EntityId) const noexcept;
    [[nodiscard]] crd::u32  alive_count() const noexcept;
    [[nodiscard]] crd::u32  slot_count()  const noexcept;

    class Iterator;
    [[nodiscard]] Iterator begin() const noexcept;  // skips holes; yields alive entities only
    [[nodiscard]] Iterator end()   const noexcept;
};

// 16-bit per-World component identifier. Default-constructed is null (raw == 0xFFFF).
struct ComponentId { crd::u16 raw = 0xFFFF; /* is_null, == */ };

// 256-bit fixed bitset over ComponentIds. Single cache line.
struct ComponentMask { std::array<crd::u64, 4> bits; /* set, test, clear, &, |, popcount */ };

// Trait grammar. Markers are empty / value structs; reserved records carry callbacks.
enum class StorageHint : crd::u8 { Archetype = 0, SparseSet };
enum class Replication : crd::u8 { Local, ServerAuthoritative, ClientPredicted, Remote };
struct AsyncAware {};                    // ships in v1i
struct SpatialBVH {};                    // impl Phase 3.5
struct GpuResident {};                   // impl Phase 3.8
struct History     { crd::u8 window; }; // impl Phase 3.2
struct ComponentSerialize { /* fourcc, version, 4 callbacks */ };  // wired v1k–v1l
struct Reflection         { /* display_name, fields */ };          // wired Phase 7

// Per-registration metadata. Lifecycle ops captured via if-constexpr; any may be nullptr.
struct ComponentInfo
{
    ComponentId id;
    crd::containers::StringView name;          // typeid name; static storage
    crd::usize size, alignment;
    StorageHint storage_hint;
    bool async_aware, spatial_bvh, gpu_resident;
    crd::u8 history_window;
    Replication replication;
    ComponentSerialize serialize;
    Reflection reflection;
    DefaultCtorFn default_construct;
    DtorFn        destruct;
    MoveCtorFn    move_construct;
};

// Component metadata table. Owned by World. Idempotent re-registration.
class ComponentRegistry
{
public:
    template <typename T, typename... Traits>
    ComponentId register_type(Traits&&... traits);

    [[nodiscard]] const ComponentInfo* info(ComponentId) const noexcept;
    template <typename T> [[nodiscard]] ComponentId id_of() const noexcept;
    [[nodiscard]] crd::u16 size() const noexcept;
};

// Storage backend interface. Declared only — Archetype impl in v1c, SparseSet in v1d.
class IStorageBackend
{
public:
    virtual ~IStorageBackend() = default;
    virtual void  insert(EntityId, ComponentId, const void* data) = 0;
    virtual void  remove(EntityId, ComponentId)                   = 0;
    [[nodiscard]] virtual bool has(EntityId, ComponentId) const   = 0;
    [[nodiscard]] virtual void* get_mut(EntityId, ComponentId)    = 0;
    virtual void for_each_chunk(ComponentMask required, ChunkVisitor, void* user_data) = 0;
    virtual void on_entity_destroyed(EntityId) = 0;
};

// Phase 3.0 World shell. Grows in v1c–v1n with storage bindings, relations,
// queries, systems, and indexes.
class World
{
public:
    explicit World(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    // Entity lifecycle (v1a)
    [[nodiscard]] EntityId spawn();
    void                   destroy(EntityId);            // deferred (queued)
    void                   destroy_immediate(EntityId);  // synchronous; lenient on stale
    void                   flush_destroys();             // drain queue end-of-frame

    [[nodiscard]] bool     is_alive(EntityId) const noexcept;
    [[nodiscard]] crd::u32 entity_count() const noexcept;
    [[nodiscard]] crd::u32 pending_destroy_count() const noexcept;

    [[nodiscard]] SlotMap::Iterator begin() const noexcept;
    [[nodiscard]] SlotMap::Iterator end()   const noexcept;

    // Component registry (v1b)
    template <typename T, typename... Traits>
    ComponentId register_component(Traits&&... traits);

    [[nodiscard]] const ComponentInfo* component_info(ComponentId) const noexcept;
    template <typename T> [[nodiscard]] ComponentId component_id() const noexcept;
    [[nodiscard]] crd::u16 registered_component_count() const noexcept;
    [[nodiscard]] const ComponentRegistry& components() const noexcept;
};

} // namespace crd::scene
```

## How to use it (v1a)

```cpp
crd::scene::World world;

// Spawn a few entities.
auto a = world.spawn();
auto b = world.spawn();
auto c = world.spawn();
// world.entity_count() == 3

// Destroy is deferred — `b` is still alive until end-of-frame.
world.destroy(b);
// world.is_alive(b) == true  (still alive)
// world.pending_destroy_count() == 1

world.flush_destroys();
// world.is_alive(b) == false
// world.entity_count() == 2

// Range-for skips holes and yields surviving entities only.
for (crd::scene::EntityId e : world)
{
    // do something with `a` and `c`
}

// Synchronous escape hatch for tests / scratch entities / asset reload paths.
world.destroy_immediate(a);
// `a` is gone immediately; safe to call again — leniently no-ops on stale.

// ---- v1b: register component types with traits ----------------------------

struct Transform { /* ... */ };
struct Velocity  { /* ... */ };
struct DialogTrigger { int dialog_id; };

// Default StorageHint::Archetype, no traits.
crd::scene::ComponentId pos_id = world.register_component<Transform>();

// Multiple traits in one call. Order is free; later traits override earlier
// ones on overlapping fields.
crd::scene::ComponentId vel_id = world.register_component<Velocity>(
    crd::scene::StorageHint::Archetype,
    crd::scene::AsyncAware{},
    crd::scene::History{8},
    crd::scene::Replication::ServerAuthoritative);

// Sparse / lookup-dominated component opts into SparseSet storage.
crd::scene::ComponentId trig_id = world.register_component<DialogTrigger>(
    crd::scene::StorageHint::SparseSet);

// Idempotent — second call returns the same id, ignores second-call traits.
auto same = world.register_component<Transform>();
assert(same == pos_id);

// Lookup
const auto* info = world.component_info(vel_id);
// info->size, info->alignment, info->history_window == 8, etc.

// ---- v1c2: the entity actually owns components --------------------------

EntityId entity = world.spawn();

// Place data on the entity. Creates the {Transform} archetype on first call;
// on subsequent calls with different component types, the entity moves
// archetype (e.g. {Transform} → {Transform, Velocity}) — bytewise.
world.add_component<Transform>(entity, Transform{ ... });
world.add_component<Velocity>(entity, Velocity{ ... });

// Read access — does NOT bump change-detection version counter.
const Transform* t = world.get_component<Transform>(entity);

// Mutable access — bumps the chunk's version counter for Transform
// (chunk-grain change detection — ADR-0053 v1i ChangeDetectIndex consumes this).
Transform* mt = world.get_component_mut<Transform>(entity);
mt->translation += Vec3f{1, 0, 0};

// Membership test (O(1) — archetype mask test).
bool has_vel = world.has_component<Velocity>(entity);

// Remove → entity moves to (mask & ~Velocity) archetype.
world.remove_component<Velocity>(entity);

// Bulk iteration (the foundation v1g's query DSL builds on). Walks every
// archetype whose mask is a SUPERSET of `required` and visits each chunk.
crd::scene::ComponentMask required;
required.set(world.component_id<Transform>());

world.storage().for_each_chunk(required,
    [](const crd::scene::ChunkView& chunk, void* /*user*/) {
        // chunk.entities, chunk.entity_count, chunk.present_mask
        // SoA per-component pointers come via the chunk + archetype layout
        // (v1g's query DSL packages this into a typed accessor).
    },
    /*user_data=*/nullptr);

// Storage event hook — every mutation fires through here. v1i wires this to
// the IComponentIndex fan-out; v1c2 ships the call sites with a no-op default
// so future indexes plug in without changing storage code or user code.
struct MyIndex : crd::scene::IStorageEventSink {
    void on_insert(EntityId, ComponentId, const void*) override { /* ... */ }
    void on_update(EntityId, ComponentId, const void*, const void*) override { /* ... */ }
    void on_remove(EntityId, ComponentId, const void*) override { /* ... */ }
    void on_entity_destroyed(EntityId) override { /* ... */ }
};
MyIndex idx;
world.set_storage_event_sink(&idx);
```

The post-v1c2 slices grow `World` with: a SparseSet escape-hatch backend (v1d), the mixed-backend chunk visitor (v1e), first-class relations (v1f), a query DSL (v1g), a phase-ordered scheduler (v1h), the index framework + ChangeDetect + AsyncAware (v1i), Transform + propagation (v1j), scene serialization (v1k–v1l), sandbox renderer integration (v1m), and the reserved-slot freeze (v1n). See the slice table above and `docs/phases/phase-3.0-scene-ecs.md`.

## Architectural notes for future slices

- **`Slot` is 12 bytes with default padding** (u32 + u32 + bool); ADR-0049 commentary aspires to 8 but the literal struct is 12. At 1 M entities, 12 MB of slot metadata — well within budget.
- **`flush_destroys()` does not yet dispatch `on_entity_destroyed`** — Layer 2 storage and Layer 5 indexes don't exist. The hook lands when the first observer arrives in v1c / v1i.
- **No `DefaultHash<EntityId>` specialization yet** — first need is in v1c (storage backends use entities as hash-map keys); will land alongside.

## Long-term direction

- v1b–v1n complete the eight-layer machine: components, storage, relations, queries, schedule, indexes, transform, serialization, sandbox integration, and the reserved-slot freeze.
- Phase 3.1 (eylem — Cerid-native physics) registers `RigidBody`, `Collider`, `Joint` components. ECS-native integration via `BodyHandle ↔ EntityId` mapping; `PhysicsTransform` writeback into the existing `Transform` component during `PostPhysics` schedule phase. ADR-0062 / ADR-0063 / `docs/phases/phase-3.1-eylem.md`.
- Phase 3.2 (Animation) ships the `HistoryIndex<N>` implementation for time-tunneled queries (animation interpolation reads `Transform` at `frame - 1`).
- Phase 3.3+ (font, audio, PBR, etc.) each register their own component set; `SpatialBVHIndex` impl ships with Phase 3.5 light culling at scale.
- Phase 3.8 (GPU-driven rendering) ships the `GpuResidentIndex` implementation: components flagged `GpuResident` are mirrored to GPU buffers automatically.
- Phase 4.0 (scripts) and Phase 4.2 (replication) each become a registered L5/L6/L7 slot honoring the same registration grammar — no core changes.
- Phase 5 (`crd-ui`) consumes the `ControlNodeTag` reserved at L3 boundary; UI nodes coexist with spatial nodes in the same scene tree (ADR-0057).
- Phase 7 (editor) consumes the L8 reflection slot for inspector and serializer-by-walking.

## References

- `docs/systems/scene-concurrency.md` — concurrency contracts per storage (what parallel access is legal; the spec the D-002 stress matrix tests against)
- `docs/phases/phase-3.0-scene-ecs.md` — phase plan, 14 slices, definition of done
- `docs/decisions/0020-scene-ecs-hybrid.md` — cornerstone decision (UI in tree, hybrid storage)
- `docs/decisions/0049-scene-entity-identity-slotmap.md` — L1 (this slice)
- `docs/decisions/0050-scene-storage-backends.md` — L2 ArchetypeChunk + SparseSet
- `docs/decisions/0051-scene-relations-first-class.md` — L3 Relations
- `docs/decisions/0052-scene-query-system-schedule.md` — L4 Query / System / Schedule
- `docs/decisions/0053-scene-component-index-framework.md` — L5 Index slot framework
- `docs/decisions/0054-scene-transform-hierarchy-update.md` — Transform layout + propagation
- `docs/decisions/0055-scene-serialization-toml-scen-crdr.md` — TOML authoring → SCEN CRDR
- `docs/decisions/0056-scene-reserved-l6-l8-slots.md` — Replication / Scripts / Reflection slots
- `docs/decisions/0057-scene-ui-in-tree-boundary.md` — UI scope boundary
- `docs/sessions/2026-05-06-scene-v1a-slotmap.md` — v1a session log
