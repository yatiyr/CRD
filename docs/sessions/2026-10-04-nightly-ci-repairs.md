# Nightly CI repairs: every lane red on `660a085` since 2026-09-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV](../ROADMAP.md#slice-repo.dev); diagnostics rows: [DIAG](../ROADMAP.md#slice-diag).
> Rules: [AGENTS](../../AGENTS.md). Preceding batch: [first complete-tier repairs](2026-09-14-first-complete-tier-run-repairs.md).

## User direction

On return to the project the user asked to carry on serially, with no parallel agents, and to make CI fully green
first. Changes are sent to CI without compiling or testing the whole tree locally; CI is read afterwards and the next
repairs follow. No commit or push by the agent.

## Census

[Run 37191910186](https://github.com/yatiyr/CRD/actions/runs/37191910186), the 2026-10-04 nightly at `660a085`
(complete tier), failed 24 of 25 jobs; only preflight passed. Every nightly since the 2026-09-17 push
([35279523994](https://github.com/yatiyr/CRD/actions/runs/35279523994)) failed on the same revision. The failures
reduce to twelve causes, almost all introduced by the DIAG commits after the last green tidy revision `18651d52`.

| # | Lanes | Symptom | Cause |
|---|---|---|---|
| 1 | both repository jobs, every Windows CTest lane | `test_real_mapping_owns_every_visible_preset`: unowned `linux-clang-tsan` | DIAG.1b added the preset and mapped it `diagnostic`; the test's expected set was not updated |
| 2 | every Windows CTest lane | `crd-build-profiles`: 24 visible configure presets, test expects 23 | same preset |
| 3 | every Windows CTest lane | `crd-no-std-containers-check`: 74 owning STL uses in 13 files | DIAG code and tests written after the 2026-09-14 container ban |
| 4 | `win-release`, `win-shipping`, `win-clang-cl-shipping` | `crd-sandbox` link: `per_thread_ring_capacity`, `copy_thread_samples` unresolved | the `CRD_PERF_ENABLED == 0` stubs kept the old three-argument `copy_thread_samples` and lacked two DIAG.6a functions |
| 5 | `win-clang-cl` | `test_diag_gpu_resolve.cpp`: unused `kInFlight` | dead constant in the fake GPU backend |
| 6 | `win-tidy` | `bundle.cpp:328` int-to-pointer; 18 escaped literals in `capture_export.cpp` | new DIAG.6 sources never passed the strict lane |
| 7 | five Linux GCC lanes | `profiler.cpp`: local `g_state` shadows the global (`-Wshadow`) | two reader functions inside `namespace detail` |
| 8 | `linux-gcc-release`, `linux-gcc-shipping` | `crash_capture_specimen.cpp:194`: ignored `write()` result | GCC's `warn_unused_result` ignores a `(void)` cast |
| 9 | `linux-gcc-public-checks` | `CRD_JOBS_SCHED_CHECK` redefined | the jobs header check linked both `crd-jobs` (=0) and the test-only `crd-jobs-schedcheck` twin (=1) |
| 10 | `win-vs` | `crd-project-sync-check`: "baseline is not ready" | the job configured with bare `cmake --preset`; only the project-sync configure wrapper writes the baseline |
| 11 | `win-shipping-profile` | counter-pool exhaustion specimen exits 97, test wants 42 | with asserts compiled out only `run()`'s always-on fatal fires, and it said "counter pool exhausted" while the specimen keys on "CounterPool exhausted" |
| 12 | `win-debug` (intermittent: failed 2026-09-30 and 2026-10-04 of the nightlies checked) | hang watchdog test: verdict `SuspectedHang` instead of `Progressing` | the readiness wait accepted `executing >= 1` while the ROOT was still running; after it parks and before a worker takes the child nothing executes, and the back-to-back second snapshot can land there |

`win-asan` additionally failed a fifth test, `DX12 cached DXR pipelines mint one Program identity each, retired with
the context` (DIAG.7a). Its report is TP-1 exactly: a READ of size 8 by `memmove` in `d3d10warp.dll`, 0 bytes past a
2,952-byte region `D3D12Core.dll` allocated, reached from `Dx12RasterContext::dxr_pipeline` through
`CreateStateObject`, census `driver=10.0.26100.33438`. The test creates a raytracing state object like the four RT
gates already registered.

History checks: the counter-pool failure and the `win-vs` conflict repeat on every nightly checked
(2026-09-18, 2026-09-30, 2026-10-03, 2026-10-04); the hang watchdog failure is intermittent (absent 2026-09-17,
2026-09-18 and 2026-10-03).

## Repairs

1. `scripts/test-repository-tools.py` and `scripts/test-native-build-profiles.py` expect the third diagnostic preset;
   [ci-tiers](../design/ci-tiers.md) lists it with the other never-hosted presets.
2. Container ban: `jobs.cpp` watchdog buffers and the fixed test buffers use `crd::containers::StaticArray`; the
   scheduler-check traces use `crd::containers::Array<const char*>` (`TagTrace` in the test); the minidump probe takes
   native `const wchar_t*` paths, reads into `Array<unsigned char>` and compares the module basename
   case-insensitively in place; wide paths elsewhere are `std::filesystem::path` or `std::wstring_view`; the Linux
   record parser reads bounded tokens into `crd::containers::String`; the two test mains hold the crash directory in
   a Cerid `String`. No suppression marker was added.
3. `profiler.cpp` compiled-out branch: `copy_thread_samples` has the current signature and clears `*out_contended`
   like the live path, `copy_thread_samples_with_correlation` does the same, and `sample_copy_contended_count` and
   `per_thread_ring_capacity` exist (0, matching "0 when the profiler is inactive"). A script compared every function
   the header declares with the stubs; the remaining names (`push_region`, `pop_region`, `frame_mark`) have inline
   compiled-out forms in the header.
4. Dead `kInFlight` removed (the in-flight delay is the per-submission delay array).
5. `bundle.cpp`: the `_get_osfhandle` cast carries the same `NOLINT(performance-no-int-to-ptr)` as `console_sink.cpp`
   (the API returns the handle as `intptr_t`). `capture_export.cpp`: the 18 literals are raw strings, converted by a
   script that asserted each contained only `\"` escapes and decodes byte-identically.
6. `profiler.cpp`: the two shadowing locals are `state_ptr` and their C4459 pragma blocks are gone; the strict gate
   then reported `guard_` (trailing underscore) in 20 reader functions, renamed `state_guard`.
7. The specimen keeps the `write()` byte count in a named, deliberately unused local (still async-signal-safe), drops
   its redundant `CRD_DIAG_SPECIMEN_ID` define (CMake stamps it on every specimen), and wraps its mode chain in a
   reasoned `bugprone-branch-clone` suppression: several modes reduce to the same plain fault on one platform by design.
8. `cmake/CrdPublicChecks.cmake`: a library kept out of ALL is an instrument-only twin a consumer never links and is
   no longer part of a module's consumer view. `crd-jobs-schedcheck` is the only engine library declared that way.
9. `.github/workflows/ci.yml` `windows-native`: configure through
   `python scripts/project-sync.py configure --build build/win-vs-debug --preset <preset>`, the path the
   [sync contract](../design/project-structure-sync.md) names for writing the baseline.
10. `jobs.cpp`: `run()`'s always-on fatal now reads "crd::jobs::run: CounterPool exhausted", the pool's own wording,
    the way the fiber pool already uses one wording for its assert and its fatal. Shipping still bumps the exhaustion
    tally before the compiled-out assert, so the specimen's oracle (exit 42) holds in every build type.
11. Hang watchdog test: `gated_child` sets `g_child_running` on entry and the readiness wait requires it. Once the child
    runs it spins without parking until the gate opens, so `executing >= 1` is stable across both snapshots.
12. `docs/third-party-defects.md`: the DXR identity test is the fifth `win-asan` TP-1 registration, with this run as
    evidence. The other four registered names are unchanged.

Further strict-gate findings in the touched files, all older than this batch and never reached by the hosted lane
(it stopped at step 273 of 2,185): naming of function-local constants (`kK`, `kN`, `kBatch`, `kParkers`, `kRuns`,
`kRoots`, `kJobs`, `S`/`P`/`Z`/`N`, three `k...Stream` constants) renamed to lower case per CODING within their own
function bodies; `rfind(..., 0) == 0` prefix tests use `starts_with`; one loop is `std::ranges::any_of`.

## Verification

- `test-repository-tools.py`: 51 tests, OK (3 skipped). `test-native-build-profiles.py`: 7 tests, OK.
- `check_no_std_containers.sh`: PASS.
- Strict LLVM 20.1.8 gate against `build/win-debug`: every changed C++ file clean (the Windows-visible code; the Linux
  branches of the specimen and the crash-capture parser are qualified by the GCC lanes).
- A strict-gate sweep over the 286 C++ files changed since `18651d52` (the last green `win-tidy` revision) was
  started to pre-empt the findings the hosted lane has not yet reached, and was stopped by the session's ten-minute
  background limit before it reported. It was not rerun; the hosted `win-tidy` lane is the check for those files.
- Not run locally, by the user's direction: builds and CTest. The hosted lanes are the qualification for this batch.

## Open decisions for the user

- **DIAG.1b and the TSan lane.** The row waits for a hosted `linux-clang-tsan` execution, but the preset is mapped
  `diagnostic` (never hosted), and clang++ on Linux does not build the tree under its `-Werror` set (the REPO.DEV.11
  audit counted 40 sites). Either the maintainer hosts a clang++ TSan lane, which first needs those sites fixed, or
  DIAG.1b closes on a recorded local TSan run. The row cannot reach Done through CI as mapped.
- **DIAG.6c and DIAG.7a closure.** Carried from the previous session's unanswered question: whether a row may flip
  with an environment-blocked sub-unit (6c's elevated WPR captures; 7a's PIX-blocked DX12 pass label and the
  dead-object lifetime caveat), and whether 7a's unproven DX12 program route moves to DIAG.7b.

## Publication

A push runs the change tier only. `win-vs`, `win-relwithdebinfo`, `win-debug-scalar`, `win-shipping-profile`,
`linux-gcc-debug-scalar` and both public-check presets run on the nightly or a manual dispatch, so "fully green" needs
a `workflow_dispatch` (or the next nightly) after the push.
