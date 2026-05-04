# Phase 2.8 — Material System Completion (debt items 2–3) + Depth-Only Prepass

**Status:** ⏳ planned — begins after Phase 2.7 v1d ships
**ADRs:** ADR-0044 (phase ordering decision)
**New modules:** none — extends `crd-renderer` only
**Depends on:** Phase 2.7 complete (TextureResource, MeshResource, MaterialResource with params/textures)

---

## Goal

Close material debt items 2 and 3 from `docs/debt.md` before Phase 3.0 scene/ECS starts.

These two items are **prerequisites for the scene system**, not post-scene cleanup:
- **Item 2 (PSO state):** Without per-material blend/depth/cull state in the MATR artifact, the scene system
  cannot classify draw items correctly. Every renderable would share one pipeline regardless of alpha mode or
  cull hints. Installing this after scene coding starts means designing the scene system around a known gap.
- **Item 3 (pass-keyed variants):** Multi-pass rendering (depth prepass, future shadow maps) has no plumbing
  without a per-pass shader slot. `ForwardRenderPath` already has a depth prepass and a color pass; making each
  draw item advertise which shader it uses per pass is the missing piece.

Items 4 (artifact-driven descriptor layouts) and 5 (additional shader stages) are deferred to Phase 3.4+ where
CSM, post-FX, and GPU culling create real consumers. Closing them now is horizontal scaffolding with no payoff.

**Out of scope for Phase 2.8:**
- HDR render target / tone mapping (Phase 3.4)
- Bloom, TAA, ACES tone mapping (Phase 3.4–3.6)
- Descriptor layout driven from material artifact (debt item 4 — Phase 3.4)
- Compute/mesh shader stages in MATR (debt item 5 — Phase 3.5+)
- Shadow maps (Phase 3.1+ scene/physics integration)
- Async pipeline compilation (noted in renderer optimization backlog — Phase 3.4)

---

## Architecture

### `RasterState` — new shared struct

```cpp
// engine/renderer/include/crd/renderer/raster_state.hpp
namespace crd::renderer
{
enum class AlphaMode : crd::u8 { Opaque, Masked, Transparent };
enum class CullMode  : crd::u8 { Back, Front, None };
enum class FillMode  : crd::u8 { Solid, Wireframe };
enum class BlendMode : crd::u8 { Zero, One, SrcAlpha, OneMinusSrcAlpha, SrcColor, OneMinusSrcColor };

struct RasterState
{
    AlphaMode alpha_mode   = AlphaMode::Opaque;
    CullMode  cull_face    = CullMode::Back;
    FillMode  fill_mode    = FillMode::Solid;
    bool      depth_test   = true;
    bool      depth_write  = true;
    BlendMode src_blend    = BlendMode::One;
    BlendMode dst_blend    = BlendMode::Zero;
    crd::u8   _pad[1]      = {};
};
static_assert(sizeof(RasterState) == 8);
} // namespace crd::renderer
```

### `PassType` enum

```cpp
// engine/renderer/include/crd/renderer/pass_type.hpp
namespace crd::renderer
{
enum class PassType : crd::u8
{
    Depth      = 0,   // depth prepass / shadow map depth fill
    MainColor  = 1,   // primary shaded color pass
    // future: ShadowMap = 2, Overlay = 3, ...
};
} // namespace crd::renderer
```

### Extended `MaterialResource`

After Phase 2.8 v1a + v1b:

```cpp
struct MaterialResource
{
    // v2.8a — PSO state (from RAST chunk)
    RasterState raster_state;

    // v2.8b — per-pass shader handles (from PASS chunk; replaces flat vert+frag pair)
    crd::containers::HashMap<PassType, ResourceHandle<ShaderResource>> pass_shaders;

    // v2.7c — parameters and textures
    crd::containers::HashMap<crd::containers::String, crd::math::Vec4f>                    parameters;
    crd::containers::HashMap<crd::containers::String, ResourceHandle<TextureResource>>     textures;

    explicit MaterialResource(crd::IAllocator* a);
};
```

### CRDR FourCCs added in 2.8

| FourCC | Meaning |
|--------|---------|
| `RAST` | RasterState chunk (8 bytes: AlphaMode, CullMode, FillMode, depth_test/write flags, src/dst blend) |
| `PASS` | Pass-keyed shader table chunk: `[pass_type u8, pad u8[3], resource_id u8[16]][]` |

The old hardcoded `META` 32-byte chunk (vert UUID + frag UUID) from Phase 2.6 v1e is superseded by `PASS`.
Old v1 MATR artifacts (without `RAST` or `PASS` chunks) load with default `RasterState` and a `MainColor` entry
synthesized from the `META` chunk — fully backward compatible.

---

## Slices

### v1a — PSO state + MaterialDomain in MATR artifact

**Scope:**
- `RasterState` struct in `engine/renderer/include/crd/renderer/raster_state.hpp`.
- `kFourCC_RAST` added to `crdr.hpp`.
- `MaterialDomain` enum in `engine/renderer/include/crd/renderer/material_domain.hpp`:
  `Surface` (0), `PostProcess` (1), `Compute` (2), `Decal` (3), `UI` (4). See ADR-0046.
- `kFourCC_DOMN` added to `crdr.hpp`. `DOMN` chunk: 4 bytes (MaterialDomain u8 + pad u8[3]).
- `MaterialResource` gains a `MaterialDomain domain` field.
- `MaterialResourceLoader` parses optional `DOMN` chunk; missing → `MaterialDomain::Surface` (backward compat).
- Cooker `.mat.toml` handler: parse optional `domain` key (`"surface"` / `"post_process"` / `"compute"` / etc.); emit `DOMN` chunk.
- `ForwardRenderPath` skips non-`Surface` materials (no-op; PostProcess/Compute materials are dispatched by their own systems in Phase 3.4).
- `MaterialResourceLoader` updated: parse optional `RAST` chunk into `MaterialResource::raster_state`; missing chunk
  → default `RasterState{}` (backward compatible with v1e artifacts).
- Cooker `.mat.toml` handler extended: parse optional `[raster]` table
  (`alpha_mode`, `cull_face`, `fill_mode`, `depth_test`, `depth_write`, `src_blend`, `dst_blend`); emit `RAST` chunk.
- Per-material pipeline cache in `ForwardRenderPath`: keyed by `(VariantKey, RasterState)` hash. Cache miss →
  compile new `GraphicsPipelineDesc` incorporating the material's `RasterState`.
- Bump `kMaterialLoaderVersion` to 2.

**Tests:**
- Opaque material (default RasterState, Surface domain) → same pipeline as before (regression guard).
- Transparent material (`alpha_mode = Transparent`, `depth_write = false`) → distinct pipeline key.
- Two-sided material (`cull_face = None`) → third pipeline key.
- PostProcess domain material → `ForwardRenderPath` skips it (no pipeline created for it).
- Old v1 MATR artifact (no RAST/DOMN chunks) loads without error, defaults correct.
- `.mat.toml` with `[raster]` + `domain` field → round-trip through cooker + loader.

### v1b — Pass-keyed shader variants in MaterialResource

**Scope:**
- `PassType` enum in `engine/renderer/include/crd/renderer/pass_type.hpp`.
- `kFourCC_PASS` added to `crdr.hpp`.
- `MaterialResource` replaces flat `vert_handle` + `frag_handle` fields with
  `HashMap<PassType, ResourceHandle<ShaderResource>> pass_shaders`.
- `MaterialResourceLoader` updated: parse `PASS` chunk (preferred) or synthesize from legacy `META` chunk
  (two UUIDs → create a `MainColor` entry, calling `load_sync<ShaderResource>` for each).
- Cooker `.mat.toml` handler extended: parse `[passes.depth]` + `[passes.main_color]` sections (each names
  a GLSL source file path); each → its own `ShaderResource` UUID + artifact; emits one `PASS` chunk entry per section.
- `ForwardRenderPath` queries `mat->pass_shaders[PassType::Depth]` in the depth prepass and
  `mat->pass_shaders[PassType::MainColor]` in the color pass.
- Bump `kMaterialLoaderVersion` to 3.

**Tests:**
- Material with explicit depth + main color variant → loader populates both slots; ForwardRenderPath picks
  the correct one per pass.
- Material with only `main_color` → depth prepass falls back to `MainColor` variant (render-path policy).
- Legacy META-only artifact → synthesized `MainColor` slot; no crash in ForwardRenderPath.
- `.mat.toml` with `[passes.depth]` and `[passes.main_color]` sections → end-to-end round-trip.

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

```
engine/renderer/
  include/crd/renderer/
    raster_state.hpp              ← v1a (RasterState, AlphaMode, CullMode, FillMode, BlendMode)
    pass_type.hpp                 ← v1b (PassType)
  src/
    material_resource_loader.cpp  ← v1a + v1b (extended)
    forward_render_path.cpp       ← v1a + v1b + v1c (pipeline cache, pass dispatch)

runtime/examples/
  smoke_depth_prepass.cpp         ← v1c (GPU smoke)
```

---

## Definition of done (Phase 2.8)

1. All three slices (v1a–v1c) shipped with unit tests.
2. `smoke_depth_prepass.exe` runs exit 0 on a Vulkan-capable machine, one frame rendered with
   distinct depth and color pipelines, no Vulkan validation errors.
3. `smoke_asset_import.exe` from Phase 2.7 continues to pass (no regression).
4. Material debt items 2 and 3 closed; `docs/debt.md` updated.
5. Six-configuration green: win-debug / win-relwithdebinfo / win-release / win-asan / win-clang-cl / win-tidy.
6. `docs/systems/renderer.md` updated to document `RasterState`, `PassType`, and per-material pipeline cache.

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
- ADR-0025 — Shader mechanism policy
- ADR-0026 — Shader variant key
- ADR-0030 — Shader / PSO boundary
- ADR-0032 — Frame graph v1
- `docs/debt.md` — Material system v1 gaps (items 2 and 3 closed here; items 4–5 deferred to Phase 3.4)
- `docs/phases/phase-2.7-asset-import.md` — Predecessor phase
- `docs/phases/phase-3.0-scene-ecs.md` — Successor phase
