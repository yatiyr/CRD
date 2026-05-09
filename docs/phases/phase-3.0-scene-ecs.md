# Phase 3.0 — Scene / ECS Foundation

**Status:** 🚧 active — v1a–v1m shipped 2026-05-06 / 07 / 08; phase expanded from 14 to **17 slices** (2026-05-08) to land the elite-tier authoring substrate (Öbek + Preset + Profile) before Phase 3.0 closes. **v1m (Öbek system) FULLY DELIVERED 2026-05-08** across 12 sub-slices (~2700 LOC, 58 öbek tests). Next: **v1n (Preset + Profile system)**, then v1o (sandbox integration), then v1p (reserved-slot freeze).
**ADRs:** ADR-0049, ADR-0050, ADR-0051, ADR-0052, ADR-0053, ADR-0054, ADR-0055, ADR-0056, ADR-0057, **ADR-0058 (Öbek)** ✅ realised, **ADR-0059 (Preset), ADR-0060 (Profile)** ⏳ v1n, **ADR-0061 (Async GPU upload contract)** ⏳ v1o1+v1o2 (locked 2026-05-09)
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
| v1f | Relations + 6 built-ins (ChildOf / AttachedTo / Owns / Targets / DependsOn / PossessedBy) + ReverseIndex / Acyclic / OnTargetDestroyed traits + iterative destruction worklist | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1f-relations.md` |
| v1g | Query DSL — `world.query<Cs...>().with/without/with_relation/filter` chain + range-for + chunk visitor; built on v1e's mixed-backend visitor and v1f's reverse indexes | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1g-query-dsl.md` |
| v1h | System + Schedule + Commands — `ISystem` virtual class, 7-phase fixed schedule, `register_system` / `step` / `step_fixed`, deferred-mutation `Commands` buffer flushed at phase boundaries | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1h-system-schedule.md` |
| v1i | Index framework (`IComponentIndex` + fan-out sink + auto-register) + `ChangeDetectIndex` + `AsyncAwareIndex` + 5 reserved no-op shells (History / SpatialBVH / GpuResident / Replication / Reflection); `.changed<T>()` and `.skip_pending<T>()` query operators | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1i-index-framework.md` |
| v1j | Transform (TRS + cached world) + TransformPropagation system in PreRender; `TransformDirtyFlag` SparseSet marker; six rotation-set APIs (quat/axis_angle/euler/from_to/look_at/quat_unnormalized); `set_world` / `try_set_world` with negative-determinant handling; cross-domain robust (games/robotics/aerospace/DAW); determinism bit-exact verified | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1j-transform-propagation.md` |
| v1k | SceneResource + SceneLoader (FourCC `'SCEN'`) + SceneArtifactBuilder + `World::instantiate_scene` + 6 built-in relations get serialize traits; forward-compat by FourCC, hard-fail on size/version mismatch; determinism bit-exact verified | ✅ shipped 2026-05-07 — `docs/sessions/2026-05-07-scene-v1k-scene-resource.md` |
| v1l | `cook_scene` cooker handler — `crd-cooker` extended with `SceneCooker` + `scene_cooker_inline()`; built-in TOML readers (Transform + 6 built-in relations); three-pass cooker (collect → apply alphabetical → install relations); cooker-side `TransformPropagation::step()` bakes world matrices into SCEN bytes; determinism bit-exact verified | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1l-cooker.md` |
| v1m | **Öbek system** — full ADR-0058 surface: `ObekResource` + `ObekLoader` (FourCC `'OBEK'`) + `ObekArtifactBuilder`; full `ObekCooker` (rides v1l SceneCooker, ~70% reuse); `extends` chain + nested öbek composition + cycle detection; runtime override patches with stable file_idx + symbolic name fallback + cook-time `overrides=[...]` → OOVR; `World::instantiate_obek` (+ batch API) with parent reparenting; **all three `InheritPolicy` values** (Override / Inherit-CoW with content-hash dedup / DontInherit); revert at four granularities (field/component/entity/all) + unpack/unpack_keep_overrides + enumerate_overrides; AAAA reservations (OBAT/OLNK FourCCs, ReplicationMode, streaming.lod/region, static_bake, ObekEntityGuid); 12 sub-slices, ~2700 LOC, 58 tests | ✅ shipped 2026-05-08 (12 sub-slices) — see v1m sub-table below |
| v1m1 | Öbek substrate — ObekResource + ObekLoader + ObekArtifactBuilder + World::instantiate_obek with parent reparenting; CRDR layout OINF/OETB/OCMP/D###/ORLS; ObekEntityGuid (FNV-1a 64) | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m1-obek-substrate.md` |
| v1m2 | Runtime ObekOverride + bounds-check + symbolic-name fallback; OCHN format substrate (ObekChainEntryRecord + add_chain_dependency + chunk emit/parse) | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m2-overrides-and-ochn.md` |
| v1m3a | ObekCooker class + `obek_cooker_inline` (TOML → flat OBEK CRDR; reader registry shared with SceneCooker) | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m3a-obek-cooker-substrate.md` |
| v1m3b | `extends` chain resolution: iterative walk, cycle detection, deepest-first apply, OCHN entries | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m3b-extends-chain.md` |
| v1m3c | Nested öbek refs (`obek = "..."`); recursive walk_and_apply_chain; ChildOf splice; nested name scoping | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m3c-nested-obek.md` |
| v1m3d | Cook-time `overrides = [...]` → OOVR chunk; auto-applied at instantiate before caller patches | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m3d-cook-time-overrides.md` |
| v1m4 | InheritPolicy enum + apply_trait dispatch; DontInherit fully implemented; Inherit-as-stub | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m4-inherit-policy.md` |
| v1m4b | InheritPolicy::Inherit transparent CoW backend (3 sub-slices: SharedComponentPool / per-slot ownership + force-SparseSet / content-hash dedup); demonstrated 2× memory savings on "1000 trees from same öbek" pattern | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m4b-cow-backend.md` |
| v1m5 | revert/unpack/enumerate APIs (`ObekInstantiation::source` + revert_field/component/entity/all + unpack_obek + unpack_obek_keep_overrides + enumerate_overrides) + AAAA-tier batch reservations (BatchHints + BatchInstanceTag + ObekBatchHandle + instantiate_obek_batch) | ✅ shipped 2026-05-08 — `docs/sessions/2026-05-08-scene-v1m5-revert-batch.md` |
| v1n | **Preset + Profile system** — `PresetResource` + per-type `PresetLoader` + `PresetRegistry`; first concrete types `QualityPreset` (`'PRQL'`) + `CameraPreset` (`'PRCM'`) wired into `IRenderPath` and `Camera`; `extends` chain (shares Öbek resolver); five-layer resolution stack (default → extends → preset → instance → runtime); `ProfileResolver` with closed typed predicates (os / gpu_tier / domain / mode / target_fps / cpu_cores); additive profile composition (priority-sorted stack, Cerid-unique vs Unreal first-match-wins); runtime context detection; hot-reload + atomic swap | ⏳ next |
| v1o | **Async GPU upload contract (ADR-0061) + Sandbox integration with Öbek + Preset + Profile** — 3 sub-slices: **v1o1** `crd::rhi::Fence` + non-waiting `Queue::submit(cmd, fence)`; **v1o2** `UploadHandle` + `GpuUploader::upload_*_async` + `PendingMeshUpload` component + `RenderUploadSystem` (RenderExtract); **v1o3** sandbox uses async upload + profile + öbek end-to-end; ImGui panel toggles profile + reverts overrides live; visual proof of full authoring stack | ⏳ |
| v1p | **Reserved-slot freeze** — registration grammar test for L6/L7/L8 reserved indexes (Replication / ScriptComponent / Reflection trait acceptance); öbek/preset/profile API surface frozen; closes Phase 3.0 | ⏳ |

`crd-scene` now ships **the L4 scheduling half** per ADR-0052 §3-§5. `ISystem` has `Reads`/`Writes` `ComponentSet` type aliases (computed into masks via `World::component_set_mask<Set>()`) — auto-parallel scheduling reads them in Phase 3.5; v1h dispatches serially. The 7-phase schedule (PrePhysics → PostRender) runs systems in registration order within each phase. `step_fixed(dt, fixed_dt, max_substeps)` interleaves fixed-step systems N times then variable-rate once per phase, with accumulator carry-over and spiral-of-death clamp. `Commands` queues mutations during iteration and flushes at every phase boundary; spawn is immediate (single-threaded v1h), all other ops deferred. 172 unit tests / 34602 assertions; six-config green.

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
| L2 SparseSetStorage | Full impl | v1d ✅ shipped 2026-05-07 |
| L3 Relations | Full impl + 6 built-ins (ChildOf, AttachedTo, Owns, Targets, DependsOn, PossessedBy) | v1f ✅ shipped 2026-05-07 |
| L4 Query DSL | Full impl | v1g ✅ shipped 2026-05-07 |
| L4 System + Schedule | 7-phase serial v1; auto-parallel reserved | v1h ✅ shipped 2026-05-07 |
| L5 Index framework | Full impl | v1i ✅ shipped 2026-05-07 |
| L5 ChangeDetect | Full impl (chunk-grain) | v1i ✅ shipped 2026-05-07 |
| L5 AsyncAware | Full impl | v1i ✅ shipped 2026-05-07 |
| L5 History\<N\> | API only — `at(frame)` returns current frame | v1p (freeze) |
| L5 SpatialBVH | API only — filters return full set | v1p (freeze) |
| L5 GpuResident | API only — no GPU mirror yet | v1p (freeze) |
| L6 Replication | API only — `Replication::*` accepted at registration | v1p (freeze) |
| L7 ScriptComponent type | API only — type defined, system is no-op | v1p (freeze) |
| L8 Reflection | Compile-time hooks reserved | v1p (freeze) |
| Transform component | Full impl + propagation system | v1j ✅ shipped 2026-05-07 |
| Scene serialization | TOML cooker + SCEN CRDR + SceneLoader | v1k–v1l ✅ shipped 2026-05-07/08 |
| **Öbek system** | **Full impl** — entity-graph templates with composition, variation, override patches (cook-time + runtime), all three InheritPolicy values including transparent CoW, revert at four granularities + unpack semantics, batch API, format-reserved AAAA hooks | **v1m ✅ shipped 2026-05-08 (12 sub-slices)** |
| **Preset + Profile system** | QualityPreset + CameraPreset; ProfileResolver with closed typed predicates; additive composition; hot-reload | **v1n ⏳ next** |
| Renderer integration | Sandbox loads `.scene.toml` referencing öbeks; profile-driven preset application; ImGui live override panel | **v1o ⏳** |

---

## Slices

17 slices, each individually shippable per the project's Definition of Done (six-config green, unit tests, headless smoke where applicable). Larger slices (v1c, v1g, **v1m**) may further sub-divide during implementation. Phase expanded from 14 to 17 slices on 2026-05-08 to land the elite-tier authoring substrate (Öbek + Preset + Profile) inside Phase 3.0 — see ADRs 0058/0059/0060.

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

### v1m — Öbek system ✅ shipped 2026-05-08 (12 sub-slices, ~2700 LOC, 58 öbek tests)

The single largest slice in Phase 3.0. **Delivered the elite-tier entity-graph-template substrate per ADR-0058's 19 design pillars** across 12 reviewable sub-slices. See the v1m sub-table above for sub-slice-level status; the description below documents the design surface that shipped (not a forward plan). Two follow-ups deferred to post-Phase-3.0 task #108: hot-reload watcher with OCHN graph awareness + `obekc extract` CLI tool. Six-config 814/814 / 811 release post-v1m close.

**Substrate:**
- `crd::scene::Obek`, `ObekResource`, `ObekLoader` (FourCC `'OBEK'`), `ObekArtifactBuilder`.
- CRDR layout: OINF / OETB / OCMP / ORLS / OOVR / OCHN chunks; OLNK + OBAT chunks reserved (Phase 3.5+).
- `ObekCooker` extends `crd-cooker` riding the v1l SceneCooker substrate (~70% code reuse).

**Composition + variation:**
- Nested öbek references (`obek = "..."`) — eager flatten by default, lazy reference reserved.
- Variant chains (`extends = "..."`) — depth-unbounded, cycle-detected at cook time.
- Cooker pass schedule: parse extends → flatten variant chain → walk entities (document order) → walk components (alphabetical) → resolve nested → resolve relations → validate overrides → bake world matrices → emit CRDR.

**Override patches:**
- Typed `ObekOverride` struct with packed `field_path` (no string lookup at runtime).
- Cook-time validated against öbek schema manifest.
- Sortable, diffable, persistable into SCEN.
- Symbolic-name fallback when `file_idx` fails to validate.

**InheritPolicy — all three values fully implemented:**
- `Override` (default) — private copy per instance.
- `Inherit` — shared backing with **transparent CoW write interception**. Backend storage (ArchetypeChunkStorage + SparseSetStorage) gains per-entity per-component "owned vs shared" flag bit; write paths check the bit and copy-on-first-write. Eviction tracks shared backing via reference count.
- `DontInherit` — skipped on instantiation.

**Per-instance freedom:**
- ADD entities not in source (`spawn_into_obek_instance`, tagged `instance_only`).
- Soft-DELETE source entities (`disabled = true` flag; file_idx remains stable).
- No reorder; restructure source via `obekc migrate` tool (ships in v1m or later).

**Apply / Revert at four granularities:**
- `revert_field` / `revert_component` / `revert_entity` / `revert_all`.
- `apply_back_to_source(instance, override_subset, target_path)` re-cooks source.
- `enumerate_overrides(instance)` for editor "override window" UI.

**Unpack semantics:**
- `unpack_obek(instance)` — sever öbek link, all Inherit components forcibly unlinked, overrides discarded.
- `unpack_obek_keep_overrides(instance)` — sever link but bake overrides into entities.

**Hot-reload graph-aware:**
- `OCHN` chunk lists every transitive dependency (extends + nested) with content hashes.
- Watcher tracks chain; any link change triggers transitive re-cook + atomic swap.
- Reload event lists changed `file_idx`s for selective re-apply on live instances.

**AAAA-tier future-proofing (format-reserved at v1m, runtime backends in consumer phases):**
- `instantiate_obek_batch(obek, transforms[], parent, BatchHints)` API + `OBAT` chunk for GPU instanced rendering (Phase 3.5+ renderer wires it up).
- `GpuResident` + `static_bake` per-component flags reserved in OINF (already declared in ADR-0053; v1m populates the bits).
- `ObekEntityGuid` (64-bit hash of `obek_root_id` + `file_idx`) — stable cross-machine identity for replay (Phase 8) + replication (Phase 4.2).
- `ReplicationMode` + `streaming.lod` + `streaming.region` per-entity reserved fields.
- `mode = "lazy"` opt-in flag accepted; OLNK chunk format-reserved (lazy loader lands Phase 3.5+).

**Tests (~30 cases):**
- Empty öbek cooks + round-trips.
- Single-entity Transform öbek instantiates correctly.
- Composition: vehicle with 4 nested wheels.
- Variant: sports-car extends vehicle-base; field overrides win.
- Variant chain (3 deep) resolves correctly.
- Cycle detection in extends + nested.
- All three InheritPolicy values with read + write semantics including CoW.
- Override patch validation + symbolic-name fallback.
- Per-instance ADD + soft-DELETE + override revert at all four granularities.
- `unpack_obek` + `unpack_obek_keep_overrides`.
- Hot-reload of leaf öbek triggers re-cook of consumers.
- Determinism: same source files → bit-exact CRDR bytes (FNV hash verified).
- Batch instantiation tags entities with `BatchInstanceTag`.
- `ObekEntityGuid` stable across cook re-runs.
- 100-entity stress test with mix of variants + nesting + overrides.

**ADR:** 0058 (full architecture + 19 pillars)

### v1n — Preset + Profile system (~800 LOC + ~20 tests, 3–4 days)

Phase 3.0's second new substrate: ADR-0059 (Preset) + ADR-0060 (Profile). Together they map runtime context → ordered preset bundle → typed apply callbacks against live `IPresetTarget` impls.

**Preset substrate (ADR-0059):**
- `PresetResource` + per-type `PresetLoader` + `PresetRegistry` (closed by C++ types).
- `register_type<T>(name)` registers FourCC + schema + TOML reader + apply dispatch.
- Five-layer resolution: schema default → extends chain → active preset → per-instance → runtime override.
- CRDR layout: PINF / PDAT / PCHN per cooked preset.
- `extends` chain shares the Öbek resolver (single cooker pipeline).
- Hot-reload with atomic swap + last-good fallback.

**First concrete preset types (ship in v1n):**
- `QualityPreset` (FourCC `'PRQL'`) — shadow_resolution, MSAA, SSR/SSAO quality, post-fx list. Wired into `IRenderPath::apply(QualityPreset)`.
- `CameraPreset` (FourCC `'PRCM'`) — FOV, near/far, lens, exposure curve. Wired into `Camera::apply(CameraPreset)`.

**Profile substrate (ADR-0060):**
- `ProfileResource` + `ProfileLoader` (FourCC `'PROF'`) + `ProfileResolver`.
- Closed predicate schema: `os` / `gpu_tier` / `domain` / `mode` / `target_fps` / `cpu_cores`.
- Additive composition: priority-sorted stack, all matching profiles compose, deepest priority wins per field.
- Runtime context detection (platform / RHI capability / config-driven).
- CRDR layout: FINF / FRLE / FBND chunks.

**Tests (~20 cases):**
- QualityPreset cooks + round-trips + applies to mock IRenderPath.
- CameraPreset same.
- `extends` chain depth 3 resolves correctly per field.
- Hot-reload of leaf preset re-cooks + re-applies.
- Profile single-rule match.
- Profile additive composition (3 profiles match → applied in priority order).
- Predicate operators (`==`, `>=`, `<=`, `in [...]`).
- Runtime context detection mocked + driving resolver.
- Determinism: same context + same profiles → same applied bundle bytes.
- Failed cook keeps last-good preset.

**ADR:** 0059 (Preset), 0060 (Profile)

### v1o — Sandbox renderer integration with Öbek + Preset + Profile + async GPU upload (~800 LOC, 4–5 days)

The visual proof of the full authoring stack PLUS the first consumer of the async GPU upload contract (ADR-0061). Originally planned as a small "wire renderer to scene query" slice; expanded twice — first to demonstrate Öbek + Preset + Profile end-to-end (2026-05-08), then to host the async-upload contract's first real consumer (2026-05-09).

#### v1o1 — `crd::rhi::Fence` + non-waiting `Queue::submit(cmd, fence)` (~150 LOC, 4 tests)

Adds the RHI primitive needed by the async upload contract. Vulkan backend wraps `VkFence`. Fence interface: `is_signaled()` (non-blocking), `wait()` (blocking), `reset()`. Device gains `create_fence()`. Queue gains `submit(CommandBuffer&, Fence&)` — submits without waiting and signals the fence on completion.

ADR: 0061 §"Layer 1 — `crd-rhi`".

#### v1o2 — `UploadHandle` + `GpuUploader::upload_*_async` + `PendingMeshUpload` + `RenderUploadSystem` (~250 LOC, 6 tests)

Layers 2 + 3 of ADR-0061. `UploadHandle` (move-only; owns fence + staging buffer + produced GpuMesh/GpuTexture). `GpuUploader::upload_mesh_async` / `upload_texture_async` (record + submit + return handle). `PendingMeshUpload` component. `RenderUploadSystem` (RenderExtract phase; polls handle, consumes on ready, populates Renderable buffer pointers, removes the marker, calls `world.async_aware().mark_loaded(e, ComponentTypeTag<Renderable>)`). Existing renderer code's `world.query<Transform, Renderable>().skip_pending<Renderable>()` automatically skips pending entities.

ADR: 0061 §"Layer 2" + §"Layer 3".

#### v1o3 — Sandbox integration: Öbek + Preset + Profile + async upload end-to-end (~400 LOC)

**Implementation:**
- `SandboxLayer` constructs a `World`, registers `TransformPropagation` + `RenderUploadSystem`, registers `IRenderPath` + `Camera` as `IPresetTarget` impls.
- App boot resolves `ProfileContext` (os / gpu_tier / domain=game / mode=runtime / target_fps / cpu_cores), runs `ProfileResolver`, applies bundle → renderer + camera reconfigure.
- Sandbox loads `assets/sources/sandbox.scene.toml` referencing 2–3 demo öbeks (procedural shapes graduate into öbeks; imported glTF assets become öbek-wrapped meshes).
- Asset Browser click now uses the async path: `load_async<MeshResource>` → on CPU ready, `upload_mesh_async` → spawn entity with Renderable + `PendingMeshUpload` → RenderUploadSystem flips state to Loaded automatically; render queries skip until ready.
- ImGui panel adds:
  - Profile picker (`game / simulation / daw / cinematic` — re-resolves on change).
  - Quality slider (`Low / Medium / High / Ultra` — runtime override at L4).
  - "Override window" enumerating any override patches on the currently-selected entity, with revert buttons at field / component / entity / all granularities.
  - "Unpack öbek" button — visualises the unpack operation.
- Ships demo profile bundle: `assets/profiles/default.profile.toml` with sensible game/sim/DAW/cinematic baselines.

**Tests:**
- `crd-sandbox --headless --domain=simulation` mounts a cooked scene + profile, exits 0.
- GPU smoke: sandbox renders a scene of öbeks with applied quality preset; visual verification.
- Async upload smoke: load BoomBox via the async path, verify per-frame rendering remains within target frame time during upload (no `vkQueueWaitIdle` hitch).

**ADR:** 0061 (async GPU upload); integrates 0049–0060.

### v1p — Reserved-slot freeze (~150 LOC + tests)

Phase 3.0's closer. Final pass: confirm every reserved trait (`History`, `SpatialBVH`, `GpuResident`, `Replication`, `Reflection`, `ScriptComponent`) is accepted by `register_component`, stored, and otherwise no-ops. Reserved DSL operators (`.at(frame)`, `.in_aabb()`, `.within_radius()`, `.group_by<>()`) parse and return correct empty/passthrough behaviour. Öbek + Preset + Profile API surfaces are formally frozen — Phase 3.5+ consumer phases implement against them but cannot change them.

Tests: registration accepts all traits without compilation error; deferred-impl operators round-trip without crashing; documentation in code points at the consumer-phase ADR for each; öbek/preset/profile API surface freeze verified by API hash.

**ADR:** 0053, 0056, 0058, 0059, 0060 (formal API freeze)

---

## Definition of done (Phase 3.0)

1. All **17 slices** shipped with unit tests.
2. ADRs 0049–0057 written and in `Accepted` status. (Done 2026-05-06.) **ADRs 0058–0060 written and in `Accepted` status. (Done 2026-05-08.)**
3. Six-configuration green for the entire `crd-scene` + `crd-cooker` modules across all changes.
4. `smoke_scene.exe` (headless): cook a TOML scene, mount it, instantiate entities, run one frame, assert entity count + transform correctness, exit 0.
5. `smoke_obek.exe` (headless): cook a `.obek.toml` with composition + variant + overrides, instantiate into a World, validate file_idx stability + override application + InheritPolicy semantics, exit 0.
6. `smoke_preset_profile.exe` (headless): cook QualityPreset + CameraPreset + Profile; resolve against mock context; apply to mock IPresetTargets; verify five-layer resolution precedence, exit 0.
7. `smoke_scene_render.exe` (GPU): cook a scene with one mesh + transform, load, render one frame via `ForwardRenderPath` driven by query, exit 0.
8. `smoke_scene_stress.exe` (GPU/perf, optional): 100K entities with `(Transform, Renderable)`, propagate, render, log average frame ms. Used to validate the architecture meets million-entity targets at the chunk-iteration level (extrapolation).
9. `crd-sandbox` reads a `.scene.toml` referencing öbeks; profile auto-resolves at boot; ImGui override-window panel works live.
10. `docs/systems/scene.md` written; `docs/systems/obek.md` written; `docs/systems/preset.md` written.

---

## Pulled-forward prerequisites (must land before or alongside this phase)

- **Async GPU upload** (`GpuUploader::upload_mesh_async` / `upload_texture_async`). Phase 2.8 v1g shipped CPU-side `load_async`, but GPU upload is still synchronous. Streaming scene loads would hit this hitch every time a scene mounts new geometry.
  - **Design decision LOCKED 2026-05-09: ADR-0061** — three-layer contract:
    1. `crd-rhi` adds `Fence` + `Queue::submit(cmd, fence)` non-waiting variant.
    2. `crd-renderer` adds `UploadHandle` + `GpuUploader::upload_*_async` + `PendingMeshUpload` component + `RenderUploadSystem` (RenderExtract phase).
    3. `crd-scene` is unchanged — already exposes `AsyncAwareIndex` + `skip_pending<Renderable>()`.
  - **Implementation timing:** v1o sub-slices v1o1 (RHI fence) + v1o2 (UploadHandle + PendingMeshUpload + RenderUploadSystem). v1n needs no GPU-upload changes.
  - Full ADR: `docs/decisions/0061-async-gpu-upload-contract.md`.
  - Original debt note: `docs/debt.md` → "Async GPU upload (`GpuUploader`)" — design half closed by ADR-0061; implementation half closes when v1o ships.

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
