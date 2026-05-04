# Session — Phase 2.7 v1b: MeshResource + glTF Import

**Date:** 2026-05-05  
**Shipped:** Phase 2.7 v1b — `MeshResource` + cgltf glTF import + MikkTSpace tangent generation  
**Test delta:** +4 tests (452 win-debug, up from 448)  
**Smoke delta:** +1 headless smoke (`smoke_mesh`)

---

## What shipped

### New types in `engine/renderer/`

`MeshPrimitive` + `MeshResource` in `include/crd/renderer/mesh_resource.hpp`:

- **Vertex layout:** 48 bytes/vertex, interleaved: float3 pos (0–11) + float3 normal (12–23) + float2 uv0 (24–31) + float4 tangent (32–47, w = bitangent sign).
- `MeshResource` holds three flat buffers: `vertices` (raw bytes), `indices` (u32 raw bytes), `primitives` (typed array of `MeshPrimitive`).
- `kMeshVertexStride = 48U` as inline constexpr.

### `MeshResourceLoader` (`engine/renderer/src/mesh_resource_loader.cpp`)

Parses `type='MESH'` CRDR artifact:
- Finds `VERT`, `INDX`, `PRIM` chunks via `crdr_read()`. Missing `VERT` → `LoadState::Failed`.
- PRIM chunk: 4-byte count, then N × 32-byte entries. Each entry: vertex_count u32, index_count u32, vertex_byte_offset u32, index_byte_offset u32, material_id u8[16] (ResourceId hi+lo LE).
- Uses a `MallocAllocator` member (not the LoadContext allocator); matches `TextureResourceLoader` pattern.
- `std::make_unique<MeshResource>()` (no-arg, `MallocAllocator` passed to `MeshResource` constructor).

### glTF cooker handler (`tools/asset_cooker/src/cook_handlers/mesh.cpp`)

**cgltf + MikkTSpace integration:**
- `#define CGLTF_IMPLEMENTATION` + `#include <cgltf.h>` at top.
- `mikktspace.h` + `mikktspace.c` included inside `extern "C"` block — compiles the C implementation as C++ within the same translation unit (no separate library target needed).

**`cook_primitive()`:**
1. Reads POSITION/NORMAL/TEXCOORD_0/TANGENT accessors.
2. Defaults NORMAL to (0,0,1) and UV to (0,0) if absent.
3. If TANGENT absent: reads indices into u32 array, calls `generate_tangents()` via MikkTSpace `genTangSpaceDefault()`.
4. Interleaves to 48B/vertex layout.
5. Upcasts all indices to u32.

**`build_mesh_artifact()`:** loops primitives, accumulates VERT + INDX flat buffers, builds PRIM table (4-byte count + 32-byte entries), assembles `CrdrWriter` MESH artifact.

**Multi-mesh support (`extra_artifacts`):**
- First glTF mesh → `CookResult::cooked_bytes` + `type_fourcc = kFourCC_MESH`.
- Meshes 1..N-1 → `CookResult::extra_artifacts` (new `ExtraArtifact` field on `CookResult`).
- Each extra gets a `.mesh.<sanitized_name>.meta` sidecar for stable UUID persistence across recooks.

### `CookResult` extension (`tools/asset_cooker/include/crd/cooker/cook_handler.hpp`)

```cpp
struct ExtraArtifact {
    resources::ResourceId      id;
    u32                        type_fourcc = 0;
    containers::Array<u8>      cooked_bytes;
    containers::String         name; // "<path>#<mesh_name>"
};

struct CookResult {
    // ... existing fields ...
    containers::Array<ExtraArtifact> extra_artifacts; // NEW
};
```

Backward compatible — existing handlers don't fill `extra_artifacts`, consumers see an empty array.

`cook_command.cpp` extended: after processing the main artifact, loops `result.extra_artifacts` and applies the same cache-check + write + PACK assembly logic.

### CMake changes

- `cgltf`: CPM DOWNLOAD_ONLY, INTERFACE library with SYSTEM include dir.
- `MikkTSpace`: CPM DOWNLOAD_ONLY, INTERFACE library with SYSTEM include dir (comment explains the inline-include trick).
- Both added as PRIVATE deps to `crd-cooker` in `tools/asset_cooker/CMakeLists.txt`.

### Tests (`tests/resources/test_mesh_loader.cpp`)

4 tests (no `jobs::init()` — `ResourcesJobsListener` in `test_resource_manager.cpp` handles that for the whole binary):

1. **`[loader]` MESH round-trip** — hand-assembled MESH CRDR (2 prims), `write_mesh_pack()` helper, mount, `load_sync`, verify primitive/buffer sizes.
2. **`[multi_prim]` counts** — 3-primitive mesh, verify `primitives.size() == 3` and individual vertex/index counts.
3. **`[missing_vert]` → Failed** — MESH artifact with no VERT chunk, verify `LoadState::Failed`.
4. **`[cook]` GLB cook round-trip** — `make_triangle_glb()` builds a minimal 152-byte binary GLB (JSON chunk + BIN chunk, POSITION + NORMAL + TEXCOORD_0 + TANGENT + u16 indices), registers glTF handler, cooks, mounts, loads, verifies 1 primitive with correct vertex/index counts.

### Smoke (`runtime/examples/smoke_mesh.cpp`)

Headless smoke: hand-assembles a 2-primitive MESH CRDR artifact, writes to `smoke_mesh_tmp.crdr`, mounts, `load_sync<MeshResource>`, verifies:
- `primitives.size() == 2`
- `primitives[0/1].vertex_count == 3`, `index_count == 3`
- `vertices.size() == 2 * 3 * 48`, `indices.size() == 2 * 3 * 4`
- `primitives[0/1].material_id.is_null()`

Deletes temp file, prints "smoke_mesh: OK", exits 0.

---

## Key decisions

### MikkTSpace vendor strategy

Vendored as CPM DOWNLOAD_ONLY + INTERFACE (header-only from CMake's perspective). The `mikktspace.c` implementation is compiled inline in `mesh.cpp` via `#include "mikktspace.c"` inside an `extern "C"` block. This avoids adding C language to CMake's `project(... LANGUAGES CXX)` declaration, which would force C compiler detection for the entire build.

### LANGUAGES CXX-only constraint

The root project has `LANGUAGES CXX`. Third-party C packages (GLFW, zstd) call `project(...)` in their own subdirectories and configure their own C compiler at that point. Adding a STATIC C library at the root level triggers C compiler detection before those subdirectory projects, corrupting the CMake cache. The INTERFACE + inline include pattern is the correct solution.

### `ExtraArtifact` / multi-artifact cook (Option A)

Chose `CookResult::extra_artifacts: Array<ExtraArtifact>` over a registry-callback model or separate cook invocations. Option A is the simplest backward-compatible extension: existing handlers return an empty array, `cook_command.cpp` processes extras after the main artifact with the same cache logic.

### One MESH artifact per glTF mesh (ADR-0043)

Each glTF `cgltf_mesh` node becomes one MESH CRDR artifact. Multi-mesh files produce multiple artifacts (one main + N-1 extras). This enables independent UUID addressability per mesh, which the Phase 3.0 editor will use to track individual assets. Batching into a single artifact would break this.

---

## Build notes

The win-debug build directory was deleted and reconfigured from scratch after a corrupted CMake cache (from a prior failed configure with mikktspace as STATIC). The reconfigure required manually setting `PATH`, `INCLUDE`, and `LIB` to the VS2026 MSVC + SDK paths since vcvars64.bat was not available in the session's shell. A reusable PowerShell block was established:

```powershell
$vsBase = "C:\Program Files\Microsoft Visual Studio\18\Community"
$ninja  = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$cmake  = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$msvcBase = "$vsBase\VC\Tools\MSVC"
$msvcVer  = (Get-ChildItem $msvcBase | Sort-Object Name | Select-Object -Last 1).Name
$cl       = "$msvcBase\$msvcVer\bin\Hostx64\x64"
$sdkBase  = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer   = (Get-ChildItem "$sdkBase\Include" | Sort-Object Name | Select-Object -Last 1).Name
$sdkBin   = "$sdkBase\bin\$sdkVer\x64"
$env:PATH    = "$ninja;$cmake;$cl;$sdkBin;$env:PATH"
$env:INCLUDE = "$msvcBase\$msvcVer\include;$sdkBase\Include\$sdkVer\ucrt;..."
$env:LIB     = "$msvcBase\$msvcVer\lib\x64;$sdkBase\Lib\$sdkVer\ucrt\x64;..."
```

---

## Six-configuration results

| Config | Build | CTest | Headless smokes (16) |
|---|---|---|---|
| win-debug | ✅ | 452/452 | ✅ all pass |
| win-relwithdebinfo | ✅ | 452/452 | ✅ all pass |
| win-release | ✅ | 449/449 | ✅ all pass |
| win-asan | ✅ | 452/452 | ✅ all pass |
| win-clang-cl | ✅ | 452/452 | ✅ all pass |
| win-tidy | ✅ | — | — |

Pre-existing clang-tidy `kPayload` naming warning in `test_resource_manager.cpp:839` (not introduced in this session; build still exits 0).
