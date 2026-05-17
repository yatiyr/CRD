## Session 2026-05-17 — Phase 3.1.7.6 v0b `rhi-compute-dispatch` ✅ SHIPPED

### Goal

Add the compute dispatch surface on top of v0a's pipeline types so a
caller can actually run a compute shader end-to-end: bind pipeline,
bind descriptor sets, dispatch (direct + indirect), with specialization
constants baked at create-time. First-light proof that the full
substrate path through `crd-rhi-vulkan` reaches the GPU and returns
correct results to host memory.

### What we shipped

**`engine/rhi/include/crd/rhi/command_buffer.hpp`** — 4 new pure-virtual methods on `CommandBuffer`:
- `bind_compute_pipeline(ComputePipeline&)` — sibling of `bind_pipeline` (binds at `VK_PIPELINE_BIND_POINT_COMPUTE` not `_GRAPHICS`).
- `bind_compute_descriptor_sets(PipelineLayout&, first_set, ConstSpan<DescriptorSet*>)` — sibling of `bind_descriptor_sets`. Same parameter shape; separate method because Vulkan binds to the compute bind point.
- `dispatch(group_count_x, group_count_y, group_count_z)` — **ADR-0080 D4 pinned**: parameters are WORKGROUP counts, not thread counts. Total threads = group counts × `local_size_*` from the shader.
- `dispatch_indirect(Buffer&, offset_bytes)` — reads `VkDispatchIndirectCommand` (3 × u32) from a `BufferUsage::Indirect` buffer.

**`engine/rhi/include/crd/rhi/types.hpp`** —
- `SpecializationConstantEntry { constant_id; offset; size }` — 1:1 mirror of `VkSpecializationMapEntry`. 12 bytes, trivially copyable, standard layout (pinned in test).
- `ComputePipelineDesc` extended with `specialization_entries` + `specialization_data` spans (ADR-0080 D6: baked at create-time; per-dispatch parameters use push constants).
- `BufferUsage::Indirect` flag added for indirect dispatch (+ future indirect draws).

**`engine/rhi-vulkan/src/vulkan_backend.cpp`** —
- `VulkanCommandBuffer` impl of all 4 new methods (`vkCmdBindPipeline(COMPUTE)`, `vkCmdBindDescriptorSets(COMPUTE)`, `vkCmdDispatch`, `vkCmdDispatchIndirect`).
- `create_compute_pipeline` extended: builds `VkSpecializationInfo` from desc spans when present; threads through `VkPipelineShaderStageCreateInfo::pSpecializationInfo`.
- `to_vk_buffer_usage` extended for `Storage` + `Indirect` flags.

**`runtime/examples/shaders/compute_v0b_dispatch.comp`** — first-light shader. Spec const `kBaseOffset` (constant_id = 0) + storage buffer at set 0 / binding 0. Each invocation writes `gl_GlobalInvocationID.x + kBaseOffset` to its element. `local_size = 64×1×1`.

**`tests/rhi/test_rhi.cpp`** — 6 fake-side `[v0b]` contract tests (27 assertions):
- bind_compute paths route through separate counters (don't touch graphics).
- dispatch params recorded as workgroup counts.
- dispatch_indirect threads buffer + offset.
- SpecializationConstantEntry layout pin (12 bytes / trivially copyable / standard layout).
- ComputePipelineDesc accepts spec const data through factory.
- push_constants reuse for compute via `ShaderStage::Compute` mask (D5 confirmed: no compute-specific push method).

**`tests/rhi_vulkan/test_rhi_vulkan.cpp`** — 2 `[v0b]` integration tests (147 assertions):
- **End-to-end first-light**: compile compute shader → create pipeline with `kBaseOffset = 1000` spec const → create 4-KB host-visible coherent storage buffer → allocate descriptor + bind → `dispatch(1, 1, 1)` → submit + fence-wait → readback validates each of 64 elements == `i + 1000`.
- **dispatch_indirect**: same flow but workgroup counts come from a host-uploaded `Indirect`-flagged buffer. Same per-element validation.

### Eliteness decisions (advisor-locked at slice start)

1. **Reused `push_constants` for compute** (D5). Existing signature `push_constants(PipelineLayout&, ShaderStage, offset, size, data)` already takes layout + stages mask; `ShaderStage::Compute` in the mask routes correctly. No compute-specific push method. **One API, two consumers.**
2. **Sibling `bind_compute_pipeline` + `bind_compute_descriptor_sets`** (not a `PipelineBase` hoist). Graphics and compute bind to *different* Vulkan pipeline bind points — that's two distinct operations, not polymorphism. Honest separation; cleaner Vulkan-side dispatch.
3. **No `bind_compute_storage_buffer(set, binding, buffer)` shortcut.** The phase-doc-suggested ergonomic was rejected as v9-LBVH convenience that belongs one layer above the RHI. Existing `DescriptorAllocator` + `update_buffer` + `bind_compute_descriptor_sets` composes cleanly — and the first-light test proves it.
4. **Specialization constants baked at create-time** (D6). Spec const data lives in `ComputePipelineDesc` spans; copied into `VkSpecializationInfo` during `vkCreateComputePipelines`; not retained after creation. Per-dispatch parameters use push constants — orthogonal mechanism.
5. **Smoke sync via `queue.submit(cmd, fence) + fence.wait() + MemoryUsage::GpuToCpu`** — host-coherent memory means no explicit barriers needed; v0c's `compute_barrier` API would replace this in production paths with mixed coherency or compute→graphics handoff. Honest about scope: v0b doesn't pre-pay v0c.

### v0a bug uncovered + fixed

`BufferUsage::Storage` was declared in the enum at the descriptor system's introduction but **never wired through `to_vk_buffer_usage`** to `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`. v0a's "create_buffer with Storage works" test passed because Vulkan happily creates a buffer with any usage flag combination (or none) — but using such a buffer as an SSBO at descriptor bind time would have failed validation. v0b's first-light dispatch was the first real consumer that needed the flag actually plumbed. Fixed in the same edit that added `BufferUsage::Indirect`. Comment in `vulkan_backend.cpp` notes the v0a→v0b discovery for future readers.

### Pure-virtual cascade follow-up

The 4 new pure-virtual methods on `CommandBuffer` triggered C2259 ("abstract class") on three pre-existing Fake/Smoke command-buffer impls: `smoke_renderer.cpp`, `tests/renderer/test_renderer.cpp`, `smoke_rhi_api.cpp`. All patched to no-op overrides (compute not exercised in those contexts). Same pattern as the v0a `create_compute_pipeline` cascade. Per-slice DoD caught all three.

### v0a sizeof pin relaxation

The v0a `STATIC_REQUIRE(sizeof(ComputePipelineDesc) == sizeof(void*) * 2)` no longer holds after v0b's two `ConstSpan` field additions. Replaced with a behavioral assertion — the default-constructed desc has empty spec-const spans — which preserves the "narrow surface" intent without baking in a size that evolves with the type.

### Per-slice 4-config DoD

`scripts/per-slice-check.ps1 -Parallel`, elapsed 01:15:
- win-debug    PASS (build+ctest)
- win-asan     PASS (build+ctest)
- win-shipping PASS (build+ctest)
- win-tidy     PASS (build)

No tidy debt cascade this time. Project gate is clean.

### Stats

- Engine LOC: ~280 (rhi: 40; rhi-vulkan: 90; Fake/Smoke patches: 30; tests: 120).
- Test LOC: ~270 (test_rhi.cpp: ~90; test_rhi_vulkan.cpp: ~180).
- Total v0b cases / assertions: 8 cases / 174 assertions, all green.
- Estimate at slice start: ~600 LOC engine + ~400 LOC tests, ~5 days. Actual: ~280 engine + ~270 tests, ~1 day. The reuse-graphics-push-path decision (D5) + Vulkan impls being almost-trivial wrappers around `vkCmdDispatch`/etc. shrank the LOC count significantly.

### Combined v0a+v0b stats so far (Phase 3.1.7.6)

- 2 of 6 slices SHIPPED (v0a + v0b).
- Engine LOC: ~640. Test LOC: ~550. Cases: 19. Assertions: 243.
- Calendar: 2 days (advisor-tightened estimates suggested ~5 + 5 days).

### Next slice

**v0c `rhi-compute-sync`** — `ICommandBuffer::compute_barrier(BufferAccess::ComputeWrite → BufferAccess::GraphicsRead)` etc. via typed-enum API (not raw `VkAccessFlags`). Memory dependencies for compute↔graphics handoff via pipeline barriers. Same-queue path only (no cross-queue semaphores — those are v0d async compute). First-light smoke: compute pass writes to storage buffer → barrier → vertex shader reads same buffer as vertex pull buffer, draws something to swapchain, present. ADR-0080 D7 + D8 lock at this slice.
