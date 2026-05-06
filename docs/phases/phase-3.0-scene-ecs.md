# Phase 3.0 — Scene / ECS Foundation

**Status:** ⏳ ready to start (ADRs locked 2026-05-06; all 9 sub-ADRs accepted)
**ADRs:** ADR-0049, ADR-0050, ADR-0051, ADR-0052, ADR-0053, ADR-0054, ADR-0055, ADR-0056, ADR-0057
**Cornerstone:** ADR-0020 (scene/ECS hybrid, UI in tree)
**New module:** `crd-scene`
**Depends on:** Phase 2.8 complete (MaterialResource with PSO state + pass-keyed variants)

---

## Goal

Land the foundation `crd-scene` module: a layered, slot-shaped ECS where every novel ECS extension (change detection, time-tunneling, spatial queries, GPU-residency, replication, scripts, reflection) is a slot in the same registration grammar — most filled in Phase 3.0, the rest reserved for their consumer phases.

This is the **highest-leverage single block of work in the engine**. Every Phase 3.x onwards consumes it.

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
| L1 Entity / SlotMap | Full impl | v1a |
| L2 ArchetypeChunkStorage | Full impl | v1c |
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

### v1a — `EntityId` + `SlotMap` + `World` shell (~250 LOC + tests)

`EntityId` (32:32 gen+idx), `SlotMap` (dense `Array<Slot>` + free list, deferred destroy queue), `World` shell that owns the slot map. Unit tests: spawn/destroy round-trip, generation collision detection, deferred destroy semantics, null sentinel.

**ADR:** 0049

### v1b — `ComponentRegistry` + storage-hint registration (~200 LOC + tests)

`register_component<T>(StorageHint, ...traits)` accepts the variadic trait grammar. `ComponentId` (`u16`) assigned at registration. The `IStorageBackend` interface declared. Tests: registration round-trip, duplicate-registration rejected, ComponentId stability across re-registration order.

**ADR:** 0050, 0053, 0056 (registration grammar)

### v1c — `ArchetypeChunkStorage` (~600 LOC + tests)

16 KB chunks, 64-byte aligned SoA, archetype graph (memoised lazy build), per-chunk version counters. Tests: chunk allocation, archetype creation, entity insert/move/remove across archetypes, chunk fill/spill, version-counter increment on write.

May further split into v1c1 (chunk allocator + SoA layout) and v1c2 (archetype graph + entity move) if scope grows.

**ADR:** 0050

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
- Implementations of `History`, `SpatialBVH`, `GpuResident` — API only here; impl in 3.2 / 3.5 / 3.4 respectively.
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
