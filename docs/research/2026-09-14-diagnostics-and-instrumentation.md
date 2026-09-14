# Cerid diagnostics and instrumentation research

<!-- doc-role: reference -->
> Evidence and rationale, assessed 2026-09-14. Only [ROADMAP](../ROADMAP.md#slice-diag.0) tracks work.
> Implementation: [contract](../design/runtime-diagnostics.md); direction: [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).

## Verdict and scope

Cerid needs a connected diagnostic system before expanding its renderer and UI. Existing modules already provide
valuable instruments; the priority is closing their blind spots and making evidence survive the failures they observe.
The design must support interactive games, long offline renders, headless engineering workloads, runtime-authored
CEIR/CHIR/CKIR programs and later editor/browser hosts. It must also diagnose the instrumentation itself.

No finite toolset guarantees that every crash or Heisenbug becomes easy to identify. Some failures alter timing when
instrumented; some occur outside instrumented code; power loss, forced process termination and browser eviction may
prevent a final report. The actionable standard is a declared detection envelope, useful retained evidence, deliberate
failure specimens, reproducible experiments where possible and honest missing-data reports. Absence of a sanitizer
report is not proof of absence of a defect.

This is a dated source review of HEAD `2b6dcdb090cea710fbaa7bdc4a94024ab5d2f510` plus the existing uncommitted
repository repairs. It did not reproduce new runtime failures or execute performance experiments. The
[earlier review](../sessions/2026-09-14-development-diagnostics-review.md) and
[instrument experiments](../design/test-instruments.md) supply the existing measured evidence. Product algorithm
changes in geometry, physics, audio, networking or numerics are outside this programme; their diagnostic integration
contracts remain mandatory when those products are implemented.

## Source findings and owners

The following is a dated finding-to-owner reference, not a status table. “Observed” describes source directly read;
“recorded” refers to linked prior experiments; “qualification gap” requires the implementation agent to test the
claim before reporting a new runtime defect. Search these `DG` identifiers in ROADMAP.

| Finding | Evidence and consequence | Primary owning slices |
|---|---|---|
| DG01 — tool coverage | [CMake](../../CMakeLists.txt) enables ASan/UBSan/TSan; [presets](../../CMakePresets.json) do not qualify TSan or MSVC use-after-return. Recorded fiber UAR probe misses on current MSVC flags. | DIAG.0, DIAG.3f, DIAG.1b |
| DG02 — allocator shadows | [VirtualMemoryAllocator](../../engine/foundation/memory/src/virtual_memory_allocator.cpp) poisons unused/rewound regions; corresponding per-slot poisoning was not found in reviewed pool/TLSF/linear sources. | DIAG.3a, DIAG.3b |
| DG03 — integrity and provenance | [PoolAllocator](../../engine/foundation/memory/src/allocators/pool_allocator.cpp) checks region/slot boundary, not allocated state; [TLSF owns](../../engine/foundation/memory/src/allocators/tlsf_allocator.cpp) checks region. Interior/stale/double-free detection and free-site attribution need explicit contracts. | DIAG.3c, DIAG.3d |
| DG04 — borrowed lifetime | [IAllocator](../../engine/foundation/memory/include/crd/memory/allocator.hpp), [resource handles](../../engine/assets/resources/include/crd/resources/resource_handle.hpp) and [scene slot map](../../engine/world/scene/include/crd/scene/slot_map.hpp) have different ownership roles. No universal raw-pointer liveness promise is possible. | DIAG.3e |
| DG05 — scheduler races | [Fiber pool](../../engine/foundation/jobs/src/fiber_pool.cpp) and [counter pool](../../engine/foundation/jobs/src/counter.cpp) retain plain free-list links. TSan reports were recorded by REPO.DEV.9; former immediate owner CORE-USE.2 transfers the repair to DIAG.1a. | DIAG.1a |
| DG06 — sanitizer model | [Fiber annotations](../../engine/foundation/jobs/src/sanitizer_fibers.hpp) use synchronizing switches; the instrumented deque substitutes a fence. A clean switch-path run cannot establish that artificial ordering never conceals an application race. | DIAG.1b, DIAG.4b |
| DG07 — observer lifetime | [JobObserver](../../engine/foundation/jobs/include/crd/jobs/observer.hpp) permits in-flight replacement and documents mismatched begin/end delivery. The [end-hook repair](../sessions/2026-09-13-jobs-end-hook-ordering.md) fixed a distinct completion-order bug. | DIAG.4a, DIAG.4c |
| DG08 — profiling registration | [Profiler registry](../../engine/foundation/perf/src/profiler.cpp) mutates raw allocator/name pointers under a mutex while snapshot access reads entries without that mutex. Safe deregistration/destruction and concurrent reuse need investigation and qualification. This is a source concern, not a reproduced race here. | DIAG.2b, DIAG.6a |
| DG09 — fatal-path reliability | [crash.cpp](../../engine/foundation/core/src/crash.cpp) calls MiniDumpWriteDump in the crashing process and ignores its return value before announcing a dump; second-resolution Windows filenames can collide. Linux uses snprintf/backtrace, lacks SA_ONSTACK installation and ignores install failures. | DIAG.5a, DIAG.5b |
| DG10 — assertion/logging recursion | [assert.cpp](../../engine/foundation/core/src/assert.cpp) locks an ignore table and can display a modal Windows dialog. [Logger](../../engine/foundation/log/src/logger.cpp) uses allocating queued strings and locks; its assert bridge flushes. [Ring sink](../../engine/foundation/log/src/sinks/ring_buffer_sink.cpp) is not an emergency crash recorder. | DIAG.2b, DIAG.5c |
| DG11 — disconnected GPU traces | [IFrameGraph](../../engine/gpu/gpu-context/include/crd/gpu/frame_graph.hpp) has real DX12/Vulkan pass timings used by [sandbox](../../sandbox/src/main.cpp). Search found only a test mock implementing [IProfilerGpuBackend](../../engine/foundation/perf/include/crd/perf/gpu_scope.hpp); its public example names the retired rhi module. | DIAG.6b, DIAG.7a |
| DG12 — GPU fault depth | [DX12](../../engine/gpu/gpu-context-dx12/src/dx12_validation_capture.cpp) and [Vulkan](../../engine/gpu/gpu-context-vulkan/src/vulkan_validation_capture.cpp) already capture validation. DRED/device-fault extraction and explicit GPU-assisted mode wiring were not found by the targeted engine search. | DIAG.7b, DIAG.7c |
| DG13 — authored provenance | [CEIR diagnostics](../../engine/execution/ceir/include/crd/ceir/diagnostic.hpp) own copied messages with codes/locations; [executor](../../engine/execution/ceir/include/crd/ceir/exec.hpp) has step hooks. These do not establish complete runtime CHIR→CEIR→CKIR→native/GPU provenance after optimization/reload. | DIAG.8a, DIAG.8b |
| DG14 — captures and symbols | [CPROF](../../engine/foundation/perf/include/crd/perf/capture.hpp) is a versioned snapshot format with pinned POD layouts. Crash dumps, native symbols, program generations and profiler captures lack a demonstrated unified bundle/consumer path. | DIAG.5d, DIAG.6c |
| DG15 — reproducibility | Capturing a frame or corpus input does not capture scheduler choices, clocks, random streams, external completion order or exact runtime assets. | DIAG.9a, DIAG.9b, DIAG.9c |
| DG16 — testing the tests | Four ingestion fuzzers are useful but not allocator-state, capture-reader, command, reload or fault-recovery qualification. Non-reproduced artifacts in the existing audit are not established root causes. | DIAG.10a, DIAG.10b, DIAG.10c |
| DG17 — host integration | [Application](../../engine/foundation/app/src/application.cpp) installs crash handling; [sandbox](../../sandbox/src/main.cpp) separately initializes perf and jobs tracking. A reusable diagnostics lifecycle for every host is not yet demonstrated. | DIAG.11a |
| DG18 — portability and cost | Current instruments are not a qualification claim for macOS/Metal, browser/WASM, ARM64, unattended machines or production overhead. | DIAG.11b, DIAG.11c, MAC.DIAG, WEB.DIAG |

## Memory diagnostics require allocator cooperation

ASan detects instrumented invalid accesses, including many bounds/lifetime errors. User-defined containers need
annotations to distinguish live payload from reserved capacity. MSVC stack-use-after-return requires a separate
compile option and runtime switch; enabling only the environment setting cannot add missing code generation.[^1][^2]

Cerid should keep its allocators. An instrumented allocator path must describe each logical allocation, not only the
large backing allocation. Redzones, poisoned freed slots and bounded quarantine improve detection before reuse;
explicit slot state catches duplicate frees. Free-list metadata stored inside freed payload makes blanket poisoning
incorrect: move diagnostic bookkeeping out of payload or expose only the exact allocator metadata while manipulating
it. Alignment and ASan shadow granularity require tests. No policy can distinguish arbitrary stale raw pointers after
the same address becomes a valid new allocation; handles/generations and explicit borrows cover that boundary.

Initialization is a separate concern. MSan requires a coherently instrumented dependency closure; partial builds can
mislead. Valgrind Memcheck exposes pool APIs for custom allocators and is a useful scoped Linux alternative. Leak
checks must distinguish live arenas from unreachable allocations and intentional retained caches. These routes
should be selected by demonstrated capability, not added as empty configuration names.[^3][^4]

Low-overhead guarded sampling is valuable for field-only failures. GWP-ASan's production experience supports sampled
allocations with preserved allocation/free stacks, but a sample necessarily misses other allocations and some sizes.
It is a complementary diagnostic mode, not replacement for full debug tests or a reason to replace Cerid allocators.[^5]

## Parallel correctness and Heisenbugs

TSan observes data races, with substantial execution/memory overhead and incomplete coverage of uninstrumented
dependencies. Its fiber API can introduce a happens-before relation or explicitly avoid one. Cerid must document
which ordering is real queue publication and which is instrumentation, and seed races that must remain visible
through migration. Suppressions or instrument-only synchronization cannot serve as correctness fixes.[^6][^7]

Controlled scheduling makes rare task interleavings reproducible. Microsoft's Coyote demonstrates the technique in
.NET; it is research guidance, not a drop-in C++ scheduler. For C++ atomic algorithms, bounded GenMC models exercise
weak-memory behaviours missed by a serial interleaving runner. Model bounds, abstraction and memory model must be
reported; a passing extracted model is not a proof of the whole engine.[^8][^9]

Cerid's execution tests should record causal task identities, queue publication, wait edges, fiber migrations,
completion/cancellation and owner retirement. Add real multicore stress and task-level controlled interleavings.
Hang diagnosis needs progress-sensitive watchdogs, wait graphs and stopped-fiber context, distinguishing a deadlock
from a long offline render. An external watchdog can still capture evidence when the job pool or logger stalls.

Reverse execution is an optional escalation. rr provides repeatable Linux process recordings but emulates a single
core and cannot cover arbitrary external shared memory or drivers. Windows TTD supplies reverse process debugging
with material storage/recording overhead and can have trace gaps. Neither is universal GPU replay. Their platform
eligibility and a verified sample trace belong in runbooks; never disable machine security to make a tool work.[^10][^11]

## Reliable failure capture

Microsoft recommends calling MiniDumpWriteDump from a separate process where possible because a damaged process
can deadlock during dumping. Crashpad is an established reference for a client/handler split. Cerid should own a small
public capture interface and evaluate a maintained backend or a narrowly scoped OS implementation; uploading is a
separate explicit capability, disabled by default.[^12][^13]

Linux crash handlers must respect async-signal safety. Formatting/allocating/locking in a compromised context is
unsafe; even backtrace helpers may load libgcc and allocate on first use. Preinstalled per-thread alternate stacks,
preopened emergency output and minimal fixed records reduce dependence on damaged state. Symbolization and bundle
assembly belong in a healthy process or after restart. Fatal records must preserve original exception/signal context,
actual write results and truncation, with collision-safe names and bounded fallback behaviour.[^14][^15]

Crashes are only one failure type. Assertion recursion, out-of-memory, stack exhaustion, process hangs, device loss,
failed checkpoint writes and sudden termination require different paths. The recorder must survive disabled normal
logging, full queues, unavailable directories and logger locks. An operating system kill can leave only previously
persisted records; the bundle must label that evidence boundary instead of claiming complete capture.

## Profiling and causality

Keep crd-perf as the engine-facing service and make export interoperable. Perfetto provides tracks, counters and
cross-track flows; separate clock snapshots permit CPU/GPU time alignment. A flow is a causal association, not
evidence that two clocks are directly comparable. GPU queue duration, queue wait, transfer and CPU submission time
must remain distinct.[^16][^17]

Sampling complements explicit scopes by finding work that was never instrumented. Windows WPR and Linux perf can
provide external CPU/system evidence; fiber stacks, inlining and omitted frame pointers still require qualification.
Tracy is a useful feature/performance comparator, not an automatic engine dependency. Distributed OpenTelemetry
context can later connect command/request traces across hosts, without exporting every frame event or turning an
incoming trace identifier into authorization.[^18][^19][^20][^21]

Diagnostic performance needs its own board: disabled cost, basic recorder cost, full trace cost, allocation sampling,
CPU p50/p95/p99 and memory/I/O pressure. The hot event path should use preallocated bounded storage, immutable
identities and explicit drop counts; no heap allocation, network, symbolization or global mutex. Scope timing alone
cannot establish absence of observer effects. Compare the same workload with instrumentation off/basic/full and
use external sampling to challenge conclusions.

## GPU and authored-program debugging

DX12 GPU-based validation finds descriptor/state problems during shader execution; DRED adds breadcrumbs and
page-fault information where supported. Breadcrumbs locate a region of progress, not necessarily the exact failing
operation. DRED must be configured before device creation; instrumentation can alter timing.[^22][^23]

Vulkan synchronization validation addresses access hazards; GPU-assisted validation covers additional shader-side
problems with capability and descriptor constraints. Device-fault extensions provide another optional fault source.
All must report actual activation and unsupported reasons. A validation-free run on a software adapter does not
qualify every driver/GPU.[^24][^25][^26]

Attach Cerid-owned object IDs, asset content hashes and generation IDs to queue/submission/pass/resource names.
Preserve the lowering relation from source node or CHIR location through CEIR operations and CKIR nodes to generated
instructions where the backend can expose it. Optimizations may fuse/remove nodes; attribution must be many-to-many
or explicitly unavailable. Existing diagnostic codes and owned source strings should be reused. Native DLL unload
requires symbol retention and lifetime coordination; actual future JIT backends need their debugger registration
mechanism, not an assumption that generated machine code automatically symbolizes.[^27]

## Portability, test strength and security

Metal provides shader validation and richer command-buffer failure information. Browser WebGPU instead exposes
asynchronous error scopes/device-loss handling within a sandbox, and Emscripten offers specific sanitizer support.
Their limits differ from native minidumps and x64 fibers. Define portable schemas now, then qualify actual macOS and
browser deployment in MAC.DIAG/WEB.DIAG. Browser report export must tolerate worker termination, memory growth,
storage quota and tab suspension without requiring native filesystem or debugger access.[^28][^29][^30]

Fuzzers should be bounded and deterministic from input/seed, retain minimized regressions and exercise the production
entry point. Add malformed diagnostic-bundle readers as well as stateful allocator/reload/command inputs. Coverage
is measured on instrumented code only; record branches and uncovered error paths, and prove assertions detect seeded
faults. A high line percentage alone is not an oracle.[^31][^32]

Static lock/lifetime annotations can catch mistakes earlier but have documented inference limitations and vary by
compiler version. Real-time checks can catch forbidden allocating/blocking calls, but the current RealtimeSanitizer
documentation is not evidence that Cerid's pinned LLVM 20 provides it. Compile-probe and pin any additional diagnostic
compiler separately; retain runtime allocation/blocking sentinels as a portable contract.[^33][^34]

Captures may contain private assets, keys, source paths and raw memory. Use local-only defaults, owner-selected export,
redaction of structured fields, size/retention quotas, safe filenames and bounded parsing. A raw dump cannot honestly
be declared sanitized by masking a few JSON fields. Public summaries and restricted raw evidence are separate
artifacts. Agents get typed inspection commands with authorization, cancellation and budgets; arbitrary process
memory, code execution, telemetry upload and fault injection are separate privileges.

## Source register

Primary sources accessed 2026-09-14. Living documentation describes upstream behaviour, not a qualification of
Cerid's pinned versions; each implementation slice must record actual versions and recheck material differences.
The main WebGPU specification exceeded the retrieval limit; the community explainer was readable and is explicitly
non-normative. Read the applicable normative sections against the chosen browser/provider during WEB.DIAG.

[^1]: LLVM, [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html), living documentation: memory error classes and container annotations.
[^2]: Microsoft, [MSVC stack-use-after-return](https://learn.microsoft.com/en-us/cpp/sanitizers/error-stack-use-after-return?view=msvc-170), compiler/runtime requirements.
[^3]: LLVM, [MemorySanitizer](https://clang.llvm.org/docs/MemorySanitizer.html), instrumentation closure and initialization checks.
[^4]: Valgrind, [Memcheck manual](https://valgrind.org/docs/manual/mc-manual.html), memory pools/client requests.
[^5]: Google researchers, [GWP-ASan: Sampling-Based Detection of Memory-Safety Bugs in Production](https://arxiv.org/abs/2311.09394), 2023 preprint / ICSE 2024; [Chromium implementation limits](https://chromium.googlesource.com/chromium/src.git/+/HEAD/docs/gwp_asan.md).
[^6]: LLVM, [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html), coverage, overhead and platform limits.
[^7]: LLVM, [TSan interface](https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/tsan_interface.h), fiber context and no-sync flag.
[^8]: Microsoft Research, [Coyote](https://www.microsoft.com/en-us/research/blog/coyote-making-it-easier-for-developers-to-build-reliable-asynchronous-software/), 2020, controlled concurrency technique in .NET.
[^9]: MPI-SWS, [GenMC](https://plv.mpi-sws.org/genmc/) and [CAV 2021 paper](https://plv.mpi-sws.org/genmc/cav21-paper.pdf), bounded weak-memory model checking.
[^10]: rr maintainers, [rr](https://rr-project.org/), repeatable Linux debugging and single-core/external-memory limits.
[^11]: Microsoft, [TTD command-line utility](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/time-travel-debugging-ttd-exe-command-line-util) and [troubleshooting](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/time-travel-debugging-troubleshooting), overhead, storage and missing trace data.
[^12]: Microsoft, [MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump), separate process and DbgHelp serialization requirements.
[^13]: Chromium, [Crashpad overview design](https://chromium.googlesource.com/crashpad/crashpad/+/main/doc/overview_design.md), client/handler architecture.
[^14]: Linux man-pages project, [signal-safety(7)](https://www.man7.org/linux/man-pages/man7/signal-safety.7.html), async-signal safety.
[^15]: Linux man-pages project, [backtrace(3)](https://www.man7.org/linux/man-pages/man3/backtrace.3.html), first-use allocation and unwinding limitations.
[^16]: Perfetto, [Track events](https://perfetto.dev/docs/instrumentation/track-events), tracks, flows and counters.
[^17]: Perfetto, [Clock synchronization](https://perfetto.dev/docs/concepts/clock-sync), separate clock domains and snapshots.
[^18]: Microsoft, [Windows Performance Recorder](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder), ETW recording.
[^19]: Linux perf maintainers, [perf-record(1)](https://www.man7.org/linux/man-pages/man1/perf-record.1.html), sampled call graphs and unwind methods.
[^20]: Tracy maintainers, [Tracy](https://github.com/wolfpld/tracy), profiler feature comparator.
[^21]: OpenTelemetry, [Tracing API](https://opentelemetry.io/docs/specs/otel/trace/api/), context/span/links model.
[^22]: Microsoft, [D3D12 GPU-based validation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation), shader-side diagnostics and overhead.
[^23]: Microsoft, [DRED](https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred), breadcrumbs/page faults, timing and hardware caveats.
[^24]: LunarG, [Synchronization validation](https://vulkan.lunarg.com/doc/view/latest/windows/synchronization_usage.html), access hazards and enablement.
[^25]: LunarG, [GPU-assisted validation, SDK 1.4.304](https://vulkan.lunarg.com/doc/view/1.4.304.0/windows/gpu_validation.html), version-specific capability limits; refresh for the selected SDK.
[^26]: Khronos, [VK_EXT_device_fault](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html), optional device-fault reporting.
[^27]: LLVM, [Debugging JIT-ed code](https://llvm.org/docs/DebuggingJITedCode.html), generated-code debugger integration; not a claim Cerid uses this JIT.
[^28]: Apple, [Metal shader validation](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage) and [command-buffer debugging](https://developer.apple.com/documentation/metal/command-buffer-debugging).
[^29]: GPU for the Web Community Group, [WebGPU explainer](https://gpuweb.github.io/gpuweb/explainer/), 2026-09-01 community draft, non-normative error/device-loss design.
[^30]: Emscripten, [Debugging with sanitizers](https://emscripten.org/docs/debugging/Sanitizers.html), WASM-specific instrument support.
[^31]: LLVM, [libFuzzer](https://llvm.org/docs/LibFuzzer.html), deterministic harnesses, corpora and minimization.
[^32]: LLVM, [Source-based coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html), instrumented denominator and branch/region data.
[^33]: Clang, [Thread Safety Analysis](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html), annotations and limitations.
[^34]: LLVM, [RealtimeSanitizer](https://clang.llvm.org/docs/RealtimeSanitizer.html), current upstream nonblocking instrumentation; pinned-tool availability must be tested.
