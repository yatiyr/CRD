# 2026-11-01 — HW-RESILIENCE.3: the GPU device check (CUDA)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the DX12 device check](2026-11-01-hw-resilience-3-dx12-device-check.md).

## CI read

- No completed run has a failed job; push runs up to 41308485803 were cancelled by concurrency.
- Complete-tier run 41274380605 (`de5e10b0`): every job has ended (the failures are the five causes already fixed on
  main, read in the previous note); only its Required result is still queued. Complete-tier runs 41295647725,
  41298463221 and 41302856153 and host check 41298875777 are queued, so none was dispatched.
- Needs CI: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) still wait on a green
  complete-tier run. Actions names: the three `FTL_*` variables; no `AMD_CLOUD_*` or `MAC_CLOUD_*` secrets, so those
  lanes wait on their accounts.
- [DIAG.11b](../ROADMAP.md#slice-diag.11b): no recording newer than 20261101T004352Z, in the checkout or in
  `D:\Dev\cerid-diag11b`.

## Where the check runs

`create_cuda_compute_context` is the one place the engine opens a CUDA device. Its context now opens the device
`cuda_checked_device` chooses, instead of device 0.

- The devices go in CUDA's own enumeration order: fastest first, or PCI bus order under `CUDA_DEVICE_ORDER`, with
  `CUDA_VISIBLE_DEVICES` applied. Every one is NVIDIA hardware, so there is no rank as on DX12. Each goes through
  `check_device_once` (once per process per device and ordinal, the fault seam first). A device that fails is passed
  over. When none passes, `valid()` is false, the "no CUDA device" path every consumer already takes. Each
  construction notes the cuda selection.
- The identity (`cuda_adapter`: name, driver CUDA version) and the NVRTC target (`cuda_arch`) are read without a
  context, so the chooser names a device before anything opens on it.
- The probe: the kernel goes through `emit_compute_kernel_cuda` (a kernel that does not lower leaves the device
  `unchecked`), then NVRTC compiles it for the device's own `sm_XY` with `--fmad=false`. A refused compile is the
  device's `build` failure, not `unchecked`. On Vulkan and DX12 a host without a shader compiler can still run
  cooked pipelines. Here NVRTC is linked into the backend and there is no cooked CUDA corpus, so a device NVRTC
  cannot target is one this backend can run nothing on, and passing it over is the right outcome.
- The CUBIN runs on a `CudaContextImpl` probe built on a context of its own (`cuCtxCreate`: the four-argument v4
  form on CUDA 13, the three-argument form before). It is pushed current on the thread and destroyed (popped) before
  the chosen device's primary context is retained. No module, stream or allocation of the check ever lives in the
  primary context the caller shares with kir-cuda. The probe never retains the primary context, so it cannot drop
  that context's refcount to zero and reset it. A sticky CUDA error (an illegal address, say) is process-wide by
  NVIDIA's definition, so a separate context does not contain one.
- `create_pipeline_from_cuda` now compiles and then calls `load_cubin`, which the probe also uses, so the module
  load is one routine.

Cost on this host (RTX 4070 Ti SUPER): 76 ms on win-debug (CUDA 13.3, driver 13.3), 156 ms on linux-gcc-debug
through the WSL passthrough (toolkit 12.0). That covers the emit, the NVRTC compile, the probe context and the
dispatch. Every later context pays nothing.

`KirBackendCuda` (crd-kir-cuda, built only by the CEIR GPU tests and the offline `kir-autotune` tool) still opens
device 0 on its own, as its Vulkan and DX12 twins do. The two share the primary context only while device 0 passes.
Folding the v17 test backends into the context belongs to [V17-RECONCILE](../ROADMAP.md#slice-v17-reconcile).

## Tests and evidence

New file `test_cuda_device_check.cpp` in crd-gpu-context-cuda-tests:

- The device passes once (checked, ordinal 0) and its selection is `passed`. The context leaves the thread on the
  device's primary context. A second context opens the same device and checks nothing again.
- The kernel on a context reproduces its answers. One input off by one fails as `arithmetic`, and a missing
  pipeline as `build`.
- The seam fails the preferred device. Either another device runs (`demoted`), or there is none (`no-device-passed`,
  every device failed). This host has one GPU, so only the second branch is observed. The startup line names it.
- A hook that fails every cuda device leaves the context invalid, with no buffers. Each device CUDA lists (an
  independent `cuDeviceGetCount`) is checked once, in order. The `no device` line is printed.

Results:

- win-debug: all 38 CTests whose names mention CUDA pass, after crd-gpu-context-cuda-tests and
  crd-ceir-gpu-cuda-tests were rebuilt. The CEIR cases run the context beside kir-cuda on the shared primary
  context.
- linux-gcc-debug: the same 38 pass on the RTX through the WSL passthrough. That build compiles the pre-13
  `cuCtxCreate` form.
- Hosted lanes have no NVIDIA GPU and no CUDA toolkit, so this local evidence is the row's evidence for CUDA.
- Strict tidy is clean on the four changed C++ files, clang-format finds nothing on added lines, and the 21 guards
  pass. The first tidy run crashed clang-tidy (0xC0000005) on `cuda_compute_context.cpp`; the rerun analysed it
  clean. That is this host's known misexecution class ([evidence](2026-10-26-catch2-fault-address.md)), recorded rather
  than chased.

Teeth on win-debug, each one rebuilt and observed failing, then restored, rebuilt and passing again:

- Not passing over a failed device failed the seam and no-device cases.
- Opening device 0 without the chooser failed three cases.
- Not noting the no-device selection failed the seam and no-device cases.

## Left on the row

The state-hash divergence check: the local ledger, the first divergent step and the culprit. The wire exchange
belongs to NET.3 and 4.2c.
