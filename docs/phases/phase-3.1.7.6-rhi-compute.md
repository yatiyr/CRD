# Phase 3.1.7.6 — `crd-rhi-compute`: compute pipeline substrate

**Status:** 📋 planned — locked 2026-05-17 as the prerequisite substrate
for Phase 3.1.7 v9 (geometry GPU + shader-helpers) per the eliteness
audit. Sequenced **between Phase 3.1.7 v8 close (delaunay ✅ 2026-05-17)
and v9 (GPU LBVH + V-HACD + shader-helpers)**. Same carved-out-sub-phase
pattern as 3.1.7.5 `crd-units`.

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
| **v0a** 📋 planned | **`rhi-compute-types`.** `IComputePipeline` interface + `ComputePipelineDesc` (entry point name + shader module + push-const range + descriptor-set layout). `IStorageBuffer` interface + `StorageBufferDesc` (size + usage flags + host-visible toggle + initial-state hint). Descriptor-set layout types for compute set 0 (storage buffers) + set 1 (uniform buffers) + push-const block (per-dispatch params). `Device::create_compute_pipeline` + `create_storage_buffer` factories return valid handles. **No execution path yet** — just types + factories. Smoke test creates a trivial compute pipeline + a 4-KB storage buffer; teardown clean under ASan. | ~800 | ~400 | ~5 days |
| **v0b** 📋 planned | **`rhi-compute-dispatch`.** `ICommandBuffer::bind_compute_pipeline` + `bind_compute_storage_buffer(set, binding, buffer)` + `push_compute_constants(offset, size, data)` + `dispatch(x, y, z)` + `dispatch_indirect(indirect_buffer, offset)`. Specialization constants threaded through `ComputePipelineDesc`. First-light smoke: bind pipeline, bind storage buffer initialised with `Array<u32>` zeros, dispatch one workgroup that writes `gl_GlobalInvocationID.x` to each element, readback, validate. | ~600 | ~400 | ~5 days |
| **v0c** 📋 planned | **`rhi-compute-sync`.** Pipeline barriers for compute↔graphics handoff via `ICommandBuffer::compute_barrier(BufferAccess::Write → ShaderRead)` etc. Memory dependencies map cleanly to Vulkan `VK_ACCESS_SHADER_WRITE_BIT` ↔ `VK_ACCESS_SHADER_READ_BIT`. **Same-queue path only** — no cross-queue semaphores in this slice. Smoke: compute pass writes to storage buffer → barrier → vertex shader reads same buffer as vertex pull buffer, draws something to swapchain, present. | ~400 | ~300 | ~3 days |
| **v0d** 📋 planned | **`rhi-compute-async`.** Async compute queue. Vulkan-side: probe `VkQueueFamilyProperties` for a queue family with `VK_QUEUE_COMPUTE_BIT` and **without** `VK_QUEUE_GRAPHICS_BIT` (dedicated compute queue); fall back to graphics queue if hardware does not expose one. Cross-queue semaphores for compute-write → graphics-read on a different queue. Frame-loop integration: graphics queue submits N+1 while compute queue runs N. **Open decision pin: require-vs-fallback policy** — locked at slice start per ADR-0080. | ~600 | ~400 | ~5 days |
| **v0e** 📋 planned | **`rhi-compute-shader-pipeline`.** Extend `crd-shader` for compute: GLSL `#version 450` + `comp` profile through existing shaderc + spirv-reflect pipeline. SPIR-V reflection extension parses workgroup size (`local_size_x/y/z`), specialization constants, and storage-buffer descriptor bindings. Compute-shader hot-reload via the existing `crd-shader` watch path; failed compile preserves last-good pipeline (matches graphics hot-reload contract). | ~400 | ~300 | ~3 days |
| **v0-close** 📋 planned | **Phase 3.1.7.6 CLOSED.** ADR-0080 Accepted + `docs/systems/rhi-compute.md` system overview + **18-config full sweep** (`scripts/full-sweep.ps1`) + integration smoke (trivial compute shader writes a known pattern to a storage buffer, CPU validates byte-exact). MEMORY/context/ROADMAP final sync + cluster session log. | docs + sweep | — | ~2 days |
| **TOTAL** | **6 slices** | **~2800** | **~1800** | **~4 weeks** |

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
