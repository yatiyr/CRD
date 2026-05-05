# crd-meshgen

CPU-side procedural geometry generator. Produces `crd::renderer::MeshResource` instances with
exactly one `MeshPrimitive` each. All output uses the standard 48-byte interleaved vertex layout
(`kMeshVertexStride`). No GPU objects, no file I/O — pure CPU math.

**Phase 2.7 v1e COMPLETE.**

Lives in: `engine/meshgen/`
Depends on: `crd-core`, `crd-math`, `crd-memory`, `crd-renderer`
Does NOT depend on: `crd-rhi`, `crd-shader`, `crd-resources`

## Status

| Slice | Ships | Status |
|-------|-------|--------|
| v1e | 8 generators, `smoke_meshgen.exe` headless, 11 unit tests | ✅ shipped 2026-05-05 |

## Generators

All functions are in `namespace crd::meshgen`. All take `IAllocator*` as their first argument.
All return `crd::renderer::MeshResource` by value (move).

```cpp
#include <crd/meshgen/meshgen.hpp>

namespace crd::meshgen
{
[[nodiscard]] MeshResource make_plane    (IAllocator*, float width=1, float depth=1, u32 divs_x=1, u32 divs_z=1);
[[nodiscard]] MeshResource make_box      (IAllocator*, float width=1, float height=1, float depth=1);
[[nodiscard]] MeshResource make_sphere   (IAllocator*, float radius=1, u32 lat_bands=16, u32 lon_bands=32);
[[nodiscard]] MeshResource make_icosphere(IAllocator*, float radius=1, u32 subdivisions=2);
[[nodiscard]] MeshResource make_cylinder (IAllocator*, float radius=0.5, float height=1, u32 segs=32);
[[nodiscard]] MeshResource make_cone     (IAllocator*, float radius=0.5, float height=1, u32 segs=32);
[[nodiscard]] MeshResource make_capsule  (IAllocator*, float radius=0.5, float height=1, u32 segs=32, u32 rings=8);
[[nodiscard]] MeshResource make_torus    (IAllocator*, float major_r=1, float minor_r=0.25, u32 maj_segs=32, u32 min_segs=16);
}
```

## Vertex layout

Identical to the standard mesh pipeline layout (`kMeshVertexStride = 48`):

| Bytes  | Field   | Notes |
|--------|---------|-------|
| 0–11   | float3 position | world units, centred at origin |
| 12–23  | float3 normal   | normalised; outward-facing |
| 24–31  | float2 uv0      | [0,1] × [0,1] |
| 32–47  | float4 tangent  | xyz normalised; w = bitangent sign (+1 in v1e, full MikkTSpace in future) |

Winding: **CCW front-face** (Vulkan default with `frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE`).

## Shape descriptions

| Generator    | Topology | Notes |
|--------------|----------|-------|
| `make_plane` | subdivided quad grid, XZ plane, +Y facing | `(divs_x+1) × (divs_z+1)` verts; 4 verts per `1×1` grid |
| `make_box`   | 6 × 4 separate faces (24 verts / 36 indices) | per-face flat normals; no vertex sharing between faces |
| `make_sphere` | UV sphere (lat/lon parameterisation) | `(lat_bands+1) × (lon_bands+1)` verts; normal = normalised position |
| `make_icosphere` | icosahedron + midpoint subdivision | each subdivision → 4× triangles; `HashMap<u64,u32>` edge-midpoint cache prevents duplicate verts at shared edges; key = `(min_idx << 32) | max_idx` |
| `make_cylinder` | side quads + top/bottom triangle fans | caps share a centre vertex; smooth side normals |
| `make_cone`    | side triangles + bottom fan | apex shares one vertex; smooth side normals |
| `make_capsule` | hemisphere caps + cylinder body | `segs` longitudinal bands; `rings` latitude bands per hemisphere |
| `make_torus`   | `maj_segs × min_segs` quad ring | major circle in XZ; minor circle sweeps outward |

## Invariants (verified by smoke and unit tests)

- `primitives.size() == 1` for every generator.
- `vertex_count > 0`, `index_count > 0`.
- `vertices.size() == vertex_count * kMeshVertexStride`.
- `indices.size() == index_count * sizeof(u32)`.
- All normals unit-length (tolerance 1e-4).
- Tangent w == +1 for all vertices.
- All index values < `vertex_count`.
- Sphere: all normals == normalised position.
- Icosphere: all vertices lie on a sphere of the requested radius (tolerance 1e-4).

## Minimal usage

```cpp
#include <crd/meshgen/meshgen.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

crd::memory::MallocAllocator alloc;
auto sphere = crd::meshgen::make_sphere(&alloc, 1.0F, 16U, 32U);

const auto& prim = sphere.primitives[0];
// prim.vertex_count, prim.index_count
// sphere.vertices.data() — 48 bytes per vertex, kMeshVertexStride layout
// sphere.indices.data()  — u32 index buffer
```

The mesh can be uploaded to the GPU immediately via `GpuUploader::upload_mesh`:

```cpp
#include <crd/renderer/gpu_uploader.hpp>

crd::renderer::GpuMesh gpu = crd::renderer::GpuUploader::upload_mesh(sphere, device);
// gpu.vertex_buffer, gpu.index_buffer — device-local rhi::Buffer*
```

## Tests

`tests/meshgen/test_meshgen.cpp` — 11 Catch2 test cases, 17 265 assertions:
- Shape-agnostic invariants (all 8 generators via parameterised sections)
- Sphere-specific normal invariant
- Icosphere radius invariant
- Plane subdivision vertex count formula
- Box exact vertex/index counts

```powershell
& "D:\Dev\cerid\build\win-debug\tests\meshgen\crd-meshgen-tests.exe" --reporter compact
```

## Session logs

- [v1e — crd-meshgen + sandbox Meshgen Browser](../sessions/2026-05-05-meshgen-v1e.md)
