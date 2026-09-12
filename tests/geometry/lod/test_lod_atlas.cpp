// test_lod_atlas.cpp — REN-40-C5: octahedral impostor atlas baker.

#include <crd/lod/impostor_atlas.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/mesh_resource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>

namespace
{

// Build a tiny tetrahedron mesh (4 triangles, 12 vertices — unindexed for simplicity).
// The tetrahedron has vertices at unit distance from the origin, giving coverage from every direction.
void make_tetrahedron(crd::resources::MeshResource& mesh)
{
    // tetrahedron vertices (regular, inscribed in unit sphere)
    const float v0[3] = { 0.0F,  1.0F,  0.0F};
    const float v1[3] = { 0.9428F, -0.3333F, 0.0F};
    const float v2[3] = {-0.4714F, -0.3333F,  0.8165F};
    const float v3[3] = {-0.4714F, -0.3333F, -0.8165F};

    struct Tri { const float* a; const float* b; const float* c; };
    const Tri tris[4] = {{v0, v1, v2}, {v0, v2, v3}, {v0, v3, v1}, {v1, v3, v2}};

    constexpr crd::u32 stride = crd::resources::kMeshVertexStride; // 48
    mesh.vertices.resize(12U * stride);
    mesh.indices.resize(12U * sizeof(crd::u32));
    std::memset(mesh.vertices.data(), 0, mesh.vertices.size());

    auto write_vert = [&](crd::u32 idx, const float pos[3], const float nrm[3])
    {
        crd::u8* p = mesh.vertices.data() + static_cast<crd::usize>(idx) * stride;
        std::memcpy(p, pos, 12);      // position: bytes 0-11
        std::memcpy(p + 12, nrm, 12); // normal: bytes 12-23
    };

    crd::u32 vi = 0;
    for (const auto& t : tris)
    {
        // face normal
        float e1[3] = {t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2]};
        float e2[3] = {t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2]};
        float n[3]  = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
        float nl    = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (nl > 1.0e-12F) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
        write_vert(vi + 0, t.a, n);
        write_vert(vi + 1, t.b, n);
        write_vert(vi + 2, t.c, n);
        vi += 3;
    }

    auto* idx = reinterpret_cast<crd::u32*>(mesh.indices.data());
    for (crd::u32 i = 0; i < 12; ++i) { idx[i] = i; }

    // bounds
    mesh.bounds_min[0] = -0.4714F; mesh.bounds_min[1] = -0.3333F; mesh.bounds_min[2] = -0.8165F;
    mesh.bounds_max[0] =  0.9428F; mesh.bounds_max[1] =  1.0F;    mesh.bounds_max[2] =  0.8165F;
}

} // namespace

TEST_CASE("REN-40-C5 GATE: octahedral encode/decode round-trips to sub-degree accuracy",
          "[lod][ren40][impostor]")
{
    // test directions spread over the sphere
    const float dirs[][3] = {
        { 1.0F,  0.0F,  0.0F}, {-1.0F,  0.0F,  0.0F},
        { 0.0F,  1.0F,  0.0F}, { 0.0F, -1.0F,  0.0F},
        { 0.0F,  0.0F,  1.0F}, { 0.0F,  0.0F, -1.0F},
        { 0.577F, 0.577F, 0.577F}, {-0.577F, -0.577F, -0.577F},
    };

    for (const auto& d : dirs)
    {
        float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        float dx = d[0]/len;
        float dy = d[1]/len;
        float dz = d[2]/len;

        float ox = 0.0F;
        float oy = 0.0F;
        crd::lod::oct_encode_cpu(dx, dy, dz, ox, oy);
        CHECK(ox >= -1.0F);
        CHECK(ox <=  1.0F);
        CHECK(oy >= -1.0F);
        CHECK(oy <=  1.0F);

        float rx = 0.0F;
        float ry = 0.0F;
        float rz = 0.0F;
        crd::lod::oct_decode_cpu(ox, oy, rx, ry, rz);

        float dot = dx*rx + dy*ry + dz*rz;
        INFO("dir = (" << dx << ", " << dy << ", " << dz << ") -> oct = (" << ox << ", " << oy
             << ") -> (" << rx << ", " << ry << ", " << rz << ") dot = " << dot);
        CHECK(dot > 0.9998F);
    }
}

TEST_CASE("REN-40-C5 GATE: atlas baker produces a covered atlas from a tetrahedron",
          "[lod][ren40][impostor]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    make_tetrahedron(mesh);

    crd::lod::ImpostorAtlas atlas(&alloc);
    auto report = crd::lod::bake_impostor_atlas(mesh, 4U, 16U, atlas, &alloc);

    CHECK(report.tiles_baked == 16U);
    CHECK(report.total_pixels == 64U * 64U);
    CHECK(report.covered_pixels > 0U);
    CHECK(atlas.grid == 4U);
    CHECK(atlas.tile == 16U);
    // ⭐⭐ REN-41: the atlas is a MIP PYRAMID (level 0 = grid*tile square, then box-downsampled levels), so its buffer
    // is `impostor_atlas_texels` texels across ALL levels — NOT the level-0-only (grid*tile)^2. Use the ONE canonical
    // formula (impostor_atlas.hpp): a hand-duplicated (grid*tile)^2 here is exactly the atlas/reader drift it warns of.
    CHECK(atlas.pixels.size() == static_cast<crd::usize>(crd::lod::impostor_atlas_texels(4U, 16U)) * 4U);

    // at least HALF the tiles should have coverage (a tetrahedron is visible from almost everywhere)
    CHECK(report.tiles_empty < report.tiles_baked / 2U);
}

TEST_CASE("REN-40-C5 GATE: atlas baker with grid=0 produces an empty atlas",
          "[lod][ren40][impostor]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    make_tetrahedron(mesh);

    crd::lod::ImpostorAtlas atlas(&alloc);
    auto report = crd::lod::bake_impostor_atlas(mesh, 0U, 64U, atlas, &alloc);

    CHECK(report.tiles_baked == 0U);
    CHECK(report.covered_pixels == 0U);
}

TEST_CASE("REN-40-C5 GATE: atlas coverage scales with grid resolution",
          "[lod][ren40][impostor]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    make_tetrahedron(mesh);

    crd::lod::ImpostorAtlas atlas4(&alloc);
    auto r4 = crd::lod::bake_impostor_atlas(mesh, 4U, 16U, atlas4, &alloc);

    crd::lod::ImpostorAtlas atlas8(&alloc);
    auto r8 = crd::lod::bake_impostor_atlas(mesh, 8U, 16U, atlas8, &alloc);

    CHECK(r8.tiles_baked == 64U);
    CHECK(r8.total_pixels == 128U * 128U);
    // more tiles = more total coverage (more viewing angles captured)
    CHECK(r8.covered_pixels > r4.covered_pixels);
}
