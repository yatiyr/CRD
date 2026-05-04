# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

## Active debt

_(none — all items cleared as of 2026-05-03)_

---

## Material system v1 known gaps

`MaterialResource` as shipped in v1e is a loader proof-of-concept, not a production material abstraction.
Status per Phase 2.7 / 2.8 planning (ADR-0044):
- **Item 1** — Closed by Phase 2.7 v1c (TextureResource + material params/textures).
- **Items 2–3** — Scheduled for Phase 2.8 (PSO state + pass-keyed variants). Prerequisites for Phase 3.0 scene/ECS.
- **Items 4–5** — Deferred to Phase 3.4+ (no consumer until CSM / post-FX / compute passes exist).

### 1. Material parameters (uniforms + textures) ✅ Closes Phase 2.7 v1c

**What's missing:** `MaterialResource` holds two shader handles and nothing else. There is no place to
store per-material uniform values (albedo color, roughness, metallic factor, emissive scale, tiling) or
texture slot bindings (albedo map UUID, normal map UUID, roughness/metallic map UUID, AO UUID, emissive UUID).

**What to add:**
- Extend the MATR artifact META / BLOB chunk to carry a parameter table: typed key→value pairs for
  scalars/vectors and a texture slot table (`slot_name → ResourceId`).
- `MaterialResource` stores these as a `HashMap<String, ParameterValue>` and a
  `HashMap<String, ResourceHandle<TextureResource>>`.
- `MaterialResourceLoader::load()` calls `ctx.manager->load_sync<TextureResource>(tex_id)` transitively
  for each texture slot (same pattern as the shader dep load today).
- The cooker's `.mat.toml` handler parses `[parameters]` and `[textures]` sections and serialises them.

**Why deferred:** `TextureResource` and `TextureResourceLoader` don't exist yet (no texture asset cooker
path). Land texture cooker first, then wire material parameters.

### 2. PSO state in the material artifact — Scheduled Phase 2.8 v1a

**What's missing:** Blend mode, depth test, depth write, stencil, cull mode, alpha mode, fill mode —
all the per-pipeline-state that varies wildly across opaque / masked / transparent / decal / wireframe
materials — are not encoded anywhere in the MATR artifact. The renderer currently has these hardwired
in `ForwardRenderPath`.

**What to add:**
- A `RasterState` struct: `{ AlphaMode alpha_mode; CullMode cull_face; FillMode fill; bool depth_test;
  bool depth_write; BlendMode src_blend; BlendMode dst_blend; }`.
- Serialise into the MATR artifact (either extend META or add a `RAST` chunk, 16 bytes).
- `MaterialResource` stores a `RasterState` field.
- `ForwardRenderPath`'s `PipelineResolver::resolve_pipeline()` reads the material's `RasterState`
  and includes it in the `GraphicsPipelineDesc` key.

**Why deferred:** `PipelineResolver` is currently a stub with no per-material pipeline cache. Landing a
real pipeline cache (keyed by `(VariantKey, RasterState)`) is a prerequisite.

### 3. Shader variant awareness (VariantKey integration) — Scheduled Phase 2.8 v1b

**What's missing:** The `VariantKey` / `VariantPipelineDesc` machinery from `crd-shader` (ADR-0026,
Phase 2.3d/g) is entirely disconnected from `MaterialResource`. The material has no concept of render
passes, skinning axes, or shadow vs. color variants. It stores a single vert+frag pair with no way to
express "use this variant for the depth prepass, that variant for the shadow map pass, another for the
main color pass."

**What to add:**
- `MaterialResource` stores a `HashMap<PassType, ResourceId>` mapping each render pass type to a
  specific `ShaderResource` (or a `VariantKey` that the resolver uses to look up a compiled variant).
- The MATR artifact encodes this table (replace the hardcoded vert/frag pair with a pass-keyed table).
- `ForwardRenderPath` asks the material for the shader appropriate to the current pass type rather than
  always using the same shader.
- The cooker's `.mat.toml` can declare `[passes.depth]`, `[passes.shadow]`, `[passes.main_color]`
  sections pointing to different GLSL source files or variant overrides.

**Why deferred:** Requires the PSO pipeline cache (item 2) and a stable `PassType` enum to be locked
in before the artifact format can be defined. Also needs at least two distinct render passes in
`ForwardRenderPath` (depth prepass + main color) to be worth exercising — those exist in v1g but
the depth pipeline is currently using the full vertex+fragment pipeline.

### 4. Descriptor layout — per-material bindings — Deferred Phase 3.4

**What's missing:** Nothing in `MaterialResource` drives descriptor set creation or layout at set 1+
(per-material bindings). The existing `VulkanDescriptorAllocator` and `MaterialInstance` (in `crd-renderer`)
exist but are wired to hardcoded descriptor set layouts, not material-artifact-driven layouts.

**What to add:**
- `MaterialResource` carries a derived `DescriptorSetLayout` (or enough data to construct one) based
  on what the reflected shaders declare at set 1.
- `MaterialResourceLoader` runs `spirv-reflect` results from both shader stages through a merge pass
  (same stage-merge logic as `VariantPipelineDesc`) to build the per-material binding table.
- The renderer's `MaterialInstance` is rebuilt from `MaterialResource` rather than from a
  manually-constructed layout.

**Why deferred:** Blocked on item 1 (texture slot table) and item 2 (stable pipeline key). Without
real texture slots the descriptor layout is always trivial and the value isn't visible.

### 5. Additional shader stages — Deferred Phase 3.5+

**What's missing:** The 32-byte META chunk has exactly two UUID slots (vert + frag). There is no room
for a compute shader, geometry shader, mesh shader, or task shader. A compute-only material (post-FX,
particle simulation) cannot be expressed at all.

**What to add:**
- Replace the hardcoded 32-byte META layout with a small typed table:
  `[stage_type u8, padding u8[3], resource_id u8[16]][]` — one entry per stage present.
- `MaterialResource` stores a `HashMap<Stage, ResourceHandle<ShaderResource>>` instead of two named fields.
- Update `MaterialResourceLoader` to parse the variable-length stage table.
- Update the cooker's `.mat.toml` handler to accept any combination of `[stages.vertex]`,
  `[stages.fragment]`, `[stages.compute]`, `[stages.mesh]`, etc.

**Why deferred:** Post-FX (compute) and mesh shaders are Phase 5 concerns. Keep the format simple
until there's a concrete consumer. The format change is backward-incompatible (bump `kMaterialLoaderVersion`),
so defer until all the other format changes above are batched in.

---

**Execution plan (ADR-0044):**
- Phase 2.7 v1c closes item 1.
- Phase 2.8 closes items 2 and 3 (prerequisites for scene/ECS draw classification).
- Items 4 and 5 remain open; deferred until CSM/post-FX consumers in Phase 3.4+ create real demand.

See `docs/phases/phase-2.7-asset-import.md`, `docs/phases/phase-2.8-material-completion.md`, ADR-0044.

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
