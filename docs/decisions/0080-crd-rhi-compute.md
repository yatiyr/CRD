# ADR-0080 — `crd-rhi-compute` substrate

**Status:** SUPERSEDED by ADR-0103/0104/0105 (final strike 2026-07-23, RET band). The compute substrate this ADR
built on crd-rhi was re-founded on crd-gpu-context + CKIR (ADR-0103: gpu-context owns every GPU program); the
async-compute queue semantics (D9 pointer-identity, dedicated-family detection) SURVIVE in gpu-context (already
implemented — `compute_queue()` on the context) and RET-4 ports the remaining test coverage. crd-rhi-compute's
API dies with crd-rhi at RET-8. Original text preserved below. **Accepted 2026-05-17** (Phase 3.1.7.6 v0-close). Originally Proposed 2026-05-17 at v0a opening; locked at v0-close per the established ADR-at-slice-time pattern (ADR-0078 / ADR-0079 precedents). The base text below preserves the original Proposed-state framing; revisions discovered during v0a-v0e implementation are folded into the §"Amendments at v0-close" section at the bottom.

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

## Amendments at v0-close (2026-05-17)

The original D1-D8 above were drafted at v0a opening with D9-D12 as
"locks at slice start" pins. Implementation experience across v0a-v0e
revised several decisions; the revised text below is the authoritative
contract going forward.

### D2 REVISED — storage buffers reuse existing `Buffer` interface

The original D2 proposed a separate `IStorageBuffer` interface. On v0a
orientation, we discovered the existing `Buffer` (just `desc()` + `map()` +
`unmap()`) has no per-usage divergence — the only differences between
SSBO and uniform/vertex/index are:
- the `BufferUsage` flag (already supported via `BufferUsage::Storage`);
- the `DescriptorType` at bind time (already supported via
  `DescriptorType::StorageBuffer`);
- the barriers, which live at `CommandBuffer::buffer_barrier` (v0c).

Splitting into `IStorageBuffer` would force consumers holding both kinds
to carry two pointer types for zero behavioral gain. **Storage buffers
reuse `Buffer` + `BufferUsage::Storage` flag.** Tightening pinned in
`compute_pipeline.hpp` header comment + `test_rhi_vulkan.cpp::"Vulkan
create_buffer with BufferUsage::Storage works (D2 revision)"`.

**Phantom-fix discovered at v0b**: `BufferUsage::Storage` was declared in
the enum but `to_vk_buffer_usage` never mapped it to
`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`. v0a's "Storage buffer works" test
passed because Vulkan accepts buffers with any usage combination — but
using such a buffer as an SSBO at descriptor bind would have failed
validation. Fixed in v0b alongside `BufferUsage::Indirect`.

### D7 SOFTENED — descriptor-set conventions are a guideline, not a type-level rule

The original D7 proposed "set 0 = storage, set 1 = uniform, push-const
reserved" as a type-level layout. `PipelineLayoutDesc` already takes any
caller-constructed `set_layouts` span (matches graphics flexibility); a
type-level enforcement would break that. **Convention is documented in
`docs/systems/rhi-compute.md` § Recommended descriptor-set conventions;
the RHI does not enforce it.** Compute consumers compose layouts the
same way graphics consumers do.

### D4 locked — `dispatch(x, y, z)` parameters are workgroup counts

Vulkan + DirectX convention; total threads = workgroup counts ×
`local_size_*` from shader. Pinned in test
`tests/rhi/test_rhi.cpp::"dispatch params are workgroup counts (D4)"`.

### D5 locked — push constants reuse existing `push_constants(layout, stages, ...)` method via `ShaderStage::Compute` mask

No compute-specific push method. The existing graphics push-constant
path already takes a `PipelineLayout&` + `ShaderStage` mask;
`ShaderStage::Compute` in the mask routes correctly. **One API, two
consumers.** Same 128-byte budget (Vulkan minimum guarantee). Pinned in
test `tests/rhi/test_rhi.cpp::"push_constants reuse for compute via
ShaderStage::Compute mask (D5)"`.

### D6 locked — specialization constants baked at create-time via `VkSpecializationInfo`

`ComputePipelineDesc::specialization_entries` + `specialization_data`
spans threaded through `vkCreateComputePipelines`. Not retained after
creation. Per-dispatch parameters use push constants (orthogonal
mechanism). Pinned in v0b first-light test (compute shader has
`layout(constant_id = 0) const uint kBaseOffset = 0` baked to value 1000).

### D8 RENAMED + GRANULAR — `buffer_barrier(Buffer&, BufferAccess, BufferAccess)`

Original D8 said `compute_barrier(BufferAccess::ComputeWrite →
GraphicsRead)`. Advisor lock at v0c: **`buffer_barrier` not
`compute_barrier`** — the API is resource-typed, not consumer-typed
(covers transfer↔shader, host↔indirect, fragment↔compute, anything).
Mirrors existing `transition_image` shape.

**`BufferAccess` enum is granular per Vulkan pipeline stage** (14
variants: `ComputeShaderRead/Write/ReadWrite`, `VertexShaderRead`,
`FragmentShaderRead`, `VertexAttributeRead`, `IndexRead`, `UniformRead`,
`IndirectRead`, `TransferSrc`, `TransferDst`, `HostRead`, `HostWrite`,
`None`) — no "GraphicsRead" collapse. Lets impl pick exact
`srcStageMask`/`dstStageMask` without over-barrier. Single-buffer
signature; span-batched variant filed as follow-on when a real consumer
needs it.

**`ImageAccess` gained compute variants** during v0c — discovered that
`ImageAccess::ShaderRead` was wired only to
`VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. Added `ComputeShaderRead` /
`ComputeShaderWrite` / `ComputeShaderReadWrite`; existing `ShaderRead`
stays graphics-fragment-only for back-compat.

### D9 locked — `AsyncComputePolicy::FallbackGracefully` default

Probe at device creation; if no dedicated compute queue family,
`compute_queue()` returns the same `Queue&` as `graphics_queue()`.
Opt-in `RequireDedicated` fails device creation when hardware does not
expose dedicated compute. **`compute_queue()` pointer-identity
contract**: on fallback, `&compute_queue() == &graphics_queue()` —
consumers may dispatch on the comparison to skip cross-queue setup.
Documented in `device.hpp` + tested in
`test_rhi_vulkan.cpp::"Vulkan compute_queue pointer-identity contract"`.
`Device::has_dedicated_compute_queue()` accessor for consumer-side
optimization + perf-UI annotation.

### D10 locked — `SubmitInfo` + binary `Semaphore`

`Queue::submit(const SubmitInfo&)` is the single source of truth for
the cross-queue async path. Existing 3 `Queue::submit` overloads
(swapchain-coupled, fence-only, submit_and_wait) stay for v0a/b/c
back-compat — no churn at existing call sites.

**`Semaphore` is binary only.** Timeline semaphores (Vulkan 1.2 typed
timeline with monotonic 64-bit value) ship when a real consumer needs
ordered multi-step async work (eylem v8 frame-graph, possibly).

**`SemaphoreWait` carries `PipelineStage`, not `BufferAccess`** —
semaphore wait gates a stage; the semaphore itself implies memory
visibility, so no access-mask redundancy. `PipelineStage` is its own
8-variant enum (`Top` / `ComputeShader` / `VertexInput` / `VertexShader`
/ `FragmentShader` / `ColorAttachment` / `Transfer` / `BottomOfPipe`).

### D11 + D12 locked at v0e — shaderc + spirv-reflect handles compute via existing pipeline

The binary-blob compile path already works for compute (`Stage::Compute`
maps to `shaderc_compute_shader`; spirv-reflect parses descriptor +
push-const bindings stage-agnostically). v0e formalized the missing
metadata extraction: workgroup size via `entry_points[0].local_size`,
and specialization constants via `module.spec_constants[i]` (corrected
member name — initial code used `specialization_*` which doesn't exist
in spirv-reflect 1.4.341.1).

**Spec const `size_bytes` pinned at 4** (32-bit scalars: bool/int/float).
64-bit spec consts are extremely rare; revisit when a consumer needs 8.

**Compute Effects = single-variant rule**: `VariantRequest`'s graphics
fields (`pass_type`, `alpha_mode`, `render_path`, `skinned`) are
ignored for compute Effects. Reflection on `Module` is the meaningful
surface; no high-level `Effect → ComputePipelineDesc` accessor (minimal
v0e scope per advisor — v9-LBVH builds the desc from `Module` spv bytes
+ reflected layout directly at the call site).

**`std::optional<WorkgroupSize>` over `{0,0,0}` sentinel** for the
`Module::workgroup_size()` accessor — graphics modules genuinely have
no workgroup size; `optional` is the honest signal.

### Naming pins (consolidated)

- **`Pipeline` (graphics) + `ComputePipeline` (standalone)** — no
  shared `PipelineBase` hoist. Vulkan binds graphics + compute to
  different pipeline bind points (`VK_PIPELINE_BIND_POINT_GRAPHICS` vs
  `_COMPUTE`); honest separation, not polymorphism. Filed cleanup
  follow-on: rename `Pipeline` → `GraphicsPipeline` only if a real
  readability problem surfaces (none yet).
- **`bind_compute_pipeline` + `bind_compute_descriptor_sets` sibling
  methods on `CommandBuffer`** (not generalized
  `bind_pipeline_at_bind_point(pt)`) — same bind-point divergence
  rationale.
- **`buffer_barrier` not `compute_barrier`** — see D8.

### Rejected during cluster

- `bind_compute_storage_buffer(set, binding, buffer)` shortcut from the
  original phase-doc draft for v0b — rejected as v9-LBVH ergonomic that
  belongs above the RHI. Existing `DescriptorAllocator` +
  `update_buffer` + `bind_compute_descriptor_sets` composes cleanly
  (first-light test proves it).
- Span-batched `buffer_barrier`/`SubmitInfo` variants — filed as
  follow-on when a real consumer needs batched submission.

## References

- Phase doc: `docs/phases/phase-3.1.7.6-rhi-compute.md`
- Cluster session log: `docs/sessions/2026-05-17-rhi-compute-v0-close.md`
- Per-slice session logs: `docs/sessions/2026-05-17-rhi-compute-v0{a,b,c,d,e}-*.md`
- Eliteness audit transcript: Phase 3.1.7 v8-close session 2026-05-17 (advisor + user Option B selection)
- Consumer roster: ADR-0076 §11 + Phase 3.1.7.6 phase doc § Consumers
- Determinism contract: ADR-0063 §4
- Shader compile pipeline: ADR-0009 (RHI v1a) + `docs/phases/phase-2.3-shader.md`
- System doc: `docs/systems/rhi-compute.md`
