// tga_image.cpp — the owned TGA decoder. See tga_image.hpp for the coverage contract.

#include <crd/resources/tga_image.hpp>

namespace crd::resources
{
namespace
{

constexpr crd::u32 kMaxDim = 16384;

struct TgaHeader
{
    crd::u8  id_len;
    crd::u8  cmap_type;   // 0 = none, 1 = present
    crd::u8  image_type;  // 1/2/3 raw, 9/10/11 RLE (palette/truecolor/gray)
    crd::u16 cmap_first;
    crd::u16 cmap_len;
    crd::u8  cmap_bits;   // 16/24/32
    crd::u16 x0;
    crd::u16 y0;
    crd::u16 w;
    crd::u16 h;
    crd::u8  bpp;         // 8/16/24/32
    crd::u8  desc;        // bit 4 = right-to-left, bit 5 = top-origin
};

[[nodiscard]] crd::u16 le16(const crd::u8* p) noexcept { return static_cast<crd::u16>(p[0] | (p[1] << 8U)); }

[[nodiscard]] bool read_header(crd::containers::ConstSpan<crd::u8> bytes, TgaHeader& h) noexcept
{
    if (bytes.size() < 18U) { return false; }
    const crd::u8* p = bytes.data();
    h.id_len         = p[0];
    h.cmap_type      = p[1];
    h.image_type     = p[2];
    h.cmap_first     = le16(p + 3);
    h.cmap_len       = le16(p + 5);
    h.cmap_bits      = p[7];
    h.x0             = le16(p + 8);
    h.y0             = le16(p + 10);
    h.w              = le16(p + 12);
    h.h              = le16(p + 14);
    h.bpp            = p[16];
    h.desc           = p[17];
    return true;
}

[[nodiscard]] bool header_consistent(const TgaHeader& h) noexcept
{
    const bool type_ok = h.image_type == 1 || h.image_type == 2 || h.image_type == 3 || h.image_type == 9
                      || h.image_type == 10 || h.image_type == 11;
    if (!type_ok || h.w == 0U || h.h == 0U) { return false; }
    const bool is_pal = h.image_type == 1 || h.image_type == 9;
    if (is_pal && (h.cmap_type != 1U || h.cmap_len == 0U || h.bpp != 8U)) { return false; }
    if (!is_pal && h.cmap_type != 0U) { return false; }
    const bool is_gray = h.image_type == 3 || h.image_type == 11;
    if (is_gray && h.bpp != 8U) { return false; }
    if (!is_pal && !is_gray && h.bpp != 16U && h.bpp != 24U && h.bpp != 32U) { return false; }
    if (is_pal && h.cmap_bits != 16U && h.cmap_bits != 24U && h.cmap_bits != 32U) { return false; }
    return true;
}

// one raw pixel record (1/2/3/4 bytes) → RGBA8; `pal` = expanded RGBA palette or nullptr
void emit_pixel(const crd::u8* rec, crd::u8 bpp, crd::u8 image_kind, const crd::u8* pal, crd::u32 pal_len,
                crd::u8* rgba) noexcept
{
    if (image_kind == 1) // palette index
    {
        const crd::u32 i = rec[0] < pal_len ? rec[0] : 0U;
        rgba[0]          = pal[i * 4U + 0U];
        rgba[1]          = pal[i * 4U + 1U];
        rgba[2]          = pal[i * 4U + 2U];
        rgba[3]          = pal[i * 4U + 3U];
        return;
    }
    if (image_kind == 3) // gray
    {
        rgba[0] = rec[0];
        rgba[1] = rec[0];
        rgba[2] = rec[0];
        rgba[3] = 255;
        return;
    }
    switch (bpp) // truecolor, little-endian BGR(A) / A1R5G5B5
    {
    case 16: {
        const crd::u16 v = le16(rec);
        const crd::u32 r = (v >> 10U) & 31U;
        const crd::u32 g = (v >> 5U) & 31U;
        const crd::u32 b = v & 31U;
        rgba[0]          = static_cast<crd::u8>((r * 255U + 15U) / 31U);
        rgba[1]          = static_cast<crd::u8>((g * 255U + 15U) / 31U);
        rgba[2]          = static_cast<crd::u8>((b * 255U + 15U) / 31U);
        rgba[3]          = 255; // the A1 attribute bit is unreliable in the wild (often 0 on opaque art) — decode opaque
        break;
    }
    case 24:
        rgba[0] = rec[2];
        rgba[1] = rec[1];
        rgba[2] = rec[0];
        rgba[3] = 255;
        break;
    default: // 32: BGRA
        rgba[0] = rec[2];
        rgba[1] = rec[1];
        rgba[2] = rec[0];
        rgba[3] = rec[3];
        break;
    }
}

} // namespace

bool tga_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    TgaHeader h{};
    return read_header(bytes, h) && header_consistent(h);
}

LdrError tga_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a)
{
    out.width  = 0;
    out.height = 0;
    out.pixels.clear();
    TgaHeader h{};
    if (!read_header(bytes, h)) { return LdrError::Truncated; }
    if (!header_consistent(h)) { return LdrError::BadHeader; }
    if (h.w > kMaxDim || h.h > kMaxDim) { return LdrError::TooLarge; }

    const bool    rle        = h.image_type >= 9U;
    const crd::u8 image_kind = static_cast<crd::u8>(rle ? h.image_type - 8U : h.image_type); // 1/2/3
    const crd::u32 rec_bytes = h.bpp / 8U;

    crd::usize off = 18U + h.id_len; // skip the image-ID field

    // palette → expanded RGBA
    crd::containers::Array<crd::u8> pal(a);
    if (h.cmap_type == 1U)
    {
        const crd::u32   entry_bytes = h.cmap_bits / 8U;
        const crd::usize pal_bytes   = static_cast<crd::usize>(h.cmap_len) * entry_bytes;
        if (off + pal_bytes > bytes.size()) { return LdrError::Truncated; }
        pal.resize(static_cast<crd::usize>(h.cmap_len) * 4U, 255);
        for (crd::u32 i = 0; i < h.cmap_len; ++i)
        {
            const crd::u8* e = bytes.data() + off + static_cast<crd::usize>(i) * entry_bytes;
            crd::u8        rgba[4];
            emit_pixel(e, h.cmap_bits, 2, nullptr, 0, rgba); // palette entries are little truecolor records
            pal[i * 4U + 0U] = rgba[0];
            pal[i * 4U + 1U] = rgba[1];
            pal[i * 4U + 2U] = rgba[2];
            pal[i * 4U + 3U] = rgba[3];
        }
        off += pal_bytes;
    }

    out.width  = h.w;
    out.height = h.h;
    out.pixels.resize(static_cast<crd::usize>(h.w) * h.h * 4U, 0);
    out.source_bit_depth = h.bpp == 16U ? crd::u8{5} : crd::u8{8};
    if (image_kind == 3U) { out.source_channels = 1; }
    else if (h.bpp == 32U) { out.source_channels = 4; }
    else { out.source_channels = 3; }

    const bool     top_origin = (h.desc & 0x20U) != 0U;
    const bool     right_left = (h.desc & 0x10U) != 0U;
    const crd::u64 n_px       = static_cast<crd::u64>(h.w) * h.h;

    const auto place = [&](crd::u64 i, const crd::u8* rgba) noexcept {
        const crd::u32 sx = static_cast<crd::u32>(i % h.w);
        const crd::u32 sy = static_cast<crd::u32>(i / h.w);
        const crd::u32 x  = right_left ? (h.w - 1U - sx) : sx;
        const crd::u32 y  = top_origin ? sy : (h.h - 1U - sy);
        crd::u8*       d  = out.pixels.data() + (static_cast<crd::usize>(y) * h.w + x) * 4U;
        d[0]              = rgba[0];
        d[1]              = rgba[1];
        d[2]              = rgba[2];
        d[3]              = rgba[3];
    };

    const crd::u8* p   = bytes.data() + off;
    const crd::u8* end = bytes.data() + bytes.size();
    if (!rle)
    {
        if (static_cast<crd::u64>(end - p) < n_px * rec_bytes) { return LdrError::Truncated; }
        for (crd::u64 i = 0; i < n_px; ++i)
        {
            crd::u8 rgba[4];
            emit_pixel(p + i * rec_bytes, h.bpp, image_kind, pal.data(), static_cast<crd::u32>(pal.size() / 4U), rgba);
            place(i, rgba);
        }
        return LdrError::Ok;
    }

    // RLE: packets of (1 + count) — high bit set = a RUN of one record, clear = `count` literal records
    crd::u64 i = 0;
    while (i < n_px)
    {
        if (p >= end) { return LdrError::Truncated; }
        const crd::u8  packet = *p++;
        const crd::u64 count  = static_cast<crd::u64>(packet & 0x7FU) + 1U;
        if (i + count > n_px) { return LdrError::BadData; } // a run past the image is corruption
        if ((packet & 0x80U) != 0U)
        {
            if (static_cast<crd::u64>(end - p) < rec_bytes) { return LdrError::Truncated; }
            crd::u8 rgba[4];
            emit_pixel(p, h.bpp, image_kind, pal.data(), static_cast<crd::u32>(pal.size() / 4U), rgba);
            p += rec_bytes;
            for (crd::u64 k = 0; k < count; ++k) { place(i + k, rgba); }
        }
        else
        {
            if (static_cast<crd::u64>(end - p) < count * rec_bytes) { return LdrError::Truncated; }
            for (crd::u64 k = 0; k < count; ++k)
            {
                crd::u8 rgba[4];
                emit_pixel(p + k * rec_bytes, h.bpp, image_kind, pal.data(), static_cast<crd::u32>(pal.size() / 4U), rgba);
                place(i + k, rgba);
            }
            p += count * rec_bytes;
        }
        i += count;
    }
    return LdrError::Ok;
}

} // namespace crd::resources
