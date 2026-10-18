# Scheduled runs 39013990584 and 39108972918: the worker snapshot's lost broadcast, 2026-10-18

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.4c](../ROADMAP.md#slice-diag.4c) (the cooperative worker snapshot),
> [DIAG.10b](../ROADMAP.md#slice-diag.10b) (the nonreproduced-failure register). Rules: [AGENTS](../../AGENTS.md).

## Goal

The CI read at the start of the 2026-10-18 build-speed batch found two failed nightly runs, both at `d08436b9`:
[39013990584](https://github.com/yatiyr/CRD/actions/runs/39013990584) (2026-10-17, `windows (win-asan)`) and
[39108972918](https://github.com/yatiyr/CRD/actions/runs/39108972918) (2026-10-18, `linux-gcc (linux-clang-tsan)`).
Each was the only failing job of its run; no registered failure matched either.

## linux-clang-tsan: an idle worker slept through the snapshot request

`worker snapshot: truncation is honest (total exceeds the buffer)` (crd-jobs-tests) saw `responded == 2` of three idle
background workers and `complete == false` after its 200 ms budget; no TSan report. The same failure reproduced
locally on the mirror tree `~/cerid-mirror-build/linux-clang-tsan`: 8 parallel loops of the `[worker-snapshot]` cases,
4 failing runs of 800, each the same two checks.

The cause is in the protocol, not the timing. `request_snapshot()` bumps the generation and releases one semaphore
token per thread with a broadcast wake. The sleep word of `Semaphore` is the token count itself, so a broadcast
reaches the threads already asleep and posts tokens, nothing more. A worker that has found the count at 0
(`try_drain_one()` failed) but has not yet entered the kernel wait is in neither group: the free workers wake, take
every token in their own loop turns (each turn acknowledges and finds no work), and when the late worker then waits
on the word it is 0 again, so it sleeps through the request without reaching its loop-top acknowledgement. The
2026-10-04 repair (`acquire_or_wake()`, [nightly CI repairs](2026-10-04-nightly-ci-repairs.md)) covers a woken worker
that would re-sleep inside `acquire()`; this window is before the sleep.

Repair: `worker_snapshot()` repeats a tokenless broadcast (`Semaphore::wake_sleepers()`, through
`Scheduler::wake_sleepers()` and `WorkerPool::wake_idle_workers()`) on every poll until every polled worker has
acknowledged. A worker that slept after missing the request is in the kernel by the next poll, returns from
`acquire_or_wake()` without a token and acknowledges; a worker inside a job is not at its safe point and still never
answers, so the honest-incomplete result is unchanged. No token is posted per poll, so a stuck worker does not leave
hundreds of spurious loop turns behind. The targeted-wake path already self-heals on its 1 ms backstop and is woken as
well. `semaphore.hpp` now states what a broadcast does not promise.

Tests with teeth (`crd-jobs-schedcheck-tests`, the DIAG.4b controlled-interleaving program): the semaphore gains an
opt-in sleep point `sem.pre_sleep` between the empty check and the sleep (`CRD_JOBS_SCHED_SLEEP_POINT`, off unless a
test enables it, so the counter-path traces of the recorder and driver cases keep their tags), and a gate-only break
switch `sched_check_break_snapshot_rewake`. Two cases force the interleaving: the first worker to reach the point is
held there, the two others are let reach their kernel wait, the snapshot is requested, and the held worker is let go
only when a free worker finds the count at 0 again (so the count stays 0). Repaired, the snapshot hears from all three;
with the break switch it hears from two and is incomplete, the hosted failure, deterministically. Manual tooth: with
the repeated wake removed from `worker_snapshot()` the repaired case fails `2 == 3` on win-debug; restored and rebuilt,
it passes.

| Evidence | Result |
|---|---|
| linux-clang-tsan (mirror), `[worker-snapshot]` cases of crd-jobs-tests, 8 parallel loops, before | 4 of 800 runs failed (`responded 2 == 3`, `complete`) |
| the same after the repair | 0 of 800, then 0 of 1,600 |
| crd-jobs-schedcheck-tests under TSan (linux-clang-tsan), all cases, then the two new cases 30 times | 8 cases pass; 0 of 30 failed; no TSan report |
| crd-jobs-tests, every case, linux-clang-tsan and linux-gcc-debug | pass |
| crd-jobs-schedcheck-tests, linux-gcc-debug: all cases, then the new cases 30 times | pass; 0 of 30 |
| win-debug: crd-jobs-tests (192 cases), crd-jobs-schedcheck-tests (8 cases), the new cases 40 times | pass; 0 of 40 |
| strict tidy (LLVM 20.1.8) on the 8 changed C++ files; Allman check | clean; PASS |

The probabilistic hosted guard is the existing crd-jobs-tests case on every lane; the controlled-interleaving program
is not a CTest (DIAG.4b: built and run explicitly, and by the DIAG.10b coverage measurement).

## win-asan: the allocator corpus died in `RtlCreateHeap` at process start

`crd-fuzz-memory-allocators-corpus` failed with an AddressSanitizer `use-after-poison` inside `RtlCreateHeap`
(`asan_malloc_win.cpp:1941`), reached from `msvcrt.dll` under the loader during `__scrt_common_main_seh`, before any
corpus input ran; all other failures of that job were registered. It is the signature registered on 2026-10-11 from a
local run (`win-asan-allocator-corpus-rtlcreateheap`), now seen hosted once; the next nightly run at the same
revision passed the job. The program does not import `msvcrt.dll` (its imports: ntdll, KERNEL32, USER32, MSVCP140D,
the ASan runtime, VCRUNTIME140D, VCRUNTIME140_1D, ucrtbased); a system library loaded behind USER32 initialises it, and
the ASan runtime's heap interception poisons memory that DLL then clears. No Cerid frame or input is involved. The
register entry carries the hosted sighting; it stays `nonreproduced` and is read by no build, test or CI step.

## Verification

Changed-only, per the user's process of 2026-10-18: the touched targets built on win-debug, linux-gcc-debug and
linux-clang-tsan (both from the WSL source mirror), their test programs run as above, the ctest repository guards,
tidy, the Allman check, `check-master-plan.py` and `check-repository.py`; the hosted lanes run everything else.

**Addendum, 2026-10-19.** The allocator corpus failed the same way again in push run 39129062998, and the report
reads differently than above: the crash is in `exit()` after `main` returned (`exe_common.inl` line 295), and the
memory is `f7`, poisoned by user. It was a Cerid defect, repaired in
[task C's note](2026-10-18-diag-11b-build-speed-compile-time.md#the-allocator-corpus-on-win-asan-a-cerid-defect):
the virtual memory arena released poisoned pages, and Windows' runtime keeps an unmapped range's shadow.
