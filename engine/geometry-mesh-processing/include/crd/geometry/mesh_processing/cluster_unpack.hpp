#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing -- REN-40-I7: CLUSTER UNPACK (CPU mesh shader).
//
// Unpacks selected clusters from the GPU-packed format back into renderable
// triangles.  This is the CPU reference for the mesh shader: given the
// packed cluster data (I-5) and a selection list (I-6), produce a triangle
// soup (positions + index triples) identical to what the GPU mesh shader
// would emit.
//
// Gate: cook → select all leaves → unpack → triangle count == original.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/cluster_dag_cook.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

struct ClusterUnpackResult
{
    crd::containers::Array<crd::f32> positions;
    crd::containers::Array<crd::u32> triangles;
    crd::u32                         vertex_count   = 0U;
    crd::u32                         triangle_count = 0U;

    explicit ClusterUnpackResult(crd::memory::IAllocator* a) : positions(a), triangles(a) {}
};

void unpack_selected_clusters(const crd::u32* packed_clusters,
                               const crd::u32* cluster_vertices,
                               const crd::u32* cluster_triangles_packed,
                               const crd::f32* positions,
                               const crd::u32* selected_indices, crd::u32 selected_count,
                               ClusterUnpackResult& out);

} // namespace crd::geometry::mesh_processing
