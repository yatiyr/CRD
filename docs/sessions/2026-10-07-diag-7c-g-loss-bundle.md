# DIAG.7c(g) Vulkan loss bundle, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7c](../ROADMAP.md#slice-diag.7c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-7c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.7c(e) and (f)](2026-10-07-diag-7c-descriptor-limit-and-stale-layout.md);
> [DIAG.7b(f), the DX12 removal bundle](2026-10-04-diag-7b-census.md).

## Goal

The census plan's (g): write a recorded Vulkan device loss as a readable bundle in the DIAG.5 evidence container, as
DIAG.7b(f) does for a DX12 removal. (h), a real device loss, remains.

## What landed

`engine/gpu/gpu-context-vulkan/src/vulkan_execution.{hpp,cpp}` (provider-private) and `vulkan_context.cpp`:
- **The record.** `VkLossRecord` is trivially copyable and holds no pointers. It carries the device's first failure
  (origin, result, operation, sequence), the loss's own origin and operation, the failure and slow-wait counts, the
  adapter (vendor, device, driver and API versions, name) and the `VK_EXT_device_fault` report. Operation labels are
  copied into 64-byte buffers, truncated and terminated.
- **The adapter.** `vk_register_device_adapter` is called when the context creates its device, so a bundle names the
  adapter and driver without the process that wrote it. The device-creation helper that already registered the fault
  query does this too (`register_device_diagnostics`).
- **When it is written.** After every failed seam call the seam first reads the fault report of a newly lost device,
  then writes the loss bundle: once per device, and only when the device is lost. A timed-out wait or another error
  writes nothing. When another thread is still running the fault query, that thread writes the bundle once the report
  is in, so the bundle always carries it. The dump is written outside the store's lock, and its `WriteResult` is kept
  on the last-known loss (`vk_last_loss`), which outlives the device.
- **The carrier.** The record follows a 16-byte `VkLossBundleHeader` (magic `CRVL`, version 1, record size) in the
  evidence stream of a `DeviceRemoved` live dump (`gpu_*.dmp`), the DX12 route. `vk_read_loss_bundle` reads it back
  with one bounded `read_dump_stream` and classifies it `Ok`, `NoStream`, `BadMagic`, `BadVersion` or `BadSize`. The
  magic differs from DX12's `CRDR`, so neither reader accepts the other's evidence.
- **A loss after another failure.** Before this batch, `VkDeviceFailure::lost()` was true only when the loss was the
  device's first failure. A bounded wait that timed out before the device was reported lost therefore hid the later
  loss from the fault query and the lost-device short cut. `lost()` now follows the first `VK_ERROR_DEVICE_LOST`
  (`loss_origin`, `loss_operation`), and the first failure is still kept unchanged.
- **Simulated stays labelled.** An injected loss has `loss_origin` `Simulated` in the bundle. A real loss stays the
  (h) hardware gate.

### Linux

`crash::capture_dump` is a Windows facility (DIAG.5a); the Linux backend has no non-fatal dump and returns
`Unsupported` (DIAG.5b writes a crash record only on the fatal path). On Linux the loss is therefore recorded and kept
in process, including the fault report and adapter, and its bundle result is `Unsupported`. That is what the hosted
lavapipe lanes can show. An on-disk Linux loss bundle would need a non-fatal Linux evidence writer in the crash module;
this batch does not add one.

### Not checked

`crash::capture_dump` must not be called from a fiber. The seam runs on the thread that drives the context, as the DX12
helpers do; neither provider checks for a fiber, because neither depends on the job system.

## Test

`tests/gpu/gpu-context-vulkan/test_vulkan_loss_bundle.cpp` (new, `[gpu-context][vulkan][gpu][device-loss]`):
- **Without crash capture:** an injected loss on a compute submit is recorded and kept with origin and loss origin
  `Simulated`, both operations "compute submit", the adapter name and vendor of the context, and the fault report read
  exactly when the device enabled the extension. The bundle result is `NotInstalled` on Windows and `Unsupported`
  elsewhere. The record survives the device.
- **With crash capture (Windows):**
  - A bounded wait on a fence nobody signals times out: not lost, and no `gpu_*.dmp`.
  - A loss injected after that is lost, keeps `TimedOut` as the first failure, and writes exactly one bundle.
  - A later submit on the lost device writes no second one.
  - The bundle reads back `Ok`: first failure `TimedOut`/`VK_TIMEOUT`/"test wait", loss `Simulated`/"compute submit",
    two failures, the adapter, and the fault report with the live query result.
- **Refusals (Windows):** a live dump without evidence reads `NoStream`. Hang dumps carrying foreign evidence, DX12's
  removal header, a loss header of the next version and a header without its record read `BadMagic`, `BadMagic`,
  `BadVersion` and `BadSize`.

## Evidence

- **win-debug (RTX 4070 Ti SUPER, driver 595.79):** the whole Vulkan test executable passes, 301 cases and 7,243
  assertions. `[device-loss]` passes 5 cases (137 assertions). The (g) case prints the RTX adapter with
  `VK_EXT_device_fault` enabled. The bundle carries the live fault-query result (the case asserts equality and does
  not print it; the (d) case in the same run printed `VK_ERROR_UNKNOWN`, -13, for its injected loss).
- **win-shipping and win-clang-cl-shipping:** the test executable builds (the clang-cl thin-LTO link was clean on the
  first attempt). `[device-loss]` passes 5 cases (137 assertions) and `[validation]` 10 cases (315) on both.
- **win-asan:** `[device-loss]` passes 5 cases (137 assertions) with no ASan report.
- **WSL lavapipe (llvmpipe, LLVM 20.1.2; `VK_EXT_device_fault` absent):** linux-gcc-debug, linux-gcc-asan and
  linux-clang-tsan each build the executable and pass `[device-loss]`, 5 cases and 83 assertions, with no sanitizer
  report. The (g) case takes the Linux branch: the loss is kept with its adapter and the bundle result is
  `Unsupported`. The whole executable on linux-gcc-debug: 301 cases, 300 passed (6,961 assertions) and one skipped,
  the existing REN-38-A13 shading-rate gate, which lavapipe cannot run.
- **Checks:** strict tidy is clean on the four changed C++ files (`vulkan_execution.hpp` analysed on its own) and the
  brace guard passes. clang-format `--dry-run`: the new test file is clean. The findings on the added lines of the two
  seam files are the files' established hand alignment of consecutive assignments and members, which the
  configuration does not model; none is a line break. The 8 ctest guards, check-master-plan and check-repository pass.

## Teeth

Each tooth was built on win-debug and run there:
- **Write skipped** (the bundle result forced to `NotInstalled` instead of calling `capture_dump`): the (g) case fails
  four assertions, the `Ok` result, both `gpu_*.dmp` counts and the bundle lookup.
- **Old `lost()` rule** (true only when the loss was the first failure): the (g) case fails seven assertions. The loss
  after the timeout is not lost, nothing is written, and the later submit reaches the driver (`VK_SUCCESS`, not
  `VK_ERROR_DEVICE_LOST`). The (c) and (d) cases still pass, as they should: their losses are first failures.
- **Reader version** (the reader expecting the next version): the round trip fails with `BadVersion`.

Each edit was restored, win-debug rebuilt, and the full suite above ran on the restored build.

## Remaining in DIAG.7c

- **(h) A real device loss,** following the 2026-10-05 split:
  - Measure whether lavapipe can produce a real `VK_ERROR_DEVICE_LOST` in a contained child (local, like 7b(h1) on
    WARP).
  - The opt-in hardware run is the user's (h2).
- **Hosted evidence.** The hosted run must show the (e), (f), (g) and DIAG.7a(g-3) cases passing on the lavapipe Linux
  lanes, with (g) reporting `Unsupported`. The hosted Windows lanes have no Vulkan device, so there the cases skip.
