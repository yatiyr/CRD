// png_image.cpp — the owned PNG decoder. See png_image.hpp for the coverage contract.

#include <crd/resources/png_image.hpp>

#include <crd/resources/deflate.hpp>

#include <cstring>

namespace crd::resources
{
namespace
{

constexpr crd::u32 kMaxDim = 16384; // sane cap: nothing real exceeds it; a crafted 4-GB bomb does

// ── CRC-32 (ISO 3309, reflected, poly 0xEDB88320) ──────────────────────────────────────────────────────────────────────

struct CrcTable
{
    crd::u32 t[256];

    CrcTable() noexcept
    {
        for (crd::u32 n = 0; n < 256U; ++n)
        {
            crd::u32 c = n;
            for (int k = 0; k < 8; ++k) { c = ((c & 1U) != 0U) ? 0xEDB88320U ^ (c >> 1U) : c >> 1U; }
            t[n] = c;
        }
    }
};

// ── big-endian reads ────────────────────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] crd::u32 be32(const crd::u8* p) noexcept
{
    return (static_cast<crd::u32>(p[0]) << 24U) | (static_cast<crd::u32>(p[1]) << 16U)
         | (static_cast<crd::u32>(p[2]) << 8U) | p[3];
}

// ── the IHDR legality matrix ────────────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] crd::u32 channels_of(crd::u8 color_type) noexcept
{
    switch (color_type)
    {
    case 0: return 1; // gray
    case 2: return 3; // RGB
    case 3: return 1; // palette (indices)
    case 4: return 2; // gray + alpha
    case 6: return 4; // RGBA
    default: return 0;
    }
}

[[nodiscard]] bool depth_legal(crd::u8 color_type, crd::u8 depth) noexcept
{
    switch (color_type)
    {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}

// ── defilter (per scanline; `bpp` = the filter unit in BYTES, ≥1 even for sub-byte pixels) ─────────────────────────────

[[nodiscard]] crd::u8 paeth(crd::u8 a, crd::u8 b, crd::u8 c) noexcept
{
    const int p  = static_cast<int>(a) + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) { return a; }
    if (pb <= pc) { return b; }
    return c;
}

// Reconstruct `h` filtered scanlines of `row_bytes` each from `src` (h·(1+row_bytes) bytes) into `dst` (h·row_bytes).
[[nodiscard]] bool defilter(const crd::u8* src, crd::usize src_len, crd::u8* dst, crd::u32 h, crd::usize row_bytes,
                            crd::u32 bpp) noexcept
{
    if (src_len < static_cast<crd::usize>(h) * (row_bytes + 1U)) { return false; }
    for (crd::u32 y = 0; y < h; ++y)
    {
        const crd::u8  filter = src[y * (row_bytes + 1U)];
        const crd::u8* in     = src + y * (row_bytes + 1U) + 1U;
        crd::u8*       out    = dst + static_cast<crd::usize>(y) * row_bytes;
        const crd::u8* prior  = y > 0U ? out - row_bytes : nullptr;
        switch (filter)
        {
        case 0: std::memcpy(out, in, row_bytes); break;
        case 1: // Sub
            for (crd::usize x = 0; x < row_bytes; ++x)
            {
                out[x] = static_cast<crd::u8>(in[x] + (x >= bpp ? out[x - bpp] : crd::u8{0}));
            }
            break;
        case 2: // Up
            for (crd::usize x = 0; x < row_bytes; ++x)
            {
                out[x] = static_cast<crd::u8>(in[x] + (prior != nullptr ? prior[x] : crd::u8{0}));
            }
            break;
        case 3: // Average
            for (crd::usize x = 0; x < row_bytes; ++x)
            {
                const crd::u32 left = x >= bpp ? out[x - bpp] : 0U;
                const crd::u32 up   = prior != nullptr ? prior[x] : 0U;
                out[x]              = static_cast<crd::u8>(in[x] + ((left + up) >> 1U));
            }
            break;
        case 4: // Paeth
            for (crd::usize x = 0; x < row_bytes; ++x)
            {
                const crd::u8 left = x >= bpp ? out[x - bpp] : crd::u8{0};
                const crd::u8 up   = prior != nullptr ? prior[x] : crd::u8{0};
                const crd::u8 ul   = (prior != nullptr && x >= bpp) ? prior[x - bpp] : crd::u8{0};
                out[x]             = static_cast<crd::u8>(in[x] + paeth(left, up, ul));
            }
            break;
        default: return false; // an illegal filter id is corruption
        }
    }
    return true;
}

// ── sample access on a defiltered scanline (handles 1/2/4/8/16-bit packing) ────────────────────────────────────────────

[[nodiscard]] crd::u32 sample_at(const crd::u8* row, crd::u32 pixel, crd::u32 channel, crd::u32 channels,
                                 crd::u8 depth) noexcept
{
    const crd::u32 index = pixel * channels + channel; // sample index within the row
    switch (depth)
    {
    case 1: return (row[index >> 3U] >> (7U - (index & 7U))) & 1U;
    case 2: return (row[index >> 2U] >> (2U * (3U - (index & 3U)))) & 3U;
    case 4: return (row[index >> 1U] >> (4U * (1U - (index & 1U)))) & 15U;
    case 8: return row[index];
    default: return row[index * 2U]; // 16-bit: the HIGH byte (the standard 16→8 downconversion)
    }
}

// scale a sub-byte sample to 8 bits (the spec's max-value replication)
[[nodiscard]] crd::u8 scale_to_8(crd::u32 v, crd::u8 depth) noexcept
{
    switch (depth)
    {
    case 1: return v != 0U ? crd::u8{255} : crd::u8{0};
    case 2: return static_cast<crd::u8>(v * 85U);  // 0,85,170,255
    case 4: return static_cast<crd::u8>(v * 17U);  // 0,17,…,255
    default: return static_cast<crd::u8>(v);       // 8/16 (16 already reduced to its high byte)
    }
}

// ── the decode state shared by the sequential + Adam7 paths ─────────────────────────────────────────────────────────────

struct PngState
{
    crd::u8  color_type = 0;
    crd::u8  depth      = 0;
    crd::u32 channels   = 0;
    // palette + transparency
    crd::u8  palette[256][3] = {};
    crd::u8  pal_alpha[256]  = {};
    crd::u32 pal_count       = 0;
    bool     has_trns        = false;
    crd::u32 key[3]          = {}; // gray/RGB color key (at source depth)

    // convert ONE pixel from a defiltered row into RGBA8
    void emit(const crd::u8* row, crd::u32 px, crd::u8* rgba) const noexcept
    {
        switch (color_type)
        {
        case 0: { // gray (+ optional color key)
            const crd::u32 g = sample_at(row, px, 0, 1, depth);
            const crd::u8  v = scale_to_8(g, depth);
            rgba[0]          = v;
            rgba[1]          = v;
            rgba[2]          = v;
            rgba[3]          = (has_trns && g == key[0]) ? crd::u8{0} : crd::u8{255};
            break;
        }
        case 2: { // RGB (+ optional color key)
            const crd::u32 r = sample_at(row, px, 0, 3, depth);
            const crd::u32 g = sample_at(row, px, 1, 3, depth);
            const crd::u32 b = sample_at(row, px, 2, 3, depth);
            rgba[0]          = scale_to_8(r, depth);
            rgba[1]          = scale_to_8(g, depth);
            rgba[2]          = scale_to_8(b, depth);
            rgba[3] = (has_trns && r == key[0] && g == key[1] && b == key[2]) ? crd::u8{0} : crd::u8{255};
            break;
        }
        case 3: { // palette
            const crd::u32 i = sample_at(row, px, 0, 1, depth);
            const crd::u32 c = i < pal_count ? i : 0U; // OOB palette index → caller already rejected; belt+braces
            rgba[0]          = palette[c][0];
            rgba[1]          = palette[c][1];
            rgba[2]          = palette[c][2];
            rgba[3]          = pal_alpha[c];
            break;
        }
        case 4: { // gray + alpha
            const crd::u8 v = scale_to_8(sample_at(row, px, 0, 2, depth), depth);
            rgba[0]         = v;
            rgba[1]         = v;
            rgba[2]         = v;
            rgba[3]         = scale_to_8(sample_at(row, px, 1, 2, depth), depth);
            break;
        }
        default: { // 6: RGBA
            rgba[0] = scale_to_8(sample_at(row, px, 0, 4, depth), depth);
            rgba[1] = scale_to_8(sample_at(row, px, 1, 4, depth), depth);
            rgba[2] = scale_to_8(sample_at(row, px, 2, 4, depth), depth);
            rgba[3] = scale_to_8(sample_at(row, px, 3, 4, depth), depth);
            break;
        }
        }
    }

    // palette-index bounds check over a defiltered row (color type 3 only)
    [[nodiscard]] bool palette_ok(const crd::u8* row, crd::u32 w) const noexcept
    {
        if (color_type != 3) { return true; }
        for (crd::u32 x = 0; x < w; ++x)
        {
            if (sample_at(row, x, 0, 1, depth) >= pal_count) { return false; }
        }
        return true;
    }

    [[nodiscard]] crd::usize row_bytes(crd::u32 w) const noexcept
    {
        return (static_cast<crd::usize>(w) * channels * depth + 7U) / 8U;
    }
    [[nodiscard]] crd::u32 filter_bpp() const noexcept
    {
        const crd::u32 bits = channels * depth;
        return bits < 8U ? 1U : bits / 8U;
    }
};

// Adam7: per-pass x/y origins + strides (the spec's pixel dispersal grid)
constexpr crd::u32 kA7x0[7] = {0, 4, 0, 2, 0, 1, 0};
constexpr crd::u32 kA7y0[7] = {0, 0, 4, 0, 2, 0, 1};
constexpr crd::u32 kA7dx[7] = {8, 8, 4, 4, 2, 2, 1};
constexpr crd::u32 kA7dy[7] = {8, 8, 8, 4, 4, 2, 2};

} // namespace

crd::u32 png_crc32(crd::containers::ConstSpan<crd::u8> data) noexcept
{
    static const CrcTable kTable; // computed once
    crd::u32              c = 0xFFFFFFFFU;
    for (crd::usize i = 0; i < data.size(); ++i) { c = kTable.t[(c ^ data[i]) & 0xFFU] ^ (c >> 8U); }
    return c ^ 0xFFFFFFFFU;
}

bool png_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    static constexpr crd::u8 kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= 8U && std::memcmp(bytes.data(), kSig, 8) == 0;
}

PngError png_decode(crd::containers::ConstSpan<crd::u8> bytes, PngImage& out, crd::memory::IAllocator* a)
{
    out.width  = 0;
    out.height = 0;
    out.pixels.clear();
    if (!png_sniff(bytes)) { return PngError::BadMagic; }

    // ── chunk walk: IHDR must lead; IDAT concatenates; every chunk CRC-verified ────────────────────────────────────────
    PngState                        st;
    crd::u32                        w         = 0;
    crd::u32                        h         = 0;
    crd::u8                         interlace = 0;
    bool                            have_ihdr = false;
    crd::containers::Array<crd::u8> idat(a);
    const crd::u8*                  p         = bytes.data() + 8;
    const crd::u8*                  end       = bytes.data() + bytes.size();
    bool                            have_iend = false;

    while (p + 8 <= end)
    {
        const crd::u32 len = be32(p);
        if (static_cast<crd::usize>(end - p) < 12U + static_cast<crd::usize>(len)) { return PngError::Truncated; }
        const crd::u8* type = p + 4;
        const crd::u8* data = p + 8;
        const crd::u32 crc  = be32(data + len);
        if (png_crc32(crd::containers::ConstSpan<crd::u8>(type, 4U + len)) != crc) { return PngError::BadChunkCrc; }

        if (std::memcmp(type, "IHDR", 4) == 0)
        {
            if (len != 13U || have_ihdr) { return PngError::BadHeader; }
            w             = be32(data);
            h             = be32(data + 4);
            st.depth      = data[8];
            st.color_type = data[9];
            if (w == 0U || h == 0U) { return PngError::BadHeader; }
            if (w > kMaxDim || h > kMaxDim) { return PngError::TooLarge; }
            st.channels = channels_of(st.color_type);
            if (st.channels == 0U || !depth_legal(st.color_type, st.depth)) { return PngError::BadHeader; }
            if (data[10] != 0U || data[11] != 0U) { return PngError::BadHeader; } // compression/filter methods
            interlace = data[12];
            if (interlace > 1U) { return PngError::BadHeader; }
            have_ihdr = true;
        }
        else if (std::memcmp(type, "PLTE", 4) == 0)
        {
            if (!have_ihdr || len == 0U || (len % 3U) != 0U || len > 256U * 3U) { return PngError::BadHeader; }
            st.pal_count = len / 3U;
            for (crd::u32 i = 0; i < st.pal_count; ++i)
            {
                st.palette[i][0]  = data[i * 3U + 0U];
                st.palette[i][1]  = data[i * 3U + 1U];
                st.palette[i][2]  = data[i * 3U + 2U];
                st.pal_alpha[i] = 255;
            }
        }
        else if (std::memcmp(type, "tRNS", 4) == 0)
        {
            if (!have_ihdr) { return PngError::BadHeader; }
            st.has_trns = true;
            if (st.color_type == 3)
            {
                if (len > st.pal_count) { return PngError::BadHeader; }
                for (crd::u32 i = 0; i < len; ++i) { st.pal_alpha[i] = data[i]; }
            }
            else if (st.color_type == 0)
            {
                if (len != 2U) { return PngError::BadHeader; }
                st.key[0] = (static_cast<crd::u32>(data[0]) << 8U) | data[1];
                if (st.depth < 16U) { st.key[0] &= (1U << st.depth) - 1U; }
                else { st.key[0] >>= 8U; } // compare at the reduced depth
            }
            else if (st.color_type == 2)
            {
                if (len != 6U) { return PngError::BadHeader; }
                for (int k = 0; k < 3; ++k)
                {
                    st.key[k] = (static_cast<crd::u32>(data[k * 2]) << 8U) | data[k * 2 + 1];
                    if (st.depth == 16U) { st.key[k] >>= 8U; }
                }
            }
            else { return PngError::BadHeader; } // tRNS is illegal with explicit-alpha types
        }
        else if (std::memcmp(type, "IDAT", 4) == 0)
        {
            if (!have_ihdr) { return PngError::BadHeader; }
            for (crd::u32 i = 0; i < len; ++i) { idat.push_back(data[i]); }
        }
        else if (std::memcmp(type, "IEND", 4) == 0)
        {
            have_iend = true;
            break;
        }
        // ancillary chunks (gAMA, sRGB, tEXt, pHYs, …) skip — color management is the pipeline's concern, not the codec's
        p = data + len + 4;
    }
    if (!have_iend || !have_ihdr || idat.size() == 0U) { return PngError::Truncated; } // a stream without IEND ended early
    if (st.color_type == 3 && st.pal_count == 0U) { return PngError::BadHeader; }

    // ── inflate the pixel stream (OUR zlib) ────────────────────────────────────────────────────────────────────────────
    crd::containers::Array<crd::u8> raw(a);
    if (!zlib_inflate(crd::containers::as_const_span(idat), raw)) { return PngError::BadData; }

    out.width            = w;
    out.height           = h;
    out.source_channels  = static_cast<crd::u8>(st.channels);
    out.source_bit_depth = st.depth;
    out.pixels.resize(static_cast<crd::usize>(w) * h * 4U, 0);

    crd::containers::Array<crd::u8> rows(a); // defiltered scanlines of the current (sub)image

    if (interlace == 0U)
    {
        const crd::usize rb = st.row_bytes(w);
        rows.resize(static_cast<crd::usize>(h) * rb, 0);
        if (!defilter(raw.data(), raw.size(), rows.data(), h, rb, st.filter_bpp())) { return PngError::BadData; }
        if (raw.size() != static_cast<crd::usize>(h) * (rb + 1U)) { return PngError::BadData; } // exact-size contract
        for (crd::u32 y = 0; y < h; ++y)
        {
            const crd::u8* row = rows.data() + static_cast<crd::usize>(y) * rb;
            if (!st.palette_ok(row, w)) { return PngError::BadData; }
            for (crd::u32 x = 0; x < w; ++x)
            {
                st.emit(row, x, out.pixels.data() + (static_cast<crd::usize>(y) * w + x) * 4U);
            }
        }
        return PngError::Ok;
    }

    // ── Adam7: 7 independently-filtered sub-images, scattered on the dispersal grid ────────────────────────────────────
    crd::usize off = 0;
    for (int pass = 0; pass < 7; ++pass)
    {
        const crd::u32 pw = w > kA7x0[pass] ? (w - kA7x0[pass] + kA7dx[pass] - 1U) / kA7dx[pass] : 0U;
        const crd::u32 ph = h > kA7y0[pass] ? (h - kA7y0[pass] + kA7dy[pass] - 1U) / kA7dy[pass] : 0U;
        if (pw == 0U || ph == 0U) { continue; }
        const crd::usize rb = st.row_bytes(pw);
        rows.clear();
        rows.resize(static_cast<crd::usize>(ph) * rb, 0);
        if (off + static_cast<crd::usize>(ph) * (rb + 1U) > raw.size()) { return PngError::BadData; }
        if (!defilter(raw.data() + off, static_cast<crd::usize>(ph) * (rb + 1U), rows.data(), ph, rb, st.filter_bpp()))
        {
            return PngError::BadData;
        }
        off += static_cast<crd::usize>(ph) * (rb + 1U);
        for (crd::u32 sy = 0; sy < ph; ++sy)
        {
            const crd::u8* row = rows.data() + static_cast<crd::usize>(sy) * rb;
            if (!st.palette_ok(row, pw)) { return PngError::BadData; }
            const crd::u32 y = kA7y0[pass] + sy * kA7dy[pass];
            for (crd::u32 sx = 0; sx < pw; ++sx)
            {
                const crd::u32 x = kA7x0[pass] + sx * kA7dx[pass];
                st.emit(row, sx, out.pixels.data() + (static_cast<crd::usize>(y) * w + x) * 4U);
            }
        }
    }
    if (off != raw.size()) { return PngError::BadData; } // exact-size contract across all passes
    return PngError::Ok;
}

} // namespace crd::resources
