#include <crd/geometry/mesh_processing/cluster_dag_cook.hpp>

#include <crd/geometry/mesh_processing/cluster_bvh.hpp>

#include <cstring>

namespace crd::geometry::mesh_processing
{
namespace
{

crd::u32 f32_to_bits(crd::f32 v) noexcept
{
    crd::u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

void pack_cluster(const DagCluster& c, crd::u32* out) noexcept
{
    out[0] = c.vertex_offset;
    out[1] = c.triangle_offset;
    out[2] = static_cast<crd::u32>(c.vertex_count)
           | (static_cast<crd::u32>(c.triangle_count) << 8U)
           | (c.level << 16U);
    out[3] = f32_to_bits(c.error);
    out[4] = f32_to_bits(c.parent_error);
    out[5] = f32_to_bits(c.center[0]);
    out[6] = f32_to_bits(c.center[1]);
    out[7] = f32_to_bits(c.center[2]);
    out[8] = f32_to_bits(c.radius);
    out[9] = 0U;
}

void pack_bvh_node(const ClusterBvhNode& n, crd::u32* out) noexcept
{
    out[0] = f32_to_bits(n.center[0]);
    out[1] = f32_to_bits(n.center[1]);
    out[2] = f32_to_bits(n.center[2]);
    out[3] = f32_to_bits(n.radius);
    out[4] = f32_to_bits(n.max_error);
    out[5] = f32_to_bits(n.min_parent_error);
    out[6] = n.left;
    out[7] = n.right;
}

} // anonymous namespace

ClusterDagCookReport cook_cluster_dag(const crd::f32* positions, crd::u32 vertex_count,
                                       const crd::u32* indices, crd::u32 index_count,
                                       const DagBuildOptions& opts,
                                       ClusterDagCookResult& out,
                                       crd::memory::IAllocator* scratch)
{
    ClusterDagCookReport report;

    DagBuildResult dag(scratch);
    const auto dag_rep = build_cluster_dag(positions, vertex_count,
                                            indices, index_count,
                                            opts, dag, scratch);
    if (dag_rep.status != DagBuildStatus::Ok)
    {
        report.status = (dag_rep.status == DagBuildStatus::EmptyMesh)
                      ? ClusterDagCookStatus::EmptyMesh
                      : ClusterDagCookStatus::DagBuildFailed;
        return report;
    }

    const crd::u32 cc = static_cast<crd::u32>(dag.clusters.size());

    ClusterBvhResult bvh(scratch);
    const auto bvh_rep = build_cluster_bvh(dag.clusters.data(), cc, bvh, scratch);
    if (bvh_rep.status != ClusterBvhStatus::Ok)
    {
        report.status = ClusterDagCookStatus::BvhBuildFailed;
        return report;
    }

    out.packed_clusters.resize(static_cast<crd::usize>(cc) * kClusterGpuWords);
    for (crd::u32 ci = 0; ci < cc; ++ci)
        pack_cluster(dag.clusters[ci],
                     out.packed_clusters.data() + static_cast<crd::usize>(ci) * kClusterGpuWords);

    out.packed_bvh.resize(static_cast<crd::usize>(bvh_rep.node_count) * kBvhNodeGpuWords);
    for (crd::u32 ni = 0; ni < bvh_rep.node_count; ++ni)
        pack_bvh_node(bvh.nodes[ni],
                      out.packed_bvh.data() + static_cast<crd::usize>(ni) * kBvhNodeGpuWords);

    out.cluster_vertices.resize(dag.cluster_vertices.size());
    for (crd::usize i = 0; i < dag.cluster_vertices.size(); ++i)
        out.cluster_vertices[i] = dag.cluster_vertices[i];

    const crd::u32 tri_bytes = static_cast<crd::u32>(dag.cluster_triangles.size());
    const crd::u32 tri_words = (tri_bytes + 3U) / 4U;
    out.cluster_triangles_packed.resize(tri_words);
    for (crd::u32 i = 0; i < tri_words; ++i)
        out.cluster_triangles_packed[i] = 0U;
    for (crd::u32 i = 0; i < tri_bytes; ++i)
    {
        const crd::u32 word_idx = i / 4U;
        const crd::u32 byte_idx = i % 4U;
        out.cluster_triangles_packed[word_idx] |=
            static_cast<crd::u32>(dag.cluster_triangles[i]) << (byte_idx * 8U);
    }

    out.positions.resize(dag.positions.size());
    for (crd::usize i = 0; i < dag.positions.size(); ++i)
        out.positions[i] = dag.positions[i];

    out.cluster_count       = cc;
    out.bvh_node_count      = bvh_rep.node_count;
    out.bvh_depth           = bvh_rep.depth;
    out.level_count         = dag_rep.level_count;
    out.leaf_count          = dag_rep.leaf_count;
    out.vertex_count        = static_cast<crd::u32>(dag.positions.size() / 3U);
    out.triangle_byte_count = tri_bytes;

    report.status         = ClusterDagCookStatus::Ok;
    report.cluster_count  = cc;
    report.bvh_node_count = bvh_rep.node_count;
    report.level_count    = dag_rep.level_count;
    report.leaf_count     = dag_rep.leaf_count;
    return report;
}

} // namespace crd::geometry::mesh_processing
