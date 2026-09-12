#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/cluster_dag_cook.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>

namespace mp = crd::geometry::mesh_processing;

static crd::f32 bits_to_f32(crd::u32 bits) noexcept
{
    crd::f32 v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
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

TEST_CASE("REN-40-I5: cook produces clusters and BVH",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult result(&alloc);
    const auto report = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    REQUIRE(report.status == mp::ClusterDagCookStatus::Ok);
    REQUIRE(report.cluster_count > 0U);
    REQUIRE(report.bvh_node_count > 0U);
    REQUIRE(report.level_count > 1U);
    REQUIRE(report.leaf_count > 0U);

    REQUIRE(result.packed_clusters.size() ==
        static_cast<crd::usize>(report.cluster_count) * mp::kClusterGpuWords);
    REQUIRE(result.packed_bvh.size() ==
        static_cast<crd::usize>(report.bvh_node_count) * mp::kBvhNodeGpuWords);
    REQUIRE(result.vertex_count > 0U);
}

TEST_CASE("REN-40-I5: packed cluster data round-trips correctly",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(16U, 8U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult result(&alloc);
    const auto report = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, result, &alloc);
    REQUIRE(report.status == mp::ClusterDagCookStatus::Ok);

    for (crd::u32 ci = 0; ci < result.cluster_count; ++ci)
    {
        const crd::u32* w = result.packed_clusters.data()
                          + static_cast<crd::usize>(ci) * mp::kClusterGpuWords;
        const crd::u32 vert_cnt = w[2] & 0xFFU;
        const crd::u32 tri_cnt  = (w[2] >> 8U) & 0xFFU;
        const crd::u32 vert_off = w[0];
        const crd::f32 error    = bits_to_f32(w[3]);
        const crd::f32 parent_e = bits_to_f32(w[4]);

        REQUIRE(vert_cnt > 0U);
        REQUIRE(vert_cnt <= 64U);
        REQUIRE(tri_cnt > 0U);
        REQUIRE(tri_cnt <= 124U);
        REQUIRE(vert_off + vert_cnt <= static_cast<crd::u32>(result.cluster_vertices.size()));
        REQUIRE(error >= 0.0F);
        REQUIRE(parent_e >= error);
    }
}

TEST_CASE("REN-40-I5: packed BVH data round-trips correctly",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(16U, 8U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult result(&alloc);
    const auto report = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, result, &alloc);
    REQUIRE(report.status == mp::ClusterDagCookStatus::Ok);

    for (crd::u32 ni = 0; ni < result.bvh_node_count; ++ni)
    {
        const crd::u32* w = result.packed_bvh.data()
                          + static_cast<crd::usize>(ni) * mp::kBvhNodeGpuWords;
        const crd::f32 radius = bits_to_f32(w[3]);
        const crd::u32 right  = w[7];

        REQUIRE(radius >= 0.0F);
        if (right != 0xFFFFFFFFU)
        {
            REQUIRE(w[6] == ni + 1U);
            REQUIRE(right < result.bvh_node_count);
        }
    }
}

TEST_CASE("REN-40-I5: cluster triangles pack into u32 words correctly",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(16U, 8U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult result(&alloc);
    const auto report = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, result, &alloc);
    REQUIRE(report.status == mp::ClusterDagCookStatus::Ok);

    REQUIRE(result.triangle_byte_count > 0U);
    const crd::u32 expected_words = (result.triangle_byte_count + 3U) / 4U;
    REQUIRE(result.cluster_triangles_packed.size() == expected_words);

    for (crd::u32 ci = 0; ci < result.cluster_count; ++ci)
    {
        const crd::u32* w = result.packed_clusters.data()
                          + static_cast<crd::usize>(ci) * mp::kClusterGpuWords;
        const crd::u32 tri_off  = w[1];
        const crd::u32 vert_cnt = w[2] & 0xFFU;
        const crd::u32 tri_cnt  = (w[2] >> 8U) & 0xFFU;

        for (crd::u32 ti = 0; ti < tri_cnt * 3U; ++ti)
        {
            const crd::u32 byte_idx = tri_off + ti;
            const crd::u32 word_idx = byte_idx / 4U;
            const crd::u32 shift    = (byte_idx % 4U) * 8U;
            const crd::u8  local_vi = static_cast<crd::u8>(
                (result.cluster_triangles_packed[word_idx] >> shift) & 0xFFU);
            REQUIRE(local_vi < vert_cnt);
        }
    }
}

TEST_CASE("REN-40-I5: determinism - two cooks produce identical output",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(16U, 8U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;

    mp::ClusterDagCookResult r1(&alloc);
    const auto rep1 = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, r1, &alloc);

    mp::ClusterDagCookResult r2(&alloc);
    const auto rep2 = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, r2, &alloc);

    REQUIRE(rep1.cluster_count == rep2.cluster_count);
    REQUIRE(rep1.bvh_node_count == rep2.bvh_node_count);

    for (crd::usize i = 0; i < r1.packed_clusters.size(); ++i)
        REQUIRE(r1.packed_clusters[i] == r2.packed_clusters[i]);
    for (crd::usize i = 0; i < r1.packed_bvh.size(); ++i)
        REQUIRE(r1.packed_bvh[i] == r2.packed_bvh[i]);
    for (crd::usize i = 0; i < r1.cluster_triangles_packed.size(); ++i)
        REQUIRE(r1.cluster_triangles_packed[i] == r2.cluster_triangles_packed[i]);
}

TEST_CASE("REN-40-I5: root clusters have FLT_MAX parent error in packed data",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(16U, 8U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult result(&alloc);
    const auto report = mp::cook_cluster_dag(pos.data(), vc, idx.data(),
        static_cast<crd::u32>(idx.size()), opts, result, &alloc);
    REQUIRE(report.status == mp::ClusterDagCookStatus::Ok);

    bool found_root = false;
    for (crd::u32 ci = 0; ci < result.cluster_count; ++ci)
    {
        const crd::u32* w = result.packed_clusters.data()
                          + static_cast<crd::usize>(ci) * mp::kClusterGpuWords;
        const crd::f32 parent_e = bits_to_f32(w[4]);
        if (parent_e == std::numeric_limits<crd::f32>::max())
        {
            found_root = true;
            break;
        }
    }
    REQUIRE(found_root);
}

TEST_CASE("REN-40-I5: empty mesh is rejected",
          "[geometry][mesh-processing][cook][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    mp::DagBuildOptions opts;
    mp::ClusterDagCookResult result(&alloc);
    const auto report = mp::cook_cluster_dag(nullptr, 0U, nullptr, 0U, opts, result, &alloc);
    REQUIRE(report.status == mp::ClusterDagCookStatus::EmptyMesh);
}
