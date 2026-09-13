# DX12 workload repair contract

<!-- doc-role: contract -->
> Live owner and child order: [REPO.3c.5](../ROADMAP.md#slice-repo.3c.5). Rules: [AGENTS](../../AGENTS.md).

## Purpose and evidence

Repair the existing authored renderer's failing DX12 consumers without changing algorithms, tolerances or asset
ownership. This is repository-verification repair, not acceptance of RAH-0/ADR-0107 or implementation of a new render
path. Preserve the original B18 hair/fur/scattering, RT-4 and impostor contract; local successes do not replace the
previously failing hosted tuple. [Current evidence](../sessions/2026-09-12-ci-continuation-and-dx12-workloads.md).

The actual impostor consumer passes on local Vulkan/DX12 hardware but fails on WARP: CPU survivors 25, GPU survivors
0 and black output. Temporary D3D12 debug-layer/InfoQueue instrumentation successfully initialized and reported
resource-state mismatches (527), invalid SRV descriptor placement (646), shader linkage errors (660) and warnings.
The 256-descriptor frame heap has unchecked cursor increments. A temporary 4,096-descriptor counterfactual still
failed on WARP; a larger constant alone is neither the diagnosis nor an acceptable fix. All temporary changes were
restored. Treat observed errors separately; do not infer their complete causal chain from the final black image.

## Reuse and ownership

- Provider implementation: [raster context](../../engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp) and
  [device/program context](../../engine/gpu/gpu-context-dx12/src/dx12_context.cpp).
- Existing shared/public GPU interfaces stay backend-neutral. Native SDK objects remain within the DX12 provider.
  Study the [Vulkan capture](../../engine/gpu/gpu-context-vulkan/include/crd/gpu/vulkan_validation_capture.hpp) and
  actual context creation/teardown before adding DX12 instrumentation; share contracts, not vendor types.
- Regression owners: [DX12 frame graph](../../tests/gpu/gpu-context-dx12/test_dx12_frame_graph.cpp),
  [raster tests](../../tests/gpu/gpu-context-dx12/test_dx12_raster.cpp) and
  [actual scene consumers](../../tests/rendering/scene-render/test_scene_render_gpu.cpp).
- The scene still loads the shipped authored frame/CEIR/CKIR assets. Buffer transitions and descriptor allocation
  are generic provider mechanisms. No C++ algorithm builder, demo path or geometry/physics rewrite is introduced.

## Ordered acceptance

The table owns status; this section defines requirements only.

1. **REPO.3c.5.a — reliable DX12 validation.** Enable validation before device creation; capture through a bounded,
   thread-safe, allocator-owned mechanism. Identify readiness, registration failure and dropped messages explicitly;
   unavailable instrumentation cannot qualify a silent run. Cover every participating device/context and startup,
   recording, submission, completion and teardown. Callbacks have explicit ownership/deregistration and cannot use
   destroyed storage. Check relevant HRESULTs and device-loss/fence outcomes instead of reporting successful execution
   from an invalid command list. Prove the instrument detects an intentional bounded invalid operation and a valid
   production path stays silent. No global ASan suppression, message filter hiding errors or permanent user setting.
2. **REPO.3c.5.b — resource-state correctness.** Audit the failing color indexed-indirect path against its balanced
   depth-only counterpart and subsequent compute/download consumers. Restore each scene/argument/count resource from
   its actual state; test null/separate/shared count buffers and repeated draw→compute→readback transitions. Do not
   duplicate barriers for aliased roles. Address the real consumer's observed depth/image state and alias activation
   errors under this owner. An absent aliasing barrier is a source finding until its affected execution is demonstrated.
   The [state/alias recipe](../recipes/2026-09-13-dx12-resource-states.md) and
   [reproduction/qualification](../sessions/2026-09-13-dx12-resource-state-repair.md) define the implemented native model.
3. **REPO.3c.5.c — safe frame descriptor allocation.** Replace unchecked slot arithmetic with checked contiguous
   reservations for every graphics/compute/RT/blit path. Support workloads exceeding the old 256 slots through bounded
   growth/paging and explicit allocation failure. A larger constant, wraparound, silent skipped draw or extra submission
   per draw is insufficient. Retain one graph submission, immutable descriptors for queued commands, correct table
   rebinding when heaps change, and fence-based retirement/reuse. Test exact capacity, one over, large multi-table
   draws, multiple passes/frames, cancellation/failure and existing authored consumers. Do not switch heaps halfway
   through a draw while keeping handles to a previous heap. Measure any claimed performance improvement separately.
   The [descriptor recipe](../recipes/2026-09-13-dx12-frame-descriptors.md) defines the implementation and its native limits.
4. **REPO.3c.5.d — full consumer close.** Resolve remaining emitted validation diagnostics, including the observed
   stage-linkage/attachment and resource-creation warnings; identify actual producer/consumer contracts before edits.
   Exercise B18, RT-4 and impostor unchanged oracles with hardware/WARP and the published failing-provider evidence.
   Assert nonzero execution, correct cull counts and visible pixels, validation silence and truthful skip accounting.
   Rebuild every affected executable, run incremental LLVM-20 analysis and applicable adjacent consumers. Retain missing
   CI evidence as Needs CI, never Done. Parent closure requires all children and its original complete contract.
5. **REPO.3c.7 — inner coverage as a qualified route.** The native bit stays the contract on qualified Tier-3
   providers; the documented software provider takes the barycentric pixel-corner route; both are held to one CPU
   oracle by the [route recipe](../recipes/2026-09-13-dx12-inner-coverage.md). No software skip, tier lie or oracle change.
6. **REPO.3c.10 — provider, not suppression; still open.** The DXR over-read reproduces only in the OS WARP build. The
   hash-verified 1.0.20 package fixes it but fails 13 bit-exact compute gates the OS build passes, so it was withdrawn
   from CI ([pinned-WARP recipe](../recipes/2026-09-13-dx12-pinned-warp.md)); ASan stays unsuppressed and the four
   hosted DXR gates remain open.

The persistent [validation implementation](../recipes/2026-09-12-dx12-validation-and-command-lifetimes.md) now owns
startup, callbacks, teardown and checked command completion across gpu-context-dx12 factories. Its
[local qualification and CI boundary](../sessions/2026-09-12-dx12-validation-foundation.md) are explicit; the original
temporary callback remains historical evidence. Standalone kir-dx12 is outside this registry (RAH-6.b). The advisor capability is unavailable; no review is claimed. Details may be refined from
source reuse/API inspection without dropping any requirement above or bypassing an explicit architecture gate.

## Primary references

Microsoft assigns [resource-state/barrier management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
to the application. Descriptor tables reference ranges in [shader-visible heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shader-visible-descriptor-heaps);
heap selection, table lifetime and rebinding must follow the [resource-binding specification](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html).
These references inform the repair; they do not establish that a proposed allocator or the existing renderer is correct.
