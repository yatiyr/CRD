# ADR-0048 — Material System Architecture Foundation

**Status:** Accepted
**Date:** 2026-05-05
**Tags:** arch, renderer, shader, materials, resources, cooker

---

## Context

After Phase 2.7 v1a (TextureResource) and v1b (MeshResource), the existing `MaterialResource` is a
proof-of-concept loader holding two shader handles (`vertex_shader`, `fragment_shader`) and nothing else.
It has no concept of material parameters, texture slots, render passes, shader variants, or domain.
Phase 2.7 v1c was originally scoped as a simple `HashMap<String, Vec4f>` extension (closing debt item 1).

The scope is expanded to a full material system foundation for three reasons:

1. The MATR artifact format established here is the stability boundary for Phase 7 node editor integration
   (ADR-0046 §2). Piecemeal format changes across two or three phases accumulate forward-compatibility debt.
2. ShaderOptions (compile-time shader variants keyed by material parameter values) require the parameter
   schema and option declarations to be co-designed — they cannot be added incrementally.
3. The two-tier Template/Instance split cannot be retrofitted after the scene system (Phase 3.0) starts
   consuming material handles — the scene system needs `MaterialInstance` references, not raw `MaterialTemplate`
   handles.

Three candidate architectures were evaluated against Cerid's substrate goals (node editor, multi-domain,
backend-agnostic):

- **UE5 Material Compiler:** HLSL output + Blueprint graph. Requires C++ function compilation infrastructure;
  not appropriate for a substrate engine targeting DAWs and robotics simulators alongside games.
- **O3DE ShaderInput/ShaderOption/Functor trichotomy:** Explicit split between UBO values, compile-time
  variant keys, and declarative relationships. Open source, Vulkan-first, closest to Cerid's design philosophy.
- **Godot resource-based materials:** All parameters stored as `Variant`; pipeline permutations implicit.
  Too opinionated about scene integration; hides the UBO layout from the cooker.

**O3DE is chosen as the primary reference.** Three Cerid-specific improvements are added:

1. **Inline functor declarations** in `params.toml` — `enables_option = "USE_NORMAL_MAP"` on a texture
   parameter eliminates the C++ `FunctorType` subclass for the common UseTexture case.
2. **Richer `ParameterType` enum with semantic annotations** — `Color` = `Vec4f` with sRGB annotation;
   editor shows a color picker; GPU type narrowing at bind time avoids CPU-side sRGB math.
3. **`MaterialDomain` in the artifact from day one** — ADR-0046 anticipated Phase 2.8; pulled forward into
   the `INFO` chunk in Phase 2.7 v1c to avoid a version bump.

---

## Decisions

### 1. Two-tier split: `MaterialTemplate` + `MaterialInstance`

The single `MaterialResource` is replaced by two types:

```cpp
// MaterialTemplate: immutable, loaded from MATR artifact, shared across instances.
// Registered in ResourceManager under FourCC 'MATR'.
struct MaterialTemplate
{
    MaterialDomain                                                          domain;
    crd::containers::Array<CookedParameter>                                parameters;    // schema, sorted by name_hash
    crd::containers::Array<crd::u8>                                        defaults_blob; // packed default values
    crd::containers::HashMap<PassType, ResourceHandle<ShaderResource>>     pass_shaders;
    crd::containers::Array<RasterState>                                    pso_states;    // indexed by PassType ordinal
    crd::containers::Array<ShaderOptionDecl>                               options;

    explicit MaterialTemplate(crd::IAllocator* a);
};

// MaterialInstance: mutable, holds per-object parameter overrides.
// Created from a MaterialTemplate. NOT a ResourceManager resource — caller-owned.
struct MaterialInstance
{
    ResourceHandle<MaterialTemplate>                                        tmpl;
    crd::containers::Array<crd::u8>                                        values_blob;       // override values (parallel to tmpl->parameters)
    crd::containers::HashMap<crd::u32, ResourceHandle<TextureResource>>    texture_overrides; // key = binding_slot

    explicit MaterialInstance(crd::IAllocator* a, ResourceHandle<MaterialTemplate> t);

    // Resolve the shader variant for a given pass, applying ShaderOptions derived from
    // current values_blob (inline functor evaluation).
    [[nodiscard]] ResourceHandle<ShaderResource> variant_for_pass(PassType pass) const;

    void set_float  (crd::containers::StringView name, float v);
    void set_vec4   (crd::containers::StringView name, crd::math::Vec4f v);
    void set_texture(crd::containers::StringView name, ResourceHandle<TextureResource> h);
};
```

`MaterialInstance` is **not** registered in the `ResourceManager`. It is owned by the caller (scene node,
renderable, or direct user code). Scene nodes hold `MaterialInstance` by value or unique_ptr.

### 2. Naming collision fix — pre-existing internal types renamed

Two existing internal types in `crd-renderer` clash with the new user-facing names:

| Old name | New name | Location |
|---|---|---|
| `MaterialLayout` | `MaterialBindLayout` | `engine/renderer/include/crd/renderer/material.hpp` |
| `MaterialInstance` (per-frame transient bind group) | `MaterialBindGroup` | `engine/renderer/include/crd/renderer/material.hpp` |

These are internal to `ForwardRenderPath`. The rename is mechanical with no semantic change.
`MaterialResource` is renamed to `MaterialTemplate` — same MATR FourCC, same loader registration point.

### 3. Surface function contract — `SurfaceData`

All surface shaders (hand-written or node-editor generated) implement a single entry point:

```glsl
// crd/renderer/surface_data.glsl.inc — versioned, only grows, never removes fields

struct VertexAttrs
{
    vec3 position;   // world-space
    vec3 normal;     // world-space, unit length
    vec2 uv0;
    vec4 tangent;    // xyz = tangent, w = bitangent sign
};

struct SurfaceData
{
    vec3  base_color;  // linear sRGB
    float metallic;    // 0 = dielectric, 1 = metal
    float roughness;   // perceptual roughness
    float ao;          // ambient occlusion [0,1]
    vec3  emissive;    // linear sRGB
    vec3  normal;      // world-space (or view-space — declared per pass)
    float opacity;     // for masked/transparent domains
};

// Artist / node-editor writes ONLY this function.
// Engine owns the per-pass wrapper that calls it.
void crd_evaluate_surface(in VertexAttrs attrs, inout SurfaceData surface);
```

**Stability contract:** `SurfaceData` fields are never removed. New fields may be appended with
`default = 0`. The function signature `crd_evaluate_surface(in VertexAttrs, inout SurfaceData)` is
the immovable API boundary between artist-authored GLSL and engine pass wrappers.

### 4. New MATR artifact chunk format

The old 32-byte `META` chunk (hardcoded vert+frag UUID pair) is retired. New format:

| Chunk FourCC | Size      | Contents |
|---|---|---|
| `INFO`       | 4 bytes   | `loader_version u8, domain u8, flags u8, pad u8` |
| `PRMS`       | variable  | Schema: `count u32`, then per-entry `name_hash u64, type u8, ubo_offset u16, binding_slot u8, enables_option_hash u64 (0 = none)` — sorted by name_hash |
| `DFLT`       | variable  | Default values blob — tightly packed in `ubo_offset` order |
| `PASS`       | variable  | `count u32`, then `pass_type u8, pad u8[3], resource_id u8[16]` per entry |
| `PSOS`       | variable  | `present_mask u8` (bit N = has entry for PassType N), then `RasterState` per present pass |
| `OPTS`       | variable  | `count u32`, then `name_hash u64, default_enabled u8, pad u8[7]` per option |

**Reader policy:** Unknown chunk FourCCs are skipped (forward-compatible reader). Chunks may appear in
any order. Missing `INFO` → treat as `loader_version=1, domain=Surface`. Missing `PRMS`/`DFLT` → empty
parameter list. Missing `PASS` → fall back to legacy `META` chunk (backward compat). Missing `PSOS` →
default `RasterState{}` for all passes. Missing `OPTS` → no shader options.

**Legacy `META` chunk (32 bytes):** Recognized by the loader for backward compatibility. Synthesized
into a `PassType::Forward` entry in `pass_shaders`.

### 5. `ParameterType` enum with semantic annotations

```cpp
enum class ParameterType : crd::u8
{
    Float       = 0,    // 4 bytes (f32)
    Float2      = 1,    // 8 bytes
    Float3      = 2,    // 12 bytes
    Float4      = 3,    // 16 bytes (Vec4f)
    Color       = 4,    // 16 bytes (Vec4f) — sRGB annotation; editor shows color picker
    Bool        = 5,    // 4 bytes (u32 0/1 — std140 alignment)
    Int         = 6,    // 4 bytes (i32)
    Enum        = 7,    // 4 bytes (i32 index into declared string values)
    Texture2D   = 8,    // 0 UBO bytes — binding_slot carries descriptor binding index
    TextureCube = 9,    // 0 UBO bytes
    Sampler     = 10,   // 0 UBO bytes
};
```

Types 0–7 are packed into `values_blob` on `MaterialInstance`. Types 8–10 use `texture_overrides`
keyed by `binding_slot`. GPU receives a `Color` as a raw `vec4` (sRGB annotation is editor-only).

### 6. Cook-time SPIR-V reflection → `CookedParameter` offset table

At cook time, the `.mat.toml` handler:
1. Compiles each declared pass shader to SPIR-V via shaderc.
2. Runs spirv-reflect to extract the UBO layout for set 0, binding 0 (the material UBO).
3. For each `[[parameter]]` entry in `params.toml`, locates its UBO member by name.
4. Emits a `CookedParameter`: `name_hash` (FNV-1a u64), `type`, `ubo_offset` (from spirv-reflect),
   `binding_slot`, `enables_option_hash` (FNV-1a of the enables_option string, 0 if absent).
5. Sorts all entries by `name_hash` — enables O(log N) binary search in `set_*()` at bind time.

The `values_blob` size is `max(ubo_offset + sizeof(type))` across all non-texture parameters,
padded to 16 bytes. Callers write into it via `set_float` / `set_vec4`; the whole blob is uploaded
as a push constant or UBO in `ForwardRenderPath`.

### 7. `ShaderOption` system and inline functor

ShaderOptions are compile-time variant keys (integrating with the ADR-0026 `VariantKey` machinery):

```toml
# params.toml — sample with inline functor
[[parameter]]
name        = "normal_map"
type        = "Texture2D"
binding_slot = 1
enables_option = "USE_NORMAL_MAP"   # inline functor — no C++ FunctorType subclass needed

[[option]]
name    = "USE_NORMAL_MAP"
default = false
```

At `MaterialInstance::variant_for_pass(pass)`:
1. Walk `tmpl->parameters` where `enables_option_hash != 0`.
2. For each: check if `texture_overrides` contains the corresponding `binding_slot`.
3. Set or clear the matching bit in the `VariantKey` constructed for this call.
4. Return the `ShaderResource` from `tmpl->pass_shaders[pass]`, annotated with the resolved `VariantKey`.

The `ShaderResource` already holds all compiled SPIR-V permutations keyed by `VariantKey` (ADR-0026).
`MaterialInstance::variant_for_pass` drives which permutation is selected; it does not recompile shaders.

### 8. Node editor 5-file contract

The node editor (Phase 7) outputs exactly five files per material. The runtime sees none of them:

| File | Contents |
|---|---|
| `surface.glsl` | The `crd_evaluate_surface()` implementation |
| `params.toml` | `[[parameter]]` and `[[option]]` entries |
| `pso.toml` | `RasterState` per pass (alpha_mode, cull_face, depth_test, etc.) |
| `passes.toml` | `[passes.depth_prepass]`, `[passes.forward]` → GLSL source paths |
| `options.toml` | ShaderOption declarations with default values |

The cooker ingests these five files and produces a MATR CRDR artifact via the same pipeline as
hand-authored `.mat.toml` materials. This fulfils the zero-runtime-change requirement (ADR-0046 §2).

### 9. `PassType` enum — values frozen in Phase 2.7

```cpp
enum class PassType : crd::u8
{
    DepthPrepass = 0,
    Shadow       = 1,   // reserved — no shadow pass until Phase 3.5 (CSM + PCSS)
    Forward      = 2,   // main shaded color pass (was "MainColor")
    // future: Overlay, Decal — append only
};
```

The reserved `Shadow` slot prevents on-disk value shifts when shadow maps land in Phase 3.5.
Values 0–2 are frozen; new values may only be appended.

---

## Consequences

### Phase 2.7 v1c (immediate)

- `MaterialResource` → `MaterialTemplate`. All `load_sync<MaterialResource>` call sites updated.
- `MaterialLayout` → `MaterialBindLayout`; per-frame `MaterialInstance` → `MaterialBindGroup`.
- New public types: `MaterialTemplate`, `MaterialInstance`, `ParameterType`, `CookedParameter`,
  `ShaderOptionDecl`, `MaterialDomain`, `PassType`, `RasterState`.
- New GLSL contract: `crd/renderer/surface_data.glsl.inc`.
- FourCCs added to `crdr.hpp`: `kFourCC_INFO`, `kFourCC_PRMS`, `kFourCC_DFLT`, `kFourCC_PASS`,
  `kFourCC_PSOS`, `kFourCC_OPTS`.
- `kMaterialLoaderVersion` → 2. Artifacts with version 1 (`META`-only) load via backward-compat path.
- Existing test `load_sync<MaterialResource>` → `load_sync<MaterialTemplate>`.
- Cooker `.mat.toml` handler rewritten to emit the new chunk set; no longer emits `META` chunk.

### Phase 2.8 (refined scope — GPU-side wiring only, no format changes)

- v1a: Wire `PSOS` data to Vulkan pipeline compilation. Per-material pipeline cache keyed by
  `(VariantKey, RasterState)`. `ForwardRenderPath` skips non-`Surface` domain materials.
- v1b: `ForwardRenderPath` multi-pass: `pass_shaders[DepthPrepass]` in depth prepass,
  `pass_shaders[Forward]` in color pass.
- v1c: Depth-only prepass pipeline (vertex-only, null fragment shader).

### Long-term

- Phase 7 node editor: zero runtime changes. Five-file contract → same MATR artifact format.
- Phase 5 RT: `HybridRenderPath` queries `pass_shaders[Shadow]` (reserved slot) without format bump.
- Phase 3.7: `PostProcessStack` dispatches `PostProcess` domain materials via the existing `domain` field.
- Items 4–5 from `docs/debt.md` remain deferred to their respective consumer phases (item 4 → Phase 3.5
  CSM; item 5 → Phase 3.7 post-FX or Phase 3.8 GPU-driven).

---

## Alternatives considered

- **Simple `HashMap<String, Vec4f>` extension (original v1c plan):** Closes debt item 1 but forces
  Phase 2.8 to add PASS/RAST/DOMN chunks to an artifact that already has PARM/TEXS. The cumulative
  number of `kMaterialLoaderVersion` bumps across two phases equals the cost of designing it right once.
- **Per-instance UBO allocated by `ResourceManager`:** Treating `MaterialInstance` as a resource.
  Rejected — instances are per-object, high-churn, and tightly coupled to scene lifetimes.
  ResourceManager overhead (handle table, ref-counting, loader registration) is inappropriate for
  transient per-draw data.
- **SPIR-V specialization constants instead of shader permutations:** Constants avoid recompilation
  but do not eliminate dead code at the binary level; spirv-reflect shows full code paths in profiling.
  The `VariantKey` permutation system (ADR-0026) provides dead-code elimination and aligns with the
  existing `ShaderResource` model.

---

## References

- ADR-0026 — Shader variant key (VariantKey + ShaderResource permutation model)
- ADR-0027 — Shader reflection consumption model (spirv-reflect usage)
- ADR-0030 — Shader / PSO boundary
- ADR-0038 — CRDR cooked binary container format (chunk reader, FourCC registry)
- ADR-0044 — Phase ordering: material PSO/variant before scene/ECS
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy
- `docs/phases/phase-2.7-asset-import.md` — v1c implementation plan
- `docs/phases/phase-2.8-material-completion.md` — GPU-side wiring (refined scope after this ADR)
- `docs/debt.md` — Material system v1 known gaps (items 1–3 subsumed by Phase 2.7 v1c artifact format)
