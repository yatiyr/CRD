# ADR-0053 — Scene/ECS L5: Component index slot framework

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, arch, layer-5, indexes, extensibility

---

## Context

Layer 5 is **the slot system that makes the architecture extensible**. Every novel ECS extension (change detection, time-tunneling, spatial queries, GPU-residency, async-awareness, networking replication) takes the same shape: an observer that subscribes to component lifecycle events, maintains an auxiliary structure, and exposes new filter operators on the query DSL.

Most ECS implementations bolt these on as separate subsystems with bespoke wiring. We unify them: every "novel ECS extension" implements `IComponentIndex` and registers with the World; the query DSL (Layer 4) discovers the index and exposes its operators automatically.

This is the **Cerid signature** — the architectural contribution that distinguishes this ECS. Other engines have indexes; we have an index *framework* that makes adding new ones a one-day job.

This ADR locks Layer 5.

---

## Decision

### 1. `IComponentIndex` interface

```cpp
class IComponentIndex
{
public:
    virtual ~IComponentIndex() = default;

    // Lifecycle: which components does this index observe?
    [[nodiscard]] virtual ComponentMask observed() const = 0;

    // Component lifecycle events (driven by Storage layer)
    virtual void on_insert(EntityId, ComponentId, const void* data) = 0;
    virtual void on_update(EntityId, ComponentId, const void* old, const void* new_) = 0;
    virtual void on_remove(EntityId, ComponentId) = 0;
    virtual void on_entity_destroyed(EntityId) = 0;

    // Frame lifecycle (called by World::step)
    virtual void on_frame_begin(crd::u32 frame_idx) {}
    virtual void on_frame_end  (crd::u32 frame_idx) {}

    // Diagnostics
    [[nodiscard]] virtual crd::containers::StringView name() const = 0;
};
```

Indexes are registered with the World and routed events automatically. Storage layer (ADR-0050) does not know about specific indexes — it dispatches `on_insert` / `on_update` / etc. to a generic list.

### 2. Five indexes — three ship now, two reserve API

| Index | Phase 3.0 | Status |
|---|---|---|
| `ChangeDetectIndex` | ✅ ship | Implemented v1i |
| `AsyncAwareIndex` | ✅ ship | Implemented v1i |
| `HistoryIndex<N>` | API frozen, no impl | Implemented Phase 3.2 (animation interp) |
| `SpatialBVHIndex` | API frozen, no impl | Implemented Phase 3.5 (frustum culling) |
| `GpuResidentIndex` | API frozen, no impl | Implemented Phase 3.4 (GPU-driven render) |

**Each index, including the deferred ones, is registered at component-registration time.** Registering for a deferred index in Phase 3.0 silently no-ops at runtime; when the index is implemented in its consumer phase, the runtime starts honoring the registration without caller code changes.

```cpp
// Day-one registration with traits — all slots accept this grammar.
world.register_component<Transform>(
    StorageHint::Archetype,
    History{60},               // reserve slot — impl in Phase 3.2
    SpatialBVH{},              // reserve slot — impl in Phase 3.5
    GpuResident{},             // reserve slot — impl in Phase 3.4
    AsyncAware{}               // shipping in 3.0
);

world.register_component<Renderable>(
    StorageHint::Archetype,
    AsyncAware{},              // shipping
    GpuResident{}              // reserve slot
);
```

The ADR's job is to lock the *registration grammar* — every trait above must be accepted by `register_component` from day one, even when the runtime treats it as a no-op. Adding a trait later is a breaking change to user code; reserving them now is free.

### 3. `ChangeDetectIndex` — shipping in Phase 3.0 v1i

Storage already maintains a per-chunk version counter per component (ADR-0050 §2). ChangeDetect leverages this:

```cpp
// Per-chunk: the version counter is bumped whenever any entity's component
// in this chunk is written. The chunk's "last seen by system X" version is
// stored per-(system, chunk) pair in a small hash map.

auto q = world.query<Transform>().changed<Transform>();
// Emits only entities in chunks where component_version > last_seen_version[sys, chunk].
```

False positives at chunk granularity (200 entities ≈ a chunk) are accepted — the consumer skips already-handled entities cheaply and the chunk-grain reporting cuts 99%+ of redundant work. Per-entity precision is reserved as a future opt-in (`PreciseChangeDetect{}`) for the rare case where chunk-grain false positives are unacceptable.

### 4. `AsyncAwareIndex` — shipping in Phase 3.0 v1i

Components flagged with `AsyncAware{}` may carry `LoadState` (we already have this enum from ADR-0039). The index maintains a per-component bitset of "is this entity's `T` still loading?". Queries gain `.skip_pending<T>()` which filters those entities out automatically:

```cpp
auto q = world.query<Transform, Renderable>().skip_pending<Renderable>();
// Renderer iterates only entities whose Renderable is fully resident
```

This eliminates the manual "is the mesh ready yet?" checks from every consumer. It's also what closes the open question in `docs/debt.md`'s async GPU upload entry — once `GpuResidentIndex` ships in Phase 3.4, `skip_pending<Renderable>` will also gate on GPU upload state, not just CPU load state.

### 5. `HistoryIndex<N>` — API frozen, impl deferred to Phase 3.2

Per-component ring buffer of N frames of history.

```cpp
// Registration
world.register_component<Transform>(StorageHint::Archetype, History{60});

// Query (impl deferred — returns current frame in v1)
const Transform& current = world.view<Transform>().at(entity);
const Transform& prev    = world.view<Transform>().at(entity, -1);   // 1 frame ago
const Transform& old     = world.view<Transform>().at(entity, -10);  // 10 frames ago

// World rewind for rollback (impl deferred)
world.rewind(-3);
world.step(corrected_input);
```

Storage cost per component instance: `N * sizeof(T)` extra bytes. Default `N = 0` (no history; identical perf to non-history components).

This is Cerid's killer differentiator (see ADR rationale). Networking rollback (Phase 4.2), animation interpolation (Phase 3.2), simulation debug-bisection (Phase 8), editor time-scrub (Phase 7) all consume this directly without any of them shipping their own history machinery.

### 6. `SpatialBVHIndex` — API frozen, impl deferred to Phase 3.5

```cpp
// Registration
world.register_component<Transform>(StorageHint::Archetype, SpatialBVH{});
world.register_component<Bounds>   (StorageHint::Archetype, SpatialBVH{});

// Query (impl deferred — returns full set in v1)
auto in_view = world.query<Renderable>()
                   .in_aabb(camera_frustum.aabb());

auto nearby  = world.query<Transform>()
                   .within_radius(player_pos, 50.0f);
```

Implements a BVH (or grid; choice locked in Phase 3.5 ADR) over the registered component's spatial extents. Frustum culling, range queries, broadphase contact tests all consume this — eliminates the "every system maintains its own spatial index" anti-pattern.

### 7. `GpuResidentIndex` — API frozen, impl deferred to Phase 3.4

```cpp
// Registration
world.register_component<Transform>(StorageHint::Archetype, GpuResident{});
```

The index maintains a CPU-mirrored GPU SSBO of the component data. Render extract becomes a single barrier (no per-entity copy) — matches GPU-driven rendering's staging needs (Phase 3.4+).

This is what enables million-entity rendering at 60 Hz. The CPU iteration path stays the same; the GPU upload path becomes O(1) chunks-changed instead of O(N) entities.

### 8. Wiring: indexes register, storage dispatches

```cpp
class World
{
    crd::containers::Array<std::unique_ptr<IComponentIndex>> m_indexes;

    template <typename Idx>
    void register_index(std::unique_ptr<Idx> idx);

    // Driven by Storage layer:
    void notify_insert(EntityId e, ComponentId c, const void* data)
    {
        for (auto& idx : m_indexes)
            if (idx->observed().has(c))
                idx->on_insert(e, c, data);
    }
    // ... similarly for update, remove, destroy
};
```

The dispatch loop is hot-path — it runs on every component insert/update/remove. Two optimisations:

- The observed-mask check is a 64-bit bitwise AND — faster than function-call overhead.
- Indexes that observe nothing in a given chunk's archetype are pre-filtered (their `observed()` mask is computed once at registration, intersected with archetype masks once).

For change detection specifically, the chunk-version counter is bumped inside the storage write path *before* the index dispatch — most index work is just "remember which chunks changed." Cheap.

---

## Rationale

### Why a generic index framework, not bespoke wiring per feature

Each of the five indexes could be wired manually:

- ChangeDetect → custom version counters in storage + custom query operator
- AsyncAware → custom bitset + custom filter
- History → custom ring buffer + custom query method
- SpatialBVH → custom BVH + custom query method
- GpuResident → custom SSBO + custom render extract

Five bespoke implementations: ~3000 LOC of glue, each one slightly different. A registration framework at ~250 LOC unifies all five and makes adding a sixth (`MetricsIndex`, `EditorSelectionIndex`, `NetworkPriorityIndex`) trivially cheap.

The cost is one observer-pattern indirection. At million-entity scale, the dispatch is per-chunk-write (not per-entity), so the cost is in the noise.

### Why ship ChangeDetect and AsyncAware now

Both are nearly-free given existing machinery. ChangeDetect uses the chunk version counters that storage maintains for free. AsyncAware uses the LoadState we already have (ADR-0039). Their consumers exist today (transform propagation only walks `changed<Transform>`; renderer skips entities whose mesh is still streaming). Shipping them now both validates the framework and removes existing debt (the open issue in `docs/debt.md` § "Async GPU upload" partly resolves once AsyncAware lands; fully resolves when GpuResident lands in 3.4).

### Why reserve API for the deferred indexes

If the registration grammar accepts `History{60}` from day one, components written today can later opt into history without changes to caller code. If we add `History{60}` later, every existing component registration is a breaking change.

The cost of accepting the trait now and silently ignoring it is one parameter parse + storing a `crd::u8 history_window` per component type. ~16 bytes total in the registration table.

---

## Consequences

- `IComponentIndex` is the public extension point for all "novel ECS extensions." New features (e.g. metrics, editor-selection tracking, sound occlusion) are written as Indexes — no patches to core `crd-scene`.
- The five named indexes (ChangeDetect, AsyncAware, History, SpatialBVH, GpuResident) all have stable identities, registration grammar, and query DSL operators by end of Phase 3.0. Three implementations land later.
- The query DSL (ADR-0052) is index-aware: `.changed<T>()`, `.skip_pending<T>()`, `.in_aabb()`, `.within_radius()`, `.at(frame)` are all built-in operators. The trait registration determines whether the operator does real work or no-ops.
- Index dispatch overhead is per-chunk-write, not per-entity-write. At realistic workloads (transforms updated once per frame for 1 M entities, ~5000 chunks) the dispatch cost is ≈5000 virtual calls per frame ≈ 25 µs. Invisible.
- Memory cost per registered (component × trait) is bounded: ChangeDetect uses storage's existing version counter (free); AsyncAware uses 1 bit per (entity, component) ≈ 125 KB at 1 M entities; History uses `N * sizeof(T)` extra per instance; SpatialBVH and GpuResident pay their own well-bounded costs.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0035 — Networking architecture (Replication will be a sixth Index)
- ADR-0039 — ResourceHandle semantics (LoadState consumed by AsyncAware)
- ADR-0050 — Storage backends (chunk version counters drive ChangeDetect)
- ADR-0052 — Query · System · Schedule (DSL exposes index operators)
- ADR-0056 — Reserved L6–L8 slots (Replication ships as an Index)
