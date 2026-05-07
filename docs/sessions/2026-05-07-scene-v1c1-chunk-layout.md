# 2026-05-07 — Phase 3.0 v1c1: archetype chunk allocator + SoA layout + per-chunk version counters

**Status at start:** Phase 3.0 v1a + v1b shipped earlier in the day. `crd-scene` had `EntityId` / `SlotMap` / `World`, `ComponentRegistry`, `IStorageBackend` interface (declared only), 256-bit `ComponentMask`. No archetypes, no chunks, no entity-component bindings.

**Status at end:** v1c1 SHIPPED. The chunk machinery (layout calc, header, raw chunk, allocator) is in place. v1c was split into v1c1 (this session) and v1c2 (next session) per phase doc allowance — keeps reviewable surface tight and v1c2 doesn't refactor v1c1.

---

## Goal of this session

Phase doc said:
> v1c — `ArchetypeChunkStorage` (~600 LOC + tests). 16 KB chunks, 64-byte aligned SoA, archetype graph (memoised lazy build), per-chunk version counters. **May further split into v1c1 (chunk allocator + SoA layout) and v1c2 (archetype graph + entity move) if scope grows.**

Once the architecture got concrete, three things made the split worthwhile:

1. v1c1 is data-structures-only — testable in isolation, no entity lifecycle dependencies.
2. v1c2 is mutators — needs `Archetype` struct + entity location array + `IStorageBackend` impl + `World::add_component<T>`. That's a different surface area.
3. The chunk machinery is what the next 12 slices consume; isolating it means design choices about layout-local vs global indexing, header size, and sentinel-on-failure are reviewable independently of the archetype-graph design.

So: ship v1c1 alone. Tight scope: ~250 LOC + ~150 LOC tests. End state: I can compute a chunk layout for any `ComponentMask`, allocate aligned 16 KB chunks, place entities in them via the layout's offset table, and the per-chunk version-counter array is ready for `ChangeDetectIndex` (v1i).

## What shipped

### New header (`engine/scene/include/crd/scene/archetype_chunk.hpp`)

```
constants    kChunkSize = 16 KB,
             kChunkAlignment = 64,
             kMaxComponentsPerArchetype = 32 (per-archetype, not per-World)

ChunkHeader  At byte 0 of every chunk: entity_count, entity_capacity,
             archetype_id back-ref (populated by Archetype in v1c2),
             version_counter[kMaxComponentsPerArchetype].

ChunkLayout  Per-archetype byte plan:
               components_sorted (sorted by ComponentId — canonical identity),
               sizes[i], alignments[i], offsets[i],
               entity_id_offset, entity_capacity (0 ⇒ invalid).
             is_valid() returns entity_capacity > 0.

compute_chunk_layout(mask, registry, alloc) → ChunkLayout

Chunk        Dumb 16 KB block + accessors:
               header(), entity_id_array(layout), component_array(layout, idx).
             No add_entity / remove_entity / version-bump methods —
             those live on Archetype in v1c2.

ChunkAllocator  allocate() → 64-byte aligned 16 KB block (header zeroed),
                free(chunk) → returns memory to IAllocator + clears chunk pointer,
                outstanding() → diagnostic count,
                ~dtor frees outstanding (verified under win-asan).
```

### New source (`engine/scene/src/archetype_chunk.cpp`)

`compute_chunk_layout` + `ChunkAllocator` impl. ~120 LOC.

### Modified

- `engine/scene/include/crd/scene/scene.hpp` — umbrella picks up `archetype_chunk.hpp`.
- `tests/scene/CMakeLists.txt` — added `test_archetype_chunk.cpp`.

## Design choices made and why

### Per-archetype cap (32) is independent of per-World cap (256)

`ComponentRegistry` allows up to `kMaxComponents = 256` distinct component types per World. `kMaxComponentsPerArchetype = 32` is a tighter ceiling on the *width of a single archetype* — i.e. how many components one entity can carry simultaneously.

The reason for the split: the per-chunk `version_counter` array sits *inside* the chunk header, sized in advance. Picking 256 (matching the World cap) burns 2 KB per chunk on counters that almost never see a write — wasteful. Unity DOTS uses ~128 components/archetype max with similar header math; 32 is comfortable for the ECS workloads Cerid targets (most archetypes have 2–8 components).

If a layout's component count exceeds 32, `compute_chunk_layout` returns an invalid layout (`entity_capacity == 0`). v1c2 will use this signal to reject the registration at archetype-creation time.

### `version_counter` is layout-local indexed, not global `ComponentId`

ADR-0050 §2 specifies `version_counter[kMaxComponentsPerArchetype]`, which I read literally: indexed by the archetype's *layout-local* component index (0..K-1 where K is the archetype's component count), not by the world-global `ComponentId`.

The mapping (layout-local index ↔ ComponentId) lives in `ChunkLayout::components_sorted`. ChangeDetectIndex (v1i) uses the layout-local index to bump the counter on writes. This keeps the counter array sized by the per-archetype cap (256 B at 32 × u64), not by the per-World cap (2 KB at 256 × u64).

ADR-0053 §8 was previously amended to say "256-bit AND" for the observed-mask check (for the *index dispatch* path, not the version counter); the version-counter path uses the layout-local indexing because it's hot-path per-component-write.

### `Chunk` is a dumb 16 KB block — no entity lifecycle methods

I considered three options for where chunk operations live:

- (a) `Chunk` stores its own `ChunkLayout*` — wastes a pointer per chunk.
- (b) Every `Chunk` method takes `const ChunkLayout&` — clunky but stays POD.
- (c) `Chunk` stores nothing; `Archetype` (v1c2) is the only thing that touches chunks and owns the layout.

Picked (c). v1c1 ships only `header()`, `entity_id_array(layout)`, `component_array(layout, idx)` accessors. `add_entity`, `remove_entity`, `bump_version` move to v1c2's `Archetype` because they need both the layout *and* the entity-location array (entity → (archetype, chunk, slot)).

Side effect: v1c1 is small and reviewable, and v1c2 doesn't have to refactor anything from v1c1 when it adds entity ops.

### Capacity-0 sentinel instead of `CRD_FATAL` from `compute_chunk_layout`

Two failure modes for layout computation:

1. Component count > `kMaxComponentsPerArchetype`
2. Single entity does not fit a chunk (sum of sizes too large)

Sketched first as `CRD_FATAL` inside the function. Walked back to "return invalid layout" because:

- v1c2 will want to reject at *registration time* (`World::register_component<T>(...)` should fail if `T` alone won't fit), and that needs a recoverable error path, not a hard trap.
- Tests can assert on `is_valid() == false` rather than catching a fatal.
- The caller decides recovery — `compute_chunk_layout` is just a layout calculator.

### Capacity calc: conservative estimate then iterate

The exact capacity depends on alignment padding at each SoA boundary, which depends on the running offset, which depends on the previous capacity choice — circular. Simplest correct approach:

1. Compute a conservative initial estimate: `(kChunkSize - aligned_header - 63 × comp_count) / per_entity_bytes`.
2. `layout_fits(capacity, ...)` writes offsets and verifies the running offset never exceeds `kChunkSize`.
3. If the estimate doesn't fit, decrement and retry.

For typical archetypes (2–8 components, 16-128 byte components), the estimate fits on the first try or within ~1 decrement. For pathological cases (32 huge components), it may iterate a handful of times — still O(comp_count × small_constant), invisible at registration time.

## Tests added (`tests/scene/test_archetype_chunk.cpp`, 11 cases / 88 assertions)

- Empty mask → layout has no SoA, only entity-id array; capacity = `(kChunkSize - entity_id_offset) / sizeof(EntityId)`.
- Single component → capacity > 0; offsets are 64-byte aligned; total bytes fit `kChunkSize`.
- Two components → both arrays 64-byte aligned, total fits, component count ≤ cap.
- Sort invariant: registering Position/Velocity/Renderable in that order, then setting them in the mask in *reverse* order — `components_sorted` still ascending; offsets monotonically increasing.
- Component count > `kMaxComponentsPerArchetype` (33) → invalid layout (entity_capacity == 0). Uses `TestSlot<N>` template trick to register 33 distinct types.
- `ChunkAllocator::allocate` returns 64-byte aligned memory; header is zeroed (entity_count, entity_capacity, archetype_id, all version counters).
- Multiple chunks have distinct memory; outstanding count tracks correctly.
- `ChunkAllocator::free` clears chunk pointer + decrements outstanding count.
- `~ChunkAllocator` frees outstanding chunks — verified under win-asan (3 allocated, 0 freed by hand → no leak).
- `Chunk::header` / `entity_id_array` / `component_array(layout, 0)` return correct pointers (offset arithmetic verified against `chunk.memory`).
- `static_assert` that `version_counter` array is sized exactly `kMaxComponentsPerArchetype × u64`.

## Six-configuration sweep

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | ✅ | 536/536 | 17/17 |
| win-relwithdebinfo | ✅ | 536/536 | 17/17 |
| win-release        | ✅ | 533/533 | 17/17 |
| win-asan           | ✅ | 536/536 (DLL PATH fix applied) | 17/17 |
| win-clang-cl       | ✅ | 536/536 | 17/17 |
| win-tidy           | ✅ | — | — |

Test count delta: +11 (was 525/525 / 522/522 in win-debug / win-release pre-v1c1).

`clang-format -i` run on all three new files; reformat applied; rebuild + retest verified clean.

## What's next — v1c2

`Archetype` struct (mask + layout + chunk list + entity-location array). `ArchetypeGraph` (`HashMap<ComponentMask, Archetype*>` + memoised add/remove edges; the edges let "add component X to entity in archetype A" navigate from A to A∪{X} in O(1) after the first lookup). `ArchetypeChunkStorage` implements `IStorageBackend`. `World` gains `add_component<T>(e, value)`, `has_component<T>(e)`, `get_component<T>(e)`, `remove_component<T>(e)`.

Per-chunk version counters get bumped by the storage's `insert` / `set` paths (layout-local index, automatic).

Out-of-scope until later slices: parallel iteration (`par_each` in v1g), `on_entity_destroyed` dispatch coupling with `SlotMap::flush_destroys` (v1c2 wires the storage side; full Layer-5 index dispatch lands in v1i).

### Known perf characteristic to revisit

`ChunkAllocator::free` is O(outstanding) — linear scan of `m_blocks` to find the chunk to release. Fine at v1c1 scale (a single test allocator holds at most a handful of chunks). Once v1c2's `Archetype::pop_chunk` calls `free` on the trailing chunk during archetype shrink, and a sandbox scene hits hundreds of archetypes × tens of chunks, this could hot-spot. Revisit if v1c2 stress reveals it; the swap-remove makes a fix cheap (intrusive linked list inside the chunk header, or just `Chunk` carrying its own block-array index).

## Files touched

```
A  engine/scene/include/crd/scene/archetype_chunk.hpp
A  engine/scene/src/archetype_chunk.cpp
A  tests/scene/test_archetype_chunk.cpp
M  engine/scene/include/crd/scene/scene.hpp           (umbrella picks up new header)
M  tests/scene/CMakeLists.txt                         (add test_archetype_chunk.cpp)
M  CONTEXT.md                                         (current focus → v1c2; new last-shipped entry)
M  docs/phases/phase-3.0-scene-ecs.md                 (v1c1 ✅; matrix row added; v1c slice description split into v1c1 + v1c2)
M  docs/systems/scene.md                              (status, slice table)
M  docs/systems/README.md                             (crd-scene row → v1c1)
A  docs/sessions/2026-05-07-scene-v1c1-chunk-layout.md (this file)
```

## Proposed commit message

```
feat(scene): v1c1 — archetype chunk allocator + SoA layout

Land Phase 3.0 v1c1 (split from v1c per phase doc allowance):
ChunkLayout + ChunkHeader + Chunk + ChunkAllocator. ADR-0050.

Per-archetype cap (kMaxComponentsPerArchetype = 32) is independent
of the per-World cap (kMaxComponents = 256) so the in-chunk
version-counter array stays compact (256 B). version_counter is
layout-local indexed (0..K-1 where K = this archetype's component
count), not global ComponentId — saves ~1.7 KB per chunk and matches
ADR-0050 §2's literal struct.

Chunk is a dumb 16 KB block with header() / entity_id_array(layout) /
component_array(layout, idx) accessors. No entity ops; those live on
Archetype in v1c2 because they need both the layout and the
entity-location array. compute_chunk_layout returns an invalid layout
(entity_capacity == 0) on failure rather than CRD_FATAL — caller
decides recovery (v1c2 will reject at registration time).

11 unit tests / 88 assertions added. Six-config green: win-debug
536/536, win-relwithdebinfo 536/536, win-release 533/533, win-asan
536/536 (with DLL PATH fix), win-clang-cl 536/536, win-tidy clean.
17/17 headless smokes per non-tidy config.
```
