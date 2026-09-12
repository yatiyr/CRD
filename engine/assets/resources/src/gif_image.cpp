// gif_image.cpp — OUR OWN GIF decoder + the engine's first LZW decompressor (variable-width, GIF flavour). Decodes the
// FIRST frame of a GIF87a/GIF89a stream to RGBA8 (ldr_image.hpp). Global/local color tables, GIF89a transparency, and
// 4-pass interlace are handled; multi-frame animation is a named MED follow-up. Malformed input is refused BY NAME.

#include <crd/resources/gif_image.hpp>

#include <crd/containers/array.hpp>

namespace crd::resources
{
namespace
{

constexpr crd::u32 kMaxDim = 16384U; // family-wide sane cap (matches the other codecs)

[[nodiscard]] inline crd::u16 rd16le(const crd::u8* p) noexcept
{
    return static_cast<crd::u16>(static_cast<crd::u16>(p[0]) | (static_cast<crd::u16>(p[1]) << 8U));
}

// GIF variable-width LZW decode → palette indices. `expected` bounds the output (a corrupt stream cannot run away).
[[nodiscard]] GifError lzw_decode(const crd::containers::Array<crd::u8>& data, crd::u8 min_code_size, crd::usize expected,
                                  crd::containers::Array<crd::u8>& out)
{
    const crd::u32 clear_code = 1U << min_code_size;
    const crd::u32 end_code   = clear_code + 1U;

    crd::u16 prefix[4096];
    crd::u8  suffix[4096];
    crd::u8  stack[4096];
    for (crd::u32 i = 0; i < clear_code; ++i)
    {
        prefix[i] = 0U;
        suffix[i] = static_cast<crd::u8>(i);
    }

    crd::u32   code_size = min_code_size + 1U;
    crd::u32   next_code = clear_code + 2U;
    crd::i64   prev      = -1;
    crd::u8    first      = 0;

    // LSB-first bit reader over the concatenated data sub-blocks.
    crd::usize byte_pos = 0;
    crd::u32   bitbuf   = 0;
    crd::u32   bitcnt   = 0;
    const auto read_code = [&](crd::u32 width) -> crd::i64 {
        while (bitcnt < width)
        {
            if (byte_pos >= data.size()) { return -1; } // EOF — no more code bits
            bitbuf |= static_cast<crd::u32>(data[byte_pos++]) << bitcnt;
            bitcnt += 8U;
        }
        const crd::u32 code = bitbuf & ((1U << width) - 1U);
        bitbuf >>= width;
        bitcnt -= width;
        return static_cast<crd::i64>(code);
    };

    for (;;)
    {
        const crd::i64 c = read_code(code_size);
        if (c < 0) { break; } // ran out of bits — the size check below catches any shortfall
        const crd::u32 code = static_cast<crd::u32>(c);

        if (code == clear_code)
        {
            code_size = min_code_size + 1U;
            next_code = clear_code + 2U;
            prev      = -1;
            continue;
        }
        if (code == end_code) { break; }

        if (prev < 0)
        {
            if (code >= clear_code) { return LdrError::BadData; } // first symbol must be a literal root
            first = static_cast<crd::u8>(code);
            out.push_back(first);
            prev = static_cast<crd::i64>(code);
            continue;
        }

        crd::u32 walk = code;
        crd::u32 sp   = 0;
        if (code >= next_code)
        {
            if (code > next_code) { return LdrError::BadData; } // code beyond the next assignable entry
            stack[sp++] = first;                               // KwKwK: string(prev) + first-char(prev)
            walk        = static_cast<crd::u32>(prev);
        }
        while (walk >= clear_code)
        {
            if (sp >= 4096U) { return LdrError::BadData; } // corrupt chain
            stack[sp++] = suffix[walk];
            walk        = prefix[walk];
        }
        first = static_cast<crd::u8>(walk);
        if (sp >= 4096U) { return LdrError::BadData; }
        stack[sp++] = first;

        while (sp > 0U)
        {
            out.push_back(stack[--sp]);
            if (out.size() > expected) { return LdrError::BadData; } // more pixels than the frame holds
        }

        if (next_code < 4096U)
        {
            prefix[next_code] = static_cast<crd::u16>(prev);
            suffix[next_code] = first;
            ++next_code;
            if (next_code == (1U << code_size) && code_size < 12U) { ++code_size; }
        }
        prev = static_cast<crd::i64>(code);
    }
    return LdrError::Ok;
}

} // namespace

bool gif_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    if (bytes.size() < 6U) { return false; }
    const crd::u8* p = bytes.data();
    return p[0] == 'G' && p[1] == 'I' && p[2] == 'F' && p[3] == '8' && (p[4] == '7' || p[4] == '9') && p[5] == 'a';
}

GifError gif_decode(crd::containers::ConstSpan<crd::u8> bytes, GifImage& out, crd::memory::IAllocator* a)
{
    out.width  = 0;
    out.height = 0;
    out.pixels.clear();

    if (!gif_sniff(bytes)) { return LdrError::BadMagic; }
    const crd::u8* p   = bytes.data();
    const crd::u8* end = p + bytes.size();
    if (bytes.size() < 13U) { return LdrError::Truncated; } // signature(6) + logical screen descriptor(7)
    p += 6U;

    // Logical Screen Descriptor.
    const crd::u8 lsd_packed = p[4];
    p += 7U;
    const bool     gct_flag = (lsd_packed & 0x80U) != 0U;
    const crd::u32 gct_size = gct_flag ? (2U << (lsd_packed & 0x07U)) : 0U; // 2^(N+1) entries
    const crd::u8* gct      = nullptr;
    if (gct_flag)
    {
        if (static_cast<crd::usize>(end - p) < static_cast<crd::usize>(gct_size) * 3U) { return LdrError::Truncated; }
        gct = p;
        p += static_cast<crd::usize>(gct_size) * 3U;
    }

    crd::i32 transparent_index = -1; // set by a preceding GIF89a Graphic Control Extension

    for (;;)
    {
        if (p >= end) { return LdrError::Truncated; }
        const crd::u8 block = *p++;

        if (block == 0x3BU) { return LdrError::BadData; } // trailer before any image frame

        if (block == 0x21U) // extension
        {
            if (p >= end) { return LdrError::Truncated; }
            const crd::u8 label = *p++;
            if (label == 0xF9U) // Graphic Control Extension
            {
                if (p >= end) { return LdrError::Truncated; }
                const crd::u8 sz = *p++;
                if (sz != 4U || static_cast<crd::usize>(end - p) < 5U) { return LdrError::BadData; }
                const crd::u8 gce_packed = p[0];
                if ((gce_packed & 0x01U) != 0U) { transparent_index = p[3]; }
                p += 4U;
                if (*p++ != 0x00U) { return LdrError::BadData; } // block terminator
            }
            else // comment / application / plain-text — skip the sub-block chain
            {
                for (;;)
                {
                    if (p >= end) { return LdrError::Truncated; }
                    const crd::u8 sz = *p++;
                    if (sz == 0U) { break; }
                    if (static_cast<crd::usize>(end - p) < sz) { return LdrError::Truncated; }
                    p += sz;
                }
            }
            continue;
        }

        if (block == 0x2CU) // image descriptor — the first frame
        {
            if (static_cast<crd::usize>(end - p) < 9U) { return LdrError::Truncated; }
            const crd::u32 img_w      = rd16le(p + 4);
            const crd::u32 img_h      = rd16le(p + 6);
            const crd::u8  img_packed = p[8];
            p += 9U;
            if (img_w == 0U || img_h == 0U) { return LdrError::BadHeader; }
            if (img_w > kMaxDim || img_h > kMaxDim) { return LdrError::TooLarge; }

            const bool     lct_flag  = (img_packed & 0x80U) != 0U;
            const bool     interlace = (img_packed & 0x40U) != 0U;
            const crd::u32 lct_size  = lct_flag ? (2U << (img_packed & 0x07U)) : 0U;
            const crd::u8* lct       = nullptr;
            if (lct_flag)
            {
                if (static_cast<crd::usize>(end - p) < static_cast<crd::usize>(lct_size) * 3U)
                {
                    return LdrError::Truncated;
                }
                lct = p;
                p += static_cast<crd::usize>(lct_size) * 3U;
            }
            const crd::u8* ct       = (lct != nullptr) ? lct : gct;
            const crd::u32 ct_count = (lct != nullptr) ? lct_size : gct_size;
            if (ct == nullptr || ct_count == 0U) { return LdrError::BadHeader; } // no palette to map through

            if (p >= end) { return LdrError::Truncated; }
            const crd::u8 min_code_size = *p++;
            if (min_code_size < 2U || min_code_size > 8U) { return LdrError::BadData; }

            // Gather the LZW data sub-blocks into one contiguous buffer.
            crd::containers::Array<crd::u8> lzw(a);
            for (;;)
            {
                if (p >= end) { return LdrError::Truncated; }
                const crd::u8 sz = *p++;
                if (sz == 0U) { break; }
                if (static_cast<crd::usize>(end - p) < sz) { return LdrError::Truncated; }
                for (crd::u8 i = 0; i < sz; ++i) { lzw.push_back(p[i]); }
                p += sz;
            }

            const crd::usize                pixel_count = static_cast<crd::usize>(img_w) * img_h;
            crd::containers::Array<crd::u8> indices(a);
            indices.reserve(pixel_count);
            const GifError e = lzw_decode(lzw, min_code_size, pixel_count, indices);
            if (e != LdrError::Ok) { return e; }
            if (indices.size() != pixel_count) { return LdrError::BadData; } // wrong pixel count

            // Validate every index against the active table BEFORE writing (no partial output on failure).
            for (crd::usize i = 0; i < pixel_count; ++i)
            {
                if (indices[i] >= ct_count) { return LdrError::BadData; }
            }

            out.width            = img_w;
            out.height           = img_h;
            out.source_channels  = 4;
            out.source_bit_depth = 8;
            out.pixels.resize(pixel_count * 4U, 0);

            crd::usize src       = 0;
            const auto emit_row  = [&](crd::u32 y) {
                for (crd::u32 x = 0; x < img_w; ++x)
                {
                    const crd::u8  idx = indices[src++];
                    const crd::u8* c   = ct + static_cast<crd::usize>(idx) * 3U;
                    crd::u8*       d   = out.pixels.data() + (static_cast<crd::usize>(y) * img_w + x) * 4U;
                    d[0]               = c[0];
                    d[1]               = c[1];
                    d[2]               = c[2];
                    d[3]               = (static_cast<crd::i32>(idx) == transparent_index) ? 0U : 255U;
                }
            };

            if (interlace)
            {
                constexpr crd::u32 starts[4] = {0U, 4U, 2U, 1U};
                constexpr crd::u32 steps[4]  = {8U, 8U, 4U, 2U};
                for (int pass = 0; pass < 4; ++pass)
                {
                    for (crd::u32 y = starts[pass]; y < img_h; y += steps[pass]) { emit_row(y); }
                }
            }
            else
            {
                for (crd::u32 y = 0; y < img_h; ++y) { emit_row(y); }
            }
            return LdrError::Ok; // MED-1 first increment: the first frame only
        }

        return LdrError::BadData; // unknown block sentinel
    }
}

} // namespace crd::resources
