#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — REN-40-I3: CLUSTER-DAG BUILDER.
//
// Iteratively group → simplify (QEM, locked boundary) → re-meshletize,
// producing a DAG of clusters where:
//
//   Level 0 = original meshlets (leaves, error = 0).
//   Each higher level = simplified result of grouping the level below.
//   Boundary vertices of each group are LOCKED during QEM → crack-free.
//   Error is MONOTONE: parent.error >= max(child.error) + simplify_error.
//
// **Selection criterion (GPU runtime, I-6):**
//   Render cluster C iff  parent_error > threshold  AND  own_error ≤ threshold.
//   Root clusters have parent_error = FLT_MAX (always eligible).
//
// The DAG is NOT a tree — re-meshletizing a simplified group produces
// meshlets that span multiple original meshlets. The parent-child
// relationship is GROUP-level: all children in a group share the same
// parent cluster(s) as their logical parent.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

struct DagCluster
{
    crd::u32 vertex_offset   = 0U;
    crd::u32 triangle_offset = 0U;
    crd::u8  vertex_count    = 0U;
    crd::u8  triangle_count  = 0U;
    crd::u8  pad[2]{};

    crd::f32 error        = 0.0F;
    crd::f32 parent_error = 0.0F;

    crd::f32 center[3]{};
    crd::f32 radius = 0.0F;

    crd::u32 level = 0U;
};
static_assert(sizeof(DagCluster) == 40U);

struct DagBuildResult
{
    crd::containers::Array<DagCluster> clusters;
    crd::containers::Array<crd::u32>   cluster_vertices;
    crd::containers::Array<crd::u8>    cluster_triangles;
    crd::containers::Array<crd::f32>   positions;
    crd::u32                           level_count = 0U;
    crd::u32                           leaf_count  = 0U;

    explicit DagBuildResult(crd::memory::IAllocator* a)
        : clusters(a), cluster_vertices(a), cluster_triangles(a), positions(a)
    {
    }
};

enum class DagBuildStatus : crd::u8
{
    Ok = 0,
    EmptyMesh,
    MeshletBuildFailed,
};

struct DagBuildOptions
{
    crd::f32 simplify_ratio  = 0.5F;
    crd::u32 group_size      = 4U;
    crd::f32 boundary_weight = 1000.0F;
};

struct DagBuildReport
{
    DagBuildStatus status        = DagBuildStatus::Ok;
    crd::u32       level_count   = 0U;
    crd::u32       cluster_count = 0U;
    crd::u32       leaf_count    = 0U;
};

[[nodiscard]] DagBuildReport build_cluster_dag(const crd::f32* positions, crd::u32 vertex_count,
                                               const crd::u32* indices, crd::u32 index_count,
                                               const DagBuildOptions& opts, DagBuildResult& out,
                                               crd::memory::IAllocator* scratch);

} // namespace crd::geometry::mesh_processing
