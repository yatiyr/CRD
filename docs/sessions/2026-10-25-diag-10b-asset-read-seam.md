# DIAG.12: the diag asset reader on the I/O fault seam, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [residual limitations](2026-10-25-diag-12-owner-rows.md).

## CI read

No completed run has a failed job. 40136412382, 40136198933, 40134042881 and 40131739936 were cancelled with no failed
job. 40122902896 (head `a4500838`) is still running; its four failed jobs are the causes `7bee95e8` and `17176125`
fixed. 40138646327 (head `ad7d080a`) is pending.

## Change

DIAG.12's re-check left one DIAG.10b item: `DiagAssetRead::Failed` had no test, because `read_diag_asset_file` read
with stdio past the I/O fault seam, so the coverage registry owned it as a `resource-failure` no test could force.

- `read_diag_asset_file` (`engine/foundation/perf/src/diag_assets.cpp`) consults the seam as a `ReadWhole` before it
  opens the file, as crd-platform's `read_file_text` does: Fail, Short or Cancel is `Failed` with nothing delivered,
  and a Delay is slept. The stdio reader stays: crd-perf does not link crd-platform, and the reader keeps its
  Missing/Failed split and its one-byte-past-the-bound refusal.
- Every caller already answers `Failed`: a host profile, a flight policy (also `DiagnosticHost::apply()`'s live
  re-read and `flight.policy`) and a `fault.arm` asset now fail under a `failed-read` plan like any other read.
- The coverage exclusion is deleted; `fault_seam.hpp`'s list of consulting reads and the DIAG.10a fuzz-target note in
  the design name the reader.

No file was added except this note.

## Teeth

The consult's action check made always false (`if (false && ...)`): on win-debug the asset case fails (`Ok`, not
`Failed`) and the `fault.arm` case fails (the arm succeeds and replaces the plan). Restored, rebuilt, both pass.

## Verification

- win-debug: crd-perf-tests builds; the `diag assets:`, `fault commands:`, `flight commands:`, `fault injector:` and
  `doctor:` cases (21) pass.
- linux-gcc-debug (`wsl-lane.sh`): crd-perf-tests builds and the same cases pass.
- `diag-coverage.py --validate`, the ctest guards, tidy-files, Allman, check-master-plan and check-repository pass;
  clang-format reports nothing on the added lines.

Hosted once pushed: the two cases pass on every lane (shipping lanes take the compiled-out branch), and
`crd-diag-coverage`'s registry check passes in the repository job.
