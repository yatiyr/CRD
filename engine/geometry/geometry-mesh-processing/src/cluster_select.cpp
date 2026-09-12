#include <crd/geometry/mesh_processing/cluster_select.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <cstring>

namespace crd::geometry::mesh_processing
{
namespace
{

crd::f32 bits_to_f32(crd::u32 bits) noexcept
{
    crd::f32 v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

crd::f32 cluster_distance(const crd::u32* packed_cluster, const crd::f32* cam) noexcept
{
    const crd::f32 cx = bits_to_f32(packed_cluster[5]);
    const crd::f32 cy = bits_to_f32(packed_cluster[6]);
    const crd::f32 cz = bits_to_f32(packed_cluster[7]);
    const crd::f32 dx = cx - cam[0];
    const crd::f32 dy = cy - cam[1];
    const crd::f32 dz = cz - cam[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

crd::f32 node_distance(const crd::u32* packed_node, const crd::f32* cam) noexcept
{
    const crd::f32 cx = bits_to_f32(packed_node[0]);
    const crd::f32 cy = bits_to_f32(packed_node[1]);
    const crd::f32 cz = bits_to_f32(packed_node[2]);
    const crd::f32 dx = cx - cam[0];
    const crd::f32 dy = cy - cam[1];
    const crd::f32 dz = cz - cam[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

constexpr crd::u32 kMaxStackDepth = 64U;

void collect_leaves(const crd::u32* packed_bvh, crd::u32 node_idx,
                    crd::u32* out, crd::u32& count, crd::u32 max_selected)
{
    crd::u32 stack[kMaxStackDepth];
    crd::u32 sp = 0U;
    stack[sp++] = node_idx;

    while (sp > 0U && count < max_selected)
    {
        const crd::u32 ni = stack[--sp];
        const crd::u32* w = packed_bvh + static_cast<crd::usize>(ni) * kBvhNodeGpuWords;
        const crd::u32 right = w[7];

        if (right == 0xFFFFFFFFU)
        {
            out[count++] = w[6];
        }
        else
        {
            if (sp + 2U <= kMaxStackDepth)
            {
                stack[sp++] = right;
                stack[sp++] = w[6];
            }
        }
    }
}

} // anonymous namespace

crd::u32 select_clusters_flat(const crd::u32* packed_clusters, crd::u32 cluster_count,
                               const ClusterSelectParams& params,
                               crd::u32* out_selected, crd::u32 max_selected)
{
    crd::u32 count = 0U;
    const crd::f32 tp = params.error_threshold * params.proj_factor;

    for (crd::u32 ci = 0; ci < cluster_count && count < max_selected; ++ci)
    {
        const crd::u32* w = packed_clusters + static_cast<crd::usize>(ci) * kClusterGpuWords;
        const crd::f32 error    = bits_to_f32(w[3]);
        const crd::f32 parent_e = bits_to_f32(w[4]);
        const crd::f32 dist     = cluster_distance(w, params.camera_pos);
        const crd::f32 st       = tp * dist;

        if (parent_e > st && error <= st)
            out_selected[count++] = ci;
    }
    return count;
}

crd::u32 select_clusters_bvh(const crd::u32* packed_clusters, crd::u32 cluster_count,
                              const crd::u32* packed_bvh, crd::u32 bvh_node_count,
                              const ClusterSelectParams& params,
                              crd::u32* out_selected, crd::u32 max_selected,
                              crd::memory::IAllocator* /*scratch*/)
{
    if (cluster_count == 0U || bvh_node_count == 0U) return 0U;

    const crd::f32 tp = params.error_threshold * params.proj_factor;
    crd::u32 count = 0U;

    crd::u32 stack[kMaxStackDepth];
    crd::u32 sp = 0U;
    stack[sp++] = 0U;

    while (sp > 0U && count < max_selected)
    {
        const crd::u32 ni = stack[--sp];
        const crd::u32* w = packed_bvh + static_cast<crd::usize>(ni) * kBvhNodeGpuWords;
        const crd::u32 left  = w[6];
        const crd::u32 right = w[7];

        if (right == 0xFFFFFFFFU)
        {
            const crd::u32 ci   = left;
            const crd::u32* cw  = packed_clusters + static_cast<crd::usize>(ci) * kClusterGpuWords;
            const crd::f32 err  = bits_to_f32(cw[3]);
            const crd::f32 pe   = bits_to_f32(cw[4]);
            const crd::f32 dist = cluster_distance(cw, params.camera_pos);
            const crd::f32 st   = tp * dist;

            if (pe > st && err <= st)
                out_selected[count++] = ci;
            continue;
        }

        const crd::f32 max_err = bits_to_f32(w[4]);
        const crd::f32 min_pe  = bits_to_f32(w[5]);
        const crd::f32 dist_n  = node_distance(w, params.camera_pos);
        const crd::f32 radius  = bits_to_f32(w[3]);
        const crd::f32 min_d   = dist_n > radius ? (dist_n - radius) : 0.0F;
        const crd::f32 max_d   = dist_n + radius;

        if (max_err <= tp * min_d && min_pe > tp * max_d)
        {
            collect_leaves(packed_bvh, ni, out_selected, count, max_selected);
            continue;
        }

        if (sp + 2U <= kMaxStackDepth)
        {
            stack[sp++] = right;
            stack[sp++] = left;
        }
    }
    return count;
}

} // namespace crd::geometry::mesh_processing
