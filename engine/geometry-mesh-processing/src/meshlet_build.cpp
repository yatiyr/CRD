#include <crd/geometry/mesh_processing/meshlet_build.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::mesh_processing
{

namespace
{

struct TriAdj
{
    crd::u32 neighbours[3];
    static constexpr crd::u32 kNone = ~0U;
};

void build_adjacency(const crd::u32* indices, crd::u32 tri_count, crd::u32 /*vertex_count*/,
                     crd::containers::Array<TriAdj>& adj, crd::memory::IAllocator* scratch)
{
    adj.resize(tri_count);
    for (crd::u32 i = 0; i < tri_count; ++i)
    {
        adj[i].neighbours[0] = TriAdj::kNone;
        adj[i].neighbours[1] = TriAdj::kNone;
        adj[i].neighbours[2] = TriAdj::kNone;
    }

    struct EdgeEntry
    {
        crd::u32 v0, v1;
        crd::u32 tri;
        crd::u8  edge;
    };

    const crd::usize edge_count = static_cast<crd::usize>(tri_count) * 3U;
    crd::containers::Array<EdgeEntry> edges(scratch);
    edges.reserve(edge_count);

    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        const crd::u32 i0 = indices[t * 3 + 0];
        const crd::u32 i1 = indices[t * 3 + 1];
        const crd::u32 i2 = indices[t * 3 + 2];
        auto push = [&](crd::u32 a, crd::u32 b, crd::u8 e) {
            EdgeEntry ee;
            ee.v0  = a < b ? a : b;
            ee.v1  = a < b ? b : a;
            ee.tri = t;
            ee.edge = e;
            edges.push_back(ee);
        };
        push(i0, i1, 0);
        push(i1, i2, 1);
        push(i2, i0, 2);
    }

    // Sort edges by (v0, v1) for pairing
    auto less = [](const EdgeEntry& a, const EdgeEntry& b) {
        if (a.v0 != b.v0) return a.v0 < b.v0;
        if (a.v1 != b.v1) return a.v1 < b.v1;
        return a.tri < b.tri;
    };

    // Insertion sort for small counts, otherwise a simple merge sort
    const crd::u32 n = static_cast<crd::u32>(edges.size());
    if (n <= 64U)
    {
        for (crd::u32 i = 1; i < n; ++i)
        {
            EdgeEntry key = edges[i];
            crd::u32  j   = i;
            while (j > 0 && less(key, edges[j - 1]))
            {
                edges[j] = edges[j - 1];
                --j;
            }
            edges[j] = key;
        }
    }
    else
    {
        crd::containers::Array<EdgeEntry> tmp(scratch);
        tmp.resize(n);
        // Bottom-up merge sort
        for (crd::u32 width = 1; width < n; width *= 2U)
        {
            for (crd::u32 lo = 0; lo < n; lo += width * 2U)
            {
                const crd::u32 mid = lo + width < n ? lo + width : n;
                const crd::u32 hi  = lo + width * 2U < n ? lo + width * 2U : n;
                crd::u32       i = lo, j = mid, k = lo;
                while (i < mid && j < hi)
                {
                    if (less(edges[i], edges[j]))
                        tmp[k++] = edges[i++];
                    else
                        tmp[k++] = edges[j++];
                }
                while (i < mid) tmp[k++] = edges[i++];
                while (j < hi)  tmp[k++] = edges[j++];
            }
            for (crd::u32 i = 0; i < n; ++i) edges[i] = tmp[i];
        }
    }

    // Pair matching edges
    for (crd::u32 i = 0; i + 1U < n; ++i)
    {
        const auto& a = edges[i];
        const auto& b = edges[i + 1U];
        if (a.v0 == b.v0 && a.v1 == b.v1 && a.tri != b.tri)
        {
            adj[a.tri].neighbours[a.edge] = b.tri;
            adj[b.tri].neighbours[b.edge] = a.tri;
            ++i;
        }
    }
}

} // namespace

MeshletBuildReport build_meshlets(const crd::f32* /*positions*/, crd::u32 vertex_count,
                                  const crd::u32* indices, crd::u32 index_count,
                                  const MeshletBuildOptions& opts, MeshletBuildResult& out,
                                  crd::memory::IAllocator* scratch)
{
    MeshletBuildReport report;
    const crd::u32     tri_count = index_count / 3U;

    if (index_count == 0U || vertex_count == 0U)
    {
        report.status = MeshletBuildStatus::EmptyMesh;
        return report;
    }
    if (index_count % 3U != 0U)
    {
        report.status = MeshletBuildStatus::NotTriangles;
        return report;
    }
    for (crd::u32 i = 0; i < index_count; ++i)
    {
        if (indices[i] >= vertex_count)
        {
            report.status = MeshletBuildStatus::InvalidIndex;
            return report;
        }
    }

    const crd::u32 max_v = opts.max_vertices > 0U ? opts.max_vertices : kMeshletMaxVertices;
    const crd::u32 max_t = opts.max_triangles > 0U ? opts.max_triangles : kMeshletMaxTriangles;

    crd::containers::Array<TriAdj> adj(scratch);
    build_adjacency(indices, tri_count, vertex_count, adj, scratch);

    crd::containers::Array<crd::u8> tri_used(scratch);
    tri_used.resize(tri_count);
    for (crd::u32 i = 0; i < tri_count; ++i) tri_used[i] = 0U;

    // Per-vertex: which meshlet slot it occupies (reset per meshlet)
    crd::containers::Array<crd::u32> vert_slot(scratch);
    vert_slot.resize(vertex_count);
    constexpr crd::u32 kNoSlot = ~0U;
    for (crd::u32 i = 0; i < vertex_count; ++i) vert_slot[i] = kNoSlot;

    // Frontier: triangles adjacent to current meshlet, scored by vertex reuse
    crd::containers::Array<crd::u32> frontier(scratch);

    // Current meshlet's local data (vertices and triangles collected before emit)
    crd::containers::Array<crd::u32> cur_verts(scratch);
    crd::containers::Array<crd::u8>  cur_tris(scratch);

    crd::u32 meshlet_gen = 0U;
    // Per-vertex generation to avoid resetting the whole vert_slot array each meshlet
    crd::containers::Array<crd::u32> vert_gen(scratch);
    vert_gen.resize(vertex_count);
    for (crd::u32 i = 0; i < vertex_count; ++i) vert_gen[i] = 0U;

    crd::u32 total_tri_refs = 0U;

    for (crd::u32 seed = 0; seed < tri_count; ++seed)
    {
        if (tri_used[seed] != 0U) continue;

        ++meshlet_gen;
        cur_verts.clear();
        cur_tris.clear();
        frontier.clear();

        crd::u32 cur_vc = 0U;
        crd::u32 cur_tc = 0U;

        auto add_tri = [&](crd::u32 t) {
            CRD_ASSERT(tri_used[t] == 0U);
            tri_used[t] = 1U;
            const crd::u32 i0 = indices[t * 3 + 0];
            const crd::u32 i1 = indices[t * 3 + 1];
            const crd::u32 i2 = indices[t * 3 + 2];
            const crd::u32 vv[3] = {i0, i1, i2};
            for (crd::u32 k = 0; k < 3U; ++k)
            {
                const crd::u32 v = vv[k];
                if (vert_gen[v] != meshlet_gen)
                {
                    vert_gen[v]  = meshlet_gen;
                    vert_slot[v] = cur_vc++;
                    cur_verts.push_back(v);
                }
                cur_tris.push_back(static_cast<crd::u8>(vert_slot[v]));
            }
            ++cur_tc;

            for (crd::u32 e = 0; e < 3U; ++e)
            {
                const crd::u32 nb = adj[t].neighbours[e];
                if (nb != TriAdj::kNone && tri_used[nb] == 0U)
                {
                    bool already = false;
                    for (crd::u32 fi = 0; fi < frontier.size(); ++fi)
                    {
                        if (frontier[fi] == nb) { already = true; break; }
                    }
                    if (!already) frontier.push_back(nb);
                }
            }
        };

        add_tri(seed);

        while (cur_tc < max_t && !frontier.empty())
        {
            crd::u32 best_idx  = 0U;
            crd::u32 best_score = 0U;
            crd::u32 best_new_verts = 3U;

            for (crd::u32 fi = 0; fi < frontier.size(); ++fi)
            {
                if (tri_used[frontier[fi]] != 0U)
                {
                    frontier[fi] = frontier[frontier.size() - 1U];
                    frontier.pop_back();
                    --fi;
                    continue;
                }

                const crd::u32 t  = frontier[fi];
                const crd::u32 i0 = indices[t * 3 + 0];
                const crd::u32 i1 = indices[t * 3 + 1];
                const crd::u32 i2 = indices[t * 3 + 2];

                crd::u32 reuse = 0U;
                crd::u32 nv    = 0U;
                if (vert_gen[i0] == meshlet_gen) ++reuse; else ++nv;
                if (vert_gen[i1] == meshlet_gen) ++reuse; else ++nv;
                if (vert_gen[i2] == meshlet_gen) ++reuse; else ++nv;

                if (cur_vc + nv > max_v) continue;

                if (reuse > best_score || (reuse == best_score && nv < best_new_verts))
                {
                    best_idx       = fi;
                    best_score     = reuse;
                    best_new_verts = nv;
                }
            }

            if (best_score == 0U && best_new_verts == 3U)
            {
                bool found = false;
                for (crd::u32 fi = 0; fi < frontier.size(); ++fi)
                {
                    if (tri_used[frontier[fi]] != 0U) continue;
                    const crd::u32 t  = frontier[fi];
                    const crd::u32 i0 = indices[t * 3 + 0];
                    const crd::u32 i1 = indices[t * 3 + 1];
                    const crd::u32 i2 = indices[t * 3 + 2];
                    crd::u32 nv = 0U;
                    if (vert_gen[i0] != meshlet_gen) ++nv;
                    if (vert_gen[i1] != meshlet_gen) ++nv;
                    if (vert_gen[i2] != meshlet_gen) ++nv;
                    if (cur_vc + nv <= max_v)
                    {
                        best_idx = fi;
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            }

            if (best_idx < frontier.size() && tri_used[frontier[best_idx]] == 0U)
            {
                const crd::u32 t = frontier[best_idx];
                frontier[best_idx] = frontier[frontier.size() - 1U];
                frontier.pop_back();
                add_tri(t);
            }
            else
            {
                break;
            }
        }

        // Emit meshlet
        Meshlet m;
        m.vertex_offset   = static_cast<crd::u32>(out.meshlet_vertices.size());
        m.triangle_offset = static_cast<crd::u32>(out.meshlet_triangles.size());
        m.vertex_count    = static_cast<crd::u8>(cur_vc);
        m.triangle_count  = static_cast<crd::u8>(cur_tc);
        out.meshlets.push_back(m);
        for (crd::u32 i = 0; i < cur_verts.size(); ++i)
            out.meshlet_vertices.push_back(cur_verts[i]);
        for (crd::u32 i = 0; i < cur_tris.size(); ++i)
            out.meshlet_triangles.push_back(cur_tris[i]);
        total_tri_refs += cur_tc;
    }

    out.total_triangles = tri_count;
    out.total_vertices  = vertex_count;

    report.meshlet_count  = static_cast<crd::u32>(out.meshlets.size());
    report.triangle_count = tri_count;
    if (tri_count > 0U)
    {
        crd::u32 total_vert_refs = 0U;
        for (crd::u32 i = 0; i < out.meshlets.size(); ++i)
            total_vert_refs += out.meshlets[i].vertex_count;
        report.avg_vertex_reuse = total_vert_refs > 0U
                                      ? static_cast<crd::f32>(tri_count * 3U) /
                                            static_cast<crd::f32>(total_vert_refs)
                                      : 0.0F;
    }

    report.status = MeshletBuildStatus::Ok;
    return report;
}

} // namespace crd::geometry::mesh_processing
