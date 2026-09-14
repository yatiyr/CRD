# DIAG.0 — the diagnostic capability census

<!-- doc-role: historical -->
> Dated evidence for [DIAG.0](../ROADMAP.md#slice-diag.0). Contract:
> [runtime-diagnostics](../design/runtime-diagnostics.md#diag-0); direction:
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md); source findings:
> [research](../research/2026-09-14-diagnostics-and-instrumentation.md); plan:
> [programme plan](2026-09-14-diagnostics-programme-plan.md). Rules: [AGENTS](../../AGENTS.md).

## What this is and how it was derived

DIAG.0 freezes the census the rest of DIAG inherits: every allocator, container owner, lifecycle callback,
executor/provider and diagnostic path, each classified against dated evidence, with the DG01–DG18 findings refreshed
against the current tree. This is the first of two DIAG.0 increments; it does not implement the doctor or the specimen
harness (increment two) and does not fix any defect (each routes to its owning child). DIAG.0 stays `Open` until both
increments land.

- **Reference revision.** HEAD `f1f8dced` ("repo hardening v5"). The [research](../research/2026-09-14-diagnostics-and-instrumentation.md)
  cited its sources at `2b6dcdb0` ("v4"); the v4→v5 diff changes only build/preset/doc files plus `sparse.hpp`,
  `test_stopwatch.cpp` and the fuzz harness, so every DG-cited engine diagnostic source is byte-identical between the
  two revisions and the research citations hold unchanged at HEAD.
- **Mechanical derivation.** Owners were enumerated by search, not recall: allocators by `: public IAllocator`,
  containers from the public `crd/containers` headers, executors/providers by `Executor`/`Provider` declarations,
  diagnostic paths from the crash/assert/log/perf/CEIR/GPU sources named in the research. Every row links its source
  and its test, or states that no test exercises it.
- **No invented failures.** Nothing here claims a reproduced runtime failure that was not already reproduced by an
  owning slice. A source concern (a missing check, an unqualified path) is recorded as `unqualified`, not as a defect.
  DG05's races were reproduced by REPO.DEV.9 and are the one exception, already owned by DIAG.1a.
- **Scope fence.** This census changes no allocator, scheduler, GPU or numeric algorithm. Every gap it records is
  routed to the DIAG child that owns the mechanism; the parent census inherits none of that work.

## Classification legend

- **implemented** — the code exists and is reachable through the public production path.
- **tested** — a committed test exercises its function (the linked suite), independent of diagnostic qualification.
- **unqualified** — implemented, but its diagnostic behaviour (sanitizer coverage, integrity/provenance checks,
  adversarial detection, measured overhead) is not established; the owning DIAG child qualifies it.
- **unsupported** — a planned mode or route with no command today; a route name, not an existing capability.

## Allocators — `engine/foundation/memory`

| Owner | Source | Test | Class | DG / child |
|---|---|---|---|---|
| MallocAllocator | [malloc_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/malloc_allocator.hpp) | [test_memory.cpp](../../tests/foundation/memory/test_memory.cpp) | tested | reference only; guarded out of engine use |
| LinearAllocator | [linear_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/linear_allocator.hpp) | [test_memory.cpp](../../tests/foundation/memory/test_memory.cpp) | tested; unqualified (per-slot poison) | DG02 → [DIAG.3a](../ROADMAP.md#slice-diag.3a) |
| StackAllocator | [stack_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/stack_allocator.hpp) | [test_memory.cpp](../../tests/foundation/memory/test_memory.cpp) | tested; unqualified (poison) | DG02 → DIAG.3a/3b |
| PoolAllocator | [pool_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/pool_allocator.hpp) | [test_memory.cpp](../../tests/foundation/memory/test_memory.cpp) | tested; unqualified (checks region/slot, not allocated state) | DG03 → DIAG.3c/3d |
| TlsfAllocator | [tlsf_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/tlsf_allocator.hpp) | [test_tlsf_allocator.cpp](../../tests/foundation/memory/test_tlsf_allocator.cpp) | tested; unqualified (interior/stale/double-free, free-site attribution) | DG03 → DIAG.3c/3d |
| GrowableLinearAllocator | [growable_linear_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/growable_linear_allocator.hpp) | [test_growable_linear_allocator.cpp](../../tests/foundation/memory/test_growable_linear_allocator.cpp) | tested; unqualified (poison) | DG02 → DIAG.3a/3b |
| GrowablePoolAllocator | [growable_pool_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/growable_pool_allocator.hpp) | [test_growable_pool_allocator.cpp](../../tests/foundation/memory/test_growable_pool_allocator.cpp) | tested; unqualified (allocated state) | DG03 → DIAG.3c/3d |
| GrowableTlsfAllocator | [growable_tlsf_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/growable_tlsf_allocator.hpp) | [test_growable_tlsf_allocator.cpp](../../tests/foundation/memory/test_growable_tlsf_allocator.cpp) | tested; unqualified | DG03 → DIAG.3c/3d |
| StreamingCategoryAllocator | [streaming_category_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/streaming_category_allocator.hpp) | [test_streaming_allocator.cpp](../../tests/foundation/memory/test_streaming_allocator.cpp) | tested; unqualified | DG03 → DIAG.3c/3d |
| ThreadSafeAllocator | [thread_safe_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/thread_safe_allocator.hpp) | [test_resource_manager.cpp](../../tests/assets/resources/test_resource_manager.cpp) (via the resources suite) | tested; unqualified | DG03/DG08 → DIAG.3c |
| VirtualMemoryAllocator | [virtual_memory_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/virtual_memory_allocator.hpp) | [test_virtual_memory_allocator.cpp](../../tests/foundation/memory/test_virtual_memory_allocator.cpp) | tested; poisons unused/rewound regions (the DG02 model) | DG02 → DIAG.3a/3b |
| OffsetAllocator (standalone, not `IAllocator`) | [offset_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/offset_allocator.hpp) | [test_offset_allocator.cpp](../../tests/foundation/memory/test_offset_allocator.cpp) | tested | DIAG.3d (GPU/offset arithmetic) |
| RingAllocator (standalone, not `IAllocator`) | [ring_allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocators/ring_allocator.hpp) | [test_ring_allocator.cpp](../../tests/foundation/memory/test_ring_allocator.cpp) | tested | DIAG.3b |
| IAllocator (contract) | [allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocator.hpp) | across the memory suite | implemented; no universal raw-pointer liveness promise | DG04 → DIAG.3e |

The BudgetAllocator ([harness.hpp](../../tests/support/fuzz/include/crd/fuzz/harness.hpp)) and the CountingAllocator
copies in the CEIR/tensor tests are test instruments, not engine allocators; they are named here only so the census is
exhaustive. The fuzz harness wraps `MallocAllocator` deliberately for per-block sanitizer visibility (the
`crd-lint-allow-malloc-allocator` marker).

## Containers — `engine/foundation/containers`

Public headers: `array`, `atomic_array`, `concurrent_queue`, `fixed_array`, `hash`, `hash_map`, `hash_set`,
`incremental_dag`, `log_channel`, `ring_buffer`, `sort`, `span`, `spsc_queue`, `static_array`, `string`, `string_view`.
Direct suites: [test_array_freeze](../../tests/foundation/containers/test_array_freeze.cpp),
[test_atomic_array](../../tests/foundation/containers/test_atomic_array.cpp),
[test_concurrent_queue](../../tests/foundation/containers/test_concurrent_queue.cpp),
[test_containers](../../tests/foundation/containers/test_containers.cpp),
[test_incremental_dag](../../tests/foundation/containers/test_incremental_dag.cpp),
[test_sort](../../tests/foundation/containers/test_sort.cpp),
[test_spsc_queue](../../tests/foundation/containers/test_spsc_queue.cpp). Classification: **implemented; tested** for
function. Their **live-range** diagnostic coverage (ASan modelling of every element live range, not just the backing
allocation) is **unqualified** and owned by DIAG.3b. `concurrent_queue`, `atomic_array` and `spsc_queue` are the
concurrency-sensitive members and inherit DIAG.1b's schedule/weak-memory qualification.

## Lifecycle callbacks and scopes

| Owner | Source | Test | Class | DG / child |
|---|---|---|---|---|
| JobObserver (in-flight replacement, mismatched begin/end documented) | [observer.hpp](../../engine/foundation/jobs/include/crd/jobs/observer.hpp) | [test_jobs_adapter.cpp](../../tests/foundation/perf/test_jobs_adapter.cpp), [test_fiber_migration.cpp](../../tests/foundation/perf/test_fiber_migration.cpp) | tested (adapter path); unqualified (lifetime under replacement) | DG07 → DIAG.4a/4c |
| Application crash-handler install | [application.cpp](../../engine/foundation/app/src/application.cpp) | [test_application.cpp](../../tests/foundation/app/test_application.cpp) (app lifecycle; no diagnostics-lifecycle demonstration) | implemented; unqualified (reusable per-host diagnostics lifecycle) | DG17 → DIAG.11a |
| Logger shutdown / assert-bridge flush | [logger.cpp](../../engine/foundation/log/src/logger.cpp) | [test_log.cpp](../../tests/foundation/log/test_log.cpp) | tested (function); unqualified (fatal-path recursion) | DG10 → DIAG.5c |
| perf memory-tracking scopes | [perf/memory.hpp](../../engine/foundation/perf/include/crd/perf/memory.hpp) | [test_memory_tracking.cpp](../../tests/foundation/perf/test_memory_tracking.cpp) | tested | DIAG.2b |
| perf scope push/pop + frame mark | [scope.hpp](../../engine/foundation/perf/include/crd/perf/scope.hpp) | [test_scope_push_pop.cpp](../../tests/foundation/perf/test_scope_push_pop.cpp), [test_frame_mark.cpp](../../tests/foundation/perf/test_frame_mark.cpp) | tested | DIAG.2b/6a |

## Executors and providers

| Owner | Source | Test | Class | DG / child |
|---|---|---|---|---|
| CEIR host provider / `IProvider` | [provider.hpp](../../engine/execution/ceir/include/crd/ceir/provider.hpp), [host_provider.hpp](../../engine/execution/ceir-host/include/crd/ceir/host/host_provider.hpp) | CEIR host suites | implemented; unqualified (full CHIR→CEIR→CKIR→native/GPU provenance) | DG13 → DIAG.8a/8b |
| CEIR executor step hooks | [exec.hpp](../../engine/execution/ceir/include/crd/ceir/exec.hpp) | CEIR exec suites | implemented; unqualified (provenance after optimize/reload) | DG13 → DIAG.8a/8b |
| CEIR diagnostics (codes/locations, copied messages) | [diagnostic.hpp](../../engine/execution/ceir/include/crd/ceir/diagnostic.hpp) | CEIR suites | implemented (authored provenance) | DG13 → DIAG.8a |
| Profiler registry (raw pointers under mutex; snapshot reads without it) | [profiler.cpp](../../engine/foundation/perf/src/profiler.cpp) | [test_thread_registration.cpp](../../tests/foundation/perf/test_thread_registration.cpp), [test_counters_threaded.cpp](../../tests/foundation/perf/test_counters_threaded.cpp) | tested (function); unqualified (concurrent deregistration/reuse — a source concern, not a reproduced race) | DG08 → DIAG.2b/6a |
| IProfilerGpuBackend (only a test mock implements it) | [gpu_scope.hpp](../../engine/foundation/perf/include/crd/perf/gpu_scope.hpp) | [test_gpu_scope.cpp](../../tests/foundation/perf/test_gpu_scope.cpp) | implemented (interface); unsupported (no shipping GPU implementation) | DG11 → DIAG.6b/7a |
| IFrameGraph pass timings (real DX12/Vulkan, used by sandbox) | [frame_graph.hpp](../../engine/gpu/gpu-context/include/crd/gpu/frame_graph.hpp) | sandbox path | implemented; unsupported (profiler-consumer path) | DG11 → DIAG.6b/7a |
| GPU backends (dx12, vulkan, cuda; KIR dx12/vulkan/cuda/hip/metal/webgpu) | `engine/gpu/*` | backend suites under `tests/gpu` | implemented (varies by backend); unqualified (fault depth) | DG12 → DIAG.7b/7c |

## Diagnostic paths

| Path | Source | Test | Class | DG / child |
|---|---|---|---|---|
| Crash capture (MiniDumpWriteDump in the crashing process, return ignored; Linux lacks `SA_ONSTACK`) | [crash.cpp](../../engine/foundation/core/src/crash.cpp) | **no death/child-process test** ([test_core.cpp](../../tests/foundation/core/test_core.cpp) covers neither); [main_diag.cpp](../../tests/foundation/jobs/main_diag.cpp) is a fiber-VEH printer, not a crash-path qualification | implemented; unqualified | DG09 → DIAG.5a/5b |
| Assertion (ignore-table lock; modal Windows dialog) | [assert.cpp](../../engine/foundation/core/src/assert.cpp) | `CRD_ASSERT` is exercised as a precondition check across the suites; no test qualifies the handler's fatal path | implemented; unqualified (recursion/reentrancy) | DG10 → DIAG.5c |
| Ring-buffer sink (not an emergency recorder) | [ring_buffer_sink.cpp](../../engine/foundation/log/src/sinks/ring_buffer_sink.cpp) | [test_log.cpp](../../tests/foundation/log/test_log.cpp) | tested (function); unsupported (emergency pre/post-trigger recording) | DG10 → DIAG.5c |
| CPROF capture (versioned snapshot, pinned POD layouts) | [capture.hpp](../../engine/foundation/perf/include/crd/perf/capture.hpp) | [test_capture_roundtrip.cpp](../../tests/foundation/perf/test_capture_roundtrip.cpp) | tested (round-trip); unsupported (unified crash+symbol+generation+capture bundle/consumer path) | DG14 → DIAG.5d/6c |
| DX12 / Vulkan validation capture | [dx12_validation_capture.cpp](../../engine/gpu/gpu-context-dx12/src/dx12_validation_capture.cpp), [vulkan_validation_capture.cpp](../../engine/gpu/gpu-context-vulkan/src/vulkan_validation_capture.cpp) | dx12/vulkan validation suites | implemented (validation capture); unsupported (DRED/device-fault extraction, GPU-assisted mode wiring) | DG12 → DIAG.7b/7c |
| Fiber sanitizer annotations (synchronizing switches; deque fence) | [sanitizer_fibers.hpp](../../engine/foundation/jobs/src/sanitizer_fibers.hpp) | [test_fiber_switch.cpp](../../tests/foundation/jobs/test_fiber_switch.cpp) | implemented; unqualified (artificial ordering cannot prove no concealed race) | DG06 → DIAG.1b/4b |
| Doctor (compiled/enabled/usable modes + dependencies report) | — none — | — | **unsupported** (built in DIAG.0 increment two) | DG01 → DIAG.0 |

## DG01–DG18 refresh at `f1f8dced`

The [research table](../research/2026-09-14-diagnostics-and-instrumentation.md) is the authoritative DG map; its
source citations are unchanged v4→v5 and hold at HEAD. Summary of standing and owner:

| DG | Standing at HEAD | Owning child |
|---|---|---|
| DG01 tool coverage | ASan/UBSan enabled; TSan and MSVC use-after-return not qualified in presets | DIAG.0, DIAG.3f, DIAG.1b |
| DG02 allocator shadows | VirtualMemoryAllocator poisons; per-slot poison absent in pool/TLSF/linear | DIAG.3a/3b |
| DG03 integrity/provenance | region/slot checks only; no allocated-state / stale / double-free / free-site | DIAG.3c/3d |
| DG04 borrowed lifetime | distinct ownership roles; no universal raw-pointer liveness | DIAG.3e |
| DG05 scheduler races | free-list `next_free` links; **reproduced** by REPO.DEV.9 | DIAG.1a |
| DG06 sanitizer model | synchronizing switches / deque fence; ordering unproven | DIAG.1b/4b |
| DG07 observer lifetime | in-flight replacement, mismatched begin/end documented | DIAG.4a/4c |
| DG08 profiling registration | raw pointers under mutex, snapshot reads without; source concern | DIAG.2b/6a |
| DG09 fatal-path | dump written in-process, return ignored; Linux no `SA_ONSTACK` | DIAG.5a/5b |
| DG10 assert/log recursion | ignore-table lock, modal dialog; ring sink not emergency | DIAG.2b/5c |
| DG11 disconnected GPU traces | real frame-graph timings; only a mock profiler backend | DIAG.6b/7a |
| DG12 GPU fault depth | validation captured; DRED/device-fault not wired | DIAG.7b/7c |
| DG13 authored provenance | CEIR codes/locations own copies; no full post-optimize chain | DIAG.8a/8b |
| DG14 captures/symbols | CPROF versioned; no unified bundle/consumer | DIAG.5d/6c |
| DG15 reproducibility | frame/corpus capture omits schedule/clock/random/completion | DIAG.9a/9b/9c |
| DG16 testing the tests | four ingestion fuzzers; no allocator-state/capture/command/reload/fault fuzzing | DIAG.10a/10b/10c |
| DG17 host integration | app installs crash handling; no reusable per-host lifecycle | DIAG.11a |
| DG18 portability/cost | not qualified for macOS/Metal, WASM, ARM64, unattended, production overhead | DIAG.11b/11c, MAC.DIAG, WEB.DIAG |

## Existing hardware evidence

DIAG.0 records the current hardware from existing evidence and runs no new probes. The DX12 adapter census and its
recipe ([session](2026-09-12-dx12-adapter-classification.md), [recipe](../recipes/2026-09-12-dx12-adapter-classification.md))
hold the GPU adapter classification; the numeric [bench boards](../bench/README.md) record the CPU/ISA of each measured
run at measurement time (AGENTS "measured benchmark board … at measurement time"). No new host tuple is asserted here;
DIAG's overhead qualification (DIAG.11b/11c) records the exact OS/ISA/compiler/adapter/driver tuple of each measured
mode when it measures, per the shared acceptance rules.

## Fixture manifest the rest of DIAG inherits

Increment two builds, and every later DIAG leaf reuses:

- **A specimen harness** that distinguishes an *expected detection* from an *instrument failure*: each dangerous
  specimen runs in an isolated child process with bounds; the parent passes only on the expected diagnostic code,
  identity, exit reason and artifact, and fails on an arbitrary crash, a missing executable, zero tests selected or a
  timeout. No intentional undefined behaviour runs inside an ordinary in-process unit test.
- **A doctor** that reports each mode as compiled / enabled / usable with its dependencies. At DIAG.0 most modes are
  honestly `unsupported` or `unqualified` per the tables above; the doctor states that, it does not pretend a route is
  a command.
- **The five negative controls** DIAG.0 acceptance requires the harness to detect: a missing symbolizer, a wrong
  sanitizer runtime, a zero test selection, a denied output path and a mismatched binary.

Budgets, module placement, schema name, tool tuples and provider choices are frozen in
[ADR-0133 → Implementation decisions](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions).

## Increment two: the fixture (doctor + specimen harness)

The second DIAG.0 increment builds the fixture the rest of DIAG inherits, and closes the acceptance.

- **Specimen harness** ([tests/support/diag](../../tests/support/diag), `crd-diag-harness`): a bounded child-process
  runner that classifies a specimen against an expectation into one Verdict, distinguishing an expected detection
  (Clean / Crashed / SanitizerCaught) from an instrument failure (Timeout / MissingExecutable / InstrumentAbsent /
  DeniedOutput / MismatchedBinary / ZeroSelection / Unexpected) -- never a silent pass. Timeout is the mandatory
  bound; no address-space rlimit is set (it would break ASan's shadow), recorded honestly. It records the
  OS/ISA/compiler tuple per run.
- **Specimens** ([tests/support/diag/specimens](../../tests/support/diag/specimens), built by
  [crd_diag_specimen](../../cmake/CrdDiag.cmake) with a compile-stamped identity): a clean exit, a headless hard
  crash (null write, no Windows error dialog), and a heap-buffer-overflow that a sanitizer build catches and a
  non-sanitized build reports absent.
- **The doctor** ([crd/perf/doctor.hpp](../../engine/foundation/perf/include/crd/perf/doctor.hpp)): reports each
  diagnostic mode as compiled / enabled / usable with a census-sourced disposition (most `unqualified` or
  `unsupported` at DIAG.0), tagged schema `cerid-diagnostics/1`, plus three live-probed dependencies (sanitizer
  runtime, symbolizer, output path). It states a route is unsupported rather than pretending it is a command.

The five negative controls are all detected, in dated evidence on `win-debug`:

| Negative | Detected as | Where |
|---|---|---|
| missing symbolizer | `symbolizer` dependency not present (bogus `ASAN_SYMBOLIZER_PATH`) | doctor |
| wrong / absent sanitizer runtime | `InstrumentAbsent` (a sanitizer catch expected of a non-sanitized specimen) | harness |
| zero test selection | `ZeroSelection` (a filter matching no specimen) | harness |
| denied output | `DeniedOutput` (a file used as an output directory) | harness + doctor |
| mismatched binary | `MismatchedBinary` (echoed identity != expected) | harness |

Positives pass too: the clean specimen is `Clean`, the crash specimen `Crashed`, and the sanitizer specimen is
`SanitizerCaught` on a sanitizer build (`InstrumentAbsent` on `win-debug`, which has none). Local proof: the 7
`crd-diag-harness` controls and the 4 doctor controls pass under `ctest` on `win-debug`. No source-runtime failure
was invented from the census; every gap it recorded routes to its owning DIAG child. The hosted sanitizer lanes turn
the sanitizer positive from `InstrumentAbsent` into `SanitizerCaught` -- the CI half of this slice.
