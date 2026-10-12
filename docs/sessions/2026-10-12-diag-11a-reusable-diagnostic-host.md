# DIAG.11a the reusable diagnostic host: one lifecycle for the app, headless and sandbox hosts, 2026-10-12

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11a](../ROADMAP.md#slice-diag.11a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c queue depth and native owners](2026-10-12-diag-10c-queue-depth-and-native-owners.md).

## Goal

DIAG.11a's first slice: integrate lifecycle-managed diagnostics into the existing application, headless and sandbox
hosts through optional public composition, with an authorable policy that chooses the supported modes before device
and tool initialization and says when a changed policy needs a restart. DG17 named the gap: `Application` installed
the crash handler, and the sandbox wired its flight recorder, triggers, bundle writer and fault injector in a
sandbox-only `FlightHost`. Committed locally, not pushed.

## What landed

| Piece | Where | What it does |
| --- | --- | --- |
| Diagnostic host | `engine/foundation/perf/include/crd/perf/diagnostic_host.hpp`, `src/diagnostic_host.cpp` | the sandbox's DIAG.9b flight host moved into crd-perf and widened: crash handler, flight recorder and triggers, bundle writer, fault injector with the I/O and GPU seams' hooks, flight.* and fault.* commands; modes decided before anything is installed; `apply()` for a changed profile |
| Host profile asset | `diag_assets.cpp` (`parse_diagnostic_host_profile`, `load_diagnostic_host_profile`, `diagnostic_host_profile_to_asset`, `validate_diagnostic_host_profile`) | kind `diagnostic-host`, schema 1, every key optional and refused by name when unknown or wrong |
| Committed profiles | `assets/diag/host_default.host.toml` (= `DiagnosticHostProfile{}`), `assets/diag/host_investigate.host.toml` | the local, bounded default; an investigation profile with injection and a record/inject grant |
| Path probes | `bundle.hpp` (`bundle_directory_exists`, `bundle_path_exists`) | the writer's existence checks without crd-platform, so the host stays in crd-perf |
| Application | `crd/app/application.hpp` (`ApplicationDesc::diagnostics`), `application.cpp` | a started host owns the crash handler; each `tick()` is one of its frames; `run()` gives the pool its watchdog window; an unstarted host makes the Application invalid. crd-app now links crd-perf |
| Sandbox | `sandbox/src/main.cpp` (`--diag-host`); `sandbox/src/flight_host.*` deleted | starts the engine host from the profile (CLI overrides held to the same rules) and hands it to its Application; the frame feed moved into `Application::tick` |
| Headless host | `tools/ceridc/src/main.cpp` (`ceridc mcp --diag-host <asset>`) | the host starts before the pool and the service, `poll()` after every request; without the flag nothing changes |
| Deliberate-failure application | `tests/support/diag/specimens/diag_host_specimen.cpp` (`crd-diag-host-specimen`) | starts the host from a profile file and fails a fatal invariant or a headless assertion; nothing wired by hand |

Decisions:
- The host lives in crd-perf (it needs the recorder, the injector, the commands and the bundle writer, which are all
  there); the two file checks it took from crd-platform became bundle helpers, so crd-perf gains no dependency. crd-app
  depends on crd-perf (no cycle: crd-perf depends on neither crd-app nor crd-platform).
- `"on"` versus `"required"` is the profile's way to say what a build without a mode should do: continue and report
  Unavailable, or refuse. A shipping build compiles the fault seams out, so `host_investigate.host.toml` says `"on"`
  and runs there with injection Unavailable; `"required"` would refuse.
- Fault injection is served only while it runs: on a build without the seams fault.* is not registered and the
  doctor reports the injector "compiled out" (DIAG.9b's flight host served fault.* everywhere and let every arm answer
  Unavailable). The moved shipping-branch assertions check UnknownCommand instead.
- Live versus restart: the capture directory, the bundle bound and the flight policy change on a running host; the
  crash handler and its directory, the modes, the grant, `allow_remote` and the watchdog window are chosen at start
  (the grant is fixed when the service is built, the window when the pool starts, the crash handler before devices).
  `apply()` checks everything that can refuse before changing anything, so a refused or busy candidate leaves the host
  exactly as it was.
- The writer is always the recorder's forward sink and returns at once without a capture directory, so a directory
  applied live takes effect for the next capture.

## Tests

| Program | Cases | What they hold the code to |
| --- | --- | --- |
| crd-perf-tests | 15 `[diag-host]` (7 moved from crd-sandbox-inspect-tests, now `[diag9b][diag11a]`; 8 new `[diag11a]`) | the moved DIAG.9b cases against the engine host started from a profile (hitch bundle naming this build and executable, retention, wrapped-allocator failures, the I/O hook, the watchdog window, the recipe at its bounds, refused starts and stop); the committed default equals `DiagnosticHostProfile{}` and every key round-trips (a path with a quote, a backslash and a tab included); 23 refusals each named, plus the header, the file and in-code rules; each of the ten fields classified alone, a live and a restart field together; modes with the build's capabilities modelled (everything off, "on" without seams or crash backend, "required" refused with nothing installed); the late start; `apply()` (NotStarted, Unchanged, RestartRequired naming only the restart fields, three refusals, a live directory and bound, an edited policy file at the same path reloading, Busy while a capture is pending); the crash handler installed into its directory, and an unusable directory continuing under "on" and refusing under "required"; the deliberate-failure application in a child process for both modes |
| crd-app-tests | 3 `[diag11a]` | one frame per `tick()`; `run()` gives the pool the watchdog (one more live thread in the lifetime ledger than without a host); an unstarted host makes the Application invalid. They need a window, so a headless run skips them |
| crd-ceridc-tests | 1 `[diag11a]` | the real binary started with an authored profile: flight.trigger under the profile's record grant, the doctor reporting the recorder, the crash directory created, the pending capture written at stop and read back through `bundle.inspect` (flight-record section, `same_as_host`, the ceridc module); a refused profile and a remote grant override refuse the start |

## Teeth

Fourteen deliberate breaks on win-debug, each applied with a binary replace, restored from a byte-exact copy with its
mtime bumped and rebuilt; every one failed a case.

| Break | Failed |
| --- | --- |
| the capture directory left out of the live fields | the classification and apply cases |
| `apply()` ignores restart fields | the apply case |
| `apply()` reloads only when the policy path changes | the apply case (the edited file at the same path) |
| the remote grant allowed without `allow_remote` | the refusal case |
| no late-start check | the late-start case |
| a required fault injection not refused | the modes case |
| the crash handler not installed | the crash case and the deliberate-failure case (no crash directory) |
| retention ignores the profile's bound | the apply case (two bundles kept) |
| fault.* served while injection is unavailable | the modes case |
| the canonical document drops `allow_remote` | the round-trip case |
| `Application::tick` feeds no frame | the two frame-counting app cases |
| `Application::run` skips the watchdog window | the run case (one live thread, not two) |
| `Application` accepts an unstarted host | the refusal case; the process then stopped on the debug assertion in `pump_main_thread_once` (the case went on to tick without a pool) and was killed; the case now uses `REQUIRE_FALSE` so a valid Application ends it there |
| ceridc ignores `--diag-host` | the ceridc case |

## Verification

Every lane ran the final source; "whole" means the program's every case.

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | whole tree (`all`), then the touched targets again after the tidy and wrap edits | crd-perf-tests whole (285 cases, `[diag-host]` five more times), crd-app-tests (10), crd-ceridc-tests (22, from a scratch directory with `CRD_CERIDC_EXE`), crd-sandbox-inspect-tests (24); the new cases also through CTest (29 matching tests); the sandbox smoke-ran on Vulkan with the default profile (crash handler and recorder active, a startup hitch captured) and with `host_investigate.host.toml --diag diag.capabilities` (injection active, fault.* served, grant read,record,inject) |
| win-shipping | touched targets | crd-perf-tests (149, the fifteen host cases on their shipping branches), crd-app-tests (10), crd-ceridc-tests (22), crd-sandbox-inspect-tests (24) |
| win-clang-cl-shipping | touched targets, each linked first time | the same |
| win-asan (inside vcvars) | touched targets | the same as win-debug, `[diag-host]` three more times; the only ASan reports are the heap-overflow, use-after-free and crash-capture specimens' deliberate faults |
| linux-gcc-debug | whole tree | crd-perf-tests (283), crd-app-tests (7 passed, the 3 window cases skipped under `CRD_PLATFORM_HEADLESS=1`, then all 3 passed with the WSLg display), crd-ceridc-tests (22), crd-sandbox-inspect-tests (23) |
| linux-gcc-asan | touched targets | the same programs; `[diag-host]` and the ceridc `[diag11a]` case three more times each; the only reports are the heap-overflow, use-after-free, use-after-return and leak specimens' |
| linux-clang-tsan (`setarch -R`, the preset's TSAN_OPTIONS) | touched targets | the same programs; `[diag-host]` and the ceridc case ten more times each; no TSan report |

Coverage (linux-clang-coverage, one run on the final source): the registry gained the outcome sets DiagHostStart
(8 of 9; RecorderRefused is owned as a resource failure: the recorder can refuse a validated policy only by failing
to allocate its ring), DiagHostApply (6 of 6), DiagHostChangeKind (2 of 3; Unchanged is the field's default value)
and DiagModeState (3 of 3), and the crd-perf scope group (whose `src/*.cpp` glob already took in
`diagnostic_host.cpp`) is renamed and owned by DIAG.11a as well. All 84 scope files were instrumented and every
outcome set holds its previous count; verdict PASS. `diagnostic_host.cpp` ran 264 of 310 regions, 221 of 296
branches and 31 of 32 functions. Totals: regions 11380/12699, branches 7653/9626, functions 1267/1317, lines
20851/23755. The 1.7 GB of profiles are deleted.

Checks: strict tidy (LLVM 20.1.8) clean on all 13 changed C++ files (two findings fixed: `DiagHostField` narrowed to a
u16 base, and three expected-reason literals written as raw strings); clang-format differs on the added lines only in
the repository's hand alignment, case-label indentation and include grouping, and the added lines it found over 120
columns were rewrapped; the Allman check passes; the 13 guard and repository CTests (the eight guards,
`crd-diag-coverage`, `crd-diag-seeded-faults`, `crd-build-profiles`, `crd-master-plan`, `crd-repository-hygiene`),
`diag-coverage.py --validate`, `test-diag-coverage.py`, `test-repository-tools.py` (66, three skips that need CMake
and Ninja on PATH), check-master-plan and check-repository pass. The full win-debug CTest was not run. No script or
workflow changed.

## What the hosted run must show

crd-perf-tests green with the fifteen `[diag-host]` cases on all six lanes (on the shipping lanes the injection
branches answer UnknownCommand and the recipe case checks that injection is unavailable), including the
deliberate-failure application in both modes and the in-process crash-handler case under linux-gcc-asan and
linux-clang-tsan; crd-ceridc-tests with its `[diag11a]` case on all six lanes; crd-app-tests building on every lane
(its three `[diag11a]` cases skip under `CRD_PLATFORM_HEADLESS=1`, so the window path is local evidence only);
crd-sandbox building on every lane; and `crd-diag-coverage` green with the DiagHostStart, DiagHostApply,
DiagHostChangeKind and DiagModeState outcome sets.

## Remaining in DIAG.11a

- The installed external consumer: only `Cerid::core` is packaged; the host's family (crd-perf and what it links) must
  be exported, its symbols and the diagnostic assets installed intentionally, and a thin consumer outside the tree must
  build against the installed prefix on a clean machine.
- That consumer runs, fails on purpose and is investigated without engine edits; an investigation route that decodes a
  flight record's trigger and events is needed, because `bundle.inspect` lists the section without decoding it.
- Examples for allocator selection, jobs, reload and capture on that consumer.
- The three binaries (profiling disabled, optimized profiling, diagnostic) shown for it.

## Follow-up

- The hosted lanes run with `CRD_PLATFORM_HEADLESS=1`, so the three Application cases skip there and the window path
  is local evidence (win-debug, win-asan, the shipping lanes and WSLg). A hosted step under a virtual display would
  carry them.
- The first measured frame of every sandbox run includes start-up (shader compilation, the first upload), so each run
  captures a frame hitch at start. The frame watch could ignore the first frames after start, or the host could start
  the frame clock after the first presented frame.
- `DiagnosticHost::watch_allocator` makes one decorator pair per host object; after `stop()` and a second `start()`
  it returns null, because an owner may still hold the first pair. A restarted host that needs a new watch would need
  a way to retire the old pair.
- `ceridc diag` (the one-shot verb) does not take `--diag-host`; only `ceridc mcp` hosts it.
- A debug-build assertion inside a test program on Windows still blocks rather than exits (seen again in the
  unstarted-host tooth); the queued item to make test programs' asserts terminate stands.
