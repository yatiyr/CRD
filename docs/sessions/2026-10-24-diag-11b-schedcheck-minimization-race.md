# DIAG.11b (9): the schedule minimization raced its own detector, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [controlled-interleaving twin on every lane](2026-10-24-diag-11b-schedcheck-ctests.md).

## CI read at the start

`gh run list --branch main --limit 15`: every completed run since the last batch was cancelled with no failed job.
Run 40103197370 (head `6b214076`, still running) has one failed job, linux-clang-tsan (120900661061):
`schedcheck: the failing script minimizes to its essential tags` failed `CHECK( stats.flaky == 0U )`, `1 == 0`.

## Cause

The broken variant's detector in `counter_finish_park` reads the slot's `task_id` just after the `fp.finalizing`
yield point. A script that holds `fp.finalizing` until `test.reacquired` makes the re-acquire come first, every run.
A script that does not hold it left the order to timing: a scheduler preempted between the yield point and the read
let the test thread finish the whole reclamation first, so that script found the reclamation on some runs. The
minimizer counted that candidate `flaky`, as designed; the case's claim that every visited subset is deterministic
was false.

Reproduced on the linux-clang-tsan mirror tree with the case pinned to two CPUs beside two pinned busy loops: 34 of
150 runs failed at HEAD. Besides `flaky 1`, some runs judged the empty script a reproduction on all three runs and
minimized to nothing.

## Fix

- `engine/foundation/jobs/src/counter.cpp`: a sixth yield point, `fp.checked`, after the owner-release check. It is
  inside the existing `CRD_JOBS_SCHED_CHECK` block, so the default build is unchanged.
- `tests/foundation/jobs/sched_driver.hpp`: `await_arrived_or_held(passed, held)` waits until a thread has arrived
  at `passed` or is held at `held` with its scripted turn still ahead; `arrived_locked` now counts through the same
  helper.
- `expose_with` waits on it with `fp.checked` and `fp.finalizing` before the re-acquire, and a timeout counts as broken
  choreography. Every script now fixes the order of the check and the re-acquire. Asking the driver for the
  scheduler's state, rather than reading the script, keeps the expected answer out of the choreography.

No oracle changed: K, the run budget and `stats.flaky == 0` stand, and the minimal result is still exactly
`{test.reacquired, fp.finalizing}`.

## Verification

- linux-clang-tsan, same contention loop: 0 of 300 runs failed (HEAD: 34 of 150). The whole program 80 times under the
  same load: no failure. The eight `schedcheck:` CTests pass with no TSan report.
- win-debug: the program built, the eight CTests and the ctest guards pass.
- linux-gcc-debug: the program and crd-jobs built, the eight CTests pass.
- `tidy-files.py` clean on the three files; `clang-format --dry-run` reports nothing on the added lines;
  `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: linux-clang-tsan and every other lane pass the eight `schedcheck:` CTests.
