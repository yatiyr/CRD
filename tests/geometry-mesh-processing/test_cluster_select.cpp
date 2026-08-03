#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/geometry/mesh_processing/cluster_select.hpp>
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

struct CookedFixture
{
    crd::memory::TlsfAllocator alloc{64U << 20U};
    mp::ClusterDagCookResult   cook{&alloc};
    crd::u32                   cc = 0U;

    CookedFixture()
    {
        crd::containers::Array<crd::f32> pos(&alloc);
        crd::containers::Array<crd::u32> idx(&alloc);
        make_sphere(32U, 16U, pos, idx);
        mp::DagBuildOptions opts;
        const auto rep = mp::cook_cluster_dag(pos.data(),
            static_cast<crd::u32>(pos.size() / 3U), idx.data(),
            static_cast<crd::u32>(idx.size()), opts, cook, &alloc);
        REQUIRE(rep.status == mp::ClusterDagCookStatus::Ok);
        cc = rep.cluster_count;
    }
};

TEST_CASE("REN-40-I6: flat and BVH selection produce the same set",
          "[geometry][mesh-processing][select][ren40]")
{
    CookedFixture fx;
    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.01F;
    params.camera_pos[0]   = 0.0F;
    params.camera_pos[1]   = 0.0F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> sel_flat(&fx.alloc);
    crd::containers::Array<crd::u32> sel_bvh(&fx.alloc);
    sel_flat.resize(fx.cc);
    sel_bvh.resize(fx.cc);

    const crd::u32 n_flat = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params,
        sel_flat.data(), fx.cc);
    const crd::u32 n_bvh = mp::select_clusters_bvh(
        fx.cook.packed_clusters.data(), fx.cc,
        fx.cook.packed_bvh.data(), fx.cook.bvh_node_count, params,
        sel_bvh.data(), fx.cc, &fx.alloc);

    REQUIRE(n_flat > 0U);
    REQUIRE(n_flat == n_bvh);

    crd::containers::sort(sel_flat.data(), sel_flat.data() + n_flat);
    crd::containers::sort(sel_bvh.data(), sel_bvh.data() + n_bvh);
    for (crd::u32 i = 0; i < n_flat; ++i)
        REQUIRE(sel_flat[i] == sel_bvh[i]);
}

TEST_CASE("REN-40-I6: selected clusters satisfy LOD criterion",
          "[geometry][mesh-processing][select][ren40]")
{
    CookedFixture fx;
    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.005F;
    params.camera_pos[0]   = 3.0F;
    params.camera_pos[1]   = 0.0F;
    params.camera_pos[2]   = 4.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params,
        sel.data(), fx.cc);
    REQUIRE(n > 0U);

    const crd::f32 tp = params.error_threshold * params.proj_factor;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32* w = fx.cook.packed_clusters.data()
                          + static_cast<crd::usize>(sel[i]) * mp::kClusterGpuWords;
        const crd::f32 err = bits_to_f32(w[3]);
        const crd::f32 pe  = bits_to_f32(w[4]);
        const crd::f32 cx  = bits_to_f32(w[5]);
        const crd::f32 cy  = bits_to_f32(w[6]);
        const crd::f32 cz  = bits_to_f32(w[7]);
        const crd::f32 dx  = cx - params.camera_pos[0];
        const crd::f32 dy  = cy - params.camera_pos[1];
        const crd::f32 dz  = cz - params.camera_pos[2];
        const crd::f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const crd::f32 st   = tp * dist;

        REQUIRE(pe > st);
        REQUIRE(err <= st);
    }
}

TEST_CASE("REN-40-I6: non-selected clusters violate at least one condition",
          "[geometry][mesh-processing][select][ren40]")
{
    CookedFixture fx;
    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.005F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params,
        sel.data(), fx.cc);

    crd::containers::Array<crd::u32> is_selected(&fx.alloc);
    is_selected.resize(fx.cc);
    for (crd::u32 i = 0; i < fx.cc; ++i) is_selected[i] = 0U;
    for (crd::u32 i = 0; i < n; ++i)     is_selected[sel[i]] = 1U;

    const crd::f32 tp = params.error_threshold * params.proj_factor;
    for (crd::u32 ci = 0; ci < fx.cc; ++ci)
    {
        if (is_selected[ci] == 1U) continue;
        const crd::u32* w = fx.cook.packed_clusters.data()
                          + static_cast<crd::usize>(ci) * mp::kClusterGpuWords;
        const crd::f32 err  = bits_to_f32(w[3]);
        const crd::f32 pe   = bits_to_f32(w[4]);
        const crd::f32 cx   = bits_to_f32(w[5]);
        const crd::f32 cy   = bits_to_f32(w[6]);
        const crd::f32 cz   = bits_to_f32(w[7]);
        const crd::f32 dx   = cx - params.camera_pos[0];
        const crd::f32 dy   = cy - params.camera_pos[1];
        const crd::f32 dz   = cz - params.camera_pos[2];
        const crd::f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const crd::f32 st   = tp * dist;

        const bool c1_fail = (pe <= st);
        const bool c2_fail = (err > st);
        REQUIRE((c1_fail || c2_fail));
    }
}

TEST_CASE("REN-40-I6: far camera selects root clusters",
          "[geometry][mesh-processing][select][ren40]")
{
    CookedFixture fx;
    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 10.0F;
    params.camera_pos[2]   = 100.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params,
        sel.data(), fx.cc);
    REQUIRE(n > 0U);

    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32* w = fx.cook.packed_clusters.data()
                          + static_cast<crd::usize>(sel[i]) * mp::kClusterGpuWords;
        const crd::f32 pe = bits_to_f32(w[4]);
        REQUIRE(pe == std::numeric_limits<crd::f32>::max());
    }
}

TEST_CASE("REN-40-I6: close camera selects leaf clusters",
          "[geometry][mesh-processing][select][ren40]")
{
    CookedFixture fx;
    mp::ClusterSelectParams params;
    params.error_threshold = 0.001F;
    params.proj_factor     = 0.001F;
    params.camera_pos[2]   = 2.0F;

    crd::containers::Array<crd::u32> sel(&fx.alloc);
    sel.resize(fx.cc);
    const crd::u32 n = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params,
        sel.data(), fx.cc);
    REQUIRE(n > 0U);

    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32* w = fx.cook.packed_clusters.data()
                          + static_cast<crd::usize>(sel[i]) * mp::kClusterGpuWords;
        const crd::u32 level = (w[2] >> 16U) & 0xFFFFU;
        REQUIRE(level == 0U);
    }
}

TEST_CASE("REN-40-I6: determinism",
          "[geometry][mesh-processing][select][ren40]")
{
    CookedFixture fx;
    mp::ClusterSelectParams params;
    params.error_threshold = 1.0F;
    params.proj_factor     = 0.01F;
    params.camera_pos[2]   = 5.0F;

    crd::containers::Array<crd::u32> s1(&fx.alloc), s2(&fx.alloc);
    s1.resize(fx.cc);
    s2.resize(fx.cc);

    const crd::u32 n1 = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params, s1.data(), fx.cc);
    const crd::u32 n2 = mp::select_clusters_flat(
        fx.cook.packed_clusters.data(), fx.cc, params, s2.data(), fx.cc);

    REQUIRE(n1 == n2);
    for (crd::u32 i = 0; i < n1; ++i)
        REQUIRE(s1[i] == s2[i]);
}

TEST_CASE("REN-40-I6: empty input returns zero",
          "[geometry][mesh-processing][select][ren40]")
{
    mp::ClusterSelectParams params;
    crd::u32 dummy = 0U;

    REQUIRE(mp::select_clusters_flat(nullptr, 0U, params, &dummy, 1U) == 0U);

    crd::memory::TlsfAllocator alloc(1U << 20U);
    REQUIRE(mp::select_clusters_bvh(nullptr, 0U, nullptr, 0U, params,
                                     &dummy, 1U, &alloc) == 0U);
}
