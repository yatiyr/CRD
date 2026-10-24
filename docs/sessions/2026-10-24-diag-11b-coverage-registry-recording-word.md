# DIAG.11b: the coverage registry follows the flight recorder's recording word, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [jobs basic below off](2026-10-24-diag-11b-jobs-basic-below-off.md).

## CI read at the start

`gh run list --branch main --limit 15`: the cancelled runs 40056633406 to 40039075298 have no failed job (most have
no job at all). Run 40052612807 (head `90e9a299`) was in progress, but three of its jobs had completed failed: both
`Repository checks` jobs (120754541627, 120754541645) at `test-diag-coverage.py`, and `linux-gcc (linux-gcc-release)`
(120754541616) with `crd-diag-coverage` its only failed CTest. Every lane runs that CTest, so every lane would fail it.

## Cause

`817ae34a` (record flight events without the ring mutex) folded the window state into one recording word. The
registry set "flight recorder window" in `tests/support/diag/coverage-scope.json` still found transition sites with
`\bstate\.(store|exchange|compare_exchange_\w+)\(`, which matches nothing now:

```
flight recorder window: the site pattern finds no transition
flight recorder window: kStateArmed has no producing site in the scope and no no-producer or default-value exclusion
```

(and the same for the other three states). It reached CI only now because the earlier pushes' runs were cancelled
before their repository job started. Locally it was missed because the handover guards (`ctest -R
"^crd-no-|^crd-check"`) leave out the two Python registries that key on engine source text, `crd-diag-coverage` and
`crd-diag-seeded-faults`; `dev.py plan` selects both as unbound guards. The handover lesson now names them.

## Change

- The site pattern is `\bmake_word\(|\bword\.fetch_or\(`. Every move into a state builds the word with `make_word`
  or sets FatalEntered with `fetch_or`, and the scanner needs the state token on the site's line, which a pattern on
  the compare-exchange calls would miss (build_capture's re-arm call spans three lines). It finds exactly the six
  transition units: line 301 (record's fill, Complete), 373 (fatal entry, FatalEntered), 446 (build_capture's re-arm,
  FatalEntered and Armed), 459 (close_window, Complete), 717 (trigger, Triggered); not `make_word`'s definition or
  the reloading bit.
- The design text of the gate (runtime-diagnostics.md, DIAG.10b) named the old stores and a fixed enum count; it
  now names the recording word and every registered enum.

No new file, script or option; no engine code changed.

## Verification

- Teeth: the committed pattern fails `diag-coverage.py --validate` (exit 4, the five lines above, the same in CI);
  the new one passes (84 scope files, 54 outcome sets, 24 exclusions). `test-diag-coverage.py` 30 tests pass.
- The measurement, which no hosted lane runs: `linux-clang-coverage` (WSL, clang 18.1.3) configured from the mirror,
  the eleven programs, `crd-jobs-schedcheck-tests`, `ceridc` and the seven corpus replay targets built, then
  `diag-coverage.py --build-dir`: 1,049 CTests and the schedcheck program pass, 1,155 raw profiles, verdict PASS with
  no gap and no stale exclusion; the flight recorder window has 6 of 6 units run, and every error-path set passes.
  Regions 11,543/12,872, branches 7,766/9,762, functions 1,287/1,337, lines 21,156/24,089 (reported, never a gate).
- `diag-seeded-faults.py --validate` and `test-diag-seeded-faults.py` pass. The repository steps after the coverage
  step, which the failed job never reached (`test-diag-overhead.py`, `test-sample-overhead-wpr.py`,
  `test-project-sync.py`, `test-native-build-profiles.py`, the opgen and capability-matrix checks), pass.
- `ctest -R "^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$"` on win-debug,
  `check-master-plan.py` and `check-repository.py` pass. The first guard run reported `crd-check-compile-commands`
  SEGFAULT; it passed alone at once and in three more runs of the set. The script is pure Python and runs no Cerid
  binary, so it is registered in `tests/support/diag/nonreproduced-failures.json`, not chased.

Hosted once pushed: both repository jobs pass "test-diag-coverage.py" and the steps after it, and `crd-diag-coverage`
passes on every lane.
