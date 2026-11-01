# 2026-11-01 — HW-RESILIENCE.3: the GPU device check (DX12)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the Vulkan device check](2026-11-01-hw-resilience-3-device-check.md).

## CI read

- No completed run has a failed job; push runs up to 41303292160 were cancelled by concurrency.
- Complete-tier run 41269105413 (`b0b1b245`) has ended red. Its failures are the causes 41274380605 showed, fixed on
  main, plus win-arm64-debug and win-vs, which pass on the later 41274380605.
- Complete-tier run 41274380605 (`de5e10b0`) has ended red on the five causes already fixed on main: clang-cl
  `__udivti3` (7cfed26c), the file-time case on the Windows lanes (cc5ee9e9), its constant name on win-tidy
  (4b9bc837), the multivariate buffers under GCC LTO (76330344, read again in this batch's logs) and the no-PDB
  image on win-release (79d0c0c0).
- Needs CI: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) still wait on a green
  complete-tier run. 41295647725 (`43963cb2`) is running; 41298463221 and 41302856153 are queued, so none was
  dispatched. Actions variables: the three `FTL_*` names; no `AMD_CLOUD_*` or `MAC_CLOUD_*` secrets, so those lanes
  wait on their accounts.
- Found locally: `crd-ci-platform` failed its design-document check, because 72f9696f added the host-check
  `vulkan-sdk-installer` jobs without regenerating the parity table in
  [ci-platform.md](../design/ci-platform.md). The table was regenerated from the workflows (aa667ddb), and the suite
  passes. `crd-ci-orchestrator` failed once in the same local run (73 s, log lost to an interrupted ctest). It then
  passed four times, once under ctest and three times directly, so it was recorded rather than chased.
- [DIAG.11b](../ROADMAP.md#slice-diag.11b): no recording newer than 20261101T004352Z, in the checkout or in
  `D:\Dev\cerid-diag11b`. Its four recorded workloads (alloc, io, jobs, offline) open no GPU device, so the check
  does not move their frame coverage.

## Where the check runs

Every DX12 device the engine opens with no named adapter goes through `Dx12DeviceScope::create`: the GPU, compute,
raster, ray-tracing and work-graph contexts and the default-device queries (`dx12_default_adapter_kind`, the
inner-coverage route). The chooser sits there, so one place covers them all.

- `dx12_device_check.cpp` lists the adapters: DXGI's high-performance order (`EnumAdapterByGpuPreference`, or the plain
  list where the runtime has no `IDXGIFactory6`), then WARP when DXGI does not list it (same LUID). It sorts them by
  kind, keeping DXGI's order within a kind: hardware, then unclassified, then software, then the separate WARP. The kind
  comes from the evidence-based classifier (`query_dx12_adapter_kind`), so a display-backed Basic Render Driver counts
  as software.
- Each adapter goes through `check_device_once` (once per process per adapter and ordinal, the fault seam first). One
  that failed is passed over. When none passes, `create` returns `DXGI_ERROR_NOT_FOUND`, so every context is invalid
  and the factories return nothing, the consumers' CPU path. Each call notes the dx12 selection.
- The probe: the kernel is lowered to HLSL and compiled to DXIL on the host first. A host without dxc leaves the
  adapter `unchecked`. Then a `Dx12ComputeContext` is opened on that one adapter (a probe constructor taking the
  provider-private `Dx12ProbeAdapter`). It loads the pipeline from the DXIL and runs `run_device_check`. A named
  adapter opens unchecked through the same `create`, so the probe cannot recurse. The choice runs before
  `devices_mutex` is taken.
- The probe's device is released before the caller's device is created. D3D12 keeps one device per adapter, so the
  caller gets a fresh one, with the debug layer, GBV and DRED it asks for. The existing scope logic resets those
  settings between devices.
- `dx12_adapter` now reads its identity from an `IDXGIAdapter1` (the device overload goes through it), so the
  chooser's identity and an open device's identity are byte for byte the same.

Order change: the default was `D3D12CreateDevice(nullptr)`, DXGI's first adapter (the one driving the primary
display). It is now the high-performance hardware adapter, matching Vulkan's discrete-first order. On this desktop and
on the hosted runners the adapter is the same as before.

Cost on this host (RTX 4070 Ti SUPER, win-debug): the first check takes 160 ms. That covers the dxc load and compile,
the probe device and the dispatch. A later check of the Basic Render Driver in the same process took 22 ms. Every
later context pays nothing. The hosted Windows lanes print the WARP figure in the case's
`[hw-resilience 3] dx12 device check on ...` line.

## Tests and evidence

- crd-gpu-context-dx12-tests, new `test_dx12_device_check.cpp`:
  - The adapter passes once, and its context's queue has submitted nothing. Compute and raster contexts open the same
    adapter without a second check. When DXGI lists hardware, the chosen adapter is hardware.
  - Under a `Dx12ValidationCapture`, the probe and the context are two devices and the capture stays complete and
    silent. So the check kernel is clean under the debug layer.
  - The kernel on a compute context reproduces its answers, an off-by-one input fails as `arithmetic`, and no
    pipeline fails as `build`.
  - The seam fails the preferred adapter. Here the RTX was passed over for the Basic Render Driver (`demoted`, and the
    compute context follows); where WARP is the only adapter, it is `no-device-passed`. Both branches print the
    startup line.
  - A hook that fails every dx12 adapter leaves the GPU, raster and compute contexts with no device. Every adapter
    DXGI lists is checked exactly once (an independent DXGI count, WARP deduplicated by LUID), with the
    `no device` line.
- test_dx12_validation.cpp: four cases count exactly the devices under test (`contexts_started` 1, 2, 2, 5). They now
  settle the check before the capture starts, as the Vulkan diag10c windows do.
- win-debug: the whole crd-gpu-context-dx12-tests program (its 221 earlier CTests, each in its own process, then the
  changed validation cases and the five new ones). Also every CTest of
  crd-ceir-gpu-dx12-tests, crd-gpu-context-tests, crd-gpu-context-encoder-gpu-tests, crd-render-graph-gpu-tests,
  crd-scene-render-tests, crd-sandbox-inspect-tests and the work-graph program (239), and the 21 guards. Strict tidy is
  clean on the ten changed files, and clang-format finds nothing on added lines. Windows-only code, so no Linux lane.
- Teeth, each one rebuilt and observed failing:
  - Not choosing for an unnamed device failed four cases.
  - Not passing over a failed adapter failed the fallback and no-device cases.
  - Ranking software before hardware failed the hardware-first assertion.
  - Not deduplicating WARP failed the exact check count.
  - Each was restored and rebuilt, and the cases passed again.

The CKIR test backend `KirBackendDx12` (crd-kir-dx12, used only by the CEIR GPU tests) still opens its own device with
`D3D12CreateDevice(nullptr)`, outside the context, as its Vulkan twin does. Folding the v17 test backends into the
context is [V17-RECONCILE](../ROADMAP.md#slice-v17-reconcile)'s "obsolete module ownership". No production code builds
one.

## Left on the row

The device check on CUDA, and the state-hash divergence check: the local ledger, the first divergent step and the
culprit (the wire exchange belongs to NET.3 and 4.2c).
