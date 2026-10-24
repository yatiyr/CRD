# DIAG.12: the same application's authored-program error, faulty then repaired, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [one ELF module walk](2026-10-24-diag-12-one-elf-walk.md).

## CI read at the start

`gh run list --branch main --limit 15`: the runs completed since the previous read (40110779214, 40110939655 and
40112350438, all cancelled by the next push) were read job by job; none has a failed job. 40103197370 (head `6b214076`) is still in progress with seven jobs
running, and 40113440728 (head `419570de`) pending; neither was touched. DIAG.11c (Needs CI) still waits for a
published run that carries its work.

## Change

`crd-diag-host-specimen` has a seventh mode, `program`. No new executable, asset or abstraction:

- The parent case writes the authored program `orders.ceir` beside the profile, as it already writes the profile. The
  faulty program's `core.for` steps by an authored `0`; the repaired one steps by `1` and returns 41.
- The application reads it, registers it under its file name, and runs it on the engine's own route:
  `cook_program_text`, `read_program`, `plan::compile`, `plan::run`. Provenance reaches the run through the cooked
  blob's `ORIG` chunk.
- The detector is the CEIR run (`RunResult::fault`, `bad-for-step`). Attribution is
  `plan::instr_provenance` and `render_provenance`. The application reports the fault as a `Manual` trigger (code
  0x0713) whose source is the authored op's file and line. `stop()` delivers the capture; the bundle is written and
  the run exits 7. No new `FlightTrigger` kind: the recorder already carries an application's own trigger with a
  source.
- crd-perf's module declaration gains `ceir ceir-cook` as `TEST_DEPENDS`, since the specimen it tests links them.
- The two `bundle.inspect` checks share one helper, `inspect_app_bundle`.

The new `[diag12]` case takes its oracle from the text it wrote, never from the parser: the `core.for`'s line and
first column. It checks the trigger's file (`orders.ceir`), line and message (`bad-for-step at orders.ceir:8:9 op#`),
the 40 events before it, the replay recipe, this build's identity and the application's module. It reads the same
site through `bundle.inspect`. The repaired run exits 0 with no bundle and no dump and prints `result=41`.

## Teeth

Each break was built, run, restored, rebuilt and rerun on win-debug (`build/diag12g/teeth.py`, not committed).

| Break in the application | Result |
|---|---|
| the trigger is not fired | the case fails: no bundle |
| the trigger's line is the C++ `__LINE__` | the case fails: the line oracle and `bundle.inspect` |
| the program is cooked with no file name | the case fails: the trigger's file and the message |
| a clean run is reported as a fault | the case fails in the repaired run |

## Verification

- win-debug: `crd-diag-host-specimen` and `crd-perf-tests` built; the 23 `diagnostic host` CTests and the ctest guards
  pass (15).
- linux-gcc-debug (`wsl-lane.sh build`/`test`): both targets built `-Werror`; the 23 `diagnostic host` CTests pass
  (the two window cases skip headless, as before).
- `tidy-files.py` clean on both files. `clang-format --dry-run`: the added lines' findings are the house hand
  alignment only. `check-allman-braces.py`, `check-master-plan.py` and `check-repository.py` pass.

Hosted once pushed: the program case passes on every lane.

## Remaining in DIAG.12

The application's GPU hazard, faulty and repaired; privacy across every class; removal of the remaining obsolete
scaffolding and duplicated services; the re-read of every child and DG01-DG18 against final source and published CI.
