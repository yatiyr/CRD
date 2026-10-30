# 2026-10-30 — SIMD-DISPATCH batch: the CI orchestrator suites under CTest

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (no row change; an operator item between the row-close
boards). Before: [the SELL driver](2026-10-30-simd-dispatch-sell-driver.md).

## CI first

No completed run has a failed job left to fix: push run 41059057789 (`e0c70766`) failed linux-clang-tsan and
linux-gcc-relwithdebinfo, both fixed on main (`85cf68f5`, `2c4007e2`); the cancelled runs since hold none. Push run
41112483075 (`37ebb440`, in progress, not cancelled) failed the windows-2025-vs2026 repository job: nine
`test-ci-platform.py` errors, each `p.relative_to(self.root)` in `LocalStore.list` meeting the runner's 8.3 temp path
(`RUNNER~1`) against the resolved `runneradmin` one. The merge `cf63cd90` already resolves the root there (HEAD has
no unresolved `relative_to(self.root)`); push run 41118715232 is its hosted check. The `MemoryError` line in that log
is the memory jail test's child, as designed. Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run.
Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER`, `FTL_SERVICE_ACCOUNT` exist; the last two still hold
template placeholders (`PROJECT_ID`, `NUMBER`), so the Firebase lane waits on real values. No secrets: the AMD and Mac
lanes wait on their accounts.

## Registration

`tests/CMakeLists.txt` registers `scripts/test-ci-orchestrator.py` as `crd-ci-orchestrator` and
`scripts/test-ci-orchestrator-e2e.py` as `crd-ci-orchestrator-e2e` after `crd-ci-platform`, with its 300 s timeout
and `PYTHONUTF8=1`. win-debug CTest: 76 and 3 tests pass (18 s each). WSL Python: the 76 contract tests pass.

## Linux end-to-end defect (reported to the orchestrator's owner)

`crd-ci-orchestrator-e2e` fails on Linux: `test_1_webhook_build_runs_end_to_end` ends with the plan job failed.
Its log:

```text
~~~ checkout http://127.0.0.1:39921/tmp/tmp08cdniv2/demo at f96cdbbc... (main)
fatal: repository 'http://127.0.0.1:39921/tmp/tmp08cdniv2/demo/' not found
~~~ agent error: git fetch exited 128
```

Cause: `ci/agent/agent.py` `checkout()` prefixes the coordinator address to any URL starting with `/`, meant for the
mirror's relative `/git/<repo>.git` (`scheduler.py` emits it only for `strategy == 'mirror'`). A `direct` checkout of
a POSIX filesystem repository also starts with `/`. Windows passes only because `C:\...` does not; the mirror test
passes on Linux. Fix: key the prefix on `checkout.get('strategy') == 'mirror'`, as `git_env` already does. `ci/` is
the orchestrator owner's (operator note 2026-10-30 22:10), so it is not edited here; until that fix lands every
Linux lane's CTest and the ubuntu repository job fail this one test.
