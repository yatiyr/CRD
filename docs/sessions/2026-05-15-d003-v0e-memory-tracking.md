# 2026-05-15 — Detour D-003 v0e: `crd-perf` memory tracking

## What shipped

v0e wires `crd::memory::IAllocator` instances into the profiler's per-
frame snapshot ring. Three things in this slice:

### 1. `MemoryStats` gate widened to cover `CRD_ENABLE_PROFILING`

**`engine/memory/include/crd/memory/memory_stats.hpp`**: the `on_allocate`
/ `on_deallocate` tracking previously only ran under `CRD_DEBUG`. The
new `CRD_MEMORY_STATS_TRACKING` flag activates tracking when either
`CRD_DEBUG` is defined OR `CRD_ENABLE_PROFILING == 1`. Without this,
`win-shipping-profile` would have shown zeros on the Memory panel even
though it's *exactly* the config QA uses to measure real release-mode
allocations.

Cost when on: ~3 atomic fetch_add + a peak-bytes CAS-loop per allocate.
Negligible for engine code; `win-release` and `win-shipping` (the
consumer-ship configs with `CRD_ENABLE_PROFILING=OFF`) still pay zero
overhead -- the off-gate contract holds.

### 2. Allocator registry + per-frame `AllocatorRecord`

**`engine/perf/include/crd/perf/memory.hpp`** (~50 LOC) + impl in
`profiler.cpp` (~150 LOC):

- `AllocatorInfo { name, IAllocator* }` — metadata for one registered
  slot.
- `AllocatorSnapshot { name, alloc_count, dealloc_count, bytes_in_use,
  peak_bytes, total_bytes }` — friendly POD returned by live + historical
  query APIs.
- **`register_allocator(name, IAllocator*) -> u32`** — dedups by
  allocator pointer (same instance always returns the same idx); relabel
  on re-register; mutex-protected on the cold path.
- **`unregister_allocator(idx)`** — clears the slot but does NOT shrink
  `registered_allocator_count`. High-water is preserved so UI sees a
  stable per-slot index across the capture lifetime.
- `allocator_info(idx)`, `allocator_snapshot(idx)` (live),
  `allocator_snapshot_history(idx, frames_back)` (FrameRecord lookup).
- `kMaxAllocators = 32` (engine has ~10 active; tests bring transients).

**`FrameRecord` layout extended:**

- Added `crd::u32 allocator_count` (previously a 4-byte pad).
- Added `AllocatorRecord allocators[kMaxAllocators]` array (32 × 48 B
  = 1536 B per FrameRecord). Total `FrameRecord` size:
  **3616 B** (was 2080 B). Static-asserted by sum.
- 240-slot ring memory budget: ~870 KB total (was ~500 KB).
- **CPROF on-disk format pin updated** — v0f will memcpy the new
  layout verbatim; this is the layout that ships.

### 3. `frame_mark` allocator snapshot

At every `frame_mark()` the profiler walks every registered allocator
slot, calls `IAllocator::stats().snapshot()` (a single relaxed-atomic
load per counter), and stamps an `AllocatorRecord` into the FrameRecord
ring. Empty slots write a zeroed record so the UI can render a stable
table.

## Tests `tests/perf/test_memory_tracking.cpp` (9 cases / 36 assertions):

- `register_allocator` returns a stable, non-invalid index; different
  allocator pointers get different slots.
- Dedup: same pointer → same idx; the call relabels the slot.
- `allocator_info` reports the round-tripped name + pointer.
- `allocator_snapshot` reads live stats (an alloc bumps count and
  bytes_in_use; a free bumps dealloc_count, drops bytes_in_use, keeps
  peak).
- `frame_mark` stamps allocator stats into the FrameRecord ring;
  `allocator_snapshot_history(idx, 0)` returns the captured numbers.
- `unregister_allocator` zeros the slot but preserves the high-water
  count.
- Register-after-unregister re-uses the cleared slot (no leak of slot
  indices).
- Invalid index returns zero snapshot, no crash.
- Inactive profiler returns `kInvalidAllocatorIdx`.

## Design decisions locked at v0e

1. **Widened `MemoryStats` gate to `CRD_DEBUG || CRD_ENABLE_PROFILING`.**
   This is a substantive behavioural change to a foundational module --
   but principled: stats are observation, not action; they don't violate
   determinism; and `win-shipping-profile` is explicitly the "I want
   measurement" config so paying the cost is correct. Zero-overhead
   contract is preserved when both gates are off.
2. **Dedup by allocator pointer** (not name). Catches the "register the
   same TLSF root twice under different names" mistake by returning the
   existing idx + relabeling.
3. **High-water never shrinks on unregister.** UI sees a stable per-slot
   index across capture lifetime. Slots are re-used by subsequent
   registrations (linear scan for a cleared slot before appending).
4. **`AllocatorRecord` is 48 B (with 8 B reserved pad).** Static-asserted.
   On-disk CPROF format depends on this; v0f memcpys the new
   FrameRecord layout verbatim. Total FrameRecord = 3616 B.
5. **`kMaxAllocators = 32`.** Engine has ~10 active; tests bring
   transients. Bump if `register_allocator` ever asserts on saturation.
6. **All stat reads at frame_mark are relaxed-atomic.** Snapshot is
   "approximately at this frame boundary" -- allocators bumping
   concurrently with the snapshot settle into the next frame. Same
   semantics as counters (v0b).

## Verification (5-config per-slice DoD)

| Config | Result |
|---|---|
| win-debug | **PASS** (1823/1823 full project ctest; 76/76 perf cases — 233 assertions) |
| win-asan | **PASS** (76/76 perf cases — 233 assertions) |
| win-shipping | **PASS** (6/6 perf cases — 23 assertions; 70 gated cases compile out at gate) |
| win-shipping-profile | **PASS** (1818/1818 full project ctest under LTCG + max optimization with widened MemoryStats gate) |
| win-tidy | **PASS** (build clean) |

The full project ctest under `win-shipping-profile` is the load-bearing
verification for the widened gate: every test in the engine that touches
allocators now runs the atomic counter path at release-LTCG optimization
levels. Green.

## Issues encountered

None. The `FrameRecord` layout change was caught by the existing
`static_assert(sizeof(FrameRecord) == ...)` -- updated the expected size
to include the new allocator array; everything else fell out clean.

## What unlocks now

- **v0f** capture file format can begin: `CPROF` FourCC v1 header +
  thread table + region buffer + counter ring + GPU spans + **allocator
  snapshots** (the FrameRecord serialises verbatim).
- The UI substrate is now complete on the data side -- v0g can read
  every panel's content directly from existing crd-perf surfaces:
  Frame Summary (frame_record), Timeline (thread_samples + GPU track via
  gpu_thread_index), Flame Graph (Sample tree from depths), Counters
  (counter_info + frame_record values), GPU Passes (gpu_thread_index
  samples), Memory (allocator_info + allocator_snapshot[_history]).

## Next

**v0f — CPROF capture file format + save/load/replay.** Single owner per
capture (one thread drives save / load). FourCC `'CPRO'` + version + per-
thread sample arrays + counter table + frame history + allocator table.
Roundtrip tests verify byte-identical save → load → save. ~600 LOC
engine + ~400 LOC tests, 2-3 days.
