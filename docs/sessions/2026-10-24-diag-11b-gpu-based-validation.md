# DIAG.11b (9): D3D12 GPU-based validation's cost, and the null root signature it crashed on, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [standalone target clears](2026-10-24-diag-11b-standalone-target-clears.md).

## CI read at the start

`gh run list --branch main --limit 15`: every completed run since 40067359705 was cancelled with no failed job. The
one failed job is linux-gcc-public-checks (120819184913) of the scheduled run 40075929537, still running, head
`420aaf1a`: the high-water reading that `d96eae30` repairs. No row is `Needs CI`. (l) zstd 1.5.7 and (o) clang-tidy 20
stay open: this run's only user message approves no download.

## Finding

The device validation batch read the GPU-based validation crash as the driver's. It is ours. Under cdb, the second frame
of `crd-diag-overhead --workload render --validation gpu` faults in `nvwgf2umx` (32.0.16.1088, driver 610.88), called
from `D3D12SDKLayers!NDebug::CGBVDataController::InjectValidationShader` inside the readback barrier of
`frame_readback_from`, the 8th injected barrier of the process. The faulting load reads a root parameter's slot
(index 13) through a null pointer in the driver's command list state.

`Dx12RasterContext::frame_prepare` binds `nullptr` as the graphics and compute root signature whenever a reservation
selects a descriptor heap page: once per frame here, at the frame's first reservation (a probe printed it). This drops
the previous page's tables, so a verb that forgot an argument is caught at its draw. D3D12 does not define a null root
signature, and GPU-based validation's injected work restores its state onto it. The first frame passes; the second
crashes. The standalone draws record no frame and bind no null, which is why GBV passes on the standalone targets
(checked with a temporary GBV variant of that case).

The context now creates a root signature with no parameters and binds it there instead. A different signature drops
every root argument exactly as before. These were the backend's only null binds. WARP never crashed, so the hosted
evidence is the new CTest passing there.

The harness's capture also lost 33 of GPU-based validation's 289 info messages: the default capacity is 256 records,
and the patched shaders add their own messages. The capture now holds 4,096, as `qualify_dx12_workload` does. The
count is per process (the same at 1 and 20 frames), so the board's 200 frames fit.

No register entry: no failure remains, and the trigger was our code.

## Change

- `engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp`: `m_unbound_root`, created with the context; `frame_prepare`
  binds it in place of `nullptr`.
- `tests/support/diag-overhead/workloads.cpp`: the D3D12 capture holds 4,096 records.
- `tests/support/diag-overhead/CMakeLists.txt`: `crd-diag-overhead-render-validation-gpu` on Windows (2 s here).

## Board

[Board](../bench/2026-10-24-diag-11b-device-validation.md), win-shipping on this host's adapter, 24 processes per arm:
GPU-based validation costs +66.1% frame time (+63.3 .. +71.0) and +227.7% CPU per frame (+220.2 .. +233.4). Core
measures +19.2% and +36.1% in the same binary.

## Teeth

win-debug, on this host's adapter, each restored, rebuilt and green:
- `nullptr` bound again: `crd-diag-overhead-render-validation-gpu` fails with a segmentation fault, and core passes;
- the default capacity: the gpu CTest fails with `lost: 11` ("a message was reported or lost").

## Verification

win-debug: the 20 `crd-diag-overhead` CTests, crd-gpu-context-dx12-tests (216 cases), crd-ceir-gpu-dx12-tests (47),
crd-gpu-context-encoder-gpu-tests, crd-gpu-context-dx12-work-graph-tests, crd-scene-render-tests (115,
`CRD_ASSETS_DIR` set), crd-render-graph-gpu-tests, crd-imgui-tests, crd-perf-ui-tests and both sandbox suites pass.
The rest of `dev.py plan`'s targets build. win-shipping builds the harness with no warning. No Linux lane: the engine
file is D3D12 only. The ctest guards, `tidy-files.py` and `clang-format --dry-run` on the changed C++,
`check-allman-braces.py`, `check-diag-runbooks.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: the Windows lanes pass `crd-diag-overhead-render-validation-gpu` on WARP, and the DX12 suites.

## Not done in this batch

- (9) Vulkan's GPU-assisted validation: on lavapipe the pinned layer reports `GPUAV-Compile-time-general-buffer` for
  each shader object (`vkCreateShadersEXT`, too many storage and uniform buffer bounds checks; cut off by the
  duplicate limit after 10). Its only remedy, `gpuav_force_on_robustness`, enables device robustness and skips the
  checks robustness covers. That would measure less validation, so the next batch reads the layer's threshold first.
  Then the generation-compare, recording and probe Cost rows.

Follow-up for an extra batch: none.
