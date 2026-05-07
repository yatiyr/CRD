# 2026-05-07 — Phase 3.0 v1d: `SparseSetStorage` + World dispatch

**Status at start:** Phase 3.0 v1c (whole) + allocator-audit Option C shipped earlier the same day. Every byte the World allocates already routed through one `IAllocator`. Scene tests at 87 cases / 11024 assertions, six-config 603/603.

**Status at end:** Phase 3.0 v1d shipped. Both L2 storage backends (Archetype primary + SparseSet escape-hatch) live behind the same `IStorageBackend` interface. `World` dispatches by `StorageHint` at registration. `IStorageEventSink::on_entity_destroyed` now fires once per destroy (consolidated through World), not once per backend. Per-pool version counter pre-wired so v1i ChangeDetect doesn't have to retrofit. Six-config 618/618 / 615 release / 17 smokes. Scene tests 102 cases / 34420 assertions.

---

## Goal of this session

Land the **second L2 backend** per ADR-0050 §3:

> SparseSetStorage is the escape hatch.
> - Per-component-type pool: `sparse[entity_index] -> dense_index`, `dense[dense_index] -> T` + `dense[dense_index] -> entity_id`.
> - O(1) insert, O(1) remove (swap-with-last), O(1) lookup.
> - Used for components with one of these access patterns: high churn, sparse (<5%), or lookup-dominated.

The phase doc gave a 250-LOC band for the slice. Actual landed surface is ~970 LOC including World rewiring + tests.

## What shipped

### New module files

```
engine/scene/include/crd/scene/sparse_set_storage.hpp   ~115 LOC
engine/scene/src/sparse_set_storage.cpp                 ~340 LOC
tests/scene/test_sparse_set_storage.cpp                 ~440 LOC, 15 cases
```

### Modified

- `engine/scene/include/crd/scene/world.hpp`
  - New include: `<crd/scene/sparse_set_storage.hpp>`, `<crd/scene/storage_backend.hpp>`.
  - New member: `SparseSetStorage m_sparse_storage`.
  - New member: `IStorageEventSink* m_event_sink` (with `NullStorageEventSink::instance()` default).
  - `set_storage_event_sink` now installs the sink on **both** backends and keeps a copy at `m_event_sink`.
  - `add_component` / `has_component` / `get_component` / `get_component_mut` / `remove_component` dispatch to `backend_for(id)` based on `ComponentInfo::storage_hint`.
  - New accessor: `world.sparse_storage()` (mutable + const).
  - Private helpers: `backend_for(id)`, `backend_for_const(id)`.

- `engine/scene/src/world.cpp`
  - `destroy_immediate` and `flush_destroys` now call `m_event_sink->on_entity_destroyed(e)` ONCE before draining both backends. Backends never fire `sink->on_entity_destroyed` themselves any more.
  - Constructor adds `m_sparse_storage(alloc, m_components)` and `m_event_sink(NullStorageEventSink::instance())`.

- `engine/scene/src/archetype_chunk_storage.cpp`
  - **Contract change:** `on_entity_destroyed` no longer fires `m_sink->on_entity_destroyed(e)`. The line at the start of the function is removed and replaced with a comment explaining the World-level dispatch path. Per-component `on_remove` events still fire.

- `tests/scene/CMakeLists.txt` — added `test_sparse_set_storage.cpp`.
- `tests/scene/test_world_tlsf.cpp` — extended the lifecycle case to register a `DialogTrigger` (SparseSet) and assert pool count + entity count, locking the TLSF deployment property for both backends in one test.

## Design decisions

### Pool layout

```cpp
struct Pool
{
    const ComponentInfo*               info;     // captured at pool creation
    crd::memory::IAllocator*           alloc;    // World's IAllocator
    crd::containers::Array<crd::u32>   sparse;   // entity.index() -> dense_index | kInvalid
    crd::containers::Array<EntityId>   entities; // dense_index -> EntityId
    void*                              dense;    // count*info.size bytes, info.alignment-aligned
    crd::u32                           capacity, count;
    crd::u64                           version;  // bumped on insert/update/remove
};
```

Pools are allocated lazily on first insert via the World's `IAllocator`, indexed in `m_pools[c.raw]`. `m_pools` is sized to `kMaxComponents = 256` and zero-initialised. Pool count is bounded by 256 long-lived structs — direct allocation is the right shape, no GrowablePool needed (a pool of 256 ≤256-byte structs would be over-engineering).

Dense buffer grows exponentially × 2 (start = 8 slots). Sparse table grows lazily on first write to an entity index ≥ `sparse.size()`, fill = `kInvalid`.

### Why a per-pool version counter, shipped now

Advisor flagged this during planning: **pick now, not later, or v1i has to retrofit.** ADR-0050 §6 ties the archetype chunk version counter to ChangeDetect (ADR-0053). For SparseSet there's no chunk grain — the natural unit is per-pool. `Pool::version` is bumped on every insert / update / remove; v1i `ChangeDetectIndex` will read it to drive `.changed<T>()` at pool granularity. Cost: 8 bytes per pool, two `version += 1` lines per mutation. Saves a future re-touch.

### `for_each_chunk` semantics for SparseSet

Mirror of `ArchetypeChunkStorage::for_each_chunk`'s "yield archetype iff `required ⊆ archetype.mask`" rule. A SparseSet pool's effective mask is `{c}` (single bit), so `required ⊆ {c}` reduces to:

| `required` | Result |
|---|---|
| empty (0 bits) | yield every non-empty pool |
| `{c}` (1 bit) | yield exactly the c-pool if non-empty |
| 2+ bits set | yield nothing — a single SparseSet pool can't satisfy multi-bit AND |

Multi-bit pure-SparseSet intersection is **deferred to v1e** (mixed-backend chunk visitor). The phase doc places the unified iteration interface there explicitly.

### `IStorageEventSink::on_entity_destroyed` consolidation

Before v1d, `ArchetypeChunkStorage::on_entity_destroyed` fired `m_sink->on_entity_destroyed(e)` on entry. With v1d's two-backend World, naively keeping that pattern means the sink sees the event TWICE per destroy (once from each backend). That's wrong for any sink with refcounted bookkeeping (and `IComponentIndex` framework will have plenty).

Fix: the sink fires from `World::destroy_immediate` and `World::flush_destroys` ONCE, before either backend drains. Both backends are now responsible only for per-component `on_remove` events. Archetype storage's old line was deleted; SparseSet storage never had it.

Documented inline in `world.cpp` and `sparse_set_storage.cpp` so v1i (which adds the fan-out sink) doesn't re-introduce double-fire.

### UPSERT aliasing

When a component is already present and `insert` is called again, the path is destruct-old → move-construct-new → `on_update(e, c, slot, slot)` (same pointer for `old_data` and `new_data`). The sink contract at `storage_event_sink.hpp:39-40` permits aliasing; the actual old bytes are dead between destruct and move-construct anyway. Matches the archetype precedent at `archetype_chunk_storage.cpp:279`.

### Pool destructor ordering

`SparseSetStorage::~SparseSetStorage()` calls `destroy_all_pools()`, which:
1. For each live pool: runs `~Pool()` (which destructs live elements + frees the dense buffer through `alloc`, then `Array<u32> sparse` and `Array<EntityId> entities` member dtors free their buffers).
2. Calls `m_alloc->deallocate(slot)` to free the Pool struct.

Mirrors the `ArchetypeGraph` pattern landed earlier the same day in Option C.

### LOC band vs phase doc

Phase doc said "~250 LOC + tests." Actual code surface:

```
sparse_set_storage.hpp   ~115
sparse_set_storage.cpp   ~340
world.hpp / world.cpp    ~70 (delta)
test_sparse_set_storage  ~440 (15 test cases)
test_world_tlsf delta    ~10
                         ----
                         ~975 LOC
```

`sparse_set_storage.cpp` is bigger than the band because it carries explicit support for non-trivially-movable types (move-construct + destruct paths in grow/remove/swap), the per-pool version counter, and the explicit `for_each_chunk` semantics. Tests at 440 LOC mirror the v1c2 archetype storage test density.

## Trade-offs and what's deferred

- **Multi-component intersection on SparseSet alone** → v1e. The single SparseSet pool can't satisfy multi-bit `required` from `for_each_chunk`; v1e's mixed-backend visitor handles intersections (sparse-checking entities from one pool against the others).
- **Mixed-backend queries (Archetype + SparseSet on the same query)** → v1e.
- **`par_each` over SparseSet ranges** → v1g (Layer 4 query DSL).
- **Pool deallocation when count drops to 0** → not done. Pools persist for the World's lifetime once created. Matches the archetype-graph "ids never recycle" invariant; reserves the option to add a frame-coalesced reclaim later (mirror of archetype's "empty chunks freed only when trailing" rule).

## Bugs caught during integration

### CTest can't parse em-dashes in test names

Two test names had `—` (em-dash) and CTest's name-list parser reported them as failed. Same recurring issue documented in `CLAUDE.md` (Troubleshooting § "win-debug: tests fail due to ..."). Fixed by replacing both em-dashes with `-`.

### win-release LNK1169 from PCH staleness

First six-config sweep: win-release linker reported `World::set_storage_event_sink` defined in both `world.cpp.obj` and `test_archetype_storage.cpp.obj`. Cause: PCH cached the old in-class body of `set_storage_event_sink` (pre-v1d definition was inline in the class body); the new out-of-line definition in `world.cpp` collided with the cached body in another TU's PCH.

Fix: kept `set_storage_event_sink` inline (in-class body) in `world.hpp`. The function is small enough (3 lines) that an inline definition is appropriate. Avoids the PCH-staleness collision entirely.

### win-tidy: `_pad` member name violates identifier-naming rule

clang-tidy's `readability-identifier-naming` rejected `_pad` as an invalid public member name. Renamed to `padding`.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 618 / 618 | 17 / 17 |
| win-relwithdebinfo | clean | 618 / 618 | 17 / 17 |
| win-release        | clean | 615 / 615 | 17 / 17 |
| win-asan           | clean | 618 / 618 | 17 / 17 |
| win-clang-cl       | clean | 618 / 618 | 17 / 17 |
| win-tidy           | clean | — | — |

### Scene tests

- Pre-v1d (post-Option-C): 87 cases / 11024 assertions.
- Post-v1d: 102 cases / 34420 assertions (+15 cases / +23396 assertions).

The big jump in assertions is from the 10K-entity stress case (`SparseSetStorage: 10K-entity stress with random removes preserves data integrity`) which checks all 10000 entities.

## What this unlocks

Phase 3.0 v1e (mixed-backend chunk visitor + multi-component intersection) is now the natural next slice. v1d ships the SparseSet half of the L2 contract; v1e wires both backends behind a unified iteration that the v1g query DSL will sit on top of.

The slot-shaped ECS architecture's "registered slot" extensibility property is now visible in the public API:

```cpp
world.register_component<Transform>();                              // -> Archetype
world.register_component<DialogTrigger>(StorageHint::SparseSet);    // -> SparseSet
world.register_component<RecentlyHit>(StorageHint::SparseSet, AsyncAware{}); // future trait combos
```

Both `add_component<Transform>(e, t)` and `add_component<DialogTrigger>(e, dt)` go through the same `World::add_component` path — the dispatch is invisible to callers.

## Follow-ups

None opened by this slice. v1e is the next planned slice (mixed-backend chunk visitor, ~200 LOC + tests).

## Commit message proposal

```
feat(scene): SparseSetStorage L2 backend + World dispatch by StorageHint (v1d)

Phase 3.0 v1d ships the second L2 storage backend per ADR-0050 §3. One
pool per registered SparseSet component:
  sparse[entity.index()] -> dense_index | kInvalid
  dense (raw bytes)      -> T (count*info.size, info.alignment, exp ×2 grow)
  entities[dense_index]  -> EntityId
  + pool.version : u64 (pool-grain ChangeDetect for v1i)

O(1) insert / remove (swap-with-last) / lookup. for_each_chunk yields
per-pool chunks; multi-bit intersection deferred to v1e.

World dispatches add/has/get/get_mut/remove/destroy paths to the right
backend by ComponentInfo::storage_hint. on_entity_destroyed fan-out is
consolidated through World — sink fires once per destroy, both backends
emit per-component on_remove. Archetype storage's redundant
sink->on_entity_destroyed call is removed.

Six-config DoD: 618/618 (was 603 baseline). 17/17 headless smokes per
non-tidy config. 102 scene tests / 34420 assertions (was 87 / 11024).
```
