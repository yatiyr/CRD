# DIAG.11c: the doctor reports this build's modes, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11c](../ROADMAP.md#slice-diag.11c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11c),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [DIAG.11b checks, recording and probes](2026-10-24-diag-11b-checks-recording-probes-cost.md).

## CI read at the start

`gh run list --branch main --limit 15`: the only failed job on a completed run is still linux-gcc-public-checks
(120819184913) of the scheduled run 40075929537 (`test_diag_queue_pressure.cpp:290`, high water 8 against a count
of 9), repaired by `d96eae30`. Every push run since 40079623101 was cancelled by the next push before its Windows
lanes finished, so today's heads have no hosted evidence yet. 40080021621's four Windows jobs were still running,
within the earlier runs' build and test times. DIAG.11b's (l), (o) and (8) stay the user's: this run's only user
message approves no download.

## Finding

`diag.capabilities` (and `run_doctor`) still served the DIAG.0 census, which DIAG.2b to DIAG.11b had made false:
`emergency-recording` "unsupported; DIAG.5" although `DiagnosticRecorder` keeps that record in every build;
`sanitizer-coverage` and the `sanitizer_runtime` dependency knew only ASan, so a TSan build reported no instrument;
`crash-capture` and `authored-provenance` were claimed on every platform and host; the required modes full profile,
race check and controlled replay (Modes, budgets and evidence of cost) had no row. Its host tuple duplicated the
build identity's code without the compiler version or the configuration.

## Change

- `engine/foundation/perf/src/doctor.cpp`: the tuple and the instruments come from `current_build_identity()` (the
  identity bundles record), so the doctor's own `kBuiltWithAsan` and tuple code are deleted. The tuple is
  `platform/arch/compiler version/configuration`.
- Modes: `memory-check` (ASan) and `race-check` (TSan) replace `sanitizer-coverage`; absent, each names the CMake
  switch of its own build. `full-profile` sits beside `basic-recording`. `emergency-recording` is reported present.
  `crash-capture` follows `diagnostic_host_capabilities().crash_backend` and, absent, names MAC.DIAG and WEB.DIAG (the
  later-platform hook). `gpu-fault` says the GPU device reports validation and faults.
- `controlled-replay` (replay.record and replay.run) and `authored-provenance` (program.provenance) follow the
  commands registered with the service: `DoctorLive::commands`, filled by `diag.capabilities` from its own table
  (the service's mutex is held while a handler runs, so the doctor cannot call `command_at`).
- Unchanged names: `basic-recording`, `fault-injection` and `flight-recorder` (crd-package-consumer holds them).
- No new file, script or switch: UBSan, coverage and libFuzzer are test instruments of the CI lanes, not modes an
  application asks for, and `conclusion.json` already records them per lane.

## Teeth

Each restored and rebuilt, the cases green again:
- `serves` matching any name (win-debug): the upper-module case and the capabilities case fail;
- `diag.capabilities` passing no command names (win-debug): the capabilities case fails (controlled-replay not served);
- the absent `memory-check` text without `CRD_ENABLE_ASAN` (win-debug): the instrument case fails;
- `memory-check` read from the TSan flag (win-asan, inside vcvars): the instrument case fails (`false == true`
  against `lane_promise().asan`).

Recorded, not teeth: `emergency-recording` and `gpu-fault` are constant rows (`DiagnosticRecorder` has no build gate),
and `crash-capture`'s absent branch needs a platform without a crash backend, which no tuple here has (MAC.DIAG,
WEB.DIAG). `race-check` compiled runs only on linux-clang-tsan, hosted.

## Verification

win-debug and win-asan: crd-perf-tests builds; the `doctor:` cases, `diag commands:`, the flight and diag host cases
pass. linux-gcc-debug (`wsl-lane.sh`): the same. `ceridc diag --command diag.capabilities` (win-debug, rebuilt)
reports 23 items on one page, the tuple `WINDOWS/X64/msvc 195136246/debug`, and `controlled-replay` and
`authored-provenance` served (ceridc registers crd-ceir-cook's commands). `tidy-files.py` on the five files (clean), `check-headers.py` on
doctor.hpp, `check-include-budget.py`, the ctest guards, `check-allman-braces.py`, `check-master-plan.py`,
`check-repository.py`. clang-format: the added lines match the file's hand alignment; the one rewrapped call follows
its output.

Hosted once pushed: crd-perf-tests' `doctor:` cases pass on every lane, `memory-check` compiled on win-asan,
linux-gcc-asan and `race-check` on linux-clang-tsan by `lane_promise()`.

## Not done in this batch

DIAG.11c: one complete native diagnostic workflow per required available tuple, recorded with its tuple; the audit
of public schemas and host adapters for x64-only or native-filesystem-only assumptions; the MSVC disabled-mode
instruction count (open qualification).

Follow-up for an extra batch: none.
