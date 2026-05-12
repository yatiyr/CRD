# 2026-05-12 — Detour D-002: concurrent containers + stress-hardening

**Closes detour D-002.** Main roadmap resumes at Phase 3.1.7 `crd-geometry` v0a.

Side mission to harden `crd-containers` / `crd-memory` / `crd-scene` storages under
heavy fiber load *before* the workloads that will impose it (eylem broadphase,
geometry BVH builds, parallel scene-system execution) arrive, and to add the
small, named concurrent surface those workloads will need. Ran as a 7-slice
mini-phase v0–v6.

## What shipped

### v0 — Stress harness (`tests/stress/`)
`stress_harness.hpp`: `crd::stress::run(cfg, work, oracle)` — N workers in either
**Fibers** mode (real `crd-jobs` fibers; normative lane for fiber-shaped contracts)
or **Threads** mode (`std::thread` mirror, so TSan-on-Linux can instrument it).
splitmix64 RNG seeded deterministically per `(base_seed, worker, round)`; `FailSink`
thread-safe failure recorder + `CRD_STRESS_FAIL_IF` / `CRD_STRESS_ORACLE_OK` (workers
never touch Catch2 macros — not thread-safe); round-based quiescent oracle; `bounded()`
config for the in-sweep variant, `soak()` behind the `[.soak]` Catch tag. `main_stress.cpp`:
crash-dump install + one-shot `jobs::init` (8 MB/thread frame arena) for the binary.
Lands driving the existing `crd-containers` (disjoint-write `Array`, many-readers, isolated
`TlsfAllocator`+`HashMap` churn) and isolated-per-fiber `TlsfAllocator` alloc/free/realloc
churn with byte-pattern verification.

### v1 — Scene-storage concurrency-contract inventory (`docs/systems/scene-concurrency.md`)
The single-writer model written down: no internal locking in `World` or any storage,
none planned; structural change lives behind the `Commands` deferral boundary; what runs
in parallel is iteration (one job per chunk). Legal-today patterns + an explicit
illegal-today list, a per-structure contract table (`SlotMap`, `ArchetypeChunkStorage`,
`ArchetypeGraph`, `SparseSetStorage`, `ComponentRegistry`, `SharedComponentPool`,
relations/reverse-indexes, `Commands`, indexes, `World`), how the v2 freeze guard fits,
what v6 will/won't test, and 3 open contract questions flagged (`AsyncAwareIndex::mark_loaded`
thread context; parallel `get_mut` version-counter semantics; per-fiber `Commands` stripes).
Referenced from `docs/systems/scene.md`.

### v2 — `Array` freeze guard + `FrozenView` + `jobs::parallel_reduce`
`crd::containers::Array<T>` gained debug-only `freeze()` / `unfreeze()` / `is_frozen()` +
a `check_mutable()` assert at the top of every structural mutator (push/pop/erase/insert/
clear/resize/reserve/shrink/assign/move-from) — element access stays allowed; the
`m_freeze_depth` member is absent (zero ABI) in non-assert builds; `freeze()` is `const`
so a `const Array&` can be frozen; re-entrant. It's a development net, not a hard barrier
(a frozen mutation trips a `CRD_ASSERT` like any other; in Release the check and the
mutation proceed — documented). `crd::containers::FrozenView<T>` — move-only RAII guard that
freeze()s on construction / unfreeze()s on destruction, exposes element access.
`crd::jobs::parallel_reduce<R>(count, num_jobs, init, map, reduce)` — map-each-range-to-a-
partial + caller-side left fold from `init` in job order (`R` trivially copyable; `MapFn`
under the 41-byte SBO constraint; partials + JobDecls from the per-thread frame arena;
`wait()` provides the happens-before).
**`parallel_for_frozen` was dropped** — it would have forced a `crd-jobs → crd-containers`
module edge for pure sugar; the freeze guard stays in `crd-containers` and the call site
combines `FrozenView` + `parallel_for` + `wait` (documented in `jobs.hpp`).

### v3 — `crd::containers::ConcurrentQueue<T>` + scheduler re-pointed
The Vyukov bounded MPMC queue promoted out of `engine/jobs/src/mpmc_queue.hpp`: same
algorithm + memory orderings, now takes an `IAllocator*` (cell array placement-new'd,
explicitly destroyed + deallocated in the dtor). Flat name (matches `SpscQueue`).
`try_push` / `try_emplace` / `try_pop` (non-blocking, `[[nodiscard]] bool`). `T` must be
trivially copyable (same as the source; non-trivial-T support deferred). The `crd-jobs`
scheduler's 3 priority injection queues now use `crd::containers::ConcurrentQueue<JobDecl>`
(via a private `JobInjectionQueue` alias; `enqueue`→`try_push`, `dequeue`→`try_pop`).
`mpmc_queue.hpp` and `tests/jobs/test_mpmc.cpp` deleted (coverage moved to the new
container tests; the MPMC stress subsumes the deleted SPSC/MPSC/SPMC variants).
**New module edge:** `crd-jobs → crd-containers`, PRIVATE (internal scheduler use only,
not propagated) — this aligns reality with the documented module graph in CLAUDE.md, which
already claimed jobs "depends on core + containers only". `tests/jobs/CMakeLists.txt` gained
`crd-containers` (white-box tests include `scheduler.hpp`). Jobs scheduler verified
unchanged-behaviour after the swap (90 cases + `smoke_jobs` PASS).

### v4 — `crd::containers::AtomicArray<T>` + `CacheLinePadded<T>`
`AtomicArray<T>` — bounded lock-free *append-only* vector: fixed capacity from an
`IAllocator*`, a producer claims the next slot with one `m_head.fetch_add(1)` and
placement-constructs there; slots never recycled/moved (addresses stable). `push`/`emplace`
return the index or `npos` on overflow (a sizing bug — asserted in debug, nothing written);
reads valid post-join; `clear()` for between-pass reuse. Supports non-trivial `T`.
`CacheLinePadded<T>` — `alignas(64)` one-element-per-cache-line wrapper. The "array of
atomic counters many fibers fetch_add" pattern is `Array<CacheLinePadded<u32>>` +
`std::atomic_ref<u32>(slot.value)` — trivially copyable element so `Array` works *and* can
grow, cache-line separated; the header documents that `Array<CacheLinePadded<std::atomic<T>>>`
does **not** compile (`std::atomic` isn't movable, `Array` relocates on growth).

### v5 — Allocator stress matrix (`tests/stress/test_allocators_v5_stress.cpp`)
One test shaped to each allocator's *own* contract: `LinearAllocator` (per-fiber isolated
arenas, fill-til-near-capacity → verify monotone non-overlap + patterns → `reset()` → repeat);
`StackAllocator` (per-fiber, nested mark/alloc/`reset_to`, verifying inner rollback leaves
outer allocations intact); `PoolAllocator` (per-fiber pools, alloc/free churn with per-slot
patterns + `slots_in_use()` + exhaustion-returns-null); `GrowablePoolAllocator` (same churn
but `max_live ≫ slots_per_page` to force page growth, verifying `page_count()` grows
monotonically + never shrinks). Each ×{threads, fibers}. Plus `TlsfAllocator` adversarial
*sequential* (single-threaded; ASan is the lane): coalescing, near-OOM `try_allocate`→nullptr,
alignment churn + `allocation_size`, a 40k-iter fragmentation soup with byte-pattern verify.
Plus a one-line `MallocAllocator` sanity test. (v0 already covers TLSF concurrent isolated churn.)

### v6 — Scene-storage stress matrix (`tests/stress/test_scene_storage_stress.cpp`)
Exercises only the patterns `docs/systems/scene-concurrency.md` declares legal:
parallel chunk read (archetype) — `for_each_chunk` collects per-chunk entity-ID lists
single-threaded (copying out of the transient `ChunkView` — ASan confirms that's necessary),
then `parallel_for` one job per chunk doing const `get_component` → folded XOR vs serial
reference; parallel disjoint chunk write (archetype + sparse) — one job per chunk (so each
chunk's version counter is bumped by exactly one job — the doc's rule until a future `par_each`),
each writes its entities' component via `get_component_mut` to an idempotent value, both
backends, verified every round; concurrent reads of frozen scene state — via the harness
(fibers, normative for scene), many workers hammering `is_alive` / `get_component` /
`registered_component_count`. Does NOT do concurrent structural mutation (illegal use).
`tests/stress/CMakeLists.txt` gained `crd-scene`.

## Decisions / pins

- **Allocator stance** (detour doc): allocators stay single-threaded-by-contract; no
  thread-safe variant of `TlsfAllocator`/pool/linear/stack added speculatively. They may
  migrate across threads but never be concurrently entered. `RefCounted` objects whose
  final release can occur off the creating thread must be backed by a thread-safe allocator
  or a deferred-free queue. (debt entries filed.)
- **Concurrent hash map: deferred** to a debt entry — built only when a concrete consumer
  demands it (per-fiber scratch + merge, or a `ConcurrentQueue` of update-requests, in the
  meantime).
- **`freeze()`/`FrozenView` is debug-only** — zero size / zero ABI in shipping configs.
- **Flat `crd::containers::` names** for the concurrent containers (matches `SpscQueue`),
  not a `concurrent::` sub-namespace.
- **TSan lane discipline** (detour doc): pure concurrent primitives → TSan-via-`std::thread`
  mode normative + fiber mode additional; scene storages under their contract → fiber mode
  normative. The `[.soak]` tests are Catch-`[.]`-hidden so CI `ctest` skips them; they run
  on demand via `crd-stress-tests "[.soak]"` (a scheduled nightly lane is left as a debt item).

## Verification

Every slice was built + run on **win-debug, win-asan, and win-clang-cl** as it landed
(the per-slice clang-cl build also flushed out `-Wunused-lambda-capture` / `-Wunused-const-variable`
issues, fixed). After v5, a one-shot **`linux-gcc-debug`** build (via `scripts/wsl-build.ps1`)
was added — it caught one `-Werror=unused-but-set-variable` (fixed) and otherwise all of
v0–v6's new code compiles + runs clean under GCC's strict set (incl. `std::atomic_ref` on
libstdc++ and the AT&T `fiber_switch_lin64.S` path through the re-pointed `ConcurrentQueue`
scheduler). v6 also ran on linux-gcc-debug. Final local state: `crd-stress-tests` 18 cases /
~167k assertions, `crd-jobs-tests` 90 cases, `crd-containers-tests` 109 cases, `smoke_jobs`
PASS — green on win-debug/win-asan/win-clang-cl/linux-gcc-debug; clang-format clean.

**Not run locally:** the full 14-config `scripts/full-sweep.ps1`, the four `[.soak]` runs,
and `win-tidy` (clang-tidy over the new code). Delegated to CI (which runs ctest on
win-debug/release/asan/sse2 + the 5 linux-gcc configs + shipping, build-only on win-tidy +
win-clang-cl) — if `win-tidy` is the one red, it'll be a small targeted clang-tidy fix.

## Docs touched

`docs/detours/D-002-concurrent-containers-and-stress-hardening.md` (the detour doc, slice-by-slice),
`docs/systems/scene-concurrency.md` (new), `docs/systems/scene.md` (reference added),
`docs/systems/containers.md` (`Array` freeze guard / `FrozenView` + a "Concurrent containers"
section: `SpscQueue` / `ConcurrentQueue` / `AtomicArray` / `CacheLinePadded`),
`docs/systems/jobs.md` (`parallel_reduce`), `docs/debt.md` (deferred concurrent hash map +
sharded global allocator; nightly soak lane), `context.md`, `docs/detours/README.md`.
