# 2026-11-01 — HW-RESILIENCE.3: the GPU device check (Vulkan)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the hardware check](2026-11-01-hw-resilience-3-hardware-check.md).

## CI read

- No completed run has a failed job; push runs 41295628482 to 41299691670 were cancelled by concurrency.
- Complete-tier run 41274380605 (`de5e10b0`) is still queued on its last jobs. Its thirteen failed jobs fail on four
  causes, each fixed on main before this batch: `__udivti3` on clang-cl (7cfed26c), the file-time case on the Windows
  lanes (cc5ee9e9), its local-constant name on win-tidy (4b9bc837), the multivariate buffers on GCC LTO (76330344), and
  the no-PDB code image on win-release (79d0c0c0).
- Needs CI: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) wait on a green
  complete-tier run of the current main. Runs 41298463221 (`13453acb`) and 41295647725 (`43963cb2`) are queued, so none
  was dispatched. Actions variables: `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist; no
  `AMD_CLOUD_*` or `MAC_CLOUD_*` secrets, so those lanes wait on their accounts.
- [DIAG.11b](../ROADMAP.md#slice-diag.11b): no recording newer than 20261101T004352Z.

## Where the check runs

No production code picks a `KirBackend`; consumers receive the device a backend factory creates. So the check acts in
the factory, which is where a fallback can change what a consumer gets. The kernel stays CKIR in crd-kir
(`crd/kir/ckir_device_check.hpp`), so every backend lowers one source with its own emitter.

- The kernel: two workgroups of 64. It computes `((x * x + 3x) - 5) * (x - 2)` in f32, exchanges lanes through
  workgroup memory across a barrier, tree-sums each workgroup between barriers, and runs a u32 multiply-shift-xor chain.
  All values are exact integers below 2^24, so reduction order and contraction cannot change a bit. The answers are
  evaluated at compile time; outputs start as an all-ones NaN, so an unwritten output fails.
- `crd/gpu/device_check.hpp`: `check_device_once` (once per process per adapter and ordinal, one check at a time, the
  fault seam first), the `device_checks()` log of checks and per-backend selections with a listener,
  `run_device_check` (upload, dispatch, copy back over `IComputeContext`, as `execute_lowered_host` does, so it runs on
  any backend) and `report_device_demotions`.
- Vulkan: `create_vulkan_gpu_context` orders adapters discrete, integrated, virtual, other, CPU (before: the first
  discrete, else the first listed) and passes over each that failed. The check runs on a probe device of the same
  adapter that is destroyed before the caller's device is returned, so that device has queued nothing.
- A host that cannot lower or compile the kernel (no shaderc) has not tested the device: the adapter runs `unchecked`
  and the line says so. A pipeline the driver refuses is the device's failure (`build`).
- The fifth fault seam, `install_device_check_fault_hook`, fails a named adapter on a named backend.
- `gpu.resources` gives `device-check` and `device-selection` items and `device_checks` and `device_demotions` in its
  summary. The sandbox prints the demotion line at start.

Cost on this host (RTX 4070 Ti SUPER, win-debug): 175 to 187 ms, once per process. That is 75 to 85 ms for the probe
device, 60 ms to load shaderc and compile, 3 ms to run the kernel, and the rest to tear down. On llvmpipe
(linux-gcc-debug, WSL) it is 56 ms. Each later context pays nothing.

## Tests and evidence

- crd-kir-tests: the kernel on `eval_cpu_kernel` reproduces the compile-time answers bit for bit (521 assertions,
  spot values worked by hand). The verifier names each part. GLSL and HLSL lower the kernel.
- crd-gpu-context-tests (device-free): a check runs once and an identical second GPU is its own check; `unchecked` is
  no failure; the seam fails a named adapter before the check runs; a selection is heard only when it changes; and
  the exact demoted, no-device and unchecked lines.
- crd-gpu-context-vulkan-tests: the adapter passes once with the returned device's queue untouched; the kernel on the
  device passes and a wrong input fails as `arithmetic`; and a seam-failed adapter goes to the next adapter or to no
  device, with the startup line. This host has one Vulkan adapter, so it takes the no-device branch. gpu.resources
  reports the passed check and selection.
- win-debug: the whole crd-gpu-context-vulkan-tests program (317 cases), crd-perf-gpu-bridge-tests and the diag10c,
  diag9b, diag7c and hw-resilience CTests pass; linux-gcc-debug on llvmpipe: the hw-resilience, diag10c Vulkan and
  gpu.resources CTests pass. Strict tidy, clang-format on added lines, Allman and the 21 guards are clean.
- Four diag10c lifetime windows opened before the process's first context. The check's shaderc load is held for the
  process, and the ledger reported it as `library-handle +1`. Each now opens after a first context has come and gone.
- Teeth: a kernel constant changed (5 to 4) failed the oracle case and the device case (`arithmetic` on the RTX);
  dropping the seam consult failed the seam and fallback cases; not passing over failed adapters failed the fallback
  case. Restored, rebuilt, passing.

## Left on the row

The device check on DX12 (three device-creation sites: `Dx12GpuContext`, `Dx12ComputeContext` and the raster
context, all through one checked chooser ending at WARP) and CUDA; the state-hash divergence check (its local
primitive here, the wire exchange NET.3's and 4.2c's).
