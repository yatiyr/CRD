# Session — 2026-05-02 — `crd-jobs` v1e Priority Scheduler

**Phase:** 2.5 — `crd-jobs`  
**Slice:** v1e  
**Status:** ✅ Shipped

---

## What was built

### `engine/jobs/include/crd/jobs/job_decl.hpp` (new)

Public header exposing `Priority`, `StackSize`, and `JobDecl`. Layout is explicit with no
implicit padding so the struct occupies exactly one 64-byte cache line:

```
offset  0–7   fn (void(*)(void*))
offset  8–15  data (void*)
offset 16–19  pin_thread (i32)
offset 20     stack (StackSize)
offset 21     priority (Priority)
offset 22–63  _pad[42]
```

Static assertions verify `sizeof(JobDecl) == 64` and `alignof(JobDecl) == 64`.

### `engine/jobs/src/scheduler.hpp` / `scheduler.cpp` (new)

`crd::jobs::detail::Scheduler` — the priority-aware work distribution core.

**`SchedulerConfig`** (defined outside `Scheduler` to avoid clang-cl DR about default member
initializers in nested classes):

```cpp
struct SchedulerConfig {
    crd::u32 num_threads        = 1;
    crd::u32 deque_capacity     = 256;
    crd::u32 injection_capacity = 4096;
};
```

**`ThreadState`** per worker (individually heap-allocated, so concurrent deque operations on
different workers never share a cache line):
- `WorkStealingDeque<JobDecl>` high / normal / low
- `alignas(64) std::atomic<bool> pinned_available` (separate cache line from the deques)
- `JobDecl pinned_storage`

**`Scheduler` public surface:**
- `init(SchedulerConfig)` — allocates 3 MPMC injection queues + per-thread states.
- `shutdown()` — idempotent teardown.
- `push(JobDecl)` — pinned path: writes storage then `release` store on `pinned_available`;
  priority path: enqueues into the matching injection queue. Posts semaphore once either way.
- `push_local(u32 thread_index, JobDecl)` — pushes onto caller-owned local deque, posts
  semaphore. For child jobs spawned by a running job.
- `execute_one(u32 thread_idx)` — one full drain pass per the priority order (see below).
- `wait_for_work()` — blocks on `std::counting_semaphore<>::acquire()`.
- `num_threads()`, `is_initialized()`.

**Drain order in `execute_one()`** (deterministic; steal direction round-robin from
`(thread_idx + 1) % num_threads`):

```
1. Pinned slot (if pinned_available == true for this thread)
2. High injection queue  → High local deque → steal High from each peer
3. Normal injection queue→ Normal local deque→ steal Normal from each peer
4. Low injection queue   → Low local deque  → steal Low from each peer
5. return false
```

**`run_job()`** calls `job.fn(job.data)` synchronously (v1g replaces this with a fiber context
switch).

### Tests — `tests/jobs/test_scheduler.cpp` (new, 16 tests)

| # | Name | What it checks |
|---|---|---|
| 1 | init and shutdown | is_initialized() + re-init after shutdown |
| 2 | num_threads reflects config | 1-thread and 4-thread configs |
| 3 | single High job via injection | execute_one → ran; second call returns false |
| 4 | single Normal job via injection | basic path |
| 5 | single Low job via injection | basic path |
| 6 | drain order High before Normal before Low | push in reverse order (Low, Normal, High); verify seq 1=High, 2=Normal, 3=Low |
| 7 | injection checked before local | both injection and local hold Normal jobs; injection runs first |
| 8 | push_local executes on owning thread | basic push_local |
| 9 | local deque drains LIFO | push 1,2,3; pop order 3,2,1 |
| 10 | pinned job executes on target thread | thread 1 cannot consume thread 0's pinned slot |
| 11 | pinned slot cleared after execution | second execute_one finds nothing |
| 12 | work stealing across threads | thread 1 steals from thread 0's local deque |
| 13 | steal High before Normal | thread 0 has High + Normal local; thread 1 steals High first |
| 14 | semaphore count N pushes → N non-blocking waits | 5 pushes → 5 immediate wait_for_work() returns |
| 15 | push wakes blocked wait_for_work | real thread blocks on semaphore; push() wakes it |
| 16 | concurrent multi-thread stress | 4 workers, 4 000 mixed-priority jobs, all run exactly once |

### Side-fix — root `CMakeLists.txt`

Added `$<$<COMPILE_LANGUAGE:CXX,C>:/EHsc>` to the MSVC compile-options block. VS18 (MSVC 14.50)
updated `<chrono>` to emit C4530 ("exception handler used, but unwind semantics are not enabled")
when `/EHsc` is absent, which `/WX` escalated to a fatal error. This was a pre-existing project
break on VS18 — all 6 configurations were already failing before the scheduler work began.

---

## Decisions made

**`SchedulerConfig` outside `Scheduler`:** clang-cl (DR) does not allow default member
initializers in a nested class when that class is used as a default argument to a member
function of the outer class (`init(const Config& = {})`). Renaming and promoting the struct to
namespace scope was the minimal fix that keeps the API identical.

**Single pinned slot per thread:** The design doc specifies the pinned-job mechanism for GLFW
(thread 0). One slot is sufficient; the assert in `push()` catches double-occupancy bugs loudly
in debug builds.

**Drain order: deterministic round-robin steal direction.** Round-robin (`(thread_idx + i) %
num_threads`) instead of random was chosen for test verifiability. Production usage with many
threads may benefit from randomisation (avoids hot-spots), but the scheduler is an internal detail
that v1g will wrap — the steal strategy can be revisited there.

---

## Test results

All 6 configurations passed with zero warnings.

| Config | Passed | Total |
|---|---|---|
| win-debug | 317 | 317 |
| win-relwithdebinfo | 317 | 317 |
| win-release | 314 | 314 |
| win-asan | 317 | 317 |
| win-clang-cl | 317 | 317 |
| win-tidy | 317 | 317 |

win-release is 3 fewer than debug: the debug-only `FiberState` state-machine tests in
`test_fiber_pool.cpp` are compiled out by `#if CRD_ENABLE_ASSERTS`.

---

## Next

**v1f — Counter + wait mechanism:** `Counter` pool (pool-allocated from a fixed array, no dynamic
allocation on the hot path), Treiber waiter stack, ABA-safe double-check wait protocol (load value
→ append waiter → re-load value → yield if still nonzero), completer walks waiters and re-queues
as High-priority jobs.
