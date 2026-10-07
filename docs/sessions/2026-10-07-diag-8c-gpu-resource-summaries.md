# DIAG.8c GPU resource summaries, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8c](../ROADMAP.md#slice-diag.8c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [program provenance](2026-10-07-diag-8c-program-provenance-command.md).

## Goal

The third DIAG.8c batch adds the GPU resource summary the row lists as remaining local work: what the GPU side of a
process holds, through the same typed, bounded service and the same transports (`ceridc diag` and the MCP `diag`
tool) as the other commands.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- The row said the summaries would be "registered by the GPU context". crd-gpu-context cannot do that: it may not link
  crd-perf (its module DEPENDS are containers, core, render-asset-core), and crd-perf may not link it. The one module
  that names both is **crd-perf-gpu-bridge** (DIAG.6b(i)), so the command lives there, the same move
  `program.provenance` made from crd-ceir to crd-ceir-cook. The bridge now declares containers (it uses the string
  view directly) and its tests declare asset-io (the JSON reader).
- GPU evidence that exists today: the process-wide `identity_registry()` (DIAG.7a) counts live Resource, Program and
  Pass identities across every backend (one index space, mutex-guarded); `IGpuContext` reports its backend, adapter
  name, validity and `validation_activation()`; `IFrameGraph` reports its last build's transient bytes after and
  before aliasing and budget refusal, and its last execute's barrier, submit, pass, async-pass and present counts and
  whether it has GPU timing.
- Evidence that does not exist: neither the Vulkan nor the DX12 backend queries device heap usage or budget
  (no `VK_EXT_memory_budget` read, no `QueryVideoMemoryInfo`); the registry stores no per-resource size. No vendor
  query was added just to fill the item: heap usage is answered `unavailable` with that reason.
- ceridc already links crd-gpu-context and crd-gpu-context-vulkan transitively (through crd-ceir-gpu and the asset
  cooker), so linking the bridge adds no new layer to it.

## What changed

- **`crd/perf/gpu/gpu_resources_diag.hpp` / `gpu_resources_diag.cpp` (crd-perf-gpu-bridge):**
  `register_gpu_resources(service, command)` registers `gpu.resources` (`read`, no path). The host owns the
  `GpuResourcesCommand`: `add_context` / `remove_context` and `add_frame_graph(graph, label)` / `remove_frame_graph`
  (eight of each; a duplicate or a full list is refused), and a `runs` counter as the evidence that refusals happen
  before any work. The lists take the command's own lock, so a host may change them while another thread calls the
  service.
  - Summary: `live_resources`, `live_programs`, `live_passes`, `contexts`, `frame_graphs`, `heap_usage:"unavailable"`.
  - Items, in order: one `identities` item per kind (`res`, `prog`, `pass`; `scope:"process"`); per registered context
    a `context` item (index, backend, adapter, valid, and `core`, `synchronization`, `gpu_assisted`, each `active` or
    the reason it is off) followed by a `heap` item (`status:"unavailable"` with the reason); per registered frame
    graph a `frame-graph` item (index, label, `transient_bytes`, `transient_logical_bytes`, `budget_exceeded`,
    `barriers`, `submits`, `passes`, `async_passes`, `presents`, `gpu_timing`). With no context or no frame graph
    registered, one item of that kind says `unavailable` and why.
  - Threads: context reads are immutable after creation and the registry locks, so both are safe from any thread.
    Frame-graph counters are not synchronized with build and execute, so the header requires the host to register a
    graph only when it calls the service from the thread that drives the graph.
- **ceridc:** `bind_diag_commands` also registers `gpu.resources` with an empty context list (ceridc opens no GPU
  context), so the CLI verb and the MCP server list and answer it identically.

## Tests

`tests/gpu/perf-gpu-bridge/test_gpu_resources_diag.cpp` (2 cases, `[perf][gpu][diag]`, 158 assertions; device-free;
responses parsed with the JSON reader):

- **Evidence:** a fake Vulkan context (core active, synchronization requested but `extension-absent`, GPU-assisted not
  requested), a fake DX12 context and a fake frame graph with fixed counters. The test reads the registry's live
  counts, mints two resources, a program and a pass, and expects exactly those deltas in the summary and the
  `identities` items; the context, heap and frame-graph items carry the fakes' values in registration order. After
  retiring the identities, removing the first context and the graph (whose counters are then changed, to show a
  removed graph is never read), the counts return to the baseline, the remaining context is index 0, and the
  frame-graph item is `unavailable`. With no context, one `unavailable` context item and no heap item.
- **Refusals and pages:** a `record`-only grant is refused `unauthorized`; oversized page items, a path, another
  schema and a raised cancel flag are refused with `runs` and the service's handler count at 0; a duplicate
  registration is refused. Pages of two are cut from one snapshot (the same cursor gives the same bytes, one run), and
  another command's snapshot makes the cursor stale. The context list refuses a ninth context.

`tests/gpu/gpu-context-vulkan/test_vulkan_gpu_resources_diag.cpp` (1 case, `[gpu-context][vulkan][frame-graph][diag]`;
skips without a graphics-capable Vulkan device with shader objects): a real headless context with core validation and
its raster context. The context item equals the context's own adapter name and activation; the live resource count
equals the registry read directly, rises by exactly one for one storage buffer and returns after it is destroyed.
A real frame graph with two equal transients written by two passes is built and executed: the item's transient bytes
are positive, half the logical bytes (the two share one slot) and equal the graph's own counter; one submit, two
passes, the graph's barrier count. Validation reports no error.

`tests/tools/ceridc/test_ceridc_diag.cpp` (the existing cases, extended): the parity script gains a `gpu.resources`
snapshot, its next page and a refused path, byte-equal across the native call, the verb and the in-process MCP tool
(6 handler runs on each); the real binary's CLI answers `gpu.resources` byte-equal to a native service.

## Teeth (win-debug)

Each break was applied, the target rebuilt and the cases run. Every one failed; the source was then restored (mtime
bumped), the target rebuilt and the suites passed again (`crd-perf-gpu-bridge-tests` 14 cases, 215 assertions;
`crd-ceridc-tests` 8 cases, 334 assertions).

| Break | Result |
| --- | --- |
| Live resource count not read from the registry (reported 0) | bridge: 1 of 2 cases |
| No `unavailable` item when no context is registered | bridge: 1 of 2 cases |
| Registered contexts ignored | bridge: both cases |
| `transient_bytes` reports the logical bytes | bridge: 1 of 2 cases |
| `remove_context` returns true without removing | bridge: 1 of 2 cases |
| ceridc does not bind `gpu.resources` | ceridc: 2 assertions (the real binary's CLI answer) |

## Evidence

All on the final sources (the last source edit before the lanes was a rewrap of one test declaration):

- **win-debug:** the whole tree builds. `crd-perf-gpu-bridge-tests` 14 cases (215 assertions), `crd-ceridc-tests`
  8 (334, with `CRD_CERIDC_EXE` as CTest sets it), and the full `crd-gpu-context-vulkan-tests` on this machine's GPU,
  302 cases (7,307 assertions; the new case 64). The 8 repository guard CTests pass, and CTest runs the new cases
  and the ceridc cases in parallel.
- **win-shipping, win-clang-cl-shipping, win-asan:** each lane reconfigured (new sources) and built
  `crd-perf-gpu-bridge-tests`, `ceridc`, `crd-ceridc-tests` and `crd-gpu-context-vulkan-tests`; the clang-cl links
  were clean on the first attempt. The bridge suite passes 2 cases (158 assertions) on the two shipping lanes, where
  profiling is compiled out and the DIAG.6b(i) cases with it, and 14 (215) on win-asan; `crd-ceridc-tests` passes 8
  (334) on all three; the Vulkan `diag gpu.resources` case passes (64 assertions) on all three on this GPU. win-asan
  ran inside vcvars with no ASan report.
- **WSL (lavapipe):** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build the four targets and pass the
  bridge suite (14 cases, 215), `crd-ceridc-tests` (8, 334) and the Vulkan case (64) with no ASan, UBSan or TSan
  report in Cerid code. The first TSan run of the Vulkan case was made without `TSAN_OPTIONS` and reported one race
  inside the Khronos validation layer (its queue-retire thread against `DestroyFence` in the frame graph's
  destructor); that is the layer race the repository's suppression file already names for that library. With the
  hosted lane's `TSAN_OPTIONS` (the committed suppressions) the case passed three times with no report.
- **Checks:** strict tidy is clean on the 5 changed C++ sources (the new header through its source). clang-format
  `--dry-run` reports only the repository's hand alignment, include grouping and one-line `case` style; the one
  over-long line was rewrapped by hand. The Allman check, check-master-plan and check-repository pass.

## Hosted CI must later show

This batch does not move the row to Needs CI (local clauses remain). When the row is complete, its hosted run must
also show `crd-perf-gpu-bridge-tests` (14 cases with profiling, 2 with it compiled out) and `crd-ceridc-tests` (8,
including the real binary's `gpu.resources` answer) green on all six lanes, `crd-gpu-context-vulkan-tests`'
`diag gpu.resources` case passing on the lavapipe Linux lanes (it skips on the hosted Windows lanes), and `ceridc`
building on every lane with its new crd-perf-gpu-bridge link.

## Remaining (local) on DIAG.8c

- Replay preparation: what a replay of a bundle or run needs and which inputs are missing; until DIAG.9a records
  inputs it can only state the precise reason.
- `ceridc inspect` as an MCP tool under a declared authority class (the DIAG.8b hand-off).
- A GUI consumer of the same service.
- Not tested: the snapshot item cap (`kDiagMaxSnapshotItems`). Not claimed: the synchronous MCP stdio loop does not
  process `notifications/cancelled`. Not provided: device heap usage (no backend queries it).
