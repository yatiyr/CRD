# ADR-0080 — `crd-rhi-compute` substrate

**Status:** Proposed 2026-05-17 (Phase 3.1.7.6 v0a opening). Locks at v0-close per the established ADR-at-slice-time pattern (ADR-0078 / ADR-0079 precedents).

**Context.** Phase 3.1.7 v9 (geometry GPU + shader-helpers) requires GPU compute pipelines, storage-buffer descriptor bindings, compute dispatch, atomics, and compute↔graphics synchronization. The current `crd-rhi` + `crd-rhi-vulkan` is graphics-only — no `IComputePipeline`, no `IStorageBuffer`, no dispatch path. Shipping v9 on phantom infrastructure was rejected during the v9 eliteness audit 2026-05-17 (see Phase 3.1.7 v8-close session log + the choose-Option-B turn). Phase 3.1.7.6 carves out a prerequisite sub-phase to ship the missing substrate, mirroring how 3.1.7.5 `crd-units` was carved out as a substrate-prerequisite sub-phase.

The substrate has consumers beyond v9: Phase 3.1 eylem v8 GPU broadphase, Phase 3.1.5 `crd-sdf` v5 GPU 3D-texture path, Phase 3.1.6 `crd-hesap-gpu` BLAS / FFT GPU mirror, Phase 3.5+ renderer compute passes (DFAO / DFGI / GPU particles / GPU culling). The substrate must be designed for that multi-consumer surface, not narrowly for v9.

## Decisions

### D1. Additive-only RHI extension (no module split)

`crd-rhi-compute` is **not** a new module. It is an extension of the existing `engine/rhi/` (backend-agnostic) + `engine/rhi-vulkan/` (Vulkan impl). New interface types (`IComputePipeline`, `IStorageBuffer`, `ICompute*`) live in `engine/rhi/include/crd/rhi/`. Vulkan impls live in `engine/rhi-vulkan/src/`. **No existing graphics surface is renamed or removed.** This contains scope-drift risk to one axis (new compute surface) instead of two (graphics churn + compute new).

### D2. Storage buffer is a distinct interface type

`IStorageBuffer` is not a reskin of `IBuffer`. The usage flags differ (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` vs vertex/index/uniform), the descriptor-binding path differs (`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` vs uniform), the cache coherence requirements differ (SSBOs need explicit barriers for write-then-read; uniform buffers are immutable per-dispatch). Separate interfaces keep the consumer contract honest.

### D3. Compute is throughput-tier by default; deterministic reference paths stay CPU-side

Per ADR-0063 §4 (determinism is optional). GPU compute orderings vary by hardware vendor (atomic ordering, thread scheduling, warp/wave size). v9a GPU LBVH ships throughput-tier; CPU `bvh_build` stays the deterministic reference; ULP-conformance test pins the contract (`docs/phases/phase-3.1.7-geometry.md` v9a-close). Same pattern for v9b GPU BVH refit and future `crd-hesap-gpu`.

### D4. Async compute is opt-in, not default

Single-queue path (compute submitted to graphics queue) ships first in v0a–v0c. Async compute queue (dedicated `VK_QUEUE_COMPUTE_BIT` family, cross-queue semaphores) ships in v0d as opt-in. Reason: consumer-driven scoping — async compute is only worth its complexity when a frame-loop has actual graphics+compute parallelism to exploit (eylem v8 GPU broadphase, future renderer compute passes). v9 doesn't need it; v9a builds a BVH once at cook-time or once per dynamic batch.

### D5. Async compute hardware-variance policy: `FallbackGracefully` default

Vulkan-side: probe `VkQueueFamilyProperties` for a queue family with `VK_QUEUE_COMPUTE_BIT` **without** `VK_QUEUE_GRAPHICS_BIT`. If present: use it for async compute. If absent: fall back to graphics queue + log warning via `crd-log`. Opt-in `RequireDedicated` flag fails device creation when hardware does not expose a dedicated compute queue (for performance-critical consumers that cannot tolerate the fallback).

### D6. Shader pipeline: shaderc + spirv-reflect, one frontend

Compute shaders compile via the **same** shaderc + spirv-reflect frontend graphics shaders use. No second shader-compile system. Filename convention `*.comp.glsl` or `#pragma shader_stage(compute)` (final convention locked at v0e). Compute-shader hot-reload preserves the last-good pipeline on compile failure (matches graphics behavior; no new policy).

### D7. Descriptor-set layout: set 0 = storage, set 1 = uniform, push-const reserved

Conservative three-binding-path design. Set 0 holds all `IStorageBuffer` bindings; set 1 holds `IUniformBuffer` bindings; push constants hold per-dispatch parameters (workgroup-grid size, iteration index, time, etc.). The 128-byte push-const budget matches Vulkan's minimum guarantee. Higher-level layers may add set 2+ for textures or sampled images.

### D8. Backend-agnostic barrier API (typed enum, not raw flags)

`ICommandBuffer::compute_barrier(BufferAccess::ComputeWrite → BufferAccess::GraphicsRead)` takes a typed enum, not `VkAccessFlags`. Backend-agnostic by design; future DX12 / Metal backends translate the enum to their native barrier model.

## Consequences

**Positive.**
- v9 ships on real infrastructure, not phantom.
- Multi-consumer substrate ready for eylem v8, sdf v5, hesap-gpu, renderer Phase 3.5+ without per-consumer GPU-substrate re-litigation.
- RHI stays backend-agnostic; future DX12 / Metal backends extend the same compute interface.

**Negative.**
- Phase 3.1.7 close calendar grows by ~4 weeks. Accepted by user 2026-05-17 with full visibility.
- One more substrate to maintain (storage-buffer lifetime, descriptor-set allocator, barrier discipline).

**Pinned for future ADR amendments.**
- Async compute frame-loop integration policy (single queue vs separate frame-graph track) — locked when eylem v8 lands.
- GPU memory allocator (sub-allocation strategy for storage buffers) — currently rolls forward the VMA-like wrapper used for vertex/index buffers; revisit if compute workload sizes blow past that wrapper's design.

## References

- Phase doc: `docs/phases/phase-3.1.7.6-rhi-compute.md`
- Eliteness audit transcript: Phase 3.1.7 v8-close session 2026-05-17 (advisor + user Option B selection)
- Consumer roster: ADR-0076 §11 + Phase 3.1.7.6 phase doc § Consumers
- Determinism contract: ADR-0063 §4
- Shader compile pipeline: ADR-0009 (RHI v1a) + `docs/phases/phase-2.3-shader.md`
