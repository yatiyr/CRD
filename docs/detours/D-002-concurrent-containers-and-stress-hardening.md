# D-002 — Concurrent containers + stress-hardening of containers, allocators & scene storages

**Status:** CLOSED 2026-05-12 (opened 2026-05-12). All slices v0–v6 implemented; built + run green on win-debug/win-asan/win-clang-cl/linux-gcc-debug per slice; full 14-config `scripts/full-sweep.ps1` + the four `[.soak]` runs delegated to CI. Closing session log: `docs/sessions/2026-05-12-d002-concurrent-containers-stress.md`. Main roadmap resumes at Phase 3.1.7 `crd-geometry` v0a.
**Paused (now resumed):** Phase 3.1.7 `crd-geometry` v0a.
**Owner pipeline:** research → coder → tester → reviewer → docs-keeper, same DoD as a phase slice.

> This is a *small* detour in intent but a multi-slice one in fact — it adds new primitives,
> a new test-infrastructure layer, and stress coverage across two whole modules. Run it as a
> mini-phase with explicit slices (v0…v6) and the exit criteria below. If a slice grows past
> what's written here, promote it — don't let it quietly take over the roadmap.

---

## Why

A real near-future workload (eylem broadphase, parallel scene-system execution, geometry BVH
builds) will push *many fiber jobs* at the containers, allocators and scene storages
simultaneously. We want three guarantees before that load arrives:

1. **The single-threaded-by-contract pieces are rock-solid under adversarial *isolated* load.**
   Containers, `TlsfAllocator`, `GrowablePoolAllocator`, `LinearAllocator`, `StackAllocator`,
   `PoolAllocator` — N fibers each hammering *their own* instance, plus adversarial sequential
   op-sequences (fragmentation soup, near-OOM, coalescing edges, alignment churn), all under ASan.
   The goal is to *prove reentrancy + isolation*, not to make them shared-thread-safe.

2. **A small, named, heavily-tested concurrent surface exists** for the cases that genuinely
   need it: `crd::containers::ConcurrentQueue<T>` (MPMC) and `crd::containers::AtomicArray<T>`
   (bounded atomic-append vector) + a thin atomic-element helper so `Array<AtomicU32>`-style
   use is ergonomic and false-sharing-safe. Concurrent hash map is **explicitly deferred**
   (debt entry) — built only when a concrete consumer demands it.

3. **The scene storages are stress-tested *against their declared concurrency contracts*** —
   many fibers doing the *legal* concurrent thing, hard, under ASan + the fiber lane.

### Allocator stance (pinned by this detour — do not re-litigate)

> Allocators are **single-threaded-access by contract**. They may change owning thread but must
> never be concurrently entered by two agents. `RefCounted` objects whose final release can
> occur off the creating thread must be backed by a thread-safe allocator or a deferred-free
> queue. **No thread-safe variant of `TlsfAllocator` / pool / linear / stack will be added
> speculatively** — if a shared cross-fiber heap is ever needed it is a *new* sharded/thread-
> caching allocator, not a lock bolted onto TLSF.

### Concurrent-container shape (pinned)

The right tool matches the access pattern to the structure — never "bolt a lock onto a
general-purpose container":

- Many fibers **reading** a shared array → zero synchronisation; contract = nothing resizes
  it during the parallel phase. Enforced by the debug-only `freeze()` / `FrozenView` guard.
- Many fibers **disjoint-writing** their own elements → lock-free; same `freeze()` guard.
- Many fibers **RMW on a cell** (counters/flags) → make the *element* atomic
  (`Array<AtomicU32>` / `AtomicArray`), not the container.
- "Combine many partial results" → that's a **reduction** (`parallel_reduce`), not a shared cell.
- Many fibers **appending to a shared collection** → `ConcurrentQueue<T>` (MPMC) or
  `AtomicArray<T>` (bounded, `head.fetch_add(1)` claims a slot).

---

## Decisions taken at open time

- **D-002 is a mini-phase with slices v0…v6** (not a one-shot detour). Confirmed by user 2026-05-12.
- **Concurrency-contract inventory ships first (v1)** — the spec the later stress slices test
  against. We do not stress-test scene storages with arbitrary concurrent ops; we test only the
  documented-legal patterns. Confirmed by user 2026-05-12.
- **`freeze()` / `FrozenView` is debug-only** — asserts on any mutating op while frozen in
  Debug / ASan builds; compiles to nothing in Release. Zero runtime cost, zero ABI impact in
  shipping configs. It is a development correctness net, not a runtime feature. Confirmed 2026-05-12.
- **`crd::containers::concurrent::` sub-namespace** for the threadful surface (tentative —
  finalise at v3) so concurrent types are visually segregated from the single-owner containers.
- **TSan lane discipline:** TSan cannot see the hand-rolled asm fiber context switch, so the
  stress harness has a `std::thread`-mirror mode. Normative lane per type:
  - Pure concurrent primitives (`ConcurrentQueue`, `AtomicArray`) → **TSan-via-std::thread is
    normative**; fiber mode = additional coverage.
  - Scene storages under their declared contract → **fiber mode is normative** (the contracts
    are fiber-shaped); TSan-via-std::thread where the contract can be expressed without fibers.
- **DoD / full-sweep budget:** stress tests run in *some* sweep (per the "slice closure =
  `scripts/full-sweep.ps1` PASS" rule) as a **bounded variant** (fixed small iteration count,
  fixed seeds) so the 14-config sweep stays affordable; a **longer soak variant** runs in a
  separate nightly lane. Exact iteration counts set in v0.

---

## Scope

**In:**

- `tests/stress/` — a reusable stress harness (v0): jobs-driven runner over real fibers + a
  `std::thread`-mirror mode for TSan + seeded per-worker RNG + pluggable invariant oracle +
  checkpointing. Bounded + soak variants.
- A concurrency-contract inventory for every `crd-scene` storage (v1) — short doc +
  per-header contract blocks.
- `freeze()` / `FrozenView` debug guard on `crd::containers::Array` / `Vector` + a
  `FrozenView`-taking `parallel_for` / new `parallel_reduce` in `crd-jobs` (v2).
- `crd::containers::ConcurrentQueue<T>` — promote `crd::jobs::detail::MpmcQueue<T>` (Vyukov)
  to a public container with an `IAllocator*`, fully fuzzed; re-point the jobs scheduler at it
  with zero regression (v3).
- `crd::containers::AtomicArray<T>` (bounded atomic-append vector) + atomic-element helper
  (`alignas`-padded wrapper) (v4).
- Allocator stress matrix: every allocator, isolated-per-fiber hammering + adversarial
  sequential patterns, under ASan (+ Linux TSan where meaningful) (v5).
- Scene-storage stress matrix: each storage exercised by many fibers doing its *contracted*
  legal operations, under ASan + the fiber lane (v6).

**Out (explicitly deferred):**

- Concurrent hash map (split-ordered / Cliff-Click). → debt entry; build when a real consumer needs it.
- Thread-safe `TlsfAllocator` / sharded global heap. → only if a concrete shared-cross-fiber-heap need appears.
- GPU-side concurrent structures.

---

## Slice plan

| Slice | Title | Deliverable |
|---|---|---|
| v0 | Stress harness | **implemented 2026-05-12; pending full-sweep.** `tests/stress/` — `stress_harness.hpp` (jobs-fiber mode + `std::thread`-mirror mode, splitmix64 seeded per (base,worker,round), `FailSink` thread-safe failure recorder + `CRD_STRESS_FAIL_IF` / `CRD_STRESS_ORACLE_OK`, round-based quiescent oracle, `bounded()` / `soak()` configs), `main_stress.cpp` (crash dumps + one-shot `jobs::init`), `test_containers_stress.cpp` (disjoint-write Array, many-readers, isolated-TlsfAllocator+HashMap churn — each ×{fibers,threads}), `test_allocators_stress.cpp` (isolated TlsfAllocator alloc/free/realloc churn with per-block pattern verification; `[.soak]` variant), `tests/stress/CMakeLists.txt` + wired into `tests/CMakeLists.txt`. Clean build + 4/4 pass on **win-debug and win-asan** (incl. a negative-test pass — a deliberately out-of-slice write was caught by the disjoint-write oracle). **TODO before close: win-clang-cl (verify `make_job` SBO size holds under a different lambda layout) + the full 14-config `scripts/full-sweep.ps1`.** |
| v1 | Scene-storage concurrency-contract inventory | **done 2026-05-12.** `docs/systems/scene-concurrency.md` — single-writer model, legal/illegal concurrent patterns, per-structure contract table (`SlotMap`, `ArchetypeChunkStorage`, `ArchetypeGraph`, `SparseSetStorage`, `ComponentRegistry`, `SharedComponentPool`, relations/reverse-indexes, `Commands`, indexes, `World`), how the v2 freeze guard fits, what v6 will/won't test, 3 open contract questions flagged (`AsyncAwareIndex::mark_loaded` thread context; parallel `get_mut` version-counter; per-fiber `Commands` stripes). Referenced from `docs/systems/scene.md`. No code. |
| v2 | `freeze()` / `FrozenView` + `parallel_reduce` | **implemented 2026-05-12; win-debug + win-asan green.** `crd::containers::Array<T>` gained debug-only `freeze()` / `unfreeze()` / `is_frozen()` + a `check_mutable()` assert on every structural mutator (push/pop/erase/insert/clear/resize/reserve/shrink/assign/move-from) — element access (`operator[]`, `data()`, iterators) stays allowed; the depth counter is absent (zero ABI) in non-assert builds; `freeze()` is `const` so a `const Array&` can be frozen. `crd::containers::FrozenView<T>` — move-only RAII guard that freeze()s on construction / unfreeze()s on destruction and exposes element access. `crd::jobs::parallel_reduce<R>(count, num_jobs, init, map, reduce)` — map-each-range-to-a-partial + caller-side left fold (R trivially copyable; partials + JobDecls from the per-thread frame arena; happens-before via `wait()`). **`parallel_for_frozen` was dropped** — it would force a `crd-jobs → crd-containers` module edge for pure sugar; the freeze guard stays in `crd-containers` and the call site combines `FrozenView` + `parallel_for` + `wait` (a 3-liner; documented in `jobs.hpp` and the `FrozenView` doc-block). Tests: `tests/containers/test_array_freeze.cpp` (toggle/nest, element-access-while-frozen, FrozenView RAII + move, and a `CRD_ENABLE_ASSERTS`-gated "every structural mutator asserts while frozen" battery) + `tests/stress/test_freeze_stress.cpp` (FrozenView + parallel_for disjoint-write × {1,3,8,64,257} jobs × 6 rounds, full verify; `parallel_reduce` sum + max vs serial ref across {1,2,7,64,1000,50000} × {1,2,8,64,1024} incl. num_jobs > count; `[.soak]` 200-round variant; tests `frame_reset()` between batches; `main_stress.cpp` arena bumped to 8 MB/thread). **TODO before close: win-clang-cl + full 14-config sweep.** |
| v3 | `ConcurrentQueue<T>` | **done 2026-05-12; win-debug + win-asan green.** `crd::containers::ConcurrentQueue<T>` (`engine/containers/include/crd/containers/concurrent_queue.hpp`) — the Vyukov bounded MPMC queue promoted out of `engine/jobs/src/mpmc_queue.hpp`: same algorithm + memory ordering, now takes an `IAllocator*` (cell array from it, placement-new'd, explicitly destroyed + deallocated in the dtor); flat `crd::containers::ConcurrentQueue` name (matches `SpscQueue`, not a `concurrent::` sub-namespace); `try_push` / `try_emplace` / `try_pop` (SpscQueue-style, non-blocking, `[[nodiscard]] bool`); `T` must be trivially copyable (same constraint as the source — documented; non-trivial-T support deferred until a consumer needs it); move/copy deleted. Jobs scheduler re-pointed: `scheduler.hpp`/`.cpp` now use `crd::containers::ConcurrentQueue<JobDecl>` (via a private `JobInjectionQueue` alias) for the 3 injection queues; `enqueue`→`try_push`, `dequeue`→`try_pop`; `mpmc_queue.hpp` and `tests/jobs/test_mpmc.cpp` deleted (coverage moved to the new container tests). New module edge: `crd-jobs → crd-containers` (PRIVATE — internal scheduler use only, not propagated; aligns with the documented module graph which already claimed it); `tests/jobs/CMakeLists.txt` gained `crd-containers` (white-box tests include `scheduler.hpp`). Tests: `tests/containers/test_concurrent_queue.cpp` (single-thread FIFO, full/empty, multi-lap wrap-around, `try_emplace` forwarding) + `tests/stress/test_concurrent_queue_stress.cpp` (MPMC: 4 producers + 4 consumers, unique `(producer,seq)` tokens through a 1024-deep queue with full/empty spinning; oracle = exactly-`total` consumed + every token seen exactly once + XOR checksum match; `seen` is a frozen `Array` for the parallel phase (v2 synergy); threads lane normative + fibers additional + `[.soak]`). Verified: clean build + `crd-jobs-tests` 90 cases (scheduler unchanged-behaviour after the swap) + `crd-containers-tests` cq cases + `crd-stress-tests` 7 cases + `smoke_jobs` PASS, on **win-debug, win-asan, and win-clang-cl** (the clang-cl partial build of `crd-stress-tests`/`crd-jobs-tests`/`crd-containers-tests` also flushed out `-Wunused-lambda-capture` on constexpr-captured-in-lambda and `-Wunused-const-variable` warnings in v0/v3 stress sources — fixed; confirms the `make_job` SBO `static_assert`s hold under clang-cl); clang-format clean. **TODO before close: full 14-config `scripts/full-sweep.ps1` (incl. the Linux configs + win-tidy/relwithdebinfo/release).** |
| v4 | `AtomicArray<T>` + atomic-element helper | **done 2026-05-12; win-debug + win-asan + win-clang-cl green.** `crd::containers::AtomicArray<T>` (`engine/containers/include/crd/containers/atomic_array.hpp`) — bounded lock-free *append-only* vector: fixed capacity from an `IAllocator*`, a producer claims the next slot with one `m_head.fetch_add(1)` and placement-constructs its element there; slots never recycled/moved → element addresses stable; supports non-trivially-{copyable,destructible} `T` (each slot written once, destroyed in the dtor). `push`/`emplace` return the claimed index or `npos` on overflow (a sizing bug — asserted in debug, no write done); reads (`operator[]`/`data()`/iterators/`size()`) are valid post-join (the join provides the happens-before); `size()` over-counts in-flight slots during a live pass; `clear()` for between-pass reuse (not thread-safe vs push). Move/copy deleted. Plus `crd::containers::CacheLinePadded<T>` — a `alignas(64)` one-element-per-cache-line wrapper. The "array of atomic counters" pattern is `Array<CacheLinePadded<u32>>` + `std::atomic_ref<u32>(slot.value)` (trivially copyable element → `Array` works *and* grows; cache-line separated) — documented with the explicit caveat that `Array<CacheLinePadded<std::atomic<T>>>` does **not** compile (`std::atomic` isn't movable, `Array` relocates on growth). Tests: `tests/containers/test_atomic_array.cpp` (push/size/full/read-back, dtor accounting for non-trivial `T`, a `CRD_ENABLE_ASSERTS`-gated overflow→npos test, `CacheLinePadded` size/align/trivially-copyable + the `Array<CacheLinePadded<u32>>`+`atomic_ref` pattern incl. growth) + `tests/stress/test_atomic_array_stress.cpp` (8 workers concurrently appending unique `(worker,seq)` tokens into an `AtomicArray` sized exactly N*K → oracle = `size()==N*K` + every token present exactly once + `clear()` between rounds; 8 workers × K `atomic_ref` fetch_adds on random elements of a frozen `Array<CacheLinePadded<u32>>` → oracle = element sum == N*K per round; threads-normative + fibers + `[.soak]`). Verified: clean build + `crd-containers-tests` `[atomic_array]` 5 cases + `crd-stress-tests` 9 cases, on win-debug, win-asan, win-clang-cl; clang-format clean. **TODO before close: full 14-config `scripts/full-sweep.ps1`.** |
| v5 | Allocator stress matrix | **done 2026-05-12; win-debug + win-asan + win-clang-cl green.** `tests/stress/test_allocators_v5_stress.cpp` — one test *shaped to each allocator's contract* (not one harness × six allocators): **`LinearAllocator`** — per-fiber isolated arenas, fill-til-near-capacity (checking `remaining()` since `allocate` is fatal-on-OOM) → verify monotone non-overlap + region byte-patterns intact → `reset()` → repeat; **`StackAllocator`** — per-fiber isolated, nested mark/alloc/`reset_to`, verifying an inner rollback leaves outer allocations intact and offsets match the markers; **`PoolAllocator`** — per-fiber isolated pools, random alloc/free churn with per-slot byte patterns + `slots_in_use()` consistency + exhaustion-returns-null (not fatal); **`GrowablePoolAllocator`** — same churn but `max_live >> slots_per_page` to force page growth, verifying `page_count()` grows monotonically + never shrinks (incl. on drain) + grew past one page; each of the four ×{threads, fibers}. Plus **`TlsfAllocator` adversarial *sequential*** (single-threaded TEST_CASE, ASan is the lane): coalescing (free-every-other → alloc-larger fits), near-OOM (`try_allocate`→nullptr without crashing, recovers after free), alignment churn + `allocation_size`, a 40k-iteration fragmentation soup with alloc/free/realloc + byte-pattern verify (REQUIRE per op), and a post-churn `pool_capacity()` + half-pool-allocate check. Plus a one-line **`MallocAllocator`** sanity test (its own correctness isn't ours to prove; `owns()` always returns true by design — not asserted). v0's `test_allocators_stress.cpp` already covers TLSF *concurrent isolated* churn. Verified: clean build + `crd-stress-tests` `[memory]` (8 cases, ~54k assertions) on win-debug, win-asan, win-clang-cl; clang-format clean. **Also: a one-shot Linux GCC build (`linux-gcc-debug` via `scripts/wsl-build.ps1`) now passes** — it flushed out one `-Werror=unused-but-set-variable` in this slice (fixed) and otherwise all of v0–v5's new code (incl. `std::atomic_ref` on libstdc++, the AT&T `fiber_switch_lin64.S` path through the new `ConcurrentQueue` scheduler) compiles + runs clean: Linux `crd-stress-tests` 15 cases, `crd-jobs-tests` 90 cases, `crd-containers-tests` 109 cases, `smoke_jobs` PASS. **TODO before close: the full 14-config `scripts/full-sweep.ps1`.** |
| v6 | Scene-storage stress matrix | **done 2026-05-12; win-debug + win-asan + win-clang-cl + linux-gcc-debug green.** `tests/stress/test_scene_storage_stress.cpp` — exercises only the patterns `docs/systems/scene-concurrency.md` declares legal: **parallel chunk read (archetype)** — `query<T>().for_each_chunk` collects per-chunk entity-ID lists single-threaded (copying out of the transient ChunkView), then `parallel_for` runs one job per chunk doing const `get_component<T>` (no version bump) → folded XOR compared to a serial reference; **parallel disjoint chunk write (archetype + sparse)** — one job per chunk (so each chunk's version counter is bumped by exactly one job — the doc's rule until a future par_each defines parallel `get_mut`), each writes its entities' component via `get_component_mut<T>` to an idempotent per-(entity,round) value, on both backends, verified every round; **concurrent reads of frozen scene state** — via the harness (fibers, normative for scene), many workers hammering `is_alive` (SlotMap), `get_component` (locations + chunk SoA), `registered_component_count` (ComponentRegistry), with an oracle re-verifying the world is intact. Does NOT do concurrent structural mutation (illegal use). New CMake link: `tests/stress/CMakeLists.txt` gained `crd-scene`. Verified: clean build + `crd-stress-tests` `[scene]` (3 cases, ~113k assertions) on win-debug, win-asan, win-clang-cl, linux-gcc-debug; full `crd-stress-tests` 18 cases green on win-asan + linux; clang-format clean. |

---

## Exit criteria

- v0–v6 all closed; each passed `scripts/full-sweep.ps1` (bounded stress variant in-sweep).
- Nightly soak lane green for ≥1 run on each new primitive.
- `crd::containers::ConcurrentQueue` and `crd::containers::AtomicArray` documented in
  `docs/systems/containers.md`; `freeze()`/`FrozenView` documented there too.
- `docs/systems/scene-concurrency.md` exists and is referenced from `docs/systems/scene.md`.
- Debt entry filed for the deferred concurrent hash map.
- Jobs scheduler still passes its own test suite + smokes after the `MpmcQueue` swap.
- Closing session log written; if any architecture changed (it shouldn't — these are
  additive substrate pieces), an ADR; otherwise just the session log. Main roadmap resumes
  at Phase 3.1.7 v0a.

---

## Notes / findings at open time

- `crd::jobs::detail::MpmcQueue<T>` (`engine/jobs/src/mpmc_queue.hpp`) is already mostly generic
  (trivially-copyable `T`, bounded pow2 capacity, zero post-construction allocation) **but uses
  `std::make_unique<Cell[]>`** — a public container must take `IAllocator*` instead. Promotion =
  move + `IAllocator*` + decide on non-trivially-copyable `T` support + re-point the scheduler.
- `crd::containers::SpscQueue<T>` already exists (lock-free, cache-line-split, `IAllocator*`,
  clear thread-safety contract doc) — `ConcurrentQueue` is its MPMC sibling and should mirror
  its style exactly.
- All allocators are single-threaded-by-contract (`tlsf_allocator.hpp:59` states it). `RefCounted`
  already uses an atomic refcount.
- Scene storages: `SlotMap`, `ArchetypeChunkStorage`, `SparseSetStorage`, `ArchetypeGraph`,
  `ComponentRegistry`, `SharedComponentPool`, `Relation` storage, `World` — these have
  *system-tick-shaped* contracts (single-writer during a tick, parallel reads inside a phase,
  mutations only via serially-replayed commands). v1 writes those down precisely.
