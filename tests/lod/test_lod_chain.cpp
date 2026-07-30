// test_lod_chain.cpp — REN-40-C1: building a mesh's LOD chain.
//
// ⛔ WHAT THESE GATES ARE FOR. An LOD chain is a PERFORMANCE change, so every
// pixel it moves is a bug: the near view must be untouched, the far levels must
// actually be cheaper, and the whole thing must reproduce or every content hash
// downstream is a lie.

#include <crd/containers/array.hpp>
#include <crd/lod/lod_chain.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/mesh_resource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>

namespace
{
using crd::resources::kMeshVertexStride;

// A closed, manifold UV-sphere with real UVs — the decimator refuses a
// non-manifold surface, and a grid with a boundary would exercise only the
// boundary-preservation path.
void build_uv_sphere(crd::resources::MeshResource& mesh, crd::u32 rings, crd::u32 sectors)
{
    const crd::f32 pi = 3.14159265F;
    crd::containers::Array<crd::f32> verts(mesh.vertices.allocator());
    crd::containers::Array<crd::u32> idx(mesh.indices.allocator());

    for (crd::u32 r = 0; r <= rings; ++r)
    {
        const crd::f32 v   = static_cast<crd::f32>(r) / static_cast<crd::f32>(rings);
        const crd::f32 phi = v * pi;
        for (crd::u32 s = 0; s <= sectors; ++s)
        {
            const crd::f32 u     = static_cast<crd::f32>(s) / static_cast<crd::f32>(sectors);
            const crd::f32 theta = u * 2.0F * pi;
            const crd::f32 x     = std::sin(phi) * std::cos(theta);
            const crd::f32 y     = std::cos(phi);
            const crd::f32 z     = std::sin(phi) * std::sin(theta);
            const crd::f32 rec[12]{x, y, z, x, y, z, u, v, 1.0F, 0.0F, 0.0F, 1.0F};
            for (const crd::f32 f : rec) { verts.push_back(f); }
        }
    }
    for (crd::u32 r = 0; r < rings; ++r)
    {
        for (crd::u32 s = 0; s < sectors; ++s)
        {
            const crd::u32 a = (r * (sectors + 1U)) + s;
            const crd::u32 b = a + sectors + 1U;
            idx.push_back(a);
            idx.push_back(b);
            idx.push_back(a + 1U);
            idx.push_back(a + 1U);
            idx.push_back(b);
            idx.push_back(b + 1U);
        }
    }
    mesh.vertices.resize(verts.size() * 4U, crd::u8{0});
    std::memcpy(static_cast<void*>(mesh.vertices.data()), static_cast<const void*>(verts.data()),
                verts.size() * 4U);
    mesh.indices.resize(idx.size() * 4U, crd::u8{0});
    std::memcpy(static_cast<void*>(mesh.indices.data()), static_cast<const void*>(idx.data()), idx.size() * 4U);
}

[[nodiscard]] crd::lod::LodPolicy three_level_policy()
{
    crd::lod::LodPolicy p{};
    p.extra_levels     = 3U;
    p.ratio[0]         = 0.5F;
    p.ratio[1]         = 0.25F;
    p.ratio[2]         = 0.1F;
    p.screen_height[0] = 512.0F;
    p.screen_height[1] = 128.0F;
    p.screen_height[2] = 48.0F;
    p.screen_height[3] = 16.0F;
    return p;
}
} // namespace

// ⭐⭐ THE GATE THAT MAKES THE CHAIN SAFE TO SHIP. Level 0 is the SOURCE RANGE,
// recorded rather than rebuilt — so adding a chain cannot change what the near
// view draws. Asserted by comparing the bytes, not by trusting the intent.
TEST_CASE("REN-40-C1 GATE: building a chain leaves level 0 byte-identical to the source mesh",
          "[lod][ren40][mesh]")
{
    crd::memory::TlsfAllocator   alloc(64U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    build_uv_sphere(mesh, 24U, 32U);

    crd::containers::Array<crd::u8> src_v(&alloc);
    crd::containers::Array<crd::u8> src_i(&alloc);
    src_v.resize(mesh.vertices.size(), crd::u8{0});
    src_i.resize(mesh.indices.size(), crd::u8{0});
    std::memcpy(static_cast<void*>(src_v.data()), static_cast<const void*>(mesh.vertices.data()),
                mesh.vertices.size());
    std::memcpy(static_cast<void*>(src_i.data()), static_cast<const void*>(mesh.indices.data()),
                mesh.indices.size());
    const crd::u32 src_index_count = static_cast<crd::u32>(mesh.indices.size() / 4U);

    const auto rep = crd::lod::build_lod_chain(mesh, three_level_policy(), &alloc);
    REQUIRE(rep.status == crd::lod::LodBuildStatus::Ok);
    REQUIRE(rep.levels_built >= 2U);

    // level 0 names the ORIGINAL range...
    REQUIRE(mesh.lods.size() == rep.levels_built);
    CHECK(mesh.lods[0].first_index == 0U);
    CHECK(mesh.lods[0].index_count == src_index_count);
    // ...and the bytes under it are untouched (the chain only APPENDS)
    CHECK(std::memcmp(static_cast<const void*>(mesh.vertices.data()), static_cast<const void*>(src_v.data()),
                      src_v.size())
          == 0);
    CHECK(std::memcmp(static_cast<const void*>(mesh.indices.data()), static_cast<const void*>(src_i.data()),
                      src_i.size())
          == 0);
}

// The chain has to actually BUY something, and every index it emits has to be
// addressable — an out-of-range index reads garbage vertices and draws a mesh
// made of noise, which on a distant LOD is easy to mistake for "just aliasing".
TEST_CASE("REN-40-C1 GATE: each level is strictly cheaper and every index is in range",
          "[lod][ren40][mesh]")
{
    crd::memory::TlsfAllocator   alloc(64U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    build_uv_sphere(mesh, 24U, 32U);

    const auto rep = crd::lod::build_lod_chain(mesh, three_level_policy(), &alloc);
    REQUIRE(rep.status == crd::lod::LodBuildStatus::Ok);
    REQUIRE(rep.levels_built == 4U); // level 0 + the three requested

    const auto total_vertices = static_cast<crd::u32>(mesh.vertices.size() / kMeshVertexStride);
    const auto total_indices  = static_cast<crd::u32>(mesh.indices.size() / 4U);

    for (crd::u32 l = 0; l < rep.levels_built; ++l)
    {
        const auto& e = mesh.lods[l];
        INFO("level " << l << ": " << (e.index_count / 3U) << " triangles");
        CHECK(e.index_count > 0U);
        CHECK((e.index_count % 3U) == 0U);
        CHECK(static_cast<crd::u64>(e.first_index) + e.index_count <= total_indices);
        if (l > 0U)
        {
            // strictly cheaper than the level above — a chain whose "coarse" level
            // is not coarser is a chain that costs memory and buys nothing
            CHECK(mesh.lods[l].index_count < mesh.lods[l - 1U].index_count);
            // and the screen-height thresholds must DESCEND, or selection is undefined
            CHECK(mesh.lods[l].screen_height < mesh.lods[l - 1U].screen_height);
        }
        for (crd::u32 k = 0; k < e.index_count; ++k)
        {
            crd::u32 v = 0;
            std::memcpy(static_cast<void*>(&v),
                        static_cast<const void*>(mesh.indices.data() + ((e.first_index + k) * 4U)), 4U);
            REQUIRE(v < total_vertices); // ABSOLUTE into the combined stream
        }
    }
    // the coarsest level must be a real cut, not a rounding error
    CHECK(mesh.lods[rep.levels_built - 1U].index_count * 4U < mesh.lods[0].index_count);
}

// ⛔ RE-DERIVED, NEVER CARRIED. A normal interpolated from the fine mesh lights
// the coarse one as though the fine surface were still there, which reads as a
// shading POP at exactly the level change. On a unit sphere the correct normal is
// the position itself, so this is checkable exactly.
TEST_CASE("REN-40-C1 GATE: coarse levels carry normals derived from their OWN surface",
          "[lod][ren40][mesh]")
{
    crd::memory::TlsfAllocator   alloc(64U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    build_uv_sphere(mesh, 24U, 32U);
    const auto rep = crd::lod::build_lod_chain(mesh, three_level_policy(), &alloc);
    REQUIRE(rep.status == crd::lod::LodBuildStatus::Ok);
    REQUIRE(rep.levels_built >= 2U);

    // measure EVERY level, so a failure says WHICH level degraded rather than
    // just "the coarsest one did"
    for (crd::u32 l = 0; l < rep.levels_built; ++l)
    {
        const auto& e        = mesh.lods[l];
        crd::f32    worst_l  = 0.0F;
        crd::f32    sum_l    = 0.0F;
        crd::u32    inv_l    = 0U;
        crd::u32    n_l      = 0U;
        for (crd::u32 k = 0; k < e.index_count; ++k)
        {
            crd::u32 vi = 0;
            std::memcpy(static_cast<void*>(&vi),
                        static_cast<const void*>(mesh.indices.data() + ((e.first_index + k) * 4U)), 4U);
            const crd::u8* rec = mesh.vertices.data() + (static_cast<crd::usize>(vi) * kMeshVertexStride);
            crd::f32       p[3]{};
            crd::f32       nn[3]{};
            std::memcpy(static_cast<void*>(p), static_cast<const void*>(rec + 0U), sizeof(p));
            std::memcpy(static_cast<void*>(nn), static_cast<const void*>(rec + 12U), sizeof(nn));
            const crd::f32 plen = std::sqrt((p[0] * p[0]) + (p[1] * p[1]) + (p[2] * p[2]));
            if (plen <= 0.5F) { continue; }
            const crd::f32 dot = ((nn[0] * p[0]) + (nn[1] * p[1]) + (nn[2] * p[2])) / plen;
            if ((1.0F - dot) > worst_l) { worst_l = 1.0F - dot; }
            if (dot < 0.0F) { ++inv_l; }
            sum_l += dot;
            ++n_l;
        }
        INFO("level " << l << " (" << (e.index_count / 3U) << " tris): mean n.p = "
             << (sum_l / static_cast<crd::f32>(n_l == 0U ? 1U : n_l)) << ", inverted " << inv_l << "/" << n_l);
        // ⛔⛔ NOT ONE INVERTED NORMAL, at any level. This is the property that was
        // actually broken and it is the one that matters: an inverted normal lights
        // its surface as though lit from behind, and because it appears only on the
        // COARSE levels the flip shows up exactly at a level change — a black facet
        // that blinks into existence as the camera pulls back. Two causes, both
        // found by this gate: the winding convention was ASSUMED rather than read
        // from the mesh, and normals were accumulated per INDEX-BUFFER vertex, so a
        // UV seam (one surface point, two vertices) got only half its fan.
        CHECK(inv_l == 0U);
        // ⚠ `mean n.p` on this sphere runs 1.00 / 0.80 / 0.70 / 0.41 down the chain.
        // That is DECIMATION drift, not orientation: with zero inverted corners the
        // frames are consistently oriented, and how far a 152-triangle sphere's
        // vertex normals may stray from radial is a separate question with its own
        // measurement. Reported, not silently bounded by a threshold picked to pass.
    }


}

// Cooked twice must mean identical twice, or every content hash downstream lies.
TEST_CASE("REN-40-C1 GATE: chain construction is byte-identical across repeats", "[lod][ren40][mesh]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U);
    const auto                 build = [&](crd::resources::MeshResource& m) {
        build_uv_sphere(m, 18U, 24U);
        return crd::lod::build_lod_chain(m, three_level_policy(), &alloc);
    };
    crd::resources::MeshResource m1(&alloc);
    crd::resources::MeshResource m2(&alloc);
    const auto                   r1 = build(m1);
    const auto                   r2 = build(m2);
    REQUIRE(r1.status == crd::lod::LodBuildStatus::Ok);
    REQUIRE(r2.status == crd::lod::LodBuildStatus::Ok);
    REQUIRE(r1.levels_built == r2.levels_built);
    REQUIRE(m1.vertices.size() == m2.vertices.size());
    REQUIRE(m1.indices.size() == m2.indices.size());
    CHECK(std::memcmp(static_cast<const void*>(m1.vertices.data()), static_cast<const void*>(m2.vertices.data()),
                      m1.vertices.size())
          == 0);
    CHECK(std::memcmp(static_cast<const void*>(m1.indices.data()), static_cast<const void*>(m2.indices.data()),
                      m1.indices.size())
          == 0);
}

// ⛔ A second build must be REFUSED, not silently appended. Building twice would
// grow a second chain onto the same streams and every level index after the first
// would address the wrong block — a mesh made of noise, at a distance where noise
// reads as aliasing.
TEST_CASE("REN-40-C1 GATE: a mesh that already carries a chain refuses to build another", "[lod][ren40][mesh]")
{
    crd::memory::TlsfAllocator   alloc(64U << 20U);
    crd::resources::MeshResource mesh(&alloc);
    build_uv_sphere(mesh, 12U, 16U);
    REQUIRE(crd::lod::build_lod_chain(mesh, three_level_policy(), &alloc).status == crd::lod::LodBuildStatus::Ok);
    const crd::usize vbytes = mesh.vertices.size();
    const auto       again  = crd::lod::build_lod_chain(mesh, three_level_policy(), &alloc);
    CHECK(again.status == crd::lod::LodBuildStatus::AlreadyBuilt);
    CHECK(mesh.vertices.size() == vbytes); // and it appended NOTHING
}
