#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/geometry/mesh_processing/cluster_dag_cook.hpp>
#include <crd/geometry/mesh_processing/cluster_select.hpp>
#include <crd/geometry/mesh_processing/cluster_unpack.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace mp = crd::geometry::mesh_processing;

static void make_sphere(crd::u32 slices, crd::u32 stacks,
                        crd::containers::Array<crd::f32>& pos,
                        crd::containers::Array<crd::u32>& idx)
{
    pos.clear();
    idx.clear();
    constexpr crd::f32 pi = 3.14159265358979323846F;
    for (crd::u32 j = 0; j <= stacks; ++j)
    {
        const crd::f32 phi = pi * static_cast<crd::f32>(j) / static_cast<crd::f32>(stacks);
        const crd::f32 sp  = std::sin(phi);
        const crd::f32 cp  = std::cos(phi);
        for (crd::u32 i = 0; i <= slices; ++i)
        {
            const crd::f32 theta = 2.0F * pi * static_cast<crd::f32>(i) / static_cast<crd::f32>(slices);
            pos.push_back(sp * std::cos(theta));
            pos.push_back(cp);
            pos.push_back(sp * std::sin(theta));
        }
    }
    const crd::u32 row = slices + 1U;
    for (crd::u32 j = 0; j < stacks; ++j)
    {
        for (crd::u32 i = 0; i < slices; ++i)
        {
            const crd::u32 v0 = j * row + i;
            idx.push_back(v0);
            idx.push_back(v0 + row);
            idx.push_back(v0 + row + 1U);
            idx.push_back(v0);
            idx.push_back(v0 + row + 1U);
            idx.push_back(v0 + 1U);
        }
    }
}

TEST_CASE("REN-40-I8: full pipeline - close-range recovers original",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);
    const crd::u32 original_tris = static_cast<crd::u32>(idx.size() / 3U);

    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult cook(&alloc);
    const auto rep = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, cook, &alloc);
    REQUIRE(rep.status == mp::ClusterDagCookStatus::Ok);

    mp::ClusterSelectParams params;
    params.error_threshold = 0.0001F;
    params.proj_factor     = 0.0001F;
    params.camera_pos[2]   = 2.0F;

    crd::containers::Array<crd::u32> sel(&alloc);
    sel.resize(rep.cluster_count);
    const crd::u32 n = mp::select_clusters_flat(
        cook.packed_clusters.data(), rep.cluster_count, params, sel.data(), rep.cluster_count);

    mp::ClusterUnpackResult out(&alloc);
    mp::unpack_selected_clusters(
        cook.packed_clusters.data(), cook.cluster_vertices.data(),
        cook.cluster_triangles_packed.data(), cook.positions.data(),
        sel.data(), n, out);

    REQUIRE(out.triangle_count == original_tris);
}

TEST_CASE("REN-40-I8: far-range selects fewer triangles than close",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult cook(&alloc);
    const auto rep = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, cook, &alloc);
    REQUIRE(rep.status == mp::ClusterDagCookStatus::Ok);

    auto count_tris = [&](crd::f32 thresh, crd::f32 pf, crd::f32 cam_z) -> crd::u32
    {
        mp::ClusterSelectParams p;
        p.error_threshold = thresh;
        p.proj_factor     = pf;
        p.camera_pos[2]   = cam_z;

        crd::containers::Array<crd::u32> s(&alloc);
        s.resize(cook.cluster_count);
        const crd::u32 n = mp::select_clusters_flat(
            cook.packed_clusters.data(), cook.cluster_count, p, s.data(), cook.cluster_count);

        mp::ClusterUnpackResult r(&alloc);
        mp::unpack_selected_clusters(
            cook.packed_clusters.data(), cook.cluster_vertices.data(),
            cook.cluster_triangles_packed.data(), cook.positions.data(),
            s.data(), n, r);
        return r.triangle_count;
    };

    const crd::u32 close_tris = count_tris(0.0001F, 0.0001F, 2.0F);
    const crd::u32 far_tris   = count_tris(1.0F,    10.0F,   100.0F);

    REQUIRE(close_tris > far_tris);
    REQUIRE(far_tris > 0U);
}

TEST_CASE("REN-40-I8: exactly one LOD level covers the entire mesh",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult cook(&alloc);
    const auto rep = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, cook, &alloc);
    REQUIRE(rep.level_count > 1U);

    mp::ClusterSelectParams params;
    params.error_threshold = 0.0001F;
    params.proj_factor     = 0.0001F;
    params.camera_pos[2]   = 2.0F;

    crd::containers::Array<crd::u32> sel(&alloc);
    sel.resize(rep.cluster_count);
    const crd::u32 n = mp::select_clusters_flat(
        cook.packed_clusters.data(), rep.cluster_count, params, sel.data(), rep.cluster_count);
    REQUIRE(n > 0U);

    crd::u32 levels_seen = 0U;
    crd::u32 first_level = 0xFFFFFFFFU;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32* w = cook.packed_clusters.data()
                          + static_cast<crd::usize>(sel[i]) * mp::kClusterGpuWords;
        const crd::u32 lvl = (w[2] >> 16U) & 0xFFFFU;
        if (first_level == 0xFFFFFFFFU) first_level = lvl;
        if (lvl != first_level) ++levels_seen;
    }
    REQUIRE(levels_seen == 0U);
}

TEST_CASE("REN-40-I8: no cluster is selected twice",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult cook(&alloc);
    const auto rep = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, cook, &alloc);

    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.01F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> sel(&alloc);
    sel.resize(rep.cluster_count);
    const crd::u32 n = mp::select_clusters_flat(
        cook.packed_clusters.data(), rep.cluster_count, params, sel.data(), rep.cluster_count);
    REQUIRE(n > 0U);

    crd::containers::Array<crd::u32> sorted(&alloc);
    sorted.resize(n);
    for (crd::u32 i = 0; i < n; ++i) sorted[i] = sel[i];
    crd::containers::sort(sorted.data(), sorted.data() + n);
    for (crd::u32 i = 1; i < n; ++i)
        REQUIRE(sorted[i] != sorted[i - 1U]);
}

TEST_CASE("REN-40-I8: BVH selection matches flat for all test distances",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult cook(&alloc);
    const auto rep = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, cook, &alloc);

    const crd::f32 distances[] = {2.0F, 5.0F, 10.0F, 50.0F, 200.0F};
    for (crd::f32 d : distances)
    {
        mp::ClusterSelectParams params;
        params.error_threshold = 1.0F;
        params.proj_factor     = 0.005F;
        params.camera_pos[2]   = d;

        crd::containers::Array<crd::u32> sf(&alloc);
        crd::containers::Array<crd::u32> sb(&alloc);
        sf.resize(rep.cluster_count);
        sb.resize(rep.cluster_count);

        const crd::u32 nf = mp::select_clusters_flat(
            cook.packed_clusters.data(), rep.cluster_count, params,
            sf.data(), rep.cluster_count);
        const crd::u32 nb = mp::select_clusters_bvh(
            cook.packed_clusters.data(), rep.cluster_count,
            cook.packed_bvh.data(), cook.bvh_node_count, params,
            sb.data(), rep.cluster_count, &alloc);

        REQUIRE(nf == nb);
        crd::containers::sort(sf.data(), sf.data() + nf);
        crd::containers::sort(sb.data(), sb.data() + nb);
        for (crd::u32 i = 0; i < nf; ++i)
            REQUIRE(sf[i] == sb[i]);
    }
}

TEST_CASE("REN-40-I8: monotone LOD reduction with distance",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult cook(&alloc);
    const auto rep = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, cook, &alloc);
    REQUIRE(rep.status == mp::ClusterDagCookStatus::Ok);

    crd::u32 prev_tris = 0xFFFFFFFFU;
    const crd::f32 distances[] = {2.0F, 5.0F, 15.0F, 50.0F, 200.0F};
    for (crd::f32 d : distances)
    {
        mp::ClusterSelectParams params;
        params.error_threshold = 1.0F;
        params.proj_factor     = 0.01F;
        params.camera_pos[2]   = d;

        crd::containers::Array<crd::u32> sel(&alloc);
        sel.resize(cook.cluster_count);
        const crd::u32 n = mp::select_clusters_flat(
            cook.packed_clusters.data(), cook.cluster_count, params,
            sel.data(), cook.cluster_count);

        mp::ClusterUnpackResult out(&alloc);
        mp::unpack_selected_clusters(
            cook.packed_clusters.data(), cook.cluster_vertices.data(),
            cook.cluster_triangles_packed.data(), cook.positions.data(),
            sel.data(), n, out);

        REQUIRE(out.triangle_count > 0U);
        REQUIRE(out.triangle_count <= prev_tris);
        prev_tris = out.triangle_count;
    }
}

TEST_CASE("REN-40-I8: cook-select-unpack determinism end to end",
          "[geometry][mesh-processing][gate][ren40]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    mp::DagBuildOptions opts;

    mp::ClusterDagCookResult c1(&alloc);
    mp::ClusterDagCookResult c2(&alloc);
    const auto rep1 = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, c1, &alloc);
    REQUIRE(rep1.status == mp::ClusterDagCookStatus::Ok);
    const auto rep2 = mp::cook_cluster_dag(pos.data(),
        static_cast<crd::u32>(pos.size() / 3U), idx.data(),
        static_cast<crd::u32>(idx.size()), opts, c2, &alloc);
    REQUIRE(rep2.status == mp::ClusterDagCookStatus::Ok);

    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.01F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> s1(&alloc);
    crd::containers::Array<crd::u32> s2(&alloc);
    s1.resize(c1.cluster_count);
    s2.resize(c2.cluster_count);
    const crd::u32 n1 = mp::select_clusters_flat(
        c1.packed_clusters.data(), c1.cluster_count, params, s1.data(), c1.cluster_count);
    const crd::u32 n2 = mp::select_clusters_flat(
        c2.packed_clusters.data(), c2.cluster_count, params, s2.data(), c2.cluster_count);
    REQUIRE(n1 == n2);

    mp::ClusterUnpackResult r1(&alloc);
    mp::ClusterUnpackResult r2(&alloc);
    mp::unpack_selected_clusters(
        c1.packed_clusters.data(), c1.cluster_vertices.data(),
        c1.cluster_triangles_packed.data(), c1.positions.data(),
        s1.data(), n1, r1);
    mp::unpack_selected_clusters(
        c2.packed_clusters.data(), c2.cluster_vertices.data(),
        c2.cluster_triangles_packed.data(), c2.positions.data(),
        s2.data(), n2, r2);

    REQUIRE(r1.triangle_count == r2.triangle_count);
    REQUIRE(r1.vertex_count == r2.vertex_count);
    for (crd::usize i = 0; i < r1.triangles.size(); ++i)
        REQUIRE(r1.triangles[i] == r2.triangles[i]);
    for (crd::usize i = 0; i < r1.positions.size(); ++i)
        REQUIRE(r1.positions[i] == r2.positions[i]);
}
