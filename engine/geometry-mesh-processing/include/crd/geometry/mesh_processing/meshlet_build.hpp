#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — REN-40-I1: MESHLET BUILDER.
//
// Partition an indexed triangle mesh into meshlets: compact groups of ≤ N
// vertices and ≤ M triangles, each rendering as one mesh-shader workgroup.
// The algorithm is a greedy spatial growth that maximises vertex reuse
// within each meshlet — the standard approach (meshoptimizer, DirectXMesh,
// Nanite's first pass).
//
// **Output layout (GPU-ready):**
//   meshlet_vertices  [meshlet.vertex_offset .. +vertex_count]
//       → global vertex indices (u32); the mesh shader loads the vertex
//         buffer at these positions.
//   meshlet_triangles [meshlet.triangle_offset .. +triangle_count * 3]
//       → local indices (u8) into the meshlet's own vertex list; fits
//         in a single u32 per triangle when packed (3 × u8 + pad).
//
// **Determinism contract:** meshlets are emitted in the order their seed
// triangle appears in the index buffer; within a meshlet, triangles are
// added in adjacency-first, lowest-index-tie-break order. Byte-identical
// output across compilers given byte-identical input.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

inline constexpr crd::u32 kMeshletMaxVertices  = 64U;
inline constexpr crd::u32 kMeshletMaxTriangles = 124U;

struct Meshlet
{
    crd::u32 vertex_offset   = 0U;
    crd::u32 triangle_offset = 0U;
    crd::u8  vertex_count    = 0U;
    crd::u8  triangle_count  = 0U;
    crd::u8  pad[2]{};
};
static_assert(sizeof(Meshlet) == 12U);

struct MeshletBuildResult
{
    crd::containers::Array<Meshlet>  meshlets;
    crd::containers::Array<crd::u32> meshlet_vertices;
    crd::containers::Array<crd::u8>  meshlet_triangles;

    crd::u32 total_triangles = 0U;
    crd::u32 total_vertices  = 0U;

    explicit MeshletBuildResult(crd::memory::IAllocator* a)
        : meshlets(a), meshlet_vertices(a), meshlet_triangles(a)
    {
    }
};

enum class MeshletBuildStatus : crd::u8
{
    Ok = 0,
    EmptyMesh,
    NotTriangles,
    InvalidIndex,
};

struct MeshletBuildOptions
{
    crd::u32 max_vertices  = kMeshletMaxVertices;
    crd::u32 max_triangles = kMeshletMaxTriangles;
};

struct MeshletBuildReport
{
    MeshletBuildStatus status         = MeshletBuildStatus::Ok;
    crd::u32           meshlet_count  = 0U;
    crd::u32           triangle_count = 0U;
    crd::f32           avg_vertex_reuse = 0.0F;
};

[[nodiscard]] MeshletBuildReport build_meshlets(
    const crd::f32*                positions,
    crd::u32                       vertex_count,
    const crd::u32*                indices,
    crd::u32                       index_count,
    const MeshletBuildOptions&     opts,
    MeshletBuildResult&            out,
    crd::memory::IAllocator*       scratch);

} // namespace crd::geometry::mesh_processing
