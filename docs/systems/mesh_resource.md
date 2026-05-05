# MeshResource

CPU-side cooked mesh payload for `crd-renderer`. Loaded from `type='MESH'` CRDR artifacts by
`MeshResourceLoader`. Holds interleaved vertex data + u32 index data + primitive table. No GPU
objects — upload is a separate step handled by `GpuUploader` (Phase 2.7 v1d).

**Phase 2.7 v1b COMPLETE.**

Lives in: `engine/renderer/`
Depends on: `crd-core`, `crd-memory`, `crd-containers`, `crd-resources` (for `ResourceId`)
Does NOT depend on: `crd-rhi` (keep loaders free of RHI for test headlessness)

## Status

| Slice | Ships | Status |
|-------|-------|--------|
| v1b | `MeshPrimitive`, `MeshResource`, `MeshResourceLoader`, cgltf glTF cooker handler + MikkTSpace | ✅ shipped 2026-05-05 |
| v1d | `GpuUploader::upload_mesh` → `GpuMesh` (staging-buffer pattern) | ✅ shipped 2026-05-05 |

## Vertex layout

All mesh artifacts (and all `crd-meshgen` output) use one fixed interleaved stride: **48 bytes per vertex**.

```
bytes  0–11   float3  position   (world units)
bytes 12–23   float3  normal     (normalised)
bytes 24–31   float2  uv0        (primary UV set)
bytes 32–47   float4  tangent    (xyz normalised direction, w = bitangent sign: +1 or -1)
```

`inline constexpr crd::u32 kMeshVertexStride = 48U;` — `engine/renderer/include/crd/renderer/mesh_resource.hpp`

ADR-0043 rationale: single stride simplifies Vulkan `VkVertexInputAttributeDescription` binding;
no per-mesh pipeline permutation for vertex layout; matches MikkTSpace output directly.

## Types

### `MeshPrimitive`

```cpp
struct MeshPrimitive
{
    crd::u32  vertex_count;
    crd::u32  index_count;
    crd::u32  vertex_byte_offset;   // byte offset into MeshResource::vertices
    crd::u32  index_byte_offset;    // byte offset into MeshResource::indices
    crd::resources::ResourceId material_id;  // null UUID = no material assigned
};
```

### `MeshResource`

```cpp
struct MeshResource
{
    crd::containers::Array<crd::u8>       vertices;    // interleaved, kMeshVertexStride per vertex
    crd::containers::Array<crd::u8>       indices;     // u32 index buffer
    crd::containers::Array<MeshPrimitive> primitives;

    explicit MeshResource(crd::memory::IAllocator* a);
    // move-only
};
```

## CRDR artifact format (MESH)

```
artifact type: 'MESH'
loader version: 1

VERT chunk: raw interleaved vertex bytes (vertex_count × 48)
INDX chunk: raw u32 index bytes (index_count × 4)
PRIM chunk: count u32 + N × 32 bytes per entry:
  vertex_count       u32
  index_count        u32
  vertex_byte_offset u32
  index_byte_offset  u32
  material_id        u8[16]  (ResourceId, null if no material)
```

## Loader registration

```cpp
#include <crd/renderer/mesh_resource_loader.hpp>

// At startup, before any ResourceManager mounts:
crd::renderer::register_mesh_loader(&rm);

// Load:
auto h = rm.load_sync<crd::renderer::MeshResource>(mesh_id);
const crd::renderer::MeshResource* mesh = h.get();
// mesh->primitives.size()
// mesh->primitives[0].vertex_count
// mesh->vertices.data() + mesh->primitives[0].vertex_byte_offset
```

## Cooker handler (glTF)

`.glb` and `.gltf` → one `type='MESH'` artifact per glTF mesh node.

- **Parser:** cgltf (CPM DOWNLOAD_ONLY + INTERFACE; header + impl in same TU).
- **Tangents:** MikkTSpace tangent generation (`genTangSpaceDefault()`) when no TANGENT accessor is present in the glTF. MikkTSpace compiled inline in `mesh.cpp` via `extern "C"` block (avoids C compiler detection in a `LANGUAGES CXX`-only CMake project).
- **Scope:** static meshes only. Skinning, morph targets, and animations are deferred to Phase 3.2 (ADR-0044).
- **Indices:** always u32 (u16 indices upcast).
- **Multi-mesh files:** first glTF mesh → main `CookResult`; additional meshes → `CookResult::extra_artifacts`. Each extra artifact gets a `.mesh.<name>.meta` sidecar for UUID stability.
- **Material ID:** if the glTF primitive references a material, the cooker looks up its UUID from an adjacent `.mat.toml.meta` sidecar; zero UUID otherwise.

## GPU upload (v1d)

```cpp
#include <crd/renderer/gpu_uploader.hpp>

crd::renderer::GpuMesh gpu_mesh = crd::renderer::GpuUploader::upload_mesh(*mesh, device);
// gpu_mesh.vertex_buffer — unique_ptr<rhi::Buffer> (device-local, vertex_buffer | transfer_dst)
// gpu_mesh.index_buffer  — unique_ptr<rhi::Buffer> (device-local, index_buffer  | transfer_dst)
```

Upload is synchronous (fence + immediate wait). Both vertex and index buffers use the staging-buffer pattern (host-visible CpuToGpu staging → memcpy → one-shot command buffer → submit_and_wait → destroy staging).

## Session logs

- [v1b — MeshResource + cgltf glTF cooker + MikkTSpace](../sessions/2026-05-05-mesh-resource-v1b.md)
