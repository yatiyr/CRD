// Phase 2.7 v1a: stb_image-based texture cook handler.
// Decodes common image formats (PNG/JPG/TGA/BMP) to RGBA8 and generates
// a full mip chain via box filter. Output: CRDR artifact (type TXTR).

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>

#include <algorithm>
#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
namespace
{

constexpr crd::u32  kTextureHandlerVersion = 1U;

// Mirrors TextureFormat::RGBA8Unorm in crd-renderer (same on-disk byte value).
constexpr crd::u8   kTxtrFmtRgba8Unorm    = 0U;

// HEAD chunk layout (16 bytes, little-endian):
//   +0  u32 width
//   +4  u32 height
//   +8  u32 mip_count
//   +12 u8  format (kTxtrFmt*)
//   +13 u8[3] padding (zero)
constexpr crd::usize kHeadChunkBytes = 16U;

struct StbGuard
{
    stbi_uc* data = nullptr;

    explicit StbGuard(stbi_uc* p) noexcept : data(p) {}

    ~StbGuard() noexcept
    {
        if (data != nullptr)
        {
            stbi_image_free(data);
        }
    }

    StbGuard(const StbGuard&)            = delete;
    StbGuard& operator=(const StbGuard&) = delete;
    StbGuard(StbGuard&&)                 = delete;
    StbGuard& operator=(StbGuard&&)      = delete;
};

[[nodiscard]] crd::u32 compute_mip_count(crd::u32 w, crd::u32 h) noexcept
{
    crd::u32 count = 1U;
    while (w > 1U || h > 1U)
    {
        w = (w > 1U) ? w / 2U : 1U;
        h = (h > 1U) ? h / 2U : 1U;
        ++count;
    }
    return count;
}

void downsample_box(const crd::u8* src, crd::u32 src_w, crd::u32 src_h,
                    crd::u8* dst,       crd::u32 dst_w, crd::u32 dst_h) noexcept
{
    for (crd::u32 y = 0U; y < dst_h; ++y)
    {
        for (crd::u32 x = 0U; x < dst_w; ++x)
        {
            const crd::u32 sx0 = std::min(2U * x,       src_w - 1U);
            const crd::u32 sx1 = std::min(2U * x + 1U,  src_w - 1U);
            const crd::u32 sy0 = std::min(2U * y,       src_h - 1U);
            const crd::u32 sy1 = std::min(2U * y + 1U,  src_h - 1U);

            for (crd::u32 c = 0U; c < 4U; ++c)
            {
                const crd::u32 sum =
                    static_cast<crd::u32>(src[(sy0 * src_w + sx0) * 4U + c]) +
                    static_cast<crd::u32>(src[(sy0 * src_w + sx1) * 4U + c]) +
                    static_cast<crd::u32>(src[(sy1 * src_w + sx0) * 4U + c]) +
                    static_cast<crd::u32>(src[(sy1 * src_w + sx1) * 4U + c]);
                dst[(y * dst_w + x) * 4U + c] = static_cast<crd::u8>(sum / 4U);
            }
        }
    }
}

CookResult texture_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::Array<crd::u8> src_bytes(ctx.allocator);
    if (!fs::read_file_binary(fs::Path(ctx.source_path), src_bytes))
    {
        return result;
    }

    int img_w    = 0;
    int img_h    = 0;
    int channels = 0;
    stbi_uc* raw = stbi_load_from_memory(
        src_bytes.data(),
        static_cast<int>(src_bytes.size()),
        &img_w, &img_h, &channels, STBI_rgb_alpha);

    if (raw == nullptr || img_w <= 0 || img_h <= 0)
    {
        stbi_image_free(raw);
        return result;
    }

    const StbGuard guard(raw);

    const crd::u32 base_w    = static_cast<crd::u32>(img_w);
    const crd::u32 base_h    = static_cast<crd::u32>(img_h);
    const crd::u32 mip_count = compute_mip_count(base_w, base_h);

    crd::resources::CrdrWriter writer(ctx.allocator, ctx.id, crd::resources::kFourCC_TXTR);

    // HEAD chunk
    crd::u8 head[kHeadChunkBytes] = {};
    std::memcpy(head + 0,  &base_w,     sizeof(crd::u32));
    std::memcpy(head + 4,  &base_h,     sizeof(crd::u32));
    std::memcpy(head + 8,  &mip_count,  sizeof(crd::u32));
    head[12] = kTxtrFmtRgba8Unorm;
    writer.add_chunk(crd::resources::kFourCC_HEAD,
                     crd::containers::ConstSpan<crd::u8>(head, kHeadChunkBytes));

    // MIP0 from stb_image output (already RGBA)
    {
        const crd::usize base_bytes =
            static_cast<crd::usize>(base_w) * static_cast<crd::usize>(base_h) * 4U;
        writer.add_chunk(crd::resources::kFourCC_MIP0,
                         crd::containers::ConstSpan<crd::u8>(
                             reinterpret_cast<const crd::u8*>(raw), base_bytes));
    }

    // Remaining mip levels — ping-pong two scratch buffers to avoid O(N^2) memory.
    if (mip_count > 1U)
    {
        crd::containers::Array<crd::u8> scratch0(ctx.allocator);
        crd::containers::Array<crd::u8> scratch1(ctx.allocator);

        const crd::usize base_bytes =
            static_cast<crd::usize>(base_w) * static_cast<crd::usize>(base_h) * 4U;
        scratch0.resize(base_bytes);
        std::memcpy(scratch0.data(), raw, base_bytes);

        crd::u32 prev_w = base_w;
        crd::u32 prev_h = base_h;

        for (crd::u32 lvl = 1U; lvl < mip_count; ++lvl)
        {
            const crd::u32   cur_w     = (prev_w > 1U) ? prev_w / 2U : 1U;
            const crd::u32   cur_h     = (prev_h > 1U) ? prev_h / 2U : 1U;
            const crd::usize cur_bytes =
                static_cast<crd::usize>(cur_w) * static_cast<crd::usize>(cur_h) * 4U;

            scratch1.resize(cur_bytes);
            downsample_box(scratch0.data(), prev_w, prev_h,
                           scratch1.data(), cur_w,  cur_h);

            writer.add_chunk(
                crd::resources::make_mip_fourcc(static_cast<crd::u8>(lvl)),
                crd::containers::as_const_span(scratch1));

            scratch0 = std::move(scratch1);
            prev_w   = cur_w;
            prev_h   = cur_h;
        }
    }

    result.type_fourcc     = crd::resources::kFourCC_TXTR;
    result.cooked_bytes    = writer.finish();
    result.handler_version = kTextureHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_texture_handler()
{
    register_cook_handler(".png",  texture_handler);
    register_cook_handler(".jpg",  texture_handler);
    register_cook_handler(".jpeg", texture_handler);
    register_cook_handler(".tga",  texture_handler);
    register_cook_handler(".bmp",  texture_handler);
}

} // namespace crd::cooker
