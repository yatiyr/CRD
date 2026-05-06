# Phase 2.8 — Material GPU Wiring + Depth Prepass + Sandbox Rendering

**Status:** ✅ COMPLETE — v1a–v1e SHIPPED 2026-05-05; v1f + v1g SHIPPED 2026-05-06.
**ADRs:** ADR-0044 (phase ordering decision), ADR-0048 (material system architecture — artifact format done in Phase 2.7 v1c)
**New modules:** none — extends `crd-renderer` and `crd-sandbox` only
**Depends on:** Phase 2.7 complete (TextureResource, MeshResource, MaterialTemplate with full MATR format, GpuUploader, crd-meshgen)

---

## Goal

Wire the material artifact data established by Phase 2.7 v1c (ADR-0048) into actual GPU pipeline
compilation and multi-pass rendering, then bring the sandbox up to rendering real geometry through
the material system. Seven small slices — each builds on the previous, each shippable independently.

**Why this ordering matters for the successor phases:**
- PSO state wiring (v1a–v1c) is a prerequisite for the scene system (Phase 3.0) — without per-material
  pipelines, every renderable shares one pipeline regardless of alpha mode or cull hints.
- A concrete default material (v1d) is required before any geometry can render at all in v1e–v1g.
- Sandbox rendering (v1e–v1g) closes the "human-visible proof" gap before we commit to Phase 3.0 ECS scope.

**Out of scope for Phase 2.8:**
- HDR render target / tone mapping (Phase 3.4)
- Bloom, TAA, ACES tone mapping (Phase 3.4–3.6)
- Descriptor layout driven from material artifact (debt item 4 — Phase 3.4)
- Compute/mesh shader stages in MATR (debt item 5 — Phase 3.5+)
- Shadow maps (Phase 3.1+ scene/physics integration)
- Async pipeline compilation (renderer optimization backlog — Phase 3.4)
- GPU instancing (Phase 3.2)

---

## Architecture

Phase 2.7 v1c already ships: `MaterialTemplate`, `MaterialInstance`, `ParameterType`, `CookedParameter`,
`ShaderOptionDecl`, `MaterialDomain`, `PassType`, `RasterState`. The full MATR artifact format
(INFO/PRMS/DFLT/PASS/PSOS/OPTS) is also complete. See ADR-0048.

Phase 2.7 v1d already ships: `GpuUploader::upload_mesh()`, `GpuMesh` (vertex + index buffers),
`SandboxLayer` with Dear ImGui overlay and Meshgen Browser panel.

Phase 2.8 adds **no new types and no new FourCCs.** It extends `ForwardRenderPath` to consume the
artifact data, adds a concrete default material's GLSL source + mat.toml, wires `ForwardRenderPath`
into the sandbox, and adds demo assets + the asset browser.

### Resolved decisions

**Pipeline cache key:** `crd::u64 = VariantKey::hash() ^ fnv1a(reinterpret_cast<const u8*>(&raster_state), sizeof(RasterState))`.
No new key type needed — `VariantKey` already has a hash; XOR with a byte-hash of `RasterState`.

**Fallback when pass shader absent:** `ForwardRenderPath` uses the `PassType::Forward` shader for all
passes when `pass_shaders[DepthPrepass].vert` is null. This is the render-path policy (consistent with
the Phase 2.7 loader's legacy META synthesis). Masked/transparent items with absent depth shaders should
specify `depth_write = false` via `RasterState`, making their absence harmless.

---

## Slices

### v1a — Per-material pipeline cache (PSO state wiring)

**Scope:**
- Per-material pipeline cache in `ForwardRenderPath` keyed by `(VariantKey, RasterState)` hash
  (see resolved cache key above).
- Cache miss → compile a new `GraphicsPipelineDesc` incorporating `material->pso_states[pass_type]`
  (from the Phase 2.7 `PSOS` chunk) into the `VkGraphicsPipelineCreateInfo`.
- `ForwardRenderPath` checks `material->domain` and skips non-`Surface` materials (`PostProcess`/`Compute`
  materials are dispatched by their own systems in Phase 3.4).
- No new types, no new FourCCs, no loader changes — all artifact data was populated by Phase 2.7 v1c.

**Tests:**
- Opaque material (default `RasterState`, `Surface` domain) → same pipeline as before (regression guard).
- Transparent material (`alpha_mode = Transparent`, `depth_write = false`) → distinct pipeline key.
- Two-sided material (`cull_face = None`) → third distinct pipeline key.
- `PostProcess` domain material → `ForwardRenderPath` skips it (no pipeline created).

---

### v1b — Multi-pass shader selection

**Scope:**
- `ForwardRenderPath` calls `mat_inst.variant_for_pass(PassType::DepthPrepass)` in the depth prepass
  and `mat_inst.variant_for_pass(PassType::Forward)` in the color pass.
- `ShaderOptions` evaluated by `MaterialInstance::variant_for_pass()` drive the `VariantKey` used to
  look up the compiled SPIR-V permutation from `ShaderResource`.
- Fallback: when `pass_shaders[DepthPrepass].vert` is null, use `PassType::Forward` shader for all
  passes (see resolved fallback above).
- No format changes — `PASS` chunk data already loaded by `MaterialTemplate`.

**Tests:**
- Material with explicit `DepthPrepass` + `Forward` variant → render path picks the correct one per pass.
- Material with only `Forward` variant → depth prepass uses it as fallback; no crash.
- Legacy `META`-only artifact → synthesized `Forward` slot; `ForwardRenderPath` operates normally.

---

### v1c — Depth-only prepass pipeline

**Scope:**
- `ForwardRenderPath` depth prepass builds a vertex-only `GraphicsPipelineDesc`: no fragment shader,
  `color_format = Format::Undefined`, `depth_format = Format::D32Sfloat`.
- Pipeline resolver stores per-material `{depth_pipeline, color_pipeline}` — two compiled pipelines per
  unique `(VariantKey, RasterState)` pair.
- Draw items in the depth prepass submit against the depth pipeline; the color pass submits against the
  color pipeline.
- Proves v1a + v1b end-to-end with a real pipeline distinction.
- `smoke_depth_prepass.exe` (GPU smoke): render one frame, assert depth image non-zero, exit 0. Skipped
  on headless CI (added to GPU/window smoke list in `CLAUDE.md`).

**Tests:**
- `ForwardRenderPath` produces two distinct pipeline objects for a material with a depth variant.
- Pipeline resolve for depth pass uses null fragment shader.

---

### v1d — Default lit material

**Scope:**
- `engine/renderer/shaders/surface.vert.glsl` — vertex shader; reads the standard 48-byte interleaved
  vertex layout (`kMeshVertexStride`); outputs `VertexAttrs` (position_ws, normal_ws, uv0, tangent_ws)
  to the fragment stage. Uses `surface_data.glsl.inc` (already shipped in Phase 2.7).
- `engine/renderer/shaders/surface.frag.glsl` — fragment shader; implements `crd_evaluate_surface()`;
  default material: `base_color = vec3(1.0)`, `metallic = 0.0`, `roughness = 1.0`, `emissive = vec3(0.0)`,
  `occlusion = 1.0`; diffuse-only Lambertian lighting with a hardcoded directional light
  (placeholder — real light system is Phase 3.0).
- `assets/materials/default_lit.mat.toml` — material descriptor referencing `surface.vert.glsl` +
  `surface.frag.glsl`, `domain = "Surface"`, `alpha_mode = "Opaque"`, default parameter values.
- Built-in cook: `tools/asset_cooker/` produces `default_lit.crdr` from the above; added to the
  built-in pack so it is available without extra load calls.

**Tests:**
- `default_lit.crdr` loads cleanly through `MaterialResourceLoader`; `MaterialTemplate` fields match
  the mat.toml declaration.
- Fragment shader compiles via shaderc without errors (unit-level GLSL compilation test).

---

### v1e — Sandbox rendering

**Scope:**
- Wire `ForwardRenderPath` into `SandboxLayer`. The sandbox render loop is a **manual acquire /
  record / submit loop** (not `app.run()`), so integration must fit the existing structure in
  `sandbox/src/main.cpp` — `ForwardRenderPath::build_frame()` is called inside the existing
  `cmd.begin()` block, not via a separate frame-graph executor.
- `SandboxLayer` holds a `ForwardRenderPath`, a `GpuMesh` (currently selected shape), and a
  `MaterialInstance` (pointing at `default_lit.crdr`).
- On shape selection change: call `GpuUploader::upload_mesh()` to upload the new procedural mesh;
  destroy the previous `GpuMesh`.
- `ForwardRenderPath` renders the selected mesh with the default lit material; result blits to the
  swapchain image; Dear ImGui overlay renders on top (existing flow preserved).
- No asset browser redesign yet — Meshgen Browser panel extended to trigger upload on click.

**Tests:**
- Selecting a shape from the Meshgen Browser uploads a GPU mesh and triggers a render — verified by
  the `smoke_depth_prepass.exe` GPU smoke continuing to pass (no regression).
- No Vulkan validation errors on shape switch.

---

### v1f — Demo assets ✅ SHIPPED 2026-05-06

**As shipped:**
- `assets/source/BoxTextured.glb` (5 KB, CC-BY 4.0 with trademark limitations, Cesium/Khronos).
- `assets/source/Duck.glb` (118 KB, SCEA Shared Source 1.0, Sony/Khronos).
- `assets/source/BoomBox.glb` (10 MB, CC0 1.0, UX3D/Khronos). **Substituted for the originally
  planned Suzanne** — Suzanne is not in the canonical Khronos `glTF-Sample-Assets` repo, and
  `DamagedHelmet` (the obvious alternative) ships under CC BY-NC 4.0, which is awkward for a
  permissively-licensed engine demo. BoomBox is CC0 and remains a recognisable PBR test asset.
- `assets/source/checker_512.png` and `assets/source/bricks_512.png` — procedural CC0 textures
  generated by `assets/source/generate_textures.ps1` (committed for reproducibility; not invoked
  at build time — the PNGs are the canonical input to the cooker).
- `assets/source/LICENSES.md` — license table for each asset.
- `assets/source/.gitignore` ignores `.cook_cache/` and `.meta` sidecars for non-cookable files
  (`.md`, `.ps1`, `.gitignore`); cookable-asset `.meta` sidecars are committed for stable UUIDs.
- `cook-demo-assets` CMake custom target in `sandbox/CMakeLists.txt`: `add_custom_command(OUTPUT
  demo_assets.crdr DEPENDS asset_cooker ${DEMO_ASSETS_SOURCES})` — recooks when any source asset
  changes. `crd-sandbox` declares `add_dependencies(crd-sandbox cook-demo-assets)` and receives
  the pack path via the `CRD_DEMO_ASSETS_PACK` compile definition.

**Verification:**
- `asset_cooker manifest_dump assets/cooked/demo_assets.crdr` shows 5 entries (3 MESH + 2 TXTR)
  with stable UUIDs matching the committed `.meta` sidecars.
- `crd-sandbox --headless` mounts the pack and prints `ResourceManager: mounted '...' (5 entries)`.

---

### v1g — Asset browser ✅ SHIPPED 2026-05-06

**As shipped:**
- The Meshgen Browser ImGui panel was replaced with a unified **Asset Browser** panel
  (`sandbox/src/sandbox_layer.cpp`).
- Two `ImGui::CollapsingHeader` sections, both default-open:
  - **Procedural Shapes (8)** — Plane, Box, Sphere, Icosphere, Cylinder, Cone, Capsule, Torus
    (with the same per-shape parameter sliders as before).
  - **Imported Assets (3)** — BoxTextured (glTF), Duck (glTF), BoomBox (glTF).
- A unified `Array<AssetEntry>` backs the panel; `AssetEntry::kind` tags each row as
  `Procedural` or `Imported`. Click selects → on next `on_update` the chosen mesh is uploaded.
- Imported path: `SandboxLayer` constructs a `ResourceManager`, registers `MeshResourceLoader`,
  mounts `CRD_DEMO_ASSETS_PACK`. For each glTF file it reads the `<file>.glb.meta` sidecar to
  recover the cooker-minted UUID, then on click does `load_sync<MeshResource>(id)` followed by
  `GpuUploader::upload_mesh()`.
- Per-selection metadata pane shows: name, source ("Procedural" / "glTF"), vertex count, index
  count, triangle count. Procedural rows additionally render their parameter sliders.
- Graceful fallbacks (each logs a Warn; no crash):
  - Pack file missing → "Imported Assets" section hidden.
  - `.meta` sidecar missing → that one entry skipped.
  - UUID not in the mounted manifest (stale .meta after content edit) → that one entry skipped.

**API change carried in:** `GpuUploader::upload_texture` and `upload_mesh` were retyped from
non-const& to const&. Required so `ResourceHandle<T>::get()` (which returns `const T*`) can be
passed directly. No behavioural change.

**Bug fix carried in (device-destroy crash on app close):** `Application::detach_all_layers()`
previously called `on_detach()` and cleared `m_layer_stack` but left the unique_ptrs alive in
`m_owned_layers`. Their destructors then ran in `~Application` after the `Device` local in `main`
had been destroyed — VK destroy calls in layer dtors hit a freed device. Fixed by clearing
`m_owned_layers` inside `detach_all_layers()`. `sandbox/src/main.cpp` was reordered so
`device->wait_idle()` precedes `app.detach_all_layers()` (was the other way around);
`runtime/examples/smoke_imgui_overlay.cpp` reordered identically.

---

## Module layout (planned additions)

```
engine/renderer/
  shaders/
    surface.vert.glsl                 ← v1d (default lit vertex shader)
    surface.frag.glsl                 ← v1d (default lit fragment shader + crd_evaluate_surface impl)
  src/
    forward_render_path.cpp           ← v1a + v1b + v1c (pipeline cache, domain skip, pass dispatch, depth-only pipeline)

assets/
  source/
    BoxTextured.glb                   ← v1f
    Duck.glb                          ← v1f
    Suzanne.glb                       ← v1f
    checker_512.png                   ← v1f
    bricks_512.png                    ← v1f
    LICENSES.md                       ← v1f
  materials/
    default_lit.mat.toml              ← v1d
  cooked/
    (generated by cook-demo-assets)   ← v1f

sandbox/
  src/
    sandbox_layer.cpp                 ← v1e (ForwardRenderPath wiring) + v1g (asset browser panel)
    sandbox_layer.hpp                 ← v1e + v1g

runtime/examples/
  smoke_depth_prepass.cpp             ← v1c (GPU smoke)
```

---

## Definition of done (Phase 2.8)

1. **v1a–v1c** (GPU pipeline wiring) shipped with unit tests; all tests pass across six configurations.
2. **`smoke_depth_prepass.exe`** runs exit 0 on a Vulkan-capable machine: one frame rendered with
   distinct depth and color pipelines, no Vulkan validation errors.
3. **v1d** (default lit material) ships: `surface.vert.glsl`, `surface.frag.glsl`, `default_lit.mat.toml`,
   built-in cook pack. GLSL compiles clean; material loads without error.
4. **v1e** (sandbox rendering): selecting a shape in the sandbox renders it via `ForwardRenderPath`
   with the default lit material. No Vulkan validation errors on shape switch.
5. **v1f** (demo assets): `assets/source/` committed with `LICENSES.md`; `cook-demo-assets` target
   produces valid `.crdr` packs; `BoxTextured.crdr` loads cleanly.
6. **v1g** (asset browser): unified panel lists all 11 items; click-to-switch between procedural and
   glTF assets works without crash or GPU leak.
7. **Six-configuration green** (win-debug / win-relwithdebinfo / win-release / win-asan / win-clang-cl / win-tidy)
   for all changed files.
8. `smoke_asset_import.exe` (Phase 2.7) continues to pass — no regression.
9. Material debt items 2–3 GPU wiring closed; `docs/debt.md` updated.
10. `docs/systems/renderer.md` updated to document the per-material pipeline cache and multi-pass dispatch.

---

## References

- ADR-0044 — Phase ordering decision (material PSO/variant before scene/ECS)
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy
- ADR-0048 — Material system architecture foundation (artifact format + all new types — Phase 2.7 v1c)
- ADR-0025 — Shader mechanism policy
- ADR-0026 — Shader variant key
- ADR-0030 — Shader / PSO boundary
- ADR-0032 — Frame graph v1
- `docs/debt.md` — Material system v1 gaps (artifact layer of items 2–3 closed by Phase 2.7 v1c; GPU wiring closed here)
- `docs/phases/phase-2.7-asset-import.md` — Predecessor phase (all new types shipped in v1c; GpuUploader + sandbox in v1d; meshgen in v1e)
- `docs/phases/phase-3.0-scene-ecs.md` — Successor phase
- `engine/renderer/include/crd/renderer/surface_data.glsl.inc` — GLSL contract for `VertexAttrs`, `SurfaceData`, `crd_evaluate_surface()` (shipped Phase 2.7)
