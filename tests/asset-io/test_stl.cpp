// tests/asset-io/test_stl.cpp — GEO-1: the STL parser gates. All fixtures are constructed IN MEMORY (hermetic — no files),
// covering the real-world dirty classes the parser documents: binary/ASCII auto-detect, the "solid"-prefixed BINARY trap,
// zero-normal recompute, NaN rejection, truncation, malformed grammar. Every parsed mesh must satisfy `is_consistent()`
// and hand a valid TriangleMeshView to the crd-geometry substrate.

#include <catch2/catch_test_macros.hpp>

#include <crd/assetio/imported_asset.hpp>
#include <crd/assetio/stl.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

namespace aio = crd::assetio;

namespace
{

// ── binary STL fixture builder ─────────────────────────────────────────────────────────────────────────────────────────
void push_f32(crd::containers::Array<crd::u8>& b, crd::f32 v)
{
    crd::u8 raw[4];
    std::memcpy(raw, &v, 4);
    for (crd::u8 x : raw) { b.push_back(x); }
}
void push_u32(crd::containers::Array<crd::u8>& b, crd::u32 v)
{
    crd::u8 raw[4];
    std::memcpy(raw, &v, 4);
    for (crd::u8 x : raw) { b.push_back(x); }
}
void push_u16(crd::containers::Array<crd::u8>& b, crd::u16 v)
{
    crd::u8 raw[2];
    std::memcpy(raw, &v, 2);
    for (crd::u8 x : raw) { b.push_back(x); }
}
// header (80 bytes, `text` copied in) + count; caller then pushes triangles.
void push_header(crd::containers::Array<crd::u8>& b, const char* text, crd::u32 count)
{
    const crd::usize len = std::strlen(text);
    for (crd::usize i = 0; i < 80; ++i) { b.push_back(i < len ? static_cast<crd::u8>(text[i]) : crd::u8{0}); }
    push_u32(b, count);
}
void push_tri(crd::containers::Array<crd::u8>& b, const crd::f32* n, const crd::f32* v0, const crd::f32* v1,
              const crd::f32* v2)
{
    for (int i = 0; i < 3; ++i) { push_f32(b, n[i]); }
    for (int i = 0; i < 3; ++i) { push_f32(b, v0[i]); }
    for (int i = 0; i < 3; ++i) { push_f32(b, v1[i]); }
    for (int i = 0; i < 3; ++i) { push_f32(b, v2[i]); }
    push_u16(b, 0);
}

[[nodiscard]] crd::containers::ConstSpan<crd::u8> span_of(const crd::containers::Array<crd::u8>& b)
{
    return crd::containers::ConstSpan<crd::u8>(b.data(), b.size());
}
[[nodiscard]] crd::containers::ConstSpan<crd::u8> span_of(const char* s)
{
    return crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(s), std::strlen(s));
}

} // namespace

TEST_CASE("assetio: binary STL parses -- counts, positions, given normals, consistency", "[assetio][stl]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> bytes(&alloc);

    // a unit quad in the XY plane: two CCW triangles, +Z normals
    const crd::f32 nz[3] = {0.0F, 0.0F, 1.0F};
    const crd::f32 p00[3] = {0.0F, 0.0F, 0.0F};
    const crd::f32 p10[3] = {1.0F, 0.0F, 0.0F};
    const crd::f32 p11[3] = {1.0F, 1.0F, 0.0F};
    const crd::f32 p01[3] = {0.0F, 1.0F, 0.0F};
    push_header(bytes, "cerid test quad", 2);
    push_tri(bytes, nz, p00, p10, p11);
    push_tri(bytes, nz, p00, p11, p01);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_stl(span_of(bytes), &alloc, asset) == aio::ImportStatus::Ok);
    REQUIRE(asset.meshes.size() == 1U);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.triangle_count() == 2U);
    CHECK(m.positions.size() == 6U); // soup: 3 verts per triangle (welding is GEO-2)
    CHECK(m.is_consistent());
    CHECK(m.has_normals());
    CHECK(m.positions[1].x == 1.0F);
    CHECK(m.positions[5].y == 1.0F);
    for (crd::usize i = 0; i < m.normals.size(); ++i) { CHECK(m.normals[i].z == 1.0F); } // the file's normals, kept
    // the crd-geometry seam: a valid view into the substrate
    const auto view = m.as_view();
    CHECK(view.triangle_count() == 2U);
    CHECK_FALSE(view.is_empty());
}

TEST_CASE("assetio: binary STL with ZERO facet normals -- recomputed from winding", "[assetio][stl]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> bytes(&alloc);

    const crd::f32 zero[3] = {0.0F, 0.0F, 0.0F};
    const crd::f32 a[3] = {0.0F, 0.0F, 0.0F};
    const crd::f32 b[3] = {2.0F, 0.0F, 0.0F};
    const crd::f32 c[3] = {0.0F, 2.0F, 0.0F};
    push_header(bytes, "zero normals", 1);
    push_tri(bytes, zero, a, b, c);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_stl(span_of(bytes), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    REQUIRE(m.has_normals());
    // CCW in XY with right-hand rule => +Z unit normal, recomputed + normalized
    for (crd::usize i = 0; i < 3; ++i)
    {
        CHECK(std::abs(m.normals[i].x - 0.0F) < 1e-6F);
        CHECK(std::abs(m.normals[i].y - 0.0F) < 1e-6F);
        CHECK(std::abs(m.normals[i].z - 1.0F) < 1e-6F);
    }
}

TEST_CASE("assetio: BINARY file whose header starts with 'solid' -- the classic trap, detected structurally", "[assetio][stl]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> bytes(&alloc);

    const crd::f32 nz[3] = {0.0F, 0.0F, 1.0F};
    const crd::f32 a[3] = {0.0F, 0.0F, 0.0F};
    const crd::f32 b[3] = {1.0F, 0.0F, 0.0F};
    const crd::f32 c[3] = {0.0F, 1.0F, 0.0F};
    push_header(bytes, "solid exported_from_cad", 1); // header LIES that it is ASCII
    push_tri(bytes, nz, a, b, c);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_stl(span_of(bytes), &alloc, asset) == aio::ImportStatus::Ok);
    CHECK(asset.meshes[0].triangle_count() == 1U); // parsed as BINARY (size equation wins over header text)
    CHECK(asset.meshes[0].positions.size() == 3U);
}

TEST_CASE("assetio: ASCII STL parses -- name captured, keywords case-insensitive", "[assetio][stl]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "Solid my part name\n"
                                      "  FACET NORMAL 0 0 1\n"
                                      "    outer loop\n"
                                      "      vertex 0 0 0\n"
                                      "      vertex 1 0 0\n"
                                      "      vertex 0 1 0\n"
                                      "    endloop\n"
                                      "  endfacet\n"
                                      "endsolid my part name\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.triangle_count() == 1U);
    CHECK(m.is_consistent());
    CHECK(std::strcmp(m.name.c_str(), "my part name") == 0); // multi-word solid name captured
    CHECK(m.positions[1].x == 1.0F);
    CHECK(m.normals[0].z == 1.0F);
}

TEST_CASE("assetio: ASCII STL with an EMPTY solid is a valid 0-triangle mesh", "[assetio][stl]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "solid empty\nendsolid empty\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    CHECK(asset.meshes[0].triangle_count() == 0U);
    CHECK(asset.meshes[0].as_view().is_empty());
}

TEST_CASE("assetio: STL failure classes -- truncated, malformed, non-finite, unrecognized", "[assetio][stl]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("truncated ASCII (no endsolid)")
    {
        const char*        text = "solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::Truncated);
        CHECK(asset.meshes.size() == 0U); // nothing appended on failure
    }
    SECTION("malformed ASCII (bad keyword mid-stream)")
    {
        const char*        text = "solid m\nfacet normal 0 0 1\nouter WRONG\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("malformed ASCII (unparseable float)")
    {
        const char*        text = "solid f\nfacet normal 0 0 x\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("non-finite ASCII vertex")
    {
        const char*        text = "solid n\nfacet normal 0 0 1\nouter loop\nvertex nan 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid n\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::NonFiniteData);
    }
    SECTION("non-finite BINARY vertex")
    {
        crd::containers::Array<crd::u8> bytes(&alloc);
        const crd::f32                  nz[3]  = {0.0F, 0.0F, 1.0F};
        const crd::f32                  bad[3] = {std::nanf(""), 0.0F, 0.0F};
        const crd::f32                  b[3]   = {1.0F, 0.0F, 0.0F};
        const crd::f32                  c[3]   = {0.0F, 1.0F, 0.0F};
        push_header(bytes, "nan", 1);
        push_tri(bytes, nz, bad, b, c);
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(bytes), &alloc, asset) == aio::ImportStatus::NonFiniteData);
    }
    SECTION("garbage bytes are NotRecognized")
    {
        const char*        text = "PK\x03\x04 definitely a zip not an stl";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(text), &alloc, asset) == aio::ImportStatus::NotRecognized);
    }
    SECTION("truncated BINARY (size equation fails, body is not ASCII)")
    {
        crd::containers::Array<crd::u8> bytes(&alloc);
        const crd::f32                  nz[3] = {0.0F, 0.0F, 1.0F};
        const crd::f32                  a[3]  = {0.0F, 0.0F, 0.0F};
        const crd::f32                  b[3]  = {1.0F, 0.0F, 0.0F};
        const crd::f32                  c[3]  = {0.0F, 1.0F, 0.0F};
        push_header(bytes, "trunc", 2); // claims 2 triangles
        push_tri(bytes, nz, a, b, c);   // delivers 1 -> size equation fails -> ASCII attempt -> NotRecognized
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_stl(span_of(bytes), &alloc, asset) == aio::ImportStatus::NotRecognized);
    }
}
