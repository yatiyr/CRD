# DIAG.10b twelve owned gaps closed with cases, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10b](../ROADMAP.md#slice-diag.10b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b); route:
> [test instruments](../design/test-instruments.md#diagnostic-coverage-and-the-nonreproduced-failure-register-diag10b).
> Rules: [AGENTS](../../AGENTS.md). Preceding: [coverage and the failure register](2026-10-11-diag-10b-coverage-and-failure-register.md).

## Goal

The first coverage measurement owned 15 real untested error paths as `owned-gap`. This batch closes the 12 that a
test case can reach without a new engine seam, removes their exclusions, and records exactly which seam each of the
other three needs. Committed locally, not pushed. No engine source changed; only tests, one test CMake file and the
coverage registry.

## The cases

Each case asserts the observable answer (the status, the sentence, what ran, what was written), not only that a
line executed. All are tagged `[diag10b]`.

| Gap (owning row) | Case | Route |
| --- | --- | --- |
| `RecordMinimizeStatus::Unrunnable` (DIAG.9c) | crd-ceir-cook-tests, `diag 10b: a minimization that cannot run, ...` | a record whose program bytes are cut in half ("does not load") and one whose entry the program does not define ("does not compile"); zero runs, `out` untouched |
| `RecordMinimizeStatus::NotReproduced` (DIAG.9c) | same case | the failing record with `main(5)` (fails, but at the second switch) and `main(0)` (does not fail); exactly one run each, since the first run already disagrees with K = 2 |
| `RecordMinimizeStatus::Budget` (DIAG.9c) | same case | K = 1 and a run bound of 2: the minimizer stops with Budget, never more runs than the bound, and the record it makes still fails at the first switch with an argument in [10, 1000] and reproduces through `replay.run`; with room to finish the same record is minimal at 10 |
| `RecordMinimizeStatus::Cancelled`, `ExternalReplay::Cancelled` (DIAG.9c, DIAG.9a) | same case | a raised cancel: Cancelled before the program runs; `check_external_replay` called directly is Cancelled with nothing said, and Allowed when the cancel is lowered |
| `HostExecutorStatus::Unavailable`, `BadArgument` (DIAG.9a) | crd-ceir-host-tests, `diag 10b: the host executor refuses ...` | the service checks build, inputs and job split before it calls the executor, so a host calling `host_replay_executor()` directly: a plan record, another build (and Ok with any build), a missing random input, a job split of 257 on replay, and a job split of 0 or 257 or a step budget of 0 on record; each with its sentence and nothing run or written |
| `RecordWrite::Failed` (DIAG.9a) | crd-ceir-cook-tests, `diag 9a: a run inspected on the inspect host records ...` | a record written under an existing file used as a directory, and into a directory that does not exist; the existing file is unchanged and nothing is created |
| `HostLoad::LoadFailed` (DIAG.8b) | crd-ceir-cook-tests, `diag 10b: the inspect host refuses a load of another asset ...` | asset id 0 on the first load (nothing installed), then another asset id after a good load (the installed generation stays and still runs to 36) |
| `ScriptOutcome::NotStarted` (DIAG.8b) | same case | a script on a host with nothing loaded: NotStarted with `NotBound`, no generation, no stops; the report is preset to Finished so the run must set it |
| `ScriptOutcome::Unfinished` (DIAG.8b) | crd-ceir-cook-tests, `diag 8c: program.inspect bounds its stops ...`, new section | `program.inspect` with a 50 ms wait bound and `main(10^12)`: Failed, "the run did not finish within the host's wait bound: timeout", no result item; the command's host cancels and joins, and the next request finishes |
| specimen `Verdict::Timeout` (DIAG.10b) | crd-perf-tests, `diag 10b: a specimen that outlives its bound ...` | the CPU-hotspot specimen (about 2 s of work) with a 200 ms bound: Timeout, `timed_out`, reason "exceeded 200 ms"; the same specimen's quick mode is Clean |

`crd-perf-tests` now depends on `crd-diag-cpu-hotspot-specimen` (`CRD_DIAG_CPU_HOTSPOT_SPECIMEN`).

## Found on the way

A record minimized by a host that registers none of the program's dialects is not Unrunnable: the blob reads with
every op opaque, and `check_external_replay` refuses it as "the record's program needs external results (18 ops,
first func.func ...)". The refusal is conservative and correct (nothing could feed an opaque op), but the sentence
says "needs" where the need is unknown, and names `func.func`. No case asserts it; it is a follow-up.

## Coverage, before and after

On `linux-clang-coverage` (WSL, clang 18.1.3), the touched programs were rebuilt and the whole measurement run once
with the old registry: 850 CTests passed, the schedcheck program passed, 951 raw profiles. The verdict was STALE with
exactly twelve lines, one per closed gap (`HostLoad: LoadFailed ran but is excluded (owned-gap)`, and so on), and no
GAP. With the twelve exclusions removed, the same export evaluates PASS.

| Measure | Before (r3, last batch) | Now |
| --- | --- | --- |
| Regions | 9,173 / 10,356 | 9,210 / 10,356 |
| Branches | 5,925 / 7,634 | 5,967 / 7,634 |
| Functions | 1,022 / 1,071 | 1,022 / 1,071 |
| Lines | 16,994 / 19,662 | 17,107 / 19,662 |

The registry now holds 23 exclusions. Three are `owned-gap`, each needing a seam an engine header must offer:

- `PolicyLoadStatus::RejectedGenerationWrap`: `DiagnosticPolicyStore` cannot start at a later generation, and 2^32 - 1
  loads are too many for a test.
- `FlightPolicyStatus::RejectedGenerationWrap`: the same for `FlightRecorder`, where each reload also allocates a ring.
- The flight recorder's `kStateFatalEntered` transition (the fatal path finds the ring mutex held and delivers the
  trigger alone): no public call holds the ring while it calls out (the sink runs after the unlock), so a case needs a
  seam that holds the ring mutex across `finish(Fatal)`.

## Teeth

Thirteen breaks on win-debug, one producing site each, applied by an exact text replacement, rebuilt, run through
CTest, restored byte for byte (a fresh write, so the restored file recompiles) and rebuilt. Every one failed its case:

| Break | Failed |
| --- | --- |
| Budget reported as Minimal | the minimizer case |
| first-pass NotReproduced reported as Budget | the minimizer case |
| "does not load" reported Refused | the minimizer case |
| "does not compile" reported Refused | the minimizer case |
| "cancelled before the program ran" reported Refused | the minimizer case |
| `check_external_replay` cancel answered Allowed | the minimizer case (the run then started: 1 run, another sentence, and the direct check) |
| a failed record write reported Ok | the inspected-run case, both paths |
| the adapter's wrong-executor refusal as Failed | the adapter case |
| the adapter's bad schedule as Failed | the adapter case |
| a load of another asset reported Rejected | the inspect-host case |
| NotStarted reported Cancelled | the inspect-host case |
| Unfinished reported Cancelled | the `program.inspect` bounds case |
| the harness's Timeout verdict as Unexpected | the harness case |

After the restore the three programs were rebuilt and pass in full on win-debug: crd-ceir-cook-tests 110 cases,
crd-ceir-host-tests 54, crd-perf-tests 267.

## Verification

| Lane | Built | crd-ceir-cook-tests | crd-ceir-host-tests | crd-perf-tests |
| --- | --- | --- | --- | --- |
| win-debug | the three programs | 110 cases | 54 | 267 |
| win-shipping | the three programs | 110 | 54 | 134 (profiling off) |
| win-clang-cl-shipping | the three programs, linked first time | 110 | 54 | 134 |
| win-asan (inside vcvars) | the three programs | 110 | 54 | 267 |
| linux-gcc-debug (WSL) | the three programs | 110 | 54 | 265 |
| linux-gcc-asan (WSL) | the three programs | 110 | 54 | 265 |
| linux-clang-tsan (WSL, `setarch -R`) | the three programs | 110 | 54 | 265 |
| linux-clang-coverage (WSL) | the measured set | 850 CTests in the measurement | | |

- The only sanitizer reports are the deliberate specimens' (heap overflow, use after free and the null access on
  win-asan; heap overflow, use after free, use after return and the leak on linux-gcc-asan), repeated into the test
  log by the harness. No TSan report.
- The last edit, a reworded header comment in `test_host_replay_diag.cpp`, came after the lane runs; win-debug was
  rebuilt and the adapter case rerun after it.
- The new cases (and the two existing cases that gained checks) repeated 10 times on win-debug at `-j 8` and 10 times
  under TSan: no failure.
- Strict tidy is clean on all six changed C++ files. `clang-format --dry-run` restricted to the added lines differs
  only in the repository's hand alignment of consecutive declarations and assignments, and the include grouping it
  always reports. The Allman check passes.
- `test-diag-coverage.py` (29), `test-diag-seeded-faults.py` (32), `test-repository-tools.py` (65), `diag-coverage.py
  --validate` (23 exclusions), the eight `crd-no-*`/`crd-check*` guards and the repository CTests
  (`crd-diag-coverage`, `crd-diag-seeded-faults`, `crd-master-plan`, `crd-repository-tools`,
  `crd-repository-hygiene`, `crd-ci-tiers`) pass on win-debug; `check-master-plan.py` and `check-repository.py` pass.

## What the hosted run must also show

crd-ceir-cook-tests and crd-ceir-host-tests green on all six lanes with their `[diag10b]` cases, and crd-perf-tests
with its `[diag10b]` timeout case (the CPU-hotspot specimen is built with it).

## Follow-up

- The three seams, then their cases: a generation setter for `DiagnosticPolicyStore` and for `FlightRecorder`, and
  a way to hold the flight recorder's ring mutex across `finish(Fatal)`. Each touches a widely included perf header.
- `check_external_replay`'s sentence for a program whose ops are opaque (a host that lacks its dialects) says it
  "needs external results" and names the first opaque op; it should say the need is unknown.
- `Verdict::MissingSymbolizer`, the hosted seeds and the hosted run are unchanged from the previous note.
