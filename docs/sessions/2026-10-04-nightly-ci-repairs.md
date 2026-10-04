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
  dead-object lifetime caveat), and how the DX12 program route counts: it is now measured as a warning-severity
  correlation, because no Core error names a pipeline state ([g-6](2026-10-04-diag-7a-dx12-program-route.md)).

## Publication

A push runs the change tier only. `win-vs`, `win-relwithdebinfo`, `win-debug-scalar`, `win-shipping-profile`,
`linux-gcc-debug-scalar` and both public-check presets run on the nightly or a manual dispatch, so "fully green" needs
a `workflow_dispatch` (or the next nightly) after the push.

## Second batch, after run 37219198785

The user pushed the first batch as `bf99a5e` and run
[37219198785](https://github.com/yatiyr/CRD/actions/runs/37219198785) started. Correction to the publication note
above: this push touched CMake and the workflow, so preflight resolved it to the **complete** tier and every lane,
including `win-vs` and both public-check presets, runs on the push itself; no manual dispatch was needed. Pushes do not
cancel each other (`cancel-in-progress` applies to pull requests only).

Early results: both repository-check jobs now **pass** (causes 1 and 2 above repaired). Two lanes failed fast:

- **`win-vs`**: the project-sync configure wrapper now runs, generates the solution, and then refuses
  "Protected repository area: `.cpm-cache/imgui/.../imgui.cpp`". Hosted CI keeps `CPM_SOURCE_CACHE` inside the
  checkout, so the generated `crd-imgui` project lists dependency sources under `.cpm-cache/`, which the sync model
  validated as project paths; locally the same files live under `build/_deps` and were already treated as dependency
  items. Repair: `THIRD_PARTY_ROOTS` (`.cpm-cache`, `external`) in `project_sync/storage.py`; `cmake_model` skips
  sources and CMake inputs there (`outside_project`); `project()` records those items as dependency items without
  edit-path validation. Edits there stay refused. The [sync contract](../design/project-structure-sync.md) states the
  rule. Regression `test_dependency_cache_items_are_recorded_not_project_source` reproduces the hosted error against
  the unpatched parser and passes with the fix; `test-project-sync.py` 60 tests OK.
- **`win-tidy`**: the lane progressed past the perf sources and stopped on older findings:
  `symbol_index.cpp` (two `data()[i]` subscripts, a loop that is `find`) and `diagnostic_allocator.cpp` (a
  constant-like `CRD_DIAG_HAS_EXECINFO` macro, replaced by testing `__has_include(<execinfo.h>)` directly, standard
  since C++17; a misplaced widening cast). Both files clean under the local strict gate. The tidy build step now runs
  `cmake --build ... -- -k 0`: the job still fails on any finding, but one run reports every independent finding
  instead of stopping at the first failing step.

Also in this batch: [DIAG.7a g-6](2026-10-04-diag-7a-dx12-program-route.md) and the
[DIAG.7b census](2026-10-04-diag-7b-census.md). The remaining 21 lanes of run 37219198785 were still running when the
batch was handed over; their results are read before further work.

## Third batch: the first Linux test results

With the repairs in, every Linux GCC lane of run 37219198785 built for the first time since 2026-09-14 and ran its
tests. `linux-gcc-debug` and `linux-gcc-debug-sse2` passed all but three of 6,829 tests; `linux-gcc-asan` stopped at
test discovery. `win-debug-scalar` passed.

- **Worker snapshot on an idle pool saw one of two workers** (`test_diag_worker_snapshot.cpp:47`, `:149`). In the
  default scheduler mode an idle worker slept inside `Semaphore::acquire()`, which re-sleeps internally when it wakes
  to an empty count. `request_snapshot()` releases one token per worker and broadcasts, but the first worker to run
  could drain every token in its own loop turns while the others woke, found nothing and re-slept inside `acquire()`
  without ever reaching the loop-top acknowledgement. Repair: `Semaphore::acquire_or_wake()` sleeps once and returns
  after the first wake, token or not, and `Scheduler::wait_for_work` uses it on the default path, so every woken
  worker revisits its safe point; a wake without a token costs one loop turn. Discriminating proof in WSL
  (`linux-gcc-debug`, Ubuntu 24.04): with only that change reverted the idle-pool test failed 2 of 10 runs; with it,
  20 of 20 passed. Full `crd-jobs-tests` passed on Linux (173 cases, 29,719 assertions) and Windows (174 cases).
- **Fiber unwind crashed** (`test_diag_unwind_qualification.cpp:158`, SIGSEGV). On SysV the initial fiber frame made
  `fiber_switch` return straight into the entry function with `fiber_abort`'s address as its return address, so
  libgcc's unwinder (behind glibc `backtrace()`) took `fiber_abort` for a caller, read the CFI of whatever precedes it
  in memory, computed a frame past the stack top and faulted; Windows' table walk stops at the fiber entry. Any
  backtrace taken on a Linux fiber (crash record, sampler) was exposed. Repair: `crd_fiber_start` in
  `fiber_switch_lin64.S`, the first code of every fiber, carries `.cfi_undefined rip` (the standard end-of-stack
  marker) and clears RBP, then calls the entry function from R12 and the never-return safety net from R13;
  `fiber_init_stack` stores those in the R12/R13 slots and returns into the trampoline. Alignment is unchanged for the
  entry function (RSP % 16 == 8). WSL: the `[unwind]` cases pass (2 cases, 43 assertions), including the fiber walk
  that crashed, and the jobs suite above.
- **ASan died at thread exit** ("Failed to munmap" while listing `crd-stress-tests`). `guard_current_thread_stack()`
  replaced each thread's alternate signal stack with a `thread_local` buffer; AddressSanitizer gives every thread an
  `mmap`ed alternate stack and unmaps whichever one is current at thread exit, so it tried to unmap Cerid's buffer.
  A standalone `-fsanitize=address` repro in WSL reproduces it ("failed to deallocate 0x10000 bytes", exit 1) and the
  keep-existing policy exits cleanly, keeping ASan's 32 KiB stack. Repair: an alternate stack another runtime already
  installed is borrowed, never replaced, and left with its owner on uninstall; only Cerid's own stack is disabled
  again. The [DIAG.5b contract](../design/runtime-diagnostics.md#diag-5b) states it. Catch2's fatal-condition handler
  also installs one while tests run, which the uninstall-restoration test had silently relied on Cerid clobbering; that
  test now records the stack before `install()` and asserts the same stack afterwards (borrowed: kept; none:
  disabled). WSL: `[crash-capture],[crash]` 18 cases and the whole `crd-core-tests` (22 cases) pass.

Also in this batch: [DIAG.7b(b)](2026-10-04-diag-7b-census.md#b-landed-dred-setup-before-device-creation), the DRED
setup before device creation, with the 7a (f-3) activation reasons rewritten without nested conditionals.

### Open: clang-cl unguarded-thread overflow (DIAG.5a)

`win-clang-cl` on run 37219198785 failed one of 7,135 tests: `crash capture: a stack overflow on an UNguarded worker
thread still dumps` exited 0xC0000005 with no dump instead of 0xC00000FD with one dump. The guarded-thread case
passed, and production workers are guarded (`worker_loop` calls `guard_current_thread_stack`). The unguarded case
asserts a measured property, that the OS's default guard slack leaves room for the crash filter's hand-off, which
holds on hosted MSVC and on the workstation's clang-cl. Workstation `win-clang-cl` (clang-cl 20.1.8): the case passed 3
of 3 runs, guarded case passed. The hosted lane uses clang-cl **22.1.3**, which this workstation does not have, so the
failure is not reproduced and no code was changed on a guess. It stays open on DIAG.5a: the next hosted run shows
whether it repeats; a reproduction needs LLVM 22 or a hosted diagnostic run.

## Fourth batch: second-run results and the stack-overflow guarantee

Publication correction: both the third batch (`87f23d4`) and the DIAG.7b commit (`8e17e30`) reached GitHub; a stale
local remote-tracking ref (a silently failing `git fetch`) made them look unpushed, and the user was asked to push
again without need. `git ls-remote origin` is the check to use. Run
[37231001716](https://github.com/yatiyr/CRD/actions/runs/37231001716) covers `8e17e30`.

Run [37220832553](https://github.com/yatiyr/CRD/actions/runs/37220832553) (`519c663`, second batch): **`win-vs` passes**
(the project-sync dependency-tree repair works) and `win-clang-cl` passes. Every Linux lane still fails on the snapshot
and unwind defects the third batch repairs. `win-debug` failed one test, closing the open clang-cl finding above.

### Closed: the unguarded-thread overflow was a false guarantee, now an engine guarantee (DIAG.5a)

`crash capture: a stack overflow on an UNguarded worker thread still dumps` failed on `win-debug` (MSVC) as well:
exit 0xC00000FD, no dump. With the clang-cl shape (0xC0000005) that is one mechanism. With no stack left to enter the
dispatcher, Windows terminates with the original code and the filter never runs; when dispatch starts and faults
partway, the second fault becomes the exit code. The test's comment had recorded the OS default slack (about one
page) as a measured guarantee; two hosted counterexamples refute it, although the workstation passed 30 of 30 (a
different OS build and recursion phase). LLVM 22 was not the cause.

Repair (a lifecycle hook, not a call every thread must remember): `crash.cpp` registers a TLS callback
(`.CRT$XLB`, kept by `/INCLUDE:_tls_used` and `/INCLUDE:kCrdCrashTlsCallback`) that, while crash capture is installed,
calls `SetThreadStackGuarantee` with 64 KiB for every thread the process creates; `install()` enables it and
`uninstall()` disables it. It runs under the loader lock with one atomic load and one kernel32 call. The process-wide
effect and its limits (pre-existing threads untouched; Linux keeps per-thread registration) are in the
[DIAG.5a contract](../design/runtime-diagnostics.md#diag-5a). New test `threads created after install() reserve the stack
guarantee automatically` queries the guarantee (`SetThreadStackGuarantee` with 0): 0 before install, 65,536 on a thread
created while installed, 0 on a thread that already existed, 0 again after uninstall, on MSVC and on clang-cl. The
overflow test is renamed `a stack overflow on a raw thread created after install() dumps`, its comment records the
refutation, and both overflow tests now print the specimen marker (-1: the filter never ran). Workstation: the raw-thread
overflow passed 20 of 20, `[crash],[crash-capture]` 26 cases on MSVC and clang-cl, `crd-core-tests` 30 cases; strict gate
clean on the three files.

### Release builds carry no CodeView identity: the dump probe's oracle was wrong (DIAG.5a)

Run 37219198785's `win-release` (the first Release lane to run tests since the DIAG work) failed
`test_diag_crash_capture.cpp:164`: the dump did not "name the specimen with an RSDS CV record". Reproduced on the
workstation's `win-release`. `dumpbin /headers` shows why: the Debug specimen's PE debug directory has a `cv` entry
(RSDS, pointing at its PDB) and the Release specimen has none, because the `win-release` preset links without
`/DEBUG`. The dump was correct; the oracle assumed every build carries a PDB identity. The probe now reports
`find_dump_module` (named, rsds) separately, and a small PE reader, `image_has_rsds`, reads the binary's own debug
directory; both crash tests assert the module is named and that the dump carries RSDS exactly when the binary does.
`crd-core-tests` passes on `win-release` (22 cases) and `win-debug` (30 cases); strict gate clean.

The new stack-guarantee test also exposed a race in its own first draft: `std::thread` can return before the new
thread runs its `DLL_THREAD_ATTACH` callbacks, so a "pre-existing" thread could attach after `install()` and, correctly,
be guaranteed. It now waits until that thread is demonstrably running before installing: 40 of 40 runs.
