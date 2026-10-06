# Session — 2026-10-06 — async logger shutdown lost wakeup

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Owning row: [DIAG.5c](../ROADMAP.md#slice-diag.5c).

## Goal

Fix the hang that failed the complete-tier run
[37427716844](https://github.com/yatiyr/CRD/actions/runs/37427716844) at `41f9962c`. On `linux-clang-tsan`, CTest #1380
(`after shutdown the bridge is uninstalled and a late log does not hang`) ran into the 3600 s timeout. Every other job
passed.

## Reproduction

The WSL reference build `~/cerid-build/linux-clang-tsan` reproduced it at once. Running that single test 150 times with
a 5 s bound hung 65 times. A hung run stayed at 0 % CPU, so it was blocked rather than slow. Running the test under gdb
and interrupting it showed where:
- the main thread was in `std::thread::join()` inside `crd::log::shutdown()` (`logger.cpp`, the worker join);
- the worker was in `queue_cv.wait()` inside `worker_main`, with an empty queue.

The late log named in the test was never reached. The hang was in the `shutdown()` that comes right after `init()`.

## Root cause

`shutdown()` cleared `running` and called `queue_cv.notify_all()` without holding `queue_mutex`. The worker checks
`running` in its wait predicate while it holds that mutex. If the store and the notify both land after the worker has
checked the predicate (queue empty, still running) and before it blocks, the notify is lost. The worker then sleeps
forever, and `join()` never returns. The window is widest just after `init()`, when the worker is only reaching its
first wait. TSan's slower scheduling made it common.

The same review found a second path of the same kind. `dispatch_impl` checked `running` before it took `queue_mutex`,
and did not check again after waiting on a full queue. A log racing shutdown could pass that check, then push after the
worker had already drained and exited. That record could never be drained, so the `flush()` inside `shutdown()` would
wait forever. The 2026-09-15 DIAG.5c note had called this gate reasoned but not exercised.

## Changes

- `engine/foundation/log/src/logger.cpp`: `shutdown()` stores `running = false` under `queue_mutex`, then notifies.
  `running` stays atomic, so other readers still work without the lock.
- `dispatch_impl` checks `running` under `queue_mutex`, right before the push and after any overflow wait. A log that
  loses the race is counted in `dropped_count()`, as the DIAG.5c(d) lifecycle contract says. The earlier check outside
  the lock is removed.
- `tests/foundation/log/test_diag_assert_reentrancy.cpp`, two new cases:
  - `async init followed at once by shutdown never loses the stop wakeup` runs 500 async init/shutdown cycles.
  - `a log racing async shutdown is dropped and never stranded in the queue` runs 200 cycles. Each cycle starts a
    producer after `init()`, calls `shutdown()` while the producer is logging, and joins the producer before the next
    `init()`.
- Both cases run their loop on a helper thread under a 60 s bound. When the bound expires, the test writes one stderr
  line and exits with code 3, so a regression fails in about a minute rather than at the 3600 s CTest timeout. No
  oracle, timeout or registered-failure entry was changed.

## Verification

- Teeth on `linux-clang-tsan`, with each fix reverted separately and the lane rebuilt each time:
  - with the old `shutdown()` store, the init/shutdown case hit the bound and exited 3 in 2 of 2 runs;
  - with the old `dispatch` check, the racing-log case hit the bound and exited 3 in 3 of 3 runs.
  Both fixes were then restored, the file mtime was bumped and the lane was rebuilt.
- After the fix:
  - the original test passed 300 of 300 isolated runs, and later 200 of 200 (before the fix, 65 of 150 hung);
  - the `[reentrancy]` tag passed 40 of 40 runs;
  - `crd-log-tests` passed all 28 cases, both from the binary and through `ctest`.
- `crd-log-tests` on the other lanes:
  - all 28 cases passed on `linux-gcc-debug`, `linux-gcc-asan`, `win-debug` and `win-asan` (run inside vcvars);
  - all 26 cases passed on `win-shipping` and `win-clang-cl-shipping`, where asserts are compiled out;
  - the `win-debug` `[reentrancy]` tag passed 30 of 30 runs.
- Checks:
  - `tidy-files.py` is clean for both files, and `check-allman-braces.py` passes.
  - The added lines are clang-format clean. The pre-existing hand-aligned declarations in both files are unchanged.

## Row state

DIAG.5c stays **Needs CI**. Its hosted evidence must now include a complete-tier run in which `linux-clang-tsan` passes
`crd-log-tests`, including the two new cases.
