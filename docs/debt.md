# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

## Active debt

### Async GPU upload (`GpuUploader`)

**Why it matters:** the sandbox now kicks `load_async<MeshResource>` on click and finalises the upload on the first frame after the load fiber signals Ready (Phase 2.8 follow-up, 2026-05-06). That removes the disk-I/O + parse hitch from the main thread. But `GpuUploader::upload_mesh` / `upload_texture` still end with a synchronous `device.graphics_queue().submit_and_wait(*cmd)` — a `vkQueueWaitIdle` on the main thread. For BoomBox-class assets (~10 MB GLB → ~30 MB raw mesh) that's a visible hitch even though the CPU-side load is now off the main thread.

**What's needed:** an async upload contract.
- `GpuUploader::upload_mesh_async(const MeshResource&, Device&) → UploadHandle` (and the texture twin).
- The job pool runs the staging fill + `vkCmdCopyBuffer` recording on a worker fiber.
- Submission goes onto the graphics queue without `wait`. A fence (or timeline semaphore) tracks completion.
- Caller polls `UploadHandle::is_ready()` per frame and only swaps the GPU resource pointer (e.g. `m_gpu_mesh`) once the fence signals.
- Concurrent uploads share a transfer queue if the device exposes one; otherwise they batch onto graphics behind the existing graphics submissions.

**Why it's deferred:** designing the upload-handle contract well requires at least two real consumers shaping it. Today there's exactly one (sandbox click). The right moment is **Phase 3.0+** — once the scene/ECS layer can spawn a streaming load (terrain tile, LOD swap, scene-load preload), the contract has two callers and the design surface is informed by both. Until then, premature design risks baking in a single-callsite assumption.

**Where it's referenced:**
- `docs/phases/phase-3.0-scene-ecs.md` — listed as a prerequisite for streamed scene loads.
- `engine/renderer/src/gpu_uploader.cpp` — current implementation.
- `sandbox/src/sandbox_layer.cpp::try_finalize_pending_load()` — comment points here when the synchronous upload runs.

---

## Material system v1 known gaps

`MaterialResource` as shipped in Phase 2.6 v1e is a loader proof-of-concept, not a production material
abstraction. Phase 2.7 v1c (ADR-0048) redesigns it as a full material system foundation: `MaterialTemplate`
+ `MaterialInstance` two-tier split, new MATR artifact format (INFO/PRMS/DFLT/PASS/PSOS/OPTS chunks),
`ParameterType` enum, `ShaderOption` system with inline functor, `SurfaceData` GLSL contract, `PassType` enum,
`MaterialDomain` (pulled forward from Phase 2.8), `RasterState` encoding in the artifact.

**Updated status (post-ADR-0048):**
- **Items 1–3 (artifact layer)** — Closed by Phase 2.7 v1c. The artifact format now carries: parameter
  schema (PRMS), defaults (DFLT), pass-keyed shaders (PASS), PSO state per pass (PSOS), shader options (OPTS).
- **Items 1–3 (GPU wiring)** — Phase 2.8 wires the artifact data to Vulkan pipeline compilation
  (per-material pipeline cache, multi-pass ForwardRenderPath, depth-only prepass).
- **Items 4–5** — Still deferred (item 4 → Phase 3.5 CSM; item 5 → Phase 3.7 post-FX or Phase 3.8 GPU-driven).
  No consumer exists yet.

### 1. Material parameters, texture slots, and full parameter system ✅ Closes Phase 2.7 v1c

**What was missing:** `MaterialResource` held two shader handles and nothing else. No parameter schema,
no texture slots, no shader variants, no material domain, no render-pass awareness.

**What v1c delivers:**
- `MaterialTemplate` (replaces `MaterialResource`): loaded from MATR artifact. Carries parameter schema
  (`Array<CookedParameter>` sorted by name_hash), default values blob, pass-keyed shader handles
  (`HashMap<PassType, ResourceHandle<ShaderResource>>`), PSO state per pass, shader option declarations.
- `MaterialInstance` (caller-owned, not in ResourceManager): mutable overrides atop a `MaterialTemplate`.
  `set_float` / `set_vec4` / `set_texture` write into a `values_blob`. `variant_for_pass(pass)` evaluates
  inline functor rules and returns the correct `ShaderResource` permutation.
- `ParameterType` enum: Float/Float2/Float3/Float4/Color/Bool/Int/Enum/Texture2D/TextureCube/Sampler.
- Cook-time SPIR-V reflection: spirv-reflect extracts UBO offsets; cooker emits `CookedParameter` entries
  sorted by name_hash for O(log N) binary search at bind time.
- Inline functor: `enables_option = "USE_NORMAL_MAP"` on a texture parameter — no C++ subclass needed.

### 2. PSO state in the material artifact — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1a

**Artifact layer (v1c):** `PSOS` chunk carries a `RasterState` per PassType (present_mask + RasterState
array). `RasterState`: AlphaMode, CullMode, FillMode, depth_test, depth_write, src/dst BlendMode.

**GPU wiring (Phase 2.8 v1a):** `ForwardRenderPath` reads `material->pso_states[pass_type]` and
incorporates it into the `GraphicsPipelineDesc` key. Per-material pipeline cache keyed by
`(VariantKey, RasterState)`. `ForwardRenderPath` skips non-`Surface` domain materials.

### 3. Shader variant awareness (VariantKey + pass-keyed variants) — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1b

**Artifact layer (v1c):** `PASS` chunk stores `HashMap<PassType, ResourceId>`. `OPTS` chunk stores shader
option declarations. `MaterialInstance::variant_for_pass(pass)` evaluates inline functor rules, constructs
a `VariantKey`, and returns the appropriate `ShaderResource` from `tmpl->pass_shaders[pass]`.

**GPU wiring (Phase 2.8 v1b):** `ForwardRenderPath` calls `mat_inst.variant_for_pass(DepthPrepass)` in
the depth prepass and `mat_inst.variant_for_pass(Forward)` in the color pass. Each pass uses the shader
selected by the instance, not a hardcoded vert+frag pair.

### 4. Descriptor layout — per-material bindings — Deferred Phase 3.5

**What's missing:** Nothing in `MaterialTemplate` drives descriptor set creation or layout for set 1+
(per-material bindings). The `VulkanDescriptorAllocator` and `MaterialBindGroup` (formerly `MaterialInstance`)
are wired to hardcoded layouts, not artifact-driven layouts.

**What to add (Phase 3.5):**
- `MaterialTemplate` carries enough reflected binding data to construct a `VkDescriptorSetLayout` at load
  time (or defer to the first bind).
- `MaterialResourceLoader` merges spirv-reflect results across pass shaders to build the per-material
  binding table.
- `MaterialBindGroup` is rebuilt from `MaterialTemplate` rather than from a manually-constructed layout.

**Why deferred:** No concrete consumer (texture arrays, multiple samplers) until CSM and area-light
materials land in Phase 3.5, and post-FX materials in Phase 3.7.

### 5. Additional shader stages — Deferred Phase 3.5+

**What's missing:** The PASS chunk stores vertex+fragment shader pairs (one `ShaderResource` per PassType).
There is no slot for compute, mesh, or task shaders. A compute-only material (post-FX, particle simulation)
cannot be expressed.

**What to add (Phase 3.5):**
- Extend `ShaderResource` to carry multiple stages (vertex/fragment/compute/mesh as a tagged union).
- Update `MaterialTemplate::pass_shaders` value type to `ResourceHandle<ShaderResource>` where each
  `ShaderResource` declares its own stage set (already possible via the existing shader mechanism).
- The PASS chunk format is already stage-agnostic (one ResourceId per PassType entry). Only the shader
  artifact format changes — the material artifact format is unaffected.

**Why deferred:** Compute and mesh shaders are Phase 5 concerns. The PASS chunk format already accommodates
them — the `ShaderResource` inside can carry any combination of stages.

---

**Updated execution plan:**
- Phase 2.7 v1c closes the artifact layer of items 1–3 (full material foundation: ADR-0048).
- Phase 2.8 wires items 2–3 to actual Vulkan pipeline compilation and multi-pass rendering.
- Items 4 and 5 remain open; deferred until consumers create real demand (item 4 → Phase 3.5 CSM /
  area lights; item 5 → Phase 3.7 post-FX compute / Phase 3.8 GPU-driven culling).

See `docs/phases/phase-2.7-asset-import.md`, `docs/phases/phase-2.8-material-completion.md`,
ADR-0044, ADR-0046, ADR-0048.

## Long-term deferred

- **Multi-viewport ImGui** — Vulkan multi-viewport has known rough edges.
  Single-viewport docking only until `crd-ui` ships (planned Phase 5+).
  At that point, game/editor surfaces move to `crd-ui`; ImGui stays debug-only
  and multi-viewport is no longer needed.

## GPU instancing (planned Phase 3.2)

v1h ships `draw_indexed(index_count, first_index, vertex_offset)` — non-instanced only
(`instance_count` hardwired to 1 in the Vulkan call). When instancing lands:

**RHI changes:**
- Add `draw_instanced(vertex_count, instance_count, first_vertex, first_instance)` to
  `CommandBuffer` (non-indexed instanced path).
- Add `draw_indexed_instanced(index_count, instance_count, first_index, vertex_offset, first_instance)`
  to `CommandBuffer` (indexed instanced path, mirrors `vkCmdDrawIndexed` fully).
- All four draw variants (`draw`, `draw_indexed`, `draw_instanced`, `draw_indexed_instanced`)
  coexist; `VulkanCommandBuffer` implements all four.

**Renderer changes:**
- `Renderable` and `DrawItem` gain `instance_count = 1` (default keeps backward compat).
- `ForwardRenderPath` dispatch logic: `instance_count == 1` → non-instanced path (no
  regression); `instance_count > 1` → instanced path.
- GPU instance data buffer (transforms, material indices) is a Phase 3 GPU scene buffer
  concern — `crd-resources` provides per-frame upload; `ForwardRenderPath` binds it as
  a storage buffer at set 0 binding 1 or via push constants for the base instance.

**When:** After Phase 3.1 (stable entity/transform storage in the scene system) ships and
a GPU instance data layout is frozen. Instancing without a stable instance buffer contract
produces nothing useful. Target: Phase 3.2.

**Do NOT prematurely add `instance_count` to `Renderable` / `DrawItem` before that point.**

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

- **Disabled-trace benchmark** (2026-05-03) — Replaced compile-time-eliminated
  `CRD_LOG_TRACE` call with `CRD_LOG_INFO` gated by `runtime_level = Error`. The
  benchmark now measures the runtime short-circuit cost in all build configurations.
- **Doxygen per-symbol comments in crd-core** (2026-05-03) — Added `///` docs to all
  symbols in `types.hpp` (14 aliases), `platform.hpp` (18 macros + 3 functions), and
  `assert.hpp` (2 type aliases + 4 functions + 4 macros).
- **No SPSC RingBuffer** (2026-05-03) — Added `SpscQueue<T>` in
  `engine/containers/include/crd/containers/spsc_queue.hpp`. Lock-free, cache-line
  padded head/tail atomics, wait-free push and pop. Tested single-threaded and with
  concurrent 1M-item producer/consumer.
- **No file watcher in crd-platform** (2026-05-03) — Added polling-based `FileWatcher`
  in `engine/platform/`. Uses `fs::last_modified_unix_seconds()` on each polled path.
  Handles add/remove by handle, fires callbacks synchronously in `poll()`.
- **Multi-viewport ImGui deferred** (2026-05-03) — Moved to "Long-term deferred" above.
  Will not land until `crd-ui` ships; ImGui stays debug-only forever.
