// test_gltf_export.cpp — GEO-4 (D-007): the FIRST EXPORTER's round-trip gate. An ExportAsset (hand-built cooked-layout
// 48-byte vertex streams + a KHR-extended material + a 3-node hierarchy) writes to .glb through OUR writer and reads
// back through OUR importer — the two halves validate each other. Geometry round-trips BYTE-exact (f32 bit patterns
// through the BIN chunk untouched); material parameters and the node TRS/hierarchy value-exact.

#include <crd/assetio/gltf.hpp>
#include <crd/assetio/gltf_export.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/ldr_image.hpp>
#include <crd/resources/png_encode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{

// one cooked 48-byte vertex record
void push_vertex(crd::containers::Array<crd::u8>& out, const float pos[3], const float nrm[3], const float uv[2],
                 const float tan[4])
{
    const auto put = [&](const float* f, crd::u32 n) {
        for (crd::u32 i = 0; i < n; ++i)
        {
            crd::u8 b[4];
            std::memcpy(b, &f[i], 4U);
            for (crd::u32 k = 0; k < 4U; ++k) { out.push_back(b[k]); }
        }
    };
    put(pos, 3U);
    put(nrm, 3U);
    put(uv, 2U);
    put(tan, 4U);
}

void push_index(crd::containers::Array<crd::u8>& out, crd::u32 v)
{
    crd::u8 b[4];
    std::memcpy(b, &v, 4U);
    for (crd::u32 k = 0; k < 4U; ++k) { out.push_back(b[k]); }
}

} // namespace

TEST_CASE("GEO-4: glb export round-trips through our importer -- geometry byte-exact, materials + nodes value-exact",
          "[assetio][gltf][export]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    namespace aio = crd::assetio;

    // ── build the export asset: a triangle mesh with distinctive f32 values (incl. a subnormal-adjacent value and
    //    a negative zero — byte-exactness must survive the JSON/BIN split) ──────────────────────────────────────────
    crd::containers::Array<crd::u8> verts(&alloc);
    const float p0[3] = {0.0F, -0.0F, 0.125F};
    const float p1[3] = {1.5F, 2.25F, -3.75F};
    const float p2[3] = {-0.333333343F, 100.0F, 0.0F};
    const float n0[3] = {0.0F, 0.0F, 1.0F};
    const float uv0[2] = {0.25F, 0.75F};
    const float t0[4] = {1.0F, 0.0F, 0.0F, -1.0F};
    push_vertex(verts, p0, n0, uv0, t0);
    push_vertex(verts, p1, n0, uv0, t0);
    push_vertex(verts, p2, n0, uv0, t0);
    crd::containers::Array<crd::u8> idx(&alloc);
    push_index(idx, 0U);
    push_index(idx, 1U);
    push_index(idx, 2U);

    aio::ExportAsset ea(&alloc);
    aio::ExportMesh  em;
    em.name     = "tri";
    em.vertices = crd::containers::ConstSpan<crd::u8>(verts.data(), verts.size());
    em.indices  = crd::containers::ConstSpan<crd::u8>(idx.data(), idx.size());
    em.material = 0;
    ea.meshes.push_back(em);

    aio::ExportMaterial mat;
    mat.name              = "glassy";
    mat.base_color[0]     = 0.5F;
    mat.base_color[1]     = 1.0F;
    mat.base_color[2]     = 0.25F;
    mat.base_color[3]     = 0.875F;
    mat.metallic          = 0.125F;
    mat.roughness         = 0.375F;
    mat.emissive[1]       = 2.0F;
    mat.emissive_strength = 5.0F; // KHR_materials_emissive_strength
    mat.ior               = 1.31F; // KHR_materials_ior
    mat.transmission      = 0.9F;  // KHR_materials_transmission
    ea.materials.push_back(mat);

    // 3-node hierarchy: root -> child(mesh) + a second root
    aio::ExportNode root;
    root.name           = "rig";
    root.translation[0] = 1.0F;
    ea.nodes.push_back(root);
    aio::ExportNode child;
    child.name        = "part";
    child.parent      = 0;
    child.mesh        = 0;
    child.rotation[2] = 0.70710678F; // 90 deg about Z
    child.rotation[3] = 0.70710678F;
    child.scale[0]    = 2.0F;
    ea.nodes.push_back(child);
    aio::ExportNode lone;
    lone.name = "prop";
    ea.nodes.push_back(lone);

    // ── export → import ────────────────────────────────────────────────────────────────────────────────────────────
    crd::containers::Array<crd::u8> glb(&alloc);
    REQUIRE(aio::gltf_export_glb(ea, glb, &alloc));
    REQUIRE(glb.size() > 12U);

    aio::ImportedAsset ia(&alloc);
    const auto st = aio::parse_glb(crd::containers::ConstSpan<crd::u8>(glb.data(), glb.size()), &alloc, ia);
    REQUIRE(st == aio::ImportStatus::Ok);

    // geometry: BYTE-exact round-trip (bit_cast comparison — -0.0 and exact fractions survive)
    REQUIRE(ia.meshes.size() == 1U);
    const aio::ImportedMesh& im = ia.meshes[0];
    REQUIRE(im.positions.size() == 3U);
    const auto same_bits = [](float a, float b) {
        crd::u32 ua = 0;
        crd::u32 ub = 0;
        std::memcpy(&ua, &a, 4U);
        std::memcpy(&ub, &b, 4U);
        return ua == ub;
    };
    CHECK(same_bits(im.positions[0].x, p0[0]));
    CHECK(same_bits(im.positions[0].y, p0[1])); // -0.0 preserved
    CHECK(same_bits(im.positions[0].z, p0[2]));
    CHECK(same_bits(im.positions[1].x, p1[0]));
    CHECK(same_bits(im.positions[1].z, p1[2]));
    CHECK(same_bits(im.positions[2].x, p2[0])); // 1/3-ish f32 exact through the BIN chunk
    REQUIRE(im.normals.size() == 3U);
    CHECK(same_bits(im.normals[0].z, 1.0F));
    REQUIRE(im.uv0.size() == 3U);
    CHECK(same_bits(im.uv0[0].x, 0.25F));
    REQUIRE(im.tangent.size() == 3U);
    CHECK(same_bits(im.tangent[0].w, -1.0F)); // the authored bitangent sign survives
    REQUIRE(im.indices.size() == 3U);
    CHECK(im.indices[2] == 2U);

    // material: value-exact incl. all three KHR extensions
    REQUIRE(ia.materials.size() == 1U);
    const aio::ImportedMaterial& imat = ia.materials[0];
    CHECK(same_bits(imat.base_color.x, 0.5F));
    CHECK(same_bits(imat.base_color.z, 0.25F));
    CHECK(same_bits(imat.metallic, 0.125F));
    CHECK(same_bits(imat.roughness, 0.375F));
    CHECK(same_bits(imat.emissive.y, 2.0F));
    CHECK(same_bits(imat.emissive_strength, 5.0F));
    CHECK(same_bits(imat.ior, 1.31F));
    CHECK(same_bits(imat.transmission, 0.9F));

    // nodes: TRS + hierarchy + mesh binding
    REQUIRE(ia.nodes.size() == 3U);
    CHECK(same_bits(ia.nodes[0].translation.x, 1.0F));
    REQUIRE(ia.nodes[0].children.size() == 1U);
    CHECK(ia.nodes[0].children[0] == 1U);
    CHECK(same_bits(ia.nodes[1].rotation.z, 0.70710678F));
    CHECK(same_bits(ia.nodes[1].scale.x, 2.0F));
    CHECK(ia.nodes[2].children.size() == 0U);
}

TEST_CASE("GEO-4: glb export REFUSES structural invalidity", "[assetio][gltf][export]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    namespace aio = crd::assetio;
    crd::containers::Array<crd::u8> out(&alloc);

    { // a vertex span that is not a multiple of 48
        aio::ExportAsset ea(&alloc);
        crd::u8          junk[47] = {};
        aio::ExportMesh  em;
        em.vertices = crd::containers::ConstSpan<crd::u8>(junk, sizeof(junk));
        crd::u8 one_index[4] = {};
        em.indices           = crd::containers::ConstSpan<crd::u8>(one_index, 4U);
        ea.meshes.push_back(em);
        CHECK_FALSE(aio::gltf_export_glb(ea, out, &alloc));
    }
    { // an out-of-range index
        aio::ExportAsset                ea(&alloc);
        crd::containers::Array<crd::u8> verts(&alloc);
        const float z3[3] = {0.0F, 0.0F, 0.0F};
        const float z2[2] = {0.0F, 0.0F};
        const float z4[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        push_vertex(verts, z3, z3, z2, z4);
        crd::containers::Array<crd::u8> idx(&alloc);
        push_index(idx, 7U); // only 1 vertex exists
        aio::ExportMesh em;
        em.vertices = crd::containers::ConstSpan<crd::u8>(verts.data(), verts.size());
        em.indices  = crd::containers::ConstSpan<crd::u8>(idx.data(), idx.size());
        ea.meshes.push_back(em);
        CHECK_FALSE(aio::gltf_export_glb(ea, out, &alloc));
    }
    { // a node parented to itself
        aio::ExportAsset ea(&alloc);
        aio::ExportNode  n;
        n.parent = 0;
        ea.nodes.push_back(n);
        CHECK_FALSE(aio::gltf_export_glb(ea, out, &alloc));
    }
}

TEST_CASE("GEO-4 pt 2: an embedded PNG texture round-trips -- encode with OUR encoder, re-import, decode byte-exact",
          "[assetio][gltf][export]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    namespace aio = crd::assetio;
    namespace res = crd::resources;

    // a 2x2 image with 4 distinctive texels (every channel unique -- a scatter/channel-swap bug cannot hide)
    const crd::u8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 10, 20, 30, 40};
    const auto png = res::png_encode_rgba(crd::containers::ConstSpan<crd::u8>(texels, 16U), 2U, 2U, &alloc);
    REQUIRE(png.size() > 8U);

    // a 1-vertex... no -- a minimal valid mesh + a material binding the texture as baseColor
    crd::containers::Array<crd::u8> verts(&alloc);
    const float z3[3] = {0.0F, 0.0F, 0.0F};
    const float z2[2] = {0.0F, 0.0F};
    const float z4[4] = {1.0F, 0.0F, 0.0F, 1.0F};
    push_vertex(verts, z3, z3, z2, z4);
    push_vertex(verts, z3, z3, z2, z4);
    push_vertex(verts, z3, z3, z2, z4);
    crd::containers::Array<crd::u8> idx(&alloc);
    push_index(idx, 0U);
    push_index(idx, 1U);
    push_index(idx, 2U);

    aio::ExportAsset ea(&alloc);
    aio::ExportMesh  em;
    em.vertices = crd::containers::ConstSpan<crd::u8>(verts.data(), verts.size());
    em.indices  = crd::containers::ConstSpan<crd::u8>(idx.data(), idx.size());
    em.material = 0;
    ea.meshes.push_back(em);
    aio::ExportMaterial mat;
    mat.base_color_image = 0;
    mat.normal_image     = 0;
    mat.normal_scale     = 0.5F;
    ea.materials.push_back(mat);
    aio::ExportImage img;
    img.name = "checker";
    img.png  = crd::containers::ConstSpan<crd::u8>(png.data(), png.size());
    ea.images.push_back(img);

    crd::containers::Array<crd::u8> glb(&alloc);
    REQUIRE(aio::gltf_export_glb(ea, glb, &alloc));

    aio::ImportedAsset ia(&alloc);
    REQUIRE(aio::parse_glb(crd::containers::ConstSpan<crd::u8>(glb.data(), glb.size()), &alloc, ia) == aio::ImportStatus::Ok);

    // the image came back embedded; the material slots survived
    REQUIRE(ia.images.size() == 1U);
    REQUIRE(ia.images[0].bytes.size() == png.size());
    REQUIRE(ia.materials.size() == 1U);
    CHECK(ia.materials[0].base_color_image == 0);
    CHECK(ia.materials[0].normal_image == 0);

    // decode the embedded bytes through OUR decoder: every texel byte-exact
    res::LdrImage decoded(&alloc);
    REQUIRE(res::ldr_decode(crd::containers::ConstSpan<crd::u8>(ia.images[0].bytes.data(), ia.images[0].bytes.size()),
                            decoded, &alloc) == res::LdrError::Ok);
    REQUIRE(decoded.width == 2U);
    REQUIRE(decoded.height == 2U);
    REQUIRE(decoded.pixels.size() == 16U);
    for (crd::u32 i = 0; i < 16U; ++i) { CHECK(decoded.pixels[i] == texels[i]); }
}
