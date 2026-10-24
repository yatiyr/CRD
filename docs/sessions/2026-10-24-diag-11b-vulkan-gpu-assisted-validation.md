# DIAG.11b (9): Vulkan GPU-assisted validation's cost and the layer's cost notices, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [D3D12 GPU-based validation](2026-10-24-diag-11b-gpu-based-validation.md).

## CI read at the start

`gh run list --branch main --limit 15`: every completed run after 40067359705 was cancelled with no failed job. The
one failed job is linux-gcc-public-checks (120819184913) of the scheduled run 40075929537, head `420aaf1a`:
`held.fibers[0].high_water == 9U` read 8, the high-water reading that `d96eae30` repairs. (l) zstd 1.5.7 and (o)
clang-tidy 20 stay open: this run's only user message approves no download.

## Finding

`crd-diag-overhead --workload render --validation gpu --check` on linux-gcc-shipping (lavapipe, the pinned 1.4.341.1
layer) failed on ten `GPUAV-Compile-time-general-buffer` warnings, one per `vkCreateShadersEXT`, the tenth with the
layer's duplicate-limit note. The message says the shader has so many storage/uniform buffer bounds checks that it will
compile and run slowly, and names `gpuav_force_on_robustness` as the remedy.

- **Threshold.** The layer's code (its symbols and line table, no download):
  `DescriptorClassGeneralBufferPass::Instrument`, `descriptor_class_general_buffer_pass.cpp:261`, warns when the
  pass's instrumentation count is above 75 (`cmp $0x4b; ja`). It is fixed; no setting moves it.
- **Which shaders.** `VK_LAYER_GPUAV_DEBUG_PRINT_INSTRUMENTATION_INFO` and `..._DUMP_INSTRUMENTED_SHADERS`: 19 of the
  125 shader modules exceed 75 (104 x10, 107 x7, 187 x2, all vertex shaders; the largest fragment shader has 70). The
  187 one makes 165 scalar `OpAccessChain` loads from the one `uint` runtime array at set 0, binding 0: CKIR's
  word-addressed storage buffer, one check per 32-bit word (vertex pulling and instance data). The count follows
  from that layout; it is not a defect, and every check still runs.
- **Message type.** A probe in the capture callback: all ten arrive as `VALIDATION` type at `WARNING` severity, the
  same as API-misuse warnings, so the type cannot tell them apart; the message-ID name can.

`gpuav_force_on_robustness` would skip the checks robustness covers, so the arm would measure less validation; it is
not used.

## Change

The notice reports the instrumentation's own cost, which is what this arm measures, not the application's use of the
API. The capture classifies it the way it already classifies the layer's setup refusal
(`WARNING-GPU-Assisted-Validation`, DIAG.7c(e)), by its pinned message-ID name:

- `engine/gpu/gpu-context/include/crd/gpu/validation.hpp`: `ValidationReport::cost_notices`; `clean()` unchanged.
- `engine/gpu/gpu-context-vulkan/src/vulkan_validation_capture.cpp`: a warning whose name starts
  `GPUAV-Compile-time-` (the general and texel buffer passes) counts in `cost_notices`, not `warnings`; an error under
  that name still counts as an error. `ValidationMessage::cost_notice` marks the record.
- `tests/support/diag-overhead`: the record's `validation` object reports `cost_notices`, the harness prints no
  notice, and `crd-diag-overhead-render-validation-gpu` runs on Linux (3.1 s on linux-gcc-shipping, 3.2 s on
  linux-gcc-debug; 120 s limit).
- `tests/gpu/gpu-context-vulkan/test_diag_validation_identity.cpp`: the synthetic-message case gains a step: a
  notice is counted apart, an error under the same name and a warning under another name still count.

No new file besides this note; `diag-overhead.py` needed no change (it judges the four failure counts only).

## Board

[Board](../bench/2026-10-24-diag-11b-device-validation.md), linux-gcc-shipping on lavapipe, 12 processes per arm, binary
`42304733741a`: GPU-assisted validation costs +80.3% frame time (+79.6 .. +83.3) and +129.4% CPU per frame (+124.0 ..
+132.2); core +12.0% and +3.1% in the same binary. Each GPU-assisted process: errors 0, warnings 0, lost 0,
instrumentation failures 0, cost notices 10.

## Teeth

Each restored, rebuilt and green again:
- the prefix renamed (`GPUAV-Compile-time-renamed`): `crd-diag-overhead-render-validation-gpu` fails on
  linux-gcc-shipping ("a message was reported or lost");
- the severity guard dropped: the identity case fails on linux-gcc-debug (the error counts 0 of 1, notices 2);
- the classification off: the same case fails (warnings 4 == 3, notices 0 == 1).

## Verification

linux-gcc-debug: the 25 Vulkan validation, DIAG and render-harness CTests (the identity case with 23 assertions, run
directly). win-debug: the same set plus the DX12 validation cases (29 CTests); crd-gpu-context-dx12-tests and
crd-perf-gpu-bridge build. linux-gcc-shipping: the six render CTests. The ctest guards, `tidy-files.py` and
`clang-format --dry-run` on the changed C++ (added lines keep the file's aligned groups), `check-allman-braces.py`,
`check-diag-runbooks.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: the Linux lanes pass `crd-diag-overhead-render-validation-gpu` on lavapipe with the pinned layer,
and every lane with a Vulkan device the identity case.

## Not done in this batch

- (9) the runbooks' other Cost rows: generation checks, recording, probes.

Follow-up for an extra batch: none.
