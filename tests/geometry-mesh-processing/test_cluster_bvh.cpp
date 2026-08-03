#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/cluster_bvh.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cfloat>

namespace mp = crd::geometry::mesh_processing;

static void make_line_clusters(crd::u32 count, crd::containers::Array<mp::DagCluster>& out)
{
    out.clear();
    for (crd::u32 i = 0; i < count; ++i)
    {
        mp::DagCluster c{};
        c.center[0]    = static_cast<crd::f32>(i) * 2.0F;
        c.center[1]    = 0.0F;
        c.center[2]    = 0.0F;
        c.radius       = 1.0F;
        c.error        = static_cast<crd::f32>(i) * 0.1F;
        c.parent_error = static_cast<crd::f32>(count - i);
        c.level        = 0U;
        c.vertex_count    = 1U;
        c.triangle_count  = 1U;
        out.push_back(c);
    }
}

static void make_3d_clusters(crd::u32 n, crd::containers::Array<mp::DagCluster>& out)
{
    out.clear();
    crd::u32 idx = 0U;
    for (crd::u32 z = 0; z < n; ++z)
    {
        for (crd::u32 y = 0; y < n; ++y)
        {
            for (crd::u32 x = 0; x < n; ++x)
            {
                mp::DagCluster c{};
                c.center[0]    = static_cast<crd::f32>(x) * 3.0F;
                c.center[1]    = static_cast<crd::f32>(y) * 3.0F;
                c.center[2]    = static_cast<crd::f32>(z) * 3.0F;
                c.radius       = 1.0F;
                c.error        = static_cast<crd::f32>(idx) * 0.01F;
                c.parent_error = 100.0F - static_cast<crd::f32>(idx) * 0.01F;
                c.level        = 0U;
                c.vertex_count    = 1U;
                c.triangle_count  = 1U;
                out.push_back(c);
                ++idx;
            }
        }
    }
}

static bool sphere_contains(const mp::ClusterBvhNode& parent,
                            const mp::ClusterBvhNode& child) noexcept
{
    const crd::f32 dx = parent.center[0] - child.center[0];
    const crd::f32 dy = parent.center[1] - child.center[1];
    const crd::f32 dz = parent.center[2] - child.center[2];
    const crd::f32 d  = std::sqrt(dx * dx + dy * dy + dz * dz);
    return d + child.radius <= parent.radius + 1.0e-4F;
}

TEST_CASE("REN-40-I4: every cluster appears as exactly one leaf",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::containers::Array<mp::DagCluster> clusters(&alloc);
    make_line_clusters(32U, clusters);

    mp::ClusterBvhResult result(&alloc);
    const auto report = mp::build_cluster_bvh(clusters.data(), 32U, result, &alloc);

    REQUIRE(report.status == mp::ClusterBvhStatus::Ok);
    REQUIRE(report.leaf_count == 32U);
    REQUIRE(report.node_count == 63U);

    crd::containers::Array<crd::u32> seen(&alloc);
    seen.resize(32U);
    for (crd::u32 i = 0; i < 32U; ++i) seen[i] = 0U;

    for (crd::u32 ni = 0; ni < report.node_count; ++ni)
    {
        if (result.nodes[ni].right == 0xFFFFFFFFU)
        {
            const crd::u32 ci = result.nodes[ni].left;
            REQUIRE(ci < 32U);
            REQUIRE(seen[ci] == 0U);
            seen[ci] = 1U;
        }
    }
    for (crd::u32 i = 0; i < 32U; ++i) REQUIRE(seen[i] == 1U);
}

TEST_CASE("REN-40-I4: internal spheres enclose children",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::containers::Array<mp::DagCluster> clusters(&alloc);
    make_3d_clusters(4U, clusters);

    mp::ClusterBvhResult result(&alloc);
    const auto report = mp::build_cluster_bvh(clusters.data(),
        static_cast<crd::u32>(clusters.size()), result, &alloc);
    REQUIRE(report.status == mp::ClusterBvhStatus::Ok);

    for (crd::u32 ni = 0; ni < report.node_count; ++ni)
    {
        const auto& node = result.nodes[ni];
        if (node.right == 0xFFFFFFFFU) continue;

        REQUIRE(sphere_contains(node, result.nodes[node.left]));
        REQUIRE(sphere_contains(node, result.nodes[node.right]));
    }
}

TEST_CASE("REN-40-I4: error fields propagate correctly",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::containers::Array<mp::DagCluster> clusters(&alloc);
    make_3d_clusters(3U, clusters);

    mp::ClusterBvhResult result(&alloc);
    const auto report = mp::build_cluster_bvh(clusters.data(),
        static_cast<crd::u32>(clusters.size()), result, &alloc);
    REQUIRE(report.status == mp::ClusterBvhStatus::Ok);

    for (crd::u32 ni = 0; ni < report.node_count; ++ni)
    {
        const auto& node = result.nodes[ni];
        if (node.right == 0xFFFFFFFFU) continue;

        const auto& left  = result.nodes[node.left];
        const auto& right_child = result.nodes[node.right];

        crd::f32 child_max_err = left.max_error;
        if (right_child.max_error > child_max_err) child_max_err = right_child.max_error;
        REQUIRE(node.max_error >= child_max_err - 1.0e-6F);

        crd::f32 child_min_pe = left.min_parent_error;
        if (right_child.min_parent_error < child_min_pe) child_min_pe = right_child.min_parent_error;
        REQUIRE(node.min_parent_error <= child_min_pe + 1.0e-6F);
    }
}

TEST_CASE("REN-40-I4: DFS order - left child at index + 1",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::containers::Array<mp::DagCluster> clusters(&alloc);
    make_line_clusters(16U, clusters);

    mp::ClusterBvhResult result(&alloc);
    const auto report = mp::build_cluster_bvh(clusters.data(), 16U, result, &alloc);
    REQUIRE(report.status == mp::ClusterBvhStatus::Ok);

    for (crd::u32 ni = 0; ni < report.node_count; ++ni)
    {
        const auto& node = result.nodes[ni];
        if (node.right != 0xFFFFFFFFU)
        {
            REQUIRE(node.left == ni + 1U);
            REQUIRE(node.right > ni + 1U);
            REQUIRE(node.right < report.node_count);
        }
    }
}

TEST_CASE("REN-40-I4: determinism",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(2U << 20U);
    crd::containers::Array<mp::DagCluster> clusters(&alloc);
    make_3d_clusters(3U, clusters);

    mp::ClusterBvhResult r1(&alloc);
    const auto rep1 = mp::build_cluster_bvh(clusters.data(),
        static_cast<crd::u32>(clusters.size()), r1, &alloc);

    mp::ClusterBvhResult r2(&alloc);
    const auto rep2 = mp::build_cluster_bvh(clusters.data(),
        static_cast<crd::u32>(clusters.size()), r2, &alloc);

    REQUIRE(rep1.node_count == rep2.node_count);
    REQUIRE(rep1.leaf_count == rep2.leaf_count);
    REQUIRE(rep1.depth == rep2.depth);

    for (crd::u32 ni = 0; ni < rep1.node_count; ++ni)
    {
        REQUIRE(r1.nodes[ni].left == r2.nodes[ni].left);
        REQUIRE(r1.nodes[ni].right == r2.nodes[ni].right);
        REQUIRE(r1.nodes[ni].center[0] == r2.nodes[ni].center[0]);
        REQUIRE(r1.nodes[ni].center[1] == r2.nodes[ni].center[1]);
        REQUIRE(r1.nodes[ni].center[2] == r2.nodes[ni].center[2]);
        REQUIRE(r1.nodes[ni].radius == r2.nodes[ni].radius);
        REQUIRE(r1.nodes[ni].max_error == r2.nodes[ni].max_error);
        REQUIRE(r1.nodes[ni].min_parent_error == r2.nodes[ni].min_parent_error);
    }
}

TEST_CASE("REN-40-I4: single cluster produces one leaf",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    mp::DagCluster c{};
    c.center[0] = 1.0F;
    c.center[1] = 2.0F;
    c.center[2] = 3.0F;
    c.radius       = 0.5F;
    c.error        = 0.1F;
    c.parent_error = 10.0F;

    mp::ClusterBvhResult result(&alloc);
    const auto report = mp::build_cluster_bvh(&c, 1U, result, &alloc);

    REQUIRE(report.status == mp::ClusterBvhStatus::Ok);
    REQUIRE(report.node_count == 1U);
    REQUIRE(report.leaf_count == 1U);
    REQUIRE(report.depth == 0U);

    REQUIRE(result.nodes[0].right == 0xFFFFFFFFU);
    REQUIRE(result.nodes[0].left == 0U);
    REQUIRE(result.nodes[0].center[0] == 1.0F);
    REQUIRE(result.nodes[0].center[1] == 2.0F);
    REQUIRE(result.nodes[0].center[2] == 3.0F);
    REQUIRE(result.nodes[0].radius == 0.5F);
    REQUIRE(result.nodes[0].max_error == 0.1F);
    REQUIRE(result.nodes[0].min_parent_error == 10.0F);
}

TEST_CASE("REN-40-I4: empty input is rejected",
          "[geometry][mesh-processing][bvh][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    mp::ClusterBvhResult result(&alloc);
    const auto report = mp::build_cluster_bvh(nullptr, 0U, result, &alloc);
    REQUIRE(report.status == mp::ClusterBvhStatus::EmptyInput);
}
