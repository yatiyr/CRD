# Phase 3.5 — Modern rendering pipeline prologue + PBR / IBL / lighting

**Status:** 📋 planned (ADR-0077 §4 — prologue addition; existing PBR scope from `docs/ROADMAP.md` outlook).
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md` (§4 modern-pipeline prologue), ADR-0046 (RT hybrid strategy, downstream)
**Slot:** after Phase 3.4 audio close. AAA-class rendering kicks off here.

## Why the prologue exists

The original Phase 3.5 scope (PBR / IBL / CSM / SSS / NPR / area lights) is the **shading content**. ADR-0077 §4 adds a v0 prologue covering the **modern GPU pipeline** that AAA engines (UE5 Nanite, Frostbite, idTech) use. Without the pipeline, the shading content runs in the legacy vertex-shader / hull / tessellation pipeline — which works but doesn't scale to Nanite-class geometry density.

Ship the pipeline first; build the shading on it.

## v0 — Modern pipeline prologue (NEW, ADR-0077 §4)

### v0a — Mesh shaders

**Scope:**
- `crd-rhi` extensions for mesh shader pipeline state (`VK_EXT_mesh_shader` / DX12 mesh shaders).
- Cooker emits **meshlet-packed** mesh format alongside the existing index-buffer mesh format: ~64 verts × 124 tris per meshlet (Vulkan recommendation), per-meshlet bounding cone + cluster cone for backface + frustum culling.
- Per-meshlet LOD selection.
- New `MeshletResource` ('MLET' FourCC) alongside existing `MeshResource`.
- `meshoptimizer` integration for meshlet packing (in cooker).
- Sample render path using mesh shaders for static geometry (replaces vertex-buffer-based draws).

**References:**
- Karis "Nanite — A Deep Dive" (SIGGRAPH 2021).
- NVIDIA "Mesh Shading: Towards Greater Efficiency of Geometry Processing" (2018).
- AMD "Triangle Visibility Buffer 2.0" (2021).
- `meshoptimizer` library (Zeux).

### v0b — Visibility buffer rendering

**Scope:**
- New `IRenderPath` implementation `VisibilityBufferPath`.
- Visibility buffer: 32-bit per pixel (16 mesh-id + 16 triangle-id) at scene resolution.
- Material shading pass: deferred, indexes into bindless material descriptors via the visibility-buffer fetch.
- Compatible with mesh shader path (v0a) — meshlet-id encoded in visibility.
- Trade-off: massive material variety with constant per-pixel cost, but loses some forward-renderer features (transparency, custom forward post).

**References:**
- Burns & Hunt "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading" (Journal of Computer Graphics Techniques, 2013).
- Munkberg, Hasselgren & Akenine-Möller "Visibility buffer 2.0" (HPG 2022).

### v0c — GPU-driven culling

**Scope:**
- Compute-shader frustum culling — per-instance / per-meshlet.
- Hi-Z occlusion culling — depth pyramid (last-frame reproject) + per-AABB test.
- Output: indirect draw arguments via `VkDrawIndirectCount` (or DX12 equivalent).
- Cluster-cone backface culling at meshlet level.
- `crd-renderer` extension: `IndirectDrawList` analogous to current `DrawList`.

**References:**
- "GPU-Driven Rendering Pipelines" Sebastian Aaltonen / Ubisoft Massive (SIGGRAPH 2015).
- Karis 2021 (above, Nanite culling).

### v0d — Work graphs (where supported)

**Scope:**
- DX12 Ultimate `WorkGraph` (cross-vendor since 2024).
- Vulkan: `VK_EXT_work_graphs` (proposal as of writing; ship when ratified).
- Replaces dependency-graph dispatching (compute → indirect → compute) with self-feeding compute pipelines.
- Reduces command-buffer overhead for complex dispatch chains.

This is optional — ships if/when extension is stable.

### v0e — Variable rate shading (VRS)

**Scope:**
- Per-tile shading rate via screen-space VRS image.
- Per-primitive VRS via mesh shader output.
- Heuristics: lower shading rate in motion-blurred / out-of-focus regions, foveated rendering with eye-tracker hint.

## v1+ — PBR + IBL + lighting (existing ROADMAP outlook)

The existing Phase 3.5 scope from `docs/ROADMAP.md`:

- HDR pipeline (R16G16B16A16F) + ACES / AgX tone map + auto-exposure.
- Cook-Torrance GGX PBR.
- Image-Based Lighting: HDRI → prefiltered env map + irradiance SH + BRDF LUT.
- Punctual lights: point / spot / directional with physical attenuation.
- Cascaded Shadow Maps + PCSS soft shadows + contact shadows.
- Area lights (LTC approximation, Heitz 2016).
- Emissive meshes.
- Extended shading models: clear coat, anisotropic, cloth, iridescence, transmission.
- Subsurface scattering (pre-integrated + Jimenez separable).
- Toon / NPR.

The mesh-shader / visibility-buffer / GPU-driven prologue means these shading models run in a more scalable pipeline.

## Dependencies

- `crd-rhi` (extensions for mesh shaders + bindless descriptor heaps + work graphs).
- `crd-renderer` (pipeline integration + new IRenderPath).
- `crd-resources` (`MeshletResource` cooker integration).
- `crd-geometry-mesh-processing` (`meshoptimizer` integration for meshlet packing).

## Reference reading

(See per-slice references above; comprehensive list:)
- Akenine-Möller, Haines, Hoffman, Pesce, Iwanicki & Hillaire "Real-Time Rendering" 4th ed. (2018, with online appendix updates).
- Lengyel "Foundations of Game Engine Development" series — especially Vol 2 (Rendering, 2019).
- Karis "Real Shading in Unreal Engine 4" (SIGGRAPH 2013).
- Heitz "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines" (SIGGRAPH 2016).
- Jimenez et al. "Separable Subsurface Scattering" (Eurographics 2015).
- Lagarde, de Rousiers "Moving Frostbite to PBR" (SIGGRAPH 2014).

## Out of scope

- Ray tracing (Phase 5).
- Volumetric atmosphere / clouds (Phase 3.6).
- Post-processing (Phase 3.7).
- GPU particles / ocean (Phase 3.8).
- GI (Phase 3.9).

## Revisit triggers

This stub becomes a full phase plan when:
- Phase 3.4 audio close.
- A research dossier ships (`docs/research/cerid-rendering-modern.md`).
- Phase 3.1.7 geometry close (`crd-geometry-mesh-processing` provides meshlet packing input).
