# Session — 2026-04-26 — bridge + crd-containers v1a

## Goal

Two things in one session:

1. **Bridge:** wire `crd-core`'s `CRD_ASSERT` failure path into `crd-log`
   so that any assertion landing while the logger is up emits a
   `Critical` record and flushes — small prerequisite that was deferred
   from the memory session.
2. **`crd-containers` v1a:** start the container module with the
   non-string, non-hash-table pieces — `Array<T>`, `FixedArray<T, N>`,
   `Span` aliases, `hash.hpp` defaults, and the channel.

The bigger v1b (`String`, `RingBuffer`) and v1c (`HashMap`, `HashSet`)
are deliberately deferred; this session finishes the foundation that
they'll build on.

## What we built / changed

### Bridge (assert ↔ log)

- Added `crd::set_assert_handler(...)` / `crd::get_assert_handler()` to
  `engine/core/include/crd/core/assert.hpp`. Storage is a single
  `std::atomic<AssertHandler>`.
- Added a thread-local re-entrancy guard in
  `engine/core/src/assert.cpp` so an assert fired *inside* the handler
  doesn't recurse infinitely.
- `crd::log::init()` now installs a default handler that emits
  `CRD_LOG_CRITICAL(g_log_default, ...)` + `flush()`, *only if* the user
  hasn't already installed their own.
- `crd::log::shutdown()` removes that handler before tearing down sinks
  so a race-y assert during shutdown can't try to log into dead state.

### `crd-containers` v1a

- New module under `engine/containers/`:
  - `include/crd/containers/alignment.hpp` — *(reuses crd-memory's; nothing new here)*
  - `include/crd/containers/log_channel.hpp` — channel decl + `force_link_log_channel()`
  - `include/crd/containers/span.hpp` — `Span<T>` / `ConstSpan<T>` aliases for `std::span`, plus `make_span` / `as_span` / `as_const_span`
  - `include/crd/containers/hash.hpp` — `hash_u64` (splitmix64 + golden-ratio seed), `fnv1a_64`, `hash_string`, `DefaultHash<T>` specialisations for every integer, `const char*`, `std::string_view`, and pointers, with `std::hash<T>` fallback
  - `include/crd/containers/array.hpp` — `Array<T>` (allocator-aware via `IAllocator*` ctor arg, 1.5x growth, initial cap 8, `push_back`/`try_push_back`, `emplace_back`, `swap_remove`, `erase`, `insert`, `resize`, `shrink_to_fit`, raw-pointer iterators, copy/move semantics)
  - `include/crd/containers/fixed_array.hpp` — `FixedArray<T, N>` (stack-only, `alignas(T) std::byte` storage, `push_back` asserts on full, `try_push_back` returns bool)
  - `include/crd/containers/containers.hpp` — umbrella header + per-TU anchor against MSVC linker stripping
  - `src/log_channel.cpp` — `g_log_containers` definition + `force_link_log_channel()`
  - `CMakeLists.txt` — links to `crd-core`, `crd-log`, `crd-memory`
- `tests/containers/CMakeLists.txt` + `tests/containers/test_containers.cpp` — 32 Catch2 tests covering hash math, Span helpers, every Array operation, FixedArray edge cases, sub-budget exhaustion via `try_push_back`, dtor counting, and channel registration.
- Smoke runtime updated: a new container section in `runtime/src/main.cpp`
  exercises `Array<u32>`, `FixedArray<const char*, 4>`, `Array` backed by a
  `LinearAllocator(256)` (showing `try_push_back` halting cleanly when the
  arena fills), and `hash_u64` / `hash_string` calls.

### Docs

- `docs/systems/containers.md` — short stable overview, marks v1a ✅,
  v1b/v1c/v1d as ⏳.
- `docs/containers/CONTAINERS_FILE.md` — long deep-dive, ten sections,
  same shape as `LOG_FILE.md` and `MEMORY_FILE.md`.
- `docs/systems/README.md` — added `crd-containers` row.
- `docs/ROADMAP.md` — Phase 1 step list reordered to put containers
  before math (per session decision), v1a step marked 🚧 → v1a
  shippable, decision log entry added, "Where I left off" updated.
- `CONTEXT.md` — status table updated.

## Plain-English explanation

### Bridge

Until today, `CRD_ASSERT(false)` would print a stack trace to stderr,
pop a Windows MessageBox, and (in some paths) call `std::abort()`. None
of that landed in `engine.log`, which is the file you'll most often
look at after a crash. The bridge fixes that: when the logger is alive,
every assertion failure also writes a `Critical` record to all sinks
and flushes, so the file on disk has the last words even if the dialog
gets dismissed.

The architecture is one-way. `crd-core` exposes a function-pointer slot
(`set_assert_handler`) and calls whatever's in it. `crd-log` is the
only client that installs anything in that slot. There's no reverse
include from core back to log — core stays self-sufficient and
loggable-but-not-required.

### Containers v1a

The container module is the engine's `std::vector` / `std::span` /
`std::hash` replacement. Every container takes an `IAllocator*` as a
constructor argument, not a template parameter — so swapping a heap
arena out for a streaming arena later doesn't change any types in your
program.

`Array<T>` is the workhorse: dynamic, contiguous, growable. 1.5x growth
strategy, initial capacity 8 elements. Two ways to add to it:
`push_back` for the boring path (heap OOM = fatal) and `try_push_back`
for sub-budget allocators where you want to stop pushing gracefully
when the arena is full. There's also `swap_remove(i)` — O(1) if you
don't care about order, perfect for entity lists and draw call buckets.

`FixedArray<T, N>` is the bounded counterpart. Lives entirely on the
stack (or wherever you put it), no heap touched. Use it when the upper
bound is known at compile time — vertex stream slots, texture
bindings, anywhere you'd otherwise reach for a magic number.

`Span<T>` is just `std::span<T>` with a few helpers. Pass these around
when you want to read someone else's memory without taking ownership
or copying.

`hash.hpp` ships the building blocks for the hash table coming in v1c:
splitmix64 for integers (with a non-zero seed mix so 0 doesn't map to
0), FNV-1a for byte sequences, and a `DefaultHash<T>` template that
dispatches to the right one based on what `T` is.

The big things deliberately *not* in v1 — `Vector` (= `Array`), linked
lists, RB-tree maps, stacks, queues, generic Tree/Graph — are
explained at length in `docs/containers/CONTAINERS_FILE.md` §4. Short
version: either a duplicate of what we already have, cache-hostile, or
the wrong abstraction (subsystem-specific structures will live in their
subsystems, not in a generic container module).

## Decisions made

- **Containers come before math** in Phase 1 (was: math before
  containers). User preference; lets log's RingBufferSink migrate to
  `crd-containers::RingBuffer` in v1d before any new module needs to
  rebuild on top of math.
- **`crd-containers` ships in three sub-versions**: v1a (this), v1b
  (`String`, `RingBuffer`), v1c (`HashMap`, `HashSet`). v1d is the
  cleanup pass.
- **Naming:** `Array`, `HashMap`, `HashSet`, `String`, `RingBuffer`,
  `FixedArray`. EA STL / Bitsquid style. No `Vector` alias.
- **Allocator pattern:** constructor argument (`IAllocator*`), not
  template parameter. Type stays stable across allocator changes —
  critical for streaming.
- **`Array<T>` growth:** 1.5x (Folly/EA preference) with initial
  capacity 8.
- **Two push APIs:** `push_back` (assert + grow / fatal on OOM) and
  `try_push_back` (returns false if a sub-budget allocator refused).
- **Iterators are raw pointers.** Trivial, std-compatible, no custom
  iterator class.
- **`Span<T>` is an alias for `std::span<T>`.** No reason to write our
  own.
- **`hash_u64` uses splitmix64 with a golden-ratio XOR seed.** Removes
  the `hash(0) == 0` fixed point at a one-XOR cost.
- **`hash.hpp` includes a `DefaultHash<T>` template** that specialises
  for the common engine types and falls back to `std::hash<T>` for
  everything else. v1c's `HashMap<K, V>` will default-use this.
- **No tree/graph/linked-list in containers.** Those will live in their
  respective subsystems (scene graph, render graph, BVH, behavior
  tree) where their specific invariants matter.
- **MSVC linker dead-code stripping countermeasure:** an
  anonymous-namespace anchor in `containers.hpp` references a tiny
  `force_link_log_channel()` function in `log_channel.cpp`. Without
  this, the linker strips the entire channel TU because the test
  executable references nothing from it directly.

## Files touched

- `CMakeLists.txt` — uncommented `add_subdirectory(engine/containers)`.
- `engine/core/include/crd/core/assert.hpp` — added `set/get_assert_handler`, `AssertHandler` type alias.
- `engine/core/src/assert.cpp` — handler storage (atomic), thread-local re-entrancy guard, fire-handler in `report_assert_failure`.
- `engine/log/src/logger.cpp` — install default handler in `init()`, uninstall in `shutdown()`.
- `engine/containers/**` — entire new module (8 headers, 1 source, 1 CMakeLists).
- `tests/CMakeLists.txt` — `add_subdirectory(containers)`.
- `tests/containers/CMakeLists.txt` + `tests/containers/test_containers.cpp` — new (32 tests).
- `tests/log/test_log.cpp` — appended 3 new tests for the bridge.
- `runtime/CMakeLists.txt` — link `crd-containers`.
- `runtime/src/main.cpp` — added container smoke section.
- `docs/systems/containers.md` — new short overview.
- `docs/systems/README.md` — added `crd-containers` row.
- `docs/containers/CONTAINERS_FILE.md` — new deep-dive.
- `docs/ROADMAP.md` — status table flipped, Phase 1 reordered, decision-log entry, "Where I left off" updated.
- `CONTEXT.md` — status table flipped.

## Tests / verification

- Build: ✅ `cmake --build --preset win-debug` clean.
- Tests: ✅ `75/75` total (`2 core + 16 log + 25 memory + 32 containers`)
  green via `ctest --preset win-debug` in 0.73 s.
- Manual: ran `crd-runtime.exe`. Container smoke section prints exactly
  what we'd expect:
  - `Array<u32>: size=10 capacity=12 front=0 back=81` — 1.5x growth
    confirmed (8 → 12 after pushing 9 items).
  - `Linear-backed Array: pushed 18 u32s before exhaustion` — the
    `try_push_back` path correctly stops when the 256-byte
    `LinearAllocator` runs out, and the memory channel even logs the
    exhaustion warning ("ContainerScratch exhausted (requested 108…)").
  - `hash_u64(42) = 0xBDD7…2FEB6E95` — non-zero, well-distributed.

## Next session starts with

1. Open `docs/ROADMAP.md`, re-read the "Where I left off" section.
2. Begin `crd-containers` v1b: `String` (SSO 23 inline + 8 alloc ptr =
   32-byte struct), `StringView` alias to `std::string_view`,
   `RingBuffer<T>` (single-threaded, power-of-two capacity, mask-based
   indexing).
3. ~20 Catch2 tests: SSO boundary at 23, heap promotion past 23,
   append, comparison ops, empty/full RingBuffer, push/pop FIFO order,
   wrap-around, snapshot order.

End-of-session goal: String + RingBuffer working, all tests green.
v1c (HashMap + HashSet) remains for the session after; v1d is the log
RingBufferSink migration once `RingBuffer<T>` exists.
