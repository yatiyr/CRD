# 2026-05-07 — Phase 3.0 v1i: Index framework + ChangeDetect + AsyncAware + reserved slots

**Status at start:** Phase 3.0 v1h shipped. ISystem + 7-phase schedule + Commands. Scene tests 172 / 34602, six-config 688/688.

**Status at end:** v1i shipped. `IComponentIndex` plug-point + fan-out sink + auto-registration of trait-implied indexes + `ChangeDetectIndex` + `AsyncAwareIndex` + five reserved no-op shells (History / SpatialBVH / GpuResident / Replication / Reflection) + `.changed<T>()` and `.skip_pending<T>()` query operators. Six-config 708/708 / 705 release / 17 smokes. Scene tests 192 / 34669.

**Layer 5 — the Cerid signature — is now observable in code.**

---

## Goal of this session

Land Layer 5's plug-point per ADR-0053. Three deliverables:

1. **Generic `IComponentIndex` framework** — every "novel ECS extension" implements this and registers with World; the storage backends fan events to all registered indexes whose `observed()` mask includes the touched component. Adding a new index becomes a one-day job.
2. **Two shipping consumers**: `ChangeDetectIndex` (drives `.changed<T>()` for cross-phase change tracking) and `AsyncAwareIndex` (drives `.skip_pending<T>()` for filter-out-loading-data semantics).
3. **Five reserved no-op shells** for History / SpatialBVH / GpuResident / Replication / Reflection — auto-registered when their trait flag is set on a component, accept events silently, ready for their consumer phases (3.2 / 3.5 / 3.8 / 4.2 / 7) to drop in real impls without caller code changes.

## What shipped

### New module files

```
engine/scene/include/crd/scene/component_index.hpp        ~50 LOC — IComponentIndex interface
engine/scene/include/crd/scene/change_detect_index.hpp    ~80 LOC
engine/scene/src/change_detect_index.cpp                  ~75 LOC
engine/scene/include/crd/scene/async_aware_index.hpp      ~95 LOC
engine/scene/src/async_aware_index.cpp                    ~95 LOC
engine/scene/include/crd/scene/reserved_indexes.hpp       ~80 LOC — 5 no-op shells
tests/scene/test_index_framework.cpp                      ~370 LOC, 20 cases
```

### Modified

- `engine/scene/include/crd/scene/world.hpp`
  - New includes for the four index headers.
  - New public methods: `register_index<Idx>(args...)`, `find_index<Idx>()`, `index_count()`, `current_frame()`.
  - **`set_storage_event_sink` semantic changed**: now installs an EXTERNAL test/debug sink that runs ALONGSIDE registered indexes. Pre-v1i tests that don't register indexes see unchanged behavior. Documented at the API doc-block.
  - **Internal `IndexFanOutSink`** nested class — single sink installed on both backends, dispatches every event to all indexes whose `observed()` mask matches, then forwards to `m_external_sink`.
  - New private members: `m_indexes` Array, `m_fanout_sink`, `m_external_sink`, `m_frame_index`.
  - `register_component<T>(traits...)` now calls `auto_register_indexes_for(id)` after registry stamping — auto-creates ChangeDetectIndex (always) plus any reserved-slot index whose trait flag is set.
  - New inline template body for `auto_register_indexes_for` using a generic lambda `ensure_and_watch<Idx>` that lazy-creates by exact dynamic type and adds the new ComponentId to the index's observed mask.
  - New inline `Query<Cs...>::changed<T>()` and `skip_pending<T>()` template operators (with both `&` and `&&` ref-qualified overloads).
  - Removed `m_event_sink` member (replaced by `m_fanout_sink` + `m_external_sink`).

- `engine/scene/src/world.cpp`
  - Constructor installs `m_fanout_sink` on both storage backends; `m_external_sink` defaults to `NullStorageEventSink`.
  - **Destruction order changed**: backends drain (firing per-component on_remove) BEFORE the fan-out fires `on_entity_destroyed`. Previously the order was reversed; the new order ensures that `ChangeDetect::on_entity_destroyed`'s entry-clear isn't clobbered by trailing `on_remove` events from the drain.
  - `step()` and `step_fixed()` increment `m_frame_index` and call `notify_frame_begin/end` once per call (fixed substeps share one frame).

- `engine/scene/include/crd/scene/storage_event_sink.hpp`
  - Doc-block on `on_entity_destroyed` updated: it now fires LAST (after backend drain), as the "all done — clean up your state" signal.

- `engine/scene/include/crd/scene/query.hpp`
  - New `ChangeDetectFilter` and `SkipPendingFilter` POD types alongside `RelationFilter` / `PredicateFilter`.
  - Query template grows two member arrays (`m_change_filters`, `m_skip_pending_filters`).
  - `run_query_pipeline` signature extended with the two new filter arrays.
  - Forward declarations for `ChangeDetectIndex` and `AsyncAwareIndex` (concrete types are pulled in via world.hpp).

- `engine/scene/src/query.cpp`
  - Includes `change_detect_index.hpp` and `async_aware_index.hpp` for index method calls.
  - `entity_passes` extended with two new filter passes (after relations, before custom predicates).
  - `run_query_pipeline` accepts and forwards the two new filter arrays into the visit context.

- `tests/scene/CMakeLists.txt` — added `test_index_framework.cpp`.

## Five architectural decisions pinned (advisor-driven)

### 1. ChangeDetect semantic = "modified during current frame"

`since_frame = world.current_frame()` captured at chain time. Filter passes iff `last_change_frame[(c, e)] >= since_frame`. Cross-phase pattern works naturally: PrePhysics writes Transform → PreRender's `query<Transform>().changed<Transform>()` sees those entities, regardless of whether the query was constructed before or after the write within the same frame.

Cross-frame "what changed since last time my system ran" is a v1h+1 evolution (requires per-system tracking; v1g's queries are constructed-fresh-per-step so they can't carry cross-frame snapshots).

### 2. Per-entity ChangeDetect for v1i; chunk-grain is v1h+1 slot

`HashMap<u64 key, u32 frame>` keyed by `(ComponentId, entity_index, entity_generation)` packed into one u64. Memory: ~16 B per (entity, watched-component) pair currently tracked. At 100K entities × 5 watched components ≈ 8 MB — acceptable for v1i; chunk-grain optimization (per ADR-0053 §3) lands when memory pressure shows.

Alternatives considered:
- Direct chunk version_counter[c] reads (storage already maintains them) — requires per-query snapshot of every chunk's counter at chain time; fragile when chunks are added/removed mid-frame.
- Per-(component, chunk) HashMap — fewer entries but needs chunk-pointer keys plus entity→chunk lookup at filter time; more complex.

The simpler shape ships sooner; the harder shape lands when the query benchmark actually shows it matters.

### 3. AsyncAware default = Loading on insert

`on_insert` marks `LoadState::Loading` (data present but not ready). Caller flips to `Loaded` via `mark_loaded(e, c)` when async work completes; `mark_failed(e, c)` for errors. `.skip_pending<T>()` excludes `Loading` only (Failed entities pass — caller decides how to handle).

Renderer + physics + audio pattern: tag the component with `AsyncAware{}` on registration, `mark_loaded` from the asset-load callback, queries automatically filter pending entities. No manual "is the data ready?" branches in consumer code.

### 4. `set_storage_event_sink` coexists with indexes (semantic changed)

Before v1i: single sink, replaced on each call, received every event.
After v1i: external sink runs ALONGSIDE registered indexes (forwarded by the fan-out).

Pre-v1i tests that don't register indexes: behavior unchanged (no indexes → fan-out forwards everything to the external sink). Pinned in code comments and the v1i session log so future readers know the contract.

### 5. Reserved no-op indexes auto-register on trait flag

Per ADR-0053 §2: "Each index, including the deferred ones, is registered at component-registration time." v1i honours this: `register_component<T>(History{60})` triggers `auto_register_indexes_for` which lazy-creates `HistoryIndex` (a no-op shell) and adds T's id to its `observed()` mask.

When the real impl ships (Phase 3.2 for History, 3.5 for SpatialBVH, etc.), the user's day-one registration starts doing real work without any caller code change — the contract ADR-0053 was written for.

## Bugs caught during integration

### Destruction order: ChangeDetect entries clobbered by trailing on_remove

Initial draft fired `m_fanout_sink.on_entity_destroyed(current)` BEFORE the backend drain. The drain then fires per-component `on_remove`, which `ChangeDetect::on_remove` records as a change → re-adds entries the prior `on_entity_destroyed` had just cleared.

Fix: reordered to drain → fan-out's on_entity_destroyed. The `IStorageEventSink::on_entity_destroyed` doc-block was updated to reflect this ("fires last, after the per-component on_remove drain"). All 14 v1f relation tests pass under the new order — they don't rely on the old "before drain" timing.

### Two test failures from invalid expectations

- "Fan-out: registered index receives on_insert/on_update": expected 1 update, got 2. Fix: `archetype_chunk_storage::get_mut` ALSO fires `on_update` (line 407), not just `add_component` UPSERT. Test expectation corrected to 2.
- "AsyncAwareIndex: on_remove drops state": passed in isolation, failed when run with other tests due to my order misdiagnosis — actual failure was the destruction-order bug above ("ChangeDetectIndex on_entity_destroyed clears entries" test, line 341). The reorder fixes both.

### Two unused using-decls (clang-tidy)

`Reflection` and `ReflectionIndex` were imported but only ReplicationIndex was actually exercised. Removed.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 708 / 708 | 17 / 17 |
| win-relwithdebinfo | clean | 708 / 708 | 17 / 17 |
| win-release        | clean | 705 / 705 | 17 / 17 |
| win-asan           | clean | 708 / 708 | 17 / 17 |
| win-clang-cl       | clean | 708 / 708 | 17 / 17 |
| win-tidy           | clean | — | — |

### Scene tests

- Pre-v1i: 172 cases / 34602 assertions.
- Post-v1i: 192 cases / 34669 assertions (+20 cases / +67 assertions).

### LOC

- `component_index.hpp`           ~50
- `change_detect_index.hpp/.cpp`  ~155
- `async_aware_index.hpp/.cpp`    ~190
- `reserved_indexes.hpp`           ~80
- `world.hpp` delta              ~250 (register_index + find_index + auto-register helper + fan-out class + frame counter + Query::changed/skip_pending bodies)
- `world.cpp` delta              ~110 (fan-out impl + frame lifecycle + destruction reorder)
- `query.hpp/.cpp` delta         ~60 (filter type forward decls + entity_passes extensions)
- `storage_event_sink.hpp` delta  ~5 (doc rewrite)
- `test_index_framework.cpp`     ~370
- Total                          ~1270

## What this unlocks

v1j (Transform + propagation system) is now the natural next slice. It consumes v1i directly: `query<Transform>().changed<Transform>()` is the dirty-flag scan that drives propagation. The v1i + v1j combination is what the rest of the engine has been waiting for — TransformPropagation in PreRender, ChangeDetect in v1i, the rendering path's `query<Transform, Renderable>().skip_pending<Renderable>()` in RenderExtract.

Beyond v1j:
- **v1k (SceneResource + SceneLoader)** picks up automatic AsyncAware integration: a streaming loader marks entities Loading on spawn, flips to Loaded when the asset completes. Renderer never sees half-loaded scenes.
- **Phase 3.2 (animation interp)** drops a real `HistoryIndex<N>` impl in; existing components registered with `History{60}` start ring-buffering their last 60 frames. Zero caller code changes.
- **Phase 3.5 (light culling)** drops a real `SpatialBVHIndex` impl in; the placeholder `.in_aabb()` operator (deferred from v1g) starts hitting the BVH.
- **Phase 3.8 (GPU-driven rendering)** drops a real `GpuResidentIndex` impl in; `Renderable` components tagged with `GpuResident{}` get their data CPU-mirrored to a GPU SSBO automatically.
- **Phase 4.2 (networking)** drops a real `ReplicationIndex` impl in; `Replication::ServerAuthoritative` components start emitting delta packets.
- **Phase 7 (editor)** drops a real `ReflectionIndex` impl in; the editor walks `Reflection.fields` for property panels.

Every slot the ADR locked in v1b + v1f + v1i is now wired end-to-end. A new ECS extension shipped post-v1i is a single header + impl + a single `register_index<MyIndex>()` call.

## Follow-ups

None opened. The "per-system change-tracking" + chunk-grain ChangeDetect + ComponentRef-with-auto-bump + jobs-Counter-based parallel `par_each` are all v1h+1 / Phase 3.5 evolutions with their slots already documented.

## Commit message proposal

```
feat(scene): IComponentIndex framework + ChangeDetect + AsyncAware + reserved slots (v1i, ADR-0053)

Phase 3.0 v1i ships Layer 5 — the plug-point that makes new ECS
extensions a one-day job:

  - IComponentIndex interface (extends IStorageEventSink with observed()
    mask, on_frame_begin/end hooks, name()).
  - World::register_index<Idx>(args...) + find_index<Idx>() + IndexFanOutSink.
  - Auto-registration on register_component(traits...): ChangeDetect
    always; AsyncAware/History/SpatialBVH/GpuResident/Replication/
    Reflection lazy-created when their trait flag is set.
  - ChangeDetectIndex (per-entity; HashMap<u64, u32 frame>) + Query
    .changed<T>() operator (filters to current-frame writes).
  - AsyncAwareIndex (per-(entity,component) LoadState) + Query
    .skip_pending<T>() operator (excludes Loading).
  - Five reserved no-op shells (History/SpatialBVH/GpuResident/
    Replication/Reflection) ready for their consumer phases.

set_storage_event_sink coexists with registered indexes. Destruction
order reorganized: backends drain (per-component on_remove) BEFORE the
fan-out fires on_entity_destroyed; storage_event_sink.hpp doc updated.

Six-config DoD: 708/708 (was 688). 17/17 headless smokes per non-tidy
config. 192 scene tests / 34669 assertions (was 172 / 34602).
```
