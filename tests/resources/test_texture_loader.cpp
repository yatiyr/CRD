#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <new>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/texture_resource.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>

namespace fs = crd::platform::fs;
using namespace crd::resources;

alignas(crd::memory::GrowableTlsfAllocator) static unsigned char s_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
static crd::memory::GrowableTlsfAllocator& s_alloc = *::new (s_alloc_buf) crd::memory::GrowableTlsfAllocator(); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// ── Pack assembly helpers ─────────────────────────────────────────────────

struct TxtrArt
{
    ResourceId                      id;
    crd::u32                        type_fourcc;
    crd::containers::Array<crd::u8> crdr_bytes;
    const char*                     name;
};

static fs::Path write_txtr_pack(crd::containers::Array<TxtrArt>& arts)
{
    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8>       pool(&s_alloc);
    crd::containers::Array<ManifestEntry> entries(&s_alloc);

    for (const TxtrArt& art : arts)
    {
        const crd::u32 off = static_cast<crd::u32>(pool.size());
        for (const char* p = art.name; *p != '\0'; ++p)
        {
            pool.push_back(static_cast<crd::u8>(*p));
        }
        pool.push_back(0U);

        ManifestEntry e;
        e.id            = art.id;
        e.type_fourcc   = art.type_fourcc;
        e.flags         = 0U;
        e.blob_offset   = 0U;
        e.blob_size     = static_cast<crd::u64>(art.crdr_bytes.size());
        e.name_strp_idx = off;
        entries.push_back(e);
    }

    // Pass 1: measure pack header size to compute blob offsets.
    {
        CrdrWriter p1(&s_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        const auto b1 = p1.finish();
        crd::u64 pos  = static_cast<crd::u64>(b1.size());
        for (crd::usize i = 0U; i < arts.size(); ++i)
        {
            entries[i].blob_offset = pos;
            pos += static_cast<crd::u64>(arts[i].crdr_bytes.size());
        }
    }

    // Pass 2: assemble with real offsets.
    CrdrWriter p2(&s_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();

    for (const TxtrArt& art : arts)
    {
        for (crd::usize i = 0U; i < art.crdr_bytes.size(); ++i)
        {
            pack_bytes.push_back(art.crdr_bytes[i]);
        }
    }

    const auto str_id = pack_id.to_string(&s_alloc);
    crd::containers::String tmp_name("test_txtr_", &s_alloc);
    tmp_name.append(str_id);
    tmp_name.append(".crdr");

    const fs::Path path(crd::containers::StringView(tmp_name.data(), tmp_name.size()));
    REQUIRE(fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)));
    return path;
}

// Build a TXTR CRDR artifact with HEAD + MIP0..MIP(count-1) for a 4×4 base.
// All pixels filled with `fill` (4 bytes, RGBA).
static crd::containers::Array<crd::u8> make_txtr_artifact(
    ResourceId id, crd::u32 mip_count, const crd::u8 fill[4], crd::u8 format_byte = 0U)
{
    CrdrWriter writer(&s_alloc, id, kFourCC_TXTR);

    // HEAD chunk (16 bytes)
    constexpr crd::u32 base_w = 4U;
    constexpr crd::u32 base_h = 4U;
    crd::u8 head[16]          = {};
    std::memcpy(head + 0, &base_w,     sizeof(crd::u32));
    std::memcpy(head + 4, &base_h,     sizeof(crd::u32));
    std::memcpy(head + 8, &mip_count,  sizeof(crd::u32));
    head[12] = format_byte; // TextureFormat byte value (0 = RGBA8Unorm, 3 = RGBA8UnormSrgb)
    writer.add_chunk(kFourCC_HEAD, crd::containers::ConstSpan<crd::u8>(head, 16U));

    // MIP chunks
    crd::u32 mip_w = base_w;
    crd::u32 mip_h = base_h;
    for (crd::u32 lvl = 0U; lvl < mip_count; ++lvl)
    {
        const crd::usize mip_bytes = static_cast<crd::usize>(mip_w) *
                                     static_cast<crd::usize>(mip_h) * 4U;
        crd::containers::Array<crd::u8> pixels(&s_alloc);
        pixels.resize(mip_bytes);
        for (crd::usize i = 0U; i < mip_bytes; i += 4U)
        {
            pixels[i + 0U] = fill[0];
            pixels[i + 1U] = fill[1];
            pixels[i + 2U] = fill[2];
            pixels[i + 3U] = fill[3];
        }
        writer.add_chunk(make_mip_fourcc(static_cast<crd::u8>(lvl)),
                         crd::containers::as_const_span(pixels));

        mip_w = (mip_w > 1U) ? mip_w / 2U : 1U;
        mip_h = (mip_h > 1U) ? mip_h / 2U : 1U;
    }

    return writer.finish();
}

// ── Test 1: basic loader round-trip ──────────────────────────────────────────

TEST_CASE("TextureResource loads from CRDR artifact", "[resources][texture][loader]")
{
    constexpr crd::u8 k_red[4] = { 255U, 0U, 0U, 255U };

    const ResourceId txtr_id = ResourceId::mint_random();

    crd::containers::Array<TxtrArt> arts(&s_alloc);
    arts.push_back(TxtrArt{
        txtr_id, kFourCC_TXTR,
        make_txtr_artifact(txtr_id, 3U, k_red),
        "test.txtr"
    });

    const fs::Path pack_path = write_txtr_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::resources::register_texture_loader(&rm);

    const MountId mid = rm.mount_manifest(pack_path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<crd::resources::TextureResource>(txtr_id);
    CHECK(handle.is_ready());

    const crd::resources::TextureResource* tex = handle.get();
    REQUIRE(tex != nullptr);
    CHECK(tex->mip_count == 3U);
    CHECK(tex->mips.size() == 3U);
    CHECK(tex->mips[0].width  == 4U);
    CHECK(tex->format == crd::resources::TextureFormat::RGBA8Unorm);

    (void)fs::remove_file(pack_path);
}

// ── Test 1b: the sRGB format byte (GEO-3 stage 2b) ───────────────────────────

TEST_CASE("TextureResource loads the RGBA8UnormSrgb format byte", "[resources][texture][loader][geo]")
{
    constexpr crd::u8 k_grey[4] = { 188U, 188U, 188U, 255U };

    const ResourceId txtr_id = ResourceId::mint_random();
    crd::containers::Array<TxtrArt> arts(&s_alloc);
    arts.push_back(TxtrArt{
        txtr_id, kFourCC_TXTR,
        make_txtr_artifact(txtr_id, 2U, k_grey, 3U), // 3 = RGBA8UnormSrgb (the sRGB-cooked color path)
        "test_srgb.txtr"
    });
    const fs::Path pack_path = write_txtr_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::resources::register_texture_loader(&rm);
    const MountId mid = rm.mount_manifest(pack_path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<crd::resources::TextureResource>(txtr_id);
    CHECK(handle.is_ready());
    const crd::resources::TextureResource* tex = handle.get();
    REQUIRE(tex != nullptr);
    CHECK(tex->format == crd::resources::TextureFormat::RGBA8UnormSrgb);
    CHECK(tex->mip_count == 2U); // the size validation applies to sRGB exactly like unorm

    (void)fs::remove_file(pack_path);
}

// ── Test 2: mip chain dimensions ─────────────────────────────────────────────

TEST_CASE("TextureResource mip chain has correct dimensions", "[resources][texture][mip_dims]")
{
    constexpr crd::u8 green[4] = { 0U, 255U, 0U, 255U };

    const ResourceId txtr_id = ResourceId::mint_random();

    crd::containers::Array<TxtrArt> arts(&s_alloc);
    arts.push_back(TxtrArt{
        txtr_id, kFourCC_TXTR,
        make_txtr_artifact(txtr_id, 3U, green),
        "dims.txtr"
    });

    const fs::Path pack_path = write_txtr_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::resources::register_texture_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::TextureResource>(txtr_id);
    REQUIRE(handle.is_ready());

    const crd::resources::TextureResource* tex = handle.get();
    REQUIRE(tex != nullptr);
    REQUIRE(tex->mips.size() == 3U);

    CHECK(tex->mips[0].width  == 4U);
    CHECK(tex->mips[0].height == 4U);
    CHECK(tex->mips[1].width  == 2U);
    CHECK(tex->mips[1].height == 2U);
    CHECK(tex->mips[2].width  == 1U);
    CHECK(tex->mips[2].height == 1U);

    (void)fs::remove_file(pack_path);
}

// ── Test 3: missing HEAD chunk fails ─────────────────────────────────────────

TEST_CASE("TextureResource fails when HEAD chunk is absent", "[resources][texture][missing_head]")
{
    const ResourceId txtr_id = ResourceId::mint_random();

    // Build a TXTR artifact with only a MIP0 chunk and no HEAD.
    crd::containers::Array<crd::u8> mip0_pixels(&s_alloc);
    mip0_pixels.resize(4U * 4U * 4U, 0U);

    CrdrWriter writer(&s_alloc, txtr_id, kFourCC_TXTR);
    writer.add_chunk(kFourCC_MIP0, crd::containers::as_const_span(mip0_pixels));
    auto crdr_bytes = writer.finish();

    crd::containers::Array<TxtrArt> arts(&s_alloc);
    arts.push_back(TxtrArt{ txtr_id, kFourCC_TXTR, std::move(crdr_bytes), "nohead.txtr" });

    const fs::Path pack_path = write_txtr_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::resources::register_texture_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::TextureResource>(txtr_id);
    CHECK(handle.state() == LoadState::Failed);

    (void)fs::remove_file(pack_path);
}

// ── Test 4: cook a TGA via the cook handler ───────────────────────────────────

TEST_CASE("TextureResource cooked from TGA round-trip", "[resources][texture][cook]")
{
    // Build a minimal 4×4 32-bit TGA (BGRA on disk → RGBA in output).
    // Pixels: BGRA = {0,0,255,255} → stb_image output RGBA = {255,0,0,255} (red).
    constexpr crd::usize tga_header_bytes  = 18U;
    constexpr crd::u32   img_dim          = 4U;
    constexpr crd::usize pixel_bytes      = img_dim * img_dim * 4U;
    constexpr crd::usize tga_total_bytes   = tga_header_bytes + pixel_bytes;

    crd::u8 tga_buf[tga_total_bytes] = {};
    // Header
    tga_buf[2]  = 2U;   // image type: uncompressed true-color
    tga_buf[12] = static_cast<crd::u8>(img_dim); // width lo
    tga_buf[14] = static_cast<crd::u8>(img_dim); // height lo
    tga_buf[16] = 32U;  // bits per pixel
    tga_buf[17] = 0x08U; // image descriptor: 8 alpha bits, bottom-up origin
    // Pixel data: BGRA = {0,0,255,255} for each pixel
    for (crd::usize i = 0U; i < pixel_bytes; i += 4U)
    {
        tga_buf[tga_header_bytes + i + 0U] = 0U;   // B
        tga_buf[tga_header_bytes + i + 1U] = 0U;   // G
        tga_buf[tga_header_bytes + i + 2U] = 255U; // R
        tga_buf[tga_header_bytes + i + 3U] = 255U; // A
    }

    // Write TGA to a temp file.
    const ResourceId tga_id   = ResourceId::mint_random();
    const auto       str_id   = tga_id.to_string(&s_alloc);
    crd::containers::String tga_name("test_cook_", &s_alloc);
    tga_name.append(str_id);
    tga_name.append(".tga");
    const fs::Path tga_path(crd::containers::StringView(tga_name.data(), tga_name.size()));
    REQUIRE(fs::write_file_binary(
        tga_path,
        crd::containers::ConstSpan<crd::u8>(tga_buf, tga_total_bytes)));

    // Register builtin cook handlers (idempotent — safe to call multiple times).
    crd::cooker::register_builtin_handlers();

    const crd::cooker::CookHandlerFn fn =
        crd::cooker::find_cook_handler(".tga");
    REQUIRE(fn != nullptr);

    const ResourceId txtr_id = ResourceId::mint_random();
    crd::cooker::CookContext cook_ctx;
    cook_ctx.source_path = crd::containers::StringView(tga_name.data(), tga_name.size());
    cook_ctx.id          = txtr_id;
    cook_ctx.allocator   = &s_alloc;

    crd::cooker::CookResult cook_result = fn(cook_ctx);
    REQUIRE(cook_result.ok);
    CHECK(cook_result.type_fourcc == kFourCC_TXTR);

    (void)fs::remove_file(tga_path);

    // Assemble a PACK from the cooked bytes and load it.
    crd::containers::Array<TxtrArt> arts(&s_alloc);
    arts.push_back(TxtrArt{
        txtr_id, kFourCC_TXTR,
        std::move(cook_result.cooked_bytes),
        "cooked.txtr"
    });

    const fs::Path pack_path = write_txtr_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::resources::register_texture_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::TextureResource>(txtr_id);
    REQUIRE(handle.is_ready());

    const crd::resources::TextureResource* tex = handle.get();
    REQUIRE(tex != nullptr);
    REQUIRE(!tex->mips.empty());
    REQUIRE(!tex->mips[0].pixels.empty());

    // First pixel R channel should be 255 (red).
    CHECK(tex->mips[0].pixels[0] == 255U);
    CHECK(tex->mips[0].width     == img_dim);
    CHECK(tex->mips[0].height    == img_dim);

    (void)fs::remove_file(pack_path);
}
