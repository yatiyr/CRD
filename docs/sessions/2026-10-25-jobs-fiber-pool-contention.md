# JOBS-HARD.3: the fiber pool's contention, fixed, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [JOBS-HARD.3](../ROADMAP.md#slice-jobs-hard.3). Contract:
> [ADR-0033](../decisions/0033-jobs-implementation-architecture.md) decisions 8 and 9; readings:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Board:
> [bench](../bench/2026-10-25-jobs-fiber-pool-contention.md). Rules: [AGENTS](../../AGENTS.md). Previous:
> [external sampling](2026-10-25-diag-11b-external-sampling.md).

The user asked for the fiber pool's contention to be fixed fully. The 2026-10-25 WPR trace of the overhead harness's
jobs workload (2048 tiny jobs a frame, eight threads on 16 logical processors) put 53% of the off arm's sampled CPU in
`FiberPool::acquire`, 37% on the instruction after the free-list head's `lock cmpxchg`. The complete-tier run
40195335794 was left running and untouched; no CI run was started.

## Step 0: the harness-tree check could never pass (1e64d18d)

`sample-overhead-wpr.py record` refused the user's elevated run with TOOL_MISSING on two freshly built trees: a dry
run of any target on a CONFIGURE_DEPENDS tree stops at the always-dirty glob-verification phony and never reports the
target. The check now runs the glob check for real and dry-runs build.ninja and the target past that edge; the
[external-sampling session](2026-10-25-diag-11b-external-sampling.md) has the addendum, the mechanism and the teeth.
`diag-overhead.py` also gained `--before-exe` (bd2d2893): `run` interleaves the before-off, before-basic and
before-full arms of the build made before a change and reports each mode's change in frame time and CPU per frame;
`instructions` counts that build's off mode. Tests and teeth in `test-diag-overhead.py` (22 cases).

## Cause

Every job acquired a fiber from its tier's Treiber stack and released it there: two compare-exchanges on the head,
plus the in-use count and the peak on the same line, per job, from every worker. The head itself is sound: a 64-bit
word of index and pop generation, every pop bumps the generation, so ABA needs exactly 2^32 pops inside one window.
Nothing else shares the line: the head, the count and the peak are written by the same thread in the same call, so
co-locating them costs one line transfer instead of two, and each tier has its own line. The cost was true sharing:
one line written twice per job by every worker.

## Design and why

**Considered first, rejected: a per-worker cache of idle fibers** in front of the free list, filled and drained in
batches (the shape the task suggested, as Go's per-P free lists and jemalloc's tcache do). It makes the free list
local, but a cached fiber is idle, so the in-use count becomes a sum of per-worker counts and the high water the peak
of that sum. That peak cannot be observed without a global update on every acquire, and `crd::BudgetReading` promises
a high water the pool really held and never below any level it held: `pressure_report` proves a window Relaxed from
it, and the queue-pressure cases assert `in_use == 9` and `high_water == 9` exactly. An idle cache therefore keeps one
contended atomic per job or gives up the exact readings.

**Chosen: the kept fiber** (1a12466c, e0de26cc). A thread keeps the fiber of the job it completed, still acquired,
rebuilds its stack and runs its next new job of the same tier on it. It returns it before a resume, before a job of
another tier, just before it blocks waiting for work (the semaphore's pre-sleep step, through a `SleepHook` on both
wake paths), when it leaves its loop, when the main thread's `wait()` or pump returns, and at shutdown. The pool's
counters are untouched, so the readings stay exact: a kept fiber is held, nothing else can take it. Stealing a kept
fiber was considered and rejected: a reading could then show every fiber in use while a request would still be served,
so Full and Saturated would overstate the pool. It is not needed for safety: each other thread holds at most one fiber
at any moment, so a refusal still needs `parked + threads − 1` fibers held, the condition before the change, and the
sizing rule (parked fibers plus threads) is unchanged. The slot is a plain pointer only its thread touches, so there is
no hand-off between threads and no interleaving for the schedule-check twin to explore; no yield point was added.

The first version (1a12466c) returned the fiber on every empty `try_pop`. At sixteen threads that left one free-list
acquisition for every 3.6 jobs (112,354 for 409,600), because a worker that finds the queues empty usually finds a
token too and loops without sleeping. e0de26cc returns it only at the pre-sleep step: 203, 207 and 50 to 124
acquisitions at four, eight and sixteen threads (`FiberPool::acquisitions()`, a per-tier tally added for this).

**Then the wake (cdcc3e79).** On linux-gcc-shipping at the boards' shape (four threads on 5555) e0de26cc made off
25.3% slower and basic 18.8% slower than before, while full got 25.8% faster: a mode doing more work per job became
faster than off, the signature of contention that per-job work relieves. The cause was in the scheduler, exposed by
the change: every `push()` posted a token and a wake, a token is consumed only by an idle worker, so a frame of 2048
jobs left up to 2048 tokens for idle workers to spin through (an empty scan of every queue each), and the producer paid
a `FUTEX_WAKE` syscall per job on Linux even with no sleeper (`Semaphore::wake_one` is unconditional). The pool's two
compare-exchanges per job had paced the consumers; without them they drained faster than the producer filled, spun on
the shared queue and semaphore lines and slowed the producer. (`strace` is not installed in the WSL distribution and
installing it is a download that waits for the user; the per-push wake is read from the code, the rest from the A/B
boards.) `jobs::run` of a span now pushes its jobs without a wake each and posts at most one token per thread with one
wake, after its first injected job and after its last; pinned jobs keep their own broadcast and single pushes are
unchanged. The `Semaphore` protocol is untouched.

## Results

Numbers are in the [bench note](../bench/2026-10-25-jobs-fiber-pool-contention.md), each a paired, interleaved board
of the jobs workload, cdcc3e79 against the c4a56808 build (the final boards ran while an interactive game used 1 to 4
cores; recorded there):

| Lane | Shape | off: frame, CPU | basic | full |
|---|---|---|---|---|
| win-shipping-profile | 4 threads on 5555 | -39.9%, -42.4% | -40.6%, -43.0% | -30.9%, -32.9% |
| win-shipping-profile | 8 threads on FFFF | -50.2%, -67.0% | -52.0%, -64.9% | -29.3%, -49.7% |
| win-shipping-profile | 16 threads on FFFF | -88.6%, -86.2% | -87.7%, -86.2% | -83.4%, -81.3% |
| linux-gcc-shipping twin | 4 threads on 5555 | -46.1%, -57.1% | -44.2%, -54.6% | -38.4%, -44.6% |
| linux-gcc-shipping twin | 8 threads on FFFF | -46.8%, -68.6% | -46.8%, -68.6% | -38.0%, -61.2% |
| linux-gcc-shipping twin | 16 threads on FFFF | -98.0%, -87.6% | -98.0%, -88.0% | -97.6%, -87.8% |

The sixteen-thread frames were 1.7 to 2.0 ms on Windows and 15.8 ms on WSL2 before; they are 0.23 and 0.32 ms now.
cachegrind, off mode on linux-gcc-shipping: 1,207,675 → 1,050,694 instructions a frame at one thread (-13.00%),
about 2.83 M → 1.25 M at four (-56%); off over compiled-out +0.00% (within the 1% budget), and in the four-thread boards
-0.80% (Windows) and -0.31% (Linux). Full now costs +57% (Windows, contended) and +42% (Linux) over off at four
threads: the pool's contention no longer hides its per-job work.

Caveats, stated rather than left to the tables: the final boards ran on a contended host (the e0de26cc boards, idle,
show the kept fiber alone; the batch-wake probes had no host check); on the contended eight-thread Windows board basic
read +4.3% CPU over off, "over (2%)" (a 24-process board, not a budget board; the idle e0de26cc eight-thread board read
-2.4%), and with the pool's contention gone basic's own per-job cost is no longer hidden, so the budget boards and the
elevated sampling decide it; the sixteen-thread Linux "before" frames (15.8 ms on WSL2) were pathological, so -98%
describes that host, not a general factor.

## Correctness

| Lane | Runs |
|---|---|
| win-debug | `tests/jobs` (211 CTests) three times, `tests/diag-overhead` (20), the ctest guards (15), on every commit |
| win-asan (inside vcvars) | `tests/jobs` three times, `tests/diag-overhead` |
| linux-gcc-debug, linux-gcc-asan, linux-clang-tsan (mirror lanes) | `tests/jobs` and `tests/diag-overhead` on every commit; ASan and TSan `tests/jobs` four more times; both pinned to two CPUs beside two pinned busy loops (twice each); the fiber-keep, minimization, handshake, perturbed-stress, cross-thread and queue-pressure cases 20 times under that load; the minimization and handshake cases 100 times on TSan under it |

New cases, each with teeth (break, fail, restore, rebuild, pass): `test_fiber_keep.cpp` (reuse and its progress-epoch
reset; the other-tier return; the shutdown drain with no assert and the lifetime ledger balanced; a resume returns the
kept fiber first, readings exact at 1 and 2; a fiber parked on worker 1 completes on another, which keeps and returns
it; at the exact sizing bound a worker still gets a fiber while the others stream and keep theirs, and the kept ones go
back when they idle; readings while jobs stream, on both wake paths; the main thread's pumps; the free list's traffic,
at least eight jobs an acquisition) and `test_batch_wake.cpp` (tokens capped at the thread count; a batch wakes all
three sleeping workers for a four-job rendezvous). Eleven breaks of the kept fiber and three of the batch wake each fail
them. The queue-pressure, task-identity, pool-exhaustion, lifetime and diag-overhead cases pass unchanged; the
coverage registry (`crd-diag-coverage`) and the seeded faults pass on win-debug.

A schedule-check finding on the way: the first version added gate-only yield points to the pool's shared path to count
its traffic. The minimization case builds its seed from the recorded trace of every yield point, so those points landed
in seeds at timing-dependent places and the replay wedged at the driver's deadline (one in three repeats on win-asan
under load, and on linux-gcc-debug). The points were removed in favour of the `acquisitions()` tally; the schedule-check
suite is back to its own tags and passed 100 of 100 minimization runs on TSan under the neighbour.

## A pre-existing TSan-only corruption the new case exposes

On linux-clang-tsan, "fiber keep: while tiny jobs stream the readings stay exact and bounded, and an idle pool holds
nothing" failed once in the lane run and about one in 25 to one in 300 runs alone: a SIGSEGV with a wild pointer, in
the reader thread (`Scheduler::pinned_pressure`) or a worker (`keep_fiber`) or TSan's `MemoryAccess`. One direct run
hung instead and was killed after about two hours (its last live thread in `hrtimer_nanosleep`), so on a hosted lane the
symptom may be a CTest timeout of that case rather than a crash. Evidence:

- It reproduces on c4a56808, before any of this work, with a probe using only the public API (3 of 49 runs with two
  shared-path pools, 2 of 150 with a shared and a targeted pool), so it is not a regression of these commits.
- It needs ThreadSanitizer and an outside thread calling `jobs::pressure_snapshot()` while the pool runs: 0 of 1000
  runs unsanitized (linux-gcc-debug), 0 of 300 under ASan, 0 of 300 with a reader that reads no pool state, 0 of 300
  for the existing cross-thread fiber-resume stress under TSan.
- gdb, at the crash: the faulting thread runs on its own stack, inside no fiber stack of either pool (all 416 recorded);
  no signal arrives before the fault; hardware watchpoints on the scheduler's four thread-state slots saw only init and
  shutdown write them. The bad values are the reader's own callee-saved iterator register holding the return address
  of its call into `__tsan_atomic32_load`, a pointer read as eight bytes of machine code, pointers cut to 32 bits, and
  values shaped `{pid, uid}`.
- That points at TSan's runtime with many fibers (each `__tsan_create_fiber` is a TSan thread; two pools are 416), not
  at the pool. An LLVM fix in this area concerns the per-thread signal context `__tsan_destroy_fiber` left behind
  ([commit](https://repo.hca.bsc.es/gitlab/rferrer/llvm-epi/-/commit/d8a0f76de7bd98dc7a271bc15b39a4cdbfdf6ecb)); whether the
  hosted runtime has it is not checked. The third-party register needs a reproduction without Cerid code, which this is
  not yet, so it is a JOBS-HARD.3 finding. The case is kept as it is; the hosted linux-clang-tsan run decides whether it
  follows the runtime or this host (whose WSL processes are known to misexecute, `host-misexecutes-local-faults`).

## Verification and housekeeping

- `tidy-files.py` clean on every changed file (it also flagged the pre-existing `for_each_fiber` forwarding reference,
  now `const Fn&` like `for_each_counter`); `check-allman-braces.py`; `clang-format --dry-run` reports nothing new on
  added lines beyond the hand-aligned columns of the surrounding code.
- win-debug ctest guards (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, include budget among
  them), `check-master-plan.py`, `check-repository.py`; `imgui.ini` removed from the root.
- Both harness trees rebuilt at the final commit and current by the repaired `tree_current`.

## Not done here, and what only the hosted run can prove

- The user's elevated sampling run (`scripts\elevated-captures.bat sampling`): it re-records the jobs rows on the
  repaired pool. Expected: jobs off over compiled-out loses its `FiberPool::acquire` gap and jobs full reads its own
  cost at every shape.
- Hosted: every lane builds the new cases and passes them, `crd-jobs-schedcheck-tests` included; linux-clang-tsan with
  the streaming case decides the TSan finding above; macOS and other CPUs are not measured here.
