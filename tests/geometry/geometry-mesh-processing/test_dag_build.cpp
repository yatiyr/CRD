#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/dag_build.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

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

TEST_CASE("REN-40-I3: DAG has more clusters than original meshlets",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::DagBuildResult  result(&alloc);
    const auto report = mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                              static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    REQUIRE(report.status == mp::DagBuildStatus::Ok);
    REQUIRE(report.leaf_count > 0U);
    REQUIRE(report.cluster_count > report.leaf_count);
    REQUIRE(report.level_count > 1U);
}

TEST_CASE("REN-40-I3: error is monotone - parent error >= max child error",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::DagBuildResult  result(&alloc);
    (void)mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    for (crd::u32 ci = 0; ci < static_cast<crd::u32>(result.clusters.size()); ++ci)
    {
        const auto& c = result.clusters[ci];
        if (c.level == 0U)
        {
            REQUIRE(c.error == 0.0F);
        }
        else
        {
            REQUIRE(c.error > 0.0F);
        }
        REQUIRE(c.parent_error >= c.error);
    }
}

TEST_CASE("REN-40-I3: root clusters have parent_error = FLT_MAX",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::DagBuildResult  result(&alloc);
    const auto report = mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                              static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    crd::u32 root_count = 0U;
    for (crd::u32 ci = 0; ci < report.cluster_count; ++ci)
    {
        if (result.clusters[ci].parent_error == std::numeric_limits<crd::f32>::max())
            ++root_count;
    }
    REQUIRE(root_count > 0U);
}

TEST_CASE("REN-40-I3: leaf clusters cover all original triangles",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    const crd::u32 tc = static_cast<crd::u32>(idx.size() / 3U);
    mp::DagBuildOptions opts;
    mp::DagBuildResult  result(&alloc);
    (void)mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    crd::u32 leaf_tris = 0U;
    for (crd::u32 ci = 0; ci < static_cast<crd::u32>(result.clusters.size()); ++ci)
    {
        if (result.clusters[ci].level == 0U)
            leaf_tris += result.clusters[ci].triangle_count;
    }
    REQUIRE(leaf_tris == tc);
}

TEST_CASE("REN-40-I3: bounding spheres contain all cluster vertices",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::DagBuildResult  result(&alloc);
    (void)mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    for (crd::u32 ci = 0; ci < static_cast<crd::u32>(result.clusters.size()); ++ci)
    {
        const auto& c = result.clusters[ci];
        for (crd::u32 vi = 0; vi < c.vertex_count; ++vi)
        {
            const crd::u32 gv = result.cluster_vertices[c.vertex_offset + vi];
            const crd::f32 dx = result.positions[gv * 3U + 0U] - c.center[0];
            const crd::f32 dy = result.positions[gv * 3U + 1U] - c.center[1];
            const crd::f32 dz = result.positions[gv * 3U + 2U] - c.center[2];
            const crd::f32 d  = std::sqrt(dx * dx + dy * dy + dz * dz);
            REQUIRE(d <= c.radius + 1.0e-5F);
        }
    }
}

TEST_CASE("REN-40-I3: determinism",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(16U, 8U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;

    mp::DagBuildResult r1(&alloc);
    const auto rep1 = mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                            static_cast<crd::u32>(idx.size()), opts, r1, &alloc);

    mp::DagBuildResult r2(&alloc);
    const auto rep2 = mp::build_cluster_dag(pos.data(), vc, idx.data(),
                                            static_cast<crd::u32>(idx.size()), opts, r2, &alloc);

    REQUIRE(rep1.cluster_count == rep2.cluster_count);
    REQUIRE(rep1.level_count == rep2.level_count);
    for (crd::u32 ci = 0; ci < rep1.cluster_count; ++ci)
    {
        REQUIRE(r1.clusters[ci].level == r2.clusters[ci].level);
        REQUIRE(r1.clusters[ci].error == r2.clusters[ci].error);
        REQUIRE(r1.clusters[ci].vertex_count == r2.clusters[ci].vertex_count);
        REQUIRE(r1.clusters[ci].triangle_count == r2.clusters[ci].triangle_count);
    }
}

TEST_CASE("REN-40-I3: empty mesh is rejected",
          "[geometry][mesh-processing][dag][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    mp::DagBuildOptions opts;
    mp::DagBuildResult  result(&alloc);
    const auto report = mp::build_cluster_dag(nullptr, 0U, nullptr, 0U, opts, result, &alloc);
    REQUIRE(report.status == mp::DagBuildStatus::EmptyMesh);
}
