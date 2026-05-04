# ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy

**Status:** Accepted
**Date:** 2026-05-04
**Tags:** arch, renderer, shader, materials, rt, post-fx

---

## Context

Three future-facing design questions must be answered before Phase 2.7/2.8 implementation begins,
because the answers affect the MATR artifact format and the RHI surface, and changing those later is
expensive.

1. **Post-FX materials** (bloom, SSAO, tone map) operate on screen-space images, not mesh vertex data.
   They need a different "domain" from surface materials. Without a domain concept in the artifact, the
   renderer cannot route materials to the correct frame graph passes.

2. **Node editor shaders** (Phase 7) must integrate without requiring runtime changes. The stability
   point must be established now.

3. **Ray tracing** will be a Phase 5 feature. The architectural strategy must be reserved now so the
   Phase 3–4 renderer does not paint itself into a corner.

---

## Decisions

### 1. `MaterialDomain` — added to MATR artifact in Phase 2.8 v1a

A `DOMN` chunk (4 bytes) is added to the MATR artifact format alongside `RAST` and `PASS`.

```cpp
// engine/renderer/include/crd/renderer/material_domain.hpp
namespace crd::renderer
{
enum class MaterialDomain : crd::u8
{
    Surface      = 0,   // standard mesh surface — depth, shadow, main color passes
    PostProcess  = 1,   // full-screen effect — reads from frame graph output images
    Compute      = 2,   // GPU-side computation — particles, skinning, culling
    Decal        = 3,   // projected onto surfaces after main pass
    UI           = 4,   // 2D screen-space (crd-ui, Phase 5)
};
} // namespace crd::renderer
```

**Domain semantics:**
- `Surface`: participates in depth prepass + shadow pass + main color pass. Input: vertex data.
- `PostProcess`: runs in a full-screen pass after the main color pass. Inputs: color buffer, depth
  buffer, screen-space normals, motion vectors (all from the frame graph).
- `Compute`: no vertex stage. Runs in a compute dispatch. Inputs: declared UAVs/SRVs.
- `Decal`: runs after main pass. Reads depth to reconstruct world position.
- `UI`: 2D screen-space. No depth test. Blocked until `crd-ui` ships (Phase 5).

**Default value:** `MaterialDomain::Surface`. Artifacts without a `DOMN` chunk load as `Surface`
(backward compatible with all existing v1–v2 MATR artifacts).

**Frame graph routing:** `ForwardRenderPath` checks `material->domain` and skips materials with
non-`Surface` domains (they are routed to dedicated passes by the respective render systems).
Post-process materials are owned and dispatched by `PostProcessStack` (Phase 3.4).

### 2. Node editor shaders — zero runtime changes required

The runtime never sees node graphs. The pipeline is:

```
Node graph (Phase 7 editor tool)
    → compile → GLSL source
    → compile → SPIR-V
    → pack → ShaderResource CRDR artifact (same format as hand-written GLSL shaders)
    → cook → MATR artifact (same PASS/RAST/DOMN/PARM/TEXS chunks)
    → load → MaterialResource (same runtime type)
```

All node graph authoring lives in `tools/node_editor/` (Phase 7). The `crd-resources` loader sees the
same artifact it sees today. No new loader types, no new chunk types, no new runtime interfaces are
needed for node editor support. This is the same approach used by Unreal Engine (material compiler
outputs HLSL) and Godot (VisualShader outputs its shader language).

**Implication:** The artifact format established by Phases 2.6–2.8 is the stability boundary. Design
decisions made there (chunk types, SPIR-V storage, reflection data) must remain stable through Phase 7.
Format version bumps (`kMaterialLoaderVersion`) are allowed; structural breaks are not.

### 3. Ray tracing — hybrid rasterize + RT secondary effects

**Strategy:** Hybrid rendering. Rasterize primary visibility (existing `ForwardRenderPath` — fast, runs
on all GPUs). Ray-trace secondary effects only (reflections, ambient occlusion, one-bounce GI, shadows).

```
ForwardRenderPath (rasterized primary)
    ↓ color + depth + normals + motion vectors
HybridRenderPath (Phase 5) — implements IRenderPath
    ↓ BLAS per MeshResource + TLAS per frame
    ↓ RT passes: RTAO, RT reflections, RT shadows, RTGI probes
    ↓ denoiser (DLSS / FSR / Intel OIDN)
    ↓ composite into final color
PostProcessStack (Phase 3.4)
    ↓ bloom, TAA, tone map, DoF
Swapchain blit
```

**Fallback policy (software substitution for non-RT hardware):**
| RT effect | Software fallback |
|-----------|------------------|
| RT ambient occlusion | SSAO / GTAO (Phase 3.5) |
| RT reflections | SSR (Phase 3.5) |
| RT shadows | CSM (Phase 3.4) |
| RT global illumination | IBL + SSAO (Phase 3.4) |

**RHI extensions required (Phase 5):**
- `AccelerationStructure` resource type (BLAS per MeshResource, TLAS per scene)
- `RayTracingPipeline` pipeline type (ray generation, miss, closest-hit, any-hit shaders)
- New GLSL stages: `VK_SHADER_STAGE_RAYGEN_KHR`, `VK_SHADER_STAGE_MISS_KHR`, `VK_SHADER_STAGE_CLOSEST_HIT_KHR`
- Vulkan extensions: `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`

These are **additive extensions** to `crd-rhi`, not replacements. The existing RHI surface is unchanged.
`crd-rhi-vulkan` gains RT support as an opt-in capability query (`Device::supports_ray_tracing()`).

**`IRenderPath` is the correct abstraction.** `HybridRenderPath` is a new implementation alongside
`ForwardRenderPath`. ADR-0016 reserved the Visibility-Buffer path for Phase 5; that same slot serves
`HybridRenderPath`. No restructuring of the renderer interface is needed.

**Full path tracing** (Cyberpunk 2077 overdrive style) is explicitly out of scope for real-time Cerid.
It requires 4090-class hardware and eliminates the rasterized primary path entirely. Cerid's target is
the Lumen/RTXGI model: rasterized primary + RT secondary, fallback on all hardware.

---

## Consequences

- Phase 2.8 v1a adds `MaterialDomain` enum + `DOMN` chunk to MATR artifact. `kMaterialLoaderVersion` → 2.
- `ForwardRenderPath` skips non-`Surface` materials (no regression for current path).
- Phase 3.4 introduces `PostProcessStack` (owns PostProcess domain dispatch) and `PostProcessMaterial` GLSL template.
- Phase 7 node editor compiles to existing `ShaderResource` artifacts — no loader changes.
- Phase 5 RHI extensions: `AccelerationStructure`, `RayTracingPipeline` — opt-in, backward compatible.
- Phase 5 `HybridRenderPath` requires: MeshResource on GPU (Phase 2.7), scene TLAS (Phase 3.0), denoiser integration.
- Geometry shaders: never added as first-class engine feature (deprecated, slow on AMD, absent on Metal).
- Tessellation shaders: Phase 3.4 when terrain LOD has a real consumer.
- Mesh shaders: Phase 5 alongside RT.

---

## Alternatives considered

- **Per-system post-FX (no MaterialDomain):** Each post-FX effect hard-coded in the renderer, not material-driven. Rejected: non-composable, breaks the "authoring text → runtime binary" principle for custom post-FX.
- **Full path tracing as RT strategy:** Rejected; not real-time on typical hardware; breaks the fallback requirement.
- **Deferred render path as base for RT:** Rejected; ADR-0016 ships Clustered Forward+ as the baseline; Deferred is a later alternative `IRenderPath`, not the base.

---

## References

- ADR-0016 — Render path strategy (IRenderPath is the correct abstraction; Visibility-Buffer path reserved)
- ADR-0025 — Shader mechanism policy
- ADR-0030 — Shader / PSO boundary
- ADR-0032 — Frame graph v1
- ADR-0044 — Phase ordering decision
- `docs/phases/phase-2.8-material-completion.md` — MaterialDomain lands in v1a
- `docs/phases/phase-3.0-scene-ecs.md` — scene TLAS foundation for RT
