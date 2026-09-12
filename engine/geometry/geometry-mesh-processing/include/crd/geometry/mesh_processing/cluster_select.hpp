#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing -- REN-40-I6: CLUSTER LOD SELECTION.
//
// CPU-side selection that mirrors the GPU compute kernel exactly.
//
// Selection criterion for cluster C:
//   parent_error  >  threshold * distance * proj_factor   (parent too coarse)
//   error         <= threshold * distance * proj_factor   (own detail fine)
//
// Two implementations (produce identical results):
//   select_clusters_flat  — O(cluster_count), one pass over all clusters
//   select_clusters_bvh   — O(selected * log N), BVH prunes subtrees
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/cluster_dag_cook.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

struct ClusterSelectParams
{
    crd::f32 error_threshold = 1.0F;
    crd::f32 proj_factor     = 0.001F;
    crd::f32 camera_pos[3]   = {};
};

[[nodiscard]] crd::u32 select_clusters_flat(const crd::u32* packed_clusters, crd::u32 cluster_count,
                                             const ClusterSelectParams& params,
                                             crd::u32* out_selected, crd::u32 max_selected);

[[nodiscard]] crd::u32 select_clusters_bvh(const crd::u32* packed_clusters, crd::u32 cluster_count,
                                            const crd::u32* packed_bvh, crd::u32 bvh_node_count,
                                            const ClusterSelectParams& params,
                                            crd::u32* out_selected, crd::u32 max_selected,
                                            crd::memory::IAllocator* scratch);

} // namespace crd::geometry::mesh_processing
