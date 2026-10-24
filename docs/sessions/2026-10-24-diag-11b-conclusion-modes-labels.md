# DIAG.11b (9): diagnostic modes and specimen counts in the conclusion artifact, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b), [CI tiers](../design/ci-tiers.md),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [controlled-interleaving twin](2026-10-24-diag-11b-schedcheck-ctests.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job without a fix on main. The completed runs
after 40052612807 (failed, repaired by `9583f26b`) were cancelled with no failed job; 40067359705 (head `9ce438da`)
was running with 13 jobs green and none failed.

## Change

Clause (9) asks for conclusion artifacts for diagnostic specimens and mode activation. A lane's `conclusion.json`
recorded `CRD_ENABLE_PROFILING` but no sanitizer, and its label histogram could not count the contract-fatal modes,
which had no label; so a sanitizer lane that ran no specimen read the same as one that ran all of them.

- `scripts/ci-evidence.py`: `MODES` lists the CMake switches that change what a lane's binaries are (ASan, UBSan,
  TSan, libFuzzer, coverage, asserts, profiling, clang-tidy); they join the recorded cache keys and `modes` names
  those that are on. `junit_summary` also counts every executed test's outcome per label (`results.labels`), from
  the inventory's labels without the `cerid.` ownership annotations. The job summary gains `Diagnostic modes` and
  `By label` rows. No new script: the bundle already read the cache, the inventory and the JUnit file.
- `cmake/CrdDiag.cmake`: `crd_diag_fatal_tests` labels every mode `diag;diag-fatal` (80 CTests on win-debug).
- `tests/foundation/jobs/CMakeLists.txt`: the schedcheck cases are labelled `diag;diag-race` (eight).
- dev.py ownership needed no change: on win-debug, `dev.py plan --path .../containers/array.hpp` lists
  `crd-containers-fatal-specimen` among the build targets, and `select_tests` with that target selects its nine
  `crd-containers-fatal-*` CTests by their command's artifact.

On win-debug after reconfiguring: labels `diag` 106, `diag-fatal` 80, `diag-overhead` 18, `diag-race` 8, `fuzz` 13.
`ctest -L "diag-fatal|diag-race"` (2.0 s) and a local bundle give `Diagnostic modes | asserts, profiling` and
`By label | diag 88 executed: 88 passed ...; diag-fatal 80 ...; diag-race 8 ...`.

Teeth (`test-repository-tools.py CiEvidence`, whose fixture cache now has `CRD_ENABLE_ASAN=ON` and `TSAN=OFF` and
whose inventory has a `cerid.` label): with the modes left out of the cache keys the test fails (`[] != ['ASan']`);
with the labels not passed to `junit_summary` it fails (`{} != {'gpu': ...}`); restored, it passes.

## Verification

win-debug: reconfigured; the labelled CTests and the ctest guards (`^crd-no-|^crd-check`). The tooling tests,
`check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`. No C++ changed.

Hosted once pushed: every lane's job summary has a `Diagnostic modes` row naming its sanitizers (win-asan and
linux-gcc-asan ASan, linux-gcc-asan UBSan, linux-clang-tsan TSan) and a `By label` row whose `diag-fatal` and
`diag-race` counts are nonzero and all passed on every lane that builds the tests.

## Not done in this batch

- The rest of (9): the hosted sanitizer, corpus and negative-control audit with the cost of the sanitizer and
  device-validation modes (the runbooks' Cost rows that name the clause), and the fuzz and soak cadence. The
  detector cases of crd-perf-tests share one program with other cases, so they count only in the lane's total.
- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; this run's only user message approves none.
  (8) waits for the user's elevated recording.
- In passing: the leak runbook's Cost row still said the statistics gate was over 1%; it now gives the
  microbenchmark as the worst case and the realistic frame's 0.18%.
