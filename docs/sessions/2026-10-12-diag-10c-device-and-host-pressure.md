# DIAG.10c device and host pressure, the driver-call mode witness and fault attribution, 2026-10-12

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c workloads](2026-10-11-diag-10c-workloads.md).

## Goal

DIAG.10c's clause "record accessible driver/device-reset and host-pressure evidence; do not label a hardware/OS failure
an engine bug without a discriminating reproduction", with the GPU side of the queue/VRAM pressure clause (the OS view
of device memory and the frame graph's transient budget) and the numerical-mode probe around GPU driver calls. GPU
queue depth and the native owners in the lifetime ledger (the window, the WASAPI device's COM objects and event, native
devices, queues, fences and swapchains) are the next slice: they belong with the shutdown of those owners. Committed
locally, not pushed.

## What landed

| Piece | Where | What it does |
| --- | --- | --- |
| Host memory reading | `engine/foundation/core/include/crd/core/host_pressure.hpp`, `src/host_pressure.cpp` | physical, commit (bounded only where enforced) and this process's resident set with its peak, as `BudgetReading`s; unread with a reason, never zero |
| Device memory reading | `crd/gpu/device_pressure.hpp`, `IGpuContext::device_memory()`; DX12 `dx12_device_memory.{hpp,cpp}`, Vulkan `vulkan_device_memory.{hpp,cpp}` | DXGI `QueryVideoMemoryInfo` (local, non-local) and `VK_EXT_memory_budget` (device-local heaps, the others), per process and adapter; an absent segment stays unread |
| Transient budget reading | `IFrameGraph::transient_pressure()` on both backends | bound, footprint, high water (a refused build's included), one refusal per refused build |
| `gpu.resources` | `engine/gpu/perf-gpu-bridge/src/gpu_resources_diag.cpp` | each context's device memory (budget, usage, level per segment) and each graph's transient budget; `device_memory_read` replaces `heap_usage: "unavailable"` |
| Driver-call mode witness | `crd/gpu/driver_witness.hpp`, `src/driver_witness.cpp` | CreateInstance, CreateDevice, CompileShader, Submit, Present: a mode a call leaves is put back, counted per kind, first offender kept |
| Loss pressure | `dx12_record_removal`, the Vulkan seam's first-loss path | host and device readings with the loss's own sequence (`dx12_last_loss_pressure`, `vk_last_loss_pressure`) |
| Attribution | `attribute_device_fault` | Injected, EngineDefect (only reproduced on another configuration), PressureCoincident, NotReproduced, Undiscriminated |

Decisions:
- The device reading is the OS's per-process, per-adapter view, so it is on `IGpuContext` (the device foundation) and
  covers memory any device of the process allocated on that adapter. A DX12 context holds its own device; DXGI answers
  for the adapter, which is what an attribution needs.
- A zero budget with zero use is an absent segment, not a full one. WARP answers 0/0 for its non-local segment group;
  read as a bounded reading it would be Full and every WARP loss would read as pressure-coincident.
- `VK_EXT_memory_budget` is enabled whenever the device advertises it; it is a query-only extension with no feature
  bit.
- The witness never judges the caller's mode: the declared mode is the jobs boundary's business. It only guarantees a
  driver or compiler cannot change the mode behind the engine's back and reports when one tried.
- Attribution is the strongest claim the evidence allows, never a guess about hardware. A reproduction elsewhere
  outranks pressure; a same-adapter reproduction does not exclude the adapter.

## Defect found and fixed: a second build of one recording

The transient-budget case first built, was refused, and built again on the same recording. On DX12 the next test in the
program (`DX12 authored fullscreen clears survive cooking and reach uncovered pixels`) then failed at
`create_dx12_gpu_context` under its validation capture; run alone it passed, and it failed only after this case. The
second build had placed the transients again over the first build's: it overwrote the wrappers that hold the placed
resources, so those resources, their heaps and the device stayed alive, and enabling the debug layer with a live
device made the next device creation fail. The new device reading measured it directly: with 2048 x 2048 transients,
DXGI usage stayed 32 MiB up after the graph was destroyed (one footprint per extra build). On Vulkan the transient images
are created when declared and bound in the build, so a second build bound memory to images already bound; the RTX
budget saw 16 MiB stay in use, and the footprint doubled.

Considered and rejected: releasing the previous build's transients at the top of `build()`. It fixed DX12, but on
Vulkan it destroyed the declaration-time images and the first build then crashed (SIGSEGV in the case), and Vulkan
cannot rebind memory to a bound image, so the two backends would have had different `build()` contracts. Fixed instead
on both backends: a frame graph builds once per recording. Once a build has placed transients (whether it then
succeeded or the budget refused it) another `build()` without `reset()` is refused and logged; it is not a budget
refusal. The recovery from a budget refusal is reset, record again, build. The contract is on `IFrameGraph::build()`.
The engine's four `build()` callers (frame-cook's runtime, ceir-gpu's render materialization, render-graph, the scene
renderer) build once per recording; their suites pass (below).

## llvmpipe's memory budget

Mesa's llvmpipe (the hosted Linux lanes' Vulkan device; LLVM 20.1.2 on the WSL reference host) advertises
`VK_EXT_memory_budget`, but the heap usage it reports does not follow the process's allocations: 1146138624 bytes before,
during and after a 256 MiB `vkAllocateMemory` and its free (other runs: 999067648 and 1209688064, each constant within the
run). Its reading is the driver's estimate. The memory case checks the reading's shape there and prints the observed
values with `WARN` instead of holding usage to the allocation; on every other driver usage must follow it. The leak
check after the frame graph dies is vacuous on llvmpipe for the same reason; the refusal's return value is the check
that bites there.

## Tests

| Program | New cases (`[diag10c]`) | What they hold the code to |
| --- | --- | --- |
| crd-core-tests | 2 | physical bound equals `GetPerformanceInfo`/`sysinfo`; commit enforced on Windows and only in strict overcommit on Linux (read from /proc by the test); resident follows 64 MiB the test maps and touches through the OS |
| crd-gpu-context-tests | 6 | attribution, every rule by hand; worst level ignores an unenforced commit limit and unread segments; loss pressure carries both readings; names; the witness restores, counts, keeps the first divergence, returns the call's value |
| crd-gpu-context-dx12-tests | 5 | DXGI usage follows a committed 256 MiB buffer on the default adapter and on WARP; transient budget reading, refusal and the second-build refusal with the DXGI leak check; witness counts for device creation, dxc, submission and present; recorded and injected removals carry the pressure with their sequence and attribute to Injected |
| crd-gpu-context-vulkan-tests | 4 | the reading exists exactly when the device advertises the extension (asked by the test); usage follows a 256 MiB allocation (not on llvmpipe, as above); transient budget and second-build refusal with the budget leak check; witness counts for instance and device creation, shaderc and submission; injected loss pressure and attribution |
| crd-perf-gpu-bridge-tests | (updated) | `gpu.resources` device memory and transient fields from fakes, including unavailable segments and a graph that keeps no counts |
| crd-gpu-context-vulkan-tests | (updated) | the real-device `gpu.resources` case: the heap item matches the context's own reading; the graph's transient fields |

## Teeth

Fourteen deliberate breaks, each failing a case; each restored from a byte-exact backup with its mtime bumped, the
lane rebuilt and the cases green again.

| Break | Failed |
| --- | --- |
| DX12 rebuild leak (the superseded release removed, before the refusal design) | the DXGI usage check after the graph dies (+32 MiB) |
| DX12 refusal flag never set | `CHECK_FALSE(tight->build())`, DXGI usage +16 MiB |
| Vulkan refusal flag never set | budget-refusal flags and counts, footprint doubled, VK budget usage +16 MiB |
| The witness does not put the mode back | the witness case's mode checks |
| A same-adapter reproduction counted as an engine defect | the attribution cases |
| WARP's 0/0 segment read as a bounded reading | `capacity > 0` on WARP |
| DX12 frame graph does not count refusals | the refusal counts and the Exhausted verdict |
| Vulkan never enables `VK_EXT_memory_budget` | the memory case's source check |
| DX12 removal does not store its pressure | both DX12 removal cases |
| Windows physical bound off by a page | the cross-check |
| D3D12CreateDevice counted as a present | the DX12 CreateDevice witness count |
| `gpu.resources` ignores the context's reading | the device-free command case |
| Vulkan seam does not take loss pressure | the Vulkan loss case |
| Linux /proc kB read as 1000 bytes (linux-gcc-debug) | the cross-check against `sysinfo` |

The first round's teeth edits were made with a PowerShell `Get-Content`/`Set-Content` round trip, which re-encoded the
file's UTF-8 comment glyphs and added a BOM; the file was restored byte for byte from the backup and every later tooth
used a binary replace.

## Verification

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | whole tree (`all`) | crd-core-tests, crd-gpu-context-tests, crd-perf-gpu-bridge-tests; crd-gpu-context-dx12-tests whole (209 cases); crd-gpu-context-vulkan-tests whole on the RTX (307); crd-ceir-gpu-dx12-tests (47), crd-ceir-gpu-vulkan-tests (60); crd-scene-render-tests through CTest (114, one pre-existing skip) |
| win-shipping | touched targets | the four device-free and DX12 programs whole; Vulkan `[diag10c]`, `gpu.resources`, REN-38-B6 |
| win-clang-cl-shipping | touched targets (linked first time) | the same |
| win-asan (inside vcvars) | touched targets | the same; the only reports are the crash-capture specimen's deliberate faults |
| linux-gcc-debug | whole tree | crd-core-tests, crd-gpu-context-tests, crd-perf-gpu-bridge-tests; crd-gpu-context-vulkan-tests whole on llvmpipe (306 passed, one pre-existing skip) |
| linux-gcc-asan | touched targets | the same four programs (Vulkan filtered); `[diag10c]` x3; only the crash-capture specimen reports |
| linux-clang-tsan (`setarch -R`, the preset's TSAN_OPTIONS) | touched targets | the same; `[diag10c]` x10; no TSan report |

Coverage (linux-clang-coverage, one run on the final engine source): the registry gained the scope group "host and
device pressure evidence and the driver-call mode witness" (`host_pressure.cpp`, `device_pressure.cpp`,
`driver_witness.cpp`), the program crd-gpu-context-tests (now eleven) and the outcome set FaultAttribution. Every
selected CTest passed; all 82 scope files were instrumented; the new group ran 147 of 168 regions and 115 of 150
branches; FaultAttribution 6 of 6; verdict PASS. Totals: regions 10748/12026, branches 7159/9048, functions 1195/1247,
lines 19687/22525. The 1.7 GB of profiles are deleted. The backends' device-memory and loss-pressure files are not in
the scope: their programs need a device, and the coverage lane runs device-free programs only.

Checks: strict tidy (LLVM 20.1.8) clean on all 31 changed C++ files (one finding fixed: a local constant's name);
clang-format differs only in the repository's hand alignment, case-label style, leading-operator wrapping and include
grouping; four lines over 120 columns were rewrapped; the Allman check passes; the guard CTests and crd-diag-coverage,
crd-diag-seeded-faults, crd-build-profiles, crd-ci-tiers, crd-developer-workflow, crd-repository-tools,
crd-master-plan and crd-repository-hygiene pass; `diag-coverage.py --validate` and `test-diag-coverage.py` (29) pass;
check-master-plan and check-repository pass. A direct run of crd-scene-render-tests from the repository root (outside
CTest, so without its asset environment) failed 30 cases and left 18 `.crdr` files at the root; they were deleted and
the CTest run above is the evidence. The full win-debug CTest was not run. No script or workflow changed.

## Follow-up

- GPU queue depth and the native owners in the lifetime ledger (the row's remaining local clause).
- The hang watchdog could classify a stall from the host, device and transient readings.
- `retire_transients_to` on Vulkan hands the transient buffer's native handle to the retire list but not its
  `VulkanTransientBuffer` wrapper (free_transients deletes it); check whether a reset with work in flight leaks the
  wrapper.
- llvmpipe's `VK_EXT_memory_budget` usage: if a later Mesa tracks allocations, the memory case's llvmpipe branch can
  hold it to the allocation too.
