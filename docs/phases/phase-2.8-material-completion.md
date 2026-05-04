# Phase 2.8 — Material GPU Wiring + Depth-Only Prepass

**Status:** ⏳ planned — begins after Phase 2.7 v1d ships
**ADRs:** ADR-0044 (phase ordering decision), ADR-0048 (material system architecture — artifact format done in Phase 2.7 v1c)
**New modules:** none — extends `crd-renderer` only
**Depends on:** Phase 2.7 complete (TextureResource, MeshResource, MaterialTemplate with full MATR format)

---

## Goal

Wire the material artifact data established by Phase 2.7 v1c (ADR-0048) into actual GPU pipeline
compilation and multi-pass rendering. The MATR artifact format is already complete — Phase 2.8 does
no format changes. It reads the data and drives Vulkan.

These wiring steps are **prerequisites for the scene system** (Phase 3.0), not post-scene cleanup:
- **PSO state wiring:** `MaterialTemplate::pso_states` (from `PSOS` chunk) must drive `VkGraphicsPipeline`
  compilation. Without a per-material pipeline cache, every renderable shares one pipeline regardless of
  alpha mode or cull hints.
- **Multi-pass rendering:** `ForwardRenderPath` must call `mat_inst.variant_for_pass(DepthPrepass)` and
  `mat_inst.variant_for_pass(Forward)` to use the correct shader per pass. The `PASS` chunk is in the
  artifact; the render path just needs to call it.

Items 4 (artifact-driven descriptor layouts) and 5 (additional shader stages) are deferred to Phase 3.4+
where CSM, post-FX, and GPU culling create real consumers.

**Out of scope for Phase 2.8:**
- HDR render target / tone mapping (Phase 3.4)
- Bloom, TAA, ACES tone mapping (Phase 3.4–3.6)
- Descriptor layout driven from material artifact (debt item 4 — Phase 3.4)
- Compute/mesh shader stages in MATR (debt item 5 — Phase 3.5+)
- Shadow maps (Phase 3.1+ scene/physics integration)
- Async pipeline compilation (noted in renderer optimization backlog — Phase 3.4)

---

## Architecture

Phase 2.7 v1c already ships: `MaterialTemplate`, `MaterialInstance`, `ParameterType`, `CookedParameter`,
`ShaderOptionDecl`, `MaterialDomain`, `PassType`, `RasterState`. The full MATR artifact format (INFO/PRMS/DFLT/PASS/PSOS/OPTS) is also complete. See ADR-0048.

Phase 2.8 adds **no new types and no new FourCCs**. It extends `ForwardRenderPath` and the pipeline resolver to consume the data that Phase 2.7 put in the artifact.

### What Phase 2.8 wires

**v1a — Per-material pipeline cache:**
`ForwardRenderPath` currently has no per-material pipeline cache. v1a adds one keyed by
`(VariantKey, RasterState)`. Cache miss → compile a new `GraphicsPipelineDesc` incorporating
the material's `pso_states[pass]` into the VkPipeline. `ForwardRenderPath` also skips materials
with `domain != MaterialDomain::Surface`.

**v1b — Multi-pass shader selection:**
`ForwardRenderPath` calls `mat_inst.variant_for_pass(DepthPrepass)` in the depth prepass and
`mat_inst.variant_for_pass(Forward)` in the color pass. Each pass gets the correct shader from
the `PASS` chunk, with ShaderOptions evaluated by `MaterialInstance`.

**v1c — Depth-only prepass pipeline:**
The depth prepass builds a vertex-only `GraphicsPipelineDesc` (no fragment shader, `color_format =
Format::Undefined`, `depth_format = Format::D32Sfloat`). Pipeline resolver stores per-material
`{depth_pipeline, color_pipeline}` pairs.

---

## Slices

### v1a — Per-material pipeline cache (PSO state wiring)

**Scope:**
- Per-material pipeline cache in `ForwardRenderPath` keyed by `(VariantKey, RasterState)` hash.
  Cache miss → compile a new `GraphicsPipelineDesc` incorporating `material->pso_states[pass_type]`
  (from the Phase 2.7 `PSOS` chunk) into the `VkGraphicsPipelineCreateInfo`.
- `ForwardRenderPath` checks `material->domain` and skips non-`Surface` materials (PostProcess/Compute
  materials are dispatched by their own systems in Phase 3.4).
- No new types, no new FourCCs, no loader changes — all artifact data was populated by Phase 2.7 v1c.

**Tests:**
- Opaque material (default RasterState, Surface domain) → same pipeline as before (regression guard).
- Transparent material (`alpha_mode = Transparent`, `depth_write = false`) → distinct pipeline key.
- Two-sided material (`cull_face = None`) → third distinct pipeline key.
- PostProcess domain material → `ForwardRenderPath` skips it (no pipeline created).

### v1b — Multi-pass shader selection

**Scope:**
- `ForwardRenderPath` calls `mat_inst.variant_for_pass(PassType::DepthPrepass)` in the depth prepass
  and `mat_inst.variant_for_pass(PassType::Forward)` in the color pass.
- ShaderOptions evaluated by `MaterialInstance::variant_for_pass()` drive the `VariantKey` used to
  look up the compiled SPIR-V permutation from `ShaderResource`.
- Fallback policy when a pass shader is absent: use `PassType::Forward` shader for all passes (render-path
  policy; consistent with the Phase 2.7 loader's legacy META synthesis).
- No format changes — `PASS` chunk data already loaded by `MaterialTemplate`.

**Tests:**
- Material with explicit `DepthPrepass` + `Forward` variant → render path picks the correct one per pass.
- Material with only `Forward` variant → depth prepass uses it as fallback; no crash.
- Legacy `META`-only artifact → synthesized `Forward` slot; `ForwardRenderPath` operates normally.

### v1c — Depth-only prepass pipeline

**Scope:**
- `ForwardRenderPath` depth prepass builds a vertex-only `GraphicsPipelineDesc`: no fragment shader,
  `color_format = Format::Undefined`, `depth_format = Format::D32Sfloat`.
- Pipeline resolver stores per-material `{depth_pipeline, color_pipeline}` — two compiled pipelines per
  unique `(VariantKey, RasterState)` pair.
- Draw items in the depth prepass submit against the depth pipeline; the color pass submits against the
  color pipeline.
- Proves v1a + v1b end-to-end with a real pipeline distinction.
- `smoke_depth_prepass.exe` (GPU smoke): render one frame, assert depth image non-zero, exit 0. Skipped on
  headless CI (added to GPU/window smoke list in `CLAUDE.md`).

**Tests:**
- `ForwardRenderPath` produces two distinct pipeline objects for a material with a depth variant.
- Pipeline resolve for depth pass uses null fragment shader.

---

## Module layout (planned additions)

All new types (`RasterState`, `PassType`, `MaterialTemplate`, `MaterialInstance`, etc.) were shipped in
Phase 2.7 v1c. Phase 2.8 changes are confined to `ForwardRenderPath` implementation.

```
engine/renderer/
  src/
    forward_render_path.cpp       ← v1a + v1b + v1c (pipeline cache, domain skip, pass dispatch, depth-only pipeline)

runtime/examples/
  smoke_depth_prepass.cpp         ← v1c (GPU smoke)
```

---

## Definition of done (Phase 2.8)

1. All three slices (v1a–v1c) shipped with unit tests.
2. `smoke_depth_prepass.exe` runs exit 0 on a Vulkan-capable machine, one frame rendered with
   distinct depth and color pipelines, no Vulkan validation errors.
3. `smoke_asset_import.exe` from Phase 2.7 continues to pass (no regression).
4. Material debt items 2–3 GPU wiring closed; `docs/debt.md` updated.
5. Six-configuration green: win-debug / win-relwithdebinfo / win-release / win-asan / win-clang-cl / win-tidy.
6. `docs/systems/renderer.md` updated to document the per-material pipeline cache and multi-pass dispatch.

---

## Open questions

- **Fallback policy when a pass shader is absent.** If a material has no `PassType::Depth` entry, what does
  `ForwardRenderPath` do? Options: (a) skip the draw item in that pass, (b) use `MainColor` shader.
  Lean toward (b) — simpler, correct for opaque geometry; masked/transparent items with absent depth shaders
  should also specify `depth_write = false` via RasterState so their absence is harmless.
- **Pipeline cache key hash.** `(VariantKey, RasterState)` — use `VariantKey`'s existing hash extended by
  a `memcmp`-based hash of `RasterState` bytes, or add a dedicated `RasterStateKey` hash type?

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
- `docs/phases/phase-2.7-asset-import.md` — Predecessor phase (all new types shipped in v1c)
- `docs/phases/phase-3.0-scene-ecs.md` — Successor phase
