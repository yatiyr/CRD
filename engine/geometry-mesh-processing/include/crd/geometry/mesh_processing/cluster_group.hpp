#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — REN-40-I2: CLUSTER GROUPING.
//
// Partition meshlets into groups of ~N (typically 4) for the Cluster-DAG
// simplification pipeline. Adjacent meshlets go into the same group so that
// the group's shared boundary — the vertices that appear in meshlets from
// DIFFERENT groups — is minimised. These boundary vertices become the LOCKED
// set when the DAG builder (I-3) simplifies the group's interior: cracks
// are impossible because neighboring groups always agree at the boundary.
//
// **Adjacency:** two meshlets are adjacent if they share at least one global
// vertex. The weight of the adjacency edge = number of shared vertices.
// Heavier edges make better within-group bonds (more shared vertices →
// fewer boundary vertices after grouping).
//
// **Grouping algorithm:** greedy growth, deterministic. Seed = first
// ungrouped meshlet (lowest index). Grow by adding the ungrouped neighbor
// with the highest total shared-vertex weight to meshlets already in the
// group. Stop at target_group_size or exhaustion. Deterministic tie-break:
// lower meshlet index wins.
//
// **Output:**
//   groups          — array of (offset, count) into group_meshlets
//   group_meshlets  — flat array of meshlet indices, ordered by group
//   boundary_vertices / boundary_offsets — per-group locked vertex set
//   adjacency       — the meshlet adjacency CSR graph (reused by I-3)
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/meshlet_build.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

struct MeshletAdjacency
{
    crd::containers::Array<crd::u32> offsets;
    crd::containers::Array<crd::u32> neighbors;
    crd::containers::Array<crd::u32> weights;

    explicit MeshletAdjacency(crd::memory::IAllocator* a)
        : offsets(a), neighbors(a), weights(a)
    {
    }
};

struct ClusterGroup
{
    crd::u32 first = 0U;
    crd::u32 count = 0U;
};

struct ClusterGroupResult
{
    crd::containers::Array<ClusterGroup> groups;
    crd::containers::Array<crd::u32>     group_meshlets;
    crd::containers::Array<crd::u32>     boundary_vertices;
    crd::containers::Array<crd::u32>     boundary_offsets;
    MeshletAdjacency                     adjacency;

    explicit ClusterGroupResult(crd::memory::IAllocator* a)
        : groups(a), group_meshlets(a), boundary_vertices(a), boundary_offsets(a), adjacency(a)
    {
    }
};

struct ClusterGroupOptions
{
    crd::u32 target_group_size = 4U;
};

enum class ClusterGroupStatus : crd::u8
{
    Ok = 0,
    EmptyInput,
};

struct ClusterGroupReport
{
    ClusterGroupStatus status               = ClusterGroupStatus::Ok;
    crd::u32           group_count          = 0U;
    crd::u32           avg_group_size_x10   = 0U;
    crd::u32           boundary_vertex_count = 0U;
};

[[nodiscard]] ClusterGroupReport group_meshlets(
    const MeshletBuildResult&  meshlets,
    crd::u32                   vertex_count,
    const ClusterGroupOptions& opts,
    ClusterGroupResult&        out,
    crd::memory::IAllocator*   scratch);

} // namespace crd::geometry::mesh_processing
