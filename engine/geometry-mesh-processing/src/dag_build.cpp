#include <crd/geometry/mesh_processing/dag_build.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/cluster_group.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/meshlet_build.hpp>
#include <crd/geometry/mesh_processing/qem_decimate.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/select.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::mesh_processing
{

namespace
{

using V3 = crd::math::Vec3<crd::f32>;

void compute_cluster_bounds(const crd::f32* positions, const crd::u32* verts, crd::u32 vc,
                            crd::f32 (&center)[3], crd::f32& radius)
{
    if (vc == 0U)
    {
        center[0] = center[1] = center[2] = 0.0F;
        radius = 0.0F;
        return;
    }
    crd::f64 cx = 0.0;
    crd::f64 cy = 0.0;
    crd::f64 cz = 0.0;
    for (crd::u32 i = 0; i < vc; ++i)
    {
        const crd::u32 v = verts[i];
        cx += static_cast<crd::f64>(positions[v * 3U + 0U]);
        cy += static_cast<crd::f64>(positions[v * 3U + 1U]);
        cz += static_cast<crd::f64>(positions[v * 3U + 2U]);
    }
    const crd::f64 inv = 1.0 / static_cast<crd::f64>(vc);
    center[0] = static_cast<crd::f32>(cx * inv);
    center[1] = static_cast<crd::f32>(cy * inv);
    center[2] = static_cast<crd::f32>(cz * inv);

    crd::f32 max_r2 = 0.0F;
    for (crd::u32 i = 0; i < vc; ++i)
    {
        const crd::u32 v  = verts[i];
        const crd::f32 dx = positions[v * 3U + 0U] - center[0];
        const crd::f32 dy = positions[v * 3U + 1U] - center[1];
        const crd::f32 dz = positions[v * 3U + 2U] - center[2];
        const crd::f32 r2 = dx * dx + dy * dy + dz * dz;
        if (r2 > max_r2) max_r2 = r2;
    }
    radius = crd::math::sqrt(max_r2);
}

crd::f32 compute_group_error(const crd::containers::Array<V3>& orig_pos,
                             const crd::containers::Array<V3>& sim_pos)
{
    crd::f32 max_dist = 0.0F;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(orig_pos.size()); ++i)
    {
        crd::f32 min_d2 = std::numeric_limits<crd::f32>::max();
        for (crd::u32 j = 0; j < static_cast<crd::u32>(sim_pos.size()); ++j)
        {
            const crd::f32 dx = orig_pos[i].x - sim_pos[j].x;
            const crd::f32 dy = orig_pos[i].y - sim_pos[j].y;
            const crd::f32 dz = orig_pos[i].z - sim_pos[j].z;
            const crd::f32 d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < min_d2) min_d2 = d2;
        }
        const crd::f32 d = crd::math::sqrt(min_d2);
        if (d > max_dist) max_dist = d;
    }
    return max_dist;
}

} // namespace

DagBuildReport build_cluster_dag(const crd::f32* positions, crd::u32 vertex_count, const crd::u32* indices,
                                 crd::u32 index_count, const DagBuildOptions& opts, DagBuildResult& out,
                                 crd::memory::IAllocator* scratch)
{
    DagBuildReport report;
    if (vertex_count == 0U || index_count == 0U || (index_count % 3U) != 0U)
    {
        report.status = DagBuildStatus::EmptyMesh;
        return report;
    }

    // ── Copy positions ────────────────────────────────────────────────────
    out.positions.resize(static_cast<crd::usize>(vertex_count) * 3U);
    for (crd::u32 i = 0; i < vertex_count * 3U; ++i) out.positions[i] = positions[i];
    crd::u32 total_verts = vertex_count;

    // ── Level 0: build meshlets ───────────────────────────────────────────
    MeshletBuildOptions mopts;
    MeshletBuildResult  mresult(scratch);
    const auto          mrep = build_meshlets(positions, vertex_count, indices, index_count, mopts, mresult, scratch);
    if (mrep.status != MeshletBuildStatus::Ok)
    {
        report.status = DagBuildStatus::MeshletBuildFailed;
        return report;
    }

    // Emit leaf clusters
    const crd::u32 leaf_count = static_cast<crd::u32>(mresult.meshlets.size());
    for (crd::u32 mi = 0; mi < leaf_count; ++mi)
    {
        const auto& ml = mresult.meshlets[mi];
        DagCluster  c;
        c.vertex_offset   = static_cast<crd::u32>(out.cluster_vertices.size());
        c.triangle_offset = static_cast<crd::u32>(out.cluster_triangles.size());
        c.vertex_count    = ml.vertex_count;
        c.triangle_count  = ml.triangle_count;
        c.error           = 0.0F;
        c.parent_error    = 0.0F;
        c.level           = 0U;

        for (crd::u32 vi = 0; vi < ml.vertex_count; ++vi)
            out.cluster_vertices.push_back(mresult.meshlet_vertices[ml.vertex_offset + vi]);
        for (crd::u32 ti = 0; ti < static_cast<crd::u32>(ml.triangle_count) * 3U; ++ti)
            out.cluster_triangles.push_back(mresult.meshlet_triangles[ml.triangle_offset + ti]);

        compute_cluster_bounds(out.positions.data(), out.cluster_vertices.data() + c.vertex_offset, c.vertex_count,
                               c.center, c.radius);
        out.clusters.push_back(c);
    }

    out.leaf_count = leaf_count;

    // ── Iterative DAG building ────────────────────────────────────────────
    crd::containers::Array<crd::u32> cur_level(scratch);
    for (crd::u32 i = 0; i < leaf_count; ++i) cur_level.push_back(i);

    crd::u32 level = 0U;

    while (static_cast<crd::u32>(cur_level.size()) > 1U)
    {
        // Build a temporary MeshletBuildResult for the current level
        MeshletBuildResult tmp_ml(scratch);
        for (crd::u32 ci = 0; ci < static_cast<crd::u32>(cur_level.size()); ++ci)
        {
            const auto& dc = out.clusters[cur_level[ci]];
            Meshlet     m;
            m.vertex_offset   = static_cast<crd::u32>(tmp_ml.meshlet_vertices.size());
            m.triangle_offset = static_cast<crd::u32>(tmp_ml.meshlet_triangles.size());
            m.vertex_count    = dc.vertex_count;
            m.triangle_count  = dc.triangle_count;
            tmp_ml.meshlets.push_back(m);
            for (crd::u32 vi = 0; vi < dc.vertex_count; ++vi)
                tmp_ml.meshlet_vertices.push_back(out.cluster_vertices[dc.vertex_offset + vi]);
            for (crd::u32 ti = 0; ti < static_cast<crd::u32>(dc.triangle_count) * 3U; ++ti)
                tmp_ml.meshlet_triangles.push_back(out.cluster_triangles[dc.triangle_offset + ti]);
        }
        tmp_ml.total_triangles = 0U;
        tmp_ml.total_vertices  = total_verts;

        // Group
        ClusterGroupOptions gopts;
        gopts.target_group_size = opts.group_size > 0U ? opts.group_size : 4U;
        ClusterGroupResult gresult(scratch);
        const auto         grep = group_meshlets(tmp_ml, total_verts, gopts, gresult, scratch);
        if (grep.status != ClusterGroupStatus::Ok || grep.group_count == 0U) break;

        crd::containers::Array<crd::u32> next_level(scratch);
        bool any_simplified = false;

        for (crd::u32 gi = 0; gi < grep.group_count; ++gi)
        {
            const auto& grp = gresult.groups[gi];

            // ── Merge group's meshlets into local mesh ────────────────
            crd::containers::Array<crd::u32> local_remap(scratch);
            local_remap.resize(total_verts);
            constexpr crd::u32 unmapped = ~0U;
            for (crd::u32 i = 0; i < total_verts; ++i) local_remap[i] = unmapped;

            crd::containers::Array<V3>       local_pos(scratch);
            crd::containers::Array<crd::u32> local_idx(scratch);

            for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
            {
                const crd::u32 mi_in_tmp = gresult.group_meshlets[grp.first + mi_local];
                const crd::u32 ci        = cur_level[mi_in_tmp];
                const auto&    dc        = out.clusters[ci];

                for (crd::u32 ti = 0; ti < dc.triangle_count; ++ti)
                {
                    for (crd::u32 k = 0; k < 3U; ++k)
                    {
                        const crd::u8  li = out.cluster_triangles[dc.triangle_offset + ti * 3U + k];
                        const crd::u32 gv = out.cluster_vertices[dc.vertex_offset + li];
                        if (local_remap[gv] == unmapped)
                        {
                            local_remap[gv] = static_cast<crd::u32>(local_pos.size());
                            V3 p;
                            p.x = out.positions[gv * 3U + 0U];
                            p.y = out.positions[gv * 3U + 1U];
                            p.z = out.positions[gv * 3U + 2U];
                            local_pos.push_back(p);
                        }
                        local_idx.push_back(local_remap[gv]);
                    }
                }
            }

            if (local_idx.size() < 3U)
            {
                for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
                    next_level.push_back(cur_level[gresult.group_meshlets[grp.first + mi_local]]);
                continue;
            }

            // ── Remap boundary vertices to local indices ──────────────
            const crd::u32 bv_beg = gresult.boundary_offsets[gi];
            const crd::u32 bv_end = gresult.boundary_offsets[gi + 1U];
            crd::containers::Array<crd::u32> locked(scratch);
            for (crd::u32 bi = bv_beg; bi < bv_end; ++bi)
            {
                const crd::u32 gv = gresult.boundary_vertices[bi];
                if (gv < total_verts && local_remap[gv] != unmapped) locked.push_back(local_remap[gv]);
            }

            // ── QEM decimate ──────────────────────────────────────────
            const crd::u32 local_tc = static_cast<crd::u32>(local_idx.size()) / 3U;

            HalfEdgeMesh<crd::f32> he_mesh(scratch);
            const auto build_st = he_mesh.build_from(
                crd::containers::ConstSpan<V3>{local_pos.data(), local_pos.size()},
                crd::containers::ConstSpan<crd::u32>{local_idx.data(), local_idx.size()});

            if (build_st != BuildStatus::Ok || !he_mesh.is_manifold())
            {
                for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
                    next_level.push_back(cur_level[gresult.group_meshlets[grp.first + mi_local]]);
                continue;
            }

            crd::u32 target = static_cast<crd::u32>(
                static_cast<crd::f32>(local_tc) * (opts.simplify_ratio > 0.0F ? opts.simplify_ratio : 0.5F));
            if (target < 4U) target = 4U;
            if (target >= local_tc)
            {
                for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
                    next_level.push_back(cur_level[gresult.group_meshlets[grp.first + mi_local]]);
                continue;
            }

            QemDecimateOptions<crd::f32> qopts;
            qopts.target_face_count = target;
            qopts.boundary_weight   = opts.boundary_weight;
            qopts.output_allocator  = scratch;
            qopts.locked_vertices = crd::containers::ConstSpan<crd::u32>{locked.data(), locked.size()};

            QemDecimateReport qrep;
            auto              simplified = qem_decimate(he_mesh, qopts, &qrep);

            crd::containers::Array<V3>       sim_pos(scratch);
            crd::containers::Array<crd::u32> sim_idx(scratch);
            simplified.to_indexed(sim_pos, sim_idx);

            if (sim_pos.empty() || sim_idx.empty() || sim_idx.size() / 3U >= local_idx.size() / 3U)
            {
                for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
                    next_level.push_back(cur_level[gresult.group_meshlets[grp.first + mi_local]]);
                continue;
            }

            any_simplified = true;

            // ── Compute group error ───────────────────────────────────
            crd::f32 max_child_error = 0.0F;
            for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
            {
                const crd::f32 ce = out.clusters[cur_level[gresult.group_meshlets[grp.first + mi_local]]].error;
                if (ce > max_child_error) max_child_error = ce;
            }
            const crd::f32 simplify_error = compute_group_error(local_pos, sim_pos);
            const crd::f32 group_error    = max_child_error + simplify_error;

            // ── Append positions ───────────────────────────────────────
            const crd::u32 base_vert = total_verts;
            for (crd::u32 vi = 0; vi < static_cast<crd::u32>(sim_pos.size()); ++vi)
            {
                out.positions.push_back(sim_pos[vi].x);
                out.positions.push_back(sim_pos[vi].y);
                out.positions.push_back(sim_pos[vi].z);
            }
            total_verts += static_cast<crd::u32>(sim_pos.size());

            // ── Re-meshletize ─────────────────────────────────────────
            crd::containers::Array<crd::f32> sim_pos_flat(scratch);
            sim_pos_flat.reserve(sim_pos.size() * 3U);
            for (crd::u32 vi = 0; vi < static_cast<crd::u32>(sim_pos.size()); ++vi)
            {
                sim_pos_flat.push_back(sim_pos[vi].x);
                sim_pos_flat.push_back(sim_pos[vi].y);
                sim_pos_flat.push_back(sim_pos[vi].z);
            }

            MeshletBuildOptions remopts;
            MeshletBuildResult  remresult(scratch);
            (void)build_meshlets(sim_pos_flat.data(), static_cast<crd::u32>(sim_pos.size()), sim_idx.data(),
                                 static_cast<crd::u32>(sim_idx.size()), remopts, remresult, scratch);

            // ── Emit parent clusters ──────────────────────────────────
            for (crd::u32 nmi = 0; nmi < static_cast<crd::u32>(remresult.meshlets.size()); ++nmi)
            {
                const auto& nm = remresult.meshlets[nmi];
                DagCluster  c;
                c.vertex_offset   = static_cast<crd::u32>(out.cluster_vertices.size());
                c.triangle_offset = static_cast<crd::u32>(out.cluster_triangles.size());
                c.vertex_count    = nm.vertex_count;
                c.triangle_count  = nm.triangle_count;
                c.error           = group_error;
                c.parent_error    = 0.0F;
                c.level           = level + 1U;

                for (crd::u32 vi = 0; vi < nm.vertex_count; ++vi)
                    out.cluster_vertices.push_back(base_vert + remresult.meshlet_vertices[nm.vertex_offset + vi]);
                for (crd::u32 ti = 0; ti < static_cast<crd::u32>(nm.triangle_count) * 3U; ++ti)
                    out.cluster_triangles.push_back(remresult.meshlet_triangles[nm.triangle_offset + ti]);

                compute_cluster_bounds(out.positions.data(), out.cluster_vertices.data() + c.vertex_offset,
                                       c.vertex_count, c.center, c.radius);

                next_level.push_back(static_cast<crd::u32>(out.clusters.size()));
                out.clusters.push_back(c);
            }

            // ── Set parent_error on children ──────────────────────────
            for (crd::u32 mi_local = 0; mi_local < grp.count; ++mi_local)
            {
                const crd::u32 ci = cur_level[gresult.group_meshlets[grp.first + mi_local]];
                out.clusters[ci].parent_error = group_error;
            }
        }

        if (!any_simplified) break;

        cur_level.clear();
        for (crd::u32 i = 0; i < static_cast<crd::u32>(next_level.size()); ++i)
            cur_level.push_back(next_level[i]);
        ++level;
    }

    // ── Root clusters: parent_error = FLT_MAX ─────────────────────────────
    for (crd::u32 i = 0; i < static_cast<crd::u32>(cur_level.size()); ++i)
        out.clusters[cur_level[i]].parent_error = std::numeric_limits<crd::f32>::max();

    out.level_count      = level + 1U;
    report.level_count   = out.level_count;
    report.cluster_count = static_cast<crd::u32>(out.clusters.size());
    report.leaf_count    = leaf_count;
    report.status        = DagBuildStatus::Ok;
    return report;
}

} // namespace crd::geometry::mesh_processing
