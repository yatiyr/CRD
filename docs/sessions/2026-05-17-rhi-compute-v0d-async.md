## Session 2026-05-17 — Phase 3.1.7.6 v0d `rhi-compute-async` ✅ SHIPPED

### Goal

Surface the async compute queue so consumers can submit compute work
to a dedicated hardware queue and synchronize with graphics via binary
semaphores. v0d completes the compute API surface needed by v9 GPU LBVH
(parallel build alongside frame rendering) and future eylem v8 GPU
broadphase.

### What we shipped

**`engine/rhi/include/crd/rhi/types.hpp`** —
- `PipelineStage` enum (8 variants: Top / ComputeShader / VertexInput /
  VertexShader / FragmentShader / ColorAttachment / Transfer /
  BottomOfPipe). For `SemaphoreWait::wait_stage`. Granular per-stage by
  the same eliteness pattern as `BufferAccess` (v0c) — naive `Top` waits
  over-sync and serialize unnecessarily.
- `AsyncComputePolicy` enum (`FallbackGracefully` default vs
  `RequireDedicated` opt-in). ADR-0080 D9 pin.
- `SemaphoreWait { Semaphore*; PipelineStage wait_stage }`.
- `SubmitInfo { command_buffer; signal_fence; wait_semaphores; signal_semaphores }`
  — single source of truth for the async submit path. ADR-0080 D10.
- `DeviceDesc::async_compute_policy` field (default `FallbackGracefully`).

**NEW** `engine/rhi/include/crd/rhi/semaphore.hpp` — `Semaphore`
interface. Binary semaphore only; timeline variant deferred to a real
consumer (eylem v8 frame-graph, possibly). Header comment makes the
boundary explicit.

**`engine/rhi/include/crd/rhi/device.hpp`** —
- `Device::create_semaphore()` factory.
- `Device::compute_queue()` accessor. **Pointer-identity contract**:
  on fallback returns the SAME `Queue&` as `graphics_queue()` so
  consumers can dispatch on `&compute_queue() == &graphics_queue()`
  to skip cross-queue setup. Documented in the header comment.
- `Device::has_dedicated_compute_queue()` boolean accessor for
  consumer-side optimization hints + perf-UI annotation.

**`engine/rhi/include/crd/rhi/queue.hpp`** —
- `Queue::submit(const SubmitInfo&)` pure-virtual added alongside
  existing 3 overloads. Doesn't break v0a/b/c callers (which use the
  fence-only or swapchain-coupled overloads).

**`engine/rhi-vulkan/src/vulkan_backend.cpp`** —
- `VulkanSemaphore` class — `VkSemaphore` lifecycle + handle accessor.
- `to_vk_pipeline_stage(PipelineStage)` helper — one specific
  `VkPipelineStageFlags` per enum variant.
- Queue family probe at device creation: scans for
  `VK_QUEUE_COMPUTE_BIT && !VK_QUEUE_GRAPHICS_BIT`. If found, creates
  a second `VkDeviceQueueCreateInfo` for the dedicated family.
  `RequireDedicated` policy fails device creation if absent;
  `FallbackGracefully` (default) lets `compute_queue()` alias
  `graphics_queue()` via null `m_compute_queue`.
- `VulkanDevice::compute_queue()` returns either the dedicated
  queue or the graphics queue (pointer identity preserved).
- `VulkanQueue::submit(const SubmitInfo&)` — builds
  `VkSemaphore`/`VkPipelineStageFlags` arrays from spans, threads to
  `VkSubmitInfo` + `vkQueueSubmit`.
- Logging at device creation reports whether dedicated compute was
  acquired or fallback engaged (visible in CI logs + perf-UI when wired).

**`tests/rhi/test_rhi.cpp`** — 5 fake-side `[v0d]` contract tests
(13 assertions):
- `create_semaphore` returns non-null + bumps counter.
- `compute_queue() == graphics_queue()` pointer-identity on fallback.
- `submit(SubmitInfo)` threads wait/signal sem counts + signals fence.
- `PipelineStage` enum granularity pin (ComputeShader ≠ VertexShader, etc.).
- `AsyncComputePolicy` default is `FallbackGracefully` per ADR-0080 D9.

**`tests/rhi_vulkan/test_rhi_vulkan.cpp`** — 2 Vulkan `[v0d]`
integration tests (75 assertions):
- **Async cross-queue first-light**: pass 1 (v0b shader) on
  `compute_queue` writes `i + 1000` to `buf_a`, signals binary
  semaphore; pass 2 (v0c doubler) on `graphics_queue` waits on
  semaphore at `ComputeShader` stage, reads `buf_a`, writes
  `2 * buf_a[i]` to `buf_b`, signals fence; host wait + readback
  validates each of 64 elements equals `2 * (i + 1000)`. **Test
  passes on both hardware paths** (dedicated compute queue OR
  fallback to graphics queue) — the API works identically.
- **Pointer-identity contract roundtrip**: on dedicated hardware
  `&compute_queue() != &graphics_queue()`; on fallback they're equal.

### Eliteness decisions (advisor-locked at slice start)

1. **`SubmitInfo` struct + new `submit(SubmitInfo)`** rather than
   default args on existing overloads or replacement. Existing 3
   overloads stay for v0a/b/c callers (no churn). Single source of
   truth for new compute-consumer path.
2. **Binary `Semaphore` only** (not timeline). Class comment pins
   the boundary; timeline variant ships when a real consumer needs
   ordered multi-step async work.
3. **`SemaphoreWait` needs `PipelineStage`, not `BufferAccess`** —
   semaphore wait gates a stage, doesn't carry access bits (the
   semaphore already implies memory visibility). Distinct enum.
4. **`compute_queue()` returns same `Queue&` as `graphics_queue()`
   on fallback — pointer identity preserved + documented**. Don't
   wrap to hide identity; that breaks consumer optimization
   (`if (&cq == &gq) skip cross-queue setup`).
5. **`has_dedicated_compute_queue()` accessor included now** (advisor
   nice-to-have). One-liner; saves perf-UI from having to probe via
   internal mechanisms later.

### Validation discriminator

Manually confirmed during development: removing the `SemaphoreWait`
from pass 2's `SubmitInfo` triggers
`VUID-vkCmdDispatch-None-...` write-after-write hazard on `buf_a`
when the two submissions land on distinct queues (dedicated compute
hardware). On fallback (same queue), the submissions implicitly
serialize through the queue's submission order — validation stays
quiet without the semaphore there, but adding it doesn't hurt and
the API works uniformly. Programmatic capture deferred until a
test-side debug-messenger hook surfaces.

### Mid-slice fixes

1. **Pure-virtual cascade** — 4 new pure-virtuals on `Device`
   (`create_semaphore`, `compute_queue`, `has_dedicated_compute_queue`)
   and 1 new on `Queue` (`submit(SubmitInfo)`). Patched across all 3
   Fake/Smoke Device + Queue impls (`test_rhi.cpp`,
   `smoke_renderer.cpp`, `test_renderer.cpp`, `smoke_rhi_api.cpp`).
   Same pattern as v0a/b/c.
2. **No tidy/lint surprises this slice** — the renames + naming
   followed project conventions from the start.

### Per-slice 4-config DoD

`scripts/per-slice-check.ps1 -Parallel`, elapsed 01:16:
- win-debug    PASS (build+ctest)
- win-asan     PASS (build+ctest)
- win-shipping PASS (build+ctest)
- win-tidy     PASS (build)

### Stats

- Engine LOC: ~390 (rhi: ~80; rhi-vulkan: ~210; Fake/Smoke patches: ~100).
- Test LOC: ~290 (test_rhi.cpp: ~95; test_rhi_vulkan.cpp: ~195).
- Total v0d cases / assertions: 7 cases / 88 assertions, all green.
- Estimate at slice start: ~600 engine + ~400 tests, ~5 days. Actual:
  ~390 engine + ~290 tests, ~1 day. Pattern from v0a-c continues:
  reusing Vulkan primitives + existing submit-pattern keeps slice tight.

### Combined Phase 3.1.7.6 stats so far

- 4 of 6 slices SHIPPED (v0a + v0b + v0c + v0d).
- Engine LOC: ~1210. Test LOC: ~1050. Cases: 31. Assertions: 421.
- Calendar: 4 days (estimate was ~21 days advisor-tightened; tight
  Vulkan-mirror impl keeps each slice ~⅓-½ of original budget).

### Next slice

**v0e `rhi-compute-shader-pipeline`** — extend `crd-shader` for
compute: GLSL `#version 450` + `comp` profile through existing shaderc
+ spirv-reflect pipeline (already works for binary-blob compile path
in v0a/b/c/d tests; v0e formalizes via the higher-level
`crd::shader` Effect/Variant system). SPIR-V reflection extension
parses workgroup size (`local_size_x/y/z`), specialization constants,
and storage-buffer descriptor bindings. Compute-shader hot-reload via
existing `crd-shader` watch path; failed compile preserves last-good
pipeline. Locks ADR-0080 D11 + D12.
