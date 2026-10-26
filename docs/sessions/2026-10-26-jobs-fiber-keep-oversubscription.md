# JOBS-HARD.3: the kept fiber on a host with fewer processors than threads, 2026-10-26

<!-- doc-role: historical -->
> Dated evidence. Live owner: [JOBS-HARD.3](../ROADMAP.md#slice-jobs-hard.3). Contract:
> [ADR-0033](../decisions/0033-jobs-implementation-architecture.md) decision 8. Board:
> [bench](../bench/2026-10-25-jobs-fiber-pool-contention.md#2026-10-26-the-spin-before-the-sleep). Rules:
> [AGENTS](../../AGENTS.md). Previous: [the fiber pool's contention](2026-10-25-jobs-fiber-pool-contention.md).

The complete-tier run 40274818980 (head 1cb54d07) failed one case on the hosted 4-vCPU Windows runner, job
`windows (win-debug)`:

```
6885 - fiber keep: frames of short jobs reach the pool's shared free list only as threads start and stop (Failed)
  CHECK( acquisitions * 8U <= crd::u64{job_count} * frame_count )
  140192 (0x223a0) <= 25600 (0x6400)
  free-list acquisitions 17524 for 25600 jobs
```

The case runs 50 frames of 512 short jobs, each pushed on its own, on a four-thread `WorkerPool` with the main thread
helping through `pump_keeping()`, and expects about one free-list acquisition a thread a frame. The run itself was left
alone; no CI run was started, cancelled or re-run.

## The numbers, read correctly

140,192 is the left side of the check, acquisitions times 8. The run had **17,524 acquisitions for 25,600 jobs, 0.68
a job** (the message line). That is not worse than the design before the kept fiber (one a job), but it is about 70
a thread a frame instead of about one.

## Reproduced here

The case pinned to a few processors (a Python runner sets its own affinity mask, which the test process inherits, so
the exit code is the test's own). Before the fix, at 1cb54d07's code:

| Lane | Processors | Runs failed | Acquisitions for 25,600 jobs |
|---|---|---|---|
| win-debug | 1 (mask 1) | 1 of 6 | 50 to 12,647 |
| win-debug | 2 (mask 3) | 6 of 6 | 16,221 to 24,932 |
| win-debug | 4 (mask F) | 2 of 6 | 53 to 7,273 |
| win-debug | 8 (mask FF) | 0 of 4 | 53 to 275 |
| linux-gcc-debug | 1 (`taskset -c 0`) | 4 of 4 | 6,262 to 8,512 |
| linux-gcc-debug | 2 (`0-1`) | 0 of 4 | 53 to 1,646 |
| linux-gcc-debug | 4 and 8 | 0 of 8 | 53 to 70 |

So the hosted failure is reproduced here with numbers like the hosted ones: about 0.9 acquisitions a job at two
processors on Windows.

## Cause: the hypothesis was refuted; the threads really did sleep, once a job

The task's hypothesis was that the "give the fiber back just before blocking" hook fired on paths that do not block,
or on every spin or yield. A temporary tally (since removed) counted every release and acquisition by path, every
call of the hook, whether the count was still 0 after the hook, how long the following kernel wait took and whether
the wake brought a token. At two processors on win-debug, one typical run:

| Hook calls | Count non-zero after the hook | Kernel waits | Waits over 20 us | Waits under 3 us | Woke with a token | Acquisitions (main, workers) |
|---:|---:|---:|---:|---:|---:|---|
| 25,090 | 2 | 25,087 | 1,476 | 3 | 25,039 | 1, 24,907 |

The hook fired once a kernel wait, only when the drain had found no token, almost never with a token posted between
the hook and the wait, and the waits were real sleeps of 3 to 20 us that ended with a token. The main thread released
its fiber 50 times, once a frame, as designed. No path released a fiber without blocking.

What did happen: **the one pushing thread was slower than the workers.** Each `push()` posts one token and one wake.
A worker ran the job, found the queues empty and the count at 0, gave its fiber back, slept a few microseconds until
the next push woke it, took a fiber again and ran that one job. One release and one acquisition a job. In the runs
that passed, the opposite held: the pusher stayed ahead, tokens piled up, and no worker ever slept (0 kernel waits).
That is why the case passed on this 24-core host and failed on the hosted runner, where the pusher's pushes (each a
kernel wake, because a sleeper always existed) are slower. On Linux one processor triggers it: the woken worker
preempts the pusher, runs one job and sleeps again.

The cost is not only the free list. On Windows the pusher paid a kernel wake (`WakeByAddressSingle` with a waiter) for
every job: at two processors the case took 0.13 to 0.19 s, against 0.045 to 0.05 s once workers stopped sleeping
between jobs.

## The fix: a worker that keeps a fiber spins for a token before it sleeps

`Semaphore::spin_for_token(rounds)` looks at the count for up to `rounds` turns, each 16 pause instructions and a yield
of the processor (`SwitchToThread` / `sched_yield`). It only reads the count, so the semaphore's protocol (sleep only
with expected 0, after a drain observed 0; release always wakes) is unchanged, and so is the token protocol above it:
`Scheduler::wait_for_work(..., spin_rounds)` spins first and then runs the same steps as before, which take the token
the spin saw. On the default path the spin comes before the worker announces it is sleeping (a pinned push's broadcast
token shows in the spin, and the pinned-lane look runs after it as before); on the targeted path it comes after the
idle bit is set, so `wake_one_idle` can pick the spinner and post the token it watches. `WorkerPool::worker_loop`
passes `kKeptFiberSpinRounds` (128) only when the worker keeps a fiber; one that keeps none sleeps at once, so an idle
pool spins once after its last job and never again until it runs one (the targeted path's 1 ms backstop wakes do not
spin). A fiber is now given back only when the thread really blocks or leaves, never on a spin that goes on to find
work.

The pressure readings are unchanged: a spinning worker is running and holds its one kept fiber, counted as acquired,
as before; a blocked worker holds none; the sizing bound (parked fibers plus threads) is untouched. The idle-pool cases
(`eventually(in_use == 0)`) pass unchanged.

### Spin variants measured and why this one

win-debug, the case at 50 frames, acquisitions / wall time, five runs each:

| Variant | 1 processor | 2 processors | 4 processors |
|---|---|---|---|
| none (1cb54d07) | 50 / 0.044-0.056 s | 4,708-24,922 / 0.078-0.186 s | 57-23,567 / 0.030-0.173 s |
| 64 turns, pause and yield each | 50-53 / 0.043-0.058 s | 53-60 / 0.043-0.051 s | 53-57 / 0.029-0.034 s |
| 64 turns, pause only | 50-3,124 / 0.044-0.172 s | 177-2,203 / 0.058-0.199 s | 54-120 / 0.029-0.060 s |
| 30 us, pause and yield | 50-54 | 56-69 / 0.044-0.055 s | 53-68 |
| 30 us, pause only | 50-1,632 | 59-4,451 / 0.051-0.213 s | 55-69 |

The yield is essential: without it the spinners keep the pushing thread off the processors it needs. Turn counts swept
on win-debug: 16, 64 and 256 all held 50 to 64 acquisitions at one, two and four processors. On linux-gcc-debug one
processor is the hard case (four runnable threads on one processor; a yield may run another spinner rather than the
pusher): 8 turns 6,416-7,107, 16 turns 3,950-4,485, 64 turns 1,034-1,708, **128 turns 567-831** (25 runs), 256 turns
310-431. A yield only every 16th turn (64 and 128 turns) and stopping after a yield that ran another thread were tried
and did not help Linux one processor. 128 is the count kept: about 4x under the case's bound on Linux at one processor
and 25x or more everywhere else. A spin that finds no token costs about 70 us (win-debug) to 120 us
(linux-gcc-debug) on an idle host, twice the 36 us and 60 us measured at 64 turns (180 us at 256 turns on Linux).

## The case now oversubscribes itself

`fiber keep: with fewer processors than threads a worker keeps its fiber between pushed jobs` runs the same frames with
the process limited to its first one and then its first two processors (`ScopedProcessorLimit`: the process mask on
Windows, the calling thread's set on Linux, which the pool's workers inherit; restored after the pool shuts down), so a
large CI host exercises the slow-pusher shape too. The unpinned case stays as it was.

## Evidence

Acquisitions for 25,600 jobs after the fix (bound 3,200):

| Lane | Shape | Runs failed | Acquisitions |
|---|---|---|---|
| win-debug | 1 processor | 0 of 25 | 50-53 |
| win-debug | 2 processors | 0 of 25 | 53-64 |
| win-debug | 4 processors | 0 of 25 | 53-67 |
| win-debug | 8 processors | 0 of 25 | 53-55 |
| win-debug | 4 processors beside 8 busy loops on the same 4 | 0 of 15 | 50-62 |
| win-debug | the self-pinned case, unpinned process | 0 of 20 | 53-62 (its two-processor run) |
| linux-gcc-debug | 1 processor | 0 of 25 | 567-831 |
| linux-gcc-debug | 2 processors | 0 of 25 | 53-179 |
| linux-gcc-debug | 4, 8 and all processors | 0 of 75 | 53-54 |
| linux-gcc-debug | 4 processors beside 4 busy loops on the same 4 | 0 of 15 | 53-62 |
| linux-gcc-debug | the self-pinned case | 0 of 20 | 582-732 (its one-processor run) |

That is about one acquisition for 480 jobs, a thread a frame, wherever a processor is free for the pusher, and one for
30 to 45 jobs at four threads on one Linux processor.

**Teeth.** `kKeptFiberSpinRounds` set to 0 (the old behaviour), rebuilt: the self-pinned case failed 8 of 8 on
win-debug (17,711 to 25,355 acquisitions) and 8 of 8 on linux-gcc-debug (6,601 to 7,939); the unpinned case still
passed on this host (53), which is why the pinned case was added. Restored and rebuilt.

**Suites.** `crd-jobs-tests` whole: win-debug 205 cases, win-asan inside vcvars 205 cases, linux-gcc-debug 204 cases,
linux-gcc-asan 205 cases, all passed (no sanitizer report); `crd-jobs-schedcheck-tests` 8 cases on win-debug and
linux-gcc-debug. The schedcheck models were not re-run with a changed protocol because the protocol did not change:
the spin is a read of the count before the unchanged steps.

**Board.** win-shipping-profile, the overhead harness's jobs workload, 12 processes of 2000 frames per arm, against the
same harness built at 1cb54d07 (`--before-exe`; SHA-256 prefixes before `9ef907677bd3`, after `12f44c8765bf`):

| Shape | off p50 before → after, frame change, CPU change | basic | full |
|---|---|---|---|
| 4 threads on 5555 | 204.9 → 204.9 us, -1.20% (-2.50% .. +2.40%), -1.35% (-2.89% .. +1.22%) | -0.01%, +0.43% | -1.69%, -1.44% |
| 8 threads on FFFF | 182.1 → 181.0 us, -1.10% (-5.51% .. +6.14%), +1.54% (-2.80% .. +8.83%) | -4.08%, -2.02% | +0.21%, +0.92% |
| 4 threads on 5 (two processors) | 401.1 → 360.0 us, -12.79% (-15.94% .. -5.60%), +17.61% (+9.62% .. +27.01%) | -12.80%, +23.80% | -7.16%, +5.75% |

The speed-up of 2026-10-25 is kept at four and eight threads (the jobs workload wakes once per batch, so its workers
rarely sleep between jobs). With more threads than processors the frame is 13% faster and costs 18% more CPU: the
spinning is that CPU. The host was idle (`build-bench.py host`: 0.3 cores of other load).

**A measured cost.** On linux-gcc-debug at four threads on one processor the case takes 0.089 to 0.100 s instead of
0.051 to 0.070 s: under Linux's fair scheduler a spinning worker is runnable and takes a share of the only processor
from the pusher, and the futex wake is a syscall for every push whether or not a thread sleeps, so not sleeping saves
the pusher nothing there. At two processors it is 0.031 to 0.034 s against 0.027 to 0.037 s, and equal at four and
eight. On Windows the spin is never slower and up to 3.5x faster on this case. The pool sizes itself to the hardware,
so one processor for four threads only happens when other programs take the processors; the trade is kept and
recorded.

## Verification and housekeeping

- `tidy-files.py` clean on the seven changed files; `check-allman-braces.py` PASS.
- win-debug ctest guards, `check-master-plan.py`, `check-repository.py`; repository root swept for `imgui.ini` and
  stray files.
- `crd-diag-overhead` rebuilt on build\win-shipping-profile and build\win-shipping at the final commit;
  `sample-overhead-wpr.py`'s tree check passes on both.

## What only the hosted run can prove

The next complete-tier run: the hosted 4-vCPU Windows runner passes both short-frame cases, and every Linux and
Windows lane builds and passes the new self-pinned case (its affinity calls succeeding under the runner's job
object).
