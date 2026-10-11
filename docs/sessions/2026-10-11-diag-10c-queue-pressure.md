# DIAG.10c queue pressure and exhausted budgets, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c lifetime accounting](2026-10-11-diag-10c-lifetime-accounting.md).

## Goal

DIAG.10c's clause "reusable optional probes for ... queue/VRAM pressure and exhausted budgets at existing execution
boundaries", for the CPU side: the crd-jobs pool and the ResourceManager's memory budget. The OS view of device memory
(VRAM), GPU queue depth and the frame graph's transient budget are left to the driver/device-reset and host-pressure
clause, which owns the GPU driver calls. Committed locally, not pushed.

## The probe (crd-core)

`engine/foundation/core/include/crd/core/budget_pressure.hpp`:

| API | What it does |
| --- | --- |
| `BudgetReading` | bound (or `bounded == false`), held now, high water where the owner keeps it, refusals, soft-bound overruns, and whether it was read at all |
| `pressure_level(r, amount, near_permille = 900)` | Unread, Unbounded, Within, Near, Full, Over; Near is `amount * 1000 >= bound * threshold`, formed as `q * near + ceil(r * near / 1000)` so no product overflows (checked at `UINT64_MAX`); a threshold outside 1..1000 is taken as 1000 |
| `pressure_report(before, after)` | the window's verdict, worst first: Exhausted, Overrun, Saturated, Near, then Relaxed only when a kept high water proves the peak below Near, else Undetermined; Unjudged when the readings cannot be compared; the peak as a range |
| `format_pressure_report` | one line, e.g. `injection.normal: exhausted (in use 2/2, peak 2+, refused +1)` |

Decisions:
- The probe never guesses. A high water the owner does not keep bounds the window's peak only from below (rendered
  `peak 2+`), and a high water from before the window bounds it only from above (`peak 2..9`); a verdict that would
  need the missing side is Undetermined, never Relaxed.
- A refusal outranks everything: a window that refused a request is Exhausted even if both ends look calm.
- A monotonic count that went back, another bound, or a reading of an unbounded resource is Unjudged: two readings of
  different resources, or across a re-init, are not compared.
- Integer arithmetic only; no allocation; nothing is changed.

## The jobs boundary

`crd::jobs::pressure_snapshot()` (`engine/foundation/jobs/include/crd/jobs/jobs.hpp`):

| Reading | Bound | Held now | High water | Refusals |
| --- | --- | --- | --- | --- |
| `counters` | `max_counters` | acquired | new: raised after the acquire's own increment, beside `m_acquired` | `CounterPool::exhaustions` |
| `fibers[t]` | the tier's count | acquired | the existing per-tier peak | new per-tier tally |
| `injection[p]` | `injection_queue_capacity` | the queue's racy depth | not kept | new per-lane tally |
| `pinned` | `pinned_queue_capacity` per thread | the fullest lane | the highest lane peak (raised by producers) | new tally |

The injection queues keep no high water on purpose: a push would have to read the consumers' position, a cache line
the consumers write, on every push. A window over an injection queue is therefore never Relaxed.

### Defects found and fixed

1. **A full injection queue dropped the job.** `Scheduler::push` guarded `try_push` with `CRD_ASSERT_MSG` only; with
   asserts compiled out the job vanished, its counter never reached zero and its waiter hung. Fiber resumes go through
   the High injection queue, so a dropped resume would strand a parked fiber. Now the refusal is counted and is an
   always-on fatal, "injection queue full". `push_local` (a full local deque) had the same shape and is now an
   always-on fatal too; it has no public caller.
2. **The pinned lane was a single slot.** A second job pinned to a thread before that thread drained the first found
   the slot occupied: an assert where asserts are on, and an unconditional overwrite (the first job lost) elsewhere.
   Concurrent producers also wrote the plain slot without synchronisation. Authored CEIR reaches it: two
   `task.main_thread` tasks launched before either is awaited each lower to a job pinned to thread 0.
   - Against the old code (win-debug): the new crd-ceir-host-tests case and the first new crd-jobs-tests case both
     stopped on `Scheduler::push: pinned slot already occupied`.
   - Against the old code (win-shipping, asserts off): the first new crd-jobs-tests case hung until the 60 s timeout
     killed it.
   - Fix: each thread's pinned lane is a bounded MPMC queue (`crd::containers::ConcurrentQueue`,
     `Config::pinned_queue_capacity`, default 64, a power of two, checked by an always-on fatal at init), drained by
     its thread before anything else, as the slot was. A full lane is counted and is an always-on fatal, "pinned lane
     full". [ADR-0033](../decisions/0033-jobs-implementation-architecture.md) has a dated amendment.

A full queue or lane stays a fatal rather than a wait: the pool's existing rule for counter and fiber exhaustion is
to fail fast with a named, counted report, and a producer that blocks on a full lane of its own thread would never
return.

## The ResourceManager budget

The memory budget is soft: a load is never refused, and when every resident block is pinned, held or still loading,
eviction stops above the budget without a trace. `ResourceManager::memory_pressure()` reports the budget (unbounded at
the default `~0`), the use, the high water of the use once a load's eviction settled (or a reload's re-accounting,
which does not evict) and the overruns: the settles that ended above the budget. It never reports a refusal.

## Tests

| Program | New `[diag10c]` cases |
| --- | --- |
| crd-core-tests | 7: levels for unread and unbounded readings; the exact threshold (capacity 10, 1001 and `UINT64_MAX`, thresholds 1, 500, 1000 and out of range, a bound of 0); every Unjudged cause; the worst-first order; Relaxed only when proven (a high water from before the window, an untracked one); the rendered lines and a cut buffer; the stable names |
| crd-jobs-tests | 8: four pinned jobs in one submission and 96 concurrent pinned pushes from six producers all run on thread 0; the pinned reading after concurrent pushes is Relaxed with a high water of 1 to 3; readings unread before init and empty after it; a chain of 9 jobs held in a pool of 10 counters and 10 small fibers reads 9 of each and Near, a chain of 10 reads Full and the window Saturated, and the idle window after it Undetermined; a chain of 2 is proven Relaxed while the injection queue's window stays Undetermined; four queued and four pinned jobs on a one-thread pool fill an injection queue and a pinned lane of 4 (Full, Saturated) and drain; the queue-full specimen in both modes |
| crd-resources-tests | 1: the default budget reads Unbounded; two pinned loads into a one-blob budget read 2 blobs, Over, one overrun and an Overrun window; after unpinning, the next load evicts back to Full with no new overrun while the high water stays; a window at the budget is Saturated |
| crd-ceir-host-tests | 1: two `task.main_thread` tasks in flight before either is awaited return 77 + 5 |

Specimens (child processes, `tests/support/diag/specimens`):
- New `crd-diag-queue-full-specimen injection|pinned`: one thread that never pumps, a capacity of 2, three pushes. Its
  assert platform handler reads `pressure_snapshot()` on the named fatal, prints the window as
  `CRD_DIAG_PRESSURE=...` and exits 42 only if the window is Exhausted with one refusal at the bound. The test checks
  the exit and the exact line: `injection.normal: exhausted (in use 2/2, peak 2+, refused +1)` and
  `pinned: exhausted (in use 2/2, peak 2, refused +1)`.
- The counter and fiber exhaustion specimens now also require their pressure reading to show the refusal with the
  pool or tier at its bound (exit 60 otherwise).

## Teeth

Eleven deliberate breaks on win-debug, applied and restored one at a time by exact replacement (the restore rewrites
the file), each followed by a rebuild of the target and a run of the cases named:

| Break | Result |
| --- | --- |
| A full injection queue drops the job (no tally, no fatal) | queue-full case failed: exit 97 (a different assert fired later, not the queue's fatal; no pressure line) |
| The injection tally removed, fatal kept | exit 60 |
| A full pinned lane drops the job | exit 97, as above |
| The pinned high water never raised | 2 cases failed (high water 0) |
| The counter high water never raised | 2 cases failed (high water 0, verdict Within instead of Near) |
| The fiber tier's own refusal tally removed | fiber exhaustion case failed: exit 60 |
| `pressure_snapshot` reports no counter refusals | counter exhaustion case failed: exit 60 |
| The ResourceManager's settle not noted after eviction | resources case failed (high water 0) |
| The near threshold rounded down | threshold case failed (capacity 1001, `UINT64_MAX`, threshold 1) |
| Relaxed judged from the lower bound of the peak | 2 cases failed (Undetermined became Relaxed) |
| A high water that rose in the window ignored | 2 cases failed (Saturated and Near missed) |

The last break first failed to compile under `/WX` (C4551); that run used the previous executable and is not counted.
It was rewritten warning-free and rerun. After the last restore all four programs were rebuilt and passed whole on
win-debug, and no `TEETH` marker of this batch remains.

## Verification

Whole programs on the final source (crd-core-tests, crd-jobs-tests, crd-resources-tests, crd-ceir-host-tests):

| Lane | Result |
| --- | --- |
| win-debug | whole tree built (`all`, 409 steps); 63 / 189 / 130 / 55 cases pass |
| win-shipping | 55 / 181 / 130 / 55 pass |
| win-clang-cl-shipping | 55 / 181 / 130 / 55 pass; linked on the first try |
| win-asan (inside vcvars) | 63 / 189 / 130 / 55 pass; the only ASan reports are the crash-capture specimen's deliberate faults |

| linux-gcc-debug | 55 / 188 / 130 / 55 pass; `[diag10c]` 28 / 15 / 2 / 1 |
| linux-gcc-asan | 55 / 188 / 130 / 55 pass; `[diag10c]` repeated 3 times; the only ASan reports are the crash-capture specimen's (as in the previous batch) |
| linux-clang-tsan (`setarch -R`) | 55 / 188 / 130 / 55 pass; `[diag10c]` repeated 10 times with no TSan report; the whole jobs program's only TSan report is the data-race specimen's |

The `[diag10c]` repeats under TSan include the 96 and 24 concurrent pinned pushes from worker producers, the case
that raced on the old plain slot.

### A local TSan timeout, explained (not a code defect)

The first whole run of crd-jobs-tests on linux-clang-tsan failed one case: `tsan harness: the data-race specimen is
caught, or reported absent` got Timeout (10 s) instead of SanitizerCaught, and again on an immediate rerun of the same
executable. The specimen links only Threads and was not rebuilt by this batch. gdb on the hung specimen showed the
racing thread in `__tsan::ReportRace` -> `SymbolizerProcess::ReadFromSymbolizer`, waiting on its `llvm-symbolizer-18`
child. The lane script ran under `bash -lc`, whose Ubuntu profile sets `DEBUGINFOD_URLS`; after the WSL VM restart
the symbolizer waited on the unreachable debuginfod server. With the variable unset the specimen was caught in
0.05 s (exit 66) and the whole program passed (188 cases). It is registered as `wsl-tsan-symbolizer-debuginfod`
(status `explained`) in `tests/support/diag/nonreproduced-failures.json`; hosted steps run in non-login shells.

### Coverage (linux-clang-coverage on WSL)

- The registry gained the scope group `queue and budget pressure probe` and two outcome sets, PressureLevel and
  PressureVerdict. `PressureVerdict::Unjudged` is now assigned explicitly, so every value has a producing site.
- The measurement ran 949 CTests (the previous batch's 933 plus this batch's 16), all passing. Its first verdict was
  GAP for one reason only: `budget_pressure.hpp` holds declarations and constants, so it has no code to instrument.
  It left the scope paths (its enums are still read from it, as `os_resource_probe.hpp` was handled before), and the
  same export, rechecked with `--export`, is PASS.
- PressureLevel 6 of 6, PressureVerdict 7 of 7; the new group ran 124 of 127 regions and 111 of 116 branches.
- Totals, 78 of 78 scope files instrumented: regions 10,239/11,460, branches 6,741/8,530, functions 1,149/1,201, lines
  18,875/21,621. The 1.6 GB of profiles are deleted.

## Checks

- Strict tidy (LLVM 20.1.8, win-debug compile database) is clean on all 14 changed C++ sources after one fix (a
  nested conditional in a test helper).
- clang-format: the new files are clean; on the changed lines of the modified files it differs only in the
  repository's hand alignment, case-label indentation, constructor-initializer and include grouping. Two wraps that
  fit in 120 columns were joined.
- The Allman check, the 8 `crd-no-*`/`crd-check*` guards and the `crd-diag-coverage`, `crd-diag-seeded-faults`,
  `crd-build-profiles`, `crd-repository-tools`, `crd-developer-workflow`, `crd-ci-tiers` and `crd-master-plan` CTests,
  `diag-coverage.py --validate`, `test-diag-coverage.py` (29) and `test-diag-seeded-faults.py` (33; the seeded jobs
  and specimen CMake lines are untouched) pass; check-master-plan and check-repository pass.
- The full win-debug CTest was not run.

## What the hosted run must show

- crd-core-tests (twenty-eight `[diag10c]` cases), crd-jobs-tests (fifteen, including the queue-full specimen in both
  modes, and the concurrent pinned pushes under linux-clang-tsan), crd-resources-tests (two) and crd-ceir-host-tests
  (one) green on all six lanes, with the counter and fiber exhaustion specimen cases green with their pressure
  readings.
- `crd-diag-coverage` green with the budget-pressure scope group and the PressureLevel and PressureVerdict sets.

## Follow-ups

- The hang watchdog could classify a stall with a refusal or a saturated pool from these readings; its comment
  already names pool exhaustion as separate work.
- The specimen harness could clear `DEBUGINFOD_URLS` in its children, so a sanitizer specimen never waits on the
  network wherever the harness runs.
- The GPU side of pressure (VRAM through DXGI and `VK_EXT_memory_budget`, GPU queue depth, the frame graph's transient
  budget) belongs with the driver/device-reset clause.
