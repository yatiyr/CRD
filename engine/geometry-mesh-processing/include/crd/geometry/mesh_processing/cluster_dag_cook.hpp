#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing -- REN-40-I5: CLUSTER DAG COOK.
//
// Entry point that takes raw positions + indices and produces a GPU-ready
// packed cluster DAG + BVH.  Orchestrates: meshlet build (I-1) ->
// cluster grouping (I-2) -> DAG build (I-3) -> BVH build (I-4) -> pack.
//
// Output is flat u32 arrays uploadable to a single GPU storage buffer.
//
// ── Per-cluster packed layout (10 u32 words, 40 bytes) ──────────────────
//   [0] vertex_offset     u32 — into cluster_vertices
//   [1] triangle_offset   u32 — byte offset into packed cluster_triangles
//   [2] vertex_count:8 | triangle_count:8 | level:16
//   [3] error             f32 bits
//   [4] parent_error      f32 bits
//   [5] center.x          f32 bits
//   [6] center.y          f32 bits
//   [7] center.z          f32 bits
//   [8] radius            f32 bits
//   [9] reserved          0
//
// ── Per-BVH-node packed layout (8 u32 words, 32 bytes) ──────────────────
//   [0] center.x          f32 bits
//   [1] center.y          f32 bits
//   [2] center.z          f32 bits
//   [3] radius            f32 bits
//   [4] max_error         f32 bits
//   [5] min_parent_error  f32 bits
//   [6] left              u32
//   [7] right             u32
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/dag_build.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

inline constexpr crd::u32 kClusterGpuWords = 10U;
inline constexpr crd::u32 kBvhNodeGpuWords = 8U;

struct ClusterDagCookResult
{
    crd::containers::Array<crd::u32> packed_clusters;
    crd::containers::Array<crd::u32> packed_bvh;
    crd::containers::Array<crd::u32> cluster_vertices;
    crd::containers::Array<crd::u32> cluster_triangles_packed;
    crd::containers::Array<crd::f32> positions;

    crd::u32 cluster_count       = 0U;
    crd::u32 bvh_node_count      = 0U;
    crd::u32 bvh_depth           = 0U;
    crd::u32 level_count         = 0U;
    crd::u32 leaf_count          = 0U;
    crd::u32 vertex_count        = 0U;
    crd::u32 triangle_byte_count = 0U;

    explicit ClusterDagCookResult(crd::memory::IAllocator* a)
        : packed_clusters(a), packed_bvh(a), cluster_vertices(a),
          cluster_triangles_packed(a), positions(a)
    {}
};

enum class ClusterDagCookStatus : crd::u8
{
    Ok = 0,
    EmptyMesh,
    DagBuildFailed,
    BvhBuildFailed,
};

struct ClusterDagCookReport
{
    ClusterDagCookStatus status       = ClusterDagCookStatus::Ok;
    crd::u32             cluster_count  = 0U;
    crd::u32             bvh_node_count = 0U;
    crd::u32             level_count    = 0U;
    crd::u32             leaf_count     = 0U;
};

[[nodiscard]] ClusterDagCookReport cook_cluster_dag(const crd::f32* positions, crd::u32 vertex_count,
                                                     const crd::u32* indices, crd::u32 index_count,
                                                     const DagBuildOptions& opts,
                                                     ClusterDagCookResult& out,
                                                     crd::memory::IAllocator* scratch);

} // namespace crd::geometry::mesh_processing
