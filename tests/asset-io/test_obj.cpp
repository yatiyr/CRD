// tests/asset-io/test_obj.cpp — GEO-1: OBJ+MTL parser gates. Hermetic in-memory fixtures covering the documented dirty
// classes: corner deduplication across separate index spaces, negative indices, n-gon fan triangulation, object/material
// mesh splitting, MTL resolution (and the missing-.mtl survival path), mixed corners, failure classes.

#include <catch2/catch_test_macros.hpp>

#include <crd/assetio/imported_asset.hpp>
#include <crd/assetio/obj.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

namespace aio = crd::assetio;

namespace
{
[[nodiscard]] crd::containers::ConstSpan<crd::u8> span_of(const char* s)
{
    return crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(s), std::strlen(s));
}
} // namespace

TEST_CASE("assetio: OBJ quad -- corner dedup collapses shared vertices, fan triangulation", "[assetio][obj]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    // a unit quad as ONE 4-corner face (v/vt/vn on every corner); shared corners must dedup: 4 out vertices, 2 triangles
    const char* text = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                       "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                       "vn 0 0 1\n"
                       "f 1/1/1 2/2/1 3/3/1 4/4/1\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    REQUIRE(asset.meshes.size() == 1U);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.positions.size() == 4U); // deduplicated (soup would be 6)
    CHECK(m.triangle_count() == 2U); // 4-gon -> 2 fan triangles
    CHECK(m.is_consistent());
    CHECK(m.has_normals());
    CHECK(m.has_uv0());
    CHECK(m.uv0[2].x == 1.0F);
    CHECK(m.uv0[2].y == 1.0F);
    for (crd::usize i = 0; i < m.normals.size(); ++i) { CHECK(m.normals[i].z == 1.0F); }
    // fan: (0,1,2) (0,2,3)
    CHECK(m.indices[0] == 0U);
    CHECK(m.indices[3] == 0U);
    CHECK(m.indices[4] == 2U);
    CHECK(m.indices[5] == 3U);
}

TEST_CASE("assetio: OBJ negative (relative) indices resolve per spec", "[assetio][obj]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "f -3 -2 -1\n"; // relative: the three most recent vertices, in order

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.triangle_count() == 1U);
    CHECK(m.positions[0].x == 0.0F); // -3 -> vertex 1
    CHECK(m.positions[1].x == 1.0F); // -2 -> vertex 2
    CHECK(m.positions[2].y == 1.0F); // -1 -> vertex 3
    CHECK_FALSE(m.has_normals());    // no vn in the file -> normals cleared, not zero-filled
    CHECK_FALSE(m.has_uv0());
}

TEST_CASE("assetio: OBJ objects + usemtl SPLIT meshes; MTL resolves; missing material survives", "[assetio][obj]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                mtl = "newmtl red\nKd 1 0 0\nNs 98\n"
                                     "newmtl shiny\nKd 0.2 0.2 0.9\nPr 0.15\nPm 1\n";
    const char*                obj = "mtllib scene.mtl\n"
                                     "o first\nusemtl red\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
                                     "o second\nusemtl shiny\n"
                                     "v 0 0 1\nv 1 0 1\nv 0 1 1\nf 4 5 6\n"
                                     "o third\nusemtl nonexistent\n"
                                     "v 0 0 2\nv 1 0 2\nv 0 1 2\nf 7 8 9\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_mtl(span_of(mtl), &alloc, asset) == aio::ImportStatus::Ok);
    REQUIRE(asset.materials.size() == 2U);
    CHECK(asset.materials[0].base_color.x == 1.0F);
    CHECK(asset.materials[0].metallic == 0.0F);
    // Ns 98 -> roughness = sqrt(2/100) ~ 0.1414
    CHECK(std::abs(asset.materials[0].roughness - std::sqrt(0.02F)) < 1e-5F);
    CHECK(asset.materials[1].roughness == 0.15F); // explicit Pr wins
    CHECK(asset.materials[1].metallic == 1.0F);

    REQUIRE(aio::parse_obj(span_of(obj), &alloc, asset) == aio::ImportStatus::Ok);
    REQUIRE(asset.meshes.size() == 3U);
    CHECK(std::strcmp(asset.meshes[0].name.c_str(), "first") == 0);
    CHECK(asset.meshes[0].material == 0);
    CHECK(asset.meshes[1].material == 1);
    CHECK(asset.meshes[2].material == -1);  // unresolved usemtl -> -1, geometry SURVIVES
    CHECK(asset.warning_count >= 1U);       // ... with a warning
    CHECK(asset.meshes[2].positions[0].z == 2.0F);
}

TEST_CASE("assetio: OBJ mixed corners -- missing vn fills zero + warns (dirty file imports)", "[assetio][obj]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "vn 0 0 1\n"
                                      "f 1//1 2//1 3\n"; // third corner has NO normal

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.has_normals()); // some corners had vn -> the array is kept per-vertex
    CHECK(m.normals[0].z == 1.0F);
    CHECK(m.normals[2].z == 0.0F); // the missing one is HONESTLY zero (GEO-2 recomputes)
    CHECK(asset.warning_count >= 1U);
}

TEST_CASE("assetio: OBJ lines/points/unknown keywords skip with warnings, never kill the file", "[assetio][obj]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "p 1\nl 1 2\nweirdkeyword foo bar\n"
                                      "f 1 2 3\n"
                                      "# trailing comment\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    CHECK(asset.meshes.size() == 1U);
    CHECK(asset.meshes[0].triangle_count() == 1U);
    CHECK(asset.warning_count == 3U); // p + l + unknown
}

TEST_CASE("assetio: OBJ failure classes", "[assetio][obj]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("face index out of range is Malformed")
    {
        const char*        text = "v 0 0 0\nv 1 0 0\nf 1 2 3\n"; // index 3 doesn't exist
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("face with fewer than 3 corners is Malformed")
    {
        const char*        text = "v 0 0 0\nv 1 0 0\nf 1 2\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("non-finite vertex is NonFiniteData")
    {
        const char*        text = "v nan 0 0\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::NonFiniteData);
    }
    SECTION("garbage is NotRecognized")
    {
        const char*        text = "this is not an obj file at all\njust some text\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_obj(span_of(text), &alloc, asset) == aio::ImportStatus::NotRecognized);
    }
    SECTION("MTL property before newmtl is Malformed")
    {
        const char*        text = "Kd 1 0 0\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_mtl(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
}
