// smoke_texture.cpp — Phase 2.7 v1a headless smoke test.
//
// Programmatically assembles a 4×4 RGBA8 TXTR CRDR artifact with 3 mip levels,
// wraps it in a PACK, mounts it, registers the TextureResourceLoader, and loads
// the texture via load_sync(). Exits 0 on success, 1 on any failure.
// No GPU required.

#include <crd/containers/array.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/texture_resource.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;
using namespace crd::resources;

static crd::memory::TlsfAllocator g_alloc{256ULL << 20};

// ── Build a TXTR CRDR artifact for a 4×4 image with 3 mip levels ─────────────

static crd::containers::Array<crd::u8> build_txtr_artifact(ResourceId id)
{
    // HEAD chunk (16 bytes)
    constexpr crd::u32 w         = 4U;
    constexpr crd::u32 h         = 4U;
    constexpr crd::u32 k_mip_count  = 3U;
    constexpr crd::u8  fmt_rgba8  = 0U;

    crd::u8 head[16] = {};
    std::memcpy(head + 0, &w,        sizeof(crd::u32));
    std::memcpy(head + 4, &h,        sizeof(crd::u32));
    std::memcpy(head + 8, &k_mip_count, sizeof(crd::u32));
    head[12] = fmt_rgba8;

    CrdrWriter writer(&g_alloc, id, kFourCC_TXTR);
    writer.add_chunk(kFourCC_HEAD, crd::containers::ConstSpan<crd::u8>(head, 16U));

    // Three mip levels: 4×4, 2×2, 1×1 — red pixels (RGBA = 255,0,0,255).
    constexpr crd::u32 mip_widths[3]  = { 4U, 2U, 1U };
    constexpr crd::u32 mip_heights[3] = { 4U, 2U, 1U };

    for (crd::u32 lvl = 0U; lvl < k_mip_count; ++lvl)
    {
        const crd::usize bytes =
            static_cast<crd::usize>(mip_widths[lvl]) *
            static_cast<crd::usize>(mip_heights[lvl]) * 4U;

        crd::containers::Array<crd::u8> pixels(&g_alloc);
        pixels.resize(bytes);
        for (crd::usize i = 0U; i < bytes; i += 4U)
        {
            pixels[i + 0U] = 255U; // R
            pixels[i + 1U] = 0U;   // G
            pixels[i + 2U] = 0U;   // B
            pixels[i + 3U] = 255U; // A
        }
        writer.add_chunk(make_mip_fourcc(static_cast<crd::u8>(lvl)),
                         crd::containers::as_const_span(pixels));
    }

    return writer.finish();
}

// ── Assemble and write a PACK from a single artifact ─────────────────────────

static fs::Path write_pack(ResourceId txtr_id,
                            crd::containers::Array<crd::u8>& crdr_bytes)
{
    const ResourceId pack_id = ResourceId::mint_random();

    const char* art_name = "smoke.txtr";
    crd::containers::Array<crd::u8> pool(&g_alloc);
    for (const char* p = art_name; *p != '\0'; ++p)
    {
        pool.push_back(static_cast<crd::u8>(*p));
    }
    pool.push_back(0U);

    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    ManifestEntry e;
    e.id            = txtr_id;
    e.type_fourcc   = kFourCC_TXTR;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(crdr_bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    // Pass 1: compute pack header size.
    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        const auto b1        = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    // Pass 2: real offsets.
    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::usize i = 0U; i < crdr_bytes.size(); ++i)
    {
        pack_bytes.push_back(crdr_bytes[i]);
    }

    const auto str_id = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("smoke_texture_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");

    const fs::Path path(crd::containers::StringView(tmp.data(), tmp.size()));
    if (!fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_texture: failed to write pack\n");
        std::exit(1);
    }
    return path;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    const ResourceId txtr_id   = ResourceId::mint_random();
    auto             crdr_bytes = build_txtr_artifact(txtr_id);
    const fs::Path   pack_path  = write_pack(txtr_id, crdr_bytes);

    ResourceManager rm(&g_alloc);
    crd::resources::register_texture_loader(&rm);

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_texture: mount_manifest failed\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    auto handle = rm.load_sync<crd::resources::TextureResource>(txtr_id);
    if (!handle.is_ready())
    {
        std::fprintf(stderr, "smoke_texture: load_sync failed (state=%d)\n",
                     static_cast<int>(handle.state()));
        (void)fs::remove_file(pack_path);
        return 1;
    }

    const crd::resources::TextureResource* tex = handle.get();
    if (tex == nullptr)
    {
        std::fprintf(stderr, "smoke_texture: payload is null\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->mip_count != 3U)
    {
        std::fprintf(stderr, "smoke_texture: expected mip_count=3, got %u\n", tex->mip_count);
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->mips.size() != 3U)
    {
        std::fprintf(stderr, "smoke_texture: expected mips.size()=3\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->mips[0].width != 4U || tex->mips[0].height != 4U)
    {
        std::fprintf(stderr, "smoke_texture: mip0 dims wrong (%u×%u)\n",
                     tex->mips[0].width, tex->mips[0].height);
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->mips[1].width != 2U || tex->mips[1].height != 2U)
    {
        std::fprintf(stderr, "smoke_texture: mip1 dims wrong (%u×%u)\n",
                     tex->mips[1].width, tex->mips[1].height);
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->mips[2].width != 1U || tex->mips[2].height != 1U)
    {
        std::fprintf(stderr, "smoke_texture: mip2 dims wrong (%u×%u)\n",
                     tex->mips[2].width, tex->mips[2].height);
        (void)fs::remove_file(pack_path);
        return 1;
    }

    // Verify pixel content: R=255, G=0, B=0, A=255
    if (tex->mips[0].pixels.size() != 4U * 4U * 4U)
    {
        std::fprintf(stderr, "smoke_texture: mip0 pixel buffer size wrong\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->mips[0].pixels[0] != 255U || tex->mips[0].pixels[1] != 0U ||
        tex->mips[0].pixels[2] != 0U   || tex->mips[0].pixels[3] != 255U)
    {
        std::fprintf(stderr, "smoke_texture: mip0 pixel[0] wrong\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (tex->format != crd::resources::TextureFormat::RGBA8Unorm)
    {
        std::fprintf(stderr, "smoke_texture: format != RGBA8Unorm\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    handle = {};
    (void)fs::remove_file(pack_path);

    std::printf("smoke_texture: OK — TextureResource loaded "
                "(mip_count=%u, mip0=%u×%u, format=RGBA8Unorm)\n",
                tex->mip_count, tex->mips[0].width, tex->mips[0].height);
    return 0;
}
