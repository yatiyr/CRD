# DIAG.12: one application, faulty then repaired, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [DIAG.11c portable boundary](2026-10-24-diag-11c-portable-boundary.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job that main has not fixed. The scheduled
40075929537 finished with linux-gcc-public-checks (120819184913, repaired by `d96eae30`) and its Required result
failed; every completed push run since was cancelled by the next push with no failed job. 40103197370 (head
`6b214076`) is running and 40106467117 is pending; neither was touched.

DIAG.11c has only hosted evidence left, so it is Needs CI on 40103197370 for its doctor and workflow clauses; its
two portable-boundary cases need a push run at or after `85fee881`.

## Change

DIAG.12 asks for one application that shows each detection class faulty, then repaired. `crd-diag-host-specimen`
(DIAG.11a) already is that application: it starts `DiagnosticHost` from an authored profile and fails on purpose. It
now takes `<mode> [repaired]`, so no new executable, target or source route was needed:

| Mode | Faulty run | Detector | Repaired run |
|---|---|---|---|
| `invariant`, `assert` | a refund one larger than the payment | fatal invariant, headless assertion | the refund equals the payment |
| `use-after-free` | an order written after its release | the `DiagnosticAllocator` quarantine (every lane): event `0x0043` carries the allocation and free PCs, then invariant `0x0712` | no write |
| `wait` | two jobs each wait on the other's counter | the jobs watchdog window the host gives the pool: a `Hang` bundle, kind `WaitCycle`, 2 counters, exit 5 | only one job waits |
| `race` | two jobs add to one plain `int` | ThreadSanitizer (`halt_on_error`); elsewhere the mode announces sanitizer `none` and the harness says InstrumentAbsent | an atomic counter, tally 400000 |

A repaired run prints `CRD_HOST_CLEAN bundles=0` and exits 0; a faulty run never prints it, and an unknown mode or
argument exits 2 (it used to return 0, which a repaired arm would have read as clean).

`test_diag_host.cpp` shares one runner (`run_app`), one fatal-end check and one repaired check: exit 0, verdict Clean,
the clean line, no `flight-*.cdb` and an empty crash directory. Three new `[diag12]` cases cover use after free (both
sites must lie inside the application's module in the bundle's own module list, the trigger names
`diag_host_specimen.cpp`), the wait cycle and the race (judged by `lane_promise().tsan`, never the child's tag). The
existing `[diag-workflow]` case now runs the repaired arm of `invariant` and `assert` too.

## Teeth

win-debug, each break rebuilt, run, restored, rebuilt and rerun (`build/diag12/teeth.py`, not committed):

| Break | Result |
|---|---|
| use-after-free faulty arm loses the write | fails: exit 0, no bundle |
| use-after-free repaired arm keeps the write | fails: not Clean, a bundle written |
| the stale-write event drops the allocation PC | fails: `in_module(stale.value_a)` |
| wait faulty arm: the second job does not wait | fails: exit 6, no bundle |
| wait repaired arm keeps the cycle | fails: not Clean |
| race repaired arm runs one job | fails: no `CRD_HOST_TALLY 400000` |
| race announces `tsan` outside ThreadSanitizer | fails: Unexpected, not InstrumentAbsent |
| balance repaired arm keeps the extra refund | fails: not Clean, a bundle written |

linux-clang-tsan: the faulty race arm made atomic fails (`verdict == SanitizerCaught` reads Unexpected); restored,
rebuilt, passes.

## Verification

- win-debug: `crd-diag-host-specimen`, `crd-perf-tests`; the 21 `diagnostic host` CTests pass.
- `wsl-lane.sh build`/`test`: the same 21 on linux-gcc-debug and linux-clang-tsan (the race caught), and the four
  application cases on linux-gcc-asan (LeakSanitizer clean on every repaired run).
- `tidy-files.py` on both C++ files clean (it named `misc-redundant-expression` in the first label expression, now an
  `#if`); `clang-format --dry-run`: the added lines' findings are the files' own declaration alignment only; no added
  line over 120 columns; `check-allman-braces.py`; the ctest guards; `check-master-plan.py`; `check-repository.py`.

Hosted once pushed: crd-perf-tests' four application cases pass on every lane; the race case reads SanitizerCaught on
linux-clang-tsan and InstrumentAbsent on every other lane.

## Remaining in DIAG.12

The same application's real crash (dump or signal record), authored-program error (CEIR) and GPU hazard, faulty and
repaired; capture import and inspection, source attribution and privacy confirmed across them; obsolete scaffolding
and duplicated services removed; then the re-read of every child and DG01-DG18 against final source and published CI.

Follow-up for an extra batch: none.
