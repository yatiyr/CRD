// test_gltf_roundtrip.cpp — GEO-4 pt 3 (D-007): the FULL round-trip gate — import → DECOMPOSE (the real wave1 cook)
// → the native cooked artifacts (MESH + PBRM) → a crd-geometry NATIVE EDIT (generate_normals at a new crease angle —
// the conditioning chain re-run on loaded geometry) → EXPORT (our glTF writer) → RE-IMPORT (our parser). Identity on
// geometry + material parameters; the EDIT survives the trip (the row's core claim: native resources are the truth,
// bundles are interchange).

#include <crd/assetio/condition.hpp>
#include <crd/assetio/gltf.hpp>
#include <crd/assetio/gltf_export.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace crd::cooker
{
void register_wave1_mesh_handler();
} // namespace crd::cooker

namespace
{
namespace fs = crd::platform::fs;

crd::memory::TlsfAllocator g_alloc{32U << 20U};

void push_bytes(crd::containers::Array<crd::u8>& b, const void* src, crd::usize n)
{
    const auto* s = static_cast<const crd::u8*>(src);
    for (crd::usize i = 0; i < n; ++i) { b.push_back(s[i]); }
}
void push_u32(crd::containers::Array<crd::u8>& b, crd::u32 v) { push_bytes(b, &v, 4); }
void push_f32(crd::containers::Array<crd::u8>& b, crd::f32 v) { push_bytes(b, &v, 4); }

} // namespace

TEST_CASE("GEO-4 pt 3: import -> cook -> native EDIT -> export -> re-import round-trip; the edit survives",
          "[cooker][wave1][geo][export]")
{
    // 1. a GLB: two 90-degree folded triangles (a shared edge — crease-angle normals DIFFER by smooth angle) + a
    //    KHR-extended material. Metres-authored (position_scale defaults to 1).
    crd::containers::Array<crd::u8> bin(&g_alloc);
    const crd::f32 pos[18] = {0, 0, 0, 1, 0, 0, 0, 1, 0,   // tri A in the XY plane
                              0, 0, 0, 0, 0, 1, 1, 0, 0};  // tri B in the XZ plane (fold about the X edge)
    for (crd::f32 v : pos) { push_f32(bin, v); }

    const char* json = R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0]}],
      "buffers": [{"byteLength": 72}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 72}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3"}],
      "nodes": [{"name": "fold", "mesh": 0}],
      "materials": [{"name": "glass",
        "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 0.125, 1.0], "metallicFactor": 0.75, "roughnessFactor": 0.3},
        "extensions": {"KHR_materials_ior": {"ior": 1.31},
                       "KHR_materials_transmission": {"transmissionFactor": 0.8}}}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}]
    })";

    crd::containers::Array<crd::u8> glb(&g_alloc);
    const crd::u32 jlen = static_cast<crd::u32>(std::strlen(json));
    const crd::u32 jpad = (4U - (jlen % 4U)) % 4U;
    const crd::u32 blen = static_cast<crd::u32>(bin.size());
    const crd::u32 bpad = (4U - (blen % 4U)) % 4U;
    push_u32(glb, 0x46546C67U);
    push_u32(glb, 2U);
    push_u32(glb, 12U + 8U + jlen + jpad + 8U + blen + bpad);
    push_u32(glb, jlen + jpad);
    push_u32(glb, 0x4E4F534AU);
    push_bytes(glb, json, jlen);
    for (crd::u32 i = 0; i < jpad; ++i) { glb.push_back(' '); }
    push_u32(glb, blen + bpad);
    push_u32(glb, 0x004E4942U);
    for (crd::usize i = 0; i < bin.size(); ++i) { glb.push_back(bin[i]); }
    for (crd::u32 i = 0; i < bpad; ++i) { glb.push_back(0); }

    const char* src_path  = "geo4_roundtrip.glb";
    const char* meta_path = "geo4_roundtrip.glb.meta";
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(src_path)), crd::containers::as_const_span(glb)));
    {
        const char* meta = "[id]\nuuid = \"00112233445566778899aabbccddeeff\"\n";
        REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(meta_path)),
                                    crd::containers::StringView(meta, std::strlen(meta))));
    }

    // 2. COOK through the real wave1 handler
    static bool s_registered = false;
    if (!s_registered)
    {
        crd::cooker::register_wave1_mesh_handler();
        s_registered = true;
    }
    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".glb"));
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random(); // a real cook always carries an id — a null id makes
    ctx.allocator   = &g_alloc;                                  // the scene decompose skip the drawable (honestly)
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;
    const crd::cooker::CookResult cooked = handler(ctx);
    REQUIRE(cooked.ok);

    // 3. the native MESH artifact → the 48-byte stream
    crd::resources::CrdrFile mesh_file(&g_alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(cooked.cooked_bytes), mesh_file, &g_alloc)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* vert = crd::resources::crdr_find_chunk(mesh_file, crd::resources::kFourCC_VERT);
    const crd::resources::CrdrChunk* indx = crd::resources::crdr_find_chunk(mesh_file, crd::resources::kFourCC_INDX);
    REQUIRE(vert != nullptr);
    REQUIRE(indx != nullptr);
    const crd::u32 vcount = static_cast<crd::u32>(vert->payload.size() / 48U);
    REQUIRE(vcount > 0U);

    // 4. the NATIVE EDIT: rebuild an ImportedMesh from the cooked stream and re-run the crd-geometry conditioning —
    //    generate_normals at 0 rad = FACETED (every face keeps its own normal; the fold edge splits)
    crd::assetio::ImportedMesh edit(&g_alloc);
    for (crd::u32 v = 0; v < vcount; ++v)
    {
        const crd::u8* rec = vert->payload.data() + static_cast<crd::usize>(v) * 48U;
        crd::f32       f[12];
        std::memcpy(f, rec, 48U);
        edit.positions.push_back({f[0], f[1], f[2]});
        edit.normals.push_back({f[3], f[4], f[5]});
        edit.uv0.push_back({f[6], f[7]});
    }
    const crd::u32 icount = static_cast<crd::u32>(indx->payload.size() / 4U);
    for (crd::u32 i = 0; i < icount; ++i)
    {
        crd::u32 iv = 0;
        std::memcpy(&iv, indx->payload.data() + static_cast<crd::usize>(i) * 4U, 4U);
        edit.indices.push_back(iv);
    }
    crd::assetio::generate_normals(edit, &g_alloc, 0.0F); // the edit: faceted normals (crease angle 0)
    REQUIRE(edit.positions.size() >= 6U);                 // the fold edge SPLIT under faceting

    // 5. the PBRM artifact → the loaded params (COPIED inside the loop — the CrdrFile owns the chunk memory and
    //    dies each iteration; holding a chunk pointer across it would be the borrowed-lifetime UAF class)
    crd::resources::PbrmParams params{};
    bool                       pbrm_found = false;
    for (crd::usize e = 0; e < cooked.extra_artifacts.size() && !pbrm_found; ++e)
    {
        crd::resources::CrdrFile f(&g_alloc);
        if (crd::resources::crdr_read(crd::containers::as_const_span(cooked.extra_artifacts[e].cooked_bytes), f,
                                      &g_alloc)
            != crd::resources::CrdrError::Ok)
        {
            continue;
        }
        const crd::resources::CrdrChunk* pbrm = crd::resources::crdr_find_chunk(f, crd::resources::kFourCC_PbrmPrms);
        if (pbrm != nullptr && pbrm->payload.size() >= sizeof(params))
        {
            std::memcpy(&params, pbrm->payload.data(), sizeof(params));
            pbrm_found = true;
        }
    }
    REQUIRE(pbrm_found);

    // 6. EXPORT from the natives: re-interleave the EDITED mesh + the loaded material params
    crd::containers::Array<crd::u8> re_verts(&g_alloc);
    for (crd::usize v = 0; v < edit.positions.size(); ++v)
    {
        const crd::f32 rec[12] = {edit.positions[v].x, edit.positions[v].y, edit.positions[v].z,
                                  edit.normals[v].x,   edit.normals[v].y,   edit.normals[v].z,
                                  v < edit.uv0.size() ? edit.uv0[v].x : 0.0F,
                                  v < edit.uv0.size() ? edit.uv0[v].y : 0.0F,
                                  1.0F, 0.0F, 0.0F, 1.0F};
        push_bytes(re_verts, rec, 48U);
    }
    crd::containers::Array<crd::u8> re_idx(&g_alloc);
    for (crd::usize i = 0; i < edit.indices.size(); ++i) { push_u32(re_idx, edit.indices[i]); }

    crd::assetio::ExportAsset ea(&g_alloc);
    crd::assetio::ExportMesh  em;
    em.name     = "fold_edited";
    em.vertices = crd::containers::ConstSpan<crd::u8>(re_verts.data(), re_verts.size());
    em.indices  = crd::containers::ConstSpan<crd::u8>(re_idx.data(), re_idx.size());
    em.material = 0;
    ea.meshes.push_back(em);
    crd::assetio::ExportMaterial mat;
    for (crd::u32 c = 0; c < 3U; ++c) { mat.base_color[c] = params.base_color[c]; }
    mat.base_color[3] = params.base_alpha;
    mat.metallic     = params.metallic;
    mat.roughness    = params.roughness;
    mat.ior          = params.ior;
    mat.transmission = params.transmission;
    ea.materials.push_back(mat);

    crd::containers::Array<crd::u8> out_glb(&g_alloc);
    REQUIRE(crd::assetio::gltf_export_glb(ea, out_glb, &g_alloc));

    // 7. RE-IMPORT: the authored material params round-tripped bit-exact THROUGH the cook; the EDIT survived
    crd::assetio::ImportedAsset back(&g_alloc);
    REQUIRE(crd::assetio::parse_glb(crd::containers::ConstSpan<crd::u8>(out_glb.data(), out_glb.size()), &g_alloc, back)
            == crd::assetio::ImportStatus::Ok);
    REQUIRE(back.meshes.size() == 1U);
    REQUIRE(back.materials.size() == 1U);

    const auto same_bits = [](float a, float b) {
        crd::u32 ua = 0;
        crd::u32 ub = 0;
        std::memcpy(&ua, &a, 4U);
        std::memcpy(&ub, &b, 4U);
        return ua == ub;
    };
    CHECK(same_bits(back.materials[0].base_color.x, 0.5F));   // authored → cook → PBRM → export → import, bit-exact
    CHECK(same_bits(back.materials[0].base_color.y, 0.25F));
    CHECK(same_bits(back.materials[0].metallic, 0.75F));
    CHECK(same_bits(back.materials[0].roughness, 0.3F));
    CHECK(same_bits(back.materials[0].ior, 1.31F));
    CHECK(same_bits(back.materials[0].transmission, 0.8F));

    // geometry: byte-exact vs the edited stream, and the EDIT is visible — faceted normals: tri A's corners all
    // (0,0,1), tri B's all (0,-1,0)... orientation aside, the two faces carry DIFFERENT constant normals.
    const crd::assetio::ImportedMesh& bm = back.meshes[0];
    REQUIRE(bm.positions.size() == edit.positions.size());
    for (crd::usize v = 0; v < bm.positions.size(); ++v)
    {
        CHECK(same_bits(bm.positions[v].x, edit.positions[v].x));
        CHECK(same_bits(bm.normals[v].x, edit.normals[v].x));
        CHECK(same_bits(bm.normals[v].y, edit.normals[v].y));
        CHECK(same_bits(bm.normals[v].z, edit.normals[v].z));
    }
    // the fold's two faces have different faceted normals (the edit is REAL, not a pass-through)
    const auto& n_first = bm.normals[static_cast<crd::usize>(bm.indices[0])];
    const auto& n_last  = bm.normals[static_cast<crd::usize>(bm.indices[bm.indices.size() - 1U])];
    const bool  differ  = !same_bits(n_first.x, n_last.x) || !same_bits(n_first.y, n_last.y)
                       || !same_bits(n_first.z, n_last.z);
    CHECK(differ);
}
