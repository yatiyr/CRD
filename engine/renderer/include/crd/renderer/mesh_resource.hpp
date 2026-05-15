#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::renderer
{

// Interleaved vertex stride used by all MESH CRDR artifacts (ADR-0043).
//   bytes  0–11: float3  position
//   bytes 12–23: float3  normal
//   bytes 24–31: float2  uv0
//   bytes 32–47: float4  tangent  (xyz = direction, w = bitangent sign: +1 or -1)
//
// ── Dimensional-type contract (ADR-0078 §4 D29 — v0d-4) ──────────────
// MeshResource stores raw bytes; the SI-meter interpretation lives at
// two boundaries:
//   1. COOKER — glTF cooker (`tools/asset_cooker/src/cook_handlers/
//      mesh.cpp`) reads `.meta [cook] position_scale` per ADR-0078 §2 D18
//      and multiplies vertex positions into SI metres at cook time.
//   2. RUNTIME — when a `Renderable` lifts a MeshResource into a
//      `scene::Transform` parent, the typed `scene::Transform::translation
//      = Vec3<Length32>` per ADR-0078 §3 D17 carries the dim tag.
// The byte buffer in MeshResource stays raw — typed access would require
// re-tagging on every vertex read, defeating the SIMD upload path
// (ADR-0078 §3 D22 SIMD-boundary pin).
inline constexpr crd::u32 kMeshVertexStride = 48U;

// One contiguous triangle-list draw range within a MeshResource.
struct MeshPrimitive
{
    crd::u32                   vertex_count       = 0;
    crd::u32                   index_count        = 0;
    crd::u32                   vertex_byte_offset = 0; // byte offset into MeshResource::vertices
    crd::u32                   index_byte_offset  = 0; // byte offset into MeshResource::indices
    crd::resources::ResourceId material_id;             // null UUID = no material
};

struct MeshResource
{
    crd::containers::Array<crd::u8>       vertices;   // interleaved, kMeshVertexStride per vertex
    crd::containers::Array<crd::u8>       indices;    // u32 index buffer
    crd::containers::Array<MeshPrimitive> primitives;

    explicit MeshResource(crd::memory::IAllocator* a)
        : vertices(a), indices(a), primitives(a)
    {
    }

    MeshResource(const MeshResource&)            = delete;
    MeshResource& operator=(const MeshResource&) = delete;
    MeshResource(MeshResource&&)                 = default;
    MeshResource& operator=(MeshResource&&)      = default;
};

} // namespace crd::renderer
