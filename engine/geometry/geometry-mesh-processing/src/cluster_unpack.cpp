#include <crd/geometry/mesh_processing/cluster_unpack.hpp>

namespace crd::geometry::mesh_processing
{
namespace
{

crd::u8 unpack_tri_byte(const crd::u32* packed, crd::u32 byte_idx) noexcept
{
    const crd::u32 word = packed[byte_idx / 4U];
    const crd::u32 shift = (byte_idx % 4U) * 8U;
    return static_cast<crd::u8>((word >> shift) & 0xFFU);
}

} // anonymous namespace

void unpack_selected_clusters(const crd::u32* packed_clusters,
                               const crd::u32* cluster_vertices,
                               const crd::u32* cluster_triangles_packed,
                               const crd::f32* positions,
                               const crd::u32* selected_indices, crd::u32 selected_count,
                               ClusterUnpackResult& out)
{
    crd::u32 total_verts = 0U;
    crd::u32 total_tris  = 0U;
    for (crd::u32 si = 0; si < selected_count; ++si)
    {
        const crd::u32* w = packed_clusters
                          + static_cast<crd::usize>(selected_indices[si]) * kClusterGpuWords;
        total_verts += w[2] & 0xFFU;
        total_tris  += (w[2] >> 8U) & 0xFFU;
    }

    out.positions.resize(static_cast<crd::usize>(total_verts) * 3U);
    out.triangles.resize(static_cast<crd::usize>(total_tris) * 3U);
    out.vertex_count   = total_verts;
    out.triangle_count = total_tris;

    crd::u32 v_cursor = 0U;
    crd::u32 t_cursor = 0U;

    for (crd::u32 si = 0; si < selected_count; ++si)
    {
        const crd::u32* w = packed_clusters
                          + static_cast<crd::usize>(selected_indices[si]) * kClusterGpuWords;
        const crd::u32 vert_off = w[0];
        const crd::u32 tri_off  = w[1];
        const crd::u32 vc       = w[2] & 0xFFU;
        const crd::u32 tc       = (w[2] >> 8U) & 0xFFU;

        for (crd::u32 vi = 0; vi < vc; ++vi)
        {
            const crd::u32 global_vi = cluster_vertices[vert_off + vi];
            out.positions[(v_cursor + vi) * 3U + 0U] = positions[global_vi * 3U + 0U];
            out.positions[(v_cursor + vi) * 3U + 1U] = positions[global_vi * 3U + 1U];
            out.positions[(v_cursor + vi) * 3U + 2U] = positions[global_vi * 3U + 2U];
        }

        for (crd::u32 ti = 0; ti < tc; ++ti)
        {
            const crd::u32 byte_base = tri_off + ti * 3U;
            const crd::u8  li0 = unpack_tri_byte(cluster_triangles_packed, byte_base + 0U);
            const crd::u8  li1 = unpack_tri_byte(cluster_triangles_packed, byte_base + 1U);
            const crd::u8  li2 = unpack_tri_byte(cluster_triangles_packed, byte_base + 2U);
            out.triangles[(t_cursor + ti) * 3U + 0U] = v_cursor + li0;
            out.triangles[(t_cursor + ti) * 3U + 1U] = v_cursor + li1;
            out.triangles[(t_cursor + ti) * 3U + 2U] = v_cursor + li2;
        }

        v_cursor += vc;
        t_cursor += tc;
    }
}

} // namespace crd::geometry::mesh_processing
