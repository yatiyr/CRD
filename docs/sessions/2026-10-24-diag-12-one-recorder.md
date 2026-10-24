# DIAG.12: one recorder, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [allocation and free sites](2026-10-24-diag-12-allocation-sites.md).

## CI read

No completed run has a failed job that main has not fixed. 40125506582, 40126129075, 40127079573 and 40128305793
were cancelled before any job started. 40122902896 (head `a4500838`) is still running; its four failed jobs
(linux-gcc-release, linux-gcc-shipping, linux-gcc-relwithdebinfo, win-clang-cl-shipping) are the two causes
`7bee95e8` and `17176125` fix.

## Audit: scaffolding and duplicated services

Each diagnostic component without a consumer outside its own tests was classed as duplicated (another component
carries its clause) or unwired (its clause has no other carrier).

- **`DiagnosticRecorder` (DIAG.2b): duplicated.** No engine, tool or host code used it, and nothing called
  `capture_emergency`. Its record lived only in process memory, so a crash could not leave it anywhere. Yet the
  doctor reported `emergency-recording` as present and enabled in every build. The flight recorder's header and
  design said a hard crash leaves "the DIAG.2b emergency record", and the crash header told hosts to route the crash
  report there. Every clause has another carrier, named in the DIAG.2b design section: `FlightRecorder` (ring,
  overflow, concurrency, lifecycle, the fatal path with the ring held), the crash capture's record (DIAG.5a/5b; DIAG.5c
  already routes a fatal assert there), and the owners' own registration lifetimes (DG08 in DIAG.6a, the jobs
  observer in DIAG.10c, unloaded modules in DIAG.9b).
- **`FramePacingAnalyzer` (DIAG.6c(i)): unwired.** The header-only analyzer is tested, but no tool, panel or command
  reaches it. Next DIAG.12 item.
- **Specimens:** every `crd_diag_specimen` target has a consuming test, a seeded-fault entry or a runbook script.
- Kept: the debugger, reverse-debugging and WPR recipe scripts (the row keeps external tooling recipes), and
  `measure.hpp` (test support for eight test files).

## Change

- Deleted `crd/perf/diagnostic_recorder.hpp`, `diagnostic_recorder.cpp` and `test_diagnostic_recorder.cpp`.
- `doctor.cpp` no longer reports `emergency-recording`. The `doctor:` case now checks that no such mode is claimed.
- Removed the stale references from the comments in `crash.hpp` and `flight_recorder.hpp`, and from the DIAG.9b
  design text.

## Teeth

The `emergency-recording` row put back in `doctor.cpp` (win-debug) fails `doctor: reports the schema, host tuple and
the diagnostic modes`. With the row removed again, after a rebuild, the case passes.

## Verification

- win-debug (reconfigured for the removed sources): crd-perf-tests builds; `[doctor]` (7 cases), `diag commands:*`
  (12) and `[flight]` (25) pass.
- linux-gcc-debug (`wsl-lane.sh`, reconfigured): crd-perf-tests builds with no `warning:` line; `[doctor]` and
  `diag commands:*` pass.
- Guards: 15 of 15. tidy-files passes the four changed C++ files. clang-format finds no more issues than HEAD in any
  of them. Allman, check-master-plan and check-repository pass.

Hosted once pushed: crd-perf-tests' `doctor:` cases pass on every lane.
