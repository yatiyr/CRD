# 2026-05-07 — Phase 3.0 v1c2: Archetype + ArchetypeGraph + ArchetypeChunkStorage + IStorageEventSink + typed World API

**Status at start:** Phase 3.0 v1a + v1b + v1c1 shipped earlier the same day. The chunk machinery (`ChunkLayout` / `ChunkHeader` / `Chunk` / `ChunkAllocator`) was in place. No archetype struct, no archetype graph, no entity-component bindings, no `add_component<T>`.

**Status at end:** v1c2 SHIPPED. The engine has its first real "entity owns components" capability. Archetype graph is memoised + zero-allocation on the hot navigation path. `IStorageEventSink` — the **Cerid L5 plug point** — is wired on every storage mutation with a `NullStorageEventSink` default. v1i and beyond can fan out to the `IComponentIndex` framework without touching any storage call sites or user code. 11 of 14 slices remain in Phase 3.0; v1d (SparseSet escape-hatch backend) is next.

---

## Goal of this session

Land the architectural moment Phase 3.0 was building toward: bind entities to components, walk archetypes coherently for queries, and put the lifecycle hook in place that every future ECS extension consumes. Per the user's brief: *"with cerid signature and as perfect as possible and better than or equal to elite engines."*

Elite-tier ECS storage characteristics (Bevy v2+ / Unity DOTS / flecs) are well-known:

1. Cache-coherent SoA iteration — chunks fit L1, version counters bump only on writes.
2. **Memoised archetype graph** — first lookup of `after_add(A, C)` walks + creates; subsequent calls are O(1) hash hit. Bevy and flecs both do this.
3. **Bytewise entity move** — storage doesn't know concrete C++ types; uses lifecycle ops captured at registration.
4. **O(1) entity location** — entity index → (archetype, chunk, slot) via dense parallel array.
5. **swap_remove** with last-entity location patch.
6. **Trailing-chunk-only free** — empty middle chunks stay; only last empty chunk gets returned to allocator (matches Bevy; avoids fragmentation under churn).
7. **Tag-only components** work out of the box (per-byte SoA on a 1-byte struct).
8. **Insert is upsert** — `add_component<T>(e, t1); add_component<T>(e, t2);` leaves t2 visible. Standard ECS verb.

Cerid signature additions on top of the elite baseline:

- **`IStorageEventSink`** — every storage mutation routes through one virtual interface. `NullStorageEventSink` is the default. v1i swaps in the `IComponentIndex` fan-out. The eight-layer architecture's "extensibility from day one" property is realised here: adding a new index in any consumer phase (Replication, History, SpatialBVH, GpuResident, Reflection, Scripts, Metrics, EditorSelection, …) is a single `IComponentIndex` impl + registration. **No storage call sites change. No user code changes.**

## What shipped

### New module files

```
engine/scene/include/crd/scene/
    archetype.hpp                ArchetypeId, EntityLocation, Archetype struct
                                 (mask, layout, chunks, dense edge tables sized
                                 kMaxComponents indexed by ComponentId.raw).
    archetype_graph.hpp          ArchetypeGraph: HashMap<ComponentMask, ArchetypeId>
                                 + 256-bit boost-style mask hash;
                                 archetype_for(mask), after_add(arch, c),
                                 after_remove(arch, c) — all memoised.
    storage_event_sink.hpp       IStorageEventSink + NullStorageEventSink.
                                 (THE Cerid L5 plug point.)
    archetype_chunk_storage.hpp  ArchetypeChunkStorage : IStorageBackend.
                                 Owns ChunkAllocator + ArchetypeGraph + m_locations.

engine/scene/src/
    archetype.cpp                ~30 LOC — edge-table init.
    archetype_graph.cpp          ~80 LOC — find-or-create + memoised edges.
    storage_event_sink.cpp       ~5  LOC — singleton accessor.
    archetype_chunk_storage.cpp  ~340 LOC — the load-bearing file.
    world.cpp                    extended: storage drains components on
                                 destroy_immediate / flush_destroys before
                                 the slot is freed.
```

### Modified

- `engine/scene/include/crd/scene/world.hpp` — typed templates `add_component<T>` / `has_component<T>` / `get_component<T>` / `get_component_mut<T>` / `remove_component<T>`. `World` is now non-movable (storage references registry via pointer).
- `engine/scene/include/crd/scene/storage_backend.hpp` — `insert` signature changed from `const void*` to `void*` (storage may move-from); `for_each_chunk` carries `void* user_data` (heap-free dispatch). ADR-0050 §1 amended in place.
- `engine/scene/include/crd/scene/scene.hpp` — umbrella picks up new headers.
- `engine/scene/src/world.cpp` — `destroy_immediate` / `flush_destroys` now route through `m_storage.on_entity_destroyed(e)` BEFORE `m_slots.free(e)`. The sink sees the entity-destroyed event while the slot is still alive (so a sink may iterate the entity's components one last time).
- `tests/scene/CMakeLists.txt` — adds `test_archetype_graph.cpp` + `test_archetype_storage.cpp`.

### Public API (high level)

```cpp
namespace crd::scene {

struct ArchetypeId { crd::u32 raw = 0xFFFFFFFFU; /* is_null, == */ };
inline constexpr ArchetypeId kInvalidArchetypeId{0xFFFFFFFFU};

struct EntityLocation {
    ArchetypeId archetype = kInvalidArchetypeId;
    crd::u32    chunk_index = 0;
    crd::u16    slot_in_chunk = 0;
    crd::u16    _reserved = 0;
};

struct Archetype {
    ArchetypeId id;
    ComponentMask mask;
    ChunkLayout layout;
    Array<Chunk> chunks;
    Array<ArchetypeId> add_edges;     // size = kMaxComponents
    Array<ArchetypeId> remove_edges;  // size = kMaxComponents
    explicit Archetype(IAllocator*);
    crd::u32 entity_count() const noexcept;
};

class ArchetypeGraph {
    ArchetypeGraph(IAllocator*, ChunkAllocator&, const ComponentRegistry&);
    Archetype& archetype_for(const ComponentMask&);
    Archetype& after_add(Archetype& src, ComponentId added);
    Archetype& after_remove(Archetype& src, ComponentId removed);
    Archetype* by_id(ArchetypeId);
    crd::u32 archetype_count() const noexcept;
};

class IStorageEventSink {
    virtual void on_insert(EntityId, ComponentId, const void* data) = 0;
    virtual void on_update(EntityId, ComponentId, const void* old, const void* new_) = 0;
    virtual void on_remove(EntityId, ComponentId, const void* data) = 0;
    virtual void on_entity_destroyed(EntityId) = 0;
};
class NullStorageEventSink : public IStorageEventSink { /* singleton, all no-ops */ };

class ArchetypeChunkStorage : public IStorageBackend {
    ArchetypeChunkStorage(IAllocator*, const ComponentRegistry&);
    void  insert(EntityId, ComponentId, void* data) override;        // upsert
    void  remove(EntityId, ComponentId) override;
    bool  has(EntityId, ComponentId) const override;
    void* get_mut(EntityId, ComponentId) override;                   // bumps version
    void  for_each_chunk(ComponentMask required, ChunkVisitor, void*) override;  // walks supersets
    void  on_entity_destroyed(EntityId) override;

    const void*    get_const(EntityId, ComponentId) const;           // does NOT bump version
    EntityLocation location(EntityId) const noexcept;
    ArchetypeGraph& graph() noexcept;
    void            set_event_sink(IStorageEventSink*) noexcept;
};

class World {
    /* v1a/v1b carry-over */
    template <typename T> void add_component(EntityId e, T value);   // upsert
    template <typename T> bool has_component(EntityId e) const noexcept;
    template <typename T> const T* get_component(EntityId e) const;
    template <typename T> T* get_component_mut(EntityId e);
    template <typename T> void remove_component(EntityId e);

    ArchetypeChunkStorage& storage() noexcept;
    crd::u32 archetype_count() const noexcept;
    void set_storage_event_sink(IStorageEventSink*) noexcept;
};

}
```

## Design choices made and why

### `IStorageEventSink` — the Cerid L5 plug point (the signature)

Every storage mutation calls into `m_sink->on_insert` / `on_update` / `on_remove` / `on_entity_destroyed`. `m_sink` defaults to a `NullStorageEventSink` singleton. v1i wires the real sink that fans events out to all registered `IComponentIndex` instances.

The architectural payoff: in v1c2 we are in an "indexes don't exist yet" phase. Adding the dispatch hooks now, with a no-op default, means:

- v1i implements `IComponentIndex` framework + a fan-out sink. Storage call sites stay untouched.
- Phase 3.2 implements `HistoryIndex<N>`. Just register it with the World. No storage code changes.
- Phase 3.5 implements `SpatialBVHIndex`. Same.
- Phase 3.8 implements `GpuResidentIndex`. Same.
- Phase 4.2 implements `ReplicationIndex`. Same.
- Phase 7 implements `Reflection` walker. Same.
- Phase 4.0 implements `ScriptComponent` system. Same.

This is what "extensible from day one" means in code, not just on a slide.

### Edge tables are dense `Array<ArchetypeId>` indexed by `ComponentId.raw`, not `HashMap<ComponentId, ArchetypeId>`

Bevy's archetype graph uses HashMap. But edge cache lookups happen on every `add_component<T>(e, T{})` — i.e. per mutation. At Cerid's million-entity scale, the per-archetype edge cache is hot.

Trade-off:
- HashMap: ~50 ns/lookup (hash + probe). Heap allocation per edge insert.
- `Array<ArchetypeId>(kMaxComponents)`: ~5 ns indexed load. Zero allocation on lookup. 1 KB/archetype overhead.

At 1000 archetypes (a large scene), the 1 MB of edge metadata is invisible. The 10× hot-path speedup on edge navigation is what users feel.

The advisor flagged this as a watch-in-implementation item; we committed to the dense-array form during planning.

### Bytewise entity move via lifecycle ops captured in v1b

`ArchetypeChunkStorage::move_shared_components` walks the merge of `src.layout.components_sorted` and `dst.layout.components_sorted` (both sorted ascending by ComponentId — two-pointer walk). For each shared component, it calls the registry's `move_construct(dst_bytes, src_bytes)` callback. After the move, the source slot is destructed. The bytes at the source slot must NOT be read after destruct — comment inline.

This is the only correct approach: storage doesn't know concrete C++ types. The trick is that v1b's `if constexpr` capture of `move_construct` / `destruct` makes this type-safe at registration time; the storage just calls the function pointer.

For trivially-movable types where v1b somehow chose not to capture an op, there's a `memcpy` fallback. v1b currently captures unconditionally for movable types, so the fallback is unreachable in practice — kept defensively.

### `insert` is UPSERT — second insert overwrites in place

ADR-0050 didn't specify; advisor flagged the question. Picked option (a) — upsert. `add_component<Transform>(e, t1); add_component<Transform>(e, t2);` leaves t2 visible. Most natural ECS verb. The in-place upsert path destructs the existing slot then move-constructs the new value into it; the version counter bumps; a single `on_update` event fires (not on_remove + on_insert).

### `for_each_chunk` walks SUPERSET archetypes

`required = {Position}` matches every archetype whose mask is `⊇ {Position}` — `{Position}`, `{Position, Velocity}`, `{Position, Renderable, Velocity}`, etc. NOT just exact-match. The visitor's `ChunkView::present_mask` carries the actual archetype mask so the visitor knows what's beyond the required set. v1g's query DSL (`world.query<Pos, Vel>().with<Visible>().without<Hidden>()`) builds on top.

Test pinned this explicitly with three entities split across `{Pos}`, `{Pos, Vel}`, `{Vel}` archetypes — required `{Pos}` matches first two only.

### `get_mut` bumps the version counter; `get_const` does not

Standard Bevy/DOTS chunk-grain semantics. ADR-0053 §3 explicitly accepts chunk-grain false positives — the consumer (ChangeDetect) skips already-handled entities cheaply, and the chunk-grain tracking eliminates 99%+ of redundant work. Per-entity precision is reserved as a future opt-in (`PreciseChangeDetect{}`).

### Trailing-chunk-only free

When entity removal drops a chunk to 0 entities AND it's the last chunk in the archetype's `chunks` array, the chunk is freed back to the `ChunkAllocator`. Mid-archetype empty chunks are kept allocated (matches Bevy — avoids fragmentation under steady-state churn, where the user spawns batch A, destroys batch A, spawns batch B, destroys batch B, ...). The chunk allocator's O(N) `free` (flagged in v1c1) doesn't hot-spot at this rate.

### `EntityLocation` array lives in storage, lazy-resized on access

Pinned by advisor in planning. Advantages:
- Storage owns its own lookup; `World::spawn` doesn't need to notify.
- v1d's `SparseSetStorage` can own a separate location structure (per-component sparse arrays — different shape; no conflict).
- Lazy resize is one `if (idx >= size) m_locations.resize(idx + 1)`. No coordination with SlotMap growth.

### `IStorageBackend::insert` takes `void* data`, not `const void*`

ADR-0050 §1 amendment alongside v1b's `void* user_data` addition. The honest API contract: storage may move-from the source value via the registry's `move_construct` callback. With `const void*`, every move-construct call site needed a `const_cast` — clang-tidy flagged it (`cppcoreguidelines-pro-type-const-cast`), and the cast was hiding a real semantic mismatch. Renamed to `void*`; ADR-0050 updated in place; the `IStorageBackend` stub in `test_component_registry.cpp` updated to match.

### World is non-movable

`m_storage` holds a `const ComponentRegistry*` back-pointer to `m_components`. If `World` were movable, the storage's pointer would be left dangling. The clang-cl error (`-Wdefaulted-function-deleted`) caught this — we explicitly delete the move ctor + move assignment. `World` stays heap-allocated or stack-allocated for the lifetime of its members. Standard pattern for resource-owning roots.

## Tests added (`tests/scene/`, +27 cases / +1345 assertions)

`test_archetype_graph.cpp` (10):
- Fresh graph empty; `by_id(null)` returns nullptr.
- `archetype_for(empty mask)` returns the empty archetype (id 0).
- `archetype_for` is memoised; same mask → same archetype reference + id.
- Different masks → different archetypes.
- `after_add` navigates A → A∪{C}; new mask correct.
- `after_add` of an existing component is idempotent.
- `after_add` edge memoisation — second call O(1); reverse `remove_edges` primed.
- `after_remove` navigates A → A\\{C}.
- `after_remove` of an absent component is idempotent.
- Edge tables sized to `kMaxComponents`; default entries `null`.

`test_archetype_storage.cpp` (17):
- Empty World: no archetypes, location invalid for any entity.
- `add_component<T>` creates archetype; entity placed at chunk 0 slot 0; value populated.
- `get_component_mut` writes round-trip via `get_component`.
- `add_component` is upsert — second add overwrites; no new archetype.
- Adding a second component moves entity to a new archetype; both components survive.
- `remove_component` moves entity to (mask & ~T).
- Removing the last component clears `EntityLocation`.
- Two entities in the same archetype get distinct slots.
- `swap_remove` updates trailing entity's location.
- Chunk fill+spill: 1500 entities allocate ≥ 2 chunks.
- `destroy_immediate` clears storage location and tears down components.
- `flush_destroys` tears down components for queued entities.
- `for_each_chunk` visits SUPERSET archetypes only.
- Version counter bumps on insert + on `get_mut`; does NOT bump on `get_const`.
- Default storage uses `NullStorageEventSink` (no crash on full lifecycle).
- Custom `CountingSink` records insert / update / remove / destroyed counts correctly.
- Sink receives `on_entity_destroyed` once + per-component `on_remove` events at destroy.

## Six-configuration sweep

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | ✅ | 563/563 | 17/17 |
| win-relwithdebinfo | ✅ | 563/563 | 17/17 |
| win-release        | ✅ | 560/560 | 17/17 |
| win-asan           | ✅ | 563/563 (DLL PATH fix) | 17/17 |
| win-clang-cl       | ✅ | 563/563 | 17/17 |
| win-tidy           | ✅ | — | — |

Test count delta: +27 (was 536/536 / 533/533 in win-debug / win-release pre-v1c2).

`win-release` initially failed on three v1a tests (`destroy_immediate of stale handle`, `Double destroy`, `Iteration after destroy/flush`) due to **stale `.obj` files** from v1c1's smaller `World` layout. A `--clean-first` rebuild resolved it — classic LTCG header-change-but-objs-not-rebuilt pattern documented in `CLAUDE.md` (the C4789 / stale-sizeof issue, applied to a different test suite).

`win-clang-cl` initially failed on two `-Wdefaulted-function-deleted` errors (World's defaulted move ctor was implicitly deleted because `ArchetypeChunkStorage` is non-movable) and one `-Wunused-private-field` (`m_alloc` stored in storage but never used). Fixed both: `World` move ctor/assignment explicitly deleted; `m_alloc` removed from storage (allocator passed through to members at construction; no need to retain).

`win-tidy` initially flagged: two `cppcoreguidelines-pro-type-const-cast` warnings (resolved by changing `IStorageBackend::insert` from `const void* data` to `void* data`); one `readability-isolate-declaration` (split `crd::u32 i_src = 0, i_dst = 0;` to two lines); identifier naming on test variables (sed-renamed `A_arch` → `a_arch`, `AB_arch` → `ab_arch`, etc.); `f` → `F` floating-point suffix; one `readability-redundant-casting` on a `static_cast<void*>` after the API change; em-dash and `∪` characters in `TEST_CASE` names (CTest filter is encoding-sensitive — replaced with ASCII `-` and "union"). Final tidy build clean.

`clang-format -i` run on all 13 new + modified files; reformat applied; rebuild + retest verified clean.

## What's next — v1d

`SparseSetStorage` (~250 LOC + tests, ADR-0050). The escape-hatch backend for components flagged `StorageHint::SparseSet` — high-churn / sparse / lookup-dominated. Per-component pool (`sparse[entity_index] -> dense_index`, `dense[dense_index] -> T`, `dense[dense_index] -> entity_id`). O(1) insert / remove (swap-with-last) / lookup / iteration over the dense array.

Implements the same `IStorageBackend` interface. The query layer (v1g) and index dispatcher (v1i) walk both `ArchetypeChunkStorage` and `SparseSetStorage` uniformly. v1e ships the mixed-backend chunk visitor that completes the unified iteration story.

## Files touched

```
A  engine/scene/include/crd/scene/archetype.hpp
A  engine/scene/include/crd/scene/archetype_graph.hpp
A  engine/scene/include/crd/scene/archetype_chunk_storage.hpp
A  engine/scene/include/crd/scene/storage_event_sink.hpp
A  engine/scene/src/archetype.cpp
A  engine/scene/src/archetype_graph.cpp
A  engine/scene/src/archetype_chunk_storage.cpp
A  engine/scene/src/storage_event_sink.cpp
A  tests/scene/test_archetype_graph.cpp
A  tests/scene/test_archetype_storage.cpp
M  engine/scene/include/crd/scene/scene.hpp           (umbrella picks up new headers)
M  engine/scene/include/crd/scene/world.hpp           (typed templates; non-movable; storage owner)
M  engine/scene/include/crd/scene/storage_backend.hpp (insert: const void* → void*; for_each_chunk: + user_data)
M  engine/scene/src/world.cpp                         (destroy paths route through storage)
M  tests/scene/CMakeLists.txt                         (two new test files)
M  tests/scene/test_component_registry.cpp           (StubBackend insert signature update)
M  CONTEXT.md                                         (current focus → v1d; new last-shipped entry)
M  docs/phases/phase-3.0-scene-ecs.md                 (v1c2 ✅; matrix; slice description)
M  docs/decisions/0050-scene-storage-backends.md      (insert + for_each_chunk amendments)
M  docs/systems/scene.md                              (v1c2 API + usage example)
M  docs/systems/README.md                             (crd-scene row → v1c2)
A  docs/sessions/2026-05-07-scene-v1c2-archetype-storage.md (this file)
```

## Proposed commit message

```
feat(scene): v1c2 — Archetype + Graph + Storage + Event Sink + typed World API

Land Phase 3.0 v1c2 (ADRs 0050, 0053). The engine has its first real
"entity owns components" capability.

Archetype: mask + ChunkLayout + dense Array<Chunk> + dense
Array<ArchetypeId> edge tables (sized kMaxComponents, indexed by
ComponentId.raw — 1 KB/archetype, zero-allocation O(1) navigation).

ArchetypeGraph: HashMap<ComponentMask, ArchetypeId> + 256-bit
boost-style hash; archetype_for/after_add/after_remove all memoised.
Pointer stability via Array<unique_ptr<Archetype>>.

ArchetypeChunkStorage : IStorageBackend. Insert is upsert (second
add_component<T> overwrites in place). Bytewise entity move via the
registry's lifecycle ops (move_construct + destruct). swap_remove
patches trailing entity's location. Trailing-chunk-only free
(matches Bevy). for_each_chunk walks SUPERSET archetypes (the
foundation v1g's query DSL builds on).

IStorageEventSink (the Cerid L5 plug point): every mutation calls
on_insert/on_update/on_remove/on_entity_destroyed through one
virtual interface. NullStorageEventSink default in v1c2; v1i swaps
in the IComponentIndex fan-out without touching storage call sites.
Future indexes (Replication, History, SpatialBVH, GpuResident,
Reflection, Scripts) are one-day extensions in their consumer phases.

World gains typed templates: add_component<T>(e, value),
has_component<T>(e), get_component<T>(e), get_component_mut<T>(e)
(declared write — bumps chunk version), remove_component<T>(e).
World is non-movable (storage references registry).

ADR-0050 §1 amended in place: insert takes void* (storage may
move-from); for_each_chunk takes opaque void* user_data.

27 unit tests / 1345 assertions added. Six-config green: win-debug
563/563, win-relwithdebinfo 563/563, win-release 560/560,
win-asan 563/563, win-clang-cl 563/563, win-tidy clean. 17/17
headless smokes per non-tidy config.
```
