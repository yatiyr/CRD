# DIAG.11b (9): device validation's cost, and three defects it found, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [sanitizer cost](2026-10-24-diag-11b-sanitizer-cost.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job. 40067359705 (head `9ce438da`, still
running) has one failed job, linux-gcc-shipping's `crd-repository-tools` (`CiTiers`' fixture repository: `Directory
not empty: .git`), which `b793af76` repairs. The runs after it were cancelled with no failed job; 40080021621 (head
`b793af76`) was pending. No row is `Needs CI`.

## Change

The harness's `render` workload takes `--validation core|sync|gpu`: its device is created with the core layer and
that mode (`GpuContextConfig`), under the backend's message capture; a mode the device does not run refuses the run
(exit 3, naming the reason), and `--check` fails any error, warning or lost message (printed to stderr). The record
gets a `validation` object. `diag-overhead.py` arms `validation-core|sync|gpu` (render's off mode with the flag,
reported against off, never budgeted; a run whose validation saw a message is an instrument failure).
CTests `crd-diag-overhead-render-validation-core` (every lane) and `-sync` (Linux). No new script or file outside the
board and this note.

For the D3D12 draws to be validated the raster context needed a request of its own:
`create_dx12_raster_context(alloc, const GpuContextConfig&)` (appended, as `create_dx12_gpu_context` has it) and
`IRasterContext::validation_activation()` (default: nothing requested; Vulkan returns its context's).

## Defects the validated arms found

- **The authored forward graph cleared its D3D12 output slowly, every frame.** The debug layer warned twice a frame
  (820 and 821, clear values that do not match the resource's): `forward_csm` clears `@output` to
  `[0.09, 0.10, 0.13, 1]` and reverse-Z depth 0, while a standalone target was created for black and depth 1. A target
  created without a clear value warns too, so the creation must know the clear: `FramePlans::output_clear` records the
  first clear a pass applies to `@output` (its colour and its own depth), `SceneRenderer::output_clear()` returns it,
  and `IRasterContext::create_color_depth_target_for(w, h, clear)` creates the target for it (D3D12; other backends
  ignore it). The harness, the sandbox (also after each frame switch, once its frames in flight are drained, and each
  resize) and the gizmo probe use it; `forward_basic`, the step-down, clears its output the same way. The other
  standalone targets (colour only, MSAA, VRS, stencil, G-buffer, visibility) keep their fixed or absent clear values:
  follow-up below.
- **Turning GBV on beside a live D3D12 device failed device creation** (`DXGI_ERROR_DEVICE_RESET`): D3D12 devices are
  per-adapter singletons and GBV is process-wide. `Dx12DeviceScope::create` now leaves the debug layer and GBV as they
  are while a scope's device lives; a requested mode the live settings lack is `SettledByLiveDevice` (new
  `ValidationUnsupportedReason`). The DIAG.7a(f-3) case skipped its plain-context leg when creation failed; it now
  requires it, a raster context beside the GBV device, and the refused GBV request.
- **Overlapping batched uploads raced on Vulkan.** Synchronization validation reported `WRITE_AFTER_WRITE` on the
  scene renderer's storage buffer every frame: two copies in one upload batch wrote the same range with no barrier, so
  either could land last. A batch now records its copies' destinations and puts a transfer barrier before a copy that
  overlaps one, the order the synchronous path keeps.

## Board

[Board](../bench/2026-10-24-diag-11b-device-validation.md): core validation costs +22.7% frame time and +40.0% CPU per
frame on the D3D12 hardware adapter; on lavapipe core +7.5% and +2.1%, synchronization +11.6% and +2.9%.

## Teeth

Each on its lane, then restored, rebuilt and green:
- before the clear fix the core CTest failed with the 18 warnings above (win-debug); before the batch barrier the sync
  CTest failed with 9 `SYNC-HAZARD-WRITE-AFTER-WRITE` (linux-gcc-debug);
- the raster's request passed as `false`: the core CTest refuses (`core validation is not active: not-requested`) and
  the f-3 case fails its raster leg;
- GBV changed beside a live device (the `settled` guard removed): the f-3 case's late raster context is null;
- `@output` never matched in `collect_clear_hints`: `DIAG.11b: a shipped frame's output clear is its first clear of
  @output` fails;
- `record_problem`'s validation clause removed: two `test-diag-overhead.py` cases fail.

## Verification

win-debug: the 768 CTests of crd-gpu-context-dx12-tests, crd-gpu-context-vulkan-tests, crd-scene-render-tests,
crd-frame-cook-tests and crd-diag-overhead pass (one pre-existing skip); crd-gizmo-probe builds; crd-sandbox
`--diag memory.allocators --smoke-test 3` passes on Vulkan (167 frames) and D3D12 (118), its frame installed and the
canvas made again for it.
linux-gcc-debug (mirror): the 521 CTests of crd-gpu-context-vulkan-tests, crd-scene-render-tests,
crd-frame-cook-tests and crd-diag-overhead pass. win-shipping and linux-gcc-shipping build the harness with no
warning. The ctest guards, `tidy-files.py` and `clang-format --dry-run` on the changed C++, `check-allman-braces.py`,
`test-diag-overhead.py`, `check-diag-runbooks.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: every lane passes `crd-diag-overhead-render-validation-core` (WARP with the debug layer on
Windows, lavapipe with the pinned layer on Linux) and the Linux lanes `-sync`; the DIAG.7a(f-3) case passes with its
three new legs on the Windows lanes; the frame-cook output-clear case and the Vulkan suites pass everywhere.

## Not done in this batch

- (9) GPU-assisted validation's cost (see the board), and the generation-compare, recording and probe Cost rows.
- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads this run's only user message does not approve;
  (8) waits for the user's elevated recording.

Follow-up for an extra batch: the other standalone targets' clear values (colour, MSAA, VRS, stencil, G-buffer and
visibility targets) against the graphs that clear them; which two uploads of one Vulkan batch overlap (a redundant
upload as well as the race the barrier now orders).
