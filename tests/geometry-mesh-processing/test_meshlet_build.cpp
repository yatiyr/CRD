#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/meshlet_build.hpp>
#include <crd/math/vec.hpp>
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

TEST_CASE("REN-40-I1: meshlet builder covers every triangle exactly once",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_grid(17U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    const crd::u32 tc = static_cast<crd::u32>(idx.size() / 3U);

    mp::MeshletBuildOptions opts;
    mp::MeshletBuildResult  result(&alloc);
    const auto              report = mp::build_meshlets(pos.data(), vc, idx.data(),
                                                        static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    REQUIRE(report.status == mp::MeshletBuildStatus::Ok);
    REQUIRE(report.meshlet_count > 0U);
    REQUIRE(report.triangle_count == tc);

    crd::containers::Array<crd::u32> tri_count(&alloc);
    tri_count.resize(tc);
    for (crd::u32 i = 0; i < tc; ++i) tri_count[i] = 0U;

    for (crd::u32 mi = 0; mi < static_cast<crd::u32>(result.meshlets.size()); ++mi)
    {
        const auto& m = result.meshlets[mi];
        REQUIRE(m.vertex_count <= opts.max_vertices);
        REQUIRE(m.triangle_count <= opts.max_triangles);

        for (crd::u32 ti = 0; ti < m.triangle_count; ++ti)
        {
            const crd::u8 l0 = result.meshlet_triangles[m.triangle_offset + ti * 3 + 0];
            const crd::u8 l1 = result.meshlet_triangles[m.triangle_offset + ti * 3 + 1];
            const crd::u8 l2 = result.meshlet_triangles[m.triangle_offset + ti * 3 + 2];
            REQUIRE(l0 < m.vertex_count);
            REQUIRE(l1 < m.vertex_count);
            REQUIRE(l2 < m.vertex_count);

            const crd::u32 g0 = result.meshlet_vertices[m.vertex_offset + l0];
            const crd::u32 g1 = result.meshlet_vertices[m.vertex_offset + l1];
            const crd::u32 g2 = result.meshlet_vertices[m.vertex_offset + l2];

            bool found = false;
            for (crd::u32 si = 0; si < tc; ++si)
            {
                const crd::u32 s0 = idx[si * 3 + 0];
                const crd::u32 s1 = idx[si * 3 + 1];
                const crd::u32 s2 = idx[si * 3 + 2];
                if (g0 == s0 && g1 == s1 && g2 == s2)
                {
                    tri_count[si]++;
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
        }
    }

    for (crd::u32 i = 0; i < tc; ++i)
    {
        REQUIRE(tri_count[i] == 1U);
    }
}

TEST_CASE("REN-40-I1: meshlet builder respects vertex and triangle budgets",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);

    mp::MeshletBuildOptions opts;
    opts.max_vertices  = 32U;
    opts.max_triangles = 64U;
    mp::MeshletBuildResult result(&alloc);
    const auto report = mp::build_meshlets(pos.data(), vc, idx.data(),
                                           static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    REQUIRE(report.status == mp::MeshletBuildStatus::Ok);
    for (crd::u32 mi = 0; mi < static_cast<crd::u32>(result.meshlets.size()); ++mi)
    {
        REQUIRE(result.meshlets[mi].vertex_count <= 32U);
        REQUIRE(result.meshlets[mi].triangle_count <= 64U);
        REQUIRE(result.meshlets[mi].vertex_count > 0U);
        REQUIRE(result.meshlets[mi].triangle_count > 0U);
    }
}

TEST_CASE("REN-40-I1: meshlet builder has good vertex reuse on a sphere",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(32U, 16U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    mp::MeshletBuildOptions opts;
    mp::MeshletBuildResult  result(&alloc);
    const auto report = mp::build_meshlets(pos.data(), vc, idx.data(),
                                           static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    REQUIRE(report.status == mp::MeshletBuildStatus::Ok);
    REQUIRE(report.avg_vertex_reuse > 1.5F);
}

TEST_CASE("REN-40-I1: meshlet builder determinism",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_grid(13U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    const crd::u32 ic = static_cast<crd::u32>(idx.size());
    mp::MeshletBuildOptions opts;

    mp::MeshletBuildResult r1(&alloc);
    const auto rep1 = mp::build_meshlets(pos.data(), vc, idx.data(), ic, opts, r1, &alloc);

    mp::MeshletBuildResult r2(&alloc);
    const auto rep2 = mp::build_meshlets(pos.data(), vc, idx.data(), ic, opts, r2, &alloc);

    REQUIRE(rep1.meshlet_count == rep2.meshlet_count);
    REQUIRE(r1.meshlet_vertices.size() == r2.meshlet_vertices.size());
    REQUIRE(r1.meshlet_triangles.size() == r2.meshlet_triangles.size());

    for (crd::u32 i = 0; i < static_cast<crd::u32>(r1.meshlets.size()); ++i)
    {
        REQUIRE(r1.meshlets[i].vertex_offset == r2.meshlets[i].vertex_offset);
        REQUIRE(r1.meshlets[i].triangle_offset == r2.meshlets[i].triangle_offset);
        REQUIRE(r1.meshlets[i].vertex_count == r2.meshlets[i].vertex_count);
        REQUIRE(r1.meshlets[i].triangle_count == r2.meshlets[i].triangle_count);
    }
    for (crd::u32 i = 0; i < static_cast<crd::u32>(r1.meshlet_vertices.size()); ++i)
        REQUIRE(r1.meshlet_vertices[i] == r2.meshlet_vertices[i]);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(r1.meshlet_triangles.size()); ++i)
        REQUIRE(r1.meshlet_triangles[i] == r2.meshlet_triangles[i]);
}

TEST_CASE("REN-40-I1: meshlet builder rejects invalid input",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    mp::MeshletBuildOptions    opts;

    SECTION("empty mesh")
    {
        mp::MeshletBuildResult result(&alloc);
        const auto r = mp::build_meshlets(nullptr, 0U, nullptr, 0U, opts, result, &alloc);
        REQUIRE(r.status == mp::MeshletBuildStatus::EmptyMesh);
    }
    SECTION("not triangles")
    {
        crd::f32  p[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        crd::u32  i[2] = {0, 1};
        mp::MeshletBuildResult result(&alloc);
        const auto r = mp::build_meshlets(p, 3U, i, 2U, opts, result, &alloc);
        REQUIRE(r.status == mp::MeshletBuildStatus::NotTriangles);
    }
    SECTION("out of range index")
    {
        crd::f32  p[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        crd::u32  i[3] = {0, 1, 99};
        mp::MeshletBuildResult result(&alloc);
        const auto r = mp::build_meshlets(p, 3U, i, 3U, opts, result, &alloc);
        REQUIRE(r.status == mp::MeshletBuildStatus::InvalidIndex);
    }
}

TEST_CASE("REN-40-I1: single triangle produces one meshlet",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::f32 p[9]  = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    crd::u32 i[3]  = {0U, 1U, 2U};

    mp::MeshletBuildOptions opts;
    mp::MeshletBuildResult  result(&alloc);
    const auto report = mp::build_meshlets(p, 3U, i, 3U, opts, result, &alloc);

    REQUIRE(report.status == mp::MeshletBuildStatus::Ok);
    REQUIRE(report.meshlet_count == 1U);
    REQUIRE(result.meshlets[0].vertex_count == 3U);
    REQUIRE(result.meshlets[0].triangle_count == 1U);
}

TEST_CASE("REN-40-I1: large mesh produces many meshlets with full coverage",
          "[geometry][mesh-processing][meshlet][ren40]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    crd::containers::Array<crd::f32> pos(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    make_sphere(64U, 32U, pos, idx);

    const crd::u32 vc = static_cast<crd::u32>(pos.size() / 3U);
    const crd::u32 tc = static_cast<crd::u32>(idx.size() / 3U);

    mp::MeshletBuildOptions opts;
    mp::MeshletBuildResult  result(&alloc);
    const auto report = mp::build_meshlets(pos.data(), vc, idx.data(),
                                           static_cast<crd::u32>(idx.size()), opts, result, &alloc);

    REQUIRE(report.status == mp::MeshletBuildStatus::Ok);
    REQUIRE(report.triangle_count == tc);
    REQUIRE(report.meshlet_count > 1U);

    crd::u32 sum_tc = 0U;
    for (crd::u32 mi = 0; mi < static_cast<crd::u32>(result.meshlets.size()); ++mi)
        sum_tc += result.meshlets[mi].triangle_count;
    REQUIRE(sum_tc == tc);
}
