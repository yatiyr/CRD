# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

## Active debt

- **Disabled-trace benchmark** is broken in the Release bench suite. Fix or
  replace before refreshing `docs/bench/baseline_2026-04.md`.
- **Doxygen-style per-symbol comments** are uneven in `crd-core`.
- **No SPSC `RingBuffer`** yet; v1 is single-threaded refuse-on-full.
  Lock-free version arrives with `crd-jobs` (Phase 2.5).
- **No file watcher in `crd-platform`.** Lands with shader hot-reload
  (Phase 2.3).
- **Multi-viewport ImGui** deferred — Vulkan multi-viewport has known rough
  edges. Single-viewport docking only in Phase 2.1.

## Renderer optimization backlog (post-v1g)

Intentionally deferred. These require the render path to be working end-to-end
before they pay off. Implement in order of demonstrated need, not in anticipation.

- **Transient image aliasing in the frame graph** — `FrameGraph::execute` currently
  creates transient images fresh each frame and destroys them on `reset()`. A proper
  aliasing pass would reuse GPU heap pages across mutually-exclusive transients,
  reducing VRAM by the sum of the largest non-overlapping resource sets. Prerequisite:
  lifetime analysis pass in `FrameGraph::build()`.

- **HDR render target** — `ForwardRenderPath` uses `B8G8R8A8Unorm` (LDR). Switch to
  `R16G16B16A16Sfloat` (scene linear HDR) and add a tone-map pass before the swapchain
  blit. Required before bloom, exposure, or any physically-based lighting integral.

- **Depth-only pipeline for the depth prepass** — `ForwardRenderPath` v1g reuses the
  full vertex+fragment pipeline in the depth prepass. A vertex-only pipeline (null
  fragment shader, `Format::Undefined` color, `Format::D32Sfloat` depth only) removes
  unnecessary fragment work during the prepass. Requires the per-variant pipeline cache
  to store `{depth_pipeline, color_pipeline}` pairs.

- **Async pipeline compilation** — `PipelineResolver::resolve_pipeline()` is currently
  synchronous. Slow variant compiles stall the main thread. Solution: compile on a job
  thread, return a "pending" sentinel, and render with a fallback pipeline until the
  real one is ready. Integrates with `crd-jobs` (Phase 2.5).

- **Bindless material system** — Current: one descriptor set per material instance per
  frame (set 1), allocated from the ring pool. Future: global bindless descriptor heap
  (one giant `DescriptorSet` with an array of all textures + material CBs), indexed
  via a per-draw material index in the push constants. Eliminates per-draw
  `vkCmdBindDescriptorSets` calls. Requires Vulkan device features: `descriptorIndexing`.

- **GPU-driven rendering** — CPU culling + indirect draw. Replace per-object draw calls
  with a compute dispatch that reads a scene buffer, outputs `VkDrawIndirectCommand`
  structs, and optionally writes a visible-object list. Requires: stable GPU scene buffer
  (Phase 3 scene system), `VkDrawIndirectCount` (Vulkan 1.2 core), and a GPU frustum
  cull shader. Significant throughput gain for dense scenes (> ~10k draws).

- **Split vertex streams** — Separate position-only VBO from full-attribute VBO. The
  depth prepass only needs positions; pulling the full vertex (UVs, normals, tangents)
  wastes memory bandwidth. Requires `DrawItem` to carry both VBOs and shader variants to
  declare which stream they consume.

## Cleared debt

(empty — items move here with a date when resolved)
