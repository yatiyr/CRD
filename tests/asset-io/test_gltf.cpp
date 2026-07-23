// tests/asset-io/test_gltf.cpp — GEO-3: the owned glTF parser gates. Hermetic in-memory GLB/glTF fixtures covering:
// the GLB container, plain + INTERLEAVED (strided) accessors, u16 indices, SPARSE substitution, normalized components,
// authored TANGENTs, base64 data-URI buffers, PBR + KHR material parameters, and the failure classes.

#include <catch2/catch_test_macros.hpp>

#include <crd/assetio/gltf.hpp>
#include <crd/assetio/imported_asset.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

namespace aio = crd::assetio;

namespace
{

void push_bytes(crd::containers::Array<crd::u8>& b, const void* src, crd::usize n)
{
    const crd::u8* s = static_cast<const crd::u8*>(src);
    for (crd::usize i = 0; i < n; ++i) { b.push_back(s[i]); }
}
void push_f32(crd::containers::Array<crd::u8>& b, crd::f32 v) { push_bytes(b, &v, 4); }
void push_u32(crd::containers::Array<crd::u8>& b, crd::u32 v) { push_bytes(b, &v, 4); }
void push_u16(crd::containers::Array<crd::u8>& b, crd::u16 v) { push_bytes(b, &v, 2); }

// wrap (json, bin) into a GLB byte stream (4-byte chunk padding per spec)
void build_glb(crd::containers::Array<crd::u8>& out, const char* json, const crd::containers::Array<crd::u8>& bin)
{
    const crd::u32 jlen  = static_cast<crd::u32>(std::strlen(json));
    const crd::u32 jpad  = (4U - (jlen % 4U)) % 4U;
    const crd::u32 blen  = static_cast<crd::u32>(bin.size());
    const crd::u32 bpad  = (4U - (blen % 4U)) % 4U;
    const bool     has_b = blen > 0U;
    const crd::u32 total = 12U + 8U + jlen + jpad + (has_b ? 8U + blen + bpad : 0U);
    push_u32(out, 0x46546C67U); // 'glTF'
    push_u32(out, 2U);
    push_u32(out, total);
    push_u32(out, jlen + jpad);
    push_u32(out, 0x4E4F534AU); // 'JSON'
    push_bytes(out, json, jlen);
    for (crd::u32 i = 0; i < jpad; ++i) { out.push_back(' '); }
    if (has_b)
    {
        push_u32(out, blen + bpad);
        push_u32(out, 0x004E4942U); // 'BIN'
        for (crd::usize i = 0; i < bin.size(); ++i) { out.push_back(bin[i]); }
        for (crd::u32 i = 0; i < bpad; ++i) { out.push_back(0); }
    }
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

TEST_CASE("assetio: GLB triangle -- positions + u16 indices + PBR/KHR material params", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    crd::containers::Array<crd::u8> bin(&alloc);
    // 3 × vec3 positions (36 B) then 3 × u16 indices (6 B)
    const crd::f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (crd::f32 v : pos) { push_f32(bin, v); }
    push_u16(bin, 0);
    push_u16(bin, 1);
    push_u16(bin, 2);

    const char* json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 42}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ],
      "materials": [{
        "name": "glass",
        "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.3, 0.9, 0.5], "metallicFactor": 0.1, "roughnessFactor": 0.2},
        "emissiveFactor": [1, 0.5, 0],
        "extensions": {
          "KHR_materials_emissive_strength": {"emissiveStrength": 4.0},
          "KHR_materials_ior": {"ior": 1.31},
          "KHR_materials_transmission": {"transmissionFactor": 0.9}
        }
      }],
      "meshes": [{"name": "tri", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}]
    })";

    crd::containers::Array<crd::u8> glb(&alloc);
    build_glb(glb, json, bin);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_glb(span_of(glb), &alloc, asset) == aio::ImportStatus::Ok);
    REQUIRE(asset.meshes.size() == 1U);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(std::strcmp(m.name.c_str(), "tri") == 0);
    CHECK(m.positions.size() == 3U);
    CHECK(m.triangle_count() == 1U);
    CHECK(m.positions[1].x == 1.0F);
    CHECK(m.indices[2] == 2U);
    CHECK(m.material == 0);
    CHECK(m.is_consistent());

    REQUIRE(asset.materials.size() == 1U);
    const aio::ImportedMaterial& mat = asset.materials[0];
    CHECK(std::strcmp(mat.name.c_str(), "glass") == 0);
    CHECK(mat.base_color.z == 0.9F);
    CHECK(mat.base_alpha == 0.5F);
    CHECK(mat.metallic == 0.1F);
    CHECK(mat.roughness == 0.2F);
    CHECK(mat.emissive.x == 1.0F);
    CHECK(mat.emissive_strength == 4.0F);
    CHECK(mat.ior == 1.31F);
    CHECK(mat.transmission == 0.9F);
}

TEST_CASE("assetio: glTF INTERLEAVED accessors + normalized u8 UVs + authored TANGENT", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    crd::containers::Array<crd::u8> bin(&alloc);
    // 3 vertices interleaved: pos vec3 (12) + normal vec3 (12) → stride 24; then normalized u8 UV pairs; then u32 tangent-less
    const crd::f32 vtx[18] = {0, 0, 0, /*n*/ 0, 0, 1, 1, 0, 0, /*n*/ 0, 0, 1, 0, 1, 0, /*n*/ 0, 0, 1};
    for (crd::f32 v : vtx) { push_f32(bin, v); }
    const crd::u8 uvs[6] = {0, 0, 255, 0, 0, 255}; // normalized: 0 / 1 / …
    for (crd::u8 v : uvs) { bin.push_back(v); }
    while ((bin.size() % 4U) != 0U) { bin.push_back(0); } // align the next view
    const crd::usize tan_off = bin.size();
    const crd::f32   tan[12] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, -1}; // authored, mixed handedness on purpose
    for (crd::f32 v : tan) { push_f32(bin, v); }

    crd::containers::String json(&alloc);
    json.append(R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": )");
    {
        char num[16];
        (void)std::snprintf(num, sizeof(num), "%u", static_cast<crd::u32>(bin.size()));
        json.append(num);
    }
    json.append(R"(}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 72, "byteStride": 24},
        {"buffer": 0, "byteOffset": 72, "byteLength": 6, "byteStride": 2},
        {"buffer": 0, "byteOffset": )");
    {
        char num[16];
        (void)std::snprintf(num, sizeof(num), "%u", static_cast<crd::u32>(tan_off));
        json.append(num);
    }
    json.append(R"(, "byteLength": 48}
      ],
      "accessors": [
        {"bufferView": 0, "byteOffset": 0,  "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5121, "normalized": true, "count": 3, "type": "VEC2"},
        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3}}]}]
    })");

    crd::containers::Array<crd::u8> glb(&alloc);
    build_glb(glb, json.c_str(), bin);

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_glb(span_of(glb), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.positions.size() == 3U);
    CHECK(m.indices.size() == 3U); // non-indexed → identity
    CHECK(m.has_normals());
    CHECK(m.normals[2].z == 1.0F);       // pulled through the 24-byte stride
    CHECK(m.positions[2].y == 1.0F);
    REQUIRE(m.has_uv0());
    CHECK(m.uv0[1].x == 1.0F);           // 255 normalized → exactly 1.0
    CHECK(m.uv0[2].y == 1.0F);
    REQUIRE(m.tangent.size() == 3U);     // AUTHORED tangents import as-is
    CHECK(m.tangent[2].w == -1.0F);
}

TEST_CASE("assetio: glTF SPARSE accessor substitution", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    crd::containers::Array<crd::u8> bin(&alloc);
    const crd::f32 base[9] = {0, 0, 0, 1, 0, 0, 2, 0, 0}; // 3 base positions on the x-axis
    for (crd::f32 v : base) { push_f32(bin, v); }
    push_u16(bin, 2);                 // sparse index: replace record 2
    push_u16(bin, 0);                 // (pad to keep the values view 4-aligned)
    const crd::f32 subst[3] = {9, 9, 9};
    for (crd::f32 v : subst) { push_f32(bin, v); }

    const char* json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 52}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 2},
        {"buffer": 0, "byteOffset": 40, "byteLength": 12}
      ],
      "accessors": [{
        "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
        "sparse": {
          "count": 1,
          "indices": {"bufferView": 1, "componentType": 5123},
          "values": {"bufferView": 2}
        }
      }],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}]
    })";

    crd::containers::Array<crd::u8> glb(&alloc);
    build_glb(glb, json, bin);
    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_glb(span_of(glb), &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    CHECK(m.positions[0].x == 0.0F);
    CHECK(m.positions[1].x == 1.0F);
    CHECK(m.positions[2].x == 9.0F); // SUBSTITUTED
    CHECK(m.positions[2].y == 9.0F);
}

TEST_CASE("assetio: .gltf with a base64 data-URI buffer", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    // 3 positions (0,0,0)(1,0,0)(0,1,0) as f32 little-endian, base64 of the 36 bytes:
    const char* json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}]
    })";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_gltf(span_of(json), crd::containers::ConstSpan<crd::u8>{}, &alloc, asset) == aio::ImportStatus::Ok);
    const aio::ImportedMesh& m = asset.meshes[0];
    REQUIRE(m.positions.size() == 3U);
    CHECK(m.positions[1].x == 1.0F);
    CHECK(m.positions[2].y == 1.0F);
}

TEST_CASE("assetio: glTF images + material texture SLOTS (embedded / data-URI / external uri)", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    crd::containers::Array<crd::u8> bin(&alloc);
    const crd::f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (crd::f32 v : pos) { push_f32(bin, v); }
    const crd::u8 img_bytes[8] = {0x89, 0x50, 0x4E, 0x47, 1, 2, 3, 4}; // opaque to the parser — the COOK decodes
    for (crd::u8 v : img_bytes) { bin.push_back(v); }

    // images: [0] embedded bufferView · [1] base64 data-URI ("ABCD" → 3 bytes) · [2] external percent-encoded uri
    // textures: [0]→img0 · [1]→img1 · [2]→img2 · [3] has NO core source (extension-only) → slot empty + warning
    const char* json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 44}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 8}
      ],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
      "images": [
        {"name": "albedo", "bufferView": 1},
        {"name": "mr", "uri": "data:image/png;base64,QUJD"},
        {"name": "bump", "uri": "tex%20tures/normal.png"}
      ],
      "textures": [{"source": 0}, {"source": 1}, {"source": 2}, {}],
      "materials": [{
        "pbrMetallicRoughness": {
          "baseColorTexture": {"index": 0},
          "metallicRoughnessTexture": {"index": 1, "texCoord": 1}
        },
        "normalTexture": {"index": 2, "scale": 0.8},
        "occlusionTexture": {"index": 1, "strength": 0.5},
        "emissiveTexture": {"index": 3}
      }],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}]
    })";

    crd::containers::Array<crd::u8> glb(&alloc);
    build_glb(glb, json, bin);
    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_glb(span_of(glb), &alloc, asset) == aio::ImportStatus::Ok);

    REQUIRE(asset.images.size() == 3U);
    CHECK(std::strcmp(asset.images[0].name.c_str(), "albedo") == 0);
    REQUIRE(asset.images[0].bytes.size() == 8U); // embedded: the view bytes verbatim
    CHECK(asset.images[0].bytes[0] == 0x89);
    CHECK(asset.images[0].bytes[7] == 4U);
    CHECK(asset.images[0].uri.size() == 0U);
    REQUIRE(asset.images[1].bytes.size() == 3U); // data-URI: "QUJD" → "ABC"
    CHECK(asset.images[1].bytes[0] == 'A');
    CHECK(asset.images[1].bytes[2] == 'C');
    CHECK(asset.images[2].bytes.size() == 0U); // external: PERCENT-DECODED uri, no bytes (the cook reads the file)
    CHECK(std::strcmp(asset.images[2].uri.c_str(), "tex tures/normal.png") == 0);

    REQUIRE(asset.materials.size() == 1U);
    const aio::ImportedMaterial& mat = asset.materials[0];
    CHECK(mat.base_color_image == 0);
    CHECK(mat.mr_image == 1);
    CHECK(mat.normal_image == 2);
    CHECK(mat.normal_scale == 0.8F);
    CHECK(mat.occlusion_image == 1);
    CHECK(mat.occlusion_strength == 0.5F);
    CHECK(mat.emissive_image == -1);   // texture 3 has no core source → empty slot, warned
    CHECK(asset.warning_count >= 2U);  // sourceless texture + texCoord=1 (uv0-only today)
}

TEST_CASE("assetio: glTF scene graph -- nodes (TRS + MATRIX decompose) / cameras / lights / roots", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    // nodes: [0] root TRS + children · [1] matrix T(1,2,3)·S(2,2,2) with the mesh · [2] 90°-about-Z rotation matrix
    //        · [3] camera node · [4] spot-light node; scene 0 roots = [0]; node 1/2/3/4 are children of 0
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0]}],
      "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
      "cameras": [{"name": "cam", "type": "perspective", "perspective": {"yfov": 0.8, "znear": 0.25, "zfar": 100.0}}],
      "extensions": {"KHR_lights_punctual": {"lights": [
        {"name": "key", "type": "spot", "color": [1.0, 0.5, 0.25], "intensity": 40.0, "range": 12.0,
         "spot": {"innerConeAngle": 0.2, "outerConeAngle": 0.6}}
      ]}},
      "nodes": [
        {"name": "root", "translation": [10, 20, 30], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1],
         "children": [1, 2, 3, 4]},
        {"name": "geo", "matrix": [2,0,0,0, 0,2,0,0, 0,0,2,0, 1,2,3,1], "mesh": 0},
        {"name": "spin", "matrix": [0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1]},
        {"name": "camnode", "camera": 0},
        {"name": "lightnode", "extensions": {"KHR_lights_punctual": {"light": 0}}}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}]
    })";

    aio::ImportedAsset asset(&alloc);
    REQUIRE(aio::parse_gltf(span_of(json), {}, &alloc, asset) == aio::ImportStatus::Ok);

    REQUIRE(asset.nodes.size() == 5U);
    REQUIRE(asset.roots.size() == 1U);
    CHECK(asset.roots[0] == 0U);
    CHECK(asset.nodes[0].children.size() == 4U);
    CHECK(asset.nodes[0].translation.y == 20.0F);

    // matrix node DECOMPOSED: T(1,2,3) · identity rotation · S(2,2,2), and the LIBRARY mesh reference
    const aio::ImportedNode& geo = asset.nodes[1];
    CHECK(geo.translation.x == 1.0F);
    CHECK(geo.translation.z == 3.0F);
    CHECK(geo.scale.x == 2.0F);
    CHECK(geo.scale.y == 2.0F);
    CHECK(std::fabs(geo.rotation.w - 1.0F) < 1.0e-6F);
    CHECK(geo.mesh == 0);
    REQUIRE(asset.meshes.size() == 1U);
    CHECK(asset.meshes[0].source_mesh == 0); // the primitive knows its library index

    // 90° about Z → quaternion (0, 0, sin45°, cos45°)
    const aio::ImportedNode& spin = asset.nodes[2];
    CHECK(std::fabs(spin.rotation.z - 0.70710678F) < 1.0e-5F);
    CHECK(std::fabs(spin.rotation.w - 0.70710678F) < 1.0e-5F);
    CHECK(std::fabs(spin.scale.x - 1.0F) < 1.0e-6F);

    REQUIRE(asset.cameras.size() == 1U);
    CHECK(!asset.cameras[0].is_ortho);
    CHECK(asset.cameras[0].yfov == 0.8F);
    CHECK(asset.cameras[0].znear == 0.25F);
    CHECK(asset.nodes[3].camera == 0);

    REQUIRE(asset.lights.size() == 1U);
    CHECK(asset.lights[0].type == 2U); // spot
    CHECK(asset.lights[0].color.y == 0.5F);
    CHECK(asset.lights[0].intensity == 40.0F);
    CHECK(asset.lights[0].range == 12.0F);
    CHECK(asset.lights[0].inner_cone == 0.2F);
    CHECK(asset.lights[0].outer_cone == 0.6F);
    CHECK(asset.nodes[4].light == 0);
}

TEST_CASE("assetio: glTF failure classes", "[assetio][gltf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("bad GLB magic is NotRecognized")
    {
        crd::containers::Array<crd::u8> b(&alloc);
        push_u32(b, 0x12345678U);
        push_u32(b, 2U);
        push_u32(b, 12U);
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_glb(span_of(b), &alloc, asset) == aio::ImportStatus::NotRecognized);
    }
    SECTION("JSON without `asset` is NotRecognized")
    {
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_gltf(span_of(R"({"meshes": []})"), {}, &alloc, asset) == aio::ImportStatus::NotRecognized);
    }
    SECTION("accessor past the buffer end is Truncated")
    {
        crd::containers::Array<crd::u8> bin(&alloc);
        push_f32(bin, 0.0F); // 4 bytes only
        const char* json = R"({
          "asset": {"version": "2.0"}, "buffers": [{"byteLength": 4}],
          "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
          "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}]
        })";
        crd::containers::Array<crd::u8> glb(&alloc);
        build_glb(glb, json, bin);
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_glb(span_of(glb), &alloc, asset) == aio::ImportStatus::Truncated);
    }
    SECTION("a primitive without POSITION is Malformed")
    {
        const char* json = R"({
          "asset": {"version": "2.0"},
          "meshes": [{"primitives": [{"attributes": {}}]}]
        })";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_gltf(span_of(json), {}, &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("an out-of-range material reference is Malformed")
    {
        const char* json = R"({
          "asset": {"version": "2.0"},
          "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
          "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
          "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 5}]}]
        })";
        aio::ImportedAsset asset(&alloc);
        CHECK(aio::parse_gltf(span_of(json), {}, &alloc, asset) == aio::ImportStatus::Malformed);
    }
    SECTION("non-triangle modes SKIP with a warning, triangles still import")
    {
        const char* json = R"({
          "asset": {"version": "2.0"},
          "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
          "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
          "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
          "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "mode": 0},
            {"attributes": {"POSITION": 0}, "mode": 4}
          ]}]
        })";
        aio::ImportedAsset asset(&alloc);
        REQUIRE(aio::parse_gltf(span_of(json), {}, &alloc, asset) == aio::ImportStatus::Ok);
        CHECK(asset.meshes.size() == 1U); // the POINTS primitive skipped
        CHECK(asset.warning_count >= 1U);
    }
}
