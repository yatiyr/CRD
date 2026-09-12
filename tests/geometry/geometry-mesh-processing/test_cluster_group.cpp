#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/cluster_group.hpp>
#include <crd/geometry/mesh_processing/meshlet_build.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace mp = crd::geometry::mesh_processing;

static void make_grid(crd::u32 n, crd::containers::Array<crd::f32>& pos,
                      crd::containers::Array<crd::u32>& idx)
{
    pos.clear();
    idx.clear();
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            const auto x = static_cast<crd::f32>(i) / static_cast<crd::f32>(n - 1U);
            const auto y = static_cast<crd::f32>(j) / static_cast<crd::f32>(n - 1U);
            pos.push_back(x);
            pos.push_back(y);
            pos.push_back(0.0F);
        }
    }
    for (crd::u32 j = 0; j + 1U < n; ++j)
    {
        for (crd::u32 i = 0; i + 1U < n; ++i)
        {
            const crd::u32 v0 = j * n + i;
            idx.push_back(v0);
            idx.push_back(v0 + 1U);
            idx.push_back(v0 + n + 1U);
            idx.push_back(v0);
            idx.push_back(v0 + n + 1U);
            idx.push_back(v0 + n);
        }
    }
}

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

TEST_CASE("REN-40-I2: every meshlet belongs to exactly one group",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_grid(17U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::MeshletBuildOptions mopts;
    mp::MeshletBuildResult  mresult(&alloc);
    const auto mrep = mp::build_meshlets(pos.data(), vc, idx.data(), static_cast<crd::u32>(idx.size()), mopts, mresult, &alloc);
    REQUIRE(mrep.status == mp::MeshletBuildStatus::Ok);

    const crd::u32 mc = static_cast<crd::u32>(mresult.meshlets.size());
    REQUIRE(mc > 1U);

    mp::ClusterGroupOptions gopts;
    mp::ClusterGroupResult  gresult(&alloc);
    const auto              report = mp::group_meshlets(mresult, vc, gopts, gresult, &alloc);

    REQUIRE(report.status == mp::ClusterGroupStatus::Ok);
    REQUIRE(report.group_count > 0U);

    crd::containers::Array<crd::u32> seen(&alloc);
    seen.resize(mc);
    for (crd::u32 i = 0; i < mc; ++i) seen[i] = 0U;

    crd::u32 total = 0U;
    for (crd::u32 g = 0; g < report.group_count; ++g)
    {
        const auto& grp = gresult.groups[g];
        REQUIRE(grp.count > 0U);
        for (crd::u32 gi = 0; gi < grp.count; ++gi)
        {
            const crd::u32 m = gresult.group_meshlets[grp.first + gi];
            REQUIRE(m < mc);
            REQUIRE(seen[m] == 0U);
            seen[m] = 1U;
            ++total;
        }
    }
    REQUIRE(total == mc);
}

TEST_CASE("REN-40-I2: group sizes respect target",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::MeshletBuildOptions mopts;
    mp::MeshletBuildResult  mresult(&alloc);
    (void)mp::build_meshlets(pos.data(), vc, idx.data(), static_cast<crd::u32>(idx.size()), mopts, mresult, &alloc);

    mp::ClusterGroupOptions gopts;
    gopts.target_group_size = 4U;
    mp::ClusterGroupResult gresult(&alloc);
    const auto report = mp::group_meshlets(mresult, vc, gopts, gresult, &alloc);

    REQUIRE(report.status == mp::ClusterGroupStatus::Ok);
    for (crd::u32 g = 0; g < report.group_count; ++g)
        REQUIRE(gresult.groups[g].count <= gopts.target_group_size);
}

TEST_CASE("REN-40-I2: boundary vertices are correct",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_grid(17U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::MeshletBuildOptions mopts;
    mp::MeshletBuildResult  mresult(&alloc);
    (void)mp::build_meshlets(pos.data(), vc, idx.data(), static_cast<crd::u32>(idx.size()), mopts, mresult, &alloc);

    mp::ClusterGroupOptions gopts;
    mp::ClusterGroupResult  gresult(&alloc);
    const auto report = mp::group_meshlets(mresult, vc, gopts, gresult, &alloc);

    REQUIRE(report.status == mp::ClusterGroupStatus::Ok);
    REQUIRE(report.group_count > 1U);

    const crd::u32 mc = static_cast<crd::u32>(mresult.meshlets.size());

    // Build meshlet → group map
    crd::containers::Array<crd::u32> m_to_g(&alloc);
    m_to_g.resize(mc);
    for (crd::u32 g = 0; g < report.group_count; ++g)
    {
        const auto& grp = gresult.groups[g];
        for (crd::u32 gi = 0; gi < grp.count; ++gi)
            m_to_g[gresult.group_meshlets[grp.first + gi]] = g;
    }

    // Build vertex → meshlet list
    crd::containers::Array<crd::u32> vtm_count(&alloc);
    vtm_count.resize(vc);
    for (crd::u32 i = 0; i < vc; ++i) vtm_count[i] = 0U;
    for (crd::u32 m = 0; m < mc; ++m)
    {
        const auto& ml = mresult.meshlets[m];
        for (crd::u32 vi = 0; vi < ml.vertex_count; ++vi)
            vtm_count[mresult.meshlet_vertices[ml.vertex_offset + vi]]++;
    }
    crd::containers::Array<crd::u32> vtm_off(&alloc);
    vtm_off.resize(vc + 1U);
    vtm_off[0] = 0U;
    for (crd::u32 i = 0; i < vc; ++i) vtm_off[i + 1U] = vtm_off[i] + vtm_count[i];
    crd::containers::Array<crd::u32> vtm_data(&alloc);
    vtm_data.resize(vtm_off[vc]);
    for (crd::u32 i = 0; i < vc; ++i) vtm_count[i] = 0U;
    for (crd::u32 m = 0; m < mc; ++m)
    {
        const auto& ml = mresult.meshlets[m];
        for (crd::u32 vi = 0; vi < ml.vertex_count; ++vi)
        {
            const crd::u32 gv = mresult.meshlet_vertices[ml.vertex_offset + vi];
            vtm_data[vtm_off[gv] + vtm_count[gv]] = m;
            vtm_count[gv]++;
        }
    }

    for (crd::u32 g = 0; g < report.group_count; ++g)
    {
        const crd::u32 bv_beg = gresult.boundary_offsets[g];
        const crd::u32 bv_end = gresult.boundary_offsets[g + 1U];

        for (crd::u32 bi = bv_beg; bi < bv_end; ++bi)
        {
            const crd::u32 gv = gresult.boundary_vertices[bi];
            bool in_group     = false;
            bool out_of_group = false;
            for (crd::u32 vi = vtm_off[gv]; vi < vtm_off[gv + 1U]; ++vi)
            {
                if (m_to_g[vtm_data[vi]] == g)
                    in_group = true;
                else
                    out_of_group = true;
            }
            REQUIRE(in_group);
            REQUIRE(out_of_group);
        }
    }

    REQUIRE(report.boundary_vertex_count > 0U);
}

TEST_CASE("REN-40-I2: adjacency graph is symmetric",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::MeshletBuildOptions mopts;
    mp::MeshletBuildResult  mresult(&alloc);
    (void)mp::build_meshlets(pos.data(), vc, idx.data(), static_cast<crd::u32>(idx.size()), mopts, mresult, &alloc);

    mp::ClusterGroupOptions gopts;
    mp::ClusterGroupResult  gresult(&alloc);
    (void)mp::group_meshlets(mresult, vc, gopts, gresult, &alloc);

    const auto& adj = gresult.adjacency;
    const crd::u32 mc = static_cast<crd::u32>(mresult.meshlets.size());

    for (crd::u32 m = 0; m < mc; ++m)
    {
        for (crd::u32 ai = adj.offsets[m]; ai < adj.offsets[m + 1U]; ++ai)
        {
            const crd::u32 nb = adj.neighbors[ai];
            const crd::u32 w  = adj.weights[ai];

            bool found_reverse = false;
            for (crd::u32 bi = adj.offsets[nb]; bi < adj.offsets[nb + 1U]; ++bi)
            {
                if (adj.neighbors[bi] == m)
                {
                    REQUIRE(adj.weights[bi] == w);
                    found_reverse = true;
                    break;
                }
            }
            REQUIRE(found_reverse);
        }
    }
}

TEST_CASE("REN-40-I2: determinism",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_grid(13U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::MeshletBuildOptions mopts;
    mp::MeshletBuildResult  mresult(&alloc);
    (void)mp::build_meshlets(pos.data(), vc, idx.data(), static_cast<crd::u32>(idx.size()), mopts, mresult, &alloc);

    mp::ClusterGroupOptions gopts;

    mp::ClusterGroupResult r1(&alloc);
    const auto rep1 = mp::group_meshlets(mresult, vc, gopts, r1, &alloc);

    mp::ClusterGroupResult r2(&alloc);
    const auto rep2 = mp::group_meshlets(mresult, vc, gopts, r2, &alloc);

    REQUIRE(rep1.group_count == rep2.group_count);
    for (crd::u32 g = 0; g < rep1.group_count; ++g)
    {
        REQUIRE(r1.groups[g].first == r2.groups[g].first);
        REQUIRE(r1.groups[g].count == r2.groups[g].count);
    }
    for (crd::u32 i = 0; i < static_cast<crd::u32>(r1.group_meshlets.size()); ++i)
        REQUIRE(r1.group_meshlets[i] == r2.group_meshlets[i]);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(r1.boundary_vertices.size()); ++i)
        REQUIRE(r1.boundary_vertices[i] == r2.boundary_vertices[i]);
}

TEST_CASE("REN-40-I2: single meshlet produces one group",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::f32 p[9] = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    crd::u32 i[3] = {0U, 1U, 2U};

    mp::MeshletBuildOptions mopts;
    mp::MeshletBuildResult  mresult(&alloc);
    (void)mp::build_meshlets(p, 3U, i, 3U, mopts, mresult, &alloc);

    REQUIRE(mresult.meshlets.size() == 1U);

    mp::ClusterGroupOptions gopts;
    mp::ClusterGroupResult  gresult(&alloc);
    const auto report = mp::group_meshlets(mresult, 3U, gopts, gresult, &alloc);

    REQUIRE(report.status == mp::ClusterGroupStatus::Ok);
    REQUIRE(report.group_count == 1U);
    REQUIRE(gresult.groups[0].count == 1U);
    REQUIRE(report.boundary_vertex_count == 0U);
}

TEST_CASE("REN-40-I2: empty input is rejected",
          "[geometry][mesh-processing][cluster][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    mp::MeshletBuildResult     mresult(&alloc);
    mp::ClusterGroupOptions    gopts;
    mp::ClusterGroupResult     gresult(&alloc);

    const auto report = mp::group_meshlets(mresult, 0U, gopts, gresult, &alloc);
    REQUIRE(report.status == mp::ClusterGroupStatus::EmptyInput);
}
