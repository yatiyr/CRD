# Cerid — Live Context

> Engine'in kısa-vadeli hafızası. "Şu an neredeyiz?" sorusuna cevap verir.
> "Master plan ne?" sorusunun cevabı `docs/ROADMAP.md` ve oradan
> dallanan dosyalardadır.
>
> Her session sonu `@docs-keeper` günceller. Kısa kalır. Eski session
> detayları `docs/sessions/YYYY-MM-DD-*.md`'de yaşar, burada değil.

---

## Current focus

**Phase 3.0 — Scene/ECS foundation. v1a + v1b + v1c (whole) + v1d + v1e + v1f shipped 2026-05-06 / 07. Detour D-001 (memory infrastructure) closed 2026-05-07. Allocator-audit Option C closed the make_unique<Archetype> bypass. v1f ships Layer 3 — relations as first-class components-with-target-payload (ADR-0051): six built-in tag types in `crd::scene::relations` (ChildOf / AttachedTo / Owns / Targets / DependsOn / PossessedBy) covering every (storage × acyclic × policy) combination, three opt-in traits (ReverseIndex / Acyclic / OnTargetDestroyed), iterative destruction worklist that handles 500-deep cascade trees without stack overflow. 8 slices remaining. Next: v1g — Query DSL (`world.query<Cs...>().with<>().without<>().with_relation<>().changed<>()` per ADR-0052), built on top of `World::for_each_chunk` + relation reverse indexes.**

The architecture is the **eight-layer slot-shaped ECS** designed for million-entity scenes, agents-as-components-with-scripts, UI on the same machinery (game and editor), with every novel ECS extension as a registered slot:

```
L8 Reflection / Editor          (Phase 7)        — API reserved
L7 Scripting & Behaviors        (Phase 4.0+)     — API reserved
L6 Replication / Networking     (Phase 4.2)      — API reserved
L5 Indexes                       (Phase 3.0+)
   ChangeDetect, AsyncAware                       — ship in 3.0
   History, SpatialBVH, GpuResident               — API only in 3.0
L4 Query · System · Schedule    (Phase 3.0)
L3 Relations                     (Phase 3.0)
L2 Storage backends              (Phase 3.0)
   ArchetypeChunk + SparseSet hybrid
L1 Entity / SlotMap              (Phase 3.0)
L0 Memory · Containers · Jobs    (already shipped)
```

Cerid signature: a uniform `IComponentIndex` extension framework where every novel ECS extension (history, change detect, spatial, GPU-mirror, async, replication, scripts, reflection) is a registered slot consuming the same component-lifecycle event stream. Adding the next extension is a one-day job.

Slices: ~~v1a (Entity+SlotMap)~~ ✅ → ~~v1b (registry)~~ ✅ → ~~v1c1 (chunk allocator + layout)~~ ✅ → ~~v1c2 (graph + entity move + IStorageBackend impl + sink hooks)~~ ✅ → ~~v1d (SparseSet storage)~~ ✅ → ~~v1e (mixed-backend chunk visitor)~~ ✅ → ~~v1f (relations)~~ ✅ → **v1g (query DSL)** ← active → v1h (system+schedule) → v1i (index framework + ChangeDetect + AsyncAware) → v1j (Transform + propagation) → v1k (SceneResource+Loader) → v1l (cook_scene cooker handler) → v1m (sandbox renderer integration) → v1n (reserved-slot freeze).

Active phase doc: `docs/phases/phase-3.0-scene-ecs.md`.

## Previous focus (closed)

**Phase 2.8 — Material GPU wiring + sandbox rendering + asset import. ALL SLICES SHIPPED 2026-05-06. Phase 2.8 COMPLETE.**

Phase 2.8 slices:
- v1a: Per-material pipeline cache in `ForwardRenderPath` (`m_mat_cache`, keyed by material pointer) ✅
- v1b: Multi-pass shader selection — `PipelineResolver::begin_pass()` default impl + `ForwardRenderPath` calls it before each pass ✅
- v1c: Depth-only prepass pipeline (vertex-only, `Format::Undefined` color, `D32Sfloat` depth); `SandboxPipelineResolver` compiles depth + color pipelines lazily; `smoke_depth_prepass.exe` GPU smoke ✅
- v1d: Default lit material shaders — `engine/renderer/shaders/surface.vert`, `surface.frag`, `assets/materials/default_lit.mat.toml`; standard 48B vertex layout in shaders ✅
- v1e: Sandbox rendering wired to `ForwardRenderPath` + `SandboxPipelineResolver`; orbit-camera view+projection; mesh upload on shape selection; blit color RT → swapchain; ImGui overlay on top ✅
- v1f: Demo glTF assets (BoxTextured CC-BY, Duck SCEA, BoomBox CC0) + checker_512/bricks_512 procedural PNGs (CC0) under `assets/source/`; `cook-demo-assets` CMake target produces `assets/cooked/demo_assets.crdr` (5 entries); `LICENSES.md` ✅
- v1g: Unified `Asset Browser` ImGui panel — replaces Meshgen Browser; "Procedural Shapes" + "Imported Assets" collapsing sections; click swaps mesh; `SandboxLayer` mounts the cooked pack via `ResourceManager` and `load_sync<MeshResource>` for imports ✅

Bug fix landed alongside v1f/v1g: device-destroy crash on application close — `Application::detach_all_layers()` was leaving layer instances alive in `m_owned_layers`, so layer destructors (which free GPU resources) ran during `~Application` *after* the `Device` local in `main` had been destroyed. Fixed by clearing `m_owned_layers` inside `detach_all_layers()`, and re-ordering `sandbox/src/main.cpp` so `device->wait_idle()` precedes `app.detach_all_layers()`. Same fix applied to `runtime/examples/smoke_imgui_overlay.cpp`.

RHI additions in this phase: `Format::R32G32B32A32Sfloat` + VkFormat mapping; `Module::code_bytes()` on shader interface + `StoredModule` impl.
Bugfixes: SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT was mapped to R8G8B8A8Unorm — corrected.

Full design packet: `docs/phases/phase-2.8-material-completion.md`.

Aktif phase dosyası: `docs/phases/phase-2.8-material-completion.md` (active)

## Active detour

_none — D-001 closed 2026-05-07. Main roadmap resumed at Phase 3.0 v1d._

> When a detour opens, this section names it (e.g. "D-001: investigate
> shader-cache corruption") and the main roadmap pauses until it closes.
> Detour file: `docs/detours/D-NNN-<slug>.md`. Queue rules:
> `docs/detours/README.md`.

## Last shipped milestone

**2026-05-07 — Phase 3.0 v1f: Relations layer (ADR-0051) with SIX built-in tag types — `ChildOf` / `AttachedTo` / `Owns` / `Targets` / `DependsOn` / `PossessedBy` — covering every (storage × acyclic × policy) combination that occurs in real engine work. Three opt-in traits (`ReverseIndex` / `Acyclic` / `OnTargetDestroyed`) compose orthogonally. Iterative destruction worklist replaces recursive cascade — 500-deep ChildOf trees never overflow the stack. Six-config green at 651/651 / 648 release / 17 smokes (was 629 baseline post-v1e).**

### v1f: Relations as first-class

Six built-in relation tag types in `crd::scene::relations` namespace, each with the canonical default trait combination registered by `World::register_builtin_relations()`:

| Tag           | Storage    | Acyclic | OnTargetDestroyed | Use case |
|---------------|------------|---------|-------------------|----------|
| `ChildOf`     | Archetype  | yes     | Cascade           | Scene tree, UI tree, prefab, replication scope |
| `AttachedTo`  | Archetype  | yes     | Detach            | Sockets (weapons, decals, audio sources) |
| `Owns`        | Archetype  | yes     | Cascade           | Lifetime ownership (effects own particles) |
| `Targets`     | SparseSet  | no      | SetNull           | AI lock-on, missile tracking, camera focus |
| `DependsOn`   | SparseSet  | yes     | SetNull           | Asset deps, system order, animation graph |
| `PossessedBy` | SparseSet  | no      | Detach            | Input/AI/script control link |

```cpp
World w;
w.register_builtin_relations();             // registers all six with canonical defaults
EntityId child = w.spawn();
EntityId parent = w.spawn();
w.add_relation<relations::ChildOf>(child, parent);
w.traverse_relation<relations::ChildOf>(parent, [](EntityId e, u32 depth) { ... });
w.destroy_immediate(parent);                 // cascades — child also destroyed
```

### Architecture decisions pinned

- **UPSERT short-circuit** when `old_target == target` prevents spurious storage events (v1i ChangeDetect would otherwise see a real change).
- **Iterative destruction worklist**: cascade pushes affected sources onto a stack-local Array; diamond shapes are deduped by the alive-check at each iteration.
- **`OnTargetDestroyed` requires `ReverseIndex`** (asserted at registration) — without it, "find every source pointing at the dying target" is an O(N) full scan; v1g+ may relax.
- **`would_form_cycle<Tag>(src, target)` is the public predicate** for cycle detection; tests verify it directly so they never trip the internal `CRD_ASSERT`.
- **Built-in registration is opt-in** via `register_builtin_relations()` — v1k SceneLoader will call it before SCEN deserialisation.

### Six-configuration green (post-v1f, 2026-05-07)

- win-debug:          651/651
- win-relwithdebinfo: 651/651
- win-release:        648/648
- win-asan:           651/651
- win-clang-cl:       651/651
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. Scene tests: 135 cases / 34520 assertions (was 113 / 34450 post-v1e).

Session log: `docs/sessions/2026-05-07-scene-v1f-relations.md`.

### Earlier the same day: Phase 3.0 v1e

`World::for_each_chunk(required, fn, ud)` mixed-backend chunk visitor (ADR-0050 §5). Splits `required` by `StorageHint`; pure-archetype and pure-SparseSet single-bit forward to the per-backend method (zero overhead); pure-sparse multi-bit anchors on the smallest pool then sparse-checks the rest; mixed walks archetypes ⊇ archetype_bits and sparse-checks remaining sparse_bits per entity, yielding filtered chunks into stack-local scratch. Six-config baseline 629/629.

### v1e: mixed-backend chunk visitor

```cpp
// On World — the unified iteration primitive that v1g query DSL sits on top of.
void World::for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data);
```

Algorithm:
1. Split `required` into `archetype_bits` and `sparse_bits` by walking the registry's `ComponentInfo::storage_hint`.
2. **Pure-archetype path** (`sparse_bits == 0`): forward to `m_storage.for_each_chunk(required)`. Empty `required` additionally forwards to `m_sparse_storage.for_each_chunk(required)` so visitors see all chunks of either backend.
3. **Pure-SparseSet single-bit path**: forward to `m_sparse_storage.for_each_chunk(required)`.
4. **Pure-SparseSet multi-bit path**: pick the smallest pool as anchor; if any named pool is missing or empty, intersection is empty (yield nothing). Otherwise walk anchor entities, sparse-check every other bit, build filtered scratch, yield one ChunkView.
5. **Mixed path**: walk archetypes whose `mask ⊇ archetype_bits`. For each chunk, sparse-check every `sparse_bit` per entity slot. Build filtered scratch per chunk, yield ChunkView.

ChunkView semantic: forwarded chunks carry `present_mask = arch.mask` (a superset of `required`); constructed chunks carry `present_mask = required` (exact). Visitors should treat `present_mask` as ≥ `required`. Filtered `entities` lifetime is the visitor call only — re-call clobbers scratch.

Anchor selection rationales (pinned for v1g):
- Mixed: archetype-as-anchor is the cache-coherent default. Sparse-as-anchor when the smallest sparse pool dwarfs the archetype's chunks is a future profile-driven optimisation.
- Pure-sparse multi-bit: smallest-pool-as-anchor minimises probes per entity; no cache-coherent path exists for pure-sparse intersection.

### Six-configuration green (post-v1e, 2026-05-07)

- win-debug:          629/629
- win-relwithdebinfo: 629/629
- win-release:        626/626
- win-asan:           629/629
- win-clang-cl:       629/629
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. Scene tests: 113 cases / 34450 assertions (was 102 / 34420 post-v1d).

Session log: `docs/sessions/2026-05-07-scene-v1e-mixed-visitor.md`.

### Earlier the same day: Phase 3.0 v1d

`SparseSetStorage` (the L2 escape-hatch backend per ADR-0050 §3) + World dispatch by `StorageHint` + consolidated `on_entity_destroyed` fan-out. Per-pool version counter pre-wired so v1i ChangeDetect doesn't have to retrofit. Six-config baseline 618/618.

### v1d: `SparseSetStorage`

Second L2 backend ships behind the same `IStorageBackend` interface as `ArchetypeChunkStorage`. One pool per registered SparseSet component:

```
sparse[entity.index()] -> dense_index | kInvalid    (lazy-grown crd::containers::Array<u32>)
dense (raw bytes)      -> T                          (count*info.size, info.alignment, exp ×2 grow)
entities[dense_index]  -> EntityId                   (parallel back-resolution array)
+ pool.version : u64                                 (bumped on insert/update/remove — pool-grain ChangeDetect)
```

O(1) insert (free-list-free, append-only on dense), O(1) remove (swap-with-last), O(1) lookup. Pools are allocated lazily through the World's `IAllocator` and live the World's lifetime. Pool count is bounded by `kMaxComponents` (256) so direct allocation is the right shape (no GrowablePool — ≤256 long-lived structs).

UPSERT via `insert(e, c, data)` when c already present → destruct + move-construct + `on_update` (matches archetype precedent at `archetype_chunk_storage.cpp:279`).

`for_each_chunk(required, fn, ud)` semantics:
- empty `required` → yield every non-empty pool
- single-bit `required` → yield exactly the matching pool
- multi-bit `required` → yield nothing (deferred to v1e mixed-backend visitor — a single SparseSet pool can't satisfy multi-bit AND alone)

### World dispatch by `StorageHint`

`World` grows a second member `SparseSetStorage m_sparse_storage` and dispatches `add_component` / `has_component` / `get_component` / `get_component_mut` / `remove_component` to either backend based on `ComponentInfo::storage_hint`:

```cpp
world.register_component<Position>();                              // -> ArchetypeChunkStorage
world.register_component<DialogTrigger>(StorageHint::SparseSet);   // -> SparseSetStorage
```

`world.storage()` keeps returning the archetype storage (primary backend). New `world.sparse_storage()` accesses the sparse backend for diagnostics / tests.

### Sink fan-out consolidated through World

`IStorageEventSink::on_entity_destroyed` now fires from `World` (once per destroy), not from each backend. Both backends drain their own components and emit per-component `on_remove` events through the same sink. Contract change: archetype storage's `on_entity_destroyed` no longer fires `sink->on_entity_destroyed` itself — that line is removed in `archetype_chunk_storage.cpp`.

### Allocator chain (closes the audit's promise)

Every byte the SparseSetStorage holds — sparse table, dense buffer, entities array, the Pool struct itself — flows through `m_alloc`. World on TLSF means SparseSet on TLSF too. `test_world_tlsf.cpp` extended: the lifecycle case now registers a `DialogTrigger` (SparseSet) component alongside Position/Velocity/Health, and asserts `sparse_storage().pool_count() == 1U` and `entity_count(...) == 40U`.

### Six-configuration green (post-v1d)

- win-debug:          618/618
- win-relwithdebinfo: 618/618
- win-release:        615/615
- win-asan:           618/618
- win-clang-cl:       618/618
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. Scene tests: 102 cases / 34420 assertions (was 87 / 11024).

Session log: `docs/sessions/2026-05-07-scene-v1d-sparseset.md`.

### Earlier the same day: allocator-audit Option C

`ArchetypeGraph` pools `Archetype` structs via `GrowablePoolAllocator` (parent = World allocator) — closes the `std::make_unique<Archetype>` bypass. `test_world_tlsf.cpp` (5 cases) proves end-to-end that a `World` on a `TlsfAllocator` pool runs the full ECS lifecycle. Six-config baseline 603/603.

### Option C: archetype pool + TLSF-backed World test

Three layers:

- **Layer 1** — `ArchetypeGraph` no longer uses `std::make_unique<Archetype>`. Storage changed from `Array<unique_ptr<Archetype>>` to `Array<Archetype*>` with manual `pool.allocate()` + placement-new.
- **Layer 2** — Archetype structs come from a dedicated `GrowablePoolAllocator(slot_size = sizeof(Archetype), slot_alignment = alignof(Archetype), slots_per_page = 32, parent = m_alloc)`. One parent allocate amortises 32 archetypes; 1000 archetypes ≈ 32 pages.
- **Layer 3** — `tests/scene/test_world_tlsf.cpp`: 5 cases construct a `World` on a 16 MB `TlsfAllocator` pool and run register / spawn / add / get / get_mut / remove / destroy / chunk-fill-spill (1500 entities) / destruction-returns-bytes-to-pool. ASan-clean.

Every byte the `World` allocates now flows through one root `IAllocator`: SlotMap, pending-destroy queue, ComponentRegistry's Array + HashMap, ArchetypeGraph's Array + HashMap, the per-archetype edge tables, the EntityLocation array, **the Archetype structs themselves (via the graph's GrowablePool whose parent is the World allocator)**, and the ArchetypeChunkStorage's 16 KB chunks (via ChunkAllocator's GrowablePool whose parent is also the World allocator).

Scene tests: 87 / 11024 (was 82 / 7012).
Session log: `docs/sessions/2026-05-07-archetype-pool-tlsf-world.md`.

### Earlier the same day: Detour D-001 CLOSED

**`TlsfAllocator` (production-grade, arbitrary alignment, try-allocate) + `GrowablePoolAllocator` + `ChunkAllocator` refactored to wrap the pool. v1c1 O(N) free perf debt closed.**

### D-001-b: `GrowablePoolAllocator` + ChunkAllocator refactor

`crd::memory::GrowablePoolAllocator` ships: auto-growing pages of fixed-size aligned blocks. O(1) `allocate` (free-list pop) and `deallocate` (free-list push). When the free list is empty, allocates a new page from the parent containing `slots_per_page` contiguous slots and links them into the free list. Pages are kept allocated for the life of the allocator (no auto-shrink); repeated alloc/free cycles do not thrash the parent. `owns()` is O(pages) — pages are typically few (logarithmic in allocations). Tests cover construction, page growth, free-list reuse, alignment up to 64, owns() across pages, allocation_size, move semantics, ASan leak check, and a 2000-iteration random alloc/free stress.

`crd::scene::ChunkAllocator` refactored to wrap `GrowablePoolAllocator(slot_size = 16 KB, slot_alignment = 64, slots_per_page = 64)` (= 1 MB pages). Public API unchanged. v1c2 archetype storage tests (82/82 / 7012 assertions) pass unchanged. **Closes the v1c1 perf debt:** `ChunkAllocator::free` is now O(1) instead of O(outstanding).

### Six-configuration green (post-Option-C close, 2026-05-07)

- win-debug:          603/603
- win-relwithdebinfo: 603/603
- win-release:        600/600
- win-asan:           603/603
- win-clang-cl:       603/603
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. (D-001 close baseline: 598/598; +5 cases for `test_world_tlsf.cpp`.)

### Memory subsystem state after D-001 close

| Allocator | Use case | Complexity |
|---|---|---|
| `MallocAllocator` | engine default; libc fallback | malloc-roundtrip |
| `LinearAllocator` | per-frame scratch; reset wipes everything | O(1) alloc, no per-item free |
| `StackAllocator` | nested LIFO scratch (recursive parsing) | O(1) alloc, marker-based unwind |
| `PoolAllocator` | fixed-slot-count object pools | O(1) alloc / free |
| `GrowablePoolAllocator` (NEW) | growable pool of fixed-size aligned blocks (chunks, particle records, ...) | O(1) alloc / free, page-pooled |
| `TlsfAllocator` (NEW) | general-purpose real-time heap; arbitrary alignment | O(1) alloc / free / coalesce |

`MallocAllocator` remains the engine default. TLSF and GrowablePool are opt-in `IAllocator*` consumers choose. Three deferred enhancements documented in `docs/debt.md` (Conte 8-byte trick, 32-bit support, multi-threaded TLSF — none are real Cerid v1 limitations).

### Previous: D-001-a `TlsfAllocator` (also 2026-05-07)

Canonical Masmano/Conte TLSF — O(1) allocate / deallocate / coalesce with bounded internal fragmentation. The general-purpose real-time allocator promised in CLAUDE.md since Phase 1. Ships as opt-in `IAllocator*` consumer; **`MallocAllocator` remains the engine default** (the cutover is a separate change).

### Capabilities (shipped)

- Pool sizes: up to 4 GB (`kFlIndexMax = 32`), three-sentinel layout, ~7 KB metadata.
- Alignment: **arbitrary power-of-two**, tested 16/32/64/128/256 under ASan stress.
- O(1) `allocate` / `deallocate` / coalesce via `std::countr_zero` bitmap searches.
- `reallocate` with in-place grow/shrink + alloc-copy fallback.
- **Non-throwing `try_allocate`** path returns nullptr on OOM (the `IAllocator::allocate` override wraps with `CRD_FATAL` on null).
- Owning + non-owning constructors.
- `MemoryStats` integration (debug-only counters).

### Three ASan-caught bugs fixed

1. **Off-by-16 in `init_pool`** — pool has 3 block headers (start_sentinel + free_block + end_sentinel), not 2. Free block sized `capacity - 3 × 16`, not `capacity - 2 × 16`.
2. **Insufficient `requested` size for alignment-shift** — Conte's algorithm needs `adjusted + alignment + gap_minimum` (where `gap_minimum = sizeof(BlockHeader) = 32`) to cover the worst case where the leading-gap is too small and gets advanced past `alignment`. Old code requested only `adjusted + alignment` and corrupted memory under stress.
3. **Missing `block_set_prev_used(after_new)` in leading-split** — after carving a leading remainder out of a free block, the block immediately after the new user-block had stale `kPrevFreeBit` (set when the original block was free; predecessor is now in-use). Without clearing, a later deallocate followed `prev_phys_block` to an absorbed (garbage) address. Pinned by `CRD_ASSERT(!block_prev_is_free(after_new))` in `trim_free_leading`.

### Documented as `docs/debt.md`

Three out-of-scope items consciously deferred (none are real v1 limitations for Cerid's actual platforms):
- **Thread-safety**: `IAllocator` documents project-wide convention "not thread-safe by default; per-thread arenas". Architectural decision, not specific to TLSF.
- **32-bit support**: Cerid CI is 64-bit. No 32-bit consumer or roadmap target. Adding parameterization is bug surface for zero benefit.
- **Conte's 8-byte overhead trick**: marginal optimization (saves 8 B per allocation), high-risk layout change. Defer.

### Final numbers

21 unit tests / 2208 assertions including a 1000-iteration mixed-alignment stress (16/32/64/128/256) under ASan. Six configs green: win-debug 584/584, win-relwithdebinfo 584/584, win-release 581/581, win-asan 584/584, win-clang-cl 584/584, win-tidy clean. 17/17 headless smokes per non-tidy config.

Detour D-001-b is next: `GrowablePoolAllocator` + `ChunkAllocator` refactor. After D-001-b closes, the detour ends and Phase 3.0 v1d resumes.

## Previous shipped milestone

**2026-05-07 — Phase 3.0 v1c2 SHIPPED: `ArchetypeChunkStorage : IStorageBackend` + memoised archetype graph + bytewise entity move + typed `World::add_component<T>` + `IStorageEventSink` lifecycle hooks (the Cerid L5 plug point).**

This is the architectural moment Phase 3.0 was building toward — entity-component bindings, archetype graph, and the lifecycle hook every L5 IComponentIndex (ChangeDetect, AsyncAware, History, SpatialBVH, GpuResident, Replication, Reflection, Scripts) will plug into. v1d–v1n now have something to consume.

### New public API (`engine/scene/`)

- `crd::scene::ArchetypeId` — 32-bit per-World identifier with null sentinel `0xFFFFFFFFU`. Stable for the life of the World; ids never recycle.
- `crd::scene::EntityLocation` — 16 bytes: `archetype_id`, `chunk_index`, `slot_in_chunk`. Indexed by `EntityId::index()` in the storage's `m_locations` array. Lazy-resized.
- `crd::scene::Archetype` — owns `ChunkLayout` (computed once), `Array<Chunk> chunks` (dense, append-only growth), and two dense edge tables (`add_edges` and `remove_edges`, each sized `kMaxComponents`, indexed by `ComponentId.raw`). 1 KB of edge metadata per archetype guarantees O(1) add/remove navigation with zero hashing on the hot path.
- `crd::scene::ArchetypeGraph` — `archetype_for(mask)` (O(1) avg via `HashMap<ComponentMask, ArchetypeId>` + a 256-bit boost-style hash), `after_add(arch, c)` and `after_remove(arch, c)` (memoised on first lookup, pointer-identity stable across rehashes thanks to `Array<unique_ptr<Archetype>>`).
- `crd::scene::IStorageEventSink` (the Cerid signature at L5) — virtual `on_insert` / `on_update` / `on_remove` / `on_entity_destroyed`. Default impl `NullStorageEventSink` (singleton) is wired by every storage at construction; v1i swaps in the `IComponentIndex` fan-out without touching storage call sites or any user code.
- `crd::scene::ArchetypeChunkStorage : IStorageBackend` — the primary L2 backend. Owns one `ChunkAllocator`, one `ArchetypeGraph`, one `m_locations` array. Implements `insert` (UPSERT semantics — second insert overwrites), `remove`, `has`, `get_mut` (declared write — bumps the chunk's per-component layout-local version counter), `for_each_chunk(required, visitor, user_data)` (walks **superset archetypes** — every archetype whose mask is a superset of `required`), `on_entity_destroyed` (tears down all components, fires per-component `on_remove` events to the sink, then clears the location). Plus a non-IStorageBackend `get_const` accessor that does NOT bump the version counter.
- `World` extended with typed templates: `add_component<T>(e, value)`, `has_component<T>(e)`, `get_component<T>(e)`, `get_component_mut<T>(e)`, `remove_component<T>(e)`. `add_component<T>` requires `T` to be registered. `get_component_mut` returns a pointer; the version bump happened on entry. World now owns the `ArchetypeChunkStorage`. World is non-movable (storage references World's registry).

### Behaviour pins documented in v1c2 session log

1. **Entity move is bytewise.** Components shared between source and destination archetypes are moved via the lifecycle ops captured in the registry (move_construct + destruct), with a memcpy fallback. Storage never sees concrete C++ types.
2. **`insert` is UPSERT.** Calling `add_component<Transform>(e, t1); add_component<Transform>(e, t2);` leaves t2 visible. The first registration's traits are canonical (already pinned in v1b).
3. **`for_each_chunk` walks supersets.** `required = {Position}` matches every archetype whose mask is `⊇ {Position}` — including `(Position, Velocity)` and `(Position, Renderable)`. The visitor sees `ChunkView::present_mask` set to the actual archetype mask so it knows what's beyond the required set.
4. **Edge tables are dense `Array<ArchetypeId>` indexed by `ComponentId.raw`.** 1 KB/archetype, zero allocation on the hot navigation path. Considered HashMap; rejected — at the per-mutation cost, the indexed-load is faster.
5. **Trailing-chunk-only free.** When an entity leaves a chunk and the chunk drops to 0 entities AND it's the last chunk in the archetype, the chunk is freed back to the allocator. Mid-archetype empty chunks are kept (matches Bevy — avoids fragmentation under steady-state churn).
6. **Last-entity swap-remove updates the swapped entity's location.** The release path patches `m_locations[swapped.index()].slot_in_chunk` after every byte-move.
7. **`IStorageBackend::insert` takes `void* data`, not `const void*`.** Honest API contract: storage may move-from the source. This was an ADR-0050 §1 amendment alongside v1b's `void* user_data` addition; the ADR text now matches.
8. **Bytes after `destruct` are uninitialised.** Move paths must not read source slot bytes after destructing. Documented inline in the move helper.

### Tests added (`tests/scene/test_archetype_graph.cpp` + `test_archetype_storage.cpp`, +27 cases / +1345 assertions)

Graph (10):
- Fresh graph empty; `by_id(null)` returns nullptr.
- `archetype_for(empty mask)` returns the empty archetype.
- `archetype_for` is memoised; same mask → same archetype reference.
- Different masks produce different archetypes.
- `after_add` navigates A → A∪{C}; new archetype gets the union mask.
- `after_add` of an existing component is idempotent.
- `after_add` edge memoisation: cached on first lookup; reverse `remove_edges` primed simultaneously.
- `after_remove` navigates A → A\{C}.
- `after_remove` of an absent component is idempotent.
- `Archetype::add_edges` / `remove_edges` sized to `kMaxComponents`, all entries default to `null` until populated.

Storage end-to-end (14):
- Empty World: no archetypes, location is invalid for any entity, has_component returns false.
- `add_component<T>` creates archetype, places entity in chunk 0 slot 0, value populated.
- `get_component_mut` returns a pointer; writes round-trip.
- `add_component` is upsert — second add overwrites; no new archetype.
- Adding a second component moves entity to a new archetype; both components survive.
- `remove_component` moves entity to (mask & ~T).
- Removing the last component clears `EntityLocation` (entity has no archetype-stored components).
- Two entities in the same archetype get distinct slots.
- swap_remove updates the trailing entity's location after a middle removal.
- Chunk fill+spill: 1500 entities allocate ≥ 2 chunks for the archetype.
- `destroy_immediate` clears storage location and tears down all components.
- `flush_destroys` tears down components for queued entities.
- `for_each_chunk` visits SUPERSET archetypes only (verified via three entities split across {Pos}, {Pos, Vel}, {Vel} — required {Pos} matches first two only).

Version counter (1):
- Bumps on insert; bumps on get_mut; does NOT bump on get_const.

Storage event sink (3):
- Default storage uses `NullStorageEventSink` (no-op).
- `CountingSink` records insert / update / remove / destroyed counts across full lifecycle.
- Sink receives `on_entity_destroyed` exactly once + per-component `on_remove` events (2 components → 2 on_remove + 1 on_destroyed).

### Six-configuration green

- win-debug:          563/563  (was 536, +27 new)
- win-relwithdebinfo: 563/563
- win-release:        560/560  (was 533, +27 new)
- win-asan:           563/563  (DLL PATH fix applied; clean rebuild needed first time due to stale objs from v1c1's smaller World)
- win-clang-cl:       563/563
- win-tidy:           ✅ build clean

All 17 headless smokes pass on every non-tidy config.

## Previous shipped milestone

**2026-05-07 — Phase 3.0 v1c1 SHIPPED: archetype chunk allocator + SoA layout + per-chunk version-counter array.**

v1c was split into v1c1 (chunk machinery) and v1c2 (archetype graph + entity move) per phase doc allowance, so reviewable surface stays tight and v1c2 doesn't refactor anything in v1c1.

New public API in `engine/scene/`:
- `kChunkSize = 16 KB`, `kChunkAlignment = 64`, `kMaxComponentsPerArchetype = 32` (per-archetype cap, sized in advance for the in-chunk version-counter array — independent of per-World `kMaxComponents = 256`).
- `crd::scene::ChunkHeader` — at byte 0 of every 16 KB chunk: `entity_count`, `entity_capacity`, `archetype_id` back-ref (populated by Archetype in v1c2), `version_counter[kMaxComponentsPerArchetype]` indexed by *layout-local* component index, not global `ComponentId`. ChangeDetectIndex (v1i) reads these.
- `crd::scene::ChunkLayout` — once-computed plan: `components_sorted` (canonical archetype identity, ascending `ComponentId`), `sizes`, `alignments`, `offsets`, `entity_id_offset`, `entity_capacity`. `is_valid()` returns `entity_capacity > 0`. Failure paths (component count > `kMaxComponentsPerArchetype`, or one entity does not fit a chunk) leave `entity_capacity == 0`; v1c2 decides whether to reject at registration or `CRD_FATAL`.
- `crd::scene::compute_chunk_layout(mask, registry, alloc)` — emits a `ChunkLayout`. Walks `ComponentId` ascending, pulls size/alignment from registry, computes capacity by trying a conservative initial estimate then decrementing until the layout fits inside `kChunkSize`.
- `crd::scene::Chunk` — a dumb `void* memory` + accessors (`header()`, `entity_id_array(layout)`, `component_array(layout, layout_index)`). No entity ops; those land on Archetype in v1c2 because they need the entity-location array too.
- `crd::scene::ChunkAllocator` — owns the lifetime of every chunk it hands out. `allocate()` returns a 64-byte aligned 16 KB block with the header zero-initialised; `free(chunk)` returns memory to the underlying `IAllocator` and clears the chunk's pointer; dtor frees outstanding chunks (verified under win-asan). Aligned-malloc-per-chunk in v1c1; a heap-pooled backing allocator can land later without API changes.

Design choices documented under "Design choices made and why" in the v1c1 session log:
- Per-archetype cap (32) is independent of per-World cap (256). Caps the per-chunk version-counter array at 256 B; archetypes wider than 32 components are rejected at layout time.
- `version_counter` is layout-local indexed (0..K-1 where K = this archetype's component count), not global `ComponentId`. Saves ~1.7 KB per chunk vs the original sketch.
- `Chunk` is a dumb 16 KB block — no `add_entity` / `remove_entity` / `component_array` methods. Those live on `Archetype` in v1c2 because they require both the layout *and* the entity-location array. Keeps v1c1 reviewable and v1c2 refactor-free.
- `compute_chunk_layout` returns an invalid layout (entity_capacity == 0) on failure rather than `CRD_FATAL` — caller decides recovery (v1c2 `Archetype` will reject at registration).

Tests added (`tests/scene/test_archetype_chunk.cpp`, +11 cases, +88 assertions):
- Empty-mask layout (only EntityId array; capacity bounded purely by EntityId array).
- Single-component layout with 64-byte aligned offsets; total fits 16 KB.
- Two-component layout: both arrays aligned, total fits, component count ≤ cap.
- Sorted-by-`ComponentId` regardless of mask order (set bits in reverse); offsets monotonically increasing.
- Layout invalid when component count exceeds `kMaxComponentsPerArchetype` (registers 33 distinct types via `TestSlot<N>` template).
- `ChunkAllocator::allocate` → 64-byte aligned memory + header zeroed (entity_count, entity_capacity, archetype_id, all version counters).
- Multiple chunks have distinct memory.
- `ChunkAllocator::free` clears chunk pointer + decrements outstanding count.
- ASan leak check: dtor frees outstanding chunks (3 allocated, 0 freed → no leak under asan).
- `Chunk::header` / `entity_id_array` / `component_array` return correct pointers (offset arithmetic verified against `chunk.memory`).
- `static_assert` that `version_counter` array is sized exactly `kMaxComponentsPerArchetype × u64`.

Six-configuration green:
- win-debug:          536/536  (was 525, +11 new)
- win-relwithdebinfo: 536/536
- win-release:        533/533  (was 522, +11 new)
- win-asan:           536/536
- win-clang-cl:       536/536
- win-tidy:           ✅ build clean

All 17 headless smokes pass on every non-tidy config.

## Previous shipped milestone

**2026-05-07 — Phase 3.0 v1b SHIPPED: `ComponentRegistry` + `IStorageBackend` interface + storage-hint registration grammar.**

New public API in `engine/scene/`:
- `crd::scene::ComponentId` — 16-bit per-World identity, default-zero is null (`raw == 0xFFFF`), monotonic from registration order.
- `crd::scene::ComponentMask` — 256-bit bitset (`std::array<u64, 4>`) with set/test/clear/AND/OR/popcount; supports up to 256 components, single cache line. Eliminates the v1c-archetype-truncation landmine the original 64-bit mask would have created.
- `crd::scene::StorageHint` — enum (`Archetype` default / `SparseSet`).
- `crd::scene::Replication` — enum (`Local` / `ServerAuthoritative` / `ClientPredicted` / `Remote`); stored, honoured by Phase 4.2.
- Trait markers — `AsyncAware`, `SpatialBVH`, `GpuResident` (empty), `History{u8 window}`, `ComponentSerialize{...}` (4 callbacks all defaulted), `Reflection{display_name, fields}`.
- `crd::scene::ComponentInfo` — id, name (StringView into typeid name), size, alignment, storage_hint, trait flags, replication, serialize record, reflection record, type-erased lifecycle ops (default_construct / destruct / move_construct, captured via `if constexpr`).
- `crd::scene::ComponentRegistry` — owns `Array<ComponentInfo>` + `HashMap<const void*, ComponentId>`. `register_type<T>(traits...)` variadic template, `info(id)`, `id_of<T>()`. Idempotent re-registration: registering same T twice returns the first ComponentId; second-call traits are ignored. kMaxComponents = 256, enforced via CRD_ASSERT.
- `crd::scene::IStorageBackend` interface — declared only (Archetype impl in v1c, SparseSet in v1d). `insert` / `remove` / `has` / `get_mut` / `for_each_chunk(mask, visitor, user_data)` / `on_entity_destroyed`.
- `crd::scene::ChunkView` + `ChunkVisitor` — abstract chunk view (entities pointer + count + present mask); per-component pointer tables populated by backends in v1c–v1d.
- `World` extended: `register_component<T>(traits...)`, `component_info(id)`, `component_id<T>()`, `registered_component_count()`, `components()` (registry accessor).

Type-identity strategy: per-`T` static tag (`ComponentTypeTag<T>::value`) keyed by address — no RTTI on hot path, ODR-safe via C++17 inline static. typeid().name() is still used for the debug `ComponentInfo::name` (StringView, no allocation).

Tests added (`tests/scene/test_component_registry.cpp`, +22 cases / +131 assertions):
- ComponentId default null + equality.
- Fresh registry empty; info(null/zero) returns nullptr.
- First registration produces non-null id; size/alignment/name round-trip.
- Multiple registrations get distinct, monotonic ids (0, 1, 2, ...).
- Idempotent re-registration: same id, first-call traits win, second-call ignored.
- id_of<T>() null for unregistered T.
- Default StorageHint is Archetype; explicit SparseSet stored.
- All four index trait markers (AsyncAware, SpatialBVH, GpuResident, History{N}) round-trip through ComponentInfo.
- Replication enum stored; default Local.
- Reflection record stub round-trips display_name.
- Lifecycle ops captured for default-constructible types; verified by exercising default_construct + destruct on a raw byte buffer.
- Tag-only (empty struct) component registers cleanly.
- Non-default-constructible type leaves default_construct null while destruct/move are populated.
- ComponentMask: default empty, set/test/clear, popcount across word boundaries (0/63/64/255), AND/OR.
- IStorageBackend stub class verifies polymorphic dispatch and virtual destructor; `static_assert(std::has_virtual_destructor_v<IStorageBackend>)`.
- `World::register_component` proxies through; `World::component_id<T>()` round-trips.
- `World::register_component` is idempotent.

Six-configuration green:
- win-debug:          525/525  (was 503, +22 new)
- win-relwithdebinfo: 525/525
- win-release:        522/522  (was 500, +22 new)
- win-asan:           525/525
- win-clang-cl:       525/525
- win-tidy:           ✅ build clean

All 17 headless smokes pass on every non-tidy config.

## Previous shipped milestone

**2026-05-06 — Phase 3.0 v1a SHIPPED: `crd-scene` module bootstrapped (EntityId + SlotMap + World shell).**

New module `engine/scene/` (`crd-scene`), depends on `crd-core` + `crd-containers` + `crd-memory`.

API surface (per ADR-0049):
- `crd::scene::EntityId` — 64-bit `[generation:32 | index:32]`, trivially copyable, default-zero is `null()`. `index()`, `generation()`, `is_null()`, `make(idx, gen)`, `null()`. `static_assert(sizeof == 8)`.
- `crd::scene::Slot` — `{ u32 generation; u32 next_free; bool alive; }`. Slot 0 reserved permanently as null sentinel.
- `crd::scene::SlotMap` — owns `Array<Slot>` + free-list head + alive count. `allocate()` (O(1)), `free()` (O(1), bumps generation), `is_alive()`, `alive_count()`, `slot_count()`, `begin()`/`end()` iterator that yields alive entities only and skips holes.
- `crd::scene::World` — wraps `SlotMap` + `Array<EntityId> m_pending_destroy`. `spawn()`, `destroy(e)` (deferred), `destroy_immediate(e)` (synchronous), `flush_destroys()` (drain queue, skip stale handles), `is_alive()`, `entity_count()`, `pending_destroy_count()`, range-for over alive entities.

Two minor divergences from ADR-0049 (defensible, called out for the next reader):
- §5 says `CRD_VERIFY` traps on generation overflow; impl instead silently bumps `0 → 1` in `SlotMap::free` to keep generation 0 reserved as the dead-slot sentinel value. The alive bit prevents handle resurrection within the same frame anyway.
- §4 commentary says `actually_free_slot` asserts on stale handle; impl makes `World::destroy_immediate(e)` lenient (no-op when `!is_alive(e)`) so double-destroy across the deferred queue + immediate path is safe. Pinned by test `destroy_immediate of stale handle is a no-op`.

Tests added (`tests/scene/`, +22 cases, +3448 assertions):
- `test_entity.cpp` (5): default null, `make` round-trip across u32 boundaries, equality/inequality, `is_null` semantics, trivial-copy + 8-byte size invariants.
- `test_slot_map.cpp` (9): fresh map empty, allocate-never-zero, generation collision after free, free-list LIFO reuse, multi-step alloc/free order, mixed alloc/free stress (1000 ops, deterministic seed), iterator skips holes, slot-0 sentinel never alive, out-of-range index dead.
- `test_world.cpp` (8): empty world, spawn/alive, destroy-is-deferred, flush drains all queued, `destroy_immediate` synchronous, lenient stale `destroy_immediate`, double-destroy across flush, iteration after destroy/flush.

Six-configuration green:
- win-debug:          503/503  (was 481, +22 new)
- win-relwithdebinfo: 503/503
- win-release:        500/500  (was 478, +22 new)
- win-asan:           503/503  (DLL PATH fix applied as documented)
- win-clang-cl:       503/503
- win-tidy:           ✅ build clean (no clang-tidy warnings or errors)

All 17 headless smokes pass on every non-tidy config.

## Previous shipped milestone

**2026-05-06 — Phase 3.0 architecture locked: nine ADRs accepted, 14 slices planned.**

ADRs accepted (in `Accepted` status under `docs/decisions/`):
- ADR-0049 — L1 Entity identity & SlotMap (32:32 EntityId, dense slot map, deferred destroy)
- ADR-0050 — L2 Storage backends (ArchetypeChunk + SparseSet hybrid behind `IStorageBackend`)
- ADR-0051 — L3 Relations as first-class (`Relation<Tag>` with ChildOf/AttachedTo/generic)
- ADR-0052 — L4 Query · System · Schedule (composable DSL, ISystem with Reads/Writes, phase scheduler, command buffers)
- ADR-0053 — L5 Component index slot framework (`IComponentIndex`, ChangeDetect+AsyncAware ship; History/SpatialBVH/GpuResident API only)
- ADR-0054 — Transform hierarchy update (TRS authored, world cached, push dirty propagation, single-thread serial v1)
- ADR-0055 — Scene serialization (TOML authoring → SCEN CRDR cooked; closes ADR-0020's FlatBuffers vs Cap'n Proto deferral with "neither — CRDR")
- ADR-0056 — L6–L8 reserved slots (Replication, Scripts, Reflection — registration grammar accepts traits, impls defer to 4.2 / 4.0 / 7)
- ADR-0057 — UI in scene tree boundary (`ControlNodeTag` reserved, all UI components live in `crd-ui`)

Also updated:
- `docs/phases/phase-3.0-scene-ecs.md` — rewritten around the 8-layer architecture and 14 slices, with explicit "ships now / API only" matrix per layer
- `docs/decisions/README.md` — ADR index extended with 9 new entries
- `docs/ROADMAP.md` — Phase 2.8 marked shipped; Phase 3.0 row added with link to architecture

## Previous shipped milestone

**2026-05-06 — `crd-math` interpolation primitives + Penner easing curves shipped.**

`crd-math/scalar.hpp` extended with the standard interpolation family:
- `lerp(a, b, t)`, `mix(a, b, t)` (GLSL alias), `saturate(x)`, `step(edge, x)`
- `smoothstep(e0, e1, x)` (Hermite C¹), `smootherstep(e0, e1, x)` (Perlin C²)
- `inverse_lerp(a, b, x)`, `remap(x, ia, ib, oa, ob)`
- `damp(a, b, lambda, dt)` — frame-rate-independent exponential approach

`crd-math/vec.hpp` extended:
- `lerp` for `Vec2/3/4` already existed; added `mix` aliases and `damp` componentwise overloads.

New header `crd-math/easing.hpp` — full Penner easing family (31 functions), all `T t ∈ [0,1] → T`:
- `ease_linear`
- `ease_in_*` / `ease_out_*` / `ease_in_out_*` for: Sine, Quad, Cubic, Quart, Quint, Expo, Circ, Back, Elastic, Bounce.
- Polynomial families (Quad/Cubic/Quart/Quint, Back, Bounce, Linear) are `constexpr noexcept`. Trig/exp families (Sine, Expo, Circ, Elastic) are `inline noexcept` (C++20 doesn't make `<cmath>` `constexpr` yet — moves to constexpr automatically when we adopt C++26).
- Curves are decoupled from value type: combine with `lerp` at the call site, e.g. `lerp(a, b, ease_out_cubic(t))`. No `Tween` class, no `EasingChannel`, no animation runtime.

`crd-math/math.hpp` umbrella includes `easing.hpp`.

Sandbox migration:
- Removed file-local `exp_lerp` / `exp_lerp3` helpers from `sandbox_layer.cpp`.
- `OrbitCamera` smoothing now uses `crd::math::damp` for both scalar (`s_dist`) and vector (`s_target`) channels.

Tests added (`tests/math/test_math.cpp`, +8 cases):
- scalar lerp/mix/saturate/step/inverse_lerp/remap with extrapolation past `t > 1`.
- smoothstep / smootherstep boundary saturation, midpoint exactness, monotonicity over 33 sample points.
- `damp` identity at `dt = 0`, convergence at large `dt`, frame-rate-stability cross-check (60 ticks at `dt = 1/60` ≈ 1 tick at `dt = 1`).
- Vec3 lerp / mix / damp componentwise.
- All 31 easings: `f(0) ≈ 0` and `f(1) ≈ 1` boundary anchors.
- In/Out reflection identity for the strictly monotone families.
- Monotone non-decreasing on [0,1] for Sine/Quad/Cubic/Quart/Quint/Circ/Expo (sampling 65 points each).
- Back undershoot, Elastic overshoot, Bounce stays in [0, 1].

Six-configuration green:
- win-debug:          481/481
- win-relwithdebinfo: 481/481
- win-release:        478/478
- win-asan:           481/481
- win-clang-cl:       481/481
- win-tidy:           ✅ (build clean)

## Previous shipped milestone

**2026-05-06 — Phase 2.8 v1f + v1g SHIPPED: glTF demo asset bundle + cook-demo-assets target + unified Asset Browser panel; device-destroy crash fix. Phase 2.8 COMPLETE.**

Source asset bundle (`assets/source/`):
- `BoxTextured.glb` (5 KB, CC-BY 4.0, Cesium/Khronos),
- `Duck.glb` (118 KB, SCEA Shared Source 1.0, Sony/Khronos),
- `BoomBox.glb` (10 MB, CC0, UX3D/Khronos),
- `checker_512.png` + `bricks_512.png` (procedural CC0, generated by `generate_textures.ps1`),
- `LICENSES.md` documenting per-file license terms.

`cook-demo-assets` CMake target (`sandbox/CMakeLists.txt`):
- `add_custom_command(OUTPUT demo_assets.crdr COMMAND $<TARGET_FILE:asset_cooker> cook ...)` with explicit DEPENDS on each source file — recooks on source change.
- `crd-sandbox` adds `add_dependencies(crd-sandbox cook-demo-assets)` so building sandbox triggers a cook; `CRD_DEMO_ASSETS_PACK` compile def points sandbox to the cooked pack.
- Verified output: 5 manifest entries (3 MESH + 2 TXTR), stable UUIDs across re-cooks.

`Asset Browser` panel (`sandbox/src/sandbox_layer.cpp`):
- Replaces former "Meshgen Browser". Two collapsing sections: **Procedural Shapes** (8 meshgen entries) and **Imported Assets** (3 glTF entries when the pack is mounted).
- `SandboxLayer` owns a `ResourceManager`; mounts `CRD_DEMO_ASSETS_PACK` and registers `MeshResourceLoader`. Reads each `<file>.glb.meta` sidecar to recover the cooker-minted UUID.
- Click an imported entry → `load_sync<MeshResource>` → `GpuUploader::upload_mesh()` → swap. Per-selection metadata: vertex count, index count, triangle count, source ("Procedural" or "glTF").
- Graceful fallback: missing pack / missing meta sidecar / UUID-not-in-pack each log a warning and hide the affected entry rather than aborting.

Crash fix:
- `Application::detach_all_layers()` now clears `m_owned_layers` — previously it cleared the stack but kept the unique_ptrs alive. Their destructors ran during `~Application` *after* the `Device` local in `main` had been destroyed, producing a use-after-free during VK destroy calls in layer dtors.
- `sandbox/src/main.cpp` reordered: `device->wait_idle()` is now called *before* `app.detach_all_layers()` (was the other way around). `smoke_imgui_overlay.cpp` reordered identically.

Other API changes:
- `GpuUploader::upload_texture` and `upload_mesh` now take `const&` (was non-const&). Required so `load_sync<T>::handle.get()` (returns `const T*`) can be passed directly. No behavioural change — both functions only read the CPU resource.

Six-configuration green:
- win-debug:          473/473
- win-relwithdebinfo: 473/473
- win-release:        470/470
- win-asan:           473/473
- win-clang-cl:       473/473
- win-tidy:           ✅ (build clean)

Headless smokes (17/17 across all five non-tidy configs): `smoke_config`, `smoke_containers`, `smoke_filesystem`, `smoke_frame_clock`, `smoke_jobs`, `smoke_log`, `smoke_math`, `smoke_memory`, `smoke_shader`, `smoke_resources`, `smoke_resources_async`, `smoke_resources_reload`, `smoke_resources_stream`, `smoke_resources_render`, `smoke_texture`, `smoke_mesh`, `smoke_material`. `crd-sandbox --headless` exits 0 with the demo pack mounted (5 entries, mount_id=1).

## Previous shipped milestone

**2026-05-05 — Phase 2.8 v1a–v1e SHIPPED: per-material pipeline cache + multi-pass + depth prepass + default lit shaders + sandbox 3D rendering.**

Key changes across v1a–v1e:

**ForwardRenderPath (`engine/renderer/src/forward_render_path.cpp`):**
- `get_or_compile_mat_pipelines()` — lazy pipeline cache keyed by `MaterialTemplate*`; compiles depth-only (vertex-only, `Undefined` color, `D32Sfloat` depth) and color (vert+frag, `B8G8R8A8Unorm`) pipelines from `ShaderResource::spirv` directly.
- `PipelineResolver::begin_pass(PassType)` default no-op added to interface; `ForwardRenderPath` calls `m_resolver->begin_pass()` before each draw loop to inform resolvers of the current pass.
- Material-path branching: `DrawItem::material != nullptr` uses the compiled cache; legacy items (null material) continue to use the `PipelineResolver`.

**Renderer (`engine/renderer/`):**
- `DrawItem::material` + `Renderable::material` fields added (`const MaterialInstance* material = nullptr`).
- `build_frame()` validation relaxed: accepts `material != nullptr` even when `variant` is invalid; copies material pointer into `DrawItem`.

**Shader (`engine/shader/`):**
- `Module::code_bytes()` pure virtual added to interface; `StoredModule` implements it via word-buffer reinterpret. Required so `SandboxPipelineResolver` can extract SPIR-V bytes from a compiled module without going through the resource system.

**RHI (`engine/rhi/`, `engine/rhi-vulkan/`):**
- `Format::R32G32B32A32Sfloat` added + VkFormat mapping.
- SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT was wrongly returning `R8G8B8A8Unorm` — corrected to `R32G32B32A32Sfloat`.

**Default lit shaders (`engine/renderer/shaders/`):**
- `surface.vert`: reads 48B interleaved vertex (pos/normal/uv0/tangent at locations 0–3); writes `VertexAttrs` (position_ws, normal_ws, uv0, tangent_ws) to fragment.
- `surface.frag`: implements `crd_evaluate_surface()` with default-lit values; Lambertian diffuse with hardcoded directional light.
- `assets/materials/default_lit.mat.toml`: domain=Surface, alpha_mode=Opaque.

**Sandbox (`sandbox/src/`):**
- `SandboxPipelineResolver`: compiles depth + color pipelines lazily from `Module::code_bytes()` on first `resolve_pipeline` call; `begin_pass()` tracks current pass.
- `SandboxLayer`: holds `ForwardRenderPath`, `GpuMesh`, `Renderer`; `render_scene()` builds camera matrices (local `look_at` + reverse-Z `perspective`), submits legacy-path renderable with `m_surface_variant`, runs frame graph, blits color RT → swapchain (ColorWrite→TransferSrc→blit→TransferDst→ColorWrite).
- `smoke_depth_prepass.exe`: GPU/window smoke — creates FRP, submits empty draw list, executes one frame, exits 0.

Six-configuration green:
- win-debug:          471/471
- win-relwithdebinfo: 471/471
- win-release:        468/468
- win-asan:           471/471
- win-clang-cl:       471/471
- win-tidy:           ✅ (build clean)

## Previous shipped milestone (-1)

**2026-05-05 — Phase 2.7 v1e SHIPPED: `crd-meshgen` + sandbox Meshgen Browser. Phase 2.7 COMPLETE.**

New module `engine/meshgen/`: 8 CPU-side procedural geometry generators. `smoke_meshgen.exe` headless. 11 unit tests. `crd-sandbox` Meshgen Browser ImGui panel. Six-configuration green (468/468 win-debug).

## Previous shipped milestone (–2)

**2026-05-05 — Phase 2.7 v1c SHIPPED: Full material system foundation (ADR-0048).**

New headers: `engine/renderer/include/crd/renderer/material_domain.hpp` (`MaterialDomain` enum: Surface/PostProcess/Compute/Decal/UI), `engine/renderer/include/crd/renderer/pass_type.hpp` (`PassType` enum: DepthPrepass=0/Shadow=1/Forward=2; `AlphaMode`, `CullMode`, `FillMode`, `BlendMode`, `RasterState` — 8 bytes per state), `engine/renderer/include/crd/renderer/material_template.hpp` (`ParameterType` enum, `CookedParameter` 24-byte struct, `ShaderOptionDecl` 16-byte struct, `PassShaderPair` {vert + frag `ResourceHandle<ShaderResource>`}, `MaterialTemplate` with `Array<CookedParameter> parameters`, `Array<u8> defaults_blob`, `PassShaderPair pass_shaders[3]`, `RasterState pso_states[3]`, `Array<ShaderOptionDecl> options`, `MaterialInstance` with `set_float`/`set_vec4` binary-search parameter write + `variant_for_pass` fallback).

Renames: `MaterialLayout` → `MaterialBindLayout`, `MaterialInstance` (transient GPU binding) → `MaterialBindGroup` (both in `engine/renderer/include/crd/renderer/material.hpp`). `MaterialResource` struct removed from `material_resource_loader.hpp` — replaced by `MaterialTemplate` in new header.

New MATR v2 artifact format: INFO chunk (4 bytes: loader_version, domain, flags, pad) + PASS chunk (4-byte count + N × 36-byte entries: pass_type u8, pad[3], vert_id u8[16], frag_id u8[16]). PRMS/DFLT/PSOS/OPTS chunks defined but not yet emitted by cooker (reserved for Phase 2.8). CRDR FourCCs added: `kFourCC_INFO`, `kFourCC_PRMS`, `kFourCC_DFLT`, `kFourCC_PASS`, `kFourCC_PSOS`, `kFourCC_OPTS`.

`MaterialResourceLoader` rewritten (version → 2): reads INFO → domain, PASS → `pass_shaders[]` indexed by `PassType` ordinal (load_sync<ShaderResource> for each vert+frag pair), falls back to legacy META chunk (synthesizes Forward entry) for backward-compat. Cooker rewritten (version → 2): emits INFO + PASS chunks; parses `[passes.forward]` and `[passes.depth_prepass]` TOML sections; legacy flat `vertex_shader`/`fragment_shader` keys treated as Forward pass.

5 new tests in `tests/resources/test_shader_material_loaders.cpp` tagged `[v1c]`: v2 PASS chunk round-trip, two-pass material (fwd + depth), missing PASS+META → Failed, MaterialInstance set_float round-trip, MaterialInstance variant_for_pass fallback. `smoke_material.exe` (headless, 2 scenarios: v2 + MaterialInstance + legacy META, exit 0).

Six-configuration green:
- win-debug:          457/457
- win-relwithdebinfo: 457/457
- win-release:        454/454
- win-asan:           457/457
- win-clang-cl:       457/457
- win-tidy:           ✅ (build clean)

## Previous shipped milestone (–1)

**2026-05-05 — Phase 2.7 v1b SHIPPED: `MeshResource` + cgltf glTF import + MikkTSpace tangent generation.**

`MeshPrimitive` + `MeshResource` in `engine/renderer/include/crd/renderer/mesh_resource.hpp`. Interleaved 48-byte vertex layout: float3 pos (0–11) + float3 normal (12–23) + float2 uv0 (24–31) + float4 tangent (32–47, w = bitangent sign). `MeshResourceLoader` (`engine/renderer/src/mesh_resource_loader.cpp`): reads `type='MESH'` CRDR artifact, parses VERT + INDX + PRIM chunks. PRIM chunk is 4-byte count + N × 32-byte entries (vertex_count, index_count, vertex_byte_offset, index_byte_offset, u8[16] material_id). Registered via `crd::renderer::register_mesh_loader(rm)`.

glTF cooker handler in `tools/asset_cooker/src/cook_handlers/mesh.cpp`: cgltf for parsing (CPM DOWNLOAD_ONLY + INTERFACE; header + impl in same TU), MikkTSpace for tangent generation (CPM DOWNLOAD_ONLY + INTERFACE; `mikktspace.c` included inline within `extern "C"` block — avoids C compiler detection in a LANGUAGES CXX-only project). Static meshes only (no skinning/morph targets). Reads POSITION/NORMAL/TEXCOORD_0/TANGENT accessors; generates tangents via `genTangSpaceDefault()` if TANGENT absent. Indices always upcast to u32. First glTF mesh → main `CookResult`; additional meshes → `ExtraArtifact` array (new `CookResult::extra_artifacts` field, backward-compatible; each extra artifact gets a `.mesh.<name>.meta` sidecar). Registers `.glb` + `.gltf`. `cook_command.cpp` extended to process extra_artifacts loop.

CRDR FourCCs added: `kFourCC_MESH`, `kFourCC_VERT`, `kFourCC_INDX`, `kFourCC_PRIM` (in `crdr.hpp`).

4 new tests in `tests/resources/test_mesh_loader.cpp`: MESH artifact round-trip, multi-primitive count, missing VERT → Failed, GLB cook + load round-trip (hand-assembled 152-byte binary GLB). `smoke_mesh.exe` (headless, 2-primitive MESH artifact, mounts, loads, verifies primitive sizes + null material UUIDs, exit 0).

Six-configuration green:
- win-debug:          452/452
- win-relwithdebinfo: 452/452
- win-release:        449/449
- win-asan:           452/452
- win-clang-cl:       452/452
- win-tidy:           ✅ (build clean)

## Previous shipped milestone (–2)

**2026-05-04 — Phase 2.7 v1a SHIPPED: `TextureResource` + stb_image texture cooker.**

`TextureResource` + `MipLevel` in `engine/renderer/include/crd/renderer/texture_resource.hpp`. `TextureFormat` enum (RGBA8Unorm/BC7Unorm/BC7UnormSrgb — on-disk byte values, never reorder). `TextureResourceLoader` (in `engine/renderer/src/texture_resource_loader.cpp`): reads `type='TXTR'` CRDR artifact, parses 16-byte `HEAD` chunk (width u32, height u32, mip_count u32, format u8, padding[3]), validates dims/format/mip count (max 16), reads and validates per-mip pixel size for RGBA8, copies mip pixel data into `MipLevel::pixels`. Registered via `crd::renderer::register_texture_loader(rm)`.

Texture cook handler in `tools/asset_cooker/src/cook_handlers/texture.cpp`: stb_image for decode (STBI_rgb_alpha → 4 channels, TGA BGRA→RGBA swap handled by stb), box-filter mip chain generation to 1×1 with ping-pong scratch buffers (O(W×H) memory), writes HEAD chunk + MIP0..MIPn chunks. Registers `.png`/`.jpg`/`.jpeg`/`.tga`/`.bmp` via `register_texture_handler()`, called from `register_builtin_handlers()`.

CRDR FourCCs added to `crdr.hpp`: `kFourCC_TXTR`, `kFourCC_HEAD`, `kFourCC_MIP0`–`kFourCC_MIP15` (via `make_mip_fourcc()`). 4 new tests; `smoke_texture.exe`. Six-configuration green (448/448 win-debug).

## Previous shipped milestone (–2)

**2026-05-04 — Phase 2.6 v1g SHIPPED: load_streamed + 2Q LRU eviction + memory budget + pinning. Phase 2.6 COMPLETE.**

5 new tests in `tests/resources/test_eviction.cpp`. `smoke_resources_stream.exe`. Six-configuration green (444/444 win-debug).

## Previous shipped milestone (–1)

**2026-05-04 — Phase 2.6 v1f shipped: hot-reload — mtime polling, atomic payload swap, callbacks.**

`ResourceControlBlock::payload` made `std::atomic<void*>`. `poll_hot_reload(debounce_ms)` polls mounted PACK files. `reload_mount_now(MountId)` forces reload bypassing mtime. `subscribe_reload` / `unsubscribe_reload`. Deferred-free grace period. 4 new unit tests in `test_hot_reload.cpp`. `smoke_resources_reload.exe`.

Six-configuration green:
- win-debug:          439/439
- win-relwithdebinfo: 439/439
- win-release:        436/436
- win-asan:           439/439
- win-clang-cl:       439/439
- win-tidy:           439/439

## Previous shipped milestone (–1)

**2026-05-04 — Phase 2.6 v1e shipped: ShaderResourceLoader + MaterialResourceLoader + end-to-end cooked render smoke.**

`ShaderResourceLoader` (`engine/shader/src/shader_resource_loader.cpp`, registered via `crd::shader::register_shader_loader(rm)`): reads SPVV/SPVF/SPVC chunk from a `type='SHDR'` artifact to determine stage, copies SPIRV bytes into `ShaderResource::spirv`, then drives spirv-reflect to populate `descriptor_bindings`, `push_constants`, and (for vertex stage) `vertex_attributes`. Version 1. Clang-cl fix: removed dead `to_parameter_class_local` helper (caught by `-Werror,-Wunused-function`; MSVC `/W4 /WX` doesn't flag unused statics).

`MaterialResourceLoader` (`engine/renderer/src/material_resource_loader.cpp`, registered via `crd::renderer::register_material_loader(rm)`): reads 32-byte META chunk from a `type='MATR'` artifact, extracts vert/frag `ResourceId` pairs, calls `ctx.manager->load_sync<ShaderResource>(id)` transitively for each, builds a `MaterialResource` holding both handles. Version 1.

`compile_glsl()` free function (`engine/shader/src/compile.cpp`): shaderc-backed GLSL→SPIRV helper usable in tests and the cooker without pulling in the full shader runtime. `.glsl` cooker handler: emits `type='SHDR'` CRDR with a SPVV/SPVF/SPVC chunk. `.mat.toml` cooker handler: parses TOML vert/frag source-path references, looks up UUIDs from adjacent `.meta` sidecars, emits `type='MATR'` CRDR with 32-byte META chunk.

`smoke_resources_render.exe`: cooks one `.vert.glsl` + one `.frag.glsl` inline, assembles them into a PACK with a MATR artifact, mounts, calls `load_sync<MaterialResource>`, asserts both shader handles are Ready, prints SPIRV sizes, exits 0. Output: `smoke_resources_render: OK — MaterialResource loaded with vert+frag SPIRV (vert=1040 bytes, frag=572 bytes)`.

Six new tests in `tests/resources/test_shader_material_loaders.cpp`: vertex SHDR round-trip, fragment SHDR round-trip, missing SPIRV chunk → Failed, material loads + resolves deps (verifies transitive cache and `handle_count() == 3`), missing META → Failed, real SPIRV round-trip via `compile_glsl()` (shaderc-dependent, skips gracefully if unavailable).

Six-configuration green:
- win-debug:          435/435
- win-relwithdebinfo: 435/435
- win-release:        432/432
- win-asan:           435/435
- win-clang-cl:       435/435
- win-tidy:           435/435

## Previous shipped milestone

**2026-05-04 — Phase 2.6 v1d shipped: AsyncFile + load_async<T> + fiber-cooperative wait_ready().**

`crd::platform::AsyncFile` (`engine/platform/`): job-pool async file reads. `open()` returns an AsyncFile with `is_open()`/`size()`. `read_async(offset, span)` submits a `crd-jobs` job and returns a `Counter*`; returns `nullptr` if `offset + size > file_size`. Windows backend uses `ReadFile` inside a SBO-compatible `ReadJob` (40 bytes). `crd-platform` gains a PRIVATE link dep on `crd-jobs`.

`ResourceManager::load_async<T>`: heap-allocates `AsyncLoadCtx`, submits via 8-byte `LoadJobFn` closure (within SBO limit). `m_in_flight` HashMap (keyed by ResourceId) prevents duplicate I/O when concurrent calls race for the same id. `m_mutex` released before all I/O and loader dispatch — enables recursive `load_sync` transitive dep resolution without deadlock. `run_load_job` made `public` in `ResourceManager` so the anonymous-namespace closure can call it. Counter leak fix: after storing counter in `block->load_counter`, if state is already terminal, immediately reclaim+wait.

`ResourceHandleBase::wait_ready()`: atomically exchanges `block->load_counter` (first caller claims it), calls `crd::jobs::wait()` for fiber-cooperative suspension. Terminal-state fast path also attempts exchange before returning (covers job-completes-before-store race). Spin+yield fallback for non-fiber callers. Moved to `resource_handle.cpp` (with `release_block()`) so headers don't pull in `jobs.hpp` or `loader.hpp`.

`smoke_resources_async.exe`: end-to-end async round-trip (assemble PACK, mount, `load_async<BlobResource>`, `wait_ready()`, verify 5 bytes, exit 0). Nine new tests: 4 `[platform][async_file]` in `tests/platform/test_async_file.cpp`, 5 `[resources]` load_async tests in `test_resource_manager.cpp`.

Six-configuration green:
- win-debug:          429/429
- win-relwithdebinfo: 429/429
- win-release:        426/426
- win-asan:           429/429
- win-clang-cl:       429/429
- win-tidy:           429/429

## Previous shipped milestone (–1)

**2026-05-03 — Phase 2.6 v1c shipped: RefCounted<T> + ResourceHandle<T> + load_sync<T> + cycle detection + smoke_resources.**

`crd::memory::RefCounted<T>` CRTP intrusive refcount. `ResourceControlBlock`, `ResourceHandleBase`, `ResourceHandle<T>`, thread-local cycle detection, `load_sync_impl`, `make_failed_block()`, `read_file_range()`, `smoke_resources.exe`. 20 new tests.

Six-configuration green:
- win-debug:          420/420
- win-relwithdebinfo: 420/420
- win-release:        417/417
- win-asan:           420/420
- win-clang-cl:       420/420
- win-tidy:           420/420

## Previous shipped milestone (–1)

**2026-05-03 — Phase 2.6 v1b shipped: cooker CLI + zstd compression.**

zstd v1.5.5 wired as per-chunk opt-in in `CrdrWriter::add_chunk_compressed()` (level 3 default; falls back to uncompressed if compression doesn't help). Two-pass reader in `crdr_read()` pre-allocates `decompressed_backing` before decompression loop (no span invalidation). `CrdrError::DecompressFailed` added; `main.cpp` switch updated.

`crd-cooker` static library split from `asset_cooker` executable (tests can link it directly). New headers: `cook_handler.hpp` (CookContext, CookResult, CookHandlerFn), `cook_command.hpp` (cmd_cook). `cmd_cook()`: recursive directory scan (excludes .meta + .cook_cache/), sorted for determinism, .meta sidecar mint/read, FNV1a-64 source hash, cook_key = source_hash ^ handler_version stored in `.cook_cache/<uuid>.key`, artifact stored in `.cook_cache/<uuid>.crdr`, two-pass PACK assembly (pass 1 measures CRDR size, pass 2 fills real blob_offsets), `cook.log.toml` written adjacent to the pack. `blob_passthrough_handler` for `.bin` files. Optional CMake `cook` target (CRD_COOK_ROOT + CRD_COOK_OUT). 4 new tests: registry, .bin round-trip, zstd round-trip, integration (10 files → 10 entries, byte-identical second run, "skipped" log entries).

Six-configuration green:
- win-debug:          408/408
- win-relwithdebinfo: 408/408
- win-release:        405/405
- win-asan:           408/408
- win-clang-cl:       408/408
- win-tidy:           408/408

## Previous shipped milestone

**2026-05-03 — Phase 2.6 v1a shipped: `crd-resources` + `asset_cooker` manifest_dump.**

`ResourceId` (UUID v4 via mt19937_64, UUID v5 via SHA-1 SHA-1 + Cerid namespace, parse/to_string,
36-char hyphenated format). CRDR chunked binary container (reader + writer, chunk sort, 16-byte
padding, LE serialization). `ManifestEntry` 48-byte disk format (MFST/STRP/DEPS chunks).
`ResourceManager` shell: `register_loader`, `mount_manifest` (reads CRDR PACK, populates live
index, newest-mount-wins collision), `unmount` (by MountId). `asset_cooker manifest_dump` CLI
sub-command. 38 new tests across three test files.

Also fixed: `crd-containers String` SSO encoding changed to remaining-capacity (`size_or_flag =
kSsoCapacity - size`) to eliminate `buf[kSsoCapacity]` UB exposed by new MSVC 14.50.35717
optimizer. A 23-char SSO string now has `size_or_flag = 0 = '\\0'` which doubles as the null
terminator, so `c_str()` is always correct and no out-of-bounds array access occurs.

Six-configuration green:
- win-debug:          393/393
- win-relwithdebinfo: 393/393
- win-release:        390/390
- win-asan:           393/393
- win-clang-cl:       393/393
- win-tidy:           393/393

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1k integration smoke + crd-app wiring shipped. Phase 2.5 COMPLETE.**

`smoke_jobs.cpp` rewritten from raw fiber demo (v1a) to full public API exercise: `init/shutdown`,
`run+wait`, `parallel_for` (1 000-element sum), H/N/L priority all-ran (10/20/40 jobs),
`frame_alloc/frame_reset`. All sections PASS, exit 0.

`Application::run()` now calls `crd::jobs::init(m_desc.jobs_config)` before the tick loop and
`crd::jobs::shutdown()` after, guarded by `if (!m_valid) return`. `ApplicationDesc` gained
`crd::jobs::Config jobs_config{}`. `crd-app` CMakeLists links `crd-jobs` PUBLIC.
`smoke_renderer` verified clean (exit 0) with the wired Application.

Six-configuration green:
- win-debug:          355/355
- win-relwithdebinfo: 355/355
- win-release:        352/352
- win-asan:           355/355
- win-clang-cl:       355/355
- win-tidy:           355/355 (exit 0)

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1h Public API layer shipped.**

`engine/jobs/include/crd/jobs/jobs.hpp` — full public API: `Config`, `init()`, `shutdown()`,
`run(span)` / `run(single)`, `wait()`, `run_and_wait(span)` / `run_and_wait(single)`,
`is_worker_fiber()`, `worker_index()`, `num_workers()`.

Key design decision: counter pointer stored in `Fiber::job_counter` (not TLS) so it survives
fiber suspension. 5 public-API tests. Total at v1h: 346 tests.

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1g Worker thread pool + main-thread fiber shipped.**

`WorkerPool` class in `engine/jobs/src/worker_pool.hpp/.cpp`: owns `Scheduler`, `FiberPool`, and
`CounterPool`; spawns N-1 background worker threads (indices 1..N-1) each running `worker_loop`;
thread 0 driven by `pump()` for main-thread use. All jobs run inside fiber context switches via
`job_fiber_trampoline` (looping entry fn burned into every fiber stack at pool init).

Key fix: `fiber_switch_win64.asm` now saves/restores GS:[8] (StackBase) and GS:[16] (StackLimit)
alongside RSP, so `__chkstk` and guard pages work correctly when switching between fiber stacks.
`FiberContext` extended with `tib_stack_base` / `tib_stack_limit` on Windows. `fiber_context.hpp`
now explicitly includes `platform.hpp` (was relying on PCH ordering).

Key fix: fiber reuse was broken — snapshot copy `target->context = target->initial_ctx` was
corrupted because `fiber_switch`'s register saves (push r14...) overwrite the initial frame data
on the fiber stack. Fix: call `fiber_init_stack` on completion to rebuild a fresh initial frame.
`Fiber` struct: `initial_ctx` field removed; `usable_base`, `usable_size`, `trampoline` fields
added so `WorkerPool` can re-initialize without pool context.

10 unit tests in `tests/jobs/test_jobs.cpp`: init/shutdown, re-init, default thread count,
single worker job, pump on thread 0, pinned job via pump, multiple jobs, fiber stack isolation,
pump empty probe, 1000-job concurrent stress. Also fixed pre-existing clang-tidy warnings in
`test_scheduler.cpp` (u→U suffix, member m_ prefix, braces-around-statements).

Six-configuration green:
- win-debug:          341/341
- win-relwithdebinfo: 341/341
- win-release:        338/338
- win-asan:           341/341
- win-clang-cl:       341/341
- win-tidy:           341/341

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1f Counter + wait mechanism shipped.**

`Counter` (alignas(64), one cache line: `atomic<u32> value`, `atomic<Waiter*> waiters`, pool
metadata) + `CounterPool` (generation-tagged 64-bit Treiber free list `[gen:32|idx:32]`, same
pattern as FiberPool). `Waiter` struct: `Fiber* fiber`, `u32 target`, `atomic<bool> canceled`,
`atomic<Waiter*> next`. `counter_decrement` steals waiter list atomically; `counter_wait` is a
6-step ABA-safe protocol (fast path, Treiber push, double-check, fiber_switch suspend). 14 unit
tests incl. real fiber_switch suspension + 16-thread concurrent stress.

Six-configuration green:
- win-debug:          331/331
- win-relwithdebinfo: 331/331
- win-release:        328/328
- win-asan:           331/331
- win-clang-cl:       331/331
- win-tidy:           331/331

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1e Priority Scheduler shipped.**

`Scheduler` class: three global Vyukov MPMC injection queues (High/Normal/Low), per-thread three
Chase-Lev local deques, and a single-slot pinned-job lane per thread. Drain order: pinned →
H-inject → H-local → H-steal → N-inject → N-local → N-steal → L-inject → L-local → L-steal.
`std::counting_semaphore<>` sleep/wake. 16 unit tests.

Detail: `docs/sessions/2026-05-02-jobs-v1e-scheduler.md`.

Six-configuration green:
- win-debug:          317/317  
- win-relwithdebinfo: 317/317  
- win-release:        314/314  
- win-asan:           317/317  
- win-clang-cl:       317/317  
- win-tidy:           317/317  

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1d Vyukov MPMC injection queue shipped.**

`MpmcQueue<T>` header-only template implementing the Vyukov bounded MPMC queue algorithm
(Dmitry Vyukov, 1024cores.net). Producers and consumers each have their own `alignas(64)`
atomic position counter (separate cache lines) to prevent false sharing. Each ring-buffer
cell holds an `atomic<u64>` sequence and one T. The sequence handshake uses `acquire`/`release`
orderings: producers read sequence with acquire and publish with release; consumers mirror
this. enqueue() returns false (non-blocking) when full; dequeue() returns false when empty.
Capacity must be a power of two.

10 unit tests: construction, single-item round-trip, empty returns false, full returns false,
FIFO ordering, wrap-around across 3 full laps, SPSC/MPSC/SPMC/MPMC concurrent stress
(each verifying all items consumed exactly once).

Six-configuration green:
- win-debug:          301/301
- win-relwithdebinfo: 301/301
- win-release:        298/298
- win-asan:           301/301
- win-clang-cl:       301/301
- win-tidy:           301/301

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1c Chase-Lev work-stealing deque shipped.**

`WorkStealingDeque<T>` header-only template implementing the Lê et al. 2013
algorithm ("Correct and Efficient Work-Stealing for Weak Memory Models").
Owner thread uses push() (LIFO via `m_bottom`) and pop(); any thread calls
steal() (FIFO via `m_top`). Fixed power-of-two capacity; indices are `i64`
monotonically increasing counters masked with `& (capacity-1)`.

Memory ordering: push uses `release` on `m_bottom`; pop uses `seq_cst` on
`m_bottom`-store + `seq_cst` on `m_top`-load + `seq_cst` CAS for the last-
element race; steal uses `acquire` load of `m_top`, `seq_cst` fence (required
for correctness on weak memory models per Lê et al. Thm 1), `acquire` load
of `m_bottom`, then `seq_cst` CAS.

`m_bottom` and `m_top` on separate `alignas(64)` cache lines to prevent
false sharing between owner and thieves. `CRD_COMPILER_MSVC` pragma suppresses
C4324 (structure padded due to alignment specifier).

12 tests: LIFO/FIFO ordering, exhaustion assertion, last-element race
(4 000 trials, each must give exactly one winner), two concurrent stress tests
(pre-fill + concurrent drain; concurrent push + pop + steal with back-pressure).

Six-configuration green:
- win-debug:          291/291
- win-relwithdebinfo: 291/291
- win-release:        288/288
- win-asan:           291/291
- win-clang-cl:       291/291
- win-tidy:           291/291

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1b fiber pool shipped.**

Three-tier fiber pool (Small 64 KB × 128, Medium 512 KB × 64, Large 2 MB × 16).
Platform stack allocation: VirtualAlloc (Windows) / mmap+mprotect (Linux) with
an uncommitted guard page below each stack — overflow crashes immediately.
Lock-free Treiber free list per tier using a tagged 64-bit head
`[gen:32 | idx:32]`; generation is bumped on every pop, making ABA structurally
impossible without CMPXCHG16B. `alignas(64)` on `Tier` struct prevents inter-tier
false sharing. Debug-only explicit state machine: `Idle / Active / Waiting / Ready`
with asserted transitions at acquire/release (Waiting/Ready stubs ready for v1f).
Peak-usage watermark tracked per tier for profiling. Trampoline is injected at
pool creation (looping, zero re-init cost on the hot acquire path).
13 new tests; concurrent 4-thread × 8 000-iteration ABA stress test included.

Also fixed in this session: renderer LTO miscompilation under win-relwithdebinfo
(`FrameGraph::build()` local-array tracking moved to `ImageResource` members;
`ForwardRenderPath` lambda captures changed from `&draw_list` to `m_draw_list`
via new member pointer — both were MSVC `/GL` miscompile sites).

Six-configuration green for the first time:
- win-debug:          279/279
- win-relwithdebinfo: 279/279
- win-release:        276/276
- win-asan:           279/279
- win-clang-cl:       279/279
- win-tidy:           279/279

## Previous shipped milestone

**2026-05-01 — `crd-jobs` v1a hand-rolled asm context switch shipped.**

`fiber_switch` in Windows x64 MASM and Linux x86-64 AT&T assembly: saves/restores
all callee-saved registers mandated by each ABI (Windows: RBX RBP RDI RSI R12–R15
XMM6–XMM15 MXCSR FCW; Linux: RBX RBP R12–R15). `fiber_init_stack` in C++ sets up
the initial stack frame so the first `fiber_switch` to a fresh fiber jumps to the
entry function; a sentinel `fiber_abort` return-address catches runaway fibers.
5 unit tests: round-trip, multiple re-entries, stack-local data survives,
callee-saved registers verified, two independent fibers.

Detail: (session combined with v1b above).

## Previous shipped milestone (–2)

**2026-05-01 — `crd-renderer` v1i swapchain blit + first full frame loop shipped.**

`CommandBuffer::blit_image(src, dst, src_extent, dst_extent)` added to RHI interface
and implemented in `VulkanCommandBuffer` via `vkCmdBlitImage` with `VK_FILTER_LINEAR`.
Swapchain creation now sets `VK_IMAGE_USAGE_TRANSFER_DST_BIT`. `ForwardRenderPath`
color image adds `TransferSrc` usage. New free function `add_swapchain_blit_pass`
(in `crd/renderer/swapchain_blit.hpp`) adds two frame graph passes per frame:
"swapchain-blit" (ColorWrite→TransferSrc, Undefined→TransferDst, blit_image call) and
"present-barrier" (TransferDst→Present, empty execute). All fake `CommandBuffer`
implementations updated. 4 new unit tests. Smoke updated with end-to-end blit path.

Three-flavour green:
- win-debug:    261/261
- win-release:  260/260
- win-asan:     261/261

Detail: `docs/sessions/2026-05-01-renderer-v1i-swapchain-blit.md` (to be written).

## Previous shipped milestone (–3)

**2026-05-01 — `crd-renderer` v1g `ForwardRenderPath` shipped.**

First concrete `IRenderPath`: depth prepass (opaque items, depth-only) + main color pass
(opaque + masked, full shading). `PerFrameUbo` (288 bytes) at set 0, `PerDrawPush`
(model matrix, 64 bytes) as push constants. `ForwardRenderPath::create()` allocates
ring UBO + descriptor set per frame-in-flight. Render targets (B8G8R8A8Unorm color,
D32Sfloat depth) owned by path, recreated on `resize()`.

Vulkan backend hardened: `begin_rendering` now caller-managed transitions (no implicit
layout changes), color attachment optional (null = depth-only), null fragment shader
supported in `create_graphics_pipeline` (`colorAttachmentCount = 0`). Added `inverse(Mat4f)`
via Laplace cofactor expansion. 5 new unit tests.

Three-flavour green:
- win-debug:    253/253
- win-release:  252/252
- win-asan:     253/253

Smokes: `smoke_rhi_vulkan_bootstrap` (120 frames, clean), `smoke_renderer` (frame graph
transitions verified).

Detail: `docs/sessions/2026-05-01-renderer-v1g-forward-render-path.md`.

## Previous shipped milestone (–4)

**2026-05-01 — `crd-renderer` v1e+f merged: push constants + descriptor system + material binding shipped.**

Merged v1e + v1f into one slice. RHI surface: `push_constants()`, `bind_descriptor_sets()`,
`create_descriptor_set_layout()`, `create_pipeline_layout()`, `create_descriptor_allocator()`.
Vulkan backend: `VulkanDescriptorSetLayout`, `VulkanPipelineLayout`, `VulkanDescriptorSet`,
`VulkanDescriptorAllocator` (ring-buffer, `frames_in_flight` pools — see session doc).
`ShaderStage` promoted to bitmask (Vertex=1, Fragment=2, Compute=4). Explicit `PipelineLayout`
added to `GraphicsPipelineDesc` (optional, at end — no positional-init breakage). Renderer
material system: `MaterialLayout` + `MaterialInstance` wrapping the allocator-backed
descriptor set lifecycle. 10 new unit tests, 4 new material tests.

Three-flavour green:
- win-debug:    248/248
- win-release:  247/247
- win-asan:     248/248

Detail: `docs/sessions/2026-05-01-renderer-v1ef-descriptors.md`.

## Previous shipped milestone (–5)

**2026-05-01 — `crd-renderer` v1b real draw execution shipped.**

`crd-renderer` now has a real execution layer over the prepared draw items:
minimal pass orchestration, command buffer recording, pipeline resolution, and
draw-call submission into one rendering pass without taking ownership of native
pipeline objects.

Three-flavour green:
- Debug: 228/228
- Release: 227/227
- ASan: 228/228

Detail: `docs/sessions/2026-05-01-renderer-v1b-draw-execution.md`.

## Previous shipped milestone (–6)

**2026-05-01 — `crd-renderer` v1a explicit renderables shipped.**

The engine now has its first high-level renderer consumer over the completed
shader packet: camera, explicit renderable list, draw-item preparation, and a
clean frame-plan handoff without scene/ECS commitments.

Three-flavour green:
- Debug: 227/227
- Release: 226/226
- ASan: 227/227

Detail: `docs/sessions/2026-05-01-renderer-v1a-explicit-renderables.md`.

## Previous shipped milestone (–7+)

**2026-05-01 — `crd-shader` 2.3g pipeline handoff / descriptor growth shipped.**

`crd-shader` now produces a backend-neutral handoff surface describing compiled
module usage, normalized descriptor bindings, push-constant visibility, and
vertex-input requirements for a variant. This cleanly separates shader-owned
metadata from backend-owned pipeline objects.

Three-flavour green:
- Debug: 226/226
- Release: 225/225 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 226/226

Detail: `docs/sessions/2026-05-01-shader-2.3g-pipeline-handoff.md`.

## Previous shipped milestone

**2026-04-30 — `crd-shader` 2.3f hot reload shipped.**

Successful reload now compiles and swaps atomically, failed reload keeps the
last-good live state, and reload observability is exposed through `ReloadEvent`
without crashing consumers or invalidating effect/variant identity.

Three-flavour green:
- Debug: 225/225
- Release: 224/224 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 225/225

Detail: `docs/sessions/2026-04-30-shader-2.3f-hot-reload.md`.

## Previous shipped milestone

**2026-04-30 — `crd-shader` 2.3e cache hierarchy shipped.**

Source/preprocessed/SPIR-V cache keys are now explicit, local include graphs
participate in the key path, and the runtime now has both in-memory and on-disk
SPIR-V cache reuse. Compile diagnostics expose cache hit/miss behavior without
leaking backend types.

Three-flavour green:
- Debug: 223/223
- Release: 222/222 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 223/223

Detail: `docs/sessions/2026-04-30-shader-2.3e-cache-hierarchy.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3d variant key + mechanism policy shipped.**

Structural variant identity is now deterministic and typed. `VariantKey`
generation uses only structural axes, specialization values are excluded from
the structural key by design, and the hybrid mechanism policy is now encoded
as public helper decisions (`Permutation`, `SpecializationConstant`,
`ResourceBinding`, `DynamicBranch`).

Three-flavour green:
- Debug: 220/220
- Release: 219/219 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 220/220

Detail: `docs/sessions/2026-04-29-shader-2.3d-variant-key.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3c reflection consumption shipped.**

`spirv-reflect` now drives descriptor bindings, push-constant layout, vertex
input metadata, and material-parameter discovery from the canonical internal
SPIR-V modules. Reflection data is consumed into Cerid-owned effect/module
metadata with no public GLSL/SPIR-V/Vulkan leakage.

Three-flavour green:
- Debug: 216/216
- Release: 215/215 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 216/216

Detail: `docs/sessions/2026-04-29-shader-2.3c-reflection.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3b frontend → IR seam + GLSL ingest shipped.**

GLSL source file ingestion now compiles through a runtime-loaded `shaderc`
frontend into canonical internal SPIR-V modules, without leaking GLSL/SPIR-V/
Vulkan through the public API. Successful and failing compile paths are both
covered in tests.

Three-flavour green:
- Debug: 215/215
- Release: 214/214 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 215/215

Detail: `docs/sessions/2026-04-29-shader-2.3b-glsl-ingest.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3a public envelope shipped.**

Opaque handles, backend-neutral metadata types, minimal `Effect` / `Runtime`
interfaces, and an in-memory runtime seam proving effect/variant/reload
observability without leaking GLSL/SPIR-V/Vulkan through the public API.

Three-flavour green:
- Debug: 214/214
- Release: 213/213 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 214/214

Detail: `docs/sessions/2026-04-29-shader-2.3a-envelope.md`.

## Previous shipped milestone

**2026-04-29 — GPU memory + streaming foundation shipped.**

Centralized Vulkan allocator helper, backend-owned buffer/image allocation,
real image creation path, and allocator-backed smoke/test coverage. This is not
the final allocator architecture, but it stabilizes resource ownership before
shader/renderer growth.

Three-flavour green:
- Debug: 210/210
- Release: 209/209 (Debug-only stats test correctly skipped)
- ASan: 210/210

Detail: `docs/sessions/2026-04-29-gpu-memory-streaming-foundation.md`.

## Previous shipped milestone

**2026-04-28 — ImGui debug overlay shipped.**

Debug-only Dear ImGui layer over `crd-app` + `crd-rhi-vulkan`, configured via
`crd-config`. Docking on by default, multi-viewport off by default, theme and
panel visibility from `runtime/configs/imgui_layer.toml`, and a real
triangle+overlay smoke.

Three-flavour green:
- Debug: 210/210
- Release: 209/209 (Debug-only stats test correctly skipped)
- ASan: 210/210

Detail: `docs/sessions/2026-04-28-imgui-debug-overlay.md`.

## Previous shipped milestone

**2026-04-28 — `crd-config` core shipped.**

Typed TOML wrapper over `toml++` with non-fatal schema-with-defaults behavior:
typed `get<T>(key, fallback)`, typed `set<T>(key, value)`, parse errors via
`g_log_config`, dot-path nested lookup, sample TOML, and `smoke_config`.

Three-flavour green:
- Debug: 209/209
- Release: 208/208 (Debug-only stats test correctly skipped)
- ASan: 209/209

Detail: `docs/sessions/2026-04-28-config-core.md`.

## Previous shipped milestone

**2026-04-28 — First triangle on screen.**

Full RHI/Vulkan path real: instance → device → swapchain → command buffer →
shader modules → graphics pipeline → vertex buffer → draw → present.
Dynamic rendering chosen as the minimal path. Triangle stayed narrow: no
descriptors, materials, scene graph, camera system, or allocator policy
creep.

Three-flavour green:
- Debug: 203/203
- Release: 202/202 (Debug-only stats test correctly skipped)
- ASan: 203/203

Detail: `docs/sessions/2026-04-28-rhi-vulkan-first-triangle.md`.

## Next up (next 1–5 sessions)

1. **Phase 3.0 v1a** — `EntityId` + `SlotMap` + `World` shell. Locked design (ADR-0049). ~250 LOC + tests.
2. **Phase 3.0 v1b** — `ComponentRegistry` + `register_component` variadic-trait grammar. ~200 LOC + tests.
3. **Phase 3.0 v1c** — `ArchetypeChunkStorage`. ~600 LOC + tests. Largest slice; may sub-divide into v1c1 (chunk allocator + SoA layout) and v1c2 (archetype graph + entity move).
4. **Phase 3.0 v1d** — `SparseSetStorage`. ~250 LOC + tests.
5. **Phase 3.0 v1e** — Mixed-backend chunk visitor. ~200 LOC + tests.

After v1e the foundation is in place; v1f (Relations) → v1g (Query DSL) → v1h (System+Schedule) → v1i (Index framework + ChangeDetect + AsyncAware) → v1j (Transform + propagation) → v1k (SceneResource + Loader) → v1l (cook_scene handler) → v1m (sandbox renderer integration) → v1n (reserved-slot freeze) follow in order.

## Roadmap ordering

- **Phase 2.6** — `crd-resources` + asset cooker. ✅ COMPLETE (v1a–v1g, 2026-05-04). ADRs 0036–0041.
- **Phase 2.7** — Asset import bootstrap: `TextureResource` + `MeshResource` + glTF (cgltf) + material params + GPU upload + `crd-meshgen` + `crd-sandbox`. ADRs 0042–0043, 0045. ✅ COMPLETE.
- **Phase 2.8** — Material GPU wiring + sandbox rendering + asset import: per-material pipeline cache, multi-pass shader selection, depth prepass, default lit shaders, sandbox 3D rendering, glTF demo assets, cook-demo-assets target, unified Asset Browser. ADRs 0044, 0046. ✅ COMPLETE (v1a–v1g, 2026-05-06).
- **Phase 3.0** — Scene/ECS foundation: 8-layer slot-shaped architecture (Entity → Storage hybrid → Relations → Query/System/Schedule → Index framework → reserved L6–L8). 14 slices. Cerid signature: every novel ECS extension rides the L5 index framework. ADRs 0049–0057 locked 2026-05-06. **Architecture ready, awaiting user start command.**
- **Phase 3.0** — `crd-scene` / ECS (hybrid hierarchy + SoA components + TOML → binary serialization + renderer integration). Seven sub-ADRs needed before coding. Plugs into `crd-resources` as `SceneLoader`.
- **Phase 3.1** — Physics (PhysX 5 backend + scene integration + fixed-step + deterministic mode).
- **Phase 3.2** — Animation (skeletal, blend trees, IK). First rig + skin data in `MeshResource`.
- **Phase 3.3** — `crd-font` (MTSDF atlas, FreeType+msdfgen cooker, HarfBuzz shaping, billboard text, dynamic atlas, extruded text mesh). ADR-0047.
- **Phase 3.4** — Audio (spatialized, DAW plugin host scaffold).
- **Phase 3.5–3.7** — PBR + post-FX + culling (IBL, CSM, ACES, bloom, TAA, BVH, GPU-driven). Closes debt items 4–5.
- **Phase 4.0** — C++ hot-reload DLL scripting (ADR-0034).
- **Phase 4.2** — Networking: transport layer → deterministic simulation → client-server sync (ADR-0035).
- **Phase 8** — Domain modules: robotics, aerospace, cinematic, procedural generation — after Phase 4 + editor foundations.

Full plan: `docs/ROADMAP.md` → `docs/phases/`.

## Open questions

- `crd-config` hot-reload remains 1.6b unless ImGui integration proves it
  should move earlier.
- Runtime scene binary format — FlatBuffers vs Cap'n Proto? Park for
  Phase 3.1c.

## Test counts (last quality pass)

- win-debug:          481/481
- win-relwithdebinfo: 481/481
- win-release:        478/478
- win-asan:           481/481
- win-clang-cl:       481/481
- win-tidy:           ✅ build clean

(win-release is 3 fewer than debug: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`)

## Pointers (lazy-load reference)

Agents: don't read everything. Use these breadcrumbs.

- **Hub:** `docs/ROADMAP.md` (small navigation page; safe to read fully)
- **Principles:** `docs/PRINCIPLES.md` (read every session, short)
- **Active phase only:** `docs/phases/phase-2.8-material-completion.md` (v1a–v1e complete; active); `docs/phases/phase-2.7-asset-import.md` (reference; complete); `docs/phases/phase-2.6-resources.md` (reference; complete)
- **Other phases:** `docs/phases/phase-<X>.md` (read ONLY when relevant)
- **Specific decision:** `docs/decisions/<NNNN>-<slug>.md` (find via
  `docs/decisions/README.md` tag index)
- **Last session detail:** the single file linked above, not the whole
  `docs/sessions/` folder
- **Module overview:** `docs/systems/<module>.md` (when working on that
  module)
- **Module deep-dive:** `docs/<module>/<MODULE>_FILE.md` (only when doing
  surgery)
- **Open debt:** `docs/debt.md`
- **Detour queue + rules:** `docs/detours/README.md`

When in doubt, ASK before reading large files.

## Session log (rolling, last 5)

- **2026-05-06** — Phase 3.0 architecture locked. Nine ADRs accepted (0049–0057): Entity & SlotMap, Storage backends (Archetype + SparseSet hybrid), Relations as first-class, Query/System/Schedule, Component index slot framework, Transform hierarchy update, Scene serialization (TOML + SCEN CRDR — closes ADR-0020's FlatBuffers/Cap'n Proto deferral), Reserved L6–L8 slots, UI-in-tree boundary. Phase 3.0 doc rewritten around 8-layer architecture and 14 slices. ADR index + ROADMAP updated. Ready to start v1a on user command.
- **2026-05-06** — Sandbox async asset load: `load_sync<MeshResource>` swapped for `load_async` + per-frame `is_ready()` polling on `SandboxLayer`; pending-handle state (`m_pending_load`, `m_pending_index`) added; selection re-click is a no-op while same id is in flight; "Status: loading…" appears in the metadata pane until Ready; clicking a procedural shape mid-load drops the pending import. Shutdown order in sandbox/main.cpp updated to drain `jobs::shutdown()` *before* `app.detach_all_layers()` (ResourceManager outlives in-flight load jobs). GPU upload (`GpuUploader::upload_mesh`) still synchronous — the remaining hitch — formally tracked in `docs/debt.md` and pulled forward as a Phase 3.0 prerequisite (`docs/phases/phase-3.0-scene-ecs.md`). All 6 configs green (481/481 win-debug, 478/478 release).
- **2026-05-06** — `crd-math` interpolation primitives + Penner easings shipped: scalar lerp/mix/saturate/step/smoothstep/smootherstep/inverse_lerp/remap/damp added to scalar.hpp; mix + damp componentwise added to vec.hpp; new easing.hpp with 31 Penner curves (Linear + In/Out/InOut for Sine/Quad/Cubic/Quart/Quint/Expo/Circ/Back/Elastic/Bounce); sandbox `OrbitCamera` migrated to `crd::math::damp`; 8 new tests; all 6 configs green (481/481 win-debug, 478/478 release).
- **2026-05-06** — Phase 2.8 v1f+v1g SHIPPED: glTF demo asset bundle (BoxTextured CC-BY, Duck SCEA, BoomBox CC0) + procedural CC0 PNGs (checker_512, bricks_512); `cook-demo-assets` CMake target → `assets/cooked/demo_assets.crdr` (5 entries); unified Asset Browser ImGui panel (replaces Meshgen Browser, two collapsing sections, click-to-load via `ResourceManager::load_sync<MeshResource>`); device-destroy crash fix in `Application::detach_all_layers()` + sandbox/main.cpp shutdown reorder; `GpuUploader` taken from non-const& to const&; cooker `.meta.meta` chain-growth bug fixed (`scan_recursive` now filters via `name.ends_with(".meta")`, not first-dot extension); cooker no longer writes `.meta` sidecars for handler-less files (handler lookup moved before sidecar resolution); all 6 configs green (473/473 win-debug, 470/470 release). Phase 2.8 COMPLETE.
- **2026-05-05** — Phase 2.8 v1a–v1e SHIPPED: per-material pipeline cache + multi-pass begin_pass() + depth-only prepass pipeline + surface.vert/frag default lit shaders + SandboxPipelineResolver + sandbox render_scene() wired to ForwardRenderPath; Module::code_bytes() + Format::R32G32B32A32Sfloat added; SPV_REFLECT bug fixed; smoke_depth_prepass.exe; [[maybe_unused]] release fixes; all 6 configs green (471/471 win-debug). v1f+v1g deferred.
- **2026-05-05** — Phase 2.7 trailing items closed: surface_data.glsl.inc (GLSL contract for material shaders); docs/systems/texture_resource.md + mesh_resource.md + meshgen.md; phase-2.7 DoD marked complete; sandbox GPU rendering + demo assets formally deferred to Phase 2.8 (added to phase-2.8 doc).
- **2026-05-05** — Phase 2.7 v1e SHIPPED: crd-meshgen module (8 generators: plane/box/sphere/icosphere/cylinder/cone/capsule/torus); smoke_meshgen.exe headless; 11 unit tests; crd-sandbox Meshgen Browser ImGui panel (8 shapes, vertex/index/tri counts); clang-cl normalize3 unused-function fix; all 6 configs green (468/468 win-debug). Phase 2.7 COMPLETE.
- **2026-05-05** — Phase 2.7 v1d SHIPPED: GpuUploader (upload_texture→GpuTexture, upload_mesh→GpuMesh, staging-buffer pattern); RHI additions (copy_buffer, copy_buffer_to_image, submit_and_wait, VK_REMAINING_MIP_LEVELS fix); smoke_asset_import.exe (GPU smoke, graceful skip); crd-sandbox (OrbitCamera exponential-lerp, ImGui panel, --headless); all 6 configs green (457/457 win-debug).
- **2026-05-05** — Phase 2.7 v1c SHIPPED: full material system foundation (ADR-0048): MaterialTemplate + MaterialInstance, MaterialDomain, PassType, RasterState, CookedParameter, ShaderOptionDecl, MATR v2 artifact (INFO+PASS), legacy META backward-compat, cooker rewrite, renames; 5 new tests; smoke_material.exe; all 6 configs green (457/457 win-debug).
- **2026-05-05** — Phase 2.7 v1b SHIPPED: MeshResource + MeshResourceLoader + glTF cooker handler (cgltf, MikkTSpace, multi-mesh via ExtraArtifact); 4 new tests in test_mesh_loader.cpp; smoke_mesh.exe; all 6 configs green (452/452 win-debug).
- **2026-05-04** — Phase 2.7 v1a SHIPPED: TextureResource + MipLevel + TextureFormat + TextureResourceLoader + texture cook handler (stb_image, box-filter mip gen); 4 new tests; smoke_texture.exe; all 6 configs green (448/448 win-debug).
- **2026-05-04** — Phase 2.6 v1g SHIPPED: load_streamed + 2Q LRU eviction + memory budget + pinning; 5 new tests in test_eviction.cpp; smoke_resources_stream.exe; all 6 configs green (444/444 win-debug). Phase 2.6 COMPLETE.
- **2026-05-04** — Phase 2.6 v1e shipped: ShaderResourceLoader + MaterialResourceLoader + compile_glsl() + GLSL/material cooker handlers + smoke_resources_render; 6 new tests; all 6 configs green (435/435 win-debug). Clang-cl fix: removed dead `to_parameter_class_local`.
- **2026-05-04** — Phase 2.6 v1d shipped: AsyncFile + load_async<T> + fiber-cooperative wait_ready() + load coalescing; 9 new tests; all 6 configs green (429/429 win-debug).
- **2026-05-03** — Phase 2.6 v1c shipped: RefCounted<T> + ResourceHandle<T> + load_sync<T> + cycle detection; all 6 configs green (420/420 win-debug).
- **2026-05-03** — Phase 2.6 v1b shipped: zstd compression + cooker CLI + .bin handler + 4 tests; all 6 configs green (408/408 win-debug).
- **2026-05-03** — Debt cleared: SpscQueue<T> lock-free SPSC queue (+7 tests), FileWatcher polling mtime watcher (+4 tests), Doxygen per-symbol docs in crd-core, runtime-disabled log benchmark fix, multi-viewport ImGui moved to long-term deferred. 404/404 win-debug.
- **2026-05-03** — Phase 2.6 v1a shipped: `crd-resources` (ResourceId, CRDR, ResourceManager shell) + `asset_cooker manifest_dump`; String SSO remaining-capacity fix; all 6 configs green (393/393).
- **2026-05-02** — `crd-jobs` v1k integration smoke + crd-app wiring shipped; Phase 2.5 COMPLETE; all 6 configs green (355/355 win-debug).
- **2026-05-02** — `crd-jobs` v1j per-thread frame allocator shipped; 4 new tests; all 6 configs green (355/355 win-debug).
- **2026-05-02** — `crd-jobs` v1i SBO lambda helpers shipped; `make_job<F>()` + `parallel_for()`; 5 new tests; all 6 configs green (351/351 win-debug).
- **2026-05-02** — `crd-jobs` v1h Public API shipped; `jobs.cpp` + 5 new public-API tests; `Fiber::job_counter` field (fiber-survives-suspension counter fix); all 6 configs green (346/346 win-debug).
- **2026-05-02** — `crd-jobs` v1g Worker thread pool + main-thread fiber shipped; 10 new tests; TIB save/restore fix; fiber re-init fix; all 6 configs green (341/341 win-debug).
- **2026-05-02** — `crd-jobs` v1f Counter + wait mechanism shipped; 14 new tests; NDEBUG fix for Release; all 6 configs green (331/331 win-debug).
- **2026-05-02** — `crd-jobs` v1e Priority Scheduler shipped; 16 new tests; /EHsc fix; all 6 configs green (317/317 win-debug).
- **2026-05-02** — `crd-jobs` v1d Vyukov MPMC injection queue shipped; all 6 configs green (301/301 win-debug).
- **2026-05-02** — `crd-jobs` v1c Chase-Lev work-stealing deque shipped; all 6 configs green (291/291 win-debug).
- **2026-05-02** — `crd-jobs` v1b fiber pool shipped; renderer LTO fix; all 6 configs green (279/279 win-debug).
- **2026-05-01** — `crd-jobs` v1a hand-rolled asm context switch shipped (266/266 win-debug).
- **2026-05-01** — `crd-renderer` v1i swapchain blit + first full frame loop shipped (261/261 win-debug).
- **2026-05-01** — `crd-renderer` v1h index buffer + `draw_indexed` shipped (257/257 win-debug).
- **2026-05-01** — `crd-renderer` v1g `ForwardRenderPath` shipped (253/253 win-debug).
- **2026-05-01** — `crd-renderer` v1c frame graph v1 shipped (233/233 win-debug).
- **2026-05-01** — `crd-renderer` v1b real draw execution shipped.
- **2026-05-01** — `crd-renderer` v1a explicit renderables shipped.
- **2026-05-01** — `crd-shader` 2.3g pipeline handoff shipped.
- **2026-04-29** — `crd-shader` 2.3c reflection consumption shipped.
- **2026-04-29** — `crd-shader` 2.3b frontend → IR seam + GLSL ingest shipped.
- **2026-04-29** — `crd-shader` 2.3a public envelope shipped.
- **2026-04-29** — GPU memory + streaming foundation shipped.
- **2026-04-28** — ImGui debug overlay shipped.
- **2026-04-28** — `crd-config` core shipped.
- **2026-04-28** — First triangle through full RHI/Vulkan path.
- **2026-04-27** — `crd-rhi-vulkan` bootstrap (instance/device/surface/swapchain).
- **2026-04-26** — `crd-rhi` v1a scaffold with fake-backend tests.
- **2026-04-26** — `crd-app` Phase 1.5 shipped (LayerStack + propagated
  events + sync EventBus).
- **2026-04-25** — Platform v1c (input) shipped.

> Older entries: `docs/sessions/`.
