# DIAG.9b device-loss simulation, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b fault injection](2026-10-10-diag-9b-fault-injection.md).

## Goal

The third slice of DIAG.9b: device-loss simulation through the existing DX12 and Vulkan device-removal paths
(DIAG.7b/7c), driven by the seeded, authorized fault injector of the previous slice, with the trigger and its causal
inputs captured even though the lost submission never completes. The other lifecycle stages (cook, install, execute,
teardown), the authored policy, the `flight.*` commands, the doctor, the flight bundle's identity sections and a real
host remain (see the end). This batch is committed locally and not pushed.

## What was there (checked before coding)

- Both providers already route every queue submission and completion through one seam: Vulkan's completion seam
  (`vulkan_execution.cpp`: `vk_submit`, `vk_wait`, `vk_wait_complete`, the idle waits) and DX12's command lifecycle
  (`dx12_execution.cpp`: `dx12_submit`, `dx12_signal`, `dx12_wait`).
- Vulkan already labels an injected loss: `VkFailureOrigin::Simulated`, set by the test-only
  `vk_inject_next_result`, and the DIAG.7c(g) loss bundle carries it. DX12 had no injection point; its removal record
  knew only `Observed` and `EngineForced` (the engine's own `RemoveDevice` after a failed completion).
- The providers cannot depend on crd-perf (lower modules report through small hooks), so the injector cannot be
  called from them directly. crd-core's I/O fault seam (previous slice) is the model: a hook in crd-core, compiled
  out of shipping builds, that crd-perf's injector installs.
- A loss injected at a wait would be unsafe on Vulkan: the caller treats a failed wait as the end of the work and
  frees its memory, while work already queued keeps running (Vulkan has no way to stop it; DIAG.7c(c) found exactly
  this on lavapipe). A loss injected at a submission queues nothing, so it is safe on both providers.
- D3D12 devices are per-adapter singletons while referenced, so a removed device must be released before the next
  device on that adapter is a fresh one; the DIAG.7b tests already remove devices in process this way.

## What changed

- **The GPU fault seam** (`crd/core/fault_seam.hpp`, crd-core): `install_gpu_fault_hook`, `uninstall_gpu_fault_hook`
  and `consult_gpu_fault(GpuFaultOp::Submit, operation)` answering `Proceed` or `LoseDevice`. Same discipline as the
  I/O seam: one atomic load without a hook, one hook at a time, the hook runs under the seam's mutex so uninstall
  waits for calls in progress, and the whole seam is compiled out of a shipping build (`CRD_FAULT_SEAMS`).
- **The `device-loss` site** (`crd/perf/fault_injection.hpp`): `FaultSite::DeviceLoss` (5, `kFaultSiteCount` is now
  6), no value. `FaultInjector::gpu_hook` decides one hit per consult and answers `LoseDevice` when it fires; the fired
  fault is recorded and triggers the flight recorder exactly as the other sites do, so the recipe is
  `fault-plan/1 fired=device-loss#<hit> minimal=<one-hit plan> plan=<armed plan>`. `fault.status` lists the new site.
- **Vulkan** (`vk_submit`): after the lost-device short cut and outside the completion seam's lock (the hook takes the
  injector's and the recorder's locks), a `LoseDevice` answer queues nothing and records `VK_ERROR_DEVICE_LOST` with
  origin `Simulated`. The existing path then reads the `VK_EXT_device_fault` report, writes the loss bundle once, and
  answers every later call on that device without the driver.
- **DX12** (`dx12_submit`): on a device not already removed, after the list is closed and before
  `ExecuteCommandLists`, a `LoseDevice` answer calls `ID3D12Device5::RemoveDevice`, reads DRED and records the removal
  with the new origin `Dx12RemovalOrigin::Simulated`, then fails the submission with `DXGI_ERROR_DEVICE_REMOVED`.
  Removing the device (rather than only failing the call) makes every later call meet a removed device, as after a
  real loss. The removal bundle is now version 3, since a version-2 reader does not know the new origin.

## Tests

- `tests/foundation/perf/test_diag_fault_injection.cpp`, a new `[diag9b][fault]` case: the site parses, is named and
  refuses a value; disarmed, the hook answers Proceed and counts nothing; installed on the core seam with
  `device-loss@1+1`, three consults answer Proceed, LoseDevice, Proceed; a second install is refused; the capture holds
  both frames before the fault and the fault, the `InjectedFault` trigger (site 5, hit 1) and the recipe with its
  one-hit plan; after uninstall a consult proceeds and counts nothing. On a shipping lane: arm Unavailable, install
  refused, consult Proceed.
- `tests/gpu/gpu-context-vulkan/test_vulkan_device_loss_injection.cpp` and
  `tests/gpu/gpu-context-dx12/test_dx12_device_loss_injection.cpp`, one `[device-loss][diag9b][fault]` case each, on the
  real compute contexts (DX12 with a 4-byte copy per submission):
  - Plan `device-loss@1+1`: submission 0 reaches the device and completes; submission 1 loses the device and the
    context latches invalid.
  - Vulkan: the device's failure is `Simulated` with loss origin `Simulated`, result `VK_ERROR_DEVICE_LOST` and
    operation "compute submit"; a later submission on the lost device is answered without the driver, counts as a
    failure but not as a device-loss hit (2 hits, 1 fired). The loss record names the adapter; on Windows the one
    loss bundle written reads back `Simulated` (on Linux the write reports Unsupported, as DIAG.7c(g) settled).
  - DX12: one new removal record, origin `Simulated`, DRED's removal reason `DXGI_ERROR_DEVICE_REMOVED`, the bundle
    written and read back as `Simulated` with the same sequence; the latched context submits nothing more.
  - Both: the flight capture holds the two frames before the lost submission and the fault event, the
    `InjectedFault` trigger at site 5 hit 1, a post window that was not truncated, and a recipe whose one-hit plan is
    `seed=<n>;device-loss@1+1%1000000`. That plan, parsed back from the recipe and armed alone, loses a fresh device at
    its second submission. With the hook removed, a third fresh device submits and completes (DX12 also copies the
    value through and back).
  - On a shipping lane each case asserts the refusal branch: arm Unavailable, install refused, three submissions on
    a live device all complete. On win-shipping and win-clang-cl-shipping this branch ran (8 and 6 assertions).

### Teeth

Each break was built on win-debug and run, then restored by rewriting the file, rebuilt and rerun green:

| Break | Result |
| --- | --- |
| neither provider consults the GPU seam | the Vulkan case failed 5 assertions and the DX12 case 8 (no loss, no removal) |
| the injected loss labelled `Observed` (Vulkan) and `EngineForced` (DX12) | the Vulkan case failed 5 and the DX12 case 3 (record, bundle, reproduction) |
| the site named `device-loss-x` | the perf case failed: `device-loss` no longer parses |
| the core seam ignores the hook's answer | the perf case failed: submission 1 was not lost |

After the last restore all three test executables were rebuilt on win-debug: `[diag9b]` in crd-perf-tests (22 cases),
`[device-loss]` in crd-gpu-context-vulkan-tests (6) and `[dred],[diag9b]` in crd-gpu-context-dx12-tests (8) passed.

## Verification

| Lane | crd-perf-tests `[fault]` | Vulkan `[diag9b]` / `[device-loss]` | DX12 `[diag9b]` / `[dred],[diag9b]` |
| --- | --- | --- | --- |
| win-debug | 8 cases, 2,820 assertions | 1, 58 / 6, 195 | 1, 48 / 8, 190 |
| win-shipping (seams compiled out) | 8, 2,715 | 1, 8 (refusal branch) | 1, 6 (refusal branch) |
| win-clang-cl-shipping (seams compiled out) | 8, 2,715 | 1, 8 (refusal branch) | 1, 6 (refusal branch) |
| win-asan (inside vcvars) | 8, 2,820 | - / 6, 195 | - / 8, 190 |
| linux-gcc-debug (WSL, lavapipe) | `[diag9b]` 22, 3,124 | 1, 52 / 6, 135 | n/a |
| linux-gcc-asan (WSL, ASan + UBSan, lavapipe) | `[diag9b]` 22, 3,122 | - / 6, 135 | n/a |
| linux-clang-tsan (WSL, lavapipe) | `[diag9b]` 22, 3,124 | - / 6, 135 | n/a |

- Whole suites in one process on win-debug (this machine's RTX 4070 Ti SUPER): crd-gpu-context-dx12-tests 204 cases,
  10,682 assertions, and crd-gpu-context-vulkan-tests 303 cases, 7,365 assertions, all passing, so the in-process
  removal of a DX12 device and the Vulkan loss leave no trace on the cases that follow.
- The WSL lanes ran one at a time with the pinned validation layer. `[fault]` in crd-perf-tests and `[diag9b]` in
  crd-gpu-context-vulkan-tests were repeated 3 more times on each gcc lane and 10 more under TSan, all passing; no
  sanitizer report.
- The I/O seam's consumers were rebuilt on win-debug: `[diag9b]` in crd-platform-tests (2 cases) and
  crd-resources-tests (2) pass; crd-sandbox builds.
- Two toolchain crashes, not code defects, each cleared on one retry with nothing else building: win-clang-cl-shipping
  lld-link crashed (access violation, during thin-LTO) linking crd-gpu-context-vulkan-tests while a WSL build ran,
  and win-asan's MSVC hit C1001 (access violation) compiling the unchanged `test_dx12_validation.cpp` while the TSan
  build ran.
- Strict tidy (LLVM 20.1.8) is clean on all 11 changed C++ files. clang-format shows only the repository's hand
  alignment and include grouping. The Allman check, the 8 ctest guards, `check-master-plan` and `check-repository`
  pass.

## Hosted CI must later show

On all six lanes crd-perf-tests green with the 8 `[fault]` cases (each asserting the compiled-out branch on the
shipping lanes). On the hosted Windows lanes the DX12 `[diag9b]` device-loss case run on WARP, not skipped, and the
Vulkan one skipped for want of a device; on the Linux lanes the Vulkan case run on lavapipe. The earlier `[dred]` and
`[device-loss]` cases (DIAG.7b/7c) still green with the version-3 removal bundle.

## Remaining for DIAG.9b

- Injection during cook, install, execute and teardown (load and reload are shown), with the previous working
  assets surviving a recoverable failure.
- Refusal of arbitrary external I/O replay as a shipping capability check, named by the doctor.
- The flight policy and a fault plan as authored assets, `flight.*` diag commands, and the doctor reporting the
  flight-recorder and fault-injection modes.
- The build and symbol identity sections in a flight bundle (DIAG.5d), added by the host's sink.
- A real host (crd-sandbox) installing the recorder with its triggers, its frame watch, its sink and the fault
  commands (including the GPU hook).
