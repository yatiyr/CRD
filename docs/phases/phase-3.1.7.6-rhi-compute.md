# Phase 3.1.7.6 — `crd-rhi-compute`: compute pipeline substrate

**Status:** ✅ **CLOSED 2026-05-17** — all 6 slices SHIPPED in one day
(v0a + v0b + v0c + v0d + v0e + v0-close). ADR-0080 Accepted. System
doc `docs/systems/rhi-compute.md`. Cluster session log
`docs/sessions/2026-05-17-rhi-compute-v0-close.md`. Locked 2026-05-17 as the prerequisite substrate for Phase 3.1.7
v9 (geometry GPU + shader-helpers) per the eliteness audit. Sequenced
**between Phase 3.1.7 v8 close (delaunay ✅ 2026-05-17) and v9 (GPU
LBVH + V-HACD + shader-helpers)**. Same carved-out-sub-phase pattern as
3.1.7.5 `crd-units`.

**Why this exists.** The current `crd-rhi` + `crd-rhi-vulkan` is
graphics-only. v9a GPU LBVH (Karras 2012) and v9b GPU BVH refit both
require compute pipelines, storage-buffer descriptor bindings, compute
dispatch, atomics, and compute↔graphics synchronization — none of which
the RHI exposes today. Shipping LBVH on top of a phantom compute
substrate is the exact half-baked anti-pattern the project rejects.
Phase 3.1.7.6 ships the missing substrate so v9 has real infrastructure
to consume.

**ADR:** ADR-0080 (Proposed; mint at v0a close per the established
ADR-at-slice-time pattern; ADR-0079 / ADR-0078 precedents).

**Consumers of the substrate (already enumerated):**
- Phase 3.1.7 v9a / v9b — GPU LBVH builder + GPU BVH refit.
- Phase 3.1.7 v9e — shader-helpers GLSL/HLSL ULP conformance test
  (compiles emitted shader, dispatches against C++ reference).
- Phase 3.1 eylem v8 — GPU broadphase, GPU rigid-body integration
  (deferred until eylem v1c resumes per Strategic Execution Plan).
- Phase 3.1.5 `crd-sdf` v5 — GPU 3D-texture upload + GLSL helper
  evaluation (consumed via v9e shader-helpers).
- Phase 3.1.6 `crd-hesap-gpu` — BLAS / FFT GPU mirror (consumes the
  same compute pipeline + storage-buffer substrate; no narrow version).
- Phase 3.5+ `crd-renderer` compute passes — DFAO / DFGI / GPU
  particles / GPU culling.

**Cornerstones (will be added to `docs/PRINCIPLES.md` once ADR-0080
lands):**
- Compute is a first-class RHI primitive, not a render-path-internal hack.
- Storage buffers + push constants + specialization constants are the
  three binding paths; no SSBOs hidden behind uniform buffers.
- Compute shaders compile via the same shaderc + spirv-reflect pipeline
  graphics shaders use (one frontend, one cache, one hot-reload).
- Determinism contract: GPU compute is **throughput-tier by default**;
  workloads requiring bit-exact reproducibility ship CPU reference
  paths (matches ADR-0063 §4).
- Async compute queue is **opt-in**, not default. Single-queue path
  ships first; async ships when a real consumer (eylem v8 GPU
  broadphase) requires it.

## Module layout

No new module. `crd-rhi-compute` is an **extension of existing
`engine/rhi/` (backend-agnostic) + `engine/rhi-vulkan/` (Vulkan impl)**.
Additive only — every change adds new interface types or methods; no
existing graphics surface is renamed or removed. This contains scope
drift to a single risk axis (new compute surface) instead of two
(graphics churn + compute new).

```
engine/rhi/include/crd/rhi/
  compute_pipeline.hpp      NEW — IComputePipeline + ComputePipelineDesc
  storage_buffer.hpp        NEW — IStorageBuffer + StorageBufferDesc
  compute_descriptors.hpp   NEW — descriptor layout types for compute
  command_buffer.hpp        EXTENDED — dispatch / dispatch_indirect / bind_compute_*
  device.hpp                EXTENDED — create_compute_pipeline / create_storage_buffer
  queue.hpp                 EXTENDED — formalize compute queue type

engine/rhi-vulkan/src/
  vulkan_compute_pipeline.cpp   NEW
  vulkan_storage_buffer.cpp     NEW
  vulkan_command_buffer.cpp     EXTENDED — compute path
  vulkan_device.cpp             EXTENDED — async compute queue selection
```

## Slice plan

| Slice | Content | LOC engine | LOC tests | Time |
|---|---|---|---|---|
| **v0a** ✅ 2026-05-17 | **`rhi-compute-types`.** `ComputePipeline` interface + `ComputePipelineDesc` + `Device::create_compute_pipeline` factory + Vulkan impl (`VulkanComputePipeline`, `vkCreateComputePipelines`). **D2 REVISED at slice start**: storage buffers reuse existing `Buffer` interface with `BufferUsage::Storage` flag — no separate `IStorageBuffer` split. Existing RHI already abstracted past per-usage-type model (`Buffer` has just `desc()` / `map()` / `unmap()`; per-usage differences live at descriptor-write + barrier sites). **D7 SOFTENED**: descriptor-set conventions become documented guideline, not type-level enforcement; `PipelineLayoutDesc` accepts any caller-built layout. Pipeline naming pin: `Pipeline` kept (graphics) + `ComputePipeline` standalone — no shared base (bind-point divergence is not polymorphism). Smoke test: minimal `compute_v0a.comp` GLSL → SPIR-V → pipeline lifecycle ASan-clean. **5 Vulkan-side cases** (null-shader reject / wrong-stage reject / valid creation / caller-provided layout / 8-cycle ASan multi-create) + **5 fake-side contract cases**. **11 cases / 69 assertions PASS, 4-config DoD PASS elapsed 00:30.** Session log `docs/sessions/2026-05-17-rhi-compute-v0a-types.md`. | ~360 + ~280 tests | done |
| **v0b** ✅ 2026-05-17 | **`rhi-compute-dispatch`.** `ICommandBuffer::bind_compute_pipeline` + `bind_compute_descriptor_sets(layout, first_set, sets)` + `dispatch(x, y, z)` + `dispatch_indirect(buffer, offset)`. **D5 reuse**: existing `push_constants(layout, stages, ...)` already takes layout + stages mask, so `ShaderStage::Compute` in the mask routes correctly — no compute-specific push method. **D6 (spec const baked at create-time)**: extended `ComputePipelineDesc` with `specialization_entries` + `specialization_data` spans threaded through `VkSpecializationInfo`. **D4 pinned**: dispatch params = workgroup counts, not thread counts. New `BufferUsage::Indirect` flag for `dispatch_indirect`. **Rejected the phase-doc-suggested `bind_compute_storage_buffer(set, binding, buffer)` shortcut** as v9-LBVH convenience that belongs above the RHI — existing `DescriptorAllocator` + `update_buffer` + `bind_compute_descriptor_sets` composes cleanly (the first-light test proves it). **v0a bug uncovered + fixed alongside**: `BufferUsage::Storage` was in the enum but `to_vk_buffer_usage` never mapped it to `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`; v0a's test passed because Vulkan accepts buffer with default usage, but using it as SSBO at descriptor bind would have failed validation. **First-light smoke**: compile compute shader → bake `kBaseOffset=1000` spec const → bind pipeline + descriptor set → `dispatch(1,1,1)` → 64 threads → submit + fence-wait → host readback validates each element == `i + 1000`. Direct dispatch + `dispatch_indirect` both green. **8 cases / 174 assertions PASS, 4-config DoD PASS elapsed 01:15.** Session log `docs/sessions/2026-05-17-rhi-compute-v0b-dispatch.md`. | ~280 + ~270 tests | done |
| **v0c** ✅ 2026-05-17 | **`rhi-compute-sync`.** `CommandBuffer::buffer_barrier(Buffer&, BufferAccess from, BufferAccess to)` typed-enum API + Vulkan `VkBufferMemoryBarrier` impl. **Naming pin** (advisor lock): `buffer_barrier` not `compute_barrier` — API is resource-typed, mirrors `transition_image`. **Granularity pin**: `BufferAccess` has separate `ComputeShaderRead` / `VertexShaderRead` / `FragmentShaderRead` / `VertexAttributeRead` / `IndexRead` / `UniformRead` / `IndirectRead` / `TransferSrc` / `TransferDst` / `HostRead` / `HostWrite` variants (no "GraphicsRead" collapse) — lets impl pick exact `srcStageMask`/`dstStageMask` without over-barrier. **`ImageAccess` extended** with `ComputeShaderRead`/`Write`/`ReadWrite` variants — discovered during slice that existing `ImageAccess::ShaderRead` was wired only to `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`; back-compat preserved (existing `ShaderRead` stays graphics-fragment-only). Same-queue path only; cross-queue ownership transfer = v0d. Single-buffer signature; span-batched variant filed as follow-on. **First-light**: two compute passes with barrier between them — pass 1 writes `i + 1000` to `buf_a`; `buffer_barrier(buf_a, ComputeShaderWrite → ComputeShaderRead)`; pass 2 reads `buf_a`, writes `2 * buf_a[i]` to `buf_b`; host readback validates `buf_b[i] == 2 * (i + 1000)`. Validation discriminator manually confirmed: barrier-less version triggers `VUID-vkCmdDispatch-None-...` write-after-write hazard. **5 cases / 90 assertions PASS, 4-config DoD PASS elapsed 00:29 (after 3 lint-class mid-slice fixes: tidy `bufA→buf_a`, non-ASCII `→` in test name, FakeCommandBuffer cascade)**. Session log `docs/sessions/2026-05-17-rhi-compute-v0c-sync.md`. | ~180 + ~210 tests | done |
| **v0d** ✅ 2026-05-17 | **`rhi-compute-async`.** Async compute queue + binary semaphores + full `SubmitInfo` shape. New types: `Semaphore` interface (binary; timeline deferred), `PipelineStage` enum (8 variants for `SemaphoreWait`), `AsyncComputePolicy` enum (`FallbackGracefully` default + opt-in `RequireDedicated`), `SemaphoreWait`/`SubmitInfo` structs, `DeviceDesc::async_compute_policy` field. New `Device` methods: `create_semaphore`, `compute_queue()`, `has_dedicated_compute_queue()`. New `Queue::submit(const SubmitInfo&)` overload (existing 3 overloads kept for v0a/b/c back-compat). Vulkan-side: queue family probe at device creation (`VK_QUEUE_COMPUTE_BIT && !VK_QUEUE_GRAPHICS_BIT`); creates second `VkDeviceQueueCreateInfo` for dedicated family if present. **Pointer-identity contract** (advisor lock): `compute_queue()` returns the SAME `Queue&` as `graphics_queue()` on fallback so consumers can dispatch via `&` comparison to skip cross-queue setup. **First-light**: pass 1 on `compute_queue` writes `i + 1000` to `buf_a` + signals semaphore; pass 2 on `graphics_queue` waits on semaphore at `ComputeShader` stage + reads `buf_a` + writes `2 * buf_a[i]` to `buf_b` + signals fence; host wait + readback validates `buf_b[i] == 2 * (i + 1000)`. Test passes on BOTH hardware paths (dedicated compute queue OR fallback). Validation discriminator manually confirmed: removing the `SemaphoreWait` on dedicated hardware fires `VUID-vkCmdDispatch-None-...` write-after-write hazard. **7 cases / 88 assertions PASS, 4-config DoD PASS elapsed 01:16 (no lint surprises).** Session log `docs/sessions/2026-05-17-rhi-compute-v0d-async.md`. | ~390 + ~290 tests | done |
| **v0e** ✅ 2026-05-17 | **`rhi-compute-shader-pipeline`.** Extended `crd-shader` Effect/Variant/Runtime reflection for compute. New `Module` accessors: `workgroup_size() → std::optional<WorkgroupSize>` (compute only; `nullopt` for graphics — `optional` over `{0,0,0}` sentinel, advisor pin), `specialization_constants() → ConstSpan<SpecializationConstantReflection>` (per-module, stage-agnostic, no auto-merge). New types: `WorkgroupSize { x, y, z }`, `SpecializationConstantReflection { constant_id, name, size_bytes, default_value_u32 }`. **Minimal-v0e scope** (advisor lock): reflection + hot-reload coverage only; no high-level `Effect → ComputePipelineDesc` accessor (v9-LBVH builds it directly from Module spv bytes + reflected layout). Compute Effects = single-variant rule (VariantRequest graphics fields ignored). Spec const `size_bytes` pinned at 4 (64-bit spec consts are rare; revisit per consumer). **Mid-slice spirv-reflect API correction**: actual member names are `spec_constant_count` + `spec_constants` (not `specialization_*`); default value is `void*` + explicit `default_value_size`; first u32 memcpy preserves int sign + float bit pattern alike — no typed dispatch needed. Pattern locked: grep `$VULKAN_SDK/Source/SPIRV-Reflect/spirv_reflect.h` for actual struct shape before assuming docs. **Discriminator test** (`compute_v0e_reflection.comp`): workgroup `(32, 4, 2)` (X/Y/Z swap fails loudly) + `layout(constant_id = 7) const uint kFoo = 42` + `layout(constant_id = 3) const int kBar = -1` (signed-bit pattern u32 = 0xFFFFFFFF). Wrong constant_id is worse than missing reflection. **5 cases / 35 assertions PASS, 4-config DoD PASS elapsed 00:58.** Session log `docs/sessions/2026-05-17-rhi-compute-v0e-shader-pipeline.md`. | ~80 + ~150 tests | done |
| **v0-close** ✅ 2026-05-17 | **Phase 3.1.7.6 CLOSED.** ADR-0080 **Promoted Proposed → Accepted** with full v0a-v0e amendments folded in (D1-D12 + D2 revision storage-reuses-Buffer + D7 softened conventions-become-guideline + D8 renamed `compute_barrier→buffer_barrier` + granular `BufferAccess` enum + Pipeline+ComputePipeline standalone naming pins + `compute_queue()` pointer-identity contract + minimal-v0e reflection scope + Compute-Effects-single-variant rule + 5 follow-on slices filed). NEW `docs/systems/rhi-compute.md` system doc (architecture diagram + when-to-use-what table + recommended descriptor conventions + determinism contract + async compute policy + two-layer typed architecture + test coverage + integration touch-points). ROADMAP/context/MEMORY sync. 18-config full sweep. Cluster session log `docs/sessions/2026-05-17-rhi-compute-v0-close.md`. | docs + sweep | done same day |
| **TOTAL** | **6 slices** ✅ ALL SHIPPED 2026-05-17 | **~1290** (vs ~2800 est) | **~1200** (vs ~1800 est) | **5 days** (vs ~4 wk budget; ~5-6× ahead of plan) |

## Decisions to lock per slice (carry into ADR-0080)

**v0a:**
- D1. Compute pipeline factory is `IDevice::create_compute_pipeline(const ComputePipelineDesc&)` — no global / no singleton.
- D2. Storage buffer is a distinct interface type (`IStorageBuffer`), not a reskin of `IBuffer` — usage flags differ (SSBO vs vertex/index), and the descriptor-binding path differs.
- D3. Descriptor-set layout for compute: set 0 = storage buffers, set 1 = uniform buffers, push-const block reserved for per-dispatch params (workgroup-grid size, iteration index, etc.). Future high-level layers may add set 2+ for textures.

**v0b:**
- D4. `dispatch(x,y,z)` parameters are workgroup counts, not thread counts (matches Vulkan + DirectX convention; thread count = workgroup count × `local_size_*` from shader).
- D5. Push constants for compute share the same 128-byte budget as graphics (Vulkan minimum guarantee).
- D6. Specialization constants are baked into the pipeline at create-time (not per-dispatch); per-dispatch parameters use push constants.

**v0c:**
- D7. Compute↔graphics barriers expose a typed-enum API (`BufferAccess::ComputeWrite | GraphicsRead`), not raw `VkAccessFlags` — backend-agnostic by design.
- D8. Same-queue path issues barriers via `vkCmdPipelineBarrier`; cross-queue path (v0d) uses semaphores + queue ownership transfers.

**v0d:**
- D9. **Async compute policy:** `RequireDedicated` (fail device creation if no separate compute queue family) vs `FallbackGracefully` (use graphics queue + log warning). **Recommendation:** `FallbackGracefully` default + opt-in `RequireDedicated` for performance-critical consumers (eylem v8 GPU broadphase, future).
- D10. Cross-queue semaphore creation is `IDevice::create_semaphore()` returning `ISemaphore`; barrier-with-semaphore is `ICommandBuffer::signal_semaphore(sem) + wait_semaphore(sem, stage_mask)`.

**v0e:**
- D11. Compute shaders use the same `.glsl` extension + path convention as graphics; the shaderc frontend dispatches on `#pragma shader_stage(compute)` or filename `*.comp.glsl` (final decision at v0e).
- D12. Hot-reload preserves the last-good pipeline + logs the failure via `crd-log` (matches graphics behavior; no new policy).

## Definition of Done

Same per-slice 4-config DoD as the rest of Cerid (`scripts/per-slice-check.ps1 -Parallel`): win-debug + win-asan + win-shipping + win-tidy. **Phase 3.1.7.6 close additionally runs the 18-config full sweep.** The new compute path must be exercised under win-asan to catch the usual storage-buffer aliasing + barrier-mismatch hazards.

## Risk register

- **RHI scope drift.** Mitigated by additive-only contract (ADR-0080 §1). No existing graphics surface changes; every new type / method is new.
- **GPU determinism.** GPU compute is throughput-tier by ADR-0063 + ADR-0080 §3; deterministic workloads (BLAS, LBVH conformance) ship CPU reference paths. v9a-close pins the CPU↔GPU conformance contract.
- **Async compute hardware variance.** Mitigated by `FallbackGracefully` default (D9). All consumer-driven tier hardware ships with dedicated compute queues; integrated GPUs sometimes don't — fallback handles them.
- **Calendar.** Phase 3.1.7.6 adds ~4 weeks to Phase 3.1.7 close calendar. Accepted by user 2026-05-17 with full visibility per Option B of the v9 eliteness audit.

## After this sub-phase

Phase 3.1.7 v9 unbundles into 16 sub-slices consuming the new compute
substrate (v9c V-HACD + v9a GPU LBVH + v9b GPU BVH refit + v9e
shader-helpers). See `docs/phases/phase-3.1.7-geometry.md` for the
v9 plan. After v9 close → v10 `-curves` → v11 transform-aware helpers
→ Phase 3.1.7 fully closes, then `crd-hesap-dense` v0, then eylem v1c
resumes.
