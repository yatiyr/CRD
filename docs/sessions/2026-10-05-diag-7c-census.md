# DIAG.7c census: Vulkan synchronization, GPU-assisted validation and device faults

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7c](../ROADMAP.md#slice-diag.7c); contract:
> [DIAG.7c](../design/runtime-diagnostics.md#diag-7c); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md)
> DG12. Rules: [AGENTS](../../AGENTS.md). Preceding: [DIAG.7b census](2026-10-04-diag-7b-census.md).

## Position

DIAG.7c requires DIAG.7b, which has landed (b)-(g). Its only remaining unit, (h), is the hardware-gated real device
fault. The census changes no row status and starts no device-loss reproduction. It ran while hosted CI qualified the
repairs, per the user's direction to carry on serially.

## Acceptance clause (verbatim)

> Qualify validation layers, synchronization checks, optional GPU-assisted checks and supported device-fault extension
> collection. Pin SDK/layer versions; account for shader instrumentation resource limits. Preserve queue-family,
> access, layout, descriptor and program identity. Query capabilities before enabling optional modes and retain
> original VK_ERROR_DEVICE_LOST information when further calls fail.
>
> Acceptance: controlled missing-barrier/stale-resource cases, asynchronous reports after fence completion, layer
> absent, descriptor-limit fallback and device-loss capture on declared Windows/Linux tuples. External RenderDoc/vendor
> tools may supplement evidence, never replace the public reporting path or constitute unsupported platform
> qualification.

## Workstation tuple (measured 2026-10-05)

| Item | Value |
|---|---|
| Adapter | NVIDIA GeForce RTX 4070 Ti SUPER, driver 595.79, device API 1.4.329 |
| Loader / SDK | Vulkan instance 1.4.341; SDK `C:\VulkanSDK\1.4.341.1` (the pinned SDK) |
| Validation layer | `VK_LAYER_KHRONOS_validation` 1.4.341, implementation version 1 |
| Layer extensions | `VK_EXT_validation_features` rev 2, `VK_EXT_layer_settings` rev 2 |
| Device fault | `VK_EXT_device_fault` rev 2: `deviceFault` and `deviceFaultVendorBinary` true |
| Descriptor sets | `maxBoundDescriptorSets` 32 (GPU-assisted validation reserves one for its instrumentation) |

## What exists

- **Mode requests and activation (DIAG.7a f).** `init()` loads `VK_LAYER_KHRONOS_validation` when any mode is
  requested. It enables `VK_EXT_debug_utils`, and also `VK_EXT_validation_features`, which it enumerates from the layer
  itself. It chains the synchronization and GPU-assisted enables onto `VkInstanceCreateInfo` and reports a common
  `ValidationActivation`:
  - Core is active when the layer loaded.
  - Sync is active when the features extension is present.
  - GPU-assisted is active only when the device has the stores/atomics features its instrumentation needs, which are
    then enabled on the device.
- **Capture and correlation (DIAG.7a).** `ValidationCapture` rides a debug-utils messenger with bounded records.
  Cerid identities are resolved from named objects, then from message prose. Pass labels correlate through
  `PassLabelScope`.
- **Already-qualified controlled cases (DIAG.7a g-tests):**
  - a core hazard on a named buffer;
  - a synchronization write-after-write on a named buffer with no barrier; the barrier and sync-off controls stay clean;
  - a GPU-assisted shader out-of-bounds access read back at the fence wait, correlated to a Cerid identity;
  - pass-scope correlation.
- **Device-fault analogue on DX12 (DIAG.7b).** The DX12 helpers observe removal on every completion path, bound every
  wait, force removal on failed completion, record origin and DRED, and write a versioned bundle into a dump.

## What is missing

- **Layer absent.** Requesting validation without an installed layer fails `vkCreateInstance`
  (`VK_ERROR_LAYER_NOT_PRESENT`), so the whole context is invalid. It should be a working context that reports
  `LayerAbsent` per requested mode. Capabilities are not queried before the layer is requested.
- **No version pinning in evidence.** The layer's spec and implementation versions and the loader version are never
  recorded with a report.
- **Instrumentation limits not accounted for.** GPU-assisted validation needs a free descriptor-set slot. A pipeline
  layout using every slot is silently uninstrumented. Nothing reports or tests this fallback.
- **No device-loss handling.** Every completion path ignores the result of `vkQueueSubmit` and `vkWaitForFences`, and
  waits with `UINT64_MAX`. There is:
  - no bounded wait;
  - no `VK_ERROR_DEVICE_LOST` observation;
  - no retention of the original error when later calls fail;
  - no execution-failure count.
- **No device-fault collection.** `VK_EXT_device_fault` is never enabled or read (`vkGetDeviceFaultInfoEXT`: fault
  description, address infos, vendor infos, vendor binary), and there is no removal record or bundle like DX12's.
- **No stale-resource case.** No test covers use of a destroyed or retired resource, or a mismatched image layout.

## Design points for review

1. **One completion seam, as on DX12.** Add bounded `vk_submit` and `vk_wait` helpers in the provider, mirroring
   `dx12_submit` and `dx12_wait`. They report through the capture, latch device loss, and keep the first lost result.
   The existing paths are moved onto them one by one, compute first; a wholesale rewrite is out of scope.
2. **Simulated versus real loss.** Vulkan has no `RemoveDevice`. The simulated error path is an injected
   `VK_ERROR_DEVICE_LOST` at the helper seam, labelled simulated. A real loss stays the hardware-gated reproduction it
   is on DX12. `vkGetDeviceFaultInfoEXT` is queried only after a loss; on a live device the record says "not lost".
3. **Layer absence is a capability, not a failure.** Enumerate instance layers before requesting one. A missing layer
   reports `LayerAbsent` for every requested mode, and the context still comes up.

## Sub-unit plan (order re-confirmed per step)

| Unit | Work | Gate |
|---|---|---|
| (b) | Query the layer before enabling it. Report an absent layer as `LayerAbsent` and keep the context valid. Record the layer's spec and implementation versions with the activation. | Loader-disabled layer (`VK_LOADER_LAYERS_DISABLE`) on the workstation, plus a normal run |
| (c) | Bounded `vk_submit` and `vk_wait` with device-loss observation and first-error retention, counted as execution failures. Move the compute context onto them. | Injected loss at the seam (simulated) and a timed-out wait |
| (d) | `VK_EXT_device_fault`: enable when advertised. On a recorded loss, read the fault description, address infos and vendor infos into a fixed report. Keep last-known state when a query fails. | Capability report on hardware; record path on injected loss |
| (e) | Descriptor-limit fallback: a pipeline using every descriptor-set slot under GPU-assisted validation is reported uninstrumented; one using fewer is instrumented. | GPU-assisted on hardware |
| (f) | Stale-resource cases: a destroyed or retired resource and a wrong image layout, correlated to identities. Asynchronous reports arriving only after fence completion are asserted explicitly. | Core and sync validation on hardware |
| (g) | Loss bundle in the DIAG.5 evidence container, like DX12 (f) | Bundle round-trip |
| (h) | Real device-loss reproduction: contained, hardware-gated, never on the desktop GPU outside a qualified harness | Stays visible as a hardware gate |

The hosted lanes install the pinned SDK but expose no Vulkan device. Their Vulkan device tests skip; the
`linux-gcc-asan` log shows the DIAG.7a(f) activation case as Skipped. The workstation is therefore the only device
tuple. The Linux device tuple stays a visible gate, and no hardware claim is made for it.

## (b) landed (2026-10-05): the layer is queried before it is requested

- `query_validation_layer` enumerates the instance layers before `VK_LAYER_KHRONOS_validation` is requested. The
  context keeps the result as the public `VulkanValidationLayer` (queried, present, spec version, implementation
  version), exposed through `VulkanGpuContext::validation_layer()`. That accessor is appended at the end of the class
  with a default, so existing mocks still compile.
- An absent layer is no longer requested, and its features extension is not chained, so `vkCreateInstance`
  succeeds. `initial_activation` reports `LayerAbsent` for every requested mode, and the context is valid.
- Extracting the two helpers keeps `init()` under the strict gate's function-size threshold.

`[gpu-context][vulkan][validation]` (b), on the workstation:
- The present layer is recorded as spec 1.4.341, implementation 1, with Core active.
- The layer is then made genuinely absent through the loader's own filter (`VK_LOADER_LAYERS_DISABLE`), and the same
  three-mode request yields a valid context. The layer reads as not present, and Core, Sync and GPU-assisted are each
  requested, inactive and `LayerAbsent`.
- A context that requests nothing does not enumerate layers at all.
- Teeth: with the presence check removed (the layer requested regardless, as before), the factory returns null and the
  case fails at `REQUIRE(absent != nullptr)`. Restored and rebuilt, it passes.

Results: the case passes with 21 assertions; `[validation]` 7 cases; the whole Vulkan test executable 293 cases,
6,931 assertions. The strict gate and the brace guard are clean on the three files.

## (c) landed (2026-10-05): a bounded, observed Vulkan completion seam

`vulkan_execution.hpp/.cpp` (provider-private) add `vk_submit` and `vk_wait`, the counterparts of `dx12_submit` and
`dx12_wait`:
- **Bounded wait:** waits are bounded, defaulting to the DX12 30 s.
- **Classification:** each failure is classified as `Observed` (`VK_ERROR_DEVICE_LOST`), `TimedOut`, `Failed` or
  `Simulated`.
- **First failure kept:** the first failure per device is kept, with its result, its operation label and a
  process-wide sequence. Later failures only increment the count, which retains the original `VK_ERROR_DEVICE_LOST`
  information when further calls fail.
- **Lost device:** a device known to be lost is not called again; its submissions and waits return
  `VK_ERROR_DEVICE_LOST` at once.
- **Fixed table:** a fixed table holds the per-device records, with no allocation on the failure path.
- **Last-known failure:** `vk_last_device_failure` keeps the newest first failure after its device is destroyed. The
  context destructor calls `vk_forget_device` before `vkDestroyDevice`, so a later device that reuses the handle value
  starts clean.
- **Test seam:** `vk_inject_next_result` makes the next call return a chosen result without the driver. Vulkan has no
  `RemoveDevice`, so this is the simulated error path, recorded as `Simulated`.

`VulkanComputeContext::submit_and_wait` now uses the seam. A failed submission or wait latches the context invalid, and
nothing of that submission is read back. Moving the raster, ray-tracing and DGC completion paths is later work in this
row; they still use unbounded waits.

`[gpu-context][vulkan][device-loss]` (c), on the workstation:
- A healthy compute round trip records nothing.
- A 1 ms wait on a fence nobody signals returns `VK_TIMEOUT` and records `TimedOut`, not lost. This is contained: no GPU
  work is pending.
- On a second device, an injected loss at the compute submit latches the context invalid and records `Simulated` loss on
  `compute submit`.
- A later submit returns `VK_ERROR_DEVICE_LOST` without the driver. The first failure keeps its origin, operation and
  sequence, and the count becomes 2.
- The first device's timeout record is untouched.
- After the lost device is destroyed, the last-known failure is the simulated loss, and the destroyed handle reads clean.
- Three consecutive passes, 27 assertions.
- Teeth: letting a later failure overwrite the first fails the origin, operation and sequence checks. Restored and
  rebuilt, it passes.

The strict gate and the brace guard are clean on the five changed files.

Device consumers of the compute path (hosted lanes have no Vulkan device, so this is their only device run):
- `crd-gpu-context-vulkan-tests`: 294 cases, 6,958 assertions, pass.
- `crd-kir-vulkan-tests`: 31 cases, 33,023 assertions, pass.
- `crd-ceir-gpu-vulkan-tests`: 57 cases. It passed in 5 of 7 runs.
  - Both failures were `ceir 28b-2b`, the anti-drift check of the committed autotune row. The committed row has
    `fuse=1`; the fresh measurement chose `fuse=0`, with `share` agreeing.
  - That case runs in about 0.5 s and ranks the plans by GPU timestamps (`last_gpu_ms`), which the seam's host-side
    bookkeeping does not enter. So the 30 s bound and the new submit path cannot decide it.
  - It is read as a near-tie between two plan-distinct configurations flipping on timer noise. That is a
    pre-existing weakness of a device-only anti-drift check, not a seam regression. It is recorded, not masked,
    and left for a separate fix (a margin or hysteresis for plan-distinct near-ties).

**Tooling note:** a `cmake --build <dir> --target rebuild_cache` run outside the MSVC environment emptied
`CMAKE_MAKE_PROGRAM` in `build/win-debug`. It was repaired with `scripts/configure-preset.bat win-debug
-UCMAKE_MAKE_PROGRAM`. Reconfigure only through the preset script.
