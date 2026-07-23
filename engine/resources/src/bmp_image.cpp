// bmp_image.cpp — the owned BMP decoder. See bmp_image.hpp for the coverage contract.

#include <crd/resources/bmp_image.hpp>

namespace crd::resources
{
namespace
{

constexpr crd::u32 kMaxDim = 16384;

[[nodiscard]] crd::u16 le16(const crd::u8* p) noexcept { return static_cast<crd::u16>(p[0] | (p[1] << 8U)); }
[[nodiscard]] crd::u32 le32(const crd::u8* p) noexcept
{
    return static_cast<crd::u32>(p[0]) | (static_cast<crd::u32>(p[1]) << 8U) | (static_cast<crd::u32>(p[2]) << 16U)
         | (static_cast<crd::u32>(p[3]) << 24U);
}

// shift+scale a masked channel to 8 bits
struct Mask
{
    crd::u32 mask  = 0;
    crd::u32 shift = 0;
    crd::u32 max   = 0;

    void init(crd::u32 m) noexcept
    {
        mask  = m;
        shift = 0;
        if (m == 0U)
        {
            max = 0;
            return;
        }
        while (((m >> shift) & 1U) == 0U) { ++shift; }
        max = m >> shift;
    }
    [[nodiscard]] crd::u8 extract(crd::u32 v, crd::u8 def) const noexcept
    {
        if (max == 0U) { return def; }
        return static_cast<crd::u8>(((v & mask) >> shift) * 255U / max);
    }
};

} // namespace

bool bmp_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    return bytes.size() >= 2U && bytes[0] == 'B' && bytes[1] == 'M';
}

LdrError bmp_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* /*scratch*/)
{ // BMP needs no scratch: the palette is fixed-size and rows decode straight into `out.pixels`
    out.width  = 0;
    out.height = 0;
    out.pixels.clear();
    if (!bmp_sniff(bytes)) { return LdrError::BadMagic; }
    if (bytes.size() < 54U) { return LdrError::Truncated; }
    const crd::u8* p          = bytes.data();
    const crd::u32 pixel_off  = le32(p + 10);
    const crd::u32 hdr_size   = le32(p + 14);
    if (hdr_size == 12U) { return LdrError::Unsupported; } // OS/2 core header — named, never mis-decoded
    if (hdr_size != 40U && hdr_size != 108U && hdr_size != 124U) { return LdrError::BadHeader; }
    if (bytes.size() < 14U + hdr_size) { return LdrError::Truncated; }

    const crd::i32 w_raw   = static_cast<crd::i32>(le32(p + 18));
    const crd::i32 h_raw   = static_cast<crd::i32>(le32(p + 22));
    const crd::u16 planes  = le16(p + 26);
    const crd::u16 bpp     = le16(p + 28);
    const crd::u32 comp    = le32(p + 30);
    crd::u32       pal_n   = le32(p + 46);
    const bool     topdown = h_raw < 0;
    const crd::u32 w       = static_cast<crd::u32>(w_raw);
    const crd::u32 h       = static_cast<crd::u32>(topdown ? -h_raw : h_raw);
    if (planes != 1U || w_raw <= 0 || h_raw == 0) { return LdrError::BadHeader; }
    if (w > kMaxDim || h > kMaxDim) { return LdrError::TooLarge; }

    const bool is_pal = bpp == 1U || bpp == 4U || bpp == 8U;
    const bool is_rgb = bpp == 16U || bpp == 24U || bpp == 32U;
    if (!is_pal && !is_rgb) { return LdrError::BadHeader; }
    // compression: 0 = RGB, 1 = RLE8 (bpp 8), 2 = RLE4 (bpp 4), 3 = BITFIELDS (bpp 16/32)
    if (comp > 3U) { return LdrError::Unsupported; }
    if (comp == 1U && bpp != 8U) { return LdrError::BadHeader; }
    if (comp == 2U && bpp != 4U) { return LdrError::BadHeader; }
    if (comp == 3U && bpp != 16U && bpp != 32U) { return LdrError::BadHeader; }

    // channel masks: explicit for BITFIELDS (right after the 40-byte header, or in-header for V4/V5); defaults otherwise
    Mask mr;
    Mask mg;
    Mask mb;
    Mask ma;
    if (comp == 3U)
    {
        const crd::u8* mp = p + 54; // the RGB masks sit at offset 54 both ways: appended (40-byte header) or in-header (V4/V5)
        if (bytes.size() < 54U + 12U) { return LdrError::Truncated; }
        mr.init(le32(mp));
        mg.init(le32(mp + 4));
        mb.init(le32(mp + 8));
        // an ALPHA mask exists only in V4/V5 headers (the 40-byte BITFIELDS block is exactly 3 masks — reading a 4th
        // would consume pixel data); absent ⇒ opaque
        if (hdr_size >= 108U)
        {
            if (bytes.size() < 54U + 16U) { return LdrError::Truncated; }
            ma.init(le32(mp + 12));
        }
        else { ma.init(0U); }
        if (mr.max == 0U || mg.max == 0U || mb.max == 0U) { return LdrError::BadHeader; }
    }
    else if (bpp == 16U) // legacy default: X1R5G5B5
    {
        mr.init(0x7C00U);
        mg.init(0x03E0U);
        mb.init(0x001FU);
        ma.init(0U);
    }

    // palette (BGRX entries), following the header (+ the 3-mask block when comp==3 on a 40-byte header)
    crd::u8 palette[256][4] = {};
    if (is_pal)
    {
        if (pal_n == 0U) { pal_n = 1U << bpp; }
        if (pal_n > 256U) { return LdrError::BadHeader; }
        crd::usize pal_off = 14U + hdr_size;
        if (comp == 3U && hdr_size == 40U) { pal_off += 12U; }
        if (bytes.size() < pal_off + static_cast<crd::usize>(pal_n) * 4U) { return LdrError::Truncated; }
        for (crd::u32 i = 0; i < pal_n; ++i)
        {
            const crd::u8* e  = p + pal_off + static_cast<crd::usize>(i) * 4U;
            palette[i][0]     = e[2];
            palette[i][1]     = e[1];
            palette[i][2]     = e[0];
            palette[i][3]     = 255;
        }
    }

    if (pixel_off >= bytes.size()) { return LdrError::Truncated; }
    out.width            = w;
    out.height           = h;
    if (is_pal) { out.source_channels = 1; }
    else if (bpp == 32U) { out.source_channels = 4; }
    else { out.source_channels = 3; }
    if (bpp == 16U) { out.source_bit_depth = 5; }
    else if (is_pal) { out.source_bit_depth = static_cast<crd::u8>(bpp); }
    else { out.source_bit_depth = 8; }
    out.pixels.resize(static_cast<crd::usize>(w) * h * 4U, 0);

    const auto dst_row = [&](crd::u32 src_y) noexcept -> crd::u8* {
        const crd::u32 y = topdown ? src_y : (h - 1U - src_y);
        return out.pixels.data() + static_cast<crd::usize>(y) * w * 4U;
    };

    const crd::u8*   px       = p + pixel_off;
    const crd::u8*   end      = bytes.data() + bytes.size();

    if (comp == 1U || comp == 2U) // ── RLE8 / RLE4 ────────────────────────────────────────────────────────────────────
    {
        crd::u32 x = 0;
        crd::u32 y = 0;
        while (px + 2 <= end)
        {
            const crd::u8 n = px[0];
            const crd::u8 v = px[1];
            px += 2;
            if (n > 0U) // an encoded run of n indices
            {
                for (crd::u8 k = 0; k < n && x < w; ++k, ++x)
                {
                    crd::u32 idx = 0;
                    if (comp == 1U) { idx = v; }
                    else { idx = ((k & 1U) == 0U) ? (v >> 4U) : (v & 15U); }
                    if (idx >= pal_n) { return LdrError::BadData; }
                    if (y >= h) { return LdrError::BadData; }
                    crd::u8* d = dst_row(y) + static_cast<crd::usize>(x) * 4U;
                    d[0]       = palette[idx][0];
                    d[1]       = palette[idx][1];
                    d[2]       = palette[idx][2];
                    d[3]       = 255;
                }
                continue;
            }
            if (v == 0U) // end of line
            {
                x = 0;
                ++y;
                continue;
            }
            if (v == 1U) { return LdrError::Ok; } // end of bitmap
            if (v == 2U)                          // delta
            {
                if (px + 2 > end) { return LdrError::Truncated; }
                x += px[0];
                y += px[1];
                px += 2;
                continue;
            }
            // absolute mode: v literal indices, padded to a 16-bit boundary
            const crd::u32   lit       = v;
            const crd::usize lit_bytes = comp == 1U ? lit : (lit + 1U) / 2U;
            const crd::usize padded    = (lit_bytes + 1U) & ~static_cast<crd::usize>(1U);
            if (px + padded > end) { return LdrError::Truncated; }
            for (crd::u32 k = 0; k < lit && x < w; ++k, ++x)
            {
                crd::u32 idx = 0;
                if (comp == 1U) { idx = px[k]; }
                else { idx = ((k & 1U) == 0U) ? (px[k / 2U] >> 4U) : (px[k / 2U] & 15U); }
                if (idx >= pal_n) { return LdrError::BadData; }
                if (y >= h) { return LdrError::BadData; }
                crd::u8* d = dst_row(y) + static_cast<crd::usize>(x) * 4U;
                d[0]       = palette[idx][0];
                d[1]       = palette[idx][1];
                d[2]       = palette[idx][2];
                d[3]       = 255;
            }
            px += padded;
        }
        return LdrError::Truncated; // ran out of bytes before the EOB escape
    }

    // ── uncompressed / bitfields: 4-byte-padded rows ───────────────────────────────────────────────────────────────────
    const crd::usize row_bytes = ((static_cast<crd::usize>(w) * bpp + 31U) / 32U) * 4U;
    if (static_cast<crd::usize>(end - px) < row_bytes * h) { return LdrError::Truncated; }
    for (crd::u32 sy = 0; sy < h; ++sy)
    {
        const crd::u8* row = px + static_cast<crd::usize>(sy) * row_bytes;
        crd::u8*       dst = dst_row(sy);
        for (crd::u32 x = 0; x < w; ++x)
        {
            crd::u8* d = dst + static_cast<crd::usize>(x) * 4U;
            if (is_pal)
            {
                crd::u32 idx = 0;
                if (bpp == 8U) { idx = row[x]; }
                else if (bpp == 4U) { idx = ((x & 1U) == 0U) ? (row[x / 2U] >> 4U) : (row[x / 2U] & 15U); }
                else { idx = (row[x / 8U] >> (7U - (x & 7U))) & 1U; }
                if (idx >= pal_n) { return LdrError::BadData; }
                d[0] = palette[idx][0];
                d[1] = palette[idx][1];
                d[2] = palette[idx][2];
                d[3] = 255;
            }
            else if (bpp == 24U)
            {
                d[0] = row[x * 3U + 2U];
                d[1] = row[x * 3U + 1U];
                d[2] = row[x * 3U + 0U];
                d[3] = 255;
            }
            else if (bpp == 32U)
            {
                if (comp == 3U)
                {
                    const crd::u32 v = le32(row + static_cast<crd::usize>(x) * 4U);
                    d[0]             = mr.extract(v, 0);
                    d[1]             = mg.extract(v, 0);
                    d[2]             = mb.extract(v, 0);
                    d[3]             = ma.extract(v, 255);
                }
                else // BGRX (the X byte is not alpha in plain BI_RGB)
                {
                    d[0] = row[x * 4U + 2U];
                    d[1] = row[x * 4U + 1U];
                    d[2] = row[x * 4U + 0U];
                    d[3] = 255;
                }
            }
            else // 16-bit masked
            {
                const crd::u32 v = le16(row + static_cast<crd::usize>(x) * 2U);
                d[0]             = mr.extract(v, 0);
                d[1]             = mg.extract(v, 0);
                d[2]             = mb.extract(v, 0);
                d[3]             = ma.extract(v, 255);
            }
        }
    }
    return LdrError::Ok;
}

} // namespace crd::resources
