# D-001 — Memory infrastructure for elite-tier allocator coverage

**Opened:** 2026-05-07
**Status:** 🚧 active — D-001-a in flight (TLSF allocator), D-001-b pending (GrowablePoolAllocator + ChunkAllocator refactor)
**Pauses:** Phase 3.0 v1d (`SparseSetStorage`)

---

## Why

Phase 3.0 v1c2 ships with everything in `crd-scene` routing to `MallocAllocator` via the `IAllocator*` dependency injection. The plumbing is right; the allocator behind it is not.

Two gaps surfaced during v1c2:

1. **Chunk allocation churn.** `ChunkAllocator` allocates 16 KB blocks at 64-byte alignment per archetype-chunk fill/spill. Today: `m_alloc->allocate(16384, 64)` (malloc) + `Array<void*> m_blocks` linear scan in `free` (O(outstanding), flagged in v1c1's session log as a perf-revisit candidate). Both go away with a proper pool. ECS reference engines (Bevy, Unity DOTS) all pool chunks.

2. **No TLSF.** `CLAUDE.md` and `docs/memory/MEMORY_FILE.md` have promised TLSF — Two-Level Segregated Fit, Masmano et al. 2008 — as the engine's general-purpose default allocator since Phase 1. Real-time O(1) `allocate` / `deallocate` with bounded fragmentation is the standard for sub-frame budget guarantees. Today, `default_allocator()` returns `MallocAllocator`. Every consumer pays libc malloc cost and worst-case fragmentation. Replacing the default is a separate cutover (risk-bounded) but the *type* needs to exist first.

v1d, v1e, v1f, v1g, … all inherit the same allocator chain. Pay this debt once, before Phase 3.0 grows further.

## Scope

Two-session split:

### D-001-a — `TlsfAllocator` (this session)

- `crd::memory::TlsfAllocator : public IAllocator`
- Canonical Conte parameters: 16-byte alignment, 32 sub-classes per size class, 4 GB max pool, ~7 KB metadata footprint.
- Two-level free-list classification with bitmap search (O(1)).
- Block split + bidirectional coalesce.
- Alignment up to 64 bytes supported via leading-split.
- Owning ctor (allocates pool from parent) + non-owning ctor (uses pre-allocated buffer).
- `MemoryStats` integration.
- **Does NOT replace `default_allocator()`.** The default stays `MallocAllocator` — TLSF is opt-in via `IAllocator*`. The default-allocator cutover is a separate change with its own risk surface.
- Tests: ~20 cases — alloc/free/coalesce, alignment, fragmentation, OOM, owns(), realloc, allocation_size, 1000-iteration random-pattern stress under ASan.

### D-001-b — `GrowablePoolAllocator` + `ChunkAllocator` refactor (next session)

- `crd::memory::GrowablePoolAllocator : public IAllocator`
- Fixed-size aligned blocks served from auto-growing pages.
- O(1) allocate (free-list pop) and free (free-list push).
- `owns()` walks page list (pages are few; cost is invisible).
- Refactor `crd::scene::ChunkAllocator` to be a thin wrapper over `GrowablePoolAllocator(slot_size = 16 KB, slot_alignment = 64, slots_per_page = 64)`. All v1c2 archetype-storage tests must pass unchanged.
- Closes the v1c1 O(N) `free` perf debt.
- Tests: ~10 cases — page boundary growth, free-list reuse, owns() across pages, ASan leak check on dtor.

## Exit criteria

- Both allocators pass six-config DoD (win-debug, win-relwithdebinfo, win-release, win-asan, win-clang-cl, win-tidy).
- v1c2's archetype-storage tests stay green after `ChunkAllocator` refactor.
- `MallocAllocator` remains the engine default (TLSF is opt-in via explicit construction).
- D-001-a session log + D-001-b session log written.
- This file marked closed; main roadmap resumes at v1d.

## Non-goals (explicitly deferred)

- **Replacing `default_allocator()` with `TlsfAllocator`.** A cutover with global blast radius. Worth its own slice when it lands.
- **Multi-threaded allocators.** TLSF and GrowablePool are single-threaded per project convention. Future work if multi-thread allocation pressure justifies a separate detour.
- **TLSF in 32-bit builds.** v1 ships 64-bit only. Cerid CI is 64-bit; revisit if 32-bit becomes a target.
- **Conte's 8-byte block-header trick.** Canonical TLSF has `block_header_overhead = 8` via overlapping `prev_phys_block` with the previous block's payload tail. We use the simpler `block_header_overhead = 16` for v1 — saves ~8 bytes per allocation in dense workloads but costs subtle invariant management and bug surface. Documented as future tightening.
- **`free_bytes()` / `used_bytes()` o(1) tracking.** v1 uses `MemoryStats` (already in IAllocator base) which only updates in debug builds. Production stat tracking can land alongside the default-allocator cutover.

## References

- Masmano, Ripoll, Crespo, Real (2008). "TLSF: A New Dynamic Memory Allocator for Real-Time Systems." [paper / journal version]
- Matt Conte's reference implementation (BSD): https://github.com/mattconte/tlsf
- `engine/memory/include/crd/memory/allocators/` — existing allocator inventory (Malloc / Linear / Stack / Pool)
- `docs/memory/MEMORY_FILE.md` — long-form module deep-dive (will be amended after D-001-a + D-001-b)
- `docs/sessions/2026-05-07-scene-v1c1-chunk-layout.md` — flagged the O(N) `ChunkAllocator::free` debt that D-001-b closes
- ADR-0049 / ADR-0050 — `crd-scene` storage layer that consumes this allocator infrastructure
