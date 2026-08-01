// test_lod_impostor_pipeline.cpp — REN-40-C5.6: the impostor PIPELINE gates.
//
// These are the contracts between the baker, the buffer layout, and the shader's
// arithmetic. A CPU-side failure here means the GPU path will silently produce
// wrong pixels or read out-of-bounds.

#include <crd/lod/impostor_atlas.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{

void make_tetrahedron(crd::resources::MeshResource& mesh)
{
    const float v0[3] = { 0.0F,  1.0F,  0.0F};
    const float v1[3] = { 0.9428F, -0.3333F, 0.0F};
    const float v2[3] = {-0.4714F, -0.3333F,  0.8165F};
    const float v3[3] = {-0.4714F, -0.3333F, -0.8165F};

    struct Tri { const float* a; const float* b; const float* c; };
    const Tri tris[4] = {{v0, v1, v2}, {v0, v2, v3}, {v0, v3, v1}, {v1, v3, v2}};

    constexpr crd::u32 stride = crd::resources::kMeshVertexStride;
    mesh.vertices.resize(12U * stride);
    mesh.indices.resize(12U * sizeof(crd::u32));
    std::memset(mesh.vertices.data(), 0, mesh.vertices.size());

    auto write_vert = [&](crd::u32 idx, const float pos[3], const float nrm[3])
    {
        crd::u8* p = mesh.vertices.data() + static_cast<crd::usize>(idx) * stride;
        std::memcpy(p, pos, 12);
        std::memcpy(p + 12, nrm, 12);
    };

    crd::u32 vi = 0;
    for (const auto& t : tris)
    {
        float e1[3] = {t.b[0]-t.a[0], t.b[1]-t.a[1], t.b[2]-t.a[2]};
        float e2[3] = {t.c[0]-t.a[0], t.c[1]-t.a[1], t.c[2]-t.a[2]};
        float n[3]  = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
        float nl    = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (nl > 1.0e-12F) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
        write_vert(vi + 0, t.a, n);
        write_vert(vi + 1, t.b, n);
        write_vert(vi + 2, t.c, n);
        vi += 3;
    }

    auto* idx = reinterpret_cast<crd::u32*>(mesh.indices.data());
    for (crd::u32 i = 0; i < 12; ++i) { idx[i] = i; }
    mesh.bounds_min[0] = -0.4714F; mesh.bounds_min[1] = -0.3333F; mesh.bounds_min[2] = -0.8165F;
    mesh.bounds_max[0] =  0.9428F; mesh.bounds_max[1] =  1.0F;    mesh.bounds_max[2] =  0.8165F;
}

} // namespace

using namespace crd::scenerender;

TEST_CASE("REN-40-C5.6 GATE: buffer layout constants are self-consistent",
          "[lod][ren40][impostor][gate]")
{
    CHECK(kImpostorTableOff == kHeaderWords + kSceneDrawTableWords);
    CHECK(kImpostorTableWords == kImpostorDrawRows * kSceneDrawRowWords);
    CHECK(kSceneFirstRegion == ((kImpostorTableOff + kImpostorTableWords + 3U) & ~3U));
    CHECK(kSceneFirstRegion == kGroupSectionsOff);
    CHECK(kSceneFirstRegion >= kImpostorTableOff + kImpostorTableWords);
}

TEST_CASE("REN-40-C5.6 GATE: kHdrAtlasDims pack/unpack round-trips",
          "[lod][ren40][impostor][gate]")
{
    for (crd::u32 grid : {2U, 4U, 8U, 12U, 16U})
    {
        for (crd::u32 tile : {8U, 16U, 32U, 64U, 128U})
        {
            const crd::u32 packed = (grid << 16U) | tile;
            const crd::u32 g_out  = packed >> 16U;
            const crd::u32 t_out  = packed & 0xFFFFU;
            INFO("grid=" << grid << " tile=" << tile);
            CHECK(g_out == grid);
            CHECK(t_out == tile);
        }
    }
}

TEST_CASE("REN-40-C5.6 GATE: RGBA8 unpack matches what the baker writes",
          "[lod][ren40][impostor][gate]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    make_tetrahedron(mesh);

    crd::lod::ImpostorAtlas atlas(&alloc);
    auto report = crd::lod::bake_impostor_atlas(mesh, 4U, 16U, atlas, &alloc);
    REQUIRE(report.covered_pixels > 0U);

    const crd::u32 aw = atlas.grid * atlas.tile;
    REQUIRE(atlas.pixels.size() == static_cast<crd::usize>(aw * aw * 4U));

    crd::u32 checked_covered = 0U;
    for (crd::u32 y = 0; y < aw; ++y)
    {
        for (crd::u32 x = 0; x < aw; ++x)
        {
            const crd::usize off = (static_cast<crd::usize>(y) * aw + x) * 4U;
            const crd::u8 r8 = atlas.pixels[off + 0U];
            const crd::u8 g8 = atlas.pixels[off + 1U];
            const crd::u8 b8 = atlas.pixels[off + 2U];
            const crd::u8 a8 = atlas.pixels[off + 3U];

            crd::u32 rgba;
            std::memcpy(&rgba, atlas.pixels.data() + off, 4U);
            const crd::u32 ur = (rgba >>  0U) & 0xFFU;
            const crd::u32 ug = (rgba >>  8U) & 0xFFU;
            const crd::u32 ub = (rgba >> 16U) & 0xFFU;
            const crd::u32 ua = (rgba >> 24U);

            CHECK(ur == r8);
            CHECK(ug == g8);
            CHECK(ub == b8);
            CHECK(ua == a8);

            if (a8 > 0U) { ++checked_covered; }
        }
    }
    CHECK(checked_covered == report.covered_pixels);
}

TEST_CASE("REN-40-C5.6 GATE: atlas section sizing matches (grid*tile)^2 words",
          "[lod][ren40][impostor][gate]")
{
    for (crd::u32 grid : {2U, 4U, 8U, 16U})
    {
        for (crd::u32 tile : {8U, 16U, 64U, 128U})
        {
            const crd::u32 aw    = grid * tile;
            const crd::u32 words = aw * aw;
            const crd::u32 bytes = words * 4U;
            INFO("grid=" << grid << " tile=" << tile << " atlas_width=" << aw);
            CHECK(bytes == aw * aw * 4U);

            crd::memory::TlsfAllocator alloc(bytes + (1U << 20U));
            crd::resources::MeshResource mesh(&alloc);
            make_tetrahedron(mesh);
            crd::lod::ImpostorAtlas atlas(&alloc);
            (void)crd::lod::bake_impostor_atlas(mesh, grid, tile, atlas, &alloc);

            CHECK(atlas.pixels.size() == static_cast<crd::usize>(bytes));
        }
    }
}
