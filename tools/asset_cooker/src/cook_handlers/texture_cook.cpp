// texture_cook.cpp — GEO-3 stage 2b: the shared texture cook core. See texture_cook.hpp for the color-space contract.

#include <crd/cooker/texture_cook.hpp>

#include <crd/containers/span.hpp>
#include <crd/resources/crdr.hpp>

#include <cmath>
#include <cstring>

namespace crd::cooker
{
namespace
{

// Mirrors crd::renderer::TextureFormat on-disk byte values (ADR-0042; the cooker does not link crd-renderer).
constexpr crd::u8 kTxtrFmtRgba8Unorm     = 0U;
constexpr crd::u8 kTxtrFmtRgba8UnormSrgb = 3U;

// HEAD chunk layout (16 bytes, little-endian): u32 width · u32 height · u32 mip_count · u8 format · u8[3] zero
constexpr crd::usize kHeadChunkBytes = 16U;

// ── the exact IEC 61966-2-1 transfer pair ──────────────────────────────────────────────────────────────────────────────

[[nodiscard]] crd::f32 srgb_to_linear(crd::f32 c) noexcept
{
    if (c <= 0.04045F) { return c / 12.92F; }
    return std::pow((c + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] crd::f32 linear_to_srgb(crd::f32 c) noexcept
{
    if (c <= 0.0031308F) { return c * 12.92F; }
    return 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] crd::u8 to_byte(crd::f32 v) noexcept
{
    crd::f32 clamped = v;
    if (clamped < 0.0F) { clamped = 0.0F; }
    if (clamped > 1.0F) { clamped = 1.0F; }
    return static_cast<crd::u8>(std::lround(clamped * 255.0F));
}

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

// RGBA8 → the f32 WORKING domain: RGB through the inverse transfer (identity when linear), alpha always linear.
void decode_to_working(const crd::u8* src, crd::usize px_count, bool srgb, crd::f32* dst) noexcept
{
    for (crd::usize i = 0; i < px_count; ++i)
    {
        for (crd::u32 c = 0; c < 3U; ++c)
        {
            const crd::f32 v = static_cast<crd::f32>(src[i * 4U + c]) / 255.0F;
            dst[i * 4U + c]  = srgb ? srgb_to_linear(v) : v;
        }
        dst[i * 4U + 3U] = static_cast<crd::f32>(src[i * 4U + 3U]) / 255.0F;
    }
}

// f32 working domain → stored RGBA8 bytes (forward transfer on RGB when srgb).
void encode_from_working(const crd::f32* src, crd::usize px_count, bool srgb, crd::u8* dst) noexcept
{
    for (crd::usize i = 0; i < px_count; ++i)
    {
        for (crd::u32 c = 0; c < 3U; ++c)
        {
            const crd::f32 v = src[i * 4U + c];
            dst[i * 4U + c]  = to_byte(srgb ? linear_to_srgb(v) : v);
        }
        dst[i * 4U + 3U] = to_byte(src[i * 4U + 3U]);
    }
}

// 2×2 box in the f32 working domain (edge-clamped for odd dimensions).
void downsample_box_f32(const crd::f32* src, crd::u32 src_w, crd::u32 src_h, crd::f32* dst, crd::u32 dst_w,
                        crd::u32 dst_h) noexcept
{
    for (crd::u32 y = 0U; y < dst_h; ++y)
    {
        for (crd::u32 x = 0U; x < dst_w; ++x)
        {
            const crd::u32 sx0 = 2U * x < src_w - 1U ? 2U * x : src_w - 1U;
            const crd::u32 sx1 = 2U * x + 1U < src_w - 1U ? 2U * x + 1U : src_w - 1U;
            const crd::u32 sy0 = 2U * y < src_h - 1U ? 2U * y : src_h - 1U;
            const crd::u32 sy1 = 2U * y + 1U < src_h - 1U ? 2U * y + 1U : src_h - 1U;
            for (crd::u32 c = 0U; c < 4U; ++c)
            {
                const crd::f32 sum = src[(static_cast<crd::usize>(sy0) * src_w + sx0) * 4U + c]
                                   + src[(static_cast<crd::usize>(sy0) * src_w + sx1) * 4U + c]
                                   + src[(static_cast<crd::usize>(sy1) * src_w + sx0) * 4U + c]
                                   + src[(static_cast<crd::usize>(sy1) * src_w + sx1) * 4U + c];
                dst[(static_cast<crd::usize>(y) * dst_w + x) * 4U + c] = sum * 0.25F;
            }
        }
    }
}

// A downsampled unit-vector average is not unit — renormalize each texel of a normal map ((v·2−1) → unit → back).
void renormalize_normals(crd::f32* px, crd::usize px_count) noexcept
{
    for (crd::usize i = 0; i < px_count; ++i)
    {
        const crd::f32 nx  = px[i * 4U + 0U] * 2.0F - 1.0F;
        const crd::f32 ny  = px[i * 4U + 1U] * 2.0F - 1.0F;
        const crd::f32 nz  = px[i * 4U + 2U] * 2.0F - 1.0F;
        const crd::f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1.0e-6F)
        {
            px[i * 4U + 0U] = (nx / len) * 0.5F + 0.5F;
            px[i * 4U + 1U] = (ny / len) * 0.5F + 0.5F;
            px[i * 4U + 2U] = (nz / len) * 0.5F + 0.5F;
        }
        else // a degenerate average — the flat default, never NaN
        {
            px[i * 4U + 0U] = 0.5F;
            px[i * 4U + 1U] = 0.5F;
            px[i * 4U + 2U] = 1.0F;
        }
    }
}

// ── .meta parsing ([cook] section; the MeshCookOptions grammar) ────────────────────────────────────────────────────────

[[nodiscard]] bool line_bool(crd::containers::StringView text, const char* key, bool fallback) noexcept
{
    const crd::usize klen = std::strlen(key);
    const char*      p    = text.data();
    const crd::usize n    = text.size();
    bool             in_cook = false;
    for (crd::usize i = 0; i < n;)
    {
        crd::usize end = i;
        while (end < n && p[end] != '\n') { ++end; }
        crd::usize b = i;
        while (b < end && (p[b] == ' ' || p[b] == '\t')) { ++b; }
        if (b < end && p[b] == '[')
        {
            in_cook = (end - b >= 6U) && std::strncmp(p + b, "[cook]", 6) == 0;
        }
        else if (in_cook && end - b > klen && std::strncmp(p + b, key, klen) == 0)
        {
            crd::usize v = b + klen;
            while (v < end && (p[v] == ' ' || p[v] == '\t' || p[v] == '=')) { ++v; }
            if (end - v >= 4U && std::strncmp(p + v, "true", 4) == 0) { return true; }
            if (end - v >= 5U && std::strncmp(p + v, "false", 5) == 0) { return false; }
        }
        i = end + 1U;
    }
    return fallback;
}

} // namespace

TextureCookOptions parse_texture_cook_options(crd::containers::StringView meta_text) noexcept
{
    TextureCookOptions o;
    o.srgb       = line_bool(meta_text, "srgb", true);
    o.normal_map = line_bool(meta_text, "normal_map", false);
    if (o.normal_map) { o.srgb = false; } // a normal map is DATA by definition — normal_map wins over a stray srgb=true
    return o;
}

crd::containers::Array<crd::u8> cook_texture_rgba(const crd::resources::LdrImage& image,
                                                  const TextureCookOptions& options, const crd::resources::ResourceId& id,
                                                  crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (!image.valid()) { return empty; }

    const crd::u32 base_w    = image.width;
    const crd::u32 base_h    = image.height;
    const crd::u32 mip_count = compute_mip_count(base_w, base_h);
    const bool     srgb      = options.srgb && !options.normal_map;

    crd::resources::CrdrWriter writer(alloc, id, crd::resources::kFourCC_TXTR);

    crd::u8 head[kHeadChunkBytes] = {};
    std::memcpy(head + 0, &base_w, sizeof(crd::u32));
    std::memcpy(head + 4, &base_h, sizeof(crd::u32));
    std::memcpy(head + 8, &mip_count, sizeof(crd::u32));
    head[12] = srgb ? kTxtrFmtRgba8UnormSrgb : kTxtrFmtRgba8Unorm;
    writer.add_chunk(crd::resources::kFourCC_HEAD, crd::containers::ConstSpan<crd::u8>(head, kHeadChunkBytes));

    // MIP0 = the decoded bytes verbatim (the transfer function only matters when FILTERING).
    writer.add_chunk(crd::resources::kFourCC_MIP0, crd::containers::as_const_span(image.pixels));

    if (mip_count > 1U)
    {
        // The full chain lives in the f32 WORKING domain (linear for sRGB content) so error never re-quantizes
        // through bytes between levels; each level encodes once for storage.
        crd::containers::Array<crd::f32> work0(alloc);
        crd::containers::Array<crd::f32> work1(alloc);
        crd::containers::Array<crd::u8>  stored(alloc);

        const crd::usize base_px = static_cast<crd::usize>(base_w) * base_h;
        work0.resize(base_px * 4U);
        decode_to_working(image.pixels.data(), base_px, srgb, work0.data());

        crd::u32 prev_w = base_w;
        crd::u32 prev_h = base_h;
        for (crd::u32 lvl = 1U; lvl < mip_count; ++lvl)
        {
            const crd::u32   cur_w  = (prev_w > 1U) ? prev_w / 2U : 1U;
            const crd::u32   cur_h  = (prev_h > 1U) ? prev_h / 2U : 1U;
            const crd::usize cur_px = static_cast<crd::usize>(cur_w) * cur_h;

            work1.resize(cur_px * 4U);
            downsample_box_f32(work0.data(), prev_w, prev_h, work1.data(), cur_w, cur_h);
            if (options.normal_map) { renormalize_normals(work1.data(), cur_px); }

            stored.resize(cur_px * 4U);
            encode_from_working(work1.data(), cur_px, srgb, stored.data());
            writer.add_chunk(crd::resources::make_mip_fourcc(static_cast<crd::u8>(lvl)),
                             crd::containers::as_const_span(stored));

            work0  = static_cast<crd::containers::Array<crd::f32>&&>(work1);
            work1  = crd::containers::Array<crd::f32>(alloc);
            prev_w = cur_w;
            prev_h = cur_h;
        }
    }

    return writer.finish();
}

} // namespace crd::cooker
