# DIAG.10c workloads, measured slowdowns and threads in the ledger, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c queue pressure](2026-10-11-diag-10c-queue-pressure.md).

## Goal

DIAG.10c's clauses "run warm/cold, long-soak, repeated reload and shutdown workloads with one changed stressor at a time
and seeded failure injection" and "longer offline work remains progressing; slowdowns are measured before classified",
plus the threads the lifetime-accounting batch left for "the shutdown workloads that exercise their owners". The
driver/device-reset and host-pressure clause (the GPU side) is the next batch. Committed locally, not pushed.

## The workload oracles (crd-core)

`engine/foundation/core/include/crd/core/workload_probe.hpp`, `engine/foundation/core/src/workload_probe.cpp`:

| API | What it does |
| --- | --- |
| `run_workload(spec, out)` | `cycles` x (setup, `iterations` iterations, teardown); iteration 0 of a cycle is cold; a ledger window per cycle; the worst finding, located |
| `workload_injects(seed, permille, cycle, iteration, iterations)` | which iterations are injected: pure, never iteration 0 or the last, only odd ones (a clean one follows each), splitmix64 below the share |
| `compare_stressors`, `attribute_finding` | Identical / OneChanged / SeveralChanged; a finding is Attributed only against a clean baseline differing in exactly one stressor, else NoNewFinding, BaselineFailing or NotOneStressor |
| `progress_report(readings, policy)` | Progressing, Unfinished, Stalled (no advance for longer than the bound), Regressed (the count went back), Unmeasured |
| `slowdown_report(baseline, candidate, policy)` | Unmeasured, Noisy, Steady, Faster, Inconclusive, Slower, by nearest-rank quartiles in integers |
| `format_workload_result`, `format_slowdown_report` | one line each |

Workload verdicts, worst first: SetupFailed, Diverged, FaultMissed, NotRecovered, IterationFailed, Leaked, Stalled,
FaultNotFired, Clean. An iteration reports success, its result signature and, when it knows it, the signature it should
have (otherwise it must equal cycle 0's cold signature). An injected iteration must report failure and keep the result
it says it keeps; the next must succeed with its expected result.

Decisions:
- Slowdowns are classified only from measurements: at least 8 samples in each window, a baseline median of at least
  1000 ns, and a baseline interquartile range no wider than its median (else Noisy). Slower needs the median past the
  250 permille tolerance AND the candidate's lower quartile above the baseline's upper quartile; when the quartiles
  overlap it is Inconclusive. A workload compares the halves of its warm, clean, non-recovery iterations.
- Hosted lanes never judge speed: the runners vary and CI has no retry. The real workloads assert results, lifetimes and
  progress, and print their one-line report (timing included) with `WARN` on every run as evidence. Every classifier
  verdict is held to synthetic durations; the stall cases sleep 4 and 1.4 times their bound.
- Progress is read at the end of every iteration, counting the work units of successful ones; setup and teardown fall
  inside the gaps, so a stall bound must cover an init/shutdown on the slowest lane. The real workloads use 5 s: the
  one wall-clock judgment the hosted lanes make, far above anything seen locally on a sanitizer lane.

## Threads in the lifetime ledger

`LifetimeClass::Thread` (sixth class, "thread"): acquired after the thread is constructed, released after it is joined.

| Owner | Acquired | Released |
| --- | --- | --- |
| crd-jobs workers (`WorkerPool::init`) | each `emplace_back` | `WorkerPool::shutdown` join |
| crd-jobs hang watchdog | `jobs::init` with a watchdog period | `jobs::shutdown` join |
| asynchronous logger worker | `log::init` with `async` | `log::shutdown` join |
| an inspect host's run | `InspectHost::start` | the join in a later load, start, `wait_finished` or the destructor |
| `DiagCommandPanel` request worker | `send` | `join` (the tick that delivers the answer) or the destructor |
| WASAPI render thread | `AudioDevice::start` | `stop`, or the join of a failed start |

Not wired: the crash handler thread (it lives for the process). Still outside the ledger: the window, the WASAPI
device's COM objects and event, and native device objects without a Cerid identity; they belong with the device clause.

The OS oracle is identity: `os_current_thread_identity()` read inside the thread (Windows: thread id and creation
FILETIME; Linux: gettid and the starttime of `/proc/self/task/<tid>/stat`), and `os_thread_alive(identity)`: Present
while it runs; Absent once it has exited (also while still unjoined), when its id names another process's thread or a
later thread (different start time), or when it is a zombie; Unavailable for an identity never read. A count of the
process's threads is not used: the OS loader and thread pool start and retire threads of their own.

## Workloads in place

- crd-jobs-tests, `test_diag_workloads_jobs.cpp`: three cycles of `jobs::init` (2 threads, watchdog on), 64 parallel
  sums of i*i over 16 chunks with the closed-form answer, `jobs::shutdown`. Each cycle's cold iteration reads every
  worker's identity through a job pinned to it and asks the OS whether it runs. Checked: every sum, a balanced window
  per cycle (the Thread delta during the cycle equals the thread count: workers plus watchdog), every worker Absent
  after its cycle, progress within 5 s. The 4-thread variant is clean (NoNewFinding); the variant with one 7 s delay
  is Stalled and attributed to Delay.
- crd-resources-tests, `test_diag_workloads_reload.cpp`: three cycles of a fresh ResourceManager (mount a one-mesh pack,
  cold load, 23 reloads of new content each followed by a warm load of the resident mesh, unmount and destroy). The
  memory-budget variant (1 byte) stays clean and counts overruns; the failed-read and short-read variants arm a one-hit
  plan on the real I/O seam before 11 reloads a cycle: each injected reload fails and keeps the last good mesh, the next
  swaps the new one in. A shipping build refuses every arm and the variants answer FaultNotFired. The PACK and MESH
  helpers moved from `test_fault_injection.cpp` into `tests/assets/resources/fault_pack_support.hpp`.
- crd-core-tests, `test_diag_workload_probe.cpp`: every verdict of the five new outcome sets from scripted workloads and
  synthetic series, and the OS thread oracle against a thread the case starts.
- Thread ledger checks: `test_diag_lifetime_jobs.cpp` (workers), `test_diag_lifetime_log.cpp` (the async worker,
  delivered-on identity Present then Absent), `test_inspect_host.cpp`, `test_diag_panel.cpp` and the WASAPI soak.

## Defect found and fixed: a job pinned to a worker could be stranded

On the default (shared-semaphore) wake path a pinned push posted one token and woke one arbitrary sleeper. When that was
another worker, it took the token, found nothing it could run and slept again; the target slept on with the job in its
lane, and its waiter hung. Found by the jobs soak: its 4-thread variant hung on the second run on win-debug, a debugger
showing the main thread in `jobs::wait` and every worker asleep in `Semaphore::wait_on_zero`. The pinned idle-worker
case (8 threads, a job pinned to each worker after they have gone idle, waited on with a deadline) failed on the old
scheduler at its first push (round 0, worker 1).

Fix (`engine/foundation/jobs/src/scheduler.cpp`, `scheduler.hpp`, `semaphore.hpp`):
- a pinned push on the default path issues a sequentially consistent fence and `Semaphore::release_broadcast()` (one
  token, every sleeper woken);
- a worker going to sleep sets `ThreadState::sleeping`, fences, and takes a last look at its own pinned lane; before
  sleeping it also looks for another announced sleeper whose pinned lane holds a job, and if it finds one it wakes every
  sleeper and returns instead of sleeping (`Scheduler::wake_pinned_sleeper`).

With the fences, either the target's last look sees the job, or whoever takes the push's token sees the target's
announcement and its job and wakes it. The broadcast alone leaves a window (another worker takes the token while the
target is between its last look and its sleep); the kick alone needs a token's taker to look. The targeted wake path
(ADR-0094) is unchanged. [ADR-0033](../decisions/0033-jobs-implementation-architecture.md) has a dated amendment.

## Defect found and fixed: flush() could return before the last record was written

The async-logger thread case failed once in ten TSan repetitions: `REQUIRE(seen.records == 1)` after `log::flush()`
saw 0, and the sink (on the test's stack) was then written after the case had ended. `flush()` promises "block until
all queued records are written", but it waited only for an empty queue; the worker pops a record, releases the queue
lock and only then takes the sink lock, so a flush could take the sink lock first and return early. Fix
(`engine/foundation/log/src/logger.cpp`): the worker counts the record it is delivering under the queue lock and
notifies the drain only when the queue is empty and nothing is being delivered; `flush()` and `flush_for()` wait for
both. The new case `flush returns only after the record the worker is delivering is written` logs one record and
flushes 4000 times, each after a pause of 0 to 63 microseconds that sweeps the worker's pop-to-lock window. With the
old predicate it failed on win-debug (1 early flush in 4000 rounds); the detection is probabilistic, the pass is not
(with the fix a flush cannot return while its record is queued or being delivered). The thread case now keeps its
sink state in a static, so a failed assertion can no longer leave the logger writing into a dead stack frame.

## A flaky diagnostic case fixed in passing (DIAG.4b)

`crd-jobs-schedcheck-tests` (never in CI; run directly on the coverage lane) failed `schedcheck: the driver replays a
scripted order without deadlock` 3 of 20 times under linux-clang-tsan, and 7 of 40 with the previous scheduler, so the
pinned-wake fix did not cause it: with three threads a peer worker could finish the child before the root reached its
wait, so the root never parked and none of the scripted points occurred (`seen.size() == 0`). In both park-point cases
the child now sleeps 20 ms before it finishes.

## Teeth

Each break applied alone on win-debug, the target rebuilt, the named cases run, the file restored byte for byte:

| Break | Result |
| --- | --- |
| worker thread never released at shutdown | `test_diag_lifetime_jobs.cpp` failed |
| watchdog thread never acquired | the jobs soak failed (Thread delta) |
| logger worker never released | the async logger case failed |
| inspect run thread never released | the inspect host case failed |
| panel worker never acquired | the panel case failed |
| render thread never released by `stop` | the WASAPI soak failed |
| OS probe ignores the start time | the forged-identity check failed |
| OS probe counts an exited, unjoined thread as running | the ended-is-ended check failed |
| slowdown ignores the quartile separation | the Inconclusive checks failed |
| slowdown classifies from one sample | the Unmeasured checks failed |
| slowdown ignores the baseline's spread | the Noisy check failed |
| two changed stressors accepted as one | the comparison and attribution checks failed |
| a reading without an advance resets the gap | the stalled-progress check failed |
| a fired fault reported as success passes | the FaultMissed section failed |
| a cycle's ledger window not judged | the Leaked sections failed |
| recovery not judged | the NotRecovered section failed |
| attribution ignores how many stressors changed | the NotOneStressor checks failed |
| pinned push wakes one sleeper (kick kept) | survived |
| no kick (broadcast kept) | survived |
| the old scheduler (both halves removed, HEAD) | the pinned idle-worker case failed at its first push |
| flush waits only for an empty queue | the flush case failed (1 early flush in 4000) |

Eighteen of the twenty single breaks failed a case. The two pinned-wake halves survive alone by design: each closes
the common case, and only the race between them, which no deterministic test can force, needs both. Both OS-probe teeth
were rerun after the probe's branches were merged for tidy.

## Verification

All on the final source, the seven touched test programs run whole (crd-core, crd-jobs, crd-log, crd-resources,
crd-ceir-cook, crd-perf-ui and crd-audio tests):

| Lane | Build | Result |
| --- | --- | --- |
| win-debug | whole tree (`all`) after the header change, then the touched targets | all pass: core 72 cases, jobs 192, log 33, resources 131, ceir-cook 115, perf-ui 21, audio 29 |
| win-shipping | touched targets | all pass (core 64, jobs 184, log 31, resources 131, ceir-cook 115, perf-ui 14, audio 29) |
| win-clang-cl-shipping | touched targets, linked first time | all pass (same counts as win-shipping) |
| win-asan (inside vcvars) | touched targets | all pass (same counts as win-debug) |
| linux-gcc-debug | whole tree (`all`) | all pass, plus crd-jobs-schedcheck-tests 6 cases |
| linux-gcc-asan | touched targets | all pass; the `[diag10c]`, `[workload]` and `[lifetime]` cases of six programs 3 times |
| linux-clang-tsan (`setarch -R`) | touched targets | all pass; the same cases 10 times each, no failure |

- Sanitizer reports appear only from the specimens that fault on purpose: the crash-capture specimen's 12 lines in
  crd-core-tests on linux-gcc-asan and the data-race specimen's report in crd-jobs-tests on linux-clang-tsan.
- The jobs `[workload]` cases ran 15 times in a row on win-debug, each bounded at 120 s, with no hang.
- The repaired schedcheck driver case passed 40 of 40 under linux-clang-tsan and the whole schedcheck program 10 of 10.
- The WASAPI soak (this host has a render endpoint) ran on all four Windows lanes with its render-thread checks.
- Coverage (linux-clang-coverage, clang 18): 962 CTests and the schedcheck program passed; 79 of 79 scope files
  instrumented; verdict PASS. The new group `workload oracles` ran 300 of 331 regions and 264 of 310 branches; the
  outcome sets StressorChange 3/3, SlowdownVerdict 6/6, ProgressVerdict 5/5, WorkloadVerdict 8/9 (Clean is a
  `default-value` exclusion) and FindingAttribution 4/4, and OsPresence 4/4 and LifetimeVerdict 4/4 with the thread
  probe. Totals: regions 10,602/11,858, branches 7,043/8,898, functions 1,180/1,232, lines 19,459/22,249. The 1.7 GB
  of profiles are deleted.
- Strict tidy (LLVM 20.1.8) is clean on all 26 changed C++ files (the shared test header gated through its owner,
  crd-resources-tests). The Linux-only branches of the thread probe were compiled with -Werror on three lanes but not
  tidied. clang-format differs only in the repository's hand alignment, case labels, include grouping and short bodies.
- The Allman check, `diag-coverage.py --validate` (79 scope files, 49 outcome sets, 22 exclusions),
  `test-diag-coverage.py` (29), check-master-plan and check-repository pass; on win-debug the 8 `crd-no-*` guards and
  `crd-diag-coverage`, `crd-diag-seeded-faults`, `crd-master-plan`, `crd-repository-hygiene`, `crd-build-profiles`,
  `crd-repository-tools`, `crd-ci-tiers` and `crd-developer-workflow` pass (16 of 16).
- After review the real workloads' progress bound was widened from 1 s to 5 s (and the delay variant from 1.5 s to
  7 s), so a slow hosted runner's init/shutdown cannot read as a stall. Only those test constants changed; the jobs and
  resources `[workload]` cases were rebuilt and rerun on win-debug, win-shipping, win-clang-cl-shipping, win-asan,
  linux-gcc-asan and linux-clang-tsan and pass. The coverage run above predates that change.
- The full win-debug CTest was not run. No script or workflow changed.
- Running the resources tests from the repository root leaves an empty, ignored `.crd_cook_test_tmp`; it was deleted
  before the repository checks.

## Left open

- The driver/device-reset and host-pressure clause (the GPU side of pressure, the mode witness around driver calls),
  with the window, the WASAPI device's COM objects and event, and native device objects without a Cerid identity in
  the ledger.
- `flush_for` now also waits for a record being delivered: an assert raised on the logger worker outside a sink, while
  a record is in delivery, waits out the bridge's timeout before giving up, where it used to proceed at once. No
  DIAG.5c case covers that path.
- The flush case's detection is probabilistic (1 early flush in 4000 rounds); a seam between the worker's pop and its
  sink lock would make the tooth bite every time. The schedcheck repair is a 20 ms sleep; a counter barrier would make
  the park deterministic.
- `WorkloadResult` holds two 1024-sample arrays (about 16 KB) and is returned by value: fine on a thread's stack, too
  large for a small fiber stack.
- The Linux branches of `os_resource_probe.cpp`, now with the thread probe, are compiled with -Werror but not tidied.
- `fault_pack_support.hpp` needed an explicit owner (crd-resources-tests) for the tidy gate to analyse it.

## What the hosted run must show

See the DIAG.10c row: the new `[diag10c]` counts (core 37, jobs 18, log 5, resources 3, ceir-cook 5, perf-ui 2), the
workload, pinned idle-worker and thread-ledger cases on all six lanes (jobs and resources under linux-gcc-asan and
linux-clang-tsan), the reload workload's fault variants answering FaultNotFired on the shipping lanes by design, and
`crd-diag-coverage` with the workload-oracle group and the StressorChange, SlowdownVerdict, ProgressVerdict,
WorkloadVerdict and FindingAttribution outcome sets.
