#include <crd/geometry/mesh_processing/cluster_group.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

namespace
{

struct RawEdge
{
    crd::u32 m0, m1;
};

struct WeightedEdge
{
    crd::u32 m0, m1, weight;
};

} // namespace

ClusterGroupReport group_meshlets(const MeshletBuildResult& meshlets, crd::u32 vertex_count,
                                  const ClusterGroupOptions& opts, ClusterGroupResult& out,
                                  crd::memory::IAllocator* scratch)
{
    ClusterGroupReport report;
    const crd::u32     mc = static_cast<crd::u32>(meshlets.meshlets.size());

    if (mc == 0U)
    {
        report.status = ClusterGroupStatus::EmptyInput;
        return report;
    }

    const crd::u32 tgs = opts.target_group_size > 0U ? opts.target_group_size : 4U;

    // ── Step 1: vertex → meshlet CSR ──────────────────────────────────────
    crd::containers::Array<crd::u32> vtm_count(scratch);
    vtm_count.resize(vertex_count);
    for (crd::u32 i = 0; i < vertex_count; ++i) vtm_count[i] = 0U;

    for (crd::u32 m = 0; m < mc; ++m)
    {
        const auto& ml = meshlets.meshlets[m];
        for (crd::u32 vi = 0; vi < ml.vertex_count; ++vi)
            vtm_count[meshlets.meshlet_vertices[ml.vertex_offset + vi]]++;
    }

    crd::containers::Array<crd::u32> vtm_off(scratch);
    vtm_off.resize(vertex_count + 1U);
    vtm_off[0] = 0U;
    for (crd::u32 i = 0; i < vertex_count; ++i)
        vtm_off[i + 1U] = vtm_off[i] + vtm_count[i];

    crd::containers::Array<crd::u32> vtm_data(scratch);
    vtm_data.resize(vtm_off[vertex_count]);
    for (crd::u32 i = 0; i < vertex_count; ++i) vtm_count[i] = 0U;

    for (crd::u32 m = 0; m < mc; ++m)
    {
        const auto& ml = meshlets.meshlets[m];
        for (crd::u32 vi = 0; vi < ml.vertex_count; ++vi)
        {
            const crd::u32 gv = meshlets.meshlet_vertices[ml.vertex_offset + vi];
            vtm_data[vtm_off[gv] + vtm_count[gv]] = m;
            vtm_count[gv]++;
        }
    }

    // ── Step 2: meshlet adjacency from shared vertices ────────────────────
    crd::containers::Array<RawEdge> raw_edges(scratch);
    for (crd::u32 v = 0; v < vertex_count; ++v)
    {
        const crd::u32 beg = vtm_off[v];
        const crd::u32 end = vtm_off[v + 1U];
        for (crd::u32 i = beg; i < end; ++i)
        {
            for (crd::u32 j = i + 1U; j < end; ++j)
            {
                crd::u32 mi = vtm_data[i];
                crd::u32 mj = vtm_data[j];
                if (mi > mj)
                {
                    const crd::u32 tmp = mi;
                    mi = mj;
                    mj = tmp;
                }
                RawEdge e;
                e.m0 = mi;
                e.m1 = mj;
                raw_edges.push_back(e);
            }
        }
    }

    crd::containers::sort(raw_edges.begin(), raw_edges.end(),
                          [](const RawEdge& a, const RawEdge& b) {
                              if (a.m0 != b.m0) return a.m0 < b.m0;
                              return a.m1 < b.m1;
                          });

    // Merge duplicates → weighted
    crd::containers::Array<WeightedEdge> wedges(scratch);
    const crd::u32 ne = static_cast<crd::u32>(raw_edges.size());
    if (ne > 0U)
    {
        WeightedEdge cur;
        cur.m0 = raw_edges[0].m0;
        cur.m1 = raw_edges[0].m1;
        cur.weight = 1U;
        for (crd::u32 i = 1U; i < ne; ++i)
        {
            if (raw_edges[i].m0 == cur.m0 && raw_edges[i].m1 == cur.m1)
            {
                cur.weight++;
            }
            else
            {
                wedges.push_back(cur);
                cur.m0 = raw_edges[i].m0;
                cur.m1 = raw_edges[i].m1;
                cur.weight = 1U;
            }
        }
        wedges.push_back(cur);
    }

    // Build bidirectional CSR adjacency
    const crd::u32 we_count = static_cast<crd::u32>(wedges.size());

    crd::containers::Array<crd::u32> adj_count(scratch);
    adj_count.resize(mc);
    for (crd::u32 i = 0; i < mc; ++i) adj_count[i] = 0U;
    for (crd::u32 i = 0; i < we_count; ++i)
    {
        adj_count[wedges[i].m0]++;
        adj_count[wedges[i].m1]++;
    }

    out.adjacency.offsets.resize(mc + 1U);
    out.adjacency.offsets[0] = 0U;
    for (crd::u32 i = 0; i < mc; ++i)
        out.adjacency.offsets[i + 1U] = out.adjacency.offsets[i] + adj_count[i];

    const crd::u32 adj_total = out.adjacency.offsets[mc];
    out.adjacency.neighbors.resize(adj_total);
    out.adjacency.weights.resize(adj_total);

    for (crd::u32 i = 0; i < mc; ++i) adj_count[i] = 0U;
    for (crd::u32 i = 0; i < we_count; ++i)
    {
        const crd::u32 a = wedges[i].m0;
        const crd::u32 b = wedges[i].m1;
        const crd::u32 w = wedges[i].weight;

        crd::u32 ia               = out.adjacency.offsets[a] + adj_count[a];
        out.adjacency.neighbors[ia] = b;
        out.adjacency.weights[ia]   = w;
        adj_count[a]++;

        crd::u32 ib               = out.adjacency.offsets[b] + adj_count[b];
        out.adjacency.neighbors[ib] = a;
        out.adjacency.weights[ib]   = w;
        adj_count[b]++;
    }

    // ── Step 3: greedy graph partitioning ─────────────────────────────────
    crd::containers::Array<crd::u32> meshlet_group(scratch);
    meshlet_group.resize(mc);
    constexpr crd::u32 no_group = ~0U;
    for (crd::u32 i = 0; i < mc; ++i) meshlet_group[i] = no_group;

    crd::u32                         group_idx = 0U;
    crd::containers::Array<crd::u32> cur_group(scratch);

    for (crd::u32 seed = 0U; seed < mc; ++seed)
    {
        if (meshlet_group[seed] != no_group) continue;

        cur_group.clear();
        cur_group.push_back(seed);
        meshlet_group[seed] = group_idx;

        while (static_cast<crd::u32>(cur_group.size()) < tgs)
        {
            crd::u32 best_m     = no_group;
            crd::u32 best_score = 0U;

            for (crd::u32 gi = 0; gi < static_cast<crd::u32>(cur_group.size()); ++gi)
            {
                const crd::u32 gm       = cur_group[gi];
                const crd::u32 adj_beg  = out.adjacency.offsets[gm];
                const crd::u32 adj_end  = out.adjacency.offsets[gm + 1U];
                for (crd::u32 ai = adj_beg; ai < adj_end; ++ai)
                {
                    const crd::u32 nb = out.adjacency.neighbors[ai];
                    if (meshlet_group[nb] != no_group) continue;

                    crd::u32 score = 0U;
                    for (crd::u32 gj = 0; gj < static_cast<crd::u32>(cur_group.size()); ++gj)
                    {
                        const crd::u32 gm2 = cur_group[gj];
                        const crd::u32 b2  = out.adjacency.offsets[gm2];
                        const crd::u32 e2  = out.adjacency.offsets[gm2 + 1U];
                        for (crd::u32 ai2 = b2; ai2 < e2; ++ai2)
                        {
                            if (out.adjacency.neighbors[ai2] == nb)
                                score += out.adjacency.weights[ai2];
                        }
                    }

                    if (score > best_score || (score == best_score && (best_m == no_group || nb < best_m)))
                    {
                        best_m     = nb;
                        best_score = score;
                    }
                }
            }

            if (best_m == no_group) break;
            cur_group.push_back(best_m);
            meshlet_group[best_m] = group_idx;
        }

        ClusterGroup g;
        g.first = static_cast<crd::u32>(out.group_meshlets.size());
        g.count = static_cast<crd::u32>(cur_group.size());
        out.groups.push_back(g);
        for (crd::u32 gi = 0; gi < static_cast<crd::u32>(cur_group.size()); ++gi)
            out.group_meshlets.push_back(cur_group[gi]);

        ++group_idx;
    }

    // ── Step 4: boundary vertices per group ───────────────────────────────
    out.boundary_offsets.resize(group_idx + 1U);
    out.boundary_offsets[0] = 0U;

    crd::containers::Array<crd::u32> bv_gen(scratch);
    bv_gen.resize(vertex_count);
    for (crd::u32 i = 0; i < vertex_count; ++i) bv_gen[i] = no_group;

    for (crd::u32 g = 0; g < group_idx; ++g)
    {
        const ClusterGroup& grp = out.groups[g];
        for (crd::u32 gi = 0; gi < grp.count; ++gi)
        {
            const crd::u32 m  = out.group_meshlets[grp.first + gi];
            const auto&    ml = meshlets.meshlets[m];
            for (crd::u32 vi = 0; vi < ml.vertex_count; ++vi)
            {
                const crd::u32 gv = meshlets.meshlet_vertices[ml.vertex_offset + vi];
                if (bv_gen[gv] == g) continue;

                const crd::u32 vb = vtm_off[gv];
                const crd::u32 ve = vtm_off[gv + 1U];
                bool is_boundary = false;
                for (crd::u32 vi2 = vb; vi2 < ve; ++vi2)
                {
                    if (meshlet_group[vtm_data[vi2]] != g)
                    {
                        is_boundary = true;
                        break;
                    }
                }
                bv_gen[gv] = g;
                if (is_boundary) out.boundary_vertices.push_back(gv);
            }
        }
        out.boundary_offsets[g + 1U] = static_cast<crd::u32>(out.boundary_vertices.size());
    }

    // ── Report ────────────────────────────────────────────────────────────
    report.group_count = group_idx;
    if (group_idx > 0U)
    {
        crd::u32 total = 0U;
        for (crd::u32 g = 0; g < group_idx; ++g) total += out.groups[g].count;
        report.avg_group_size_x10 = (total * 10U + group_idx / 2U) / group_idx;
    }
    report.boundary_vertex_count = static_cast<crd::u32>(out.boundary_vertices.size());
    report.status                = ClusterGroupStatus::Ok;
    return report;
}

} // namespace crd::geometry::mesh_processing
