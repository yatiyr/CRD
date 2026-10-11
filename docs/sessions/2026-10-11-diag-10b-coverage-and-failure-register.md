# DIAG.10b coverage and the nonreproduced-failure register, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10b](../ROADMAP.md#slice-diag.10b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b); route:
> [test instruments](../design/test-instruments.md#diagnostic-coverage-and-the-nonreproduced-failure-register-diag10b).
> Rules: [AGENTS](../../AGENTS.md). Preceding: [seeded detector failures](2026-10-11-diag-10b-seeded-detector-failures.md).

## Goal

DIAG.10b asks for branch, region, error-path and state-transition coverage of the DIAG scope with the instrumented
denominator explicit, for coverage exclusions and flaky or nonreproduced failures tracked as owned evidence, and
not to chase a percentage while key error paths are missing. This batch builds the measurement, runs it, owns every
gap it found and adds the failure register. Committed locally, not pushed. Seeds on hosted lanes remain.

## The lane

`CRD_ENABLE_COVERAGE` (CMakeLists.txt) adds `-fprofile-instr-generate -fcoverage-mapping` to every C and C++ TU and
`-fprofile-instr-generate` to every link. It needs clang (not MSVC) and refuses ASan, UBSan, TSan and libFuzzer,
whose instrumentation would change what is counted. The preset `linux-clang-coverage` (clang, Debug, asserts and
profiling on, warnings shown) is in the diagnostic tier of `.github/ci-tiers.json`: never hosted, like
`linux-clang-fuzz`. Measured on this host's WSL guest with Ubuntu clang 18.1.3 and its `llvm-profdata`/`llvm-cov`
18 (the hosted TSan lane uses clang 20; this lane is local). MSVC has no source-based coverage, so the denominator
is the Linux compile of each scope file: a Win32-only branch is a skipped region here and is reported as "not
compiled here", never as "did not run".

## The denominator

`tests/support/diag/coverage-scope.json` names 63 files in nine groups, each with its rows: core assertion, crash
and real-time capture (3); fault seams (1); the minimizer header (1); the memory module (15); the job observer,
schedule-check and hang watchdog (3); all of crd-perf's sources (23); CEIR inspection and command decoding with
ceridc's `diag.cpp` and `mcp.cpp` (8); run records and replay (8); the specimen harness (1). Ordinary logging, the
application host (DIAG.11a) and product algorithms are outside the scope. A scope pattern that matches nothing, or
a file in two groups, is a registry error.

## The test set and the run

`scripts/diag-coverage.py --build-dir <tree>` checks `CRD_ENABLE_COVERAGE=ON` in the cache, selects from
`ctest --show-only=json-v1` every test of eight programs (core, memory, jobs, perf, ceir-cook, ceir-host,
sandbox-inspect, ceridc) and seven corpus replays (perf bundle, flight, assets and capture, memory allocators,
ceir-record, ceridc-diag), and runs them once with `ctest -I 0,0,0,<numbers>` (CMake 3.28 in WSL has no
`--tests-from-file`) and `LLVM_PROFILE_FILE=<out>/raw/%p-%m.profraw`. A program that registers no test fails the
run. The JUnit must hold every selected test with status `run`; nothing is rerun. `crd-jobs-schedcheck-tests` is not
CTest-registered (DIAG.4b), so the registry lists it as `direct` and runs it once with Catch2's JUnit reporter,
which must hold cases and no failure. The profiles merge with `--failure-mode=any`, and `llvm-cov export
-skip-functions` reads exactly the scope files from every test program, the 16 specimens and `ceridc`.

Run r3 (the restored tree after the teeth below): 846 CTests passed, the schedcheck program passed, 946 raw
profiles, 33 objects. llvm-cov warned that 1,259 functions have mismatched data (the same names compiled under
different settings in different binaries, for example the jobs library and its schedule-check twin); their counts
come from the binary whose hash matches, and no scope file lost its instrumentation.

| Measure | Ran | Instrumented | Share |
| --- | --- | --- | --- |
| Regions | 9,173 | 10,356 | 88.6% |
| Branches | 5,925 | 7,634 | 77.6% |
| Functions | 1,022 | 1,071 | 95.4% |
| Lines | 16,994 | 19,662 | 86.4% |

All 63 scope files are instrumented. Branch counts are not exactly repeatable: r2 had 5,929 branches, r3 5,925,
with identical regions, functions and lines, from timing-dependent paths (watchdog windows, lock retries). That is
one reason a percentage is not the gate.

| Group | Files | Regions | Branches |
| --- | --- | --- | --- |
| assertion, crash and real-time capture | 3 | 153/312 | 83/226 |
| fault seams | 1 | 27/31 | 12/16 |
| minimization | 1 | 73/85 | 41/52 |
| allocator diagnostics | 15 | 1,438/1,549 | 740/924 |
| job lifetime, interleavings and hangs | 3 | 166/180 | 101/114 |
| crd-perf | 23 | 3,882/4,341 | 2,546/3,226 |
| authored provenance, inspection and command decoding | 8 | 849/937 | 563/716 |
| run records, replay and record minimization | 8 | 2,481/2,763 | 1,767/2,226 |
| specimen harness | 1 | 104/158 | 72/134 |

The crash group's low share is the Linux record path, which runs only inside specimens that die (below).

## Error paths and state transitions

llvm-cov has no notion of an error path, so the definitions are explicit. Error-path coverage is per outcome: for
32 registered result and status enums (bundles, imports, policies, assets, commands, fault plans and arming, flight
policies, triggers and window ends, observer verdicts, crash install and write results, the minimizer, allocator
violations, hang verdicts and kinds, inspection loads and scripts, record errors, writes, external replay,
divergences, input states, host executor and replay statuses, record minimization, device replay and the specimen
verdict), every value must be produced at a site whose innermost coverage region ran. A producing site is an
`Enum::Value` occurrence in the scope that is not a `case` label and not an operand of `==` or `!=`, read with
comments and string contents blanked; the count is the innermost counted segment at that column, so the two arms of
a conditional are told apart. State-transition coverage is per transition site, not per edge pair: for the flight
recorder's window machine (`kStateArmed`, `kStateTriggered`, `kStateComplete`, `kStateFatalEntered`) every state
named after `state.store(`, `exchange(` or `compare_exchange_*(` on a line is a site that must have run. A site keeps
its identity by file, state and line text, so a moved line does not orphan an exclusion.

`--validate` refused the first registry: five values have no producing site in the scope. Four are a field's default
initializer in a header (`ImportReject::None`, `HostLoad::Ok`, `DivergenceKind::None`, `DeviceDivergenceKind::None`),
owned `default-value`. The fifth is a finding: the specimen harness declares `Verdict::MissingSymbolizer` but never
returns it; no case checks for a symbolizer. It is owned `no-producer`.

## The gaps and their owners

The first full run found 30 values or sites that did not run. Each is owned in the registry with a class and reason:

| Class | Count | Values |
| --- | --- | --- |
| owned-gap | 15 | RecordMinimizeStatus Budget, NotReproduced, Unrunnable, Cancelled (DIAG.9c); HostExecutorStatus Unavailable and BadArgument through the host executor's diag adapter, RecordWrite::Failed, ExternalReplay::Cancelled (DIAG.9a); HostLoad::LoadFailed, ScriptOutcome NotStarted and Unfinished (DIAG.8b); the flight recorder's fatal path with the ring mutex held, `kStateFatalEntered` at its only site (DIAG.9b); the harness's own Timeout verdict (DIAG.10b); PolicyLoadStatus and FlightPolicyStatus RejectedGenerationWrap (2^32 - 1 reloads, no seam) |
| dying-child | 4 | WriteResult Ok, OpenFailed, DumpFailed, Suppressed: the Linux crash record is written in the fault handler of a specimen that then dies, so no profile is written |
| windows-only | 5 | InstallResult OkReplacedForeignFilter, PathTooLong; WriteResult NotInstalled, FlushFailed, HandlerTimeout |
| other-configuration | 3 | FaultArmStatus::Unavailable (shipping lanes, asserted there), Verdict::SanitizerCaught (sanitizer lanes, the seeded-fault runner's evidence), InstallResult::Unsupported (other platforms) |
| resource-failure | 3 | FlightPolicyStatus::RejectedOutOfMemory, DiagAssetRead::Failed (stdio read error; the reader bypasses the I/O fault seam), InstallResult::FilterInstallFailed (sigaction failing) |

The 15 owned gaps are real untested error paths. The registry is how they stay visible: closing one makes its
exclusion stale, so the run fails until the exclusion is removed. They are DIAG.10b's next local slice, or each
owning row's.

## Teeth

- Deleting the `RejectedUnknownVersion` check in `test_diagnostics.cpp` (one line, so the policy store never
  produced that value), rebuilding `crd-perf-tests` and rerunning the whole measurement: exit 2,
  `GAP: PolicyLoadStatus (error-path): RejectedUnknownVersion did not run [engine/foundation/perf/src/diagnostics.cpp:105]`
  and nothing else. Restored byte for byte by the same script, rebuilt, rerun: PASS (r3, the numbers above); git
  reports the file unchanged.
- On r3's real export, with a temporary registry: an exclusion of `DiagStatus::Ok`, which ran, exits 3 (STALE); a
  `windows-only` class on the Timeout verdict, whose site is compiled here, exits 3 naming
  `specimen_runner.cpp:531`; removing the crd-perf scope group fails `--validate` with 64 problems (every perf value
  loses its producing site).
- `scripts/test-diag-coverage.py` (29 cases, no build) drives every verdict through a temporary tree and a
  synthetic export, including a skipped region reported as "not compiled here", a transition exclusion matched by
  line text, a scope file the lane did not instrument, a program that registers no CTest, a JUnit that lost or failed
  a test and a build that is not a coverage lane.

## The register

`tests/support/diag/nonreproduced-failures.json` holds six entries from the notes so far, each with first seen, lane,
target, signature, reruns, status, explanation, session and rows: the win-asan `RtlCreateHeap` use-after-poison and
the `REN-40-G1 GATE (DX12)` segfault (nonreproduced), this host's WSL misexecution series (host, not settled: the
user's other-machine run in DIAG.10a), the clang-cl thin-LTO link crash (toolchain), and the ceridc sharing violation
and the device-replay shared record file (explained). The test holds every entry complete, requires its session
note to exist and to carry the first-seen date, and fails if `ci.yml`, a CMake file or any script other than the
test reads the file, so the register cannot select, skip or retry a test.

## Verification

- linux-clang-coverage (WSL): configured, the test set built (814 steps), measurement r2 (GAP before the
  exclusions), the tooth run, r3 (PASS). One heavy job at a time.
- No engine or test C++ changed, so no lane rebuild of engine code was needed. The CMake option defaults OFF and
  the other presets do not set it; win-debug was reconfigured with the preset script and its `crd-diag-coverage`
  CTest, the eight `crd-no-*`/`crd-check*` guards and the repository CTests pass.
- `test-diag-coverage.py` (29), `test-diag-seeded-faults.py`, `test-repository-tools.py` (65), `test-dev-workflow.py`,
  `check-ci-tiers.py`, the pinned actionlint on `ci.yml`, `check-master-plan.py` and `check-repository.py` pass.

## Follow-up

- Close the 15 owned gaps with cases (the record minimizer's four verdicts are the cheapest: a budget of one run, a
  cancelled token, a record edited not to reproduce, a program that does not load).
- `Verdict::MissingSymbolizer` is a declared, never-produced harness verdict: implement the symbolizer check or
  remove the value.
- A generation seam (or a smaller generation type in tests) would make both generation wraps testable.
- The seeds on hosted lanes remain DIAG.10b's last local clause before the hosted run.
