# `crd-rhi` compute substrate

**Status:** ✅ Phase 3.1.7.6 CLOSED 2026-05-17 (v0a + v0b + v0c + v0d + v0e + v0-close). ADR-0080 Accepted.

**One-paragraph summary.** Compute pipelines + storage-buffer access + dispatch + cross-stage barriers + async compute queue + binary semaphores + spec-const & workgroup reflection. Additive extension of the existing `engine/rhi/` (interface) + `engine/rhi-vulkan/` (impl) — no new module; existing graphics surface untouched. Designed for the multi-consumer surface (Phase 3.1.7 v9 GPU geometry, Phase 3.1 eylem v8 GPU broadphase, Phase 3.1.5 sdf v5 GPU 3D-texture, Phase 3.1.6 hesap-gpu BLAS/FFT, Phase 3.5+ renderer compute passes).

## When to use what

| Goal | Use | Notes |
|---|---|---|
| Create a compute pipeline | `Device::create_compute_pipeline(ComputePipelineDesc)` | shader + layout + optional spec consts |
| Spec const at create-time | `ComputePipelineDesc::specialization_entries` + `specialization_data` | baked into VkSpecializationInfo (ADR-0080 D6) |
| Per-dispatch parameter | `CommandBuffer::push_constants(layout, ShaderStage::Compute, ...)` | reuses graphics push-const method (D5) |
| Bind pipeline + descriptors | `bind_compute_pipeline` + `bind_compute_descriptor_sets` | sibling methods to graphics (different VK bind point) |
| Dispatch | `dispatch(group_x, group_y, group_z)` | **workgroup counts**, not threads (D4) |
| Indirect dispatch | `dispatch_indirect(buffer, offset)` | buffer needs `BufferUsage::Indirect` |
| Sync compute↔graphics on same queue | `buffer_barrier(buf, from, to)` (or `transition_image`) | typed-enum API; same-queue only |
| Sync across queues | `Queue::submit(SubmitInfo)` with wait/signal semaphores | binary `Semaphore`; `PipelineStage` for wait-stage |
| Async compute queue access | `Device::compute_queue()` | returns same as `graphics_queue()` on fallback (D9) |
| Check if real async | `Device::has_dedicated_compute_queue()` | for consumer optimization + perf-UI |
| Storage buffer | `create_buffer({size, BufferUsage::Storage, ...})` | reuses existing Buffer (D2 revision) |
| Inspect workgroup size | `Module::workgroup_size()` returns `optional<WorkgroupSize>` | compute modules only |
| Inspect spec consts | `Module::specialization_constants()` | per-module, stage-agnostic |

## Architecture

```
engine/rhi/include/crd/rhi/             ← backend-agnostic interfaces
  compute_pipeline.hpp                  NEW (v0a) — ComputePipeline + ComputePipelineDesc
  semaphore.hpp                         NEW (v0d) — binary Semaphore
  types.hpp                             EXTENDED (v0a-d) — ComputePipelineDesc, BufferAccess,
                                          ImageAccess compute variants, PipelineStage,
                                          AsyncComputePolicy, SemaphoreWait, SubmitInfo,
                                          SpecializationConstantEntry, BufferUsage::Indirect
  command_buffer.hpp                    EXTENDED (v0b-c) — bind_compute_pipeline,
                                          bind_compute_descriptor_sets, dispatch,
                                          dispatch_indirect, buffer_barrier
  device.hpp                            EXTENDED (v0a, v0d) — create_compute_pipeline,
                                          create_semaphore, compute_queue,
                                          has_dedicated_compute_queue
  queue.hpp                             EXTENDED (v0d) — submit(SubmitInfo)

engine/rhi-vulkan/src/vulkan_backend.cpp ← Vulkan impl
  VulkanComputePipeline                 NEW (v0a) — VkPipeline + owned VkPipelineLayout
  VulkanSemaphore                       NEW (v0d) — VkSemaphore lifecycle
  to_vk_pipeline_stage                  NEW (v0d) — PipelineStage → VkPipelineStageFlags
  to_vk_buffer_access_info              NEW (v0c) — BufferAccess → {stage, access}
  (VulkanCommandBuffer impls of compute methods + buffer_barrier + dispatch)
  (VulkanDevice queue family probe + dedicated compute queue handle)
  (VulkanQueue::submit(SubmitInfo))
  (to_vk_buffer_usage extended for Storage + Indirect flags)

engine/shader/                          ← Effect/Variant/Runtime reflection (v0e)
  types.hpp                             EXTENDED — WorkgroupSize, SpecializationConstantReflection
  effect.hpp                            EXTENDED — Module::workgroup_size, specialization_constants
  src/runtime.cpp                       EXTENDED — reflect_module pulls workgroup + spec consts
```

## Module dep graph

`crd-rhi-compute` is an extension of `crd-rhi` + `crd-rhi-vulkan`; no new module. Consumer modules link `crd-rhi` for the interface + `crd-rhi-vulkan` (or future `crd-rhi-dx12`/`-metal`) for the backend. `crd-shader` extension is part of `crd-shader` proper — same link surface.

## Recommended descriptor-set conventions for compute

**These are guidelines, not type-level enforcement** (ADR-0080 D7 softened at v0-close). `PipelineLayoutDesc` accepts any caller-built layout; consumers compose freely. The conventions below are what `crd-rhi-compute` consumers SHOULD do unless they have a specific reason not to:

- **set 0 = storage buffers** (input + output SSBOs)
- **set 1 = uniform buffers** (per-dispatch read-only data; small/frequent updates)
- **push constants for per-dispatch params** (workgroup-grid size, iteration index, time, etc. — 128-byte budget per Vulkan minimum guarantee)
- **set 2+ available for textures / sampled images** if needed

v9 LBVH builder consumer follows this pattern. Future consumers may diverge if their access patterns demand it.

## Determinism contract

GPU compute is **throughput-tier by default** (ADR-0080 D3, inherits ADR-0063 §4). Hardware-vendor variations in atomic ordering, thread scheduling, and warp/wave size make bit-exact reproducibility across hardware impossible without explicit serialization. Workloads requiring determinism ship CPU reference paths:

- v9a GPU LBVH ships throughput-tier; CPU `bvh_build` stays the deterministic reference; ULP-conformance test pins the contract.
- Future `crd-hesap-gpu` BLAS/FFT follows the same pattern.

If a consumer needs deterministic GPU compute, options are: (a) single-threaded compute dispatch (`local_size_x = 1`, single workgroup), (b) reduction with deterministic-by-construction algorithms (e.g., tree-reduction with fixed ordering), (c) post-pass correction on CPU. None of these are RHI concerns — RHI exposes the substrate; consumer picks the pattern.

## Async compute policy

**`FallbackGracefully` is the default** (ADR-0080 D9). At device creation, Vulkan-side probes for a queue family with `VK_QUEUE_COMPUTE_BIT` set and `VK_QUEUE_GRAPHICS_BIT` clear (dedicated compute queue). If found, `Device::compute_queue()` returns the dedicated queue's wrapper; if not, returns the same `Queue&` as `graphics_queue()`.

**Pointer-identity contract**: consumers MAY dispatch on `&compute_queue() == &graphics_queue()` to skip cross-queue setup when on fallback. Documented in `device.hpp` and tested in
`tests/rhi_vulkan/test_rhi_vulkan.cpp::"Vulkan compute_queue pointer-identity contract"`.

Consumers needing real async (eylem v8 GPU broadphase, future) may opt in to `AsyncComputePolicy::RequireDedicated` via `DeviceDesc`, which fails device creation if hardware does not expose a dedicated compute queue family. Use sparingly; consumer-grade hardware varies.

## Two-layer typed architecture (ADR-0078 §5)

`crd-rhi` is the lower-layer raw-scalar substrate per ADR-0078 §5: raw `f32` / `u32` / byte buffers throughout. Typed (`Quantity<D, T>`) values cross at consumer boundaries (e.g., a scene system pulling `Vec3<Length>` from an ECS component → `.value` to raw before uploading to a storage buffer). No `crd-units` dependency at the RHI layer.

## Test coverage

| Test file | Cases | Assertions |
|---|---|---|
| `tests/rhi/test_rhi.cpp` (v0a-d fake-side) | 19 | 100+ |
| `tests/rhi_vulkan/test_rhi_vulkan.cpp` (v0a-d Vulkan) | 12 | 350+ |
| `tests/shader/test_runtime.cpp` (v0e reflection) | 5 | 35 |
| **TOTAL** | **36** | **~485** |

Discriminating tests (won't pass if reflection / barriers / dispatch are silently wrong):
- v0b first-light: 64-element storage buffer write+readback with `kBaseOffset = 1000` spec const baked
- v0c two-pass barrier chain: pass 1 writes `i + 1000`, barrier, pass 2 writes `2 * buf[i]`, readback validates `2*(i+1000)`
- v0d async cross-queue: same two-pass with cross-queue semaphore (works on dedicated OR fallback)
- v0e reflection: workgroup `(32, 4, 2)` X/Y/Z swap fails; spec const `kFoo` (id=7, default=42) vs `kBar` (id=3, default=-1 → u32 0xFFFFFFFF) wrong-id-or-default fails

## Performance pins

- **Single-buffer barrier** (no batched span variant). Consumer-driven scoping — v9 LBVH single-buffer path doesn't need batching. Filed follow-on if needed.
- **No global memory barrier** (whole-pipeline `vkCmdPipelineBarrier` without buffer/image targets). Per-resource only — explicit by design.
- **`UniformRead` covers all three shader stages** (vertex / fragment / compute) since uniform-buffer reads can come from any. Consumers needing tighter scope use the stage-specific `*ShaderRead` variants.
- **Spec const `size_bytes` pinned at 4** (32-bit scalars). 64-bit spec consts ship when a consumer asks.

## Integration touch-points

- **Phase 3.1.7 v9c V-HACD**: cooker-only, no GPU; uses `crd-rhi-compute` indirectly only if a future GPU V-HACD lands.
- **Phase 3.1.7 v9a/b GPU LBVH + refit**: primary consumer. Builds Morton codes + Karras tree + AABB upsweep on the compute queue.
- **Phase 3.1.7 v9e shader-helpers**: ULP-conformance test compiles emitted GLSL and dispatches against CPU reference via this substrate.
- **Phase 3.1 eylem v8 GPU broadphase** (future): full async compute with dedicated queue + cross-queue semaphores.
- **Phase 3.1.5 crd-sdf v5 GPU 3D-texture path** (future): texture upload + dispatch.
- **Phase 3.1.6 crd-hesap-gpu** (future): BLAS L1/L2/L3 + FFT compute kernels.
- **Phase 3.5+ renderer compute passes** (future): DFAO, DFGI, GPU particles, GPU culling.

## Filed follow-on slices

Not regressions — explicit-scope deferrals:

- **`crd-rhi-compute-batch`** — span-batched variants of `buffer_barrier` + `SubmitInfo`. Ships when v9 LBVH or eylem v8 broadphase profile shows per-call overhead matters.
- **`crd-rhi-compute-timeline`** — timeline semaphores (Vulkan 1.2). Ships when an Effect needs ordered multi-step async work (eylem v8 frame-graph candidate).
- **`crd-rhi-compute-validation-hook`** — programmatic debug-messenger capture for tests so v0c/v0d validation discriminators can be programmatically asserted instead of manually confirmed.
- **`crd-rhi-compute-image-storage`** — `Image` flag + descriptor wiring for storage images (currently only buffers exercised; `ComputeShaderRead/Write/ReadWrite` ImageAccess variants are ready).
- **`crd-rhi-compute-effect-to-pipeline`** — high-level helper that builds a `ComputePipelineDesc` from a `crd::shader::Module`. Ships when a consumer asks for it (v9-LBVH currently builds directly at the call site, one-line).

## References

- ADR: `docs/decisions/0080-crd-rhi-compute.md`
- Phase doc: `docs/phases/phase-3.1.7.6-rhi-compute.md`
- Per-slice session logs: `docs/sessions/2026-05-17-rhi-compute-v0{a,b,c,d,e}-*.md`
- Cluster session log: `docs/sessions/2026-05-17-rhi-compute-v0-close.md`
- Determinism contract: ADR-0063 §4
- Two-layer typed architecture: ADR-0078 §5
