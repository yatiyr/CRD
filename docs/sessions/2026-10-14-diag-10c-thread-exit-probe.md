# DIAG.10c thread-exit probe race, 2026-10-14

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [complete run 38209499793 repairs](2026-10-12-complete-run-38209499793-repairs.md).

## The failure

The scheduled complete run [38534848594](https://github.com/yatiyr/CRD/actions/runs/38534848594) at `f5f3da3d`
failed `linux-clang-tsan` on one case, and only there:

    diag10c: the asynchronous logger's worker is one thread from init to shutdown, and the OS sees it end
    tests/foundation/log/test_diag_lifetime_log.cpp:218: FAILED:
      CHECK( crd::os_thread_alive(seen.thread).presence == OsPresence::Absent )
    with expansion: 0 == 1

`crd::log::shutdown()` had joined the worker, and the probe still said Present. The same revision passed the
scheduled run of the day before, so the case fails intermittently. The run's other failure, `win-shipping-profile`,
is not a test: its Build step was still running when the job ended at the two-hour mark with no log
(`BlobNotFound`), a lost runner.

## Cause

On Linux `os_thread_alive` reads `/proc/self/task/<tid>/stat` and calls a task ended only when its state is a
zombie or dead. A join does not wait for that state. `pthread_join` returns when the kernel clears the thread's
`CLONE_CHILD_CLEARTID` word, which `do_exit` does in `mm_release`, before `exit_notify` and `release_task` take the
task off the list; in that window the task is still listed and its state still reads `R`. The probe therefore
reported a joined thread as running whenever the check came first. The kernel marks a task `PF_EXITING`
(`include/linux/sched.h`, the flags word, field 9 of the stat line) when it enters `do_exit`, before that word is
cleared, so the flag is set whenever a join has returned and the task is still visible.

## Fix

`read_task_stat` (`engine/foundation/core/src/os_resource_probe.cpp`) also reads the flags word and treats a task
with `PF_EXITING` set as ended; the header states the rule. No assertion changed: the three post-join checks (the
logger case, the probe's own `diag10c` case and the jobs soak's per-worker check) keep asking for Absent at once.

## Verification

- Teeth on `linux-clang-tsan` (WSL): with the flag check disabled, 300 runs of `crd-log-tests "[diag10c]"` failed
  twice, at the same line with the same expansion as the hosted report; with the fix, 300 runs before the teeth and
  300 after the restore (the lane rebuilt) passed. The whole `crd-log-tests` (33 cases), `crd-core-tests
  "[diag10c]"` (41 cases) and 20 runs of `crd-jobs-tests "[diag10c]"` (18 cases) passed under TSan.
- `linux-gcc-debug` and `linux-gcc-asan`: `crd-log-tests`, `crd-core-tests` and `crd-jobs-tests` `[diag10c]` pass,
  and 200 runs of the logger case each pass.
- win-debug, win-shipping and win-clang-cl-shipping: the three suites build and their `[diag10c]` cases pass (5,
  41 and 18 cases); the changed code is Linux-only.
- Strict tidy (LLVM 20.1.8) is clean on the changed source; clang-format differs only in the hand alignment of the
  changed assignments, as in the rest of the file. The Allman check, the `crd-no-*`/`crd-check*` guards,
  `check-master-plan.py` and `check-repository.py` pass.
