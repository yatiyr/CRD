# Phase 3.0 — Scene / ECS Foundation

**Status:** 🚧 active — v1a + v1b + v1c (whole) + v1d + v1e shipped 2026-05-06 / 07; 9 of 14 slices remaining. Next: v1f (Relations).
**ADRs:** ADR-0049, ADR-0050, ADR-0051, ADR-0052, ADR-0053, ADR-0054, ADR-0055, ADR-0056, ADR-0057
**Cornerstone:** ADR-0020 (scene/ECS hybrid, UI in tree)
**New module:** `crd-scene` (bootstrapped 2026-05-06)
**Depends on:** Phase 2.8 complete (MaterialResource with PSO state + pass-keyed variants)

---

## Goal

Land the foundation `crd-scene` module: a layered, slot-shaped ECS where every novel ECS extension (change detection, time-tunneling, spatial queries, GPU-residency, replication, scripts, reflection) is a slot in the same registration grammar — most filled in Phase 3.0, the rest reserved for their consumer phases.

This is the **highest-leverage single block of work in the engine**. Every Phase 3.x onwards consumes it.

---

## Progress

| Slice | What | State |
|---|---|---|
| v1a | `EntityId` + `SlotMap` + `World` shell | ✅ shipped 2026-05-06 — `docs/sessions/2026-05-06-scene-v1a-slotmap.md` |
| v1b | `ComponentRegistry` + `IStorageBackend` + storage-hint registration grammar | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1b-registry.md` |
| v1c1 | Chunk allocator + SoA layout + per-chunk version-counter array | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1c1-chunk-layout.md` |
| v1c2 | Archetype + ArchetypeGraph + ArchetypeChunkStorage + IStorageEventSink + typed `World::add_component<T>` | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1c2-archetype-storage.md` |
| v1d | `SparseSetStorage` (escape hatch for high-churn / sparse / lookup-dominated) + World dispatch by `StorageHint` | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1d-sparseset.md` |
| v1e | `World::for_each_chunk` mixed-backend visitor (split required by hint, archetype-as-anchor for mixed, smallest-pool-as-anchor for pure-sparse multi-bit) | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1e-mixed-visitor.md` |
| v1f | Relations | ⏳ next |
| v1g–v1n | see slice list below | ⏳ |

`crd-scene` now ships **both L2 backends + a unified iteration primitive that walks across them** per ADR-0050. `World::for_each_chunk(required, fn, ud)` splits `required` by `ComponentInfo::storage_hint`; pure-archetype and pure-SparseSet single-bit forward to the per-backend method (zero overhead); pure-sparse multi-bit uses smallest-pool-as-anchor + sparse-check; mixed uses archetype-as-anchor (cache-coherent default) + sparse-check per entity. Filtered chunks live in stack-local scratch (recursive queries are safe). v1g (query DSL) sits on top of this primitive. 113 unit tests / 34450 assertions; six-config green.

---

## The eight-layer architecture

```
L8 │ Reflection / Editor       (Phase 7)             │ ADR-0056
L7 │ Scripting & Behaviors     (Phase 4.0+)          │ ADR-0056
L6 │ Replication / Networking  (Phase 4.2)           │ ADR-0056
L5 │ Indexes                   (Phase 3.0+ as needed)│ ADR-0053
   │   ChangeDetect · AsyncAware  (impl 3.0)
   │   History · SpatialBVH · GpuResident (API only) │
L4 │ Query · System · Schedule (Phase 3.0)           │ ADR-0052
L3 │ Relations                 (Phase 3.0)           │ ADR-0051
L2 │ Storage backends          (Phase 3.0)           │ ADR-0050
   │   ArchetypeChunk (primary) · SparseSet (escape) │
L1 │ Entity / SlotMap          (Phase 3.0)           │ ADR-0049
L0 │ Memory · Containers · Jobs (already shipped)    │
```

Layer 4's query DSL is the single point of contact between application code and storage. New filter operators (`.changed<>()`, `.skip_pending<>()`, `.in_aabb()`, `.at(frame)`) ride Layer 5's index framework. Layers 6–8 are reserved API slots: `Replication`, `ScriptComponent`, `Reflection` accepted in `register_component` from day one, implementations land in their consumer phases.

The architecture is "extensible from day one" because adding any future ECS extension (metrics, editor selection, sound occlusion, network priority, AI threat) is a one-day task — implement `IComponentIndex`, register, expose a query operator. No core changes.

---

## What ships in Phase 3.0

| Layer | Ships | Status |
|---|---|---|
| L1 Entity / SlotMap | Full impl | v1a ✅ shipped 2026-05-06 |
| L2 ComponentRegistry + `IStorageBackend` interface | Full impl (interface only) | v1b ✅ shipped 2026-05-07 |
| L2 Chunk machinery (ChunkLayout / ChunkHeader / Chunk / ChunkAllocator) | Full impl | v1c1 ✅ shipped 2026-05-07 |
| L2 ArchetypeChunkStorage + ArchetypeGraph + IStorageEventSink + typed World API | Full impl | v1c2 ✅ shipped 2026-05-07 |
| L2 SparseSetStorage | Full impl | v1d |
| L3 Relations | Full impl + ChildOf, AttachedTo | v1f |
| L4 Query DSL | Full impl | v1g |
| L4 System + Schedule | Phase-based v1; auto-parallel reserved | v1h |
| L5 Index framework | Full impl | v1i |
| L5 ChangeDetect | Full impl (chunk-grain) | v1i |
| L5 AsyncAware | Full impl | v1i |
| L5 History\<N\> | API only — `at(frame)` returns current frame | v1n |
| L5 SpatialBVH | API only — filters return full set | v1n |
| L5 GpuResident | API only — no GPU mirror yet | v1n |
| L6 Replication | API only — `Replication::*` accepted at registration | v1n |
| L7 ScriptComponent type | API only — type defined, system is no-op | v1n |
| L8 Reflection | Compile-time hooks reserved | v1n |
| Transform component | Full impl + propagation system | v1j |
| Scene serialization | TOML cooker + SCEN CRDR + SceneLoader | v1k–v1l |
| Renderer integration | Sandbox uses scene query for render extract | v1m |

---

## Slices

14 slices, each individually shippable per the project's Definition of Done (six-config green, unit tests, headless smoke where applicable). Larger slices (v1c, v1g) may further sub-divide during implementation.

### v1a — `EntityId` + `SlotMap` + `World` shell ✅ shipped 2026-05-06

`EntityId` (32:32 gen+idx, 8 bytes, trivially copyable, default-zero is `null()`), `SlotMap` (dense `Array<Slot>` + free-list head, slot 0 reserved permanently as the null sentinel, alive-only iterator that skips holes), `World` shell that owns the slot map and a deferred-destroy queue (`destroy` queues, `flush_destroys` drains end-of-frame, `destroy_immediate` is the synchronous escape hatch and is lenient on stale handles).

Two intentional divergences from ADR-0049 captured in the session log:

- `SlotMap::free` skips generation 0 on overflow rather than trapping (keeps generation 0 reserved as the dead-slot sentinel value).
- `World::destroy_immediate` is lenient on stale handles (no-op rather than assert), so `destroy(e); flush_destroys(); destroy_immediate(e);` is safe.

22 unit tests / 3448 assertions; six-config green (win-debug 503/503, win-release 500/500, win-asan 503/503, win-clang-cl 503/503, win-relwithdebinfo 503/503, win-tidy clean); 17/17 headless smokes per non-tidy config.

**ADR:** 0049
**Session:** `docs/sessions/2026-05-06-scene-v1a-slotmap.md`

### v1b — `ComponentRegistry` + storage-hint registration ✅ shipped 2026-05-07

`World::register_component<T>(traits...)` accepts the variadic trait grammar from ADR-0056 (`StorageHint`, `Replication`, `AsyncAware`, `SpatialBVH`, `GpuResident`, `History{N}`, `ComponentSerialize`, `Reflection`). Trait dispatch is an overload set in `detail::apply_trait` — unknown trait types fail at compile time, so the grammar is closed by the type system.

`ComponentId` is a 16-bit per-World monotonic identifier; the null sentinel is `0xFFFF` (default-constructed). `ComponentMask` is a 256-bit fixed bitset (`std::array<u64, 4>`) — chosen over the ADR-0053 "64-bit AND" reading because v1c's archetype keys would silently truncate to a 64-mask once the 65th component was registered. Four-word AND lives on a single cache line and is indistinguishable from one-word AND at the dispatch granularity (per-chunk-batch, not per-entity).

Type identity uses a per-`T` static tag (`ComponentTypeTag<T>::value` — C++17 inline static, ODR-safe across TUs) keyed by address — no RTTI on the registration hot path. typeid().name() is still used for the debug `ComponentInfo::name` field (StringView, no allocation since typeid name has static storage).

Re-registration is idempotent: registering the same `T` twice returns the existing `ComponentId` and ignores the second call's trait arguments. This lets libraries register defensively without coordination. The ADR's "duplicate-registration rejected" wording is honoured at the level of "no second registration committed" rather than "second call asserts."

`IStorageBackend` interface, `ChunkView`, and `ChunkVisitor` are declared. No backend implementations in v1b — `ArchetypeChunkStorage` lands in v1c, `SparseSetStorage` in v1d. The query layer (v1g) and index dispatcher (v1i) are written against `IStorageBackend`.

22 unit tests / 131 assertions added. Six-config green (525/525 / 522/522 release). 17/17 headless smokes per non-tidy config.

**ADR:** 0050, 0053, 0056 (registration grammar)
**Session:** `docs/sessions/2026-05-07-scene-v1b-registry.md`

### v1c — `ArchetypeChunkStorage` (split — see v1c1 + v1c2)

Per the slice's own "may split" allowance, v1c was divided into two reviewable halves once the architecture got concrete. v1c1 ships the in-chunk machinery; v1c2 builds the archetype graph + entity lifecycle on top.

#### v1c1 — Chunk allocator + SoA layout ✅ shipped 2026-05-07

`ChunkLayout` (per-archetype byte plan: sorted `ComponentId` list, sizes, alignments, offsets, entity-id offset, capacity), `ChunkHeader` (entity_count, entity_capacity, archetype_id back-ref, layout-local `version_counter[kMaxComponentsPerArchetype]`), `Chunk` (raw 16 KB block + accessors — no entity ops; those live on `Archetype`), `ChunkAllocator` (aligned alloc/free with dtor cleanup).

Two design choices documented in the v1c1 session log:

- `kMaxComponentsPerArchetype` (32) is independent of per-World `kMaxComponents` (256). Caps the in-chunk version-counter array at 256 B; archetypes wider than 32 components return an invalid layout (`entity_capacity == 0`).
- `version_counter` is *layout-local* indexed (0..K-1 where K is this archetype's component count), not by global `ComponentId`. Saves ~1.7 KB per chunk vs the original sketch.

11 tests / 88 assertions; six-config green (536/536 / 533/533 release). 17/17 headless smokes per non-tidy config.

**ADR:** 0050
**Session:** `docs/sessions/2026-05-07-scene-v1c1-chunk-layout.md`

#### v1c2 — Archetype + Graph + Storage + Event Sink + typed World API ✅ shipped 2026-05-07

`Archetype` (mask + ChunkLayout + dense `Array<Chunk>` + dense `Array<ArchetypeId>` edge tables sized `kMaxComponents`). `ArchetypeGraph` (`HashMap<ComponentMask, ArchetypeId>` + 256-bit boost-style hash + memoised `after_add` / `after_remove` edges). `ArchetypeChunkStorage : IStorageBackend` (insert is upsert; bytewise entity move via the registry's lifecycle ops; swap-remove updates trailing entity's location; trailing-chunk-only free; `for_each_chunk` walks superset archetypes). `IStorageEventSink` (the Cerid signature — every storage mutation calls `on_insert` / `on_update` / `on_remove` / `on_entity_destroyed` through this interface; v1c2 ships `NullStorageEventSink` as the default; v1i swaps in the `IComponentIndex` fan-out without touching storage call sites). `World` gains the typed templates: `add_component<T>(e, value)`, `has_component<T>(e)`, `get_component<T>(e)`, `get_component_mut<T>(e)` (declared write — bumps version), `remove_component<T>(e)`. World becomes non-movable (storage references registry).

ADR-0050 §1 amended in place: `IStorageBackend::insert` takes `void* data` rather than `const void*` (storage may move-from); `for_each_chunk` carries an opaque `void* user_data` pointer.

27 tests / 1345 assertions added. Six-config green (563/563 / 560/560 release).

**ADR:** 0050, 0053
**Session:** `docs/sessions/2026-05-07-scene-v1c2-archetype-storage.md`

### v1d — `SparseSetStorage` (~250 LOC + tests)

Per-component pool (sparse → dense → T). `IStorageBackend` impl. Tests: insert/remove O(1), iteration order matches insertion, large-N stress.

**ADR:** 0050

### v1e — Mixed-backend queries + chunk visitor (~200 LOC + tests)

Storage-side `for_each_chunk` interface. The plumbing that lets queries (next slice) walk both Archetype and SparseSet uniformly. Tests: query that touches both backends emits correct entity set.

**ADR:** 0050, 0052

### v1f — Relations (~400 LOC + tests)

`Relation<Tag>` template + `add_relation` / `remove_relation` / `get_relation_target` / `traverse_relation`. Built-in `ChildOf`, `AttachedTo`. Reverse-index trait + `Acyclic` debug assertion + `OnTargetDestroyed` policy. Tests: hierarchy add/remove, cycle detection (debug), cascade-on-destroy, AttachedTo with detach-on-destroy.

**ADR:** 0051

### v1g — Query DSL (~500 LOC + tests)

`world.query<Cs...>().with<>().without<>().with_relation<>().filter()`. Range-for and chunk-iterator API. Tests: each filter operator alone, composed filters, mixed-backend queries, empty query → empty range.

**ADR:** 0052

### v1h — System + Schedule (~300 LOC + tests)

`ISystem` interface, `Reads<>/Writes<>` declarations, `SchedulePhase` enum (PrePhysics/Physics/PostPhysics/Update/PreRender/RenderExtract/PostRender), `world.register_system(...)`, `world.step(dt)`, `world.step_fixed(dt, max_substeps)`. Phase-ordered serial dispatch in v1; auto-parallel within a phase reserved. Command buffers with thread-local accumulation + serial flush at phase boundary. Tests: phase ordering, command-buffer flush correctness, fixed-step accumulator math.

**ADR:** 0052

### v1i — Index framework + ChangeDetect + AsyncAware (~400 LOC + tests)

`IComponentIndex` interface, `World::register_index`, dispatch from storage. `ChangeDetectIndex` (chunk-version-driven `.changed<T>()` operator), `AsyncAwareIndex` (LoadState-driven `.skip_pending<T>()` operator). Tests: insert/update/remove events fire for each index, `changed<T>` skips unchanged chunks, `skip_pending<T>` filters in-flight loads.

**ADR:** 0053

### v1j — `Transform` + `TransformPropagation` system (~350 LOC + tests)

`Transform` component (TRS + cached world matrix), `TransformDirtyFlag` parallel SoA, `TransformPropagation` system in `PreRender` phase. Push-based dirty propagation, single-threaded topological walk. Tests: hierarchy propagation correctness, dirty-flag only triggers necessary recomputes, `set_world` decomposes correctly.

**ADR:** 0054

### v1k — `SceneResource` + `SceneLoader` (~300 LOC + tests)

`SceneResource` payload type, `SceneLoader` registered for FourCC `'SCEN'` with `ResourceManager`. Reads SCEN artifact (CRDR), constructs `World` content from ETBL/STRP/CMPS/C###/RELS chunks. Tests: round-trip a hand-built SCEN through the loader, schema-version-mismatch rejection, missing-component error path.

**ADR:** 0055

### v1l — `cook_scene` cooker handler (~500 LOC + tests)

`.scene.toml` → SCEN CRDR. Parses TOML, resolves `@asset:` references via `.meta` sidecars, walks component-type registry to emit per-component blobs, writes ETBL/STRP/CMPS/C###/RELS chunks. Tests: round-trip a hand-written scene TOML, reject scene with unresolved `@asset:`, reject duplicate entity names, accept and re-emit relations.

**ADR:** 0055

### v1m — Sandbox renderer integration (~200 LOC)

`SandboxLayer` constructs a `World`, populates from a cooked scene, registers `TransformPropagation`. The current explicit `Renderer::submit` calls become a query `world.query<Transform, Renderable>().par_each(...)` driving the existing `ForwardRenderPath`. Procedural-shape clicks now spawn entities into the world rather than uploading directly. Tests: `crd-sandbox --headless` mounts a cooked scene and exits 0; manually verify the sandbox panel shows scene entities under the Asset Browser's existing UI.

**ADR:** none new — integrates 0049–0055.

### v1n — Reserved-slot freeze (~150 LOC + tests)

Final pass: confirm every reserved trait (`History`, `SpatialBVH`, `GpuResident`, `Replication`, `Reflection`, `ScriptComponent`) is accepted by `register_component`, stored, and otherwise no-ops. Reserved DSL operators (`.at(frame)`, `.in_aabb()`, `.within_radius()`, `.group_by<>()`) parse and return correct empty/passthrough behaviour. Tests: registration accepts all traits without compilation error; deferred-impl operators round-trip without crashing; documentation in code points at the consumer-phase ADR for each.

**ADR:** 0053, 0056 (formal API freeze)

---

## Definition of done (Phase 3.0)

1. All 14 slices shipped with unit tests.
2. ADRs 0049–0057 written and in `Accepted` status. (Done 2026-05-06.)
3. Six-configuration green for the entire `crd-scene` module across all changes.
4. `smoke_scene.exe` (headless): cook a TOML scene, mount it, instantiate entities, run one frame, assert entity count + transform correctness, exit 0.
5. `smoke_scene_render.exe` (GPU): cook a scene with one mesh + transform, load, render one frame via `ForwardRenderPath` driven by query, exit 0.
6. `smoke_scene_stress.exe` (GPU/perf, optional): 100K entities with `(Transform, Renderable)`, propagate, render, log average frame ms. Used to validate the architecture meets million-entity targets at the chunk-iteration level (extrapolation).
7. `crd-sandbox` Asset Browser unchanged in UI; under the hood it now constructs a scene rather than directly calling `Renderer::submit`.
8. `docs/systems/scene.md` written.

---

## Pulled-forward prerequisites (must land before or alongside this phase)

- **Async GPU upload** (`GpuUploader::upload_mesh_async` / `upload_texture_async`). Phase 2.8 v1g shipped CPU-side `load_async`, but GPU upload is still synchronous. Streaming scene loads will hit this hitch every time a scene mounts new geometry. The `AsyncAwareIndex` (Layer 5, Phase 3.0 v1i) provides the query-side filter; the GPU-side completion machinery still needs to be designed and shipped.
  - Full debt note: `docs/debt.md` → "Async GPU upload (`GpuUploader`)".
  - Open question: does `crd-scene` own the polling, or does each `RenderableComponent` carry an `UploadHandle` field that the renderer's `skip_pending<Renderable>` filter consults?
  - Recommended decision: ship async upload as part of v1m (sandbox integration) — that is the first slice where streaming-load pressure becomes visible; design forced by real consumer.

---

## Out of scope for Phase 3.0

- Auto-parallel scheduling within a phase — reserved API; impl Phase 3.5+.
- Implementations of `History`, `SpatialBVH`, `GpuResident` — API only here; impl in 3.2 (animation interp), 3.5 (light culling at scale), 3.8 (GPU-driven rendering) respectively.
- Replication packets and rollback — Phase 4.2.
- Script execution, hot-reload — Phase 4.0.
- Editor inspector, reflection walker — Phase 7.
- Physics components — Phase 3.1.
- Skinning / animation components — Phase 3.2.
- UI components beyond `ControlNodeTag` — Phase 5.

---

## References

- ADR-0020 — Scene & ECS hybrid + UI in tree (cornerstone)
- ADR-0049 — Entity identity & SlotMap (L1)
- ADR-0050 — Storage backends Archetype + SparseSet (L2)
- ADR-0051 — Relations as first-class (L3)
- ADR-0052 — Query · System · Schedule (L4)
- ADR-0053 — Component index slot framework (L5)
- ADR-0054 — Transform hierarchy update model
- ADR-0055 — Scene serialization (TOML + SCEN CRDR)
- ADR-0056 — Reserved L6–L8 slots
- ADR-0057 — UI in scene tree (boundary)
- ADR-0044 — Phase ordering (this phase's predecessor in the queue)
- `docs/phases/phase-2.8-material-completion.md` — Predecessor phase (complete)
- `docs/phases/phase-3-simulation.md` — Sibling reference (3.1+)
- `docs/debt.md` — Async GPU upload prerequisite
