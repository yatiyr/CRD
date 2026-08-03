#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing -- REN-40-I4: CLUSTER BVH.
//
// Binary BVH over cluster bounding spheres for GPU traversal.
// Nodes are 32 bytes, stored in DFS pre-order (left child always at
// index + 1; right child stored explicitly).
//
// Each internal node carries the enclosing sphere of all descendant
// clusters plus aggregated max_error / min_parent_error for LOD-level
// culling.  A leaf stores the cluster index in `left` and is identified
// by `right == 0xFFFFFFFF`.
//
// Build uses Wald 2007 binned SAH (16 bins) over AABB proxies of the
// cluster spheres.  Deterministic: axis order X-Y-Z, strict-less cost
// comparison, first-wins tie-break (cf. bvh_build.hpp).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/dag_build.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

struct ClusterBvhNode
{
    crd::f32 center[3]{};
    crd::f32 radius          = 0.0F;
    crd::f32 max_error       = 0.0F;
    crd::f32 min_parent_error = 0.0F;
    crd::u32 left  = 0U;
    crd::u32 right = 0xFFFFFFFFU;
};
static_assert(sizeof(ClusterBvhNode) == 32U);

struct ClusterBvhResult
{
    crd::containers::Array<ClusterBvhNode> nodes;
    crd::u32 node_count = 0U;
    crd::u32 leaf_count = 0U;
    crd::u32 depth      = 0U;

    explicit ClusterBvhResult(crd::memory::IAllocator* a) : nodes(a) {}
};

enum class ClusterBvhStatus : crd::u8
{
    Ok = 0,
    EmptyInput,
};

struct ClusterBvhReport
{
    ClusterBvhStatus status     = ClusterBvhStatus::Ok;
    crd::u32         node_count = 0U;
    crd::u32         leaf_count = 0U;
    crd::u32         depth      = 0U;
};

[[nodiscard]] ClusterBvhReport build_cluster_bvh(const DagCluster* clusters, crd::u32 cluster_count,
                                                  ClusterBvhResult& out,
                                                  crd::memory::IAllocator* scratch);

} // namespace crd::geometry::mesh_processing
