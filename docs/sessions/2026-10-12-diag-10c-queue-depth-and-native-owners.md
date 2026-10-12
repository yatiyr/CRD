# DIAG.10c GPU queue depth, the native owners in the lifetime ledger and their shutdown, 2026-10-12

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c device and host pressure](2026-10-12-diag-10c-device-and-host-pressure.md).

## Goal

DIAG.10c's last local clause: GPU queue depth (an in-flight count at the completion seams), the native owners the
lifetime ledger did not yet record (the window, the WASAPI device's COM objects and event, native devices, queues,
fences and swapchains without a Cerid identity) and the shutdown of those owners. After it the row has only its
hosted run left. Committed locally, not pushed.

## What landed

| Piece | Where | What it does |
| --- | --- | --- |
| Queue-depth tracker | `engine/gpu/gpu-context/include/crd/gpu/queue_pressure.hpp`, `src/queue_pressure.cpp` | device-free, keyed by opaque device, queue and timeline handles: a step is in flight from the seam that queued it until a seam observed it; a timeline observed through a value retires its step and every earlier step on its queue; loss, forget and a restarted timeline abandon; per-device counts and a process total |
| Seam reports | DX12 `dx12_execution.cpp` (`dx12_submit`, `dx12_signal`, `dx12_wait`, the removal paths), each DX12 context's destructor; Vulkan `vulkan_execution.cpp` (`vk_submit`, `vk_wait`, `vk_wait_complete`, both idle waits, the first loss, `vk_forget_device`) | every fence step, observation, idle wait and loss is reported |
| Device end on DX12 | `dx12_execution.cpp` (`DeviceEndWatch`) | a private-data interface attached to each device the first time a seam reports on it; D3D12 releases it when the device is destroyed, which ends that device's counts |
| Reading | `IGpuContext::queue_pressure()` (appended, default unread), DX12 and Vulkan contexts; `gpu.resources` | the device's counts as an unbounded `BudgetReading`, unread while lost or when a step went untracked; `queue_*` fields on each context item and `queue_depth_read` in the summary |
| Ledger classes | `crd/core/lifetime_ledger.hpp` | NativeGpuObject and OsObject (eight classes, which fill the u8 masks); `LifetimeHold` for an acquisition whose release is implicit |
| Native owners | DX12 device scope, compute, ray-tracing, work-graph and raster contexts, present surface; crd-kir-dx12 backend; Vulkan context, compute context, present surface, raster upload batches, frame graph; GLFW window; WASAPI device | each records what it creates and releases |

Decisions:
- The depth is the engine's own view (submitted and not yet observed), not the GPU's. Work the GPU finished that no
  seam looked at still counts; that is what a missing wait or an owner destroyed with work queued leaves behind, and it
  needs no driver query.
- A bare queue has no bound, so the reading is unbounded and its level is reported, never judged. The Vulkan frame
  graph's two-slot ring bounds its own depth at two; the reading does not claim that bound, the test checks it.
- Queue order makes a later timeline retire earlier steps on its queue, fence-less ones included. That is how a Vulkan
  one-shot submission (no fence, then a queue idle wait) and two fences on one DX12 queue (a context's and a present
  surface's) are both counted right.
- Keys are addresses, so a key can be reused. A Vulkan device is forgotten before `vkDestroyDevice`; a DX12 device has
  no destroy call, so a private-data interface tells the tracker when it is destroyed (see below); a fence created
  again at a reused address restarts at a value at or below one still pending and abandons it.
- Two ledger classes, not one: a native GPU object and an OS object leak for different reasons and are judged by
  different oracles. The masks are u8, so eight classes fill them; a ninth widens them first.
- Not recorded, by rule: Vulkan queues (retrieved, not created; so DX12 and Vulkan counts differ), semaphores, command
  pools, allocators and lists, descriptor heaps and pools, pipelines, query heaps, the Vulkan surface, the DXGI factory
  and COM references a call releases itself.

## Defects found and fixed

1. **WASAPI restart leaked the stopped session.** `AudioDevice::start()` after `stop()` obtained a second enumerator,
   endpoint, audio client, render client, mix format and event over the first set without releasing it, and
   initialized COM once more. The new restart case saw ten OS objects held after one restart instead of five (the
   `wasapi-restart-release` tooth). A start now releases a held session first.
2. **A DX12 device's queue counts outlived the device.** The first whole-program run of crd-gpu-context-dx12-tests
   failed three new cases: the default device read as lost before they submitted anything. Earlier validation cases
   drive `dx12_signal`/`dx12_wait` directly on devices of their own, make the device fail, and never forget the queue;
   when such a device was destroyed and a later device was created at the same address, it inherited the lost counts.
   The DX12 seam now attaches a `DeviceEndWatch` (an `IUnknown` set with `SetPrivateDataInterface`) the first time it
   reports a step or a loss on a device; D3D12 releases it when the device is destroyed, and its final release forgets
   the device. Attachment is serialized so two first submissions cannot replace each other's watch. A new case
   creates a WARP device, signals once through the seam, records a loss, releases every reference and requires fresh
   counts at that address; it fails without the watch (the `dx12-device-end-watch` tooth). The whole program then
   passes (215 cases).

3. **A simulated Vulkan loss let the teardown that followed destroy objects in-flight work still used.** The new
   lost-queue case passed on every lane but exited 1 under linux-gcc-asan: LeakSanitizer reported 120 bytes allocated
   on a lavapipe worker thread. A simulated loss (`vk_inject_next_result`, or the DIAG.9b device-loss fault) leaves a
   device that still runs what it was given before the injection, and from then on every seam wait answers without
   the driver, so the frame graph and raster context were destroyed while their frame was still running. After
   recording a simulated loss the seam now drains the real device once with a direct `vkDeviceWaitIdle`, so nothing it
   was given still runs, as after a real loss; a real loss is still never called again. Removing the drain brings the
   leak back (three of three runs, the `vk-simulated-loss-drain` tooth on linux-gcc-asan).

Also fixed in passing: the Vulkan instance, device and fence creations now leave the handle null when creation fails,
so a destructor never destroys an undefined handle, and the present ring and async fence creations are split from
their semaphores so each failure is attributed.

## Tests

| Program | New cases (`[diag10c]`) | What they hold the code to |
| --- | --- | --- |
| crd-gpu-context-tests | 8 | the tracker with fake handles, every count worked out by hand: a fence retires through its value; queue order retires earlier and fence-less steps on that queue only; two fences on one queue; restarted timelines abandon; a lost device abandons and reads unread until forgotten; forgetting a queue abandons and ends the counts with the last queue; a full table leaves a step untracked and the reading unread; the default reading |
| crd-core-tests | 1 | the two new class names, eight distinct mask bits filling a u8, and `LifetimeHold` (records once, a move transfers, a move-assignment releases what it held, destruction releases) |
| crd-gpu-context-dx12-tests | 6 | a compute context holds 3 native GPU objects and 1 OS object and leaves nothing in flight; the deferred frame graph keeps its frame in flight until the next reset; the present surface holds a swapchain, a fence and an event; the shutdown workload (3 cycles x 4 round trips, Clean, every cycle Balanced, 12 steps); an injected removal abandons the raster graph's pending frame on the shared device and reads lost; a destroyed device's counts end |
| crd-gpu-context-vulkan-tests | 5 (+1 updated) | instance and device held, a compute context's fence, two round trips with high water 1; the one-shot clear observed by its idle wait and the frame graph's ring at most two in flight (high water exactly 2) and drained by its teardown; the shutdown workload; an injected loss abandons the pending frames; the present surface's swapchain and two ring fences; `gpu.resources` on a real device carries the context's queue counts |
| crd-platform-tests | 2 | a window is one OS object, a move transfers it, a move-assignment destroys the first (FindWindow by title on Windows), and the USER-object count returns to its value before the platform context; shutdown cycles |
| crd-audio-tests | 1 | five OS objects and one thread from start; stopped keeps the session; three restarts hold one session, not two; destruction balances; without an endpoint the refused start balances |
| crd-kir-dx12-tests | 1 | the backend's device, queue, fence (3) and event (1), all released |
| crd-perf-gpu-bridge-tests | (updated) | `queue_*` fields and `queue_depth_read` from fakes, read and unread, and the summary after the read context is removed |

Devices without the capability do not skip: a Vulkan context, window or audio device that cannot start must leave the
ledger balanced, and the case reports with `WARN` which half did not run.

## Teeth

Twenty-two deliberate breaks, each applied with a binary replace, restored from a byte-exact copy with its mtime
bumped, and rebuilt; every one failed a case. Twenty-one ran on win-debug; the simulated-loss drain was broken on
linux-gcc-asan, the lane that observes it.

| Break | Failed |
| --- | --- |
| a timeline retires only its own step (no queue order) | the fence-less and two-fence tracker cases |
| a restarted timeline abandons nothing | the restart case |
| a lost device abandons nothing | the loss case |
| forgetting a queue abandons nothing | the forget case |
| `dx12_wait` does not report the observation | the DX12 compute, frame-graph and workload cases |
| an injected DX12 removal does not mark the device lost | the DX12 removal case |
| the DX12 compute queue not held / the device scope's reference not held | the compute context's count (3) |
| the DX12 compute context does not forget its queue | the compute and removal cases |
| `vk_wait_complete` does not report the observation | the Vulkan compute and workload cases |
| `vkQueueWaitIdle` not reported | the one-shot clear case |
| a Vulkan loss does not mark the device lost | the Vulkan loss case |
| the Vulkan instance or a present-ring fence not released | the context and present-surface windows |
| the window's move-assignment does not release | the window case |
| the WASAPI restart does not release the stopped session | the restart case (10 objects, not 5) |
| `gpu.resources` counts unread readings / writes the fields for an unread reading | the bridge case (the first survived the original case, whose two contexts made the count symmetric; the case now also checks the count after the read context is removed, and then it fails) |
| `LifetimeHold`'s move constructor keeps the source held | the core hold case (an unmatched release) |
| the kir-dx12 event not released | the kir-dx12 case |
| the device-end watch does not forget the device | the destroyed-device case |
| a simulated Vulkan loss does not drain the real device (linux-gcc-asan) | the lost-queue case exits 1 with a LeakSanitizer report from lavapipe |

## Verification

Every lane ran the final source (the touched programs whole unless noted; Vulkan "filtered" is the `[diag10c]`
cases, the `gpu.resources` cases and REN-38-B6).

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | whole tree (`all`), then the touched targets again after the last edits | crd-core-tests (75 cases), crd-gpu-context-tests (42), crd-platform-tests (54), crd-audio-tests (30), crd-perf-gpu-bridge-tests (14), crd-kir-dx12-tests (22), crd-gpu-context-dx12-tests whole (215), crd-gpu-context-vulkan-tests whole on the RTX 4070 Ti SUPER (312); crd-ceir-gpu-dx12-tests (47) and crd-ceir-gpu-vulkan-tests (60) |
| win-shipping | touched targets | the same programs (core 67, platform 54, bridge 2, dx12 215); Vulkan filtered (11) |
| win-clang-cl-shipping | touched targets, each linked first time | the same |
| win-asan (inside vcvars) | touched targets | the same as win-debug, Vulkan filtered; the only reports are the crash-capture specimens' deliberate faults |
| linux-gcc-debug | whole tree (301 steps) | core, gpu-context, bridge, platform, audio; crd-gpu-context-vulkan-tests whole on llvmpipe (312: 311 passed, one skip that predates this batch); `[diag10c]` once more |
| linux-gcc-asan | touched targets | the same, Vulkan filtered; `[diag10c]` (core `[lifetime]`) x3; the only reports are the crash-capture specimens' (12, in crd-core-tests) |
| linux-clang-tsan (`setarch -R`, the preset's TSAN_OPTIONS) | touched targets | the same; `[diag10c]` x10; no TSan report |

The platform window cases also ran on linux-gcc-debug with the WSLg display and without `CRD_PLATFORM_HEADLESS`, so
the Linux window path (create, move, move-assignment, destroy, shutdown cycles) ran under the ledger as well (5 cases).
The first linux-gcc-debug run failed the window case: under `CRD_PLATFORM_HEADLESS=1` the case still created a window
and expected the create to fail, but WSLg has a display. The headless branch now creates nothing and checks the
context alone.

Coverage (linux-clang-coverage, one run on the final source): the registry gained the scope group "GPU queue depth
at the completion seams" (`queue_pressure.cpp`). The run passed; all 83 scope files were instrumented; the new group
ran 162 of 166 regions and 124 of 142 branches; every outcome set is at its previous count; verdict PASS. Totals:
regions 10932/12210, branches 7300/9200, functions 1222/1274, lines 20032/22875. The 1.7 GB of profiles are deleted.
No new outcome set: the tracker has no result enum, and its states are judged by the counts the cases check.

Checks: strict tidy (LLVM 20.1.8) clean on all 31 changed C++ files (one finding fixed: the Vulkan context's `init`
crossed the function-size threshold by the four added statements; instance and device creation moved into two small
helpers); clang-format differs only in the repository's hand alignment, leading-operator wrapping and include
grouping, and the three lines it found over 120 columns were rewrapped; the Allman check passes; the guard CTests,
`crd-diag-coverage`, `crd-diag-seeded-faults`, `crd-build-profiles`, `crd-master-plan` and `crd-repository-hygiene`
pass (13); `diag-coverage.py --validate` and `test-diag-coverage.py` pass; check-master-plan and check-repository
pass. The full win-debug CTest was not run. No script or workflow changed.

## What the hosted run must show

The DIAG.10c row lists it in full. New in this batch: crd-gpu-context-tests with its eight queue-depth cases;
crd-core-tests with the native-class and `LifetimeHold` case; crd-platform-tests with the window cases (balanced
headless or without a display); crd-audio-tests with the device-lifetime case (its refused start balancing on runners
without an endpoint); crd-gpu-context-dx12-tests with its six new cases on WARP (the removal case on win-debug and
win-asan only); crd-gpu-context-vulkan-tests with its five new cases on lavapipe (and, on the Windows lanes without a
Vulkan device, the failed starts balancing), including the lost-queue case without a LeakSanitizer report under
linux-gcc-asan; crd-kir-dx12-tests with its ledger case; `gpu.resources` with the queue fields; and
`crd-diag-coverage` with the queue-depth scope group.

## Follow-up

- The hang watchdog could classify a stall from the pressure and queue-depth readings.
- The WASAPI device counts an xrun on the first period after a restart (`padding == 0` with frames from the previous
  session); a per-session frame count would keep a restart from reading as a glitch.
- The crd-kir-dx12 backend submits outside the completion seams, so its queue is not in the depth.
- Vulkan semaphores, command pools, descriptor pools and the surface are not in the ledger; they are owned by objects
  that are, but a leak inside one of those owners would not show.
- `retire_transients_to` on Vulkan (queued from the previous batch).
