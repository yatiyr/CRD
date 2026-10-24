# DIAG.11b (9): the controlled-interleaving twin on every lane, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [authored rendering workload](2026-10-24-diag-11b-authored-rendering-workload.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40052612807 (head
`90e9a299`) failed 22 jobs; the three logs read again (win-asan, linux-clang-tsan, win-shipping) fail only
`crd-diag-coverage` (`test_committed_registry_describes_the_tree`), the one cause `9583f26b` repairs. The runs after it were cancelled with no
failed job; 40067359705 (head `9ce438da`) was running with six jobs green, 40073367794 pending.

## Change

Clause (9) names the DIAG.4b controlled-interleaving program, `crd-jobs-schedcheck-tests`: no CTest, on no hosted
lane. It now is one.

- `tests/foundation/jobs/CMakeLists.txt`: the program is in the default build and `crd_discover_tests` registers
  its eight cases. The twin library `crd-jobs-schedcheck` and its unoptimized objects stay out of ALL and are built
  only for this program, so the public-header checks still leave the twin out of the consumer view.
- `scripts/diag-coverage.py` and `tests/support/diag/coverage-scope.json`: the program moved from `direct` to
  `programs`. `direct` held only this program, so its runner, its Catch2 JUnit reader and the registry key are gone.
  On linux-gcc-debug, `select_tests` with the program alone selects its eight CTests.
- The race runbook's Next test names `ctest --test-dir build/win-debug -R "^schedcheck: "`, so
  `crd-check-diag-runbooks` (every lane) fails a tree that does not register the cases; Hosted and Cost rows updated.

Teeth (win-debug): with the `crd_discover_tests` line removed and the tree reconfigured, `ctest -N -R "^schedcheck: "`
lists none and `crd-check-diag-runbooks` fails ("no test of this tree matches '^schedcheck: '"); restored and
reconfigured, it lists eight and passes. The broken-variant cases are the negative controls: each passes only when
the oracle finds the broken order.

<a id="cost"></a>
## Cost

Per lane the twin compiles crd-jobs' ten C++ units and its fiber switch again with `CRD_JOBS_SCHED_CHECK=1` (on
MSVC two of them as the `/Od` objects), plus the test unit and two links. The latest `.ninja_log` duration of each of
those edges on this host, sccache on, summed: 7.1 s (win-debug), 11.8 s (linux-gcc-debug), 6.7 s (linux-clang-tsan),
7.5 s (linux-gcc-asan).

The eight cases (`ctest -R "^schedcheck: "`, wall time):

| Lane | Total | Slowest case |
| --- | ---: | --- |
| win-debug | 0.8 s | the broken late-sleeper variant, 0.27 s |
| linux-gcc-debug | 2.6 s | the broken late-sleeper variant, 0.26 s |
| linux-clang-tsan | 4.4 s | delta minimization, 0.40 s |
| linux-gcc-asan | 10.4 s | the broken late-sleeper variant, 0.27 s |

TSan and ASan report nothing. No CTest timeout was needed: the slowest case takes well under a second everywhere.

## Verification

win-debug: the program built and its eight CTests, the ctest guards; linux-gcc-debug, linux-clang-tsan and
linux-gcc-asan through `wsl-lane.sh`: the program built and its eight CTests. `test-diag-coverage.py`,
`diag-coverage.py --validate`, the tooling tests, `check-allman-braces.py`, `check-master-plan.py`,
`check-repository.py`.

Hosted once pushed: every lane builds `crd-jobs-schedcheck-tests` and passes its eight `schedcheck:` CTests (win-asan,
linux-gcc-asan and linux-clang-tsan with no sanitizer report), and `crd-check-diag-runbooks` passes.

## Not done in this batch

- The rest of (9): dev.py/CI ownership and conclusion artifacts for diagnostic specimens and mode activation; the
  hosted sanitizer, corpus and negative-control audit with the cost of the sanitizer and device-validation modes;
  the fuzz and soak cadence.
- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; this run's only user message approves none, so
  each waits for the user's approval in the conversation. (8) waits for the user's elevated recording.
