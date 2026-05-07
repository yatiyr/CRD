# 2026-05-07 — Allocator-audit Option C: Archetype pool + TLSF-backed World test

**Status at start:** D-001 closed (TLSF + GrowablePool + ChunkAllocator pooled). Allocator audit done. Audit identified one place where Phase 3.0 still bypassed the IAllocator chain: `ArchetypeGraph` allocated `Archetype` structs via `std::make_unique<Archetype>` (global `operator new`), even though every other byte of the World already routed through `m_alloc`.

**Status at end:** Bypass closed. Archetype structs now flow through a dedicated `GrowablePoolAllocator` whose parent is the World allocator. A new `tests/scene/test_world_tlsf.cpp` proves the deployment pattern end-to-end: a `TlsfAllocator` is the only heap the `World` ever touches. Six configurations green at 603/603 (was 598/598 before this session).

---

## Goal of this session

User picked **Option C** from the allocator-audit report:

> (B) "Fix the bypass AND pool Archetypes via GrowablePool"
> + Layer 3: a TLSF-backed-World example/test demonstrating the deployment pattern.
> ~110 LOC change.

Three concrete layers:

1. **Layer 1 — fix the bypass.** Replace `Array<unique_ptr<Archetype>>` with manually-allocated raw `Archetype*` so the allocation path goes through an `IAllocator`, not `operator new`.

2. **Layer 2 — pool the Archetypes.** Allocate Archetype structs from a `GrowablePoolAllocator(slot_size = sizeof(Archetype), slot_alignment = alignof(Archetype), slots_per_page = 32, parent = m_alloc)`. One allocation amortises 32 archetypes; ~32 pages cover a 1000-archetype scene.

3. **Layer 3 — TLSF-backed-World test.** A new test file constructs a `TlsfAllocator` with a 16 MB pool, hands it to `World`, runs a full ECS lifecycle, verifies that destruction returns every byte to the TLSF pool. ASan-clean.

The motivation is the deployment promise that has been driving the memory work since Phase 1: **a Cerid scene can run on a fixed-size, pre-allocated heap.** Before this session that was true everywhere except for Archetype struct allocations. After this session it is true end-to-end.

## What shipped

### Modified

- `engine/scene/include/crd/scene/archetype_graph.hpp`
  - Drops `<memory>` include, adds `<crd/memory/allocators/growable_pool_allocator.hpp>`.
  - `m_archetypes` member changes from `Array<std::unique_ptr<Archetype>>` to `Array<Archetype*>`.
  - Adds `m_archetype_pool` member (the `GrowablePoolAllocator`).
  - Move ctor + move-assign declared (no longer defaultable — destroy_all_archetypes() needs to run before storage transfer in move-assign).
  - Explicit destructor declaration.
  - Adds `archetype_pool_pages()` diagnostic.
  - Adds `destroy_all_archetypes()` private helper.

- `engine/scene/src/archetype_graph.cpp`
  - Constructor initialises the pool with the World allocator as parent.
  - `archetype_for(mask)` now `pool.allocate()` + placement-new, instead of `make_unique`.
  - `~ArchetypeGraph()` calls `destroy_all_archetypes()` (runs `~Archetype()` on every live struct so member Arrays release to `m_alloc`); pool dtor then frees pages.
  - Move ctor/assign defined: move-assign destroys current archetypes before pool storage transfer.

- `tests/scene/CMakeLists.txt`
  - Adds `test_world_tlsf.cpp` to the scene test target.

### New

- `tests/scene/test_world_tlsf.cpp` — five test cases, all `[scene][world][tlsf]`-tagged:
  1. **World can be constructed with a TlsfAllocator** — empty World on a 16 MB TLSF pool, dtor clean.
  2. **Full ECS lifecycle on a TLSF-backed World** — 200 entities, mixed components (Position / Velocity / Health), register / spawn / add / get / get_mut / remove / destroy / flush_destroys all on TLSF heap. Verifies archetype count ≥ 4 (P, P+V, P+H, P+V+H).
  3. **Chunk fill/spill works on TLSF heap** — 1500 entities forces ChunkAllocator's GrowablePool to grow at least one extra page.
  4. **World destruction returns every byte to the TLSF pool** — snapshot bytes_in_use before World construction, snapshot after destruction, assert equality (debug-only — `MemoryStats` is debug-only).
  5. **ArchetypeGraph reports its archetype-pool page count** — diagnostic check that pages allocate lazily (0 before first archetype, ≥ 1 after).

## Why a pool for Archetypes

The audit report ranked this Option B+. A flat `Array<Archetype*>` with one `IAllocator::allocate(sizeof(Archetype))` per archetype would close the bypass — that was Option A. But:

- A single archetype is small (~600 B with all the edge tables) but not tiny. 1000 archetypes = ~600 KB of fragmented small allocations on the World allocator.
- The same engine that ships TLSF and GrowablePool should not be making small one-off allocations on a general-purpose heap when the use case (fixed-size aligned blocks, lifetime-tied-to-World) is the textbook GrowablePool pattern.
- Bevy and Unity DOTS both pool archetype/chunk metadata for the same reason.

Per-page tuning was set to 32 slots (one parent allocate covers 32 archetypes). At 1000 archetypes (large scene) that is ~32 pages.

## Trade-offs and choices

- **Per-slot deallocate is not used.** `destroy_all_archetypes()` runs `~Archetype()` on every live struct, then the pool's dtor frees entire pages. Calling `pool.deallocate(slot)` per slot before the page goes away would be redundant (and races with the pool dtor's bulk free). The pool's intrusive free-list lives inside the freed memory; an Archetype struct doesn't fit a free-list link in its first 8 bytes (that's `Array<...> components`'s storage pointer). Per-slot deallocate would corrupt the free-list. Bulk free is the only correct path.

- **Move-assign is hand-written.** Default move-assign would move members in declaration order (m_alloc, m_chunks, m_registry, m_archetype_pool, m_archetypes, m_by_mask). At the point pool storage is replaced, the existing m_archetypes still hold pointers into the old pool's pages. We have to call `destroy_all_archetypes()` first.

- **`m_archetypes` is `Array<Archetype*>`, not `Array<Archetype>` in the pool.** Storing `Archetype` directly in a containers::Array would resize-move the structs (Array uses memcpy on TriviallyRelocatable types — Archetype isn't, but a fallback move-construct path still exists and would invalidate cached pointers from edge tables / EntityLocation). Pointer stability is what the pool buys us.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 603 / 603 | 17 / 17 |
| win-relwithdebinfo | clean | 603 / 603 | 17 / 17 |
| win-release        | clean | 600 / 600 | 17 / 17 |
| win-asan           | clean | 603 / 603 | 17 / 17 |
| win-clang-cl       | clean | 603 / 603 | 17 / 17 |
| win-tidy           | clean | — | — |

(win-release runs three fewer cases because `MemoryStats` debug-only checks `#if defined(CRD_DEBUG)`-out.)

### Scene tests

- Before this session: 82 cases / 7012 assertions.
- After this session: 87 cases / 11024 assertions (+5 cases / +4012 assertions).

### LOC

- `archetype_graph.hpp` +20 / -8.
- `archetype_graph.cpp` +75 / -25.
- `test_world_tlsf.cpp` +220 (new).
- Total ≈ 280 LOC added, 33 LOC removed.

## What this unlocks

The Phase 3.0 v1d slice (`SparseSetStorage`) is now starting from a clean allocator chain. Every allocator the new backend touches (`m_alloc` for `Array<u32>` sparse table, dense storage, `ComponentRegistry`) is the same `IAllocator*` that flows down from `World`. There is no longer a "but archetype structs use global new" footnote.

Beyond v1d, the TLSF-backed-World pattern is the foundation for the determinism story in `docs/PRINCIPLES.md`: a scene whose allocations all come from one fixed-capacity TLSF pool can be replayed bit-exactly, because allocation addresses are deterministic given the allocation/free order. The test in this session is the first proof that the pattern actually works.

## Follow-ups

None opened by this session. v1c1's O(N) ChunkAllocator::free debt remains closed (D-001-b). The make_unique<Archetype> bypass that was implicit-but-untracked debt is now closed too.

## Commit message proposal

```
refactor(scene): pool Archetype structs via GrowablePoolAllocator + TLSF World test

Closes the std::make_unique<Archetype> bypass in ArchetypeGraph: every byte
the World allocates now flows through one IAllocator. Archetype structs
are pooled via GrowablePoolAllocator (32 slots/page, parent = World alloc).

New test/test_world_tlsf.cpp (5 cases) constructs a World on a 16 MB
TlsfAllocator pool and runs a full ECS lifecycle including chunk fill/
spill (1500 entities), proving the deployment pattern end-to-end.

Six-config DoD: 603/603 (was 598). 17/17 headless smokes per non-tidy
config. 87 scene tests / 11024 assertions (was 82 / 7012).
```
