# 2026-10-31 — HW-RESILIENCE.2: the GPUs in the machine's identity

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2), its third part: the GPUs and drivers from the GPU
contexts. Design: [runtime diagnostics](../design/runtime-diagnostics.md#hw-resilience--hardware-fault-resilience).
Before this, the identity's GPU entries existed in the format and stayed empty.

## The adapter (crd-gpu-context)

- `crd/gpu/adapter.hpp`: `GpuAdapter` (was the compute context's `ComputeAdapter`, renamed and moved so both
  interfaces share it) gains `driver_version`, the driver as its API reports it. `IGpuContext::adapter()` is appended
  with an unknown default.
- Each backend reads it once, in one place: `detail::vk_adapter` (the physical device; `driverInfo` from
  `VkPhysicalDeviceDriverProperties`, else `driverVersion` in its vendor's packing), `detail::dx12_adapter` (DXGI;
  the user-mode driver version as Windows writes it, `32.0.15.8157`), and the CUDA context (`cuDeviceGetName`,
  `cuDriverGetVersion`). The compute contexts' two copies of the query are deleted; the GPU contexts keep the adapter
  they read and answer `adapter_name()` from it.
- `opened_adapters()`: the process's log of every adapter a device was created on, each once, at most 16. The
  backends note at their single creation points (`VulkanGpuContext` after `vkCreateDevice`, `Dx12DeviceScope::create`
  after `D3D12CreateDevice`, the CUDA primary context), so a context made by any module (kir-vulkan's included) counts.
  A listener hears the adapters noted before it, then each new one.

## The machine (crd-perf, crd-perf-gpu-bridge)

- `add_host_gpu`: the host identity is now a set of immutable snapshots behind an atomic index (snapshot k holds k
  GPUs, at most 16 additions), so `host_hardware_identity()` stays a lock-free reference a fatal delivery may take;
  the writers serialize, and once `attach_hardware_identity_to_crashes` has run every addition replaces the crash
  stream (the two-slot seam of the previous part). Names and drivers are trimmed and clipped as the reader clips them,
  so a bundle's identity equals the host's.
- `attach_gpus_to_hardware_identity()` (the bridge, the one module naming both) listens to `opened_adapters()`; an
  entry's driver is the backend and its version (`vulkan 581.57`, `dx12 32.0.15.8157`), since one GPU seen through two
  APIs reports two versions of one driver. The sandbox calls it where its diagnostic host starts.

## Verification (win-debug)

- `crd-gpu-context-tests [hw-resilience]`: the version formats (NVIDIA 10.8.8.6, Intel on Windows 18.14, the
  variant bits of `VK_MAKE_API_VERSION` ignored) and the log (unknown ignored, each once, a driver update new, full at
  16, a late listener replayed in order, a stopped one silent).
- `crd-perf-tests [hw-resilience]`: an addition is trimmed, appears once, leaves earlier references unchanged,
  round-trips through the codec clipped, reaches the next live dump after the attach, and stops at 16.
- `crd-perf-gpu-bridge-tests [hw-resilience]`: the entry's fields; adapters opened before and after the attach join.
- Devices on this machine: the Vulkan context's adapter equals its physical device's properties and `driverInfo`
  (`610.88`) and joins the host identity as `vulkan 610.88` once the bridge attaches; the DX12 context's driver
  version (`32.0.16.1088`) equals the `DriverVersion` Windows records for that PCI device in the display class; the
  CUDA context reports `13.3`; both compute contexts report their GPU context's adapter; all are in the log.
- linux-gcc-debug (WSL): the touched targets build warning-free and the same cases pass; the Vulkan device there is
  llvmpipe (`Mesa 25.2.8-0ubuntu0.24.04.2 (LLVM 20.1.2)`, Khronos vendor 0x10005).
- Teeth, each restored, rebuilt and rerun green: a listener not replayed (log and bridge cases, the Vulkan
  end-to-end), the Windows version's words reversed (format case, the registry comparison), Vulkan never using
  `driverInfo`, CUDA's minor version unscaled, `add_host_gpu` not trimming (the trim, clip and codec checks) and not
  replacing the crash stream (the dump after an addition).
- Strict tidy is clean on the 18 changed sources (it found a local constant's name, a loop for `any_of` and a
  duplicate include, all fixed); the ctest guards pass. One whole-suite `crd-perf-tests` run right after the build
  failed one assertion whose output was not kept; seven reruns passed, so per the host's record it is noted here, not
  chased. One cl.exe access violation (C1001) compiling `test_vulkan_context.cpp` at 16 jobs did not recur at 8.
