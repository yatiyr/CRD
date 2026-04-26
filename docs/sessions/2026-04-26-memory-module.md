# Session — 2026-04-26 — memory module

## Goal

Build `crd-memory` v1: a common allocator interface plus four concrete
allocators, the foundation that every later module (containers, graphics,
scripting, streaming) will layer on top of. Architecture-correct,
implementation-simple, streaming-ready.

## What we built / changed

- **New module `crd-memory`** under `engine/memory/`. Static library, depends
  on `crd-core` and `crd-log`. Public headers in
  `engine/memory/include/crd/memory/`, implementation in
  `engine/memory/src/`.
- **`IAllocator` interface** with three required virtuals
  (`allocate`/`deallocate`/`owns`) and two optional ones
  (`reallocate`/`allocation_size`) that ship with default implementations
  so future TLSF / streaming allocators can override without breaking
  callers.
- **`default_allocator()`** — process-global `MallocAllocator` instance,
  function-local static. The "I just need bytes" entry point.
- **Four concrete allocators:**
  - `MallocAllocator` — wraps `_aligned_malloc`/`aligned_alloc`. OOM is fatal.
  - `LinearAllocator` — bump pointer + `LinearScope` RAII rollback +
    `reset_to(offset)` for partial rewinds.
  - `StackAllocator` — bump pointer with typed `Marker`s + `StackScope`
    RAII. Markers carry an owner pointer in debug builds for cross-allocator
    safety.
  - `PoolAllocator` — fixed-size slots, intrusive free list, O(1) get/return,
    range-and-boundary owns() check.
- **`construct/destroy` helper templates** with placement new and
  trivial-destructibility skipping. Plus `construct_array`/`destroy_array`.
- **`MemoryStats`** — five atomic counters per allocator (alloc/dealloc
  count, bytes_in_use, peak_bytes, total_bytes), updated only in `CRD_DEBUG`
  builds. Fields exist in release for ABI stability but stay zero.
- **`alignment.hpp`** — `is_pow2`, `align_up`, `align_down`, `is_aligned`,
  plus `kDefaultAlignment = 16`, `kCachelineSize = 64`, `kMinAlignment`.
- **Memory log channel** `g_log_memory` so allocator-internal diagnostics
  (exhaustion warnings, OOM critical) route through `crd-log` cleanly.
- **25 Catch2 tests** under `tests/memory/test_memory.cpp`. All pass.
- **Runtime smoke test** in `runtime/src/main.cpp` exercising
  `default_allocator`, `LinearAllocator + LinearScope`, `PoolAllocator`,
  and `MemoryStats::snapshot`.
- **Long-form documentation** at `docs/memory/MEMORY_FILE.md` (eleven
  sections, plain English, deep dive). Short overview at
  `docs/systems/memory.md`.

## Plain-English explanation

`crd-memory` is the engine's "where do bytes come from" layer. Anything
that wants memory — a container, a sink, a renderer cache, a future
streaming system — calls `IAllocator::allocate(size, alignment)` instead
of `malloc`. The pointer it gets back is alignment-correct and tracked.
At shutdown (or earlier) you call `deallocate` to return it.

The point of having an *interface* is that the *kind* of allocator is
swappable. `default_allocator()` gives you the heap. A
`LinearAllocator(1 << 20)` gives you a 1-megabyte scratchpad that you
wipe in O(1) at frame end. A `PoolAllocator` gives you 10 000
identically-sized slots for, say, every Particle in the game. A future
`StreamingAllocator` will give you a virtual-memory-backed reservation
that pages in and out of RAM as the player moves through a 4 GB world.
None of those changes will require touching the code that *uses* the
allocator — they'll just take a different `IAllocator*` at construction
time.

You'd use it by either grabbing `default_allocator()` (boring,
heap-backed, fine for most things) or by constructing a specialized
allocator near where its lifetime is clearest — e.g. a `LinearAllocator`
field on your `Frame` struct, reset every game tick. For typed
allocations the helpers `construct<T>(alloc, args...)` and
`destroy<T>(alloc, p)` handle placement-new + dtor for you.

## Decisions made

- **`IAllocator*` constructor argument, not template parameter.** Type
  stability across allocator changes is the single most important
  decision for letting open-world streaming drop in later. EA STL /
  Bitsquid pattern, not std::vector pattern.
- **Streaming-ready interface from day one.** `reallocate` and
  `allocation_size` are virtual functions with default implementations
  (allocate+memcpy+deallocate, return 0). Future TLSF / streaming
  allocators override them; existing callers are unaffected.
- **OOM in heap allocators is fatal** (`CRD_LOG_CRITICAL` + `CRD_FATAL`).
  Sub-budget allocators (linear/stack/pool) return `nullptr` on
  exhaustion so callers can fall back gracefully.
- **`deallocate(nullptr)` is always safe.** No-throw, no-op.
- **Default alignment is 16 bytes**. SSE-friendly, fits every primitive.
- **Allocators are NOT thread-safe** (with `MallocAllocator` as the
  delegating exception). Hot paths stay branch-free.
- **`MemoryStats` tracking is debug-only** so release builds pay zero
  overhead. Public API (the struct + `snapshot()`) is identical in both
  builds.
- **Pool allocator slot reuse is LIFO.** Most-recently-freed gets the
  next allocation. Better cache behaviour, simpler implementation.
- **`StackAllocator::Marker` carries an owner pointer in debug builds**
  so wrong-allocator rollback is caught immediately.

## Files touched

- `CMakeLists.txt` — uncommented `add_subdirectory(engine/memory)`.
- `engine/memory/**` — entire new module (8 headers, 5 source files,
  1 CMakeLists).
- `tests/CMakeLists.txt` — `add_subdirectory(memory)`.
- `tests/memory/CMakeLists.txt` + `tests/memory/test_memory.cpp` — new.
- `runtime/CMakeLists.txt` — link `crd-memory`.
- `runtime/src/main.cpp` — added a memory-subsystem smoke section.
- `docs/systems/memory.md` — new short overview.
- `docs/systems/README.md` — added `crd-memory` row.
- `docs/memory/MEMORY_FILE.md` — new deep-dive.
- `docs/ROADMAP.md` — status table flipped, decision log entry.
- `CONTEXT.md` — status table flipped.

## Tests / verification

- Build: ✅ `cmake --build --preset win-debug` clean.
- Tests: ✅ `40/40` total (`2 core + 13 log + 25 memory`) green via
  `ctest --preset win-debug` in 0.48 s.
- Manual: ran `crd-runtime.exe`. Memory smoke section prints expected
  pointer addresses, scope rollback ("used=1024 bytes" → "used=0 bytes"),
  pool occupancy ("2/64 slots in use"), and live heap stats
  ("alloc=3 dealloc=1 bytes_in_use=66564"). Numbers match a manual
  accounting (1 small u32 + 64 KB scratch + 64×64 pool buffer).

## Next session starts with

1. **Bridge `crd-core` assert handler → `crd-log` Critical.** Small
   prerequisite that was deferred this session: `assert.hpp` exposes a
   `set_assert_handler()` callback; `crd-log::init()` registers a
   default handler that emits Critical and flushes. No reverse
   dependency from core to log.
2. **Begin `crd-math` v1.** Column-major matrices, radians everywhere,
   scalar first. Start with `Vec2/3/4` + basic operators + dot/cross/
   length/normalize. Catch2 unit tests as we go.
