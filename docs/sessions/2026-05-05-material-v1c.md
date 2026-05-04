# 2026-05-05 — Phase 2.7 v1c: Full Material System Foundation (ADR-0048)

## What shipped

Phase 2.7 v1c implements the complete material system foundation described in ADR-0048: the two-tier `MaterialTemplate` + `MaterialInstance` split, typed parameter schema, pass-keyed shader pairs, domain and raster-state enums, a new MATR v2 artifact format, a full cooker rewrite, and backward-compat for legacy META artifacts.

## New files

| File | Purpose |
|------|---------|
| `engine/renderer/include/crd/renderer/material_domain.hpp` | `MaterialDomain` enum (Surface/PostProcess/Compute/Decal/UI) |
| `engine/renderer/include/crd/renderer/pass_type.hpp` | `PassType`, `AlphaMode`, `CullMode`, `FillMode`, `BlendMode`, `RasterState` |
| `engine/renderer/include/crd/renderer/material_template.hpp` | `ParameterType`, `CookedParameter`, `ShaderOptionDecl`, `PassShaderPair`, `MaterialTemplate`, `MaterialInstance` |
| `runtime/examples/smoke_material.cpp` | Headless smoke: v2 PASS chunk + MaterialInstance + legacy META backward-compat |

## Modified files

| File | Change |
|------|--------|
| `engine/resources/include/crd/resources/crdr.hpp` | Added 6 FourCCs: INFO, PRMS, DFLT, PASS, PSOS, OPTS |
| `engine/renderer/include/crd/renderer/material.hpp` | Renamed `MaterialLayout`→`MaterialBindLayout`, `MaterialInstance`→`MaterialBindGroup` |
| `engine/renderer/src/material.cpp` | Updated type names |
| `engine/renderer/include/crd/renderer/material_resource_loader.hpp` | Removed inline `MaterialResource` struct, includes `material_template.hpp` |
| `engine/renderer/src/material_resource_loader.cpp` | Full rewrite: reads INFO+PASS chunks, falls back to META, version=2 |
| `tools/asset_cooker/src/cook_handlers/material.cpp` | Rewrite: emits INFO+PASS, parses `[passes.forward]`/`[passes.depth_prepass]` sections, version=2 |
| `tests/renderer/test_renderer.cpp` | 4 existing material tests updated for renamed types |
| `tests/resources/test_shader_material_loaders.cpp` | 5 new v1c tests added |
| `runtime/examples/smoke_resources_render.cpp` | Updated to use `pass_shaders[Forward].vert/frag` |
| `runtime/CMakeLists.txt` | Added `smoke_material` target |

## Key design decisions

**PassShaderPair holds both vert + frag handles** — `ShaderResource` stores one stage. A complete pass needs both, so `PassShaderPair` bundles them. The PASS chunk entry is 36 bytes: pass_type (1) + pad (3) + vert_id (16) + frag_id (16).

**`pass_shaders[]` indexed by `PassType` ordinal** — `MaterialTemplate::pass_shaders[static_cast<u8>(PassType::Forward)]`. This avoids a search on the hot path.

**`variant_for_pass` falls back to Forward** — if a requested pass has no shader pair (null vert), the loader returns the Forward pair. This lets depth-prepass materials reuse the forward shaders until a dedicated depth shader is cooked.

**Legacy META backward-compat** — if PASS chunk absent but META chunk present (old v1 artifacts), the loader synthesizes a Forward entry. This keeps `smoke_resources_render` working without recooking.

**PRMS/DFLT/PSOS/OPTS reserved** — chunks defined in crdr.hpp and parsed by the loader (ignored gracefully if not present), but cooker does not emit them yet. Phase 2.8 wires PSOS; Phase 2.9 wires PRMS/DFLT/OPTS.

## MATR artifact v2 layout

```
CRDR header
  MATR type FourCC
    INFO chunk (4 bytes): loader_version u8, domain u8, flags u8, pad u8
    PASS chunk: count u32, then N × 36 bytes:
      pass_type u8, pad[3], vert_id u8[16], frag_id u8[16]
```

## Test results

5 new unit tests in `tests/resources/test_shader_material_loaders.cpp` (tagged `[v1c]`):
1. v2 PASS chunk round-trip — single Forward pass, verifies vert+frag handles
2. Two-pass material — Forward + DepthPrepass, verifies both indexed correctly
3. Missing PASS + META → Failed result
4. MaterialInstance set_float — binary-search write, correct value after read-back
5. MaterialInstance variant_for_pass fallback — pass with null shaders returns Forward pair

## Six-configuration quality pass

| Config | Build | CTest | Smokes |
|--------|-------|-------|--------|
| win-debug | ✅ | 457/457 | ✅ 17 headless |
| win-relwithdebinfo | ✅ | 457/457 | ✅ |
| win-release | ✅ | 454/454 | ✅ |
| win-asan | ✅ | 457/457 | ✅ |
| win-clang-cl | ✅ | 457/457 | ✅ |
| win-tidy | ✅ | — | — |

(win-release 3 fewer: debug-only FiberState tests behind `#if CRD_ENABLE_ASSERTS`)

## Tidy fix note

`write_pass_entry`, `read_resource_id`, and `load_shader_pair` were initially declared `static` inside anonymous namespaces — redundant per `readability-static-definition-in-anonymous-namespace`. Removed the `static` prefix from all three.

## Next

Phase 2.7 v1d: `GpuTextureUploader` + `GpuMeshUploader` — upload a `TextureResource` or `MeshResource` into GPU memory via a staging buffer, then issue a layout transition and ownership transfer. `smoke_asset_import.exe`: cook BoxTextured.glb + a texture, mount, load, upload to GPU, render one frame, exit 0. First real mesh+texture on screen.
