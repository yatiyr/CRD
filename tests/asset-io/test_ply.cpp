// tests/asset-io/test_ply.cpp — GEO-1: PLY parser gates. Hermetic fixtures across all three formats (ascii, binary LE,
// binary BE), schema-driven skipping (unknown scalars AND variable-length lists), point clouds, failure classes.

#include <catch2/catch_test_macros.hpp>

#include <crd/assetio/imported_asset.hpp>
#include <crd/assetio/ply.hpp>
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
[[nodiscard]] crd::containers::ConstSpan<crd::u8> span_of(const crd::containers::Array<crd::u8>& b)
{
    return crd::containers::ConstSpan<crd::u8>(b.data(), b.size());
}
void push_bytes(crd::containers::Array<crd::u8>& b, const void* src, crd::usize n, bool swap)
{
    const crd::u8* s = static_cast<const crd::u8*>(src);
    for (crd::usize i = 0; i < n; ++i) { b.push_back(swap ? s[n - 1 - i] : s[i]); }
}
void push_f32e(crd::containers::Array<crd::u8>& b, crd::f32 v, bool swap) { push_bytes(b, &v, 4, swap); }
void push_i32e(crd::containers::Array<crd::u8>& b, crd::i32 v, bool swap) { push_bytes(b, &v, 4, swap); }
void push_text(crd::containers::Array<crd::u8>& b, const char* s)
{
    for (const char* c = s; *c != '\0'; ++c) { b.push_back(static_cast<crd::u8>(*c)); }
}

// A triangle with per-vertex normal + a SKIPPED uchar color triple, faces as (uchar count, int idx) — in either endianness.
void build_binary_tri(crd::containers::Array<crd::u8>& b, bool big_endian)
{
    push_text(b, "ply\n");
    push_text(b, big_endian ? "format binary_big_endian 1.0\n" : "format binary_little_endian 1.0\n");
    push_text(b, "element vertex 3\n");
    push_text(b, "property float x\nproperty float y\nproperty float z\n");
    push_text(b, "property float nx\nproperty float ny\nproperty float nz\n");
    push_text(b, "property uchar red\nproperty uchar green\nproperty uchar blue\n"); // skipped exactly
    push_text(b, "element face 1\n");
    push_text(b, "property list uchar int vertex_indices\n");
    push_text(b, "end_header\n");
    const crd::f32 pos[3][3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    for (int v = 0; v < 3; ++v)
    {
        for (int i = 0; i < 3; ++i) { push_f32e(b, pos[v][i], big_endian); }
        push_f32e(b, 0.0F, big_endian);
        push_f32e(b, 0.0F, big_endian);
        push_f32e(b, 1.0F, big_endian);
        b.push_back(10);
        b.push_back(20);
        b.push_back(30); // rgb (skipped)
    }
    b.push_back(3); // list count (uchar — single byte, endian-invariant)
    push_i32e(b, 0, big_endian);
    push_i32e(b, 1, big_endian);
    push_i32e(b, 2, big_endian);
}

} // namespace

TEST_CASE("assetio: ASCII PLY quad -- schema-driven parse, skipped properties, fan triangulation", "[assetio][ply]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "ply\n"
                                      "format ascii 1.0\n"
                                      "comment made by cerid tests\n"
                                      "element vertex 4\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "property float u\nproperty float v\n"
                                      "property float confidence\n" // skipped scalar
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "end_header\n"
                                      "0 0 0 0 0 0.9\n"
                                      "1 0 0 1 0 0.9\n"
                                      "1 1 0 1 1 0.8\n"
                                      "0 1 0 0 1 0.7\n"
                                      "4 0 1 2 3\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    REQUIRE(asset.meshes.size() == 1U);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.positions.size() == 4U);
    CHECK(m.triangle_count() == 2U); // the quad face fan-triangulated
    CHECK(m.is_consistent());
    CHECK(m.has_uv0());
    CHECK_FALSE(m.has_normals()); // no nx/ny/nz declared
    CHECK(m.uv0[2].x == 1.0F);
    CHECK(m.uv0[2].y == 1.0F);
    CHECK(m.indices[3] == 0U);
    CHECK(m.indices[4] == 2U);
    CHECK(m.indices[5] == 3U);
}

TEST_CASE("assetio: binary_little_endian PLY -- normals + skipped color bytes exact", "[assetio][ply]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> bytes(&alloc);
    build_binary_tri(bytes, false);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_ply(span_of(bytes), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.triangle_count() == 1U);
    CHECK(m.has_normals());
    CHECK(m.positions[1].x == 1.0F);
    CHECK(m.normals[0].z == 1.0F);
    CHECK(m.indices[2] == 2U);
}

TEST_CASE("assetio: binary_big_endian PLY -- byte-swapped body parses identically", "[assetio][ply]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> bytes(&alloc);
    build_binary_tri(bytes, true);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_ply(span_of(bytes), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.triangle_count() == 1U);
    CHECK(m.positions[1].x == 1.0F); // survives the swap
    CHECK(m.positions[2].y == 1.0F);
    CHECK(m.normals[0].z == 1.0F);
}

TEST_CASE("assetio: PLY POINT CLOUD (no face element) is a valid 0-triangle import", "[assetio][ply]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const char*                text = "ply\nformat ascii 1.0\n"
                                      "element vertex 2\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "end_header\n"
                                      "0 0 0\n"
                                      "1 2 3\n";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::Ok);
    CHECK(asset.meshes[0].positions.size() == 2U);
    CHECK(asset.meshes[0].triangle_count() == 0U); // scan/slicer workflows depend on this being VALID
    CHECK(asset.meshes[0].positions[1].z == 3.0F);
}

TEST_CASE("assetio: PLY failure classes", "[assetio][ply]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("no ply magic is NotRecognized")
    {
        const char*        text = "obj\nformat ascii 1.0\nend_header\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::NotRecognized);
    }
    SECTION("missing end_header is Truncated")
    {
        const char*        text = "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::Truncated);
    }
    SECTION("body ends before the declared records is Truncated")
    {
        const char*        text = "ply\nformat ascii 1.0\nelement vertex 2\n"
                                  "property float x\nproperty float y\nproperty float z\nend_header\n"
                                  "0 0 0\n"; // one of two
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::Truncated);
    }
    SECTION("face index out of range is Malformed")
    {
        const char*        text = "ply\nformat ascii 1.0\nelement vertex 3\n"
                                  "property float x\nproperty float y\nproperty float z\n"
                                  "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                                  "0 0 0\n1 0 0\n0 1 0\n"
                                  "3 0 1 7\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("non-finite vertex is NonFiniteData")
    {
        const char*        text = "ply\nformat ascii 1.0\nelement vertex 1\n"
                                  "property float x\nproperty float y\nproperty float z\nend_header\n"
                                  "nan 0 0\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::NonFiniteData);
    }
    SECTION("unknown property type is Malformed")
    {
        const char*        text = "ply\nformat ascii 1.0\nelement vertex 1\nproperty quaternion x\nend_header\n0\n";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_ply(span_of(text), &alloc, asset) == aio::ImportStatus::Malformed);
    }
}
