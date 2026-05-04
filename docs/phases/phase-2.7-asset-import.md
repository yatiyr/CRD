# Phase 2.7 — Asset import bootstrap

**Status:** 🚧 active — v1a shipped 2026-05-04, v1b shipped 2026-05-05
**ADRs:** ADR-0042 (texture cooked format), ADR-0043 (mesh resource + glTF import scope), ADR-0048 (material system architecture foundation)
**New modules:** loaders in `crd-renderer`; cooker handlers in `tools/asset_cooker/`
**Depends on:** Phase 2.6 complete (ResourceManager, ILoader registry, CRDR cooker pipeline)

---

## Goal

Put a real glTF mesh with a real texture on screen, loaded entirely through the resource system,
with no source files touched at runtime. No scene graph yet — explicit renderables only, same
pattern as `smoke_renderer`.

This slice pays off three things simultaneously:
1. Validates the resource pipeline under real-world asset load pressure.
2. Creates `TextureResource`, which unblocks all five material system debt items (see `docs/debt.md`).
3. Creates `MeshResource`, which gives Phase 3.0 scene/ECS real geometry to place rather than
   hardcoded triangles.

**Out of scope for Phase 2.7:**
- Skybox / IBL / environment maps — needs HDR pipeline + tonemap pass (Phase 3.4).
- Skeletal animation or morph targets in glTF — **firmly deferred to Phase 3.2.** Joint hierarchies,
  skin weights, and blend shapes stored without a scene transform hierarchy or animation sampler are
  bytes-on-disk with no consumer (violates vertical-slice principle). The binary format would also be
  designed without Phase 3.2's input. Re-cook from source when Phase 3.2 lands; the cooker is idempotent
  by design. Decision recorded in ADR-0044.
- BC7 GPU texture compression (gated behind `CRD_COOK_BC7`; optional in v1a, not required).
- Physics collision shapes derived from mesh data (Phase 3.1).
- GPU-driven rendering or instanced mesh draws (Phase 3.2 dep).
- Scene graph placement of loaded meshes (Phase 3.0).

---

## Architecture

### Where the new types live

`TextureResource` and `MeshResource` live in `crd-renderer`. They are plain data containers
(CPU-side only) — no GPU objects. GPU upload is a separate step (v1d) that lives in the render
path, which already has access to the `Device`. This keeps the loaders free of any RHI dependency
and lets them be tested without a Vulkan instance.

```
crd-resources (ILoader registry)
       ↑
crd-renderer (TextureResourceLoader, MeshResourceLoader, MaterialResourceLoader)
       ↑
crd-rhi (GPU upload helpers, owned by ForwardRenderPath)
```

The `LoadContext` does NOT carry a `Device*` in v1 (see ADR-0042 note). If synchronous GPU
upload at load time is ever needed, that is a deliberate extension requiring its own ADR.

### `TextureFormat` enum

```cpp
// engine/renderer/include/crd/renderer/texture_resource.hpp
namespace crd::renderer
{
enum class TextureFormat : crd::u8
{
    RGBA8Unorm,      // 4 bytes/pixel; always available
    BC7Unorm,        // 1 byte/pixel; cook-time opt-in (CRD_COOK_BC7)
    BC7UnormSrgb,    // same, sRGB transfer
};
} // namespace crd::renderer
```

### `TextureResource`

```cpp
struct MipLevel
{
    crd::u32                         width;
    crd::u32                         height;
    crd::containers::Array<crd::u8>  pixels;  // tightly packed, format-appropriate
};

struct TextureResource
{
    TextureFormat                        format;
    crd::u32                             mip_count;
    crd::containers::Array<MipLevel>     mips;  // mips[0] = full-res

    explicit TextureResource(crd::IAllocator* a) : mips(a) {}
};
```

### `MeshResource`

```cpp
// Interleaved vertex layout (48 bytes/vertex):
//   float3 position    (bytes  0–11)
//   float3 normal      (bytes 12–23)
//   float2 uv0         (bytes 24–31)
//   float4 tangent     (bytes 32–47, w = bitangent sign)

struct MeshPrimitive
{
    crd::u32    vertex_count;
    crd::u32    index_count;
    crd::u32    vertex_byte_offset;  // into MeshResource::vertices
    crd::u32    index_byte_offset;   // into MeshResource::indices
    resources::ResourceId  material_id;  // zero if no material assigned
};

struct MeshResource
{
    crd::containers::Array<crd::u8>          vertices;    // interleaved, 48 bytes/vert
    crd::containers::Array<crd::u8>          indices;     // u32 indices
    crd::containers::Array<MeshPrimitive>    primitives;

    explicit MeshResource(crd::IAllocator* a)
        : vertices(a), indices(a), primitives(a) {}
};
```

Vertex layout rationale: see ADR-0043.

### CRDR FourCCs added in 2.7

| FourCC | Meaning |
|--------|---------|
| `TXTR`  | Texture artifact type |
| `HEAD`  | Texture header chunk (width, height, mip_count, format) |
| `MIPn`  | Mip level chunk (n = 0–15); payload = raw pixel data |
| `MESH`  | Mesh artifact type |
| `VERT`  | Vertex buffer chunk |
| `INDX`  | Index buffer chunk |
| `PRIM`  | Primitive table chunk |
| `INFO`  | Material template info chunk: loader_version, domain (v1c) |
| `PRMS`  | Material parameter schema chunk (v1c) |
| `DFLT`  | Material parameter defaults blob (v1c) |
| `PASS`  | Material pass-keyed shader table (v1c) |
| `PSOS`  | Material PSO state per pass (v1c) |
| `OPTS`  | Material shader option declarations (v1c) |

---

## Slices

### v1a — `TextureResource` + texture cooker

**Scope:**
- `TextureFormat` enum + `TextureResource` struct in `engine/renderer/`.
- `TextureResourceLoader`: reads `type='TXTR'` artifact; parses `HEAD` chunk (16 bytes:
  width u32, height u32, mip_count u32, format u8, padding u8[3]); reads one `MIPn` chunk
  per mip level into `TextureResource::mips`. Registered via `register_texture_loader(rm)`.
- Texture cooker handler (`.png`, `.jpg`, `.tga`, `.bmp` → `type='TXTR'`):
  - stb_image for decode (already a CPM dep).
  - Box-filter mip generation down to 1×1.
  - Optional BC7 compression gated by `CRD_COOK_BC7` CMake option (Windows + DirectXTex only;
    graceful skip if not available).
  - Writes `HEAD` chunk + one `MIPn` chunk per level.
- `kFourCC_TXTR`, `kFourCC_HEAD`, `kFourCC_MIP0`–`kFourCC_MIP15` added to `crdr.hpp`.

**Tests (shipped):**
- `TextureResource loads from CRDR artifact` — round-trip: hand-assembled TXTR artifact → load → verify mip_count=3, mips[0].width=4, format=RGBA8Unorm.
- `TextureResource mip chain has correct dimensions` — mip dims 4×4, 2×2, 1×1.
- `TextureResource fails when HEAD chunk is absent` — missing HEAD → `LoadState::Failed`.
- `TextureResource cooked from TGA round-trip` — cook a 4×4 TGA via the texture handler, mount, load, verify R=255.
- `smoke_texture.exe` (headless): builds a TXTR artifact directly, mounts, loads, verifies all mip dims and pixel values.

### v1b — `MeshResource` + glTF import

**Scope:**
- `MeshPrimitive` + `MeshResource` structs in `engine/renderer/`.
- `MeshResourceLoader`: reads `type='MESH'` artifact; parses `VERT`, `INDX`, `PRIM` chunks.
  Registered via `register_mesh_loader(rm)`.
- glTF cooker handler (`.glb` + `.gltf` → one `type='MESH'` artifact per glTF mesh node):
  - cgltf for parsing (already declared as a cooker dep in Phase 2.6; now actually wired).
  - Static meshes only — no skinning, no morph targets, no animations.
  - Reads `POSITION`, `NORMAL`, `TEXCOORD_0`, `TANGENT` accessors; generates tangents via
    MikkTSpace if `TANGENT` is absent.
  - Index buffer: always u32 (upcast u16 indices).
  - Material UUID: if the glTF primitive references a material, look up its UUID from the
    adjacent `.meta` sidecar for the `.mat.toml` file; zero UUID if no material.
  - One `.meta` per glTF file (the mesh's own UUID); one artifact per scene mesh node
    (cooker emits multiple artifacts from one source file, one per mesh).
- `kFourCC_MESH`, `kFourCC_VERT`, `kFourCC_INDX`, `kFourCC_PRIM` added to `crdr.hpp`.

**Tests:**
- MESH round-trip: cook a minimal hand-assembled glTF (triangle, 3 verts) → load → vertex bytes match.
- Primitive count correct for a multi-primitive glTF mesh.
- Missing VERT chunk → `LoadState::Failed`.

### v1c — Material system foundation (closes debt items 1–3 artifact layer; ADR-0048)

This slice replaces the original "simple parameter wiring" scope with a full material system foundation.
See ADR-0048 for the full design rationale and all decisions.

**Scope — renaming and cleanup (no semantic change):**
- `MaterialLayout` → `MaterialBindLayout` (internal to `ForwardRenderPath`, not in public API).
- `MaterialInstance` (per-frame transient bind group) → `MaterialBindGroup` (same scope).
- `MaterialResource` → `MaterialTemplate` (same FourCC `MATR`, same loader registration point;
  all `load_sync<MaterialResource>` call sites updated to `load_sync<MaterialTemplate>`).

**Scope — new public types:**
- `MaterialDomain` enum: `Surface(0)`, `PostProcess(1)`, `Compute(2)`, `Decal(3)`, `UI(4)` (ADR-0046,
  pulled forward from Phase 2.8 into the `INFO` chunk).
- `PassType` enum: `DepthPrepass(0)`, `Shadow(1)` (reserved), `Forward(2)` — values frozen.
- `ParameterType` enum: `Float`, `Float2`, `Float3`, `Float4`, `Color`, `Bool`, `Int`, `Enum`,
  `Texture2D`, `TextureCube`, `Sampler` — semantic annotations on Color; Texture types use binding_slot.
- `CookedParameter` struct: `name_hash u64, type u8, ubo_offset u16, binding_slot u8, enables_option_hash u64`.
- `ShaderOptionDecl` struct: `name_hash u64, default_enabled u8`.
- `RasterState` struct: `AlphaMode, CullMode, FillMode, depth_test bool, depth_write bool, src/dst BlendMode`.

**Scope — `MaterialTemplate` (loaded from MATR artifact):**

```cpp
struct MaterialTemplate
{
    MaterialDomain                                                       domain;
    crd::containers::Array<CookedParameter>                             parameters;    // schema sorted by name_hash
    crd::containers::Array<crd::u8>                                     defaults_blob; // packed default values
    crd::containers::HashMap<PassType, ResourceHandle<ShaderResource>>  pass_shaders;
    crd::containers::Array<RasterState>                                 pso_states;    // indexed by PassType ordinal
    crd::containers::Array<ShaderOptionDecl>                            options;

    explicit MaterialTemplate(crd::IAllocator* a);
};
```

**Scope — `MaterialInstance` (caller-owned, not a ResourceManager resource):**

```cpp
struct MaterialInstance
{
    ResourceHandle<MaterialTemplate>                                      tmpl;
    crd::containers::Array<crd::u8>                                       values_blob;
    crd::containers::HashMap<crd::u32, ResourceHandle<TextureResource>>   texture_overrides;

    explicit MaterialInstance(crd::IAllocator* a, ResourceHandle<MaterialTemplate> t);

    [[nodiscard]] ResourceHandle<ShaderResource> variant_for_pass(PassType pass) const;
    void set_float  (crd::containers::StringView name, float v);
    void set_vec4   (crd::containers::StringView name, crd::math::Vec4f v);
    void set_texture(crd::containers::StringView name, ResourceHandle<TextureResource> h);
};
```

**Scope — `SurfaceData` GLSL contract:**
- New file `crd/renderer/surface_data.glsl.inc` containing `VertexAttrs`, `SurfaceData` struct, and
  declaration of `crd_evaluate_surface(in VertexAttrs, inout SurfaceData)`.
- Stability contract: fields never removed; new fields appended with default=0.

**Scope — New MATR artifact format (kFourCC_INFO/PRMS/DFLT/PASS/PSOS/OPTS):**
- `INFO` chunk (4 bytes): `loader_version u8, domain u8, flags u8, pad u8`.
- `PRMS` chunk: parameter schema array sorted by `name_hash` for binary search at bind time.
- `DFLT` chunk: packed default values blob parallel to `PRMS` entries.
- `PASS` chunk: `count u32` + `{pass_type u8, pad[3], resource_id[16]}` per entry.
- `PSOS` chunk: `present_mask u8` + `RasterState` per present PassType.
- `OPTS` chunk: shader option declarations.
- Reader: unknown FourCCs skipped; missing chunks load gracefully with defaults.
- Legacy `META` chunk (32-byte vert+frag pair from v1e) synthesized into `PassType::Forward` entry.
- `kMaterialLoaderVersion` → 2.

**Scope — Cooker `.mat.toml` handler rewrite:**
- Parses `[[parameter]]` entries with `name`, `type`, `binding_slot`, `enables_option` (inline functor).
- Parses `[[option]]` entries with `name` and `default`.
- Compiles pass shaders to SPIR-V via shaderc; runs spirv-reflect to extract UBO offsets for each parameter.
- Emits `CookedParameter` entries sorted by `name_hash`; emits all six new chunks.
- No longer emits legacy `META` chunk.
- Parses optional `[raster]` table per pass for `PSOS` chunk.

**FourCCs added:** `kFourCC_INFO`, `kFourCC_PRMS`, `kFourCC_DFLT`, `kFourCC_PASS`, `kFourCC_PSOS`,
`kFourCC_OPTS`.

**Tests (in `tests/resources/test_material_loader.cpp`):**
- `MaterialTemplate` round-trip: hand-assembled MATR CRDR (INFO+PRMS+DFLT+PASS+PSOS) → load → verify
  domain, parameter schema, pass_shaders entries, pso_states.
- Legacy `META`-only artifact loads → synthesized `PassType::Forward` entry, empty parameter list.
- `MaterialInstance` parameter override: set_vec4 → `values_blob` updated correctly.
- `MaterialInstance` texture override: set_texture → `texture_overrides` populated, `variant_for_pass`
  returns permuted variant (inline functor evaluation).
- Cook `.mat.toml` with `[[parameter]]` and `[[option]]` → load → schema matches declared types.
- Missing `INFO` chunk → domain defaults to `Surface`, load succeeds.

**Smoke:** `smoke_material.exe` (headless): builds MATR artifact with two pass shaders and one
`Float4` parameter, mounts, loads as `MaterialTemplate`, creates `MaterialInstance`, calls
`set_vec4("base_color", ...)`, verifies `values_blob` updated, calls `variant_for_pass(Forward)`,
verifies handle is valid. Deletes temp file, exits 0.

### v1d — GPU upload + real render smoke

**Scope:**
- `GpuTextureUploader` (or static helpers on `ForwardRenderPath`): given a `TextureResource` and
  a `Device*`, allocates a `crd::rhi::Image`, stages pixels via a CPU-visible staging buffer,
  submits a one-shot command buffer to copy to device-local memory, waits for fence, returns the
  Image handle. Mip upload loop covers all mip levels.
- `GpuMeshUploader`: given a `MeshResource` and a `Device*`, allocates vertex + index `Buffer`s,
  uploads via staging, returns handles.
- `smoke_asset_import.exe`: uses the full pipeline end-to-end:
  1. Cook a bundled glTF mesh + texture + material TOML (in-process, using cooker handlers
     directly, or from pre-cooked test assets in `tests/assets/`).
  2. Mount the resulting PACK.
  3. `load_sync<MeshResource>` + `load_sync<MaterialResource>` (which transitively loads
     `TextureResource` and both `ShaderResource`s).
  4. GPU-upload mesh + texture.
  5. Build a `Renderable`, push to `DrawList`, run `ForwardRenderPath` for one frame.
  6. Assert no Vulkan validation errors, exit 0.

**Note:** `smoke_asset_import` requires a GPU; it is skipped on headless CI. A flag or
environment variable guards the smoke skip path.

### v1e — `crd-meshgen` + sandbox bootstrap

**Scope:**
- New module `engine/meshgen/` — CPU-side procedural geometry generator. No deps on `crd-rhi`,
  `crd-renderer`, or `crd-resources`. Depends only on `crd-math`, `crd-containers`, `crd-memory`.
- Output: `crd::meshgen::MeshData` — interleaved 48B/vertex (pos+normal+uv0+tangent), same layout
  as `MeshResource`. Compatible with `GpuMeshUploader` directly.
- Shapes: `make_sphere(slices, stacks)`, `make_icosphere(subdivisions)`, `make_box(half_extents)`,
  `make_capsule(radius, half_height, slices, stacks)`, `make_cylinder(radius, half_height, slices)`,
  `make_cone(radius, height, slices)`, `make_plane(subdiv_x, subdiv_y, half_extents)`,
  `make_torus(major_r, minor_r, major_seg, minor_seg)`.
- CPU regeneration is the correct approach for changing resolution at runtime (e.g. ImGui slider).
  Geometry shaders are NOT used (deprecated, slow on AMD, absent on Metal). See ADR-0045.
- `smoke_meshgen.exe` (headless): generate sphere + box, assert vertex/index counts, normals unit
  length, UVs in [0,1], tangent W = ±1, exit 0.
- `crd-sandbox` bootstrap (gated by `CRD_BUILD_SANDBOX=ON`, default ON): `SandboxLayer` loads cooked
  assets from `assets/source/` (BoxTextured.glb, Duck.glb, Suzanne.glb + CC0 textures), renders via
  `ForwardRenderPath`, adds ImGui asset browser panel (browse loaded meshes/textures, click to switch),
  and uses `crd-meshgen` shapes for comparison. `--headless` flag for CI.

**Tests:**
- `make_box` vertex count = 24 (6 faces × 4 vertices, no shared vertices).
- `make_sphere(8,8)` vertex/index count formula check.
- All normals have unit length (within 1e-5).
- UV coordinates in [0,1] range for all shapes.
- Tangent W = ±1 (bitangent sign).

---

## Demo assets (`assets/source/`)

Initial assets committed with Phase 2.7 v1e. Attribution in `assets/source/LICENSES.md`.

| File | License | Source |
|------|---------|--------|
| `meshes/BoxTextured.glb` | CC0 | Khronos glTF-Sample-Assets |
| `meshes/Duck.glb` | Apache 2.0 | Khronos glTF-Sample-Assets |
| `meshes/Suzanne.glb` | CC0 | Blender Foundation |
| `textures/checker_512.png` | CC0 | Synthetically generated |
| `textures/bricks_512.png` | CC0 | ambientCG.com |

Phase 2.8 adds `DamagedHelmet.glb` (CC BY 4.0) when PBR shading lands.

---

## Module layout (planned additions)

```
engine/renderer/
  include/crd/renderer/
    texture_resource.hpp          ← v1a (TextureFormat, MipLevel, TextureResource)
    texture_resource_loader.hpp   ← v1a
    mesh_resource.hpp             ← v1b (MeshPrimitive, MeshResource)
    mesh_resource_loader.hpp      ← v1b
    gpu_uploader.hpp              ← v1d (GpuTextureUploader, GpuMeshUploader)
  src/
    texture_resource_loader.cpp   ← v1a
    mesh_resource_loader.cpp      ← v1b
    gpu_uploader.cpp              ← v1d

engine/meshgen/                   ← v1e (new module)
  include/crd/meshgen/meshgen.hpp
  src/meshgen.cpp
  CMakeLists.txt

tools/asset_cooker/src/cook_handlers/
  texture.cpp                     ← v1a
  gltf.cpp                        ← v1b (cgltf; emits MESH artifacts)

tests/resources/
  test_texture_loader.cpp         ← v1a
  test_mesh_loader.cpp            ← v1b
  test_material_loader.cpp        ← v1c (replaces test_material_params.cpp — full foundation scope)
tests/meshgen/
  test_meshgen.cpp                ← v1e

tests/assets/                     ← bundled test assets (tiny PNG, minimal GLB, mat TOML)
  test_quad.glb
  test_checker.png
  test_basic.mat.toml

assets/source/                    ← demo assets (v1e; see LICENSES.md)
  meshes/  textures/  materials/  LICENSES.md

runtime/examples/
  smoke_texture.cpp               ← v1a (CPU data only)
  smoke_mesh.cpp                  ← v1b (CPU data only)
  smoke_material.cpp              ← v1c (CPU data only, no GPU required)
  smoke_asset_import.cpp          ← v1d (full GPU pipeline)
  smoke_meshgen.cpp               ← v1e (headless, CPU data only)

sandbox/                          ← v1e (crd-sandbox, CRD_BUILD_SANDBOX gate)
  src/main.cpp
  src/sandbox_layer.hpp/.cpp
  src/asset_browser.hpp/.cpp
  CMakeLists.txt
```

---

## Definition of done (Phase 2.7)

1. All five slices (v1a–v1e) shipped with unit tests.
2. `smoke_asset_import.exe` runs exit 0 on a Vulkan-capable machine; no Vulkan validation errors.
3. `smoke_meshgen.exe` runs exit 0 on headless CI; all geometry invariants pass.
4. `crd-sandbox --headless` exits 0 (CPU-side resource validation).
5. `crd-sandbox` (GPU) renders BoxTextured.glb and Duck.glb interactively; asset browser switches mesh.
6. Material system foundation (ADR-0048) landed: `MaterialTemplate`, `MaterialInstance`, `SurfaceData` contract,
   full MATR chunk set, ShaderOptions, inline functor. Material debt items 1–3 (artifact layer) closed.
7. Six-configuration green: win-debug / win-relwithdebinfo / win-release / win-asan / win-clang-cl / win-tidy.
8. ADR-0042, ADR-0043, ADR-0045, ADR-0048 filed and cross-referenced here.
9. `TextureResource`, `MeshResource`, and `crd-meshgen` documented in `docs/systems/`.

---

## Open questions

- ~~**MikkTSpace tangent generation.** Does cgltf embed MikkTSpace, or do we need to vendor `mikktspace.h` separately?~~ **Resolved (v1b):** Vendored separately via CPM DOWNLOAD_ONLY + INTERFACE. `mikktspace.c` compiled inline in `mesh.cpp` within an `extern "C"` block to avoid C compiler detection in a `LANGUAGES CXX`-only CMake project.
- ~~**stb_image already in CPM?**~~ **Resolved (v1a):** Yes — added as CPM INTERFACE SYSTEM in root CMakeLists.txt.
- **Staging buffer lifetime.** GPU upload submits a one-shot command buffer. Does the staging
  buffer need to outlive the submission? Fence + immediate wait is simplest for v1d; async
  upload can be deferred.
- ~~**One artifact per glTF mesh node vs one per file.**~~ **Resolved (v1b, ADR-0043):** One MESH artifact per glTF mesh. Multi-mesh files use `CookResult::extra_artifacts` (backward-compatible extension). Extra meshes get `.mesh.<name>.meta` sidecars.

---

## References

- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope
- ADR-0045 — Sandbox, asset layout, cook workflow, crd-meshgen
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy
- ADR-0048 — Material system architecture foundation (full v1c design rationale)
- ADR-0013 — Asset pipeline (cooker is always a separate exe)
- ADR-0038 — CRDR container format (chunk registry extended here)
- ADR-0040 — Cooker CLI + CMake (cgltf and stb_image already declared as cooker deps)
- `docs/debt.md` — Material system v1 gaps (items 1–3 artifact layer closed by v1c; GPU wiring in Phase 2.8)
- `docs/systems/sandbox.md` — crd-sandbox scope contract
- `docs/phases/phase-2-graphics.md` — Renderer v1 context
- `docs/phases/phase-2.8-material-completion.md` — GPU-side wiring of v1c artifact data
