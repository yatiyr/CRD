// tests/cooker/test_texture_cook.cpp — GEO-3 stage 2b gates: the shared texture cook core (sRGB mips filtered in
// LINEAR space — the 188-vs-127 gate; normal-map renormalization; alpha always linear), the .meta [cook] color-space
// options, the stb-free standalone handler (.tga end-to-end + a hand-built PNG through OUR inflate), and the glTF
// texture DECOMPOSE (embedded images → TXTR extra artifacts, color space derived from SLOT usage, stable sidecar ids).

#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/cooker/texture_cook.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/deflate.hpp>
#include <crd/resources/ldr_image.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/png_image.hpp>
#include <crd/resources/resource_id.hpp>

#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
void register_texture_handler();   // texture.cpp — the stb-free standalone image handler
void register_wave1_mesh_handler(); // mesh_wave1.cpp — carries the glTF texture decompose
} // namespace crd::cooker

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc{crd::usize{64} << 20U, nullptr, "texture_cook-tests"};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void register_handlers_once()
{
    static bool registered = false;
    if (!registered)
    {
        crd::cooker::register_texture_handler();
        crd::cooker::register_wave1_mesh_handler();
        registered = true;
    }
}

// ── byte builders ──────────────────────────────────────────────────────────────────────────────────────────────────────

void push_bytes(crd::containers::Array<crd::u8>& b, const void* src, crd::usize n)
{
    const crd::u8* s = static_cast<const crd::u8*>(src);
    for (crd::usize i = 0; i < n; ++i) { b.push_back(s[i]); }
}
void push_u32(crd::containers::Array<crd::u8>& b, crd::u32 v) { push_bytes(b, &v, 4); }
void push_u32_be(crd::containers::Array<crd::u8>& b, crd::u32 v)
{
    b.push_back(static_cast<crd::u8>(v >> 24U));
    b.push_back(static_cast<crd::u8>(v >> 16U));
    b.push_back(static_cast<crd::u8>(v >> 8U));
    b.push_back(static_cast<crd::u8>(v));
}

// a PNG chunk: BE length + type + payload + CRC-32 over (type ‖ payload) — via OUR png_crc32
void add_png_chunk(crd::containers::Array<crd::u8>& out, const char* type, const crd::containers::Array<crd::u8>& payload)
{
    push_u32_be(out, static_cast<crd::u32>(payload.size()));
    crd::containers::Array<crd::u8> crc_input(&g_alloc);
    push_bytes(crc_input, type, 4);
    for (crd::usize i = 0; i < payload.size(); ++i) { crc_input.push_back(payload[i]); }
    push_bytes(out, type, 4);
    for (crd::usize i = 0; i < payload.size(); ++i) { out.push_back(payload[i]); }
    push_u32_be(out, crd::resources::png_crc32(crd::containers::as_const_span(crc_input)));
}

// a 1×1 RGBA8 PNG built with OUR deflate + OUR crc (the codec proves itself through its own stack)
crd::containers::Array<crd::u8> build_png_1x1(crd::u8 r, crd::u8 g, crd::u8 b, crd::u8 a)
{
    crd::containers::Array<crd::u8> png(&g_alloc);
    const crd::u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    push_bytes(png, sig, 8);
    crd::containers::Array<crd::u8> ihdr(&g_alloc);
    push_u32_be(ihdr, 1U); // width
    push_u32_be(ihdr, 1U); // height
    ihdr.push_back(8);     // bit depth
    ihdr.push_back(6);     // color type RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    add_png_chunk(png, "IHDR", ihdr);
    crd::containers::Array<crd::u8> raw(&g_alloc); // one scanline: filter 0 + RGBA
    raw.push_back(0);
    raw.push_back(r);
    raw.push_back(g);
    raw.push_back(b);
    raw.push_back(a);
    auto idat = crd::resources::zlib_deflate(crd::containers::as_const_span(raw), &g_alloc);
    add_png_chunk(png, "IDAT", idat);
    crd::containers::Array<crd::u8> empty(&g_alloc);
    add_png_chunk(png, "IEND", empty);
    return png;
}

// a 2×2 uncompressed 32-bit TGA (type 2, top-left origin), pixels given as RGBA and stored BGRA
crd::containers::Array<crd::u8> build_tga_2x2(const crd::u8 rgba[4][4])
{
    crd::containers::Array<crd::u8> tga(&g_alloc);
    const crd::u8 header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28};
    push_bytes(tga, header, 18);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        tga.push_back(rgba[i][2]); // B
        tga.push_back(rgba[i][1]); // G
        tga.push_back(rgba[i][0]); // R
        tga.push_back(rgba[i][3]); // A
    }
    return tga;
}

// the sRGB-vs-linear probe image: a black/white checker with alpha riding the same pattern
crd::resources::LdrImage make_checker_2x2()
{
    crd::resources::LdrImage img(&g_alloc);
    img.width  = 2;
    img.height = 2;
    img.pixels.resize(16U);
    const crd::u8 px[16] = {255, 255, 255, 255, /**/ 0, 0, 0, 0, /**/ 0, 0, 0, 0, /**/ 255, 255, 255, 255};
    std::memcpy(img.pixels.data(), px, 16U);
    return img;
}

struct TxtrView
{
    crd::u32       width     = 0;
    crd::u32       height    = 0;
    crd::u32       mip_count = 0;
    crd::u8        format    = 0xFF;
    const crd::u8* mip0      = nullptr;
    const crd::u8* mip1      = nullptr;
};

// crack a cooked TXTR (the ADR-0042 layout) — REQUIREs on structure, returns the interesting fields
TxtrView crack_txtr(const crd::containers::Array<crd::u8>& cooked, crd::resources::CrdrFile& file)
{
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(cooked), file, &g_alloc)
            == crd::resources::CrdrError::Ok);
    REQUIRE(file.type_fourcc == crd::resources::kFourCC_TXTR);
    const crd::resources::CrdrChunk* head = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_HEAD);
    REQUIRE(head != nullptr);
    REQUIRE(head->payload.size() == 16U);
    TxtrView v;
    std::memcpy(&v.width, head->payload.data() + 0, 4U);
    std::memcpy(&v.height, head->payload.data() + 4, 4U);
    std::memcpy(&v.mip_count, head->payload.data() + 8, 4U);
    v.format = head->payload[12];
    const crd::resources::CrdrChunk* mip0 = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_MIP0);
    REQUIRE(mip0 != nullptr);
    v.mip0 = mip0->payload.data();
    const crd::resources::CrdrChunk* mip1 = crd::resources::crdr_find_chunk(file, crd::resources::make_mip_fourcc(1));
    if (mip1 != nullptr) { v.mip1 = mip1->payload.data(); }
    return v;
}

void write_bytes_file(const char* path, const crd::containers::Array<crd::u8>& bytes)
{
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(path)), crd::containers::as_const_span(bytes)));
}

void write_text(const char* path, const char* text)
{
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(path)),
                                crd::containers::StringView(text, std::strlen(text))));
}

} // namespace

TEST_CASE("texture cook: sRGB mips filter in LINEAR space -- the 188-vs-127 gate", "[cooker][texture][geo]")
{
    const auto img = make_checker_2x2();

    crd::cooker::TextureCookOptions options; // defaults: sRGB color
    const auto id     = crd::resources::ResourceId::mint_random();
    const auto cooked = crd::cooker::cook_texture_rgba(img, options, id, &g_alloc);
    REQUIRE(!cooked.empty());

    crd::resources::CrdrFile file(&g_alloc);
    const TxtrView           v = crack_txtr(cooked, file);
    CHECK(v.width == 2U);
    CHECK(v.height == 2U);
    CHECK(v.mip_count == 2U);
    CHECK(v.format == 3U); // RGBA8UnormSrgb — the on-disk byte the loader/uploader key on

    // MIP0 is the decoded image VERBATIM (the transfer only matters when filtering)
    CHECK(std::memcmp(v.mip0, img.pixels.data(), 16U) == 0);

    // MIP1: linear-space average of black/white = 0.5 → sRGB-encoded 188. A byte-space box filter (the classic
    // mip-darkening bug) would produce 127. Alpha is COVERAGE — linear — so it averages to 128, not 188.
    REQUIRE(v.mip1 != nullptr);
    CHECK(v.mip1[0] == 188U);
    CHECK(v.mip1[1] == 188U);
    CHECK(v.mip1[2] == 188U);
    CHECK(v.mip1[3] == 128U);
}

TEST_CASE("texture cook: linear data mips average straight -- format byte 0", "[cooker][texture][geo]")
{
    const auto img = make_checker_2x2();

    crd::cooker::TextureCookOptions options;
    options.srgb      = false;
    const auto cooked = crd::cooker::cook_texture_rgba(img, options, crd::resources::ResourceId::mint_random(), &g_alloc);
    REQUIRE(!cooked.empty());

    crd::resources::CrdrFile file(&g_alloc);
    const TxtrView           v = crack_txtr(cooked, file);
    CHECK(v.format == 0U); // RGBA8Unorm
    REQUIRE(v.mip1 != nullptr);
    CHECK(v.mip1[0] == 128U); // (1+1+0+0)/4 = 0.5 → 128, no transfer anywhere
    CHECK(v.mip1[3] == 128U);
}

TEST_CASE("texture cook: normal maps RENORMALIZE downsampled mips", "[cooker][texture][geo]")
{
    // two +X normals (255,128,128) and two +Z normals (128,128,255): the raw average is NOT unit; renormalized and
    // re-encoded it lands at (218,128,218) — the halfway unit vector — never the washed-out (192,128,192) raw box.
    crd::resources::LdrImage img(&g_alloc);
    img.width  = 2;
    img.height = 2;
    img.pixels.resize(16U);
    const crd::u8 px[16] = {255, 128, 128, 255, /**/ 128, 128, 255, 255, /**/ 255, 128, 128, 255, /**/ 128, 128, 255, 255};
    std::memcpy(img.pixels.data(), px, 16U);

    crd::cooker::TextureCookOptions options;
    options.normal_map = true;
    const auto cooked = crd::cooker::cook_texture_rgba(img, options, crd::resources::ResourceId::mint_random(), &g_alloc);
    REQUIRE(!cooked.empty());

    crd::resources::CrdrFile file(&g_alloc);
    const TxtrView           v = crack_txtr(cooked, file);
    CHECK(v.format == 0U); // a normal map is DATA — linear by construction
    REQUIRE(v.mip1 != nullptr);
    CHECK(v.mip1[0] == 218U);
    CHECK(v.mip1[1] == 128U);
    CHECK(v.mip1[2] == 218U);
    CHECK(v.mip1[3] == 255U);
}

TEST_CASE("texture cook: .meta [cook] options parse", "[cooker][texture][geo]")
{
    using crd::cooker::parse_texture_cook_options;
    using crd::containers::StringView;

    SECTION("defaults: sRGB color, not a normal map")
    {
        const auto o = parse_texture_cook_options(StringView(""));
        CHECK(o.srgb);
        CHECK(!o.normal_map);
    }
    SECTION("srgb = false opts into linear data")
    {
        const auto o = parse_texture_cook_options(StringView("[id]\nuuid = \"x\"\n[cook]\nsrgb = false\n"));
        CHECK(!o.srgb);
        CHECK(!o.normal_map);
    }
    SECTION("normal_map = true implies linear even against a stray srgb = true")
    {
        const auto o = parse_texture_cook_options(StringView("[cook]\nsrgb = true\nnormal_map = true\n"));
        CHECK(!o.srgb);
        CHECK(o.normal_map);
    }
    SECTION("keys OUTSIDE [cook] are ignored")
    {
        const auto o = parse_texture_cook_options(StringView("[other]\nsrgb = false\n"));
        CHECK(o.srgb);
    }
}

TEST_CASE("texture handler: .tga end-to-end via ldr_decode -- stb retired", "[cooker][texture][geo]")
{
    register_handlers_once();
    const crd::u8 checker[4][4] = {{255, 255, 255, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}, {255, 255, 255, 255}};
    const auto    tga           = build_tga_2x2(checker);
    const char*   src_path      = "texcook_checker.tga";
    const char*   meta_path     = "texcook_checker.tga.meta";
    write_bytes_file(src_path, tga);

    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".tga"));
    REQUIRE(handler != nullptr);

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;

    { // no .meta → the sRGB-color default; the full chain TGA bytes → OUR decoder → linear-filtered mips
        const crd::cooker::CookResult result = handler(ctx);
        REQUIRE(result.ok);
        CHECK(result.type_fourcc == crd::resources::kFourCC_TXTR);
        CHECK(result.handler_version == 2U); // v2 = the ldr_decode rewrite
        crd::resources::CrdrFile file(&g_alloc);
        const TxtrView           v = crack_txtr(result.cooked_bytes, file);
        CHECK(v.format == 3U);
        REQUIRE(v.mip1 != nullptr);
        CHECK(v.mip1[0] == 188U); // the linear-space filter reached the standalone path
        CHECK(v.mip1[3] == 128U);
    }
    { // .meta srgb=false flips the SAME source to linear data (a fresh CookIO — every cook gets its own seam)
        write_text(meta_path, "[id]\nuuid = \"00112233445566778899aabbccddeeff\"\n[cook]\nsrgb = false\n");
        ctx.meta_path = crd::containers::StringView(meta_path);
        crd::cooker::CookIO meta_io(ctx.source_path, ctx.meta_path, &g_alloc);
        ctx.io = &meta_io;
        const crd::cooker::CookResult result = handler(ctx);
        REQUIRE(result.ok);
        crd::resources::CrdrFile file(&g_alloc);
        const TxtrView           v = crack_txtr(result.cooked_bytes, file);
        CHECK(v.format == 0U);
        REQUIRE(v.mip1 != nullptr);
        CHECK(v.mip1[0] == 128U);
    }

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView(meta_path)));
}

TEST_CASE("texture handler: a hand-built PNG decodes through OUR inflate", "[cooker][texture][geo]")
{
    register_handlers_once();
    const auto  png      = build_png_1x1(10, 20, 30, 255);
    const char* src_path = "texcook_pixel.png";
    write_bytes_file(src_path, png);

    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".png"));
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;

    const crd::cooker::CookResult result = handler(ctx);
    REQUIRE(result.ok);
    crd::resources::CrdrFile file(&g_alloc);
    const TxtrView           v = crack_txtr(result.cooked_bytes, file);
    CHECK(v.width == 1U);
    CHECK(v.mip_count == 1U);
    CHECK(v.mip0[0] == 10U);
    CHECK(v.mip0[1] == 20U);
    CHECK(v.mip0[2] == 30U);
    CHECK(v.mip0[3] == 255U);

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
}

TEST_CASE("wave1 glTF: embedded images DECOMPOSE into TXTR extras -- slot-derived color space, STABLE ids",
          "[cooker][wave1][texture][geo]")
{
    register_handlers_once();

    // a GLB: 1 triangle + 2 embedded 1×1 PNGs — albedo (baseColorTexture ⇒ sRGB) and bump (normalTexture ⇒ linear)
    crd::containers::Array<crd::u8> bin(&g_alloc);
    const crd::f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (crd::f32 p : pos) { push_bytes(bin, &p, 4); }
    const auto png_albedo = build_png_1x1(200, 100, 50, 255);
    const auto png_bump   = build_png_1x1(128, 128, 255, 255);
    const crd::usize albedo_off = bin.size();
    for (crd::usize i = 0; i < png_albedo.size(); ++i) { bin.push_back(png_albedo[i]); }
    const crd::usize bump_off = bin.size();
    for (crd::usize i = 0; i < png_bump.size(); ++i) { bin.push_back(png_bump[i]); }

    crd::containers::String json(&g_alloc);
    json.append(R"({"asset": {"version": "2.0"},)");
    {
        char buf[512];
        (void)std::snprintf(buf, sizeof(buf),
                            R"("buffers": [{"byteLength": %u}],)"
                            R"("bufferViews": [)"
                            R"({"buffer": 0, "byteOffset": 0, "byteLength": 36},)"
                            R"({"buffer": 0, "byteOffset": %u, "byteLength": %u},)"
                            R"({"buffer": 0, "byteOffset": %u, "byteLength": %u}],)",
                            static_cast<crd::u32>(bin.size()), static_cast<crd::u32>(albedo_off),
                            static_cast<crd::u32>(png_albedo.size()), static_cast<crd::u32>(bump_off),
                            static_cast<crd::u32>(png_bump.size()));
        json.append(buf);
    }
    json.append(R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],)"
                R"("images": [{"name": "albedo", "bufferView": 1}, {"name": "bump", "bufferView": 2}],)"
                R"("textures": [{"source": 0}, {"source": 1}],)"
                R"("materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},)"
                R"( "normalTexture": {"index": 1}}],)"
                R"("meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}]})");

    crd::containers::Array<crd::u8> glb(&g_alloc);
    const crd::u32 jlen = static_cast<crd::u32>(json.size());
    const crd::u32 jpad = (4U - (jlen % 4U)) % 4U;
    const crd::u32 blen = static_cast<crd::u32>(bin.size());
    const crd::u32 bpad = (4U - (blen % 4U)) % 4U;
    push_u32(glb, 0x46546C67U);
    push_u32(glb, 2U);
    push_u32(glb, 12U + 8U + jlen + jpad + 8U + blen + bpad);
    push_u32(glb, jlen + jpad);
    push_u32(glb, 0x4E4F534AU);
    push_bytes(glb, json.c_str(), jlen);
    for (crd::u32 i = 0; i < jpad; ++i) { glb.push_back(' '); }
    push_u32(glb, blen + bpad);
    push_u32(glb, 0x004E4942U);
    for (crd::usize i = 0; i < bin.size(); ++i) { glb.push_back(bin[i]); }
    for (crd::u32 i = 0; i < bpad; ++i) { glb.push_back(0); }

    const char* src_path = "texcook_textured.glb";
    write_bytes_file(src_path, glb);

    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".glb"));
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;

    const crd::cooker::CookResult result = handler(ctx);
    REQUIRE(result.ok);
    CHECK(result.type_fourcc == crd::resources::kFourCC_MESH); // the mesh is still the main artifact

    // the two image extras (image order) + the stage-4 AUTHORED material
    REQUIRE(result.extra_artifacts.size() == 3U);
    const crd::cooker::ExtraArtifact& ea = result.extra_artifacts[0];
    const crd::cooker::ExtraArtifact& eb = result.extra_artifacts[1];
    const crd::cooker::ExtraArtifact& em = result.extra_artifacts[2];
    CHECK(ea.type_fourcc == crd::resources::kFourCC_TXTR);
    CHECK(eb.type_fourcc == crd::resources::kFourCC_TXTR);
    CHECK(em.type_fourcc == crd::resources::kFourCC_PBRM);

    { // albedo: baseColorTexture ⇒ sRGB (format 3), pixels decoded through OUR PNG path
        crd::resources::CrdrFile file(&g_alloc);
        const TxtrView           v = crack_txtr(ea.cooked_bytes, file);
        CHECK(v.format == 3U);
        CHECK(v.mip0[0] == 200U);
        CHECK(v.mip0[2] == 50U);
    }
    { // bump: normalTexture ⇒ linear data (format 0) — the slot decided, nothing guessed
        crd::resources::CrdrFile file(&g_alloc);
        const TxtrView           v = crack_txtr(eb.cooked_bytes, file);
        CHECK(v.format == 0U);
        CHECK(v.mip0[2] == 255U);
    }

    { // the AUTHORED material: OpenPBR params verbatim + texture SLOTS wired to the cooked TXTR ids
        crd::resources::OpenPbrMaterialLoader loader;
        crd::resources::LoadContext           lctx;
        lctx.id        = em.id;
        lctx.bytes     = crd::containers::as_const_span(em.cooked_bytes);
        lctx.manager   = nullptr;
        lctx.allocator = &g_alloc;
        void* payload  = loader.load(lctx);
        REQUIRE(payload != nullptr);
        auto* mat = static_cast<crd::resources::OpenPbrMaterial*>(payload);
        CHECK(mat->params.metallic == 1.0F);  // glTF defaults survive authoring (metallicFactor default = 1)
        CHECK(mat->params.roughness == 1.0F);
        CHECK(mat->textures.base_color == ea.id); // the slot references the COOKED texture, by ResourceId
        CHECK(mat->textures.normal == eb.id);
        CHECK(mat->textures.metallic_roughness.is_null()); // unbound slots stay null — never a dangling ref
        CHECK(mat->textures.emissive.is_null());
        loader.unload(payload);
    }

    // recook → the sidecar .metas replay the SAME ids (the GEO-1 stability contract: textures AND materials)
    const crd::cooker::CookResult again = handler(ctx);
    REQUIRE(again.ok);
    REQUIRE(again.extra_artifacts.size() == 3U);
    CHECK(again.extra_artifacts[0].id == ea.id);
    CHECK(again.extra_artifacts[1].id == eb.id);
    CHECK(again.extra_artifacts[2].id == em.id);

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView("texcook_textured.glb.tex.0_albedo.meta")));
    (void)fs::remove_file(fs::Path(crd::containers::StringView("texcook_textured.glb.tex.1_bump.meta")));
    (void)fs::remove_file(fs::Path(crd::containers::StringView("texcook_textured.glb.mtl.0_mtl.meta")));
}
