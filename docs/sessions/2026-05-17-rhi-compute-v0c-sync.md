## Session 2026-05-17 — Phase 3.1.7.6 v0c `rhi-compute-sync` ✅ SHIPPED

### Goal

Add the pipeline barrier surface so compute work can hand off to other
compute, graphics, transfer, or host stages without data races. v0a +
v0b shipped the pipeline + dispatch path; v0c is the synchronization
primitive that makes multi-pass pipelines correct.

### What we shipped

**`engine/rhi/include/crd/rhi/types.hpp`** —
- New `BufferAccess` enum (14 variants) — typed-enum API for buffer
  pipeline barriers. **Granular per Vulkan pipeline stage**, not
  collapsed into a "GraphicsRead" bucket. Advisor's key call: lets the
  impl pick exact `srcStageMask`/`dstStageMask` without over-barrier.
  Variants: `None`, `ComputeShaderRead`, `ComputeShaderWrite`,
  `ComputeShaderReadWrite`, `VertexShaderRead`, `FragmentShaderRead`,
  `VertexAttributeRead`, `IndexRead`, `UniformRead`, `IndirectRead`,
  `TransferSrc`, `TransferDst`, `HostRead`, `HostWrite`.
- `ImageAccess` extended with `ComputeShaderRead` / `ComputeShaderWrite`
  / `ComputeShaderReadWrite` — addresses the v0c discovery that the
  existing `ImageAccess::ShaderRead` was wired only to
  `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. Existing `ShaderRead` stays
  graphics-fragment-only for back-compat (no consumer behavior change).

**`engine/rhi/include/crd/rhi/command_buffer.hpp`** —
- `CommandBuffer::buffer_barrier(Buffer&, BufferAccess from, BufferAccess to)`
  pure virtual. **Single-buffer signature** mirroring
  `transition_image(Image&, ImageAccess, ImageAccess)`. Naming pin:
  `buffer_barrier` not `compute_barrier` — the API is not compute-specific
  (covers transfer↔shader, host↔indirect, fragment↔compute, anything);
  resource-typed naming is API-flavored. Span-batched variant deferred
  to follow-on when a real consumer (eylem v8 GPU broadphase, future) needs it.

**`engine/rhi-vulkan/src/vulkan_backend.cpp`** —
- New `to_vk_buffer_access_info(BufferAccess) → {VkPipelineStageFlags, VkAccessFlags}`
  helper. Each variant maps to ONE specific stage + access bit pair
  (no over-barrier). `UniformRead` covers all three shader stages
  (vertex / fragment / compute) since uniform-buffer reads can come
  from any of them; consumers needing tighter scope use the specific
  `*ShaderRead` variants.
- `to_vk_access_info(ImageAccess)` extended for new compute variants.
  `ComputeShaderRead` keeps `SHADER_READ_ONLY_OPTIMAL` layout; the
  write/RW variants need `VK_IMAGE_LAYOUT_GENERAL` (storage images).
- `VulkanCommandBuffer::buffer_barrier` impl — one
  `VkBufferMemoryBarrier` + `vkCmdPipelineBarrier`. Early-out when
  `from == to` (cheap no-op; validation layer would flag a redundant
  same-state barrier anyway).

**`runtime/examples/shaders/compute_v0c_doubler.comp`** — second-pass
shader for the two-compute-pass discriminator. Reads `u_input.data[i]`,
writes `2 * u_input.data[i]` to `u_output.data[i]`. Paired with
`compute_v0b_dispatch.comp` for the end-to-end test.

**`tests/rhi/test_rhi.cpp`** — 3 fake-side `[v0c]` contract tests
(11 assertions):
- `buffer_barrier` records `from`/`to` BufferAccess values.
- `BufferAccess` granularity pinned: `ComputeShaderRead` ≠
  `VertexShaderRead` ≠ `FragmentShaderRead`; `VertexAttributeRead` ≠
  `UniformRead`; `IndirectRead` ≠ `TransferSrc`. If a future refactor
  collapses variants, this test trips.
- `ImageAccess::ShaderRead` ≠ `ImageAccess::ComputeShaderRead` —
  back-compat assertion (existing graphics consumers stay on the
  fragment-only path).

**`tests/rhi_vulkan/test_rhi_vulkan.cpp`** — 2 Vulkan `[v0c]` integration
tests (79 assertions):
- **Two-compute-pass barrier first-light** — pass 1 (v0b shader)
  writes `i + 1000` to `buf_a`; barrier
  `ComputeShaderWrite → ComputeShaderRead`; pass 2 (v0c doubler) reads
  `buf_a`, writes `2 * buf_a[i]` to `buf_b`; host readback validates
  each of 64 elements equals `2 * (i + 1000)`.
- **Same-state barrier is a no-op** (`ComputeShaderWrite → ComputeShaderWrite`).
  Cheap early-out; validation stays quiet.

### Decisions locked (ADR-0080 D8)

1. **`buffer_barrier` not `compute_barrier`** (advisor lock). The API
   is resource-typed, not consumer-typed. Mirrors `transition_image`.
2. **Granular per-stage `BufferAccess` enum** — not collapsed into
   "GraphicsRead". The smoke test discriminator (`ComputeShaderWrite
   → VertexShaderRead` for vertex-pull-from-SSBO) would lie if we
   collapsed.
3. **Single-buffer signature**; span-batched variant filed as follow-on
   when a real consumer needs it (current v9 LBVH single-buffer path
   does not).
4. **Same-queue path only.** Cross-queue ownership transfer with queue
   family indices + semaphore ordering = v0d async compute.
5. **No global memory barrier**, only per-resource. Whole-pipeline
   barriers are a deliberate addition with their own justification if
   ever needed.

### Validation-layer discriminator

Confirmed manually during development: commenting out the
`buffer_barrier` call between the two compute passes triggers
`VUID-vkCmdDispatch-None-...` (write-after-write hazard on `buf_a`).
With the barrier in place, validation stays quiet under
`VK_LAYER_KHRONOS_validation` (default debug callback at
`engine/rhi-vulkan/src/vulkan_backend.cpp:253`). Comment in the
Vulkan test documents this; programmatic capture deferred until a
test-side debug-messenger hook surfaces (no current consumer).

### Mid-slice fixes

1. **Pure-virtual cascade** across 4 Fake/Smoke command-buffer impls
   (`test_rhi.cpp`, `smoke_renderer.cpp`, `test_renderer.cpp`,
   `smoke_rhi_api.cpp`) — same pattern as v0a / v0b. All patched to
   no-op overrides.
2. **Non-ASCII test name** — the test "buffer_barrier threads from→to
   BufferAccess pair" contained `→` which tripped
   `crd-no-non-ascii-test-names` guard. Rewrote with `from->to`. Same
   class of fix as v8e during the v8 cluster.
3. **camelCase variable names** — initial Vulkan test used `bufA` /
   `bufB`; tidy flagged them as violations of the project `lower_case`
   variable convention. Renamed to `buf_a` / `buf_b`.

### Per-slice 4-config DoD

`scripts/per-slice-check.ps1 -Parallel`, elapsed 00:29 (after the 3
fixes above):
- win-debug    PASS (build+ctest)
- win-asan     PASS (build+ctest)
- win-shipping PASS (build+ctest)
- win-tidy     PASS (build)

First attempt hit the 3 lint-class fails above (1 tidy, 2 ctest from
the non-ASCII guard); fixes were ~1 minute total.

### Stats

- Engine LOC: ~180 (rhi: 60; rhi-vulkan: 100; Fake/Smoke patches: 20).
- Test LOC: ~210 (test_rhi.cpp: ~55; test_rhi_vulkan.cpp: ~155).
- Total v0c cases / assertions: 5 cases / 90 assertions, all green.
- Estimate at slice start: ~400 engine + ~300 tests, ~3 days. Actual:
  ~180 engine + ~210 tests, ~1 day. Pattern continues from v0a + v0b:
  reusing existing primitives (`vkCmdPipelineBarrier`, the
  to_vk_access_info shape) keeps slices tight.

### Combined Phase 3.1.7.6 stats so far

- 3 of 6 slices SHIPPED (v0a + v0b + v0c).
- Engine LOC: ~820. Test LOC: ~760. Cases: 24. Assertions: 333.
- Calendar: 3 days (estimate was ~16 days advisor-tightened; tight
  Vulkan-mirror impl keeps each slice ~⅓ of original budget).

### Next slice

**v0d `rhi-compute-async`** — async compute queue. Vulkan-side: probe
`VkQueueFamilyProperties` for a queue family with `VK_QUEUE_COMPUTE_BIT`
**without** `VK_QUEUE_GRAPHICS_BIT` (dedicated compute queue); fall back
to graphics queue if hardware does not expose one (`FallbackGracefully`
default per ADR-0080 D5). Cross-queue semaphores for compute-write →
graphics-read across queue families. Frame-loop integration. Locks
ADR-0080 D9 (Async compute policy: `FallbackGracefully` default; opt-in
`RequireDedicated`) + D10 (semaphore + queue ownership transfer API).
