# DIAG.6c script suites in CI, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.6c](../ROADMAP.md#slice-diag.6c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-6c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [Needs CI audit, 2026-10-09](2026-10-06-needs-ci-audit.md#rows-closed-on-6bd1ba59-2026-10-09).

## Goal

The audit of 2026-10-09 returned DIAG.6c to Partial: its C++ cases were green on every hosted lane, but no hosted job
ran the row's three script suites. This session runs them in CI, on both runners, after checking them on the WSL
reference host.

| Suite | Tests | Covers |
|---|---:|---|
| [test-sample-cpu-wpr.py](../../scripts/test-sample-cpu-wpr.py) | 15 | WPR/xperf and perf runbook verdicts, including the 2026-10-07 `-butterfly` export and `--from-etl` |
| [test-counters-capability.py](../../scripts/test-counters-capability.py) | 11 | counter classes: available, multiplexing, permission denied, unavailable, tool missing |
| [test-debugger-recipes.py](../../scripts/test-debugger-recipes.py) | 6 | lldb availability and symbol-resolution verdicts |

All three mock `wpr`, `xperf`, `perf` and `lldb` through `subprocess.run` and the tool lookup, so they need neither the
tools nor elevation.

## Wiring

The repository's other tooling suites (`test-repository-tools.py`, `test-dev-workflow.py`, ...) are wired twice: as a
CTest in [tests/CMakeLists.txt](../../tests/CMakeLists.txt) and as a step of the `repository` job in
[ci.yml](../../.github/workflows/ci.yml). The three suites follow that pattern:

- CTests `crd-diag-cpu-sampling-runbook`, `crd-diag-counters-capability` and `crd-diag-debugger-recipes`, with the
  same `PYTHONUTF8=1` environment and a 30 s timeout (each runs in well under a second);
- steps "CPU sampling runbook verdicts", "Counter capability verdicts" and "Debugger recipe verdicts" after "Affected
  workflow selection contracts", with no `if:`, so both `Repository checks (windows-2025-vs2026)` and
  `Repository checks (ubuntu-24.04)` run them. The `repository` job is a fixed job of every tier.

[scripts/README.md](../../scripts/README.md) now names the runbooks and their tests.

## Linux failure found and fixed

Run on the WSL reference host (Python 3.12.3) before the wiring, `test-sample-cpu-wpr.py` failed one test,
`test_stacks_export_names_an_activity`: the `--stacks` export passed `-process d\crd-spec` instead of
`-process crd-spec`. `analyze_trace` took the process name with `os.path.basename`, which on POSIX does not split on a
backslash. The specimen handed to xperf is always a Windows path, whatever host runs the analysis, so the script now
uses `ntpath.splitext(ntpath.basename(...))`, which splits both separators. The test was not changed; on Windows it
passed before and after. Without this fix the new Ubuntu step would have failed.

## Verification

| Check | Result |
|---|---|
| Three suites directly, Windows (Python 3.14) | 15, 11, 6 tests OK |
| Three suites directly, WSL (Python 3.12.3), before the fix | 1 failure in `test-sample-cpu-wpr.py`, others OK |
| Three suites directly, WSL, after the fix | 15, 11, 6 tests OK |
| win-debug reconfigured with `configure-preset.bat`; `ctest -R` the three names `--no-tests=error` | 3/3 passed |
| WSL `~/cerid-build/linux-gcc-debug` reconfigured; same ctest | 3/3 passed |
| win-debug `ctest -R "^crd-no-\|^crd-check" -j 8` | 8/8 passed |
| Pinned actionlint 1.7.12 (`pins.py fetch tools actionlint`, SHA-256 verified) `-shellcheck= -pyflakes=` on `ci.yml` | clean |
| `check-repository.py`, `check-master-plan.py`, `check-ci-tiers.py`, `test-repository-tools.py` (64), `test-dev-workflow.py` (54) | PASS |

No C++ changed, so no lane builds, tidy or clang-format apply to this batch.

## Row state

DIAG.6c returns to Needs CI. The first hosted run must show the three steps green on both repository runners (15, 11
and 6 tests), and the three `crd-diag-*` CTests passed in each build lane's ctest JUnit (every lane runs the full
ctest). The context pointer moves to the next unfinished row, DIAG.7b.
