# 2026-05-15 — Detour D-003 v0c: `crd-perf` jobs adapter + `win-shipping-profile` preset

## What shipped

Three things in this slice:

### 1. `win-shipping-profile` preset (per-slice DoD extension)

`CMakePresets.json` gains a new configure / build / test preset that
mirrors `win-shipping` (LTCG + `/O2` + `/OPT:ICF` + no asserts + debug
PDB) but with `CRD_ENABLE_PROFILING=ON`. Validates that `crd-perf` +
every `CRD_PERF_*` instrumentation site in the engine compiles + runs
correctly under the same optimizer the consumer ship binary sees.

**Per-slice DoD expanded 4 -> 5 configs**: win-debug + win-asan +
win-shipping (profiling OFF, off-gate zero-overhead contract) +
**win-shipping-profile (profiling ON, LTCG)** + win-tidy. Codified in
`feedback_per_slice_run_ctest.md`.

v0a + v0b retroactively validated under `win-shipping-profile`:
**1795/1795 full project ctest** passed — no LTCG-class bug in the
substrate retroactively surfaced.

### 2. `crd-jobs` `JobObserver` hook

New `engine/jobs/include/crd/jobs/observer.hpp` + `src/observer.cpp`
(~50 LOC). Single-subscriber, function-pointer-table observer with four
callbacks:
- `on_job_begin(fiber, thread_idx, priority, stack_tier)` — fires just
  before the job callable executes.
- `on_job_end(fiber, thread_idx)` — fires right after the callable
  returns (before fiber recycle).
- `on_fiber_yield(fiber, thread_idx)` — fires when the fiber suspends
  inside `counter_wait`.
- `on_fiber_resume(fiber, thread_idx)` — fires when a previously-yielded
  fiber resumes on `thread_idx` (which may differ from the yielding
  thread — that's the fiber-migration capture point).

`set_observer(*)` / `current_observer()` are the install/get pair.
Observer pointer is `std::atomic<const JobObserver*>`, acquire/release
ordered. Wired into `WorkerPool::run_job_in_fiber` (4 call sites:
on_fiber_resume for the resume path, on_job_begin before the initial
switch, on_job_end after the completion-path returns, on_fiber_yield
after the suspension-path returns). Cost when no observer: one nullptr
load + branch-not-taken per scheduler entry.

### 3. `crd-perf` jobs adapter

New `engine/perf/include/crd/perf/jobs_adapter.hpp` +
`src/jobs_adapter.cpp` (~230 LOC). Turns `JobObserver` events into
`crd-perf` Sample push/pop:
- `on_job_begin` -> `register_thread("job-worker")` lazily +
  `set_current_fiber_id(fiber pointer)` + `push_region(name="job",
  Category::Job)`. The returned `BeginToken` is **parked in a
  fixed-size open-addressed table keyed by `FiberHandle`** (512 slots,
  hash-of-pointer + linear probe, mutex-protected on insert/erase).
- `on_job_end` -> `take_token(fiber) -> pop_region(token,
  Category::Job)`. The pop writes the Sample to whichever thread is
  currently running — if the job migrated mid-flight, the
  resulting Sample has `begin_thread != end_thread`, exactly as the
  v0a-locked wire format prescribes.
- `on_fiber_yield` -> stats bump only (single paired Sample per job is
  the locked v0c shape; the future v0g UI can synthesise yield-split
  markers from the `set_current_fiber_id` history when needed).
- `on_fiber_resume` -> `register_thread` lazily +
  `set_current_fiber_id` so nested `CRD_PERF_SCOPE` regions tag with
  the right fiber id post-migration.

`install_jobs_adapter()` / `uninstall_jobs_adapter()` are idempotent;
both reset the stats counters so each off->on transition starts from
zero (catches the test-fixture contamination case).

**Module dep**: `crd-perf` now PUBLIC-links `crd-jobs`. The direction
is "substrate observes substrate" — `crd-jobs` does not depend on
`crd-perf`; the profiler subscribes via the function-pointer table.

**Tests `tests/perf/test_jobs_adapter.cpp`** (6 cases / 22 assertions):
- `install_jobs_adapter` is idempotent.
- single `run_and_wait` job produces one `Category::Job` sample +
  matching jobs_begun/jobs_ended stats.
- `parallel_for(64 items, 8 jobs)` produces exactly 8 job samples +
  exact computation (sum 0..63).
- Every job sample carries a non-zero `fiber_id` — the wire-format
  field for cross-thread reconstruction.
- A `CRD_PERF_SCOPE("inner_work")` inside a job is recorded as a child
  region with `depth >= 1`.
- With adapter never installed, zero samples are produced — the off
  path is truly inert.

## Design decisions locked at v0c

1. **Single-subscriber observer** (not multi-cast). Profiler is the
   sole consumer; the multi-subscribe surface is YAGNI. If a debug
   tool ever needs simultaneous observation, it can chain through a
   composite adapter — not a v0c concern.
2. **Function-pointer table, not virtual interface.** Saves a vtable
   indirection on the hot path; observer can be set/cleared without
   construction/destruction overhead.
3. **Fixed-size token table (512 slots, mutex-protected on insert /
   erase).** Cerid's fiber count is bounded at startup (Small 128 +
   Medium 64 + Large 16 = 208 default); 512 slots gives a ~40% load
   factor under pessimistic conditions. Lookup is a single
   pointer-keyed acquire-load + probe — no allocation per job.
4. **One paired Sample per job (begin_thread vs end_thread captured).**
   Fiber yield events bump stats but emit no Sample. UI's "this job
   migrated" rendering reads `begin_thread != end_thread` from the
   single Sample, not a synthesised pair. The locked v0a wire format
   is unchanged.
5. **Pointer-as-fiber-id** (truncated low 32 bits of `Fiber*`). Safe
   for in-process identity within one capture window. Pointer
   re-use after fiber recycle is a non-issue because the begin/end
   pair completes within one scheduler dispatch.
6. **Stats reset on install + on uninstall.** Idempotent re-install
   preserves running counters; transitioning off -> on (or on -> off)
   clears them. This was driven by a test-fixture contamination
   discovered when test #2's residual job count broke test #3's
   exact assertion.

## Verification

| Config | Result |
|---|---|
| win-debug | **PASS** (1806/1806 full project ctest; 59/59 perf cases — 172 assertions) |
| win-asan | **PASS** (59/59 perf cases — 172 assertions) |
| win-shipping | **PASS** (6/6 perf cases — 23 assertions; 53 gated cases compile out at gate — off-path inert) |
| win-shipping-profile | **PASS** (59/59 perf cases — 172 assertions under LTCG + max optimization) |
| win-tidy | **PASS** (build clean) |

5-config per-slice DoD met. First slice to run the new protocol.

## Issues encountered

1. **Stats contamination across test fixtures.** `JobsAdapterStats
   g_stats` is a singleton; test #2 left `jobs_begun = 1` so test #3
   saw 9 instead of 8. Fixed by resetting `g_stats =
   JobsAdapterStats{}` on every off -> on (install) and on -> off
   (uninstall) transition. Idempotent re-install keeps the running
   totals (early-return path).
2. **`tl_thread_index()` is the right read inside `run_job_in_fiber`,
   not `worker_index()` from the public API.** The opaque accessor
   is the post-fiber-migration-safe path; the public function
   inlines through to the same TLS but might get CSE'd across the
   fiber_switch. Re-reads it both before AND after the switch so
   the observer sees the right thread index when the fiber resumes
   on a different OS thread.

## What unlocks now

- **v0d** can begin: GPU timestamp backend in `crd-rhi-vulkan`.
  `VulkanProfilerBackend` (VkQueryPool + multi-frame-in-flight
  resolve) + `Profiler::set_gpu_backend(IProfilerGpuBackend*)` +
  `CRD_PERF_GPU_SCOPE(cmd, "name")` macro.
- The substrate is consumable today by sandbox / smoke / engine
  modules — call `install_jobs_adapter()` after `perf::init()` and
  every job in the engine produces a labeled, fiber-aware,
  thread-aware Sample. **Zero call-site code needed.**
- The 5-config per-slice DoD is now the standard for D-003 and
  every future slice. `win-shipping-profile` will catch any
  LTCG-class bug in v0d's VkQueryPool wrapper or v0g's ImGui
  rendering loop before they ship.

## Next

**v0d — GPU timestamp backend.** New code in `engine/rhi-vulkan/`
(implements the `crd-time::gpu_timestamp.hpp` opaque-handle API
from D-006) + `engine/perf/include/crd/perf/gpu_scope.hpp` for the
macro. ~500 LOC engine + ~250 LOC tests, 2-3 days.
