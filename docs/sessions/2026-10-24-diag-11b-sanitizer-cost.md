# DIAG.11b (9): the sanitizer modes' cost, and two fiber annotation leaks it found, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b), [test instruments](../design/test-instruments.md),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [conclusion modes and labels](2026-10-24-diag-11b-conclusion-modes-labels.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job. The runs after 40052612807 (failed,
repaired by `9583f26b`) were cancelled with no failed job; 40067359705 (head `9ce438da`) was running, 13 jobs green
and none failed.

## Change

Clause (9) asks for the measured cost of every mode that is a separate binary. [Board](../bench/2026-10-24-diag-11b-sanitizer-cost.md).

- `scripts/ci-evidence.py compare`: one lane's per-test time over another's from two evidence bundles (median,
  quartiles, compared and lane seconds). It reuses the bundle's JUnit reader (`junit_cases`, `case_outcome`, now
  shared with `junit_summary`); no new script, since the bundles are this script's output.
- The per-workload cost is the existing harness's off arm on each lane's binary; nothing was added for it.

The board's first Linux pass showed two growths no reference lane has, both in `sanitizer_fibers.hpp`'s model of a
completed fiber, whose context `run_job_in_fiber` rebuilds for its next job (the FP environment's reset, DIAG.10c,
depends on that rebuild, so it stays):

- **ASan.** The trampoline was entered with `__sanitizer_finish_switch_fiber(nullptr, ...)`, so ASan mapped a new fake
  stack for every job and the one saved at completion was never restored: linux-gcc-asan's jobs workload grew 41 MB a
  frame (8.7 GB after 200 frames; 116 MB with `detect_stack_use_after_return=0`). The trampoline now restores the
  saved fake stack (`sanitizer_switch_end`, which made `sanitizer_fiber_entered` identical, so it is deleted). Not
  chosen: destroying the fake stack at completion (a null save), which unmaps and unpoisons it on every job. Each
  pooled fiber keeps one fake stack until exit, which ASan cannot free while it is not current; that is bounded by the
  pool.
- **TSan.** The rebuilt trampoline never returns, so each job left its function-entry frame on the fiber's TSan
  shadow stack; every later trace on that fiber was one frame deeper and unique, and the stack depot grew with each
  file a job opened (TSan's `profile_memory` over 6000 io frames: `stacks` rising to 2000 and RSS to 2 GB; 955 MiB
  after 3600 frames). A plain TSan program opening and reading files the same way stays at 20 MiB. `sanitizer_fiber_finished()`
  pops the frame (`__tsan_func_exit`, which the runtime exports and the public header does not declare) just before
  a completed fiber switches away: io stays at 41 MiB at 50, 1200 and 3600 frames.

Tests: `asan: a fiber keeps one fake stack across the jobs it runs` (`test_jobs.cpp`, GCC and Clang ASan: 1000 jobs on
a seven-fiber pool see at most seven fake stacks; it requires one, since a runtime with use-after-return off leaves
nothing to judge) and `tsan: a completed fiber leaves no frame on its shadow stack` (`test_tsan_fiber_model.cpp`,
Linux TSan: 6000 jobs that each open a file grow resident memory by under 8 MiB). Teeth: with the saved fake stack
cleared at entry the ASan case fails (`8 <= 7`); without the pop the TSan case fails (46 MiB); restored, rebuilt, both
pass.

## Hosted audit (complete run 39239067340, head `1e0574f1`)

- Execution: win-asan 7654 passed (5 failed, all registered TP-1), linux-gcc-asan 7466 (1 registered failure),
  linux-clang-tsan 7467; each ran the 13 `fuzz` corpus replays and the 12 `diag` tests then labelled.
- Negative controls (`diag-seeded-faults/report.json`): 5 of 11 seeds detected on win-asan, 6 on linux-gcc-asan and
  linux-clang-tsan, the rest not applicable to the lane; `genmc.json` on linux-clang-tsan: the repaired model clean in
  6 executions, both seeded defects found.
- Fuzz cadence: `host-check.yml` is dispatch-only (the manual cadence); its last run, 38605464765 (2026-10-14),
  succeeded.

## Verification

linux-gcc-asan, linux-clang-tsan, linux-gcc-debug (mirror): `crd-jobs-tests`' 192, 192 and 191 CTests pass;
win-debug and win-asan: `crd-jobs-tests.exe` 192 cases pass. The changed C++ through `tidy-files.py` and
`clang-format --dry-run`; `check-allman-braces.py`; the ctest guards on win-debug; the tooling tests
(`CiEvidence`, teeth on the both-passed filter, the floor and the nearest rank); `check-master-plan.py`,
`check-repository.py`.

Hosted once pushed: the two new cases and `crd-jobs-tests` pass on linux-gcc-asan and linux-clang-tsan with no
sanitizer report; the `crd-diag-overhead-jobs-*` and `-io-*` times there read as the drop, not as a verdict.

## Not done in this batch

- The rest of (9): device validation's cost, and the generation-compare, recording and probe Cost rows of the
  runbooks that name the clause.
- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads this run's only user message does not approve;
  (8) waits for the user's elevated recording.
