# DIAG.12: privacy across every class of the same application, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) ID-8. Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [schedule minimization race](2026-10-24-diag-11b-schedcheck-minimization-race.md).

## CI read at the start

`gh run list --branch main --limit 15`: 40117742234, 40117510776 and 40115109503 completed cancelled with no job.
The only failed job of a completed run is still 40103197370's linux-clang-tsan `schedcheck:` minimization case,
fixed in `32adc973`; that run is in progress (win-relwithdebinfo building, as long as the last green run's) and
40120050759 (`32adc973`) is pending. Neither was touched.

## What ID-8 asks here

Diagnostics stay local and bounded, raw evidence is explicitly restricted, and a raw dump is never declared redacted
because its manifest is. Read per class, the shareable artifact is the bundle; the dump and the Linux record are raw.

| Class | Capture | Runtime-host paths before this batch |
|---|---|---|
| invariant, assert, use-after-free, wait, program, gpu | bundle | none: the live walk keeps file names, the program is registered as `orders.ceir`, the recipe is `<profile> <mode> [repaired]` |
| crash | dump (Windows), record (Linux) | raw: full paths, registers, memory |
| crash, imported | SymbolIndex section | **the dump's full module and PDB paths** |
| race | none (ThreadSanitizer's stderr) | not a capture |

The defect: `parse_minidump_modules` returns the module and PDB paths as the dump records them, and
`serialize_symbol_index` packed them as given, so a section imported from a dump carried the running host's
directories. The record route already kept file names (`parse_crash_record_modules`).

## Change

- `serialize_symbol_index` (engine/foundation/perf/src/symbol_index.cpp) packs `basename_view` of each name and debug
  file. One point covers every route into a section (live walk, dump, record); lookup already keyed on the identity
  and the file name (`debug_file_path`, the importer's safe-name check), so no reader changes.
- The SymbolIndex round-trip case packs a module named `C:\Users\someone\game\app.exe` with `D:/build/out/app.pdb`
  and reads back `app.exe` and `app.pdb`.
- test_diag_host.cpp: `check_private` holds a capture's bytes free of the application's directory and of the case's
  scratch directory (profile, captures, authored program), in UTF-8 or UTF-16LE, either separator, ASCII case folded.
  It runs on every bundle `read_app_bundle` reads, on the invariant and assert bundles, and on the section the crash
  case imports. The crash case also checks the raw capture does hold the application's directory: the positive
  control that keeps the absence checks from passing on a scanner that finds nothing.

No new file or helper outside the case's own file; the raw captures keep their content (classified, not redacted).

## Teeth

Each break was built, run, restored, rebuilt and rerun (`build/diag12-privacy/teeth.py`, not committed).

| Break | win-debug | linux-gcc-debug |
|---|---|---|
| the serializer packs the given path | fails: the round trip, and the crash case's section holds the application's directory (dump) | not run alone |
| as above, and the record parser keeps the `exe` line's path | not run | fails: the round trip and the crash case's section |
| the replay recipe is the profile path (argv) | fails: the invariant/assert, use-after-free, wait, program and GPU cases (scratch directory in the bundle) | not run |
| the scanner does not fold `\` | fails: the dump's positive control | not run |
| control: UTF-16LE only, then UTF-8 only | passes: the dump holds the directory in its module names and in its PDB path | not run |

## Verification

- win-debug: `crd-diag-host-specimen` and `crd-perf-tests` built; 28 `diagnostic host`, `SymbolIndex` and
  `minidump parser` CTests pass; crd-perf-tests `[symbols],[identity],[diag12],[import],[bundle]` 48 cases pass.
- linux-gcc-debug (`wsl-lane.sh build`/`test`): both targets built `-Werror` with no warning; the same 30 CTests pass
  (the two window cases skip headless, as before).
- `tidy-files.py` clean on the four files; `clang-format --dry-run` on the added lines reports only the house
  declaration alignment; `check-allman-braces.py`, the ctest guards, `check-headers.py` on symbol_index.hpp,
  `check-include-budget.py`, `check-master-plan.py` and `check-repository.py` pass.

Hosted once pushed: the six application cases that leave a capture and the SymbolIndex round trip pass on every lane.

## Remaining in DIAG.12

Removal of the remaining obsolete scaffolding and duplicated services; the re-read of every child and DG01-DG18
against final source and published CI.

Follow-up for an extra batch: two findings of this read. `kManifestFlagFieldsRedacted` describes string fields a
manifest does not have and nothing sets it (scaffolding item). The schema paragraph promises each record a redaction
class; flight events carry none (only DIAG.8b's inspected values have one): implement it or amend the contract.
