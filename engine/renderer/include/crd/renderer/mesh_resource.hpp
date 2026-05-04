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
