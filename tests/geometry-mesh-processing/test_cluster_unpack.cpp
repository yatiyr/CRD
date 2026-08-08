#include <crd/containers/array.hpp>
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

struct CookSelectFixture
{
    crd::memory::TlsfAllocator alloc{64U << 20U};
    mp::ClusterDagCookResult   cook{&alloc};
    crd::u32                   cc = 0U;
    crd::u32                   original_tri_count = 0U;

    CookSelectFixture()
    {
        crd::containers::Array<crd::f32> pos(&alloc);
        crd::containers::Array<crd::u32> idx(&alloc);
        make_sphere(32U, 16U, pos, idx);
        original_tri_count = static_cast<crd::u32>(idx.size() / 3U);
        mp::DagBuildOptions opts;
        const auto rep = mp::cook_cluster_dag(pos.data(),
            static_cast<crd::u32>(pos.size() / 3U), idx.data(),
            static_cast<crd::u32>(idx.size()), opts, cook, &alloc);
        REQUIRE(rep.status == mp::ClusterDagCookStatus::Ok);
        cc = rep.cluster_count;
    }
};

TEST_CASE("REN-40-I7: unpack all leaves recovers original triangle count",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    crd::containers::Array<crd::u32> leaves(&fx.alloc);
    for (crd::u32 ci = 0; ci < fx.cc; ++ci)
    {
        const crd::u32* w = fx.cook.packed_clusters.data()
                          + static_cast<crd::usize>(ci) * mp::kClusterGpuWords;
        const crd::u32 level = (w[2] >> 16U) & 0xFFFFU;
        if (level == 0U) leaves.push_back(ci);
    }
    REQUIRE(leaves.size() > 0U);

    mp::ClusterUnpackResult out(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(),
        fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(),
        fx.cook.positions.data(),
        leaves.data(), static_cast<crd::u32>(leaves.size()), out);

    REQUIRE(out.triangle_count == fx.original_tri_count);
    REQUIRE(out.vertex_count > 0U);
}

TEST_CASE("REN-40-I7: output triangle indices are in range",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.005F;
    params.camera_pos[2]   = 4.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params, sel.data(), fx.cc);
    REQUIRE(n > 0U);

    mp::ClusterUnpackResult out(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(),
        fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(),
        fx.cook.positions.data(),
        sel.data(), n, out);

    for (crd::u32 ti = 0; ti < out.triangle_count; ++ti)
    {
        REQUIRE(out.triangles[ti * 3U + 0U] < out.vertex_count);
        REQUIRE(out.triangles[ti * 3U + 1U] < out.vertex_count);
        REQUIRE(out.triangles[ti * 3U + 2U] < out.vertex_count);
    }
}

TEST_CASE("REN-40-I7: unpacked positions are finite",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.01F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params, sel.data(), fx.cc);

    mp::ClusterUnpackResult out(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(),
        fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(),
        fx.cook.positions.data(),
        sel.data(), n, out);

    for (crd::u32 vi = 0; vi < out.vertex_count; ++vi)
    {
        const crd::f32 x = out.positions[vi * 3U + 0U];
        const crd::f32 y = out.positions[vi * 3U + 1U];
        const crd::f32 z = out.positions[vi * 3U + 2U];
        REQUIRE(std::isfinite(x));
        REQUIRE(std::isfinite(y));
        REQUIRE(std::isfinite(z));
    }
}

TEST_CASE("REN-40-I7: single cluster unpack matches cluster counts",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    const crd::u32 ci = 0U;
    const crd::u32* w = fx.cook.packed_clusters.data();
    const crd::u32 exp_vc = w[2] & 0xFFU;
    const crd::u32 exp_tc = (w[2] >> 8U) & 0xFFU;

    mp::ClusterUnpackResult out(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(),
        fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(),
        fx.cook.positions.data(),
        &ci, 1U, out);

    REQUIRE(out.vertex_count == exp_vc);
    REQUIRE(out.triangle_count == exp_tc);
}

TEST_CASE("REN-40-I7: empty selection produces empty output",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    mp::ClusterUnpackResult out(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(),
        fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(),
        fx.cook.positions.data(),
        nullptr, 0U, out);

    REQUIRE(out.vertex_count == 0U);
    REQUIRE(out.triangle_count == 0U);
    REQUIRE(out.positions.size() == 0U);
    REQUIRE(out.triangles.size() == 0U);
}

TEST_CASE("REN-40-I7: determinism",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.01F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params, sel.data(), fx.cc);

    mp::ClusterUnpackResult r1(&fx.alloc);
    mp::ClusterUnpackResult r2(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(), fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(), fx.cook.positions.data(),
        sel.data(), n, r1);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(), fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(), fx.cook.positions.data(),
        sel.data(), n, r2);

    REQUIRE(r1.vertex_count == r2.vertex_count);
    REQUIRE(r1.triangle_count == r2.triangle_count);
    for (crd::usize i = 0; i < r1.triangles.size(); ++i)
        REQUIRE(r1.triangles[i] == r2.triangles[i]);
}

TEST_CASE("REN-40-I7: full round-trip cook-select-unpack at close range",
          "[geometry][mesh-processing][unpack][ren40]")
{
    CookSelectFixture fx;

    mp::ClusterSelectParams params;
    params.error_threshold = 0.0001F;
    params.proj_factor     = 0.0001F;
    params.camera_pos[2]   = 2.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params, sel.data(), fx.cc);
    REQUIRE(n > 0U);

    mp::ClusterUnpackResult out(&fx.alloc);
    mp::unpack_selected_clusters(
        fx.cook.packed_clusters.data(), fx.cook.cluster_vertices.data(),
        fx.cook.cluster_triangles_packed.data(), fx.cook.positions.data(),
        sel.data(), n, out);

    REQUIRE(out.triangle_count == fx.original_tri_count);

    for (crd::u32 vi = 0; vi < out.vertex_count; ++vi)
    {
        const crd::f32 x = out.positions[vi * 3U + 0U];
        const crd::f32 y = out.positions[vi * 3U + 1U];
        const crd::f32 z = out.positions[vi * 3U + 2U];
        const crd::f32 r = std::sqrt(x * x + y * y + z * z);
        REQUIRE(r < 1.5F);
    }
}
