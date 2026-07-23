#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/mesh_resource.hpp>

namespace crd::meshgen
{

// All generators produce a MeshResource with a single MeshPrimitive.
// Vertex layout follows kMeshVertexStride (48 bytes):
//   float3 position, float3 normal, float2 uv0, float4 tangent (w = bitangent sign = +1).
// All geometry uses CCW front-face winding (Vulkan default).

[[nodiscard]] crd::resources::MeshResource make_plane(
    crd::memory::IAllocator* a,
    float                    width  = 1.0F,
    float                    depth  = 1.0F,
    crd::u32                 divs_x = 1U,
    crd::u32                 divs_z = 1U);

[[nodiscard]] crd::resources::MeshResource make_box(
    crd::memory::IAllocator* a,
    float                    width  = 1.0F,
    float                    height = 1.0F,
    float                    depth  = 1.0F);

[[nodiscard]] crd::resources::MeshResource make_sphere(
    crd::memory::IAllocator* a,
    float                    radius     = 1.0F,
    crd::u32                 lat_bands  = 16U,
    crd::u32                 lon_bands  = 32U);

[[nodiscard]] crd::resources::MeshResource make_icosphere(
    crd::memory::IAllocator* a,
    float                    radius       = 1.0F,
    crd::u32                 subdivisions = 2U);

[[nodiscard]] crd::resources::MeshResource make_cylinder(
    crd::memory::IAllocator* a,
    float                    radius = 0.5F,
    float                    height = 1.0F,
    crd::u32                 segs   = 32U);

[[nodiscard]] crd::resources::MeshResource make_cone(
    crd::memory::IAllocator* a,
    float                    radius = 0.5F,
    float                    height = 1.0F,
    crd::u32                 segs   = 32U);

[[nodiscard]] crd::resources::MeshResource make_capsule(
    crd::memory::IAllocator* a,
    float                    radius = 0.5F,
    float                    height = 1.0F,
    crd::u32                 segs   = 32U,
    crd::u32                 rings  = 8U);

[[nodiscard]] crd::resources::MeshResource make_torus(
    crd::memory::IAllocator* a,
    float                    major_r   = 1.0F,
    float                    minor_r   = 0.25F,
    crd::u32                 maj_segs  = 32U,
    crd::u32                 min_segs  = 16U);

} // namespace crd::meshgen
