# Diagnostic runbooks: from a symptom to the test that decides it

<!-- doc-role: reference -->
> Contract for DIAG.11b clause (10). Status lives only in [ROADMAP](../ROADMAP.md#slice-diag.11b); the implementation
> contract is [runtime diagnostics](runtime-diagnostics.md) and [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md#id-6-2026-10-19-diag11b--investigation-runbooks-held-to-the-tree);
> the guard is [check-diag-runbooks.py](../../scripts/check-diag-runbooks.py) (`crd-check-diag-runbooks`).

Start from what you see, not from a tool. Each entry below is one table read top to bottom:

| Field | What it holds |
|---|---|
| Symptom | what the failure looks like: the report text, the exit code, the behaviour |
| Command | the first local command, on this host, that turns the symptom into evidence |
| Artifact | what that command (or the failing process) writes, and where |
| Read | the field of the artifact that decides between the explanations |
| Next test | the discriminating test: the faulty control that must fail and its valid twin that must pass |
| Hosted | the lanes whose logs show the same evidence on every push |
| Adopt | the one step that switches the capability on for a new module, test or application |
| Example | a working instance to copy |
| Cost | what the capability costs, measured, or the open clause that owns measuring it |
| Owner | the DIAG children whose contract the entry serves |

Commands run from the checkout root. A tree is `build/<preset>`, configured with `scripts\configure-preset.bat
<preset>` on Windows; the Linux lanes build and test in WSL from the ext4 mirror through `scripts/wsl-lane.sh`
([build performance](build-performance.md#wsl-source-mirror)), called from Windows as `wsl.exe --cd <checkout> -e bash
scripts/wsl-lane.sh ...`. `<angle brackets>` are placeholders. A row named `(Windows)` or `(Linux)` applies on that
host only. A `ceridc` command means the tree's own binary (`build/win-debug/tools/ceridc/ceridc.exe`); its `diag` verb
answers from a service in that ceridc process, so the evidence of a running application (its jobs, allocators, GPU
contexts, flight recorder) is asked from that application's own service: crd-sandbox's diagnostic panel
(`--diag`), or the MCP `diag` tool of `ceridc mcp --diag-host <profile>`. ceridc does not run the profiler, so its own
service answers `memory.allocators` and `capture.*` with `unavailable` and the reason; the sandbox runs it.
`crd-sandbox` and `ceridc mcp --diag-host` write into their working directory (the profile's `crash_dir`, ImGui's
`imgui.ini`): start them from a scratch directory under `build/` with the checkout's paths, never from the checkout
root. `--smoke-test 3` bounds a sandbox run to three seconds, long enough for a `--diag` answer to be logged
(`--headless` exits after one frame, before the panel answers).

<a id="human-only-publication"></a>
## Human-only publication

Nothing in these runbooks publishes. Committing, pushing, opening an issue or a pull request, attaching a bundle to a
release, uploading a dump and enabling remote access are the user's steps, taken after reading what would leave the
machine:

- every artifact here is written locally (a crash directory, a capture directory, a `build/` tree, a CTest log);
- the diagnostic service's `upload` and `remote-enable` authority classes are never granted by a runbook command, and
  an asset or a request cannot grant them ([DIAG.8c](runtime-diagnostics.md#diag-8c));
- a minidump is restricted raw material: it holds process memory and is never declared redacted
  ([DIAG.5d](runtime-diagnostics.md#diag-5d)); a structured bundle summary (`bundle.inspect`) is what a person may
  choose to share once read;
- hosted evidence is read (`gh run view`, `gh api` GET), never written.

The guard refuses a runbook command that commits, pushes, creates a release, gist, issue or pull request, dispatches
a workflow, writes through `gh api`, or grants `upload` or `remote-enable`.

## Which entry

| You see | Entry |
|---|---|
| a change to verify, and no failure yet | [which tests and lanes](#rb-route) |
| a red or cancelled hosted job | [a hosted lane is red](#rb-ci) |
| an access violation, SIGSEGV, stack overflow or fail-fast | [the process crashed](#rb-crash) |
| `CRD_FATAL`, an assertion, an out-of-memory fatal naming an allocator | [a fatal invariant stopped the process](#rb-fatal) |
| no progress, a CTest timeout, a hang, starvation or livelock report | [the process hangs](#rb-hang) |
| a ThreadSanitizer report, a result that changes under load | [a data race or an ordering bug](#rb-race) |
| an AddressSanitizer or UBSan report, a guarded allocator's redzone report | [a bad memory access](#rb-memory) |
| a stale handle, a callback after unsubscribe, a use after reload | [a handle outlives its object](#rb-lifetime) |
| memory that grows, or allocations still live at exit | [memory grows or leaks](#rb-leak) |
| a GPU validation message, a removed or lost device | [a GPU error or a lost device](#rb-gpu) |
| an authored program's run error or wrong value | [an authored program fails](#rb-program) |
| a run that must be reproduced exactly, or that differs between runs | [reproduce a run](#rb-replay) |
| a failure seen once, or a flaky test | [a rare failure](#rb-rare) |
| a slow frame, a hitch, a mode over its budget | [a slow frame or an expensive mode](#rb-slow) |
| a reader that crashes or hangs on a file | [a corrupt or hostile input](#rb-input) |
| a detector that reports nothing, a mode that may be off | [a detector stays silent](#rb-blind) |
| a wrong but non-crashing number, a changed float mode, exhausted resources | [a wrong number or a pressured resource](#rb-pressure) |
| an application or module that needs the diagnostics | [switch the diagnostics on](#rb-host) |
| a build gate that fails on one compiler or only in CI | [a build gate fails](#rb-build) |

## Entries

<a id="rb-route"></a>
### Which tests and lanes does this change need?

| Field | Value |
|---|---|
| Symptom | A change is ready and must be verified locally on what it touches, then on every lane by CI. |
| Command | `python scripts/dev.py plan --build build/win-debug`; `python scripts/dev.py plan --build build/win-debug --json` |
| Artifact | The plan: its reasons, `targets`, the selected tests and `risk_routes` (local commands, notes, hosted lanes). |
| Read | Each risk route names the local command for that risk (the win-asan check, a WSL lane check, `check-headers.py`) and the hosted lanes that qualify it. A broadened plan says why; a full scope has no local route and is CI's. |
| Next test | `python scripts/dev.py check --build build/win-debug --dry-run`; `python scripts/dev.py check --build build/win-debug`; `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh dev linux-gcc-debug --build-timeout 3000` |
| Hosted | Every lane of the pushed revision's run; the push resolves its tier from its paths ([CI tiers](ci-tiers.md)). |
| Adopt | `crd_discover_tests(<test-target>)` registers a new test executable's cases with their owner, so the plan selects them. |
| Example | `tests/foundation/jobs/CMakeLists.txt` |
| Cost | Tooling, no runtime mode; the local routes it prints are timed on the [build boards](../bench/2026-10-18-build-speed-cache-mirror.md). |
| Owner | DIAG.11b |

<a id="rb-ci"></a>
### A hosted lane is red

| Field | Value |
|---|---|
| Symptom | A job of a push or nightly run failed, or a run was cancelled after some of its jobs had finished. |
| Command | `gh run list --branch main --limit 15`; `gh run view <run> --json jobs`; `gh api repos/yatiyr/CRD/actions/jobs/<job>/logs` |
| Artifact | The job log; the lane's `evidence-<preset>` artifact (`conclusion.json`, the CTest JUnit file); the gate's line `registered failures for this lane`. |
| Read | `gate: FAIL` names each unexpected failure and each registered test that did not fail. `registered TP-n: failed` is a known third-party defect ([register](../third-party-defects.md)); a failure seen before and never reproduced is in `tests/support/diag/nonreproduced-failures.json`. A cancelled run does not read as failed: read its finished jobs. |
| Next test | `python scripts/check-registered-failures.py --lane <preset> --junit <ctest-junit.xml> --ctest-exit <code>`; `python scripts/dev.py plan --build build/win-debug --path <failing file>`; `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh test linux-gcc-asan -R "<failing test>"` |
| Hosted | Every lane ends in the registered-failure gate. |
| Adopt | `python scripts/check-registered-failures.py --lane <preset> --junit <file> --ctest-exit <code>` is the last line of a new lane's test step; a third-party failure joins the register only with its reproduction. |
| Example | `docs/third-party-defects.md`; `tests/support/diag/nonreproduced-failures.json` |
| Cost | Tooling, no runtime mode; a lane's own time is in its log ([CI tiers](ci-tiers.md)). |
| Owner | DIAG.10b, DIAG.11b |

<a id="rb-crash"></a>
### The process crashed

| Field | Value |
|---|---|
| Symptom | An access violation (`0xC0000005`), a stack overflow (`0xC00000FD`), a fail-fast (`0xC0000409`), or SIGSEGV, SIGBUS or SIGILL ended the process. |
| Command (Windows) | `python scripts/debugger-recipes.py --probe`; `cdb -z <crash dump> -c ".ecxr;kP;q"` |
| Command (Linux) | `gdb -batch -ex bt <program> <core>` |
| Artifact | Windows: `crash_<pid>_<tick>_<serial>_<attempt>.dmp`, a minidump in the host profile's `crash_dir`; Linux: `crash_<pid>_<tid>_<ns>_<serial>.log`, its path printed on stderr after `record:`; with a flight recorder also `flight-<n>.cdb`. |
| Read | The exception record's code and address, the faulting thread's frames and the module identities (PDB GUID and age, ELF build ID). A symbol file that does not match its module is refused, never read for a plausible line. A fault inside a fiber job is not caught by the Windows last-chance filter (a measured limit). |
| Next test (Windows) | `ctest --test-dir build/win-debug -R "^crash capture: "`; `ctest --test-dir build/win-debug -R "^symbolize \(windows\)"` |
| Next test (Linux) | `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh test linux-gcc-debug -R "^crash capture \(linux\): "` |
| Next test | `ctest --test-dir build/win-debug -R "^golden bundle: "` |
| Hosted | Every Windows lane, win-asan included, and every Linux lane. |
| Adopt | `crash_handler = "on"` in a `*.host.toml` profile that `crd::perf::DiagnosticHost::start(...)` loads; a thread created before the install calls `guard_current_thread_stack(...)`. |
| Example | `assets/diag/host_default.host.toml`; `tests/support/diag/specimens/crash_capture_specimen.cpp` |
| Cost | No per-frame cost: an installed handler and, on Windows, a 64 KiB stack guarantee for every thread created after install; the basic and full arms of the [overhead board](../bench/2026-10-16-diag-11b-overhead.md#verdicts) run with it installed. |
| Owner | DIAG.2b, DIAG.5a, DIAG.5b, DIAG.5d |

<a id="rb-fatal"></a>
### A fatal invariant, an assertion or a named out-of-memory fatal stopped the process

| Field | Value |
|---|---|
| Symptom | `CRD_FATAL` or a failed `CRD_ASSERT` printed its file and line and the process aborted; or an allocator's out-of-memory fatal named itself and the request. |
| Command | `ceridc diag --command bundle.inspect --path <flight bundle> --root <capture dir>` |
| Artifact | The flight bundle `flight-<n>.cdb` in the host profile's `capture_dir`: the trigger, the events of the window before it and this build's identity. |
| Read | The trigger's code, file and line, and the events before it. An allocation refusal names the allocator and the size; a refused shape in a test is a `CRD_DIAG_FATAL_OTHER` naming the allocator that refused first. |
| Next test | `ctest --test-dir build/win-debug -R "^crd-(containers|scene|ceir|resources|toml)-fatal-"`; `ctest --test-dir build/win-debug -R "^flight trigger: "`; `ctest --test-dir build/win-debug -R "^allocator consumers: "` |
| Hosted | Every lane: the `crd-*-fatal-*` modes pass on their `CRD_DIAG_FATAL_OK` line. |
| Adopt | `crd_diag_fatal_tests(<specimen> PREFIX <prefix> OWNER <test-executable> MODES <mode>)` with a specimen on `expected_fatal.hpp`; refuse exactly one allocation with `refusing_allocator.hpp`; allocate through `allocate_or_fatal(...)` or `construct_or_fatal(...)`. |
| Example | `tests/support/diag/specimens/scene_fatal_specimen.cpp`; `tests/support/diag/specimens/refusing_allocator.hpp` |
| Cost | A checked fatal adds no compare to a hot path ([null dereference repairs](../sessions/2026-10-17-diag-11b-link-time-null-dereference.md)). |
| Owner | DIAG.3a, DIAG.5c, DIAG.9b |

<a id="rb-hang"></a>
### The process hangs, a wait never returns, workers starve

| Field | Value |
|---|---|
| Symptom | No progress; a test reaches its CTest timeout; the jobs watchdog prints a hang, starvation or livelock report. |
| Command | `ceridc mcp --diag-host assets/diag/host_investigate.host.toml`; `ceridc diag --command jobs.waits` |
| Command (Windows) | `cdb -pv -p <pid> -c "~*kP;q"` |
| Artifact | Windows: `hang_<pid>_<tick>_<serial>_<attempt>.dmp`, a non-fatal live dump with the wait evidence; a flight bundle whose trigger carries the classified kind (`WaitCycle`, starvation, livelock); `jobs.waits` items. |
| Read | The wait graph's edges (which task waits on whose counter) and each worker's responsiveness; a snapshot that a stuck worker cannot answer is reported incomplete and names the task. A completed but unwaited counter is not a hang. |
| Next test | `ctest --test-dir build/win-debug -R "^hang watchdog: "`; `ctest --test-dir build/win-debug -R "^diag harness: a seeded wait-cycle deadlock"`; `ctest --test-dir build/win-debug -R "^(livelock|starvation)"`; `ctest --test-dir build/win-debug -R "^worker snapshot: "`; `ctest --test-dir build/win-debug -R "^rt sentinel: "` |
| Hosted | Every lane; linux-clang-tsan runs the same cases under the race detector. |
| Adopt | `watchdog_period_ms = 250` in the host profile; `crd::RtScope(...)` around a callback that must neither allocate nor block. |
| Example | `tests/foundation/jobs/test_diag_deadlock_specimen.cpp`; `assets/diag/host_investigate.host.toml` |
| Cost | The watchdog and the pool's diagnostics are in the basic and full arms of the jobs workload, unresolved on every tuple ([board losses](../bench/2026-10-16-diag-11b-overhead.md#losses-and-what-they-belong-to), DIAG.11b clause (4)). |
| Owner | DIAG.4a, DIAG.4c |

<a id="rb-race"></a>
### A data race, or an ordering bug that shows only sometimes

| Field | Value |
|---|---|
| Symptom | `WARNING: ThreadSanitizer: data race`, or a result that changes between runs under load. |
| Command | `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh dev linux-clang-tsan --build-timeout 3000` |
| Artifact | The TSan report in the CTest log. Suppressions are for third-party code only: `tests/support/diag/tsan-suppressions.txt`. |
| Read | The two stacks and the missing happens-before edge. Fiber switches are modelled, so a report across fibers is real; an instrument may not add ordering to make a run clean. |
| Next test (Linux) | `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh test linux-clang-tsan -R "^tsan"` |
| Next test | `cmd /c scripts\build-target.bat build\win-debug crd-jobs-schedcheck-tests`; `build/win-debug/tests/jobs/crd-jobs-schedcheck-tests.exe`; `python scripts/check-genmc-models.py --help` |
| Hosted | linux-clang-tsan. The controlled-interleaving executable is not a CTest and runs on no hosted lane (DIAG.11b clause (9)). |
| Adopt | `CRD_JOBS_SCHED_POINT(<tag>)` at a publication or reclamation step, live only in the `crd-jobs-schedcheck` twin. |
| Example | `tests/foundation/jobs/test_sched_check.cpp`; `tests/support/diag/specimens/data_race_specimen.cpp` |
| Cost | A separate binary; its slowdown is not on a board yet ([DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (9)). |
| Owner | DIAG.1a, DIAG.1b, DIAG.4b |

<a id="rb-memory"></a>
### A bad memory access

| Field | Value |
|---|---|
| Symptom | `heap-buffer-overflow`, `heap-use-after-free`, `use-after-poison`, `container-overflow`, `stack-use-after-return`, a UBSan `runtime error`, or a guarded allocator's `RedzoneOverrun`. |
| Command (Windows) | `python scripts/dev.py check --build build/win-asan` |
| Command (Linux) | `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh dev linux-gcc-asan --build-timeout 3000` |
| Artifact | The sanitizer report in the test log: the access, the allocation and free stacks, the shadow bytes; a `DiagnosticAllocator` report with both sites. |
| Read | Shadow `f7` (poisoned by user) means an allocator poisoned the range: suspect whoever poisons it (an arena reset or rewind, a freed pool slot, a TLSF header, pages given back to the OS still poisoned). `fc` is a container's unused capacity: an access past `Array::size()`. A structural walker failure names the free list or block it found broken. |
| Next test | `ctest --test-dir build/win-debug -R "^allocator asan: "`; `ctest --test-dir build/win-debug -R "^diag 3f: "`; `ctest --test-dir build/win-debug -R "^diag-alloc: "`; `ctest --test-dir build/win-debug -R "^pool: "`; `ctest --test-dir build/win-debug -R "^crd-fuzz-memory-allocators-corpus$"` |
| Hosted | win-asan; linux-gcc-asan (with UBSan). A non-sanitizer lane reports each route absent, never caught. |
| Adopt | An allocator that poisons calls `asan_poison(...)` and `asan_unpoison(...)` on exactly its own ranges; a container marks its live range with `asan_annotate_live_range(...)`; wrap a suspect allocator in `crd::memory::DiagnosticAllocator(...)`. |
| Example | `tests/support/diag/specimens/allocator_poison_specimen.cpp`; `engine/foundation/memory/include/crd/memory/diagnostic_allocator.hpp` |
| Cost | A separate binary (the ASan lanes); its slowdown is not on a board yet ([DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (9)). |
| Owner | DIAG.3b, DIAG.3c, DIAG.3d, DIAG.3f |

<a id="rb-lifetime"></a>
### A handle, entity or resource outlives its object

| Field | Value |
|---|---|
| Symptom | A stale handle reaches a reused slot, a resource handle survives a reload, a callback runs after unsubscribe. |
| Command | `ceridc diag --command gpu.resources` |
| Artifact | The process-wide identity registry's live resource, program and pass counts; a validation report naming a retired identity. |
| Read | Every get through a `Handle{index, generation}` checks the generation: a stale one is refused, never resolved to the new occupant. A native object's debug name carries its Cerid identity after destruction. |
| Next test | `ctest --test-dir build/win-debug -R "^slot_map: "`; `ctest --test-dir build/win-debug -R "^DIAG\.3e: "`; `ctest --test-dir build/win-debug -R "^GPU object identity mirrors the SlotMap"` |
| Hosted | Every lane. |
| Adopt | Address a reloadable or public object through `crd::containers::SlotMap<T>(...)` and its `Handle`, never a raw pointer that outlives a reload. |
| Example | `engine/foundation/containers/include/crd/containers/slot_map.hpp` |
| Cost | One generation compare per lookup; not boarded ([DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (9)). |
| Owner | DIAG.3e |

<a id="rb-leak"></a>
### Memory grows, or allocations are still live at exit

| Field | Value |
|---|---|
| Symptom | Resident memory grows over frames, or LeakSanitizer reports `detected memory leaks` at exit. |
| Command | `build/win-debug/sandbox/crd-sandbox.exe --diag memory.allocators --smoke-test 3`; `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh dev linux-gcc-asan --build-timeout 3000` |
| Artifact | The sandbox log's `memory.allocators` answer: one item per allocator registered with the profiler with its allocations, deallocations and bytes in use; the LeakSanitizer report. |
| Read | An allocator whose live count keeps rising is the owner to read; LeakSanitizer separates unreachable objects from retained arenas and caches. |
| Next test | `ctest --test-dir build/win-debug -R "^diag 3f: a memory leak"`; `ctest --test-dir build/win-debug -R "^allocator ownership: "`; `ctest --test-dir build/win-debug -R "^crd-diag-overhead-alloc-"` |
| Hosted | linux-gcc-asan (LeakSanitizer); every lane for the ownership cases and the overhead harness's lifetime ledger. |
| Adopt | Register an allocator with the profiler, `crd::perf::register_allocator(...)`, so `memory.allocators` lists it. |
| Example | `tests/support/diag/specimens/leak_specimen.cpp` |
| Cost | The allocator statistics cost +37.7% in the disabled mode on allocation-heavy frames ([board losses](../bench/2026-10-16-diag-11b-overhead.md#losses-and-what-they-belong-to), DIAG.11b clause (1)). |
| Owner | DIAG.3f, DIAG.6a |

<a id="rb-gpu"></a>
### A GPU validation error, a removed device or a lost device

| Field | Value |
|---|---|
| Symptom | A validation message names a Cerid resource, pass or program; `DXGI_ERROR_DEVICE_REMOVED`; `VK_ERROR_DEVICE_LOST`. |
| Command | `ceridc diag --command gpu.resources` |
| Command (Windows) | `set CRD_DX12_HARDWARE_FAULT_OPT_IN=1`; `build/win-debug/tests/gpu-context-dx12/crd-gpu-context-dx12-tests.exe "[.dx12-hardware-device-fault]" -s` |
| Artifact | `gpu_<pid>_<tick>_<serial>_<attempt>.dmp`, a `DeviceRemoved` live dump: DX12's removal bundle with DRED breadcrumbs and page-fault allocations, or Vulkan's loss record (magic CRVL) with the first failure and the device-fault report; validation reports correlated to Cerid identities. |
| Read | The in-flight pass DRED names, the page-fault allocation's Cerid identity, the loss origin (`Observed` or `Simulated`) and the first failure. Software adapters (WARP, lavapipe) are labelled as such. The hardware opt-in run above (one contained page fault in a child process, never in CI) recorded no fault on this machine's GPU on 2026-10-07; the route is explicitly unqualified. |
| Next test | `ctest --test-dir build/win-debug -R "^DIAG\.7a"`; `ctest --test-dir build/win-debug -R "^DIAG\.7c"` |
| Next test (Windows) | `ctest --test-dir build/win-debug -R "^DIAG\.7b"`; `ctest --test-dir build/win-debug -R "^DIAG\.9b: an injected device loss"` |
| Hosted | The Windows lanes on the pinned WARP adapter; the Linux lanes on lavapipe with the pinned validation layer. |
| Adopt | Name a native object with its identity through `format_debug_name(...)`; a host registers its contexts with the `gpu.resources` command. |
| Example | `tests/gpu/gpu-context-dx12/test_dx12_device_fault.cpp` |
| Cost | Device validation is a targeted mode outside the basic budget; its cost is not boarded ([DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (9)). |
| Owner | DIAG.7a, DIAG.7b, DIAG.7c |

<a id="rb-program"></a>
### An authored program fails or computes the wrong value

| Field | Value |
|---|---|
| Symptom | A CEIR or CHIR program reports a compile or run error, or returns a wrong value. |
| Command | `ceridc diag --command program.provenance --path <program.ceir> --root <dir>`; `ceridc inspect --program <program.ceir> --entry <name> --break <line> --watch <line>`; `ceridc diag --command program.inspect --grant read,execute --path <program.ceir> --root <dir> --param breaks=<line>` |
| Artifact | The command's JSON report on stdout: an op item per op with its authored file, line and column and its CHIR origins; a stop item per stop and a value item per watched line. |
| Read | A compile error names its op and line; a run error keeps its authored position through CSE, serialization and compilation; unknown attribution is an explicit gap, never a zero location. |
| Next test | `ctest --test-dir build/win-debug -R "^diag 8a: "`; `ctest --test-dir build/win-debug -R "^diag 8b: "`; `ctest --test-dir build/win-debug -R "^diag 8c: "` |
| Hosted | Every lane. |
| Adopt | A host serves its own commands through `DiagCommandService::register_command(...)`; ops parsed from text keep their positions with no extra step. |
| Example | `assets/diag/overhead_offline.ceir` |
| Cost | The CEIR profiling bridge's per-instruction timing costs 4 to 5.4 times the run in full mode ([board losses](../bench/2026-10-16-diag-11b-overhead.md#losses-and-what-they-belong-to), DIAG.11b clause (6)). |
| Owner | DIAG.2a, DIAG.8a, DIAG.8b, DIAG.8c |

<a id="rb-replay"></a>
### Reproduce a run exactly

| Field | Value |
|---|---|
| Symptom | A run must be replayed to its first divergence, or two runs of the same program differ. |
| Command | `ceridc diag --command replay.prepare --path <program.ceir> --root <dir>`; `ceridc inspect --program <program.ceir> --record <run.crpl> --seed <u64>`; `ceridc diag --command replay.run --grant read,execute --path <run.crpl> --root <dir>` |
| Artifact | replay.prepare's input items and its summary (replay prepared or unavailable); the run record `<run>.crpl`; replay.run's first divergence. |
| Read | Which input a replay needs and why each is missing; the first divergent op and value, and the guarantee it breaks. |
| Next test | `ctest --test-dir build/win-debug -R "^diag 9a"`; `ctest --test-dir build/win-debug -R "^crd-fuzz-ceir-record-corpus$"` |
| Hosted | Every lane. |
| Adopt | Read random numbers, clocks and events through the host's input seam (`--seed`, `--clock`, `--events`), so a record can hold them. |
| Example | `engine/execution/ceir-cook/src/replay_record.cpp` |
| Cost | Recording is an `execute` and `record` request, not a runtime mode; not boarded ([DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (9)). |
| Owner | DIAG.9a |

<a id="rb-rare"></a>
### A rare failure, or a flaky test

| Field | Value |
|---|---|
| Symptom | A failure was seen once and does not reproduce on a rerun, or a test fails some runs only. |
| Command | `ceridc mcp --diag-host assets/diag/host_investigate.host.toml`; `python scripts/reverse-debug-recipes.py --probe` |
| Command (Windows) | `python scripts/reverse-debug-recipes.py --route cdb-watch --mode corrupt --specimen build/win-debug/tests/diag/crd-diag-corruption-specimen.exe --source tests/support/diag/specimens/corruption_specimen.cpp --trace-dir <dir>` |
| Command (Linux) | `python3 scripts/reverse-debug-recipes.py --route gdb-record --mode corrupt --specimen <crd-diag-corruption-specimen> --source tests/support/diag/specimens/corruption_specimen.cpp --trace-dir <dir>` |
| Artifact | Flight bundles at each trigger (assertion, fatal, hang, hitch, allocation failure); a fault plan's firing log; the reverse-debugging verdict (`FOUND`, `CLEAN`, `HOST_INELIGIBLE`, `TOOL_MISSING`). |
| Read | The first failure's evidence, kept through retries; a minimized fault plan or input set that still reproduces; a candidate that fails on some runs only is a race, not a reproduction. A local failure that does not reproduce is recorded with its signature, never chased as a code defect without a reproduction. |
| Next test | `ctest --test-dir build/win-debug -R "^diag 9c"`; `ctest --test-dir build/win-debug -R "^fault plan: "`; `ctest --test-dir build/win-debug -R "^crd-diag-reverse-debug-recipes$"` |
| Next test (Linux) | `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh test linux-gcc-debug -R "^crd-diag-reverse-debug-gdb-"` |
| Hosted | Every lane; the gdb routes on the Linux lanes (record only without a sanitizer); the nightly run repeats the whole matrix. |
| Adopt | Arm a seeded fault plan through `fault.arm` under the `inject` grant (`assets/diag/allocation_failure_once.fault.toml`); shrink a reproduction with `minimize_fault_plan(...)` or `minimize_record_inputs(...)`. |
| Example | `assets/diag/allocation_failure_once.fault.toml`; `tests/support/diag/specimens/corruption_specimen.cpp` |
| Cost | Fault seams are compiled out of shipping builds; the flight recorder's basic cost is on the [overhead board](../bench/2026-10-16-diag-11b-overhead.md#verdicts). |
| Owner | DIAG.9b, DIAG.9c |

<a id="rb-slow"></a>
### A slow frame, or a mode that costs more than its budget

| Field | Value |
|---|---|
| Symptom | A frame hitch, a slow offline run, or an overhead board verdict `over` or `unresolved`. |
| Command | `build/win-debug/sandbox/crd-sandbox.exe --diag capture.start --diag-grant read,record --diag-root <dir>` (then capture.stop with a file name, from the panel); `build/win-debug/tools/cprof-export/cprof_export.exe <capture>.cprof <trace>.json`; `python scripts/diag-overhead.py run --help` |
| Command (Windows) | `python scripts/sample-cpu-wpr.py --from-etl <trace.etl>`; `scripts\elevated-captures.bat` |
| Artifact | A CPROF capture `<name>.cprof` that `capture.stop` writes under the root; its Perfetto trace-events JSON; an overhead board record `cerid-diag-overhead/1`; a WPR trace under `build/diag-overhead-wpr/`. |
| Read | Scopes, tasks, GPU passes and queue waits on their own tracks; an unscoped hotspot shows only in the sampled stacks. A board's interval decides: within, over, or unresolved (a loss, never a pass). |
| Next test | `ctest --test-dir build/win-debug -R "^crd-diag-overhead-"`; `ctest --test-dir build/win-debug -R "^perfetto export: "`; `ctest --test-dir build/win-debug -R "^CPROF "`; `ctest --test-dir build/win-debug -R "^pacing is not"`; `ctest --test-dir build/win-debug -R "^crd-diag-(cpu|overhead)-sampling-runbook$"` |
| Hosted | Every lane runs the overhead harness's structural CTests and the sampling verdict tests; the measured boards are local. |
| Adopt | A site is `CRD_FLIGHT_RECORD(<recorder>, <code>, <severity>, <message>, <a>, <b>)`, whose payload runs only when the event is kept; a scope is `CRD_PERF_SCOPE(<name>)`. |
| Example | `tests/support/diag-overhead/main.cpp`; `assets/diag/overhead_basic.flight.toml` |
| Cost | Measured: [overhead board](../bench/2026-10-16-diag-11b-overhead.md#verdicts). |
| Owner | DIAG.6a, DIAG.6b, DIAG.6c, DIAG.11b |

<a id="rb-input"></a>
### A corrupt or hostile input crashes or hangs a reader

| Field | Value |
|---|---|
| Symptom | A loader crashes, hangs or allocates without bound on one file, or a libFuzzer run reports an artifact. |
| Command | `python scripts/fuzz.py replay --build build/win-debug --target all`; `python scripts/fuzz.py minimize --build build/linux-clang-fuzz --target <target> --artifact <crash file>` |
| Artifact | libFuzzer artifacts (`crash-*`, `timeout-*`, `oom-*`, `leak-*`) under the run's scratch directory, and the run's JSON board. |
| Read | The minimized input and the reader's refusal it should have produced; a finding enters the committed corpus only through `adopt`, after the reader is fixed. |
| Next test | `ctest --test-dir build/win-debug -R "^crd-fuzz-.*-corpus$"` |
| Hosted | Every lane replays every corpus; the fuzzing preset runs bounded rounds manually. |
| Adopt | `crd_fuzz_target(<name> SOURCES <harness> CORPUS <dir>)` with a committed corpus of seeds. |
| Example | `tests/support/fuzz/include/crd/fuzz/harness.hpp` |
| Cost | The replay is a test; fuzzing rounds are timed in their boards ([test instruments](test-instruments.md#what-the-first-bounded-runs-found)). |
| Owner | DIAG.10a |

<a id="rb-blind"></a>
### A detector stays silent, or a mode may not be on

| Field | Value |
|---|---|
| Symptom | A report that should appear does not, or a run passes although its sanitizer or recorder may not have been active. |
| Command | `ceridc diag --command diag.capabilities`; `python scripts/diag-seeded-faults.py --validate`; `python scripts/diag-seeded-faults.py --build-dir build/win-debug --list`; `python scripts/diag-coverage.py --validate` |
| Artifact | The capabilities item (modes, dependencies, the grant); a seeded-fault report `cerid-diag-seeded-faults-report/1`; a coverage report `cerid-diag-coverage-report/1`. |
| Read | A route the lane cannot provide is reported absent (`InstrumentAbsent`, `LANE_UNSUPPORTED`), never caught or passed; a seed that `SURVIVED` is a blind detector. |
| Next test | `ctest --test-dir build/win-debug -R "^diag harness: "`; `ctest --test-dir build/win-debug -R "^doctor: "`; `ctest --test-dir build/win-debug -R "^crd-diag-(seeded-faults|coverage)$"` |
| Hosted | Every lane; the lane's promise comes from its configuration, so a specimen that loses its sanitizer fails. |
| Adopt | `crd_diag_specimen(<name> SOURCES <file>)` builds a bounded child the harness runs; a seed joins `tests/support/diag/seeded-faults.json`. |
| Example | `tests/support/diag/specimens/clean_specimen.cpp`; `tests/support/diag/seeded-faults.json` |
| Cost | Tests and tooling, no runtime mode ([test instruments](test-instruments.md#seeded-detector-failures-diag10b)). |
| Owner | DIAG.0, DIAG.10b |

<a id="rb-pressure"></a>
### A wrong number, a changed float mode, or a pressured resource

| Field | Value |
|---|---|
| Symptom | A result is wrong without a crash, a float mode differs from the declared one, a queue or budget runs near its bound, handles or files leak. |
| Command | `ctest --test-dir build/win-debug -R "^diag10c: "` |
| Artifact | The probe's verdicts in the test log: the differing float-mode field, the value class outside its domain, the pressure window's worst reading, the lifetime ledger's outstanding class. |
| Read | A float mode is compared field by field (a field read on one side only is `Unread`, never a match); a value is judged against the domain its boundary declares; a pressure level names an unread or unbounded resource and never judges it. |
| Next test | `ctest --test-dir build/win-debug -R "^jobs diag10c: "`; `ctest --test-dir build/win-debug -R "^diag10c: the ledger"` |
| Hosted | Every lane. |
| Adopt | Compare `read_fp_environment(...)` with `declared_fp_environment(...)` at a boundary; judge a value with `classify_value(...)` against its `FloatDomain`. |
| Example | `engine/foundation/core/include/crd/core/value_domain.hpp`; `engine/foundation/core/include/crd/core/fp_environment.hpp` |
| Cost | Probes are calls a boundary makes; not boarded ([DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (9)). |
| Owner | DIAG.10c |

<a id="rb-host"></a>
### Switch the diagnostics on in a new application

| Field | Value |
|---|---|
| Symptom | An application, tool or test host needs crash capture, the flight recorder, the watchdog and the command service. |
| Command | `ceridc mcp --diag-host assets/diag/host_default.host.toml`; `build/win-debug/sandbox/crd-sandbox.exe --diag-host assets/diag/host_investigate.host.toml --diag diag.capabilities --smoke-test 3` |
| Artifact | The started host's capabilities, and its bundles in `capture_dir` (`flight-<n>.cdb`, the newest ten kept). |
| Read | A profile that asks for a mode the build cannot provide (`required`) refuses to start; `on` continues and says the mode is unavailable. |
| Next test | `ctest --test-dir build/win-debug -R "^diag 11a: "`; `ctest --test-dir build/win-debug -R "^diagnostic host: "` |
| Hosted | Every lane; the public-checks lanes build the installed package's consumer and run its failure at both details (`crd-package-consumer`). |
| Adopt | Author a `*.host.toml` profile and start `crd::perf::DiagnosticHost::start(...)` before the window, the device and the jobs pool exist. |
| Example | `assets/diag/host_default.host.toml`; `tests/support/package/diag-consumer` |
| Cost | Measured: the basic arm of the [overhead board](../bench/2026-10-16-diag-11b-overhead.md#verdicts) is this host with the default profile. |
| Owner | DIAG.11a |

<a id="rb-build"></a>
### A build gate fails on one compiler, or only in CI

| Field | Value |
|---|---|
| Symptom | A guard CTest fails, a hot header reaches a forbidden standard header, a raw allocation result is used unchecked, or a link-time-optimized Linux link fails with a `-Werror=` diagnostic no compile showed. |
| Command | `ctest --test-dir build/win-debug -R "^crd-no-|^crd-check"`; `python scripts/check-include-budget.py --build build/win-debug`; `python scripts/check-allocation-results.py <checkout>` |
| Artifact | The guard's output: the include chain that reached a forbidden header, the unchecked allocation site, the link's "In function" line with its "inlined from" chain. |
| Read | Read the chain and fix the include or the site; never widen a budget. A link-time diagnostic lists only its first failing partition: rerun that link with `-flto-partition=one` to list every site of the program. |
| Next test | `ctest --test-dir build/win-debug -R "^crd-check-include-budget$"`; `ctest --test-dir build/win-debug -R "^crd-check-allocation-results$"` |
| Hosted | Every Ninja lane runs the guards; linux-gcc-shipping, -release and -relwithdebinfo judge the link-time warnings. |
| Adopt | A new hot header gets an entry in `scripts/include-budget.json`; a new allocation site checks its result or uses `allocate_or_fatal(...)`. |
| Example | `scripts/include-budget.json` |
| Cost | Tooling, no runtime mode; the compile time it protects is on the [compile-time board](../bench/2026-10-18-build-speed-compile-time.md). |
| Owner | DIAG.11b |

<a id="children-without-an-entry"></a>
## Children with no runbook entry

| Child | Why |
|---|---|
| DIAG.11c | Qualification of the Windows and Linux tuples and the portable boundary: evidence about the routes above, not a symptom of its own. |
| DIAG.12 | The independent close audit of every entry here: it re-runs their faulty and repaired controls. |

## Checking this document

`python scripts/check-diag-runbooks.py --build build/win-debug` holds every entry to the tree: each command's script,
flags, lane command, preset, `ceridc` verb, diag command, grant and test executable exist; each `ctest ... -R` (and
`wsl-lane.sh test ... -R`) regex matches a test the configured tree registers, on rows that apply to the host; each
artifact is something the sources write, each example an existing path, each cost a link; every DIAG child is an
entry's owner or listed above; nothing publishes. The `crd-check-diag-runbooks` CTest runs it on every Ninja lane with
that lane's tree, so the Windows lanes resolve the Windows rows and the Linux lanes the Linux rows; `--static` checks
everything but the test references. Its cases are in `scripts/test-repository-tools.py` (`DiagRunbooks`).
