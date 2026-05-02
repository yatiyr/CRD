# Session: crd-jobs v1g — Worker Thread Pool + Main-Thread Fiber

**Date:** 2026-05-02  
**Scope:** `engine/jobs/src/worker_pool.hpp`, `worker_pool.cpp`, `fiber.hpp`, `fiber_pool.cpp`, `fiber_init.cpp`, `fiber_switch_win64.asm`, `fiber_context.hpp`; `tests/jobs/test_jobs.cpp`

---

## What was built

`WorkerPool` class: the top-level job system runtime. Owns `Scheduler`, `FiberPool`, and
`CounterPool`. `init(WorkerConfig)` spawns N-1 background threads (indices 1..N-1), each running
`worker_loop`; thread 0 is enrolled as the main-thread worker and driven by `pump()`. All jobs
execute inside fiber context switches via `job_fiber_trampoline`.

### Key types

- **`WorkerConfig`** — `num_threads` (0 = hardware concurrency), fiber counts, deque/injection
  capacity, max counters.
- **`WorkerPool`** — `init()` / `shutdown()` / `push(JobDecl)` / `pump()` / `enqueue_fiber_resume(Fiber*)`.
- **`job_fiber_trampoline`** — static `noexcept` entry point burned into every fiber stack. Reads
  `tl_job_fn` / `tl_job_data`, calls the job, sets `tl_fiber = nullptr` as completion signal, then
  switches back to the scheduler context. Loops so re-acquired fibers restart cleanly.
- **`resume_fiber_fn` sentinel** — `job.fn == &resume_fiber_fn` means `job.data` is a suspended
  `Fiber*` to resume directly (used by counter wait/wake).

### Thread-local state

`tl_sched_ctx`, `tl_fiber`, `tl_job_fn`, `tl_job_data`, `tl_idx`, `tl_pool_ptr` — all
`thread_local`, initialized by `worker_loop` on entry and by `WorkerPool::init` for thread 0.

---

## Bugs fixed

### 1. TIB stack bounds not saved/restored in fiber_switch (Windows)

**Root cause:** Windows `__chkstk` and guard-page stack-probe rely on `GS:[0x08]` (StackBase) and
`GS:[0x10]` (StackLimit) matching the RSP currently in use. When `fiber_switch` swapped RSP to a
fiber stack without updating the TIB, any function that probed the stack (stack-checking functions,
functions with large frames) triggered an access violation.

**Fix:** `fiber_switch_win64.asm` now saves both TIB fields from the current thread into the `from`
`FiberContext` and restores them from the `to` context, atomically with the RSP swap. `FiberContext`
extended with `tib_stack_base` / `tib_stack_limit` fields (Windows-only, guarded by
`#if CRD_OS_WINDOWS`). `fiber_init_stack` populates these fields. `fiber_context.hpp` now explicitly
includes `platform.hpp` instead of relying on PCH ordering.

### 2. Fiber reuse crashes after 2+ uses (initial frame corruption)

**Root cause:** The original design stored a snapshot of the initial fiber context
(`f.initial_ctx = f.context`) and restored it on completion (`target->context = target->initial_ctx`)
to allow fiber reuse. This was fatally wrong: `fiber_init_stack` places the initial frame at the
top of the fiber stack. When `job_fiber_trampoline` calls `fiber_switch`, the register saves
(`push r14`, `push r13`, etc.) write into stack addresses that overlap the initial frame data. On
the second use, `r14` contains a function pointer from the first run; on the third use,
`ldmxcsr [rsp]` reads that address as MXCSR → access violation (STATUS_ACCESS_VIOLATION, exit 5).

**Fix:** Removed `Fiber::initial_ctx`. Added `Fiber::usable_base`, `Fiber::usable_size`,
`Fiber::trampoline` to store the parameters needed to rebuild the frame. On job completion,
`WorkerPool::run_job_in_fiber` calls `fiber_init_stack(target->context, target->usable_base,
target->usable_size, target->trampoline)` before returning the fiber to the pool. This rebuilds a
fresh initial frame that is not contaminated by any prior execution.

---

## Test results

10 new tests in `tests/jobs/test_jobs.cpp`:

| Test | Description |
|---|---|
| init and shutdown | `WorkerPool` lifecycle |
| re-init after shutdown | clean reset |
| default num_threads | fallback to hardware_concurrency |
| single job executes on worker thread | background thread picks up job |
| pump executes job on thread 0 | main-thread dispatch |
| pinned job executes via pump on thread 0 | pin_thread=0 isolation |
| multiple jobs all execute | 100 jobs, 4 threads |
| job runs on a fiber stack | stack pointer in fiber range |
| pump returns false when queue empty | empty-queue probe |
| concurrent multi-thread stress | 1000 jobs, 4 threads |

Also fixed pre-existing clang-tidy warnings in `test_scheduler.cpp`: lowercase `u` integer
literal suffixes → `U`; struct members without `m_` prefix; single-statement bodies without braces.

Six-configuration green:
- win-debug:          341/341
- win-relwithdebinfo: 341/341
- win-release:        338/338
- win-asan:           341/341
- win-clang-cl:       341/341
- win-tidy:           341/341

---

## Files changed

| File | Change |
|---|---|
| `engine/jobs/src/fiber.hpp` | removed `initial_ctx`; added `usable_base`, `usable_size`, `trampoline` |
| `engine/jobs/src/fiber_pool.cpp` | populate new `Fiber` fields; remove `initial_ctx` snapshot |
| `engine/jobs/src/worker_pool.hpp` | new file — `WorkerPool` declaration |
| `engine/jobs/src/worker_pool.cpp` | new file — `WorkerPool` implementation |
| `engine/jobs/src/fiber_init.cpp` | populate TIB fields; fix `u`→`U` suffix in hex literals |
| `engine/jobs/src/fiber_switch_win64.asm` | save/restore GS:[8] and GS:[16] alongside RSP |
| `engine/jobs/include/crd/jobs/detail/fiber_context.hpp` | add TIB fields; add explicit `platform.hpp` include |
| `tests/jobs/test_jobs.cpp` | new file — 10 `WorkerPool` integration tests |
| `tests/jobs/test_scheduler.cpp` | fix pre-existing clang-tidy warnings |
