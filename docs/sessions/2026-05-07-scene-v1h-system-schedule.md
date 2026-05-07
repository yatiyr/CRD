# 2026-05-07 — Phase 3.0 v1h: System + Schedule + Commands

**Status at start:** Phase 3.0 v1g shipped earlier the same day. Query DSL with `world.query<Cs...>()` + filter chain. Scene tests 157 / 34559, six-config 673/673.

**Status at end:** v1h shipped. `ISystem` virtual interface + 7-phase fixed schedule + `Commands` deferred-mutation buffer + `step(dt)` and `step_fixed(dt, fixed_dt, max_substeps)` with accumulator math. Six-config 688/688 / 685 release / 17 smokes. Scene tests 172 / 34602.

---

## Goal of this session

Land Layer 4's scheduling half (ADR-0052 §3-§5):
- `ISystem` virtual class with `Reads` / `Writes` `ComponentSet` aliases.
- Fixed 7-phase `SchedulePhase` enum (PrePhysics → PostRender).
- `World::register_system` / `step(dt)` / `step_fixed(dt, fixed_dt, max_substeps)`.
- `Commands` buffer with thread-local accumulation pattern (single-threaded for v1h) and serial flush at every phase boundary.

Per phase doc: ~300 LOC for impl + tests. Actual landed surface is ~1100 LOC including tests.

## What shipped

### New module files

```
engine/scene/include/crd/scene/system.hpp     ~95 LOC
engine/scene/include/crd/scene/commands.hpp   ~110 LOC
engine/scene/src/commands.cpp                 ~155 LOC
tests/scene/test_schedule.cpp                 ~440 LOC, 15 cases
```

### Modified

- `engine/scene/include/crd/scene/world.hpp`
  - New includes: `<crd/scene/commands.hpp>`, `<crd/scene/system.hpp>`, `<memory>`.
  - New public methods: `register_system(unique_ptr<ISystem>)`, `step(dt)`, `step_fixed(dt, fixed_dt, max_substeps)`, `commands()`, `component_set_mask<Set>()`, `backend_for_public(id)`, `add_relation_via_id(...)`, `remove_relation_via_id(...)` (the last three exposed for `Commands` to drive flush without re-instantiating templates).
  - New private members: `Array<std::unique_ptr<ISystem>> m_systems[kSchedulePhaseCount]`, `Commands m_commands_buffer`, `f64 m_fixed_accumulator`.
  - New inline templated `Commands::add_component<T>` / `remove_component<T>` / `set_component<T>` / `add_relation<Tag>` / `remove_relation<Tag>` bodies at the bottom of the file (need World's templated `id_of<T>` lookup).
  - New inline `World::component_set_mask<Set>()` template + `detail::ComponentSetMaskHelper` recursion helper.

- `engine/scene/src/world.cpp`
  - Constructor initialises `m_commands_buffer(*this)` and the per-phase system arrays with the World allocator.
  - `register_system` pushes onto the right per-phase array.
  - `step(dt)` walks 7 phases × per-phase systems; flushes commands at each phase boundary.
  - `step_fixed(dt, fixed_dt, max_substeps)`: accumulator math + spiral-of-death clamp + per-phase interleaving (N fixed-step passes, then 1 variable-rate pass).

## Design decisions (advisor-driven)

### 1. Variable-vs-fixed interleaving per phase

Each phase runs as: `for substep in [0..N): run_fixed_systems_in_phase(); run_variable_systems_in_phase()`. This matches Bevy's `FixedUpdate` semantics — variable-rate systems see the world AFTER the current phase's fixed substeps have settled. Two-pass alternative (all phases × fixed N times, then all phases × variable once) breaks phase ordering for hybrid systems and was rejected.

Test #14 verifies: 3 fixed substeps + 1 variable run = 4 total trace entries.

### 2. `fixed_dt()` virtual dropped from v1h

ADR-0052 §7 sketches per-system `fixed_dt()` overrides. Shipping it in v1h while the schedule actually uses a global `fixed_dt` (the `step_fixed` argument) would be a footgun: two systems with different `fixed_dt()` returns would silently both use the global. Dropped from v1h's ISystem; v1h+1 (Phase 3.1 physics) restores it along with per-system accumulators when the per-rate use case actually exists.

### 3. ISystem::run signature: `run(World&)` not `run(World&, Counter& fence)`

ADR-0052 §3 specifies `run(World& world, jobs::Counter& fence)`. The current `crd-jobs` API allocates Counters internally and returns `Counter*` — there's no caller-managed counter to pass into `run()`. v1h's serial dispatcher has no fence to wait on (next system starts after previous returns). v1h ships `run(World&)`; v1h+1 (parallel `par_each` over Query chunks) reintroduces the fence parameter once the jobs API exposes a caller-managed counter handle.

Documented in code at the `ISystem::run` declaration.

### 4. Commands::spawn is immediate; mutations are deferred

v1h ships single-threaded. `commands().spawn()` allocates from the SlotMap immediately (race-free in single-threaded land) and returns a real, usable EntityId. All other operations (`add_component`, `destroy`, etc.) queue records and apply on `flush()`.

Asymmetry: a system that runs `spawn()` then `add_component<T>(e, ...)` sees the entity alive in `world.is_alive(e)` immediately, but `world.has_component<T>(e)` returns false until the next flush. This is documented in `Commands`'s class doc-block. The pattern matches Bevy's `Commands::spawn` — most users add the entity's components inside the same scope and don't observe the asymmetry.

When v1h+1 enables `par_each` from worker fibers, spawn will need a deferred-with-placeholder path; the public API stays the same (returns `EntityId`), the impl gets a per-fiber stripe.

### 5. Commands payload buffer with type-erased lifecycle

Each component-typed command stores its payload in a parallel `Array<u8> m_payloads`. Per-command `payload_offset` + `payload_size` index into the buffer; the `ComponentInfo*` (looked up at queue time, redirected by id at flush) provides `move_construct` and `destruct` callbacks.

Lifetime invariants:
- At queue time: payload bytes are reserved at the right alignment, `info->move_construct` runs (or memcpy fallback for trivially-movable types). Source value is left moved-from.
- At flush: storage backend's `insert(entity, id, payload_ptr)` consumes the payload via its move-construct callback. Payload bytes are now garbage; `payload_offset` is set to 0xFFFFFFFFU as a "consumed" marker.
- If entity is dead at flush: `info->destruct(payload_ptr)` runs explicitly (so non-trivially-destructible Ts don't leak).
- At Commands destructor: any remaining unconsumed payloads have `info->destruct` run on them.

Test #14 (`Commands destructor runs payload destructors`) verifies the unflushed-destructor path with a `DtorCounter` type whose constructor/destructor count live instances.

### 6. Mid-frame system registration is unrestricted

A system registered from another system's `run()` lands in the right per-phase array and runs from the next phase the schedule visits. If you register a `PostRender` system from a `Update`-phase system's `run()`, the new system runs later in the same `step()` call. This matches Bevy's behaviour and is verified by test #15 (`SelfRegisteringSystem`).

## Bugs caught during integration

### `crd-scene` doesn't link `crd-jobs`, but world.cpp briefly included `<crd/jobs/jobs.hpp>`

Initial draft of `World::step` constructed a `crd::jobs::Counter` to pass to system `run()` calls. This required linking `crd-jobs` (which `crd-scene` doesn't currently depend on). I added `crd-jobs` to the link list, then realised the Counter API doesn't support caller-allocation — Counters come from `jobs::run()` returning `Counter*`. Backed out: dropped the fence parameter from `ISystem::run`, removed the include, removed the link dependency. v1h is jobs-independent.

Pinned in code comments: v1h+1 will revisit when par_each lands.

### win-tidy: 8 `[[nodiscard]]` warnings on test override methods

`modernize-use-nodiscard` flagged the test `RecordingSystem::phase()` / `name()` / `fixed_step()` overrides because their base-class declarations carry `[[nodiscard]]`. Added the attribute to the overrides.

### win-tidy: 3 unused using-decls

Removed `using crd::scene::Commands` / `ComponentSet` / `StorageHint` — used in earlier drafts but not in the final test code.

### `crd::containers::String` requires `<crd/containers/string.hpp>`

The test file used `crd::containers::String` for trace labels; only `string_view.hpp` was being pulled in transitively. Added the explicit include.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 688 / 688 | 17 / 17 |
| win-relwithdebinfo | clean | 688 / 688 | 17 / 17 |
| win-release        | clean | 685 / 685 | 17 / 17 |
| win-asan           | clean | 688 / 688 | 17 / 17 |
| win-clang-cl       | clean | 688 / 688 | 17 / 17 |
| win-tidy           | clean | — | — |

### Scene tests

- Pre-v1h: 157 cases / 34559 assertions.
- Post-v1h: 172 cases / 34602 assertions (+15 cases / +43 assertions).

### LOC

- `system.hpp`            ~95
- `commands.hpp`          ~110
- `commands.cpp`          ~155
- `world.hpp` delta       ~150 (factory + Commands template bodies + ComponentSet helper + schedule API)
- `world.cpp` delta       ~110
- `test_schedule.cpp`     ~440
- Total                   ~1060

## What this unlocks

v1i (Index framework + ChangeDetect + AsyncAware) is the natural next slice. It composes directly on top of v1h:

- ChangeDetectIndex consumes the per-chunk version counters that v1c2 added; the schedule's command-flush-at-phase-boundary semantics give a clean "frame mark" for the index to compare against.
- AsyncAwareIndex is driven by storage events; the same `IStorageEventSink` we've been threading since v1c2 is its plug point.
- The `IComponentIndex` framework's "register an index, get fan-out" shape mirrors the schedule's "register a system, get phase-ordered dispatch" shape.

Beyond v1i:
- v1h+1 (parallel par_each): wire `Query::for_each_chunk` into a job dispatcher; ISystem::run regains the fence parameter; Commands grows per-fiber stripes.
- v1j (Transform + propagation system): builds on v1f relations + v1g query DSL + v1h schedule. The first concrete `ISystem` consumer.

## Follow-ups

None opened. The deferred items (par_each, ComponentRef, fence parameter, per-system fixed_dt, multi-relation anchor) all have explicit slots — v1h+1 (wherever that lands) will revisit.

## Commit message proposal

```
feat(scene): System + Schedule + Commands (v1h, ADR-0052 §3-§5)

Phase 3.0 v1h ships Layer 4's scheduling half:
  - ISystem virtual class with Reads/Writes ComponentSet aliases.
  - 7-phase SchedulePhase enum (PrePhysics → PostRender).
  - World::register_system / step(dt) / step_fixed(dt, fixed_dt,
    max_substeps).
  - Commands deferred-mutation buffer with thread-local-accumulation
    pattern (single-threaded v1h; per-fiber stripes are v1h+1's job).
  - Phase-boundary command flush; spawn is immediate, mutations deferred.

step_fixed accumulator math handles dt remainders and clamps substeps
under a spiral-of-death cap. Per-phase interleaving runs fixed-step
systems N times then variable-rate systems once (Bevy FixedUpdate
semantics). Mid-frame system registration is allowed.

ISystem::run takes only `World&` for v1h — the ADR's `Counter& fence`
parameter is restored in v1h+1 when par_each lands. Per-system fixed_dt
and ComponentRef-with-auto-bump similarly deferred.

Six-config DoD: 688/688 (was 673). 17/17 headless smokes per non-tidy
config. 172 scene tests / 34602 assertions (was 157 / 34559).
```
