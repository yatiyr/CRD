#include <crd/resources/hdr_image.hpp>

#include <crd/core/types.hpp>
#include <crd/resources/crdr.hpp>

#include <cmath>
#include <cstring>

namespace crd::resources
{

using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{

constexpr crd::u32 kMaxDim = 65536U; // sane per-axis cap

// ── small byte-append helpers (Array<u8> as an output stream) ──────────────────────────────────────────────────────────────
void put_u8(Array<crd::u8>& out, crd::u8 v) { out.push_back(v); }
void put_str(Array<crd::u8>& out, const char* s) { for (const char* p = s; *p != '\0'; ++p) { out.push_back(static_cast<crd::u8>(*p)); } }
void put_u32_le(Array<crd::u8>& out, crd::u32 v) { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU)); } }
void put_f32_le(Array<crd::u8>& out, crd::f32 v)
{
    crd::u32 bits = 0; std::memcpy(&bits, &v, 4);
    put_u32_le(out, bits);
}
[[nodiscard]] crd::f32 read_f32(const crd::u8* p, bool little_endian) noexcept
{
    crd::u8 b[4] = {p[0], p[1], p[2], p[3]};
    if (!little_endian) { const crd::u8 t0 = b[0]; const crd::u8 t1 = b[1]; b[0] = b[3]; b[3] = t0; b[1] = b[2]; b[2] = t1; }
    crd::f32 v = 0.0F; std::memcpy(&v, b, 4);
    return v;
}

// ── RGBE ↔ float (Ward's canonical rgbe.c math; B→float→B is bit-exact idempotent) ──────────────────────────────────────────
void rgbe_to_float(const crd::u8 rgbe[4], crd::f32 out[3]) noexcept
{
    if (rgbe[3] == 0U) { out[0] = 0.0F; out[1] = 0.0F; out[2] = 0.0F; return; }
    const crd::f32 f = std::ldexp(1.0F, static_cast<int>(rgbe[3]) - (128 + 8));
    out[0] = static_cast<crd::f32>(rgbe[0]) * f;
    out[1] = static_cast<crd::f32>(rgbe[1]) * f;
    out[2] = static_cast<crd::f32>(rgbe[2]) * f;
}
void float_to_rgbe(crd::f32 r, crd::f32 g, crd::f32 b, crd::u8 rgbe[4]) noexcept
{
    r = r > 0.0F ? r : 0.0F; g = g > 0.0F ? g : 0.0F; b = b > 0.0F ? b : 0.0F; // RGBE has no negatives
    crd::f32 v = r > g ? r : g;
    v = v > b ? v : b;
    if (v < 1.0e-32F) { rgbe[0] = 0U; rgbe[1] = 0U; rgbe[2] = 0U; rgbe[3] = 0U; return; }
    int            e = 0;
    const crd::f32 m = std::frexp(v, &e);              // v = m·2^e, m ∈ [0.5,1)
    const crd::f32 scale = m * 256.0F / v;             // = 2^(-e)·256
    rgbe[0] = static_cast<crd::u8>(r * scale);
    rgbe[1] = static_cast<crd::u8>(g * scale);
    rgbe[2] = static_cast<crd::u8>(b * scale);
    rgbe[3] = static_cast<crd::u8>(e + 128);
}

// ── Radiance new-format RLE (one scanline of `width` RGBE pixels) ────────────────────────────────────────────────────────────
// Encode: emit the 4-byte new-RLE header then 4 channel runs (adaptive run/literal).
void rle_encode_channel(Array<crd::u8>& out, const crd::u8* chan, crd::u32 width)
{
    crd::u32 x = 0;
    while (x < width)
    {
        // find a run of >= 4 equal bytes at x
        crd::u32 run_start = x;
        crd::u32 run_len   = 0;
        while (run_start < width)
        {
            run_len = 1;
            while (run_start + run_len < width && run_len < 127U && chan[run_start + run_len] == chan[run_start]) { ++run_len; }
            if (run_len >= 4U) { break; }
            run_start += run_len;
            run_len = 0;
        }
        // emit the literal span [x, run_start)
        while (x < run_start)
        {
            crd::u32 lit = run_start - x;
            if (lit > 128U) { lit = 128U; }
            put_u8(out, static_cast<crd::u8>(lit));
            for (crd::u32 i = 0; i < lit; ++i) { put_u8(out, chan[x + i]); }
            x += lit;
        }
        // emit the run
        if (run_len >= 4U)
        {
            put_u8(out, static_cast<crd::u8>(run_len + 128U));
            put_u8(out, chan[run_start]);
            x = run_start + run_len;
        }
    }
}

} // namespace

// ── HdrImage ────────────────────────────────────────────────────────────────────────────────────────────────────────────
void HdrImage::resize(crd::u32 w, crd::u32 h, crd::u32 ch)
{
    width = w; height = h; channels = ch;
    pixels.resize(static_cast<crd::usize>(texel_count()) * ch);
    for (crd::usize i = 0; i < pixels.size(); ++i) { pixels[i] = 0.0F; }
}

// ── sniff ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
HdrCodec hdr_sniff(ConstSpan<crd::u8> bytes) noexcept
{
    if (bytes.size() >= 4U && bytes[0] == 0x76U && bytes[1] == 0x2FU && bytes[2] == 0x31U && bytes[3] == 0x01U) { return HdrCodec::Exr; }
    if (bytes.size() >= 2U && bytes[0] == static_cast<crd::u8>('P') && (bytes[1] == static_cast<crd::u8>('F') || bytes[1] == static_cast<crd::u8>('f'))) { return HdrCodec::Pfm; }
    if (bytes.size() >= 2U && bytes[0] == static_cast<crd::u8>('#') && bytes[1] == static_cast<crd::u8>('?')) { return HdrCodec::Radiance; }
    return HdrCodec::Unknown;
}

HdrError hdr_decode(ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a)
{
    switch (hdr_sniff(bytes))
    {
        case HdrCodec::Radiance: return hdr_decode_radiance(bytes, out, a);
        case HdrCodec::Pfm:      return hdr_decode_pfm(bytes, out, a);
        case HdrCodec::Exr:      return hdr_decode_exr(bytes, out, a);
        default:                 return HdrError::BadMagic;
    }
}

// ── Radiance (.hdr) decode ──────────────────────────────────────────────────────────────────────────────────────────────
HdrError hdr_decode_radiance(ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a)
{
    (void)a;
    const crd::u8* p   = bytes.data();
    const crd::u8* end = bytes.data() + bytes.size();
    if (bytes.size() < 2U || p[0] != static_cast<crd::u8>('#') || p[1] != static_cast<crd::u8>('?')) { return HdrError::BadMagic; }

    // Header: text lines until a blank line; then the resolution line.
    auto next_line = [&](const crd::u8*& q, char* buf, crd::usize cap) -> bool {
        crd::usize n = 0;
        while (q < end && *q != static_cast<crd::u8>('\n')) { if (n + 1 < cap) { buf[n++] = static_cast<char>(*q); } ++q; }
        if (q >= end) { return false; }
        ++q; // skip '\n'
        buf[n] = '\0';
        return true;
    };
    char line[256];
    const crd::u8* q = p;
    bool blank_seen = false;
    while (next_line(q, line, sizeof(line)))
    {
        if (line[0] == '\0') { blank_seen = true; break; }
    }
    if (!blank_seen) { return HdrError::BadHeader; }
    if (!next_line(q, line, sizeof(line))) { return HdrError::BadHeader; }

    // Resolution line: "-Y H +X W" (standard orientation). Parse the two integers after -Y and +X.
    auto parse_after = [&](const char* key, crd::u32& val) -> bool {
        const char* s = std::strstr(line, key);
        if (s == nullptr) { return false; }
        s += std::strlen(key);
        while (*s == ' ') { ++s; }
        crd::u32 v = 0; bool any = false;
        while (*s >= '0' && *s <= '9') { v = v * 10U + static_cast<crd::u32>(*s - '0'); ++s; any = true; }
        val = v; return any;
    };
    crd::u32 h = 0; crd::u32 w = 0;
    if (!parse_after("-Y", h) || !parse_after("+X", w)) { return HdrError::BadHeader; }
    if (w == 0 || h == 0) { return HdrError::BadHeader; }
    if (w > kMaxDim || h > kMaxDim) { return HdrError::TooLarge; }

    out.resize(w, h, 3U);
    Array<crd::u8> scan(a); scan.resize(static_cast<crd::usize>(w) * 4U); // R,G,B,E deinterleaved per scanline

    for (crd::u32 y = 0; y < h; ++y)
    {
        // Peek the scanline header (4 bytes).
        if (end - q < 4) { return HdrError::Truncated; }
        const crd::u8 h0 = q[0]; const crd::u8 h1 = q[1]; const crd::u8 h2 = q[2]; const crd::u8 h3 = q[3];
        const bool new_rle = (h0 == 2U && h1 == 2U && ((static_cast<crd::u32>(h2) << 8U) | h3) == w && w >= 8U && w <= 0x7FFFU);
        if (new_rle)
        {
            q += 4;
            for (crd::u32 ch = 0; ch < 4U; ++ch)
            {
                crd::u32 x = 0;
                while (x < w)
                {
                    if (q >= end) { return HdrError::Truncated; }
                    const crd::u8 count = *q++;
                    if (count > 128U)
                    {
                        const crd::u32 run = count - 128U;
                        if (q >= end || x + run > w) { return HdrError::BadData; }
                        const crd::u8 val = *q++;
                        for (crd::u32 i = 0; i < run; ++i) { scan[static_cast<crd::usize>(x++) * 4U + ch] = val; }
                    }
                    else
                    {
                        const crd::u32 lit = count;
                        if (end - q < static_cast<crd::isize>(lit) || x + lit > w) { return HdrError::BadData; }
                        for (crd::u32 i = 0; i < lit; ++i) { scan[static_cast<crd::usize>(x++) * 4U + ch] = *q++; }
                    }
                }
            }
        }
        else
        {
            // Flat (or old-format) — read w RGBE pixels, honoring old-format (1,1,1,n) repeat runs.
            crd::u8 prev[4] = {0, 0, 0, 0};
            crd::u32 rshift = 0;
            crd::u32 x = 0;
            while (x < w)
            {
                if (end - q < 4) { return HdrError::Truncated; }
                const crd::u8 r0 = q[0]; const crd::u8 g0 = q[1]; const crd::u8 b0 = q[2]; const crd::u8 e0 = q[3]; q += 4;
                if (r0 == 1U && g0 == 1U && b0 == 1U && x > 0)
                {
                    crd::u32 run = static_cast<crd::u32>(e0) << rshift;
                    if (x + run > w) { run = w - x; }
                    for (crd::u32 i = 0; i < run; ++i) { for (int c = 0; c < 4; ++c) { scan[static_cast<crd::usize>(x) * 4U + c] = prev[c]; } ++x; }
                    rshift += 8U;
                }
                else
                {
                    scan[static_cast<crd::usize>(x) * 4U + 0] = r0; scan[static_cast<crd::usize>(x) * 4U + 1] = g0;
                    scan[static_cast<crd::usize>(x) * 4U + 2] = b0; scan[static_cast<crd::usize>(x) * 4U + 3] = e0;
                    prev[0] = r0; prev[1] = g0; prev[2] = b0; prev[3] = e0; rshift = 0; ++x;
                }
            }
        }
        for (crd::u32 x = 0; x < w; ++x)
        {
            const crd::u8 rgbe[4] = {scan[static_cast<crd::usize>(x) * 4U + 0], scan[static_cast<crd::usize>(x) * 4U + 1], scan[static_cast<crd::usize>(x) * 4U + 2], scan[static_cast<crd::usize>(x) * 4U + 3]};
            crd::f32 rgb[3]; rgbe_to_float(rgbe, rgb);
            out.at(x, y, 0) = rgb[0]; out.at(x, y, 1) = rgb[1]; out.at(x, y, 2) = rgb[2];
        }
    }
    return HdrError::Ok;
}

// ── Radiance (.hdr) encode ──────────────────────────────────────────────────────────────────────────────────────────────
Array<crd::u8> hdr_encode_radiance(const HdrImage& img, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    if (!img.valid() || img.channels != 3U) { return out; }
    put_str(out, "#?RADIANCE\n");
    put_str(out, "FORMAT=32-bit_rle_rgbe\n\n");
    // resolution line "-Y H +X W\n"
    char reso[64]; int n = 0;
    const char* fmt_y = "-Y "; for (const char* s = fmt_y; *s; ++s) { reso[n++] = *s; }
    auto emit_uint = [&](crd::u32 v) { char tmp[12]; int t = 0; if (v == 0) { tmp[t++] = '0'; } while (v > 0) { tmp[t++] = static_cast<char>('0' + (v % 10U)); v /= 10U; } while (t > 0) { reso[n++] = tmp[--t]; } };
    emit_uint(img.height); reso[n++] = ' '; reso[n++] = '+'; reso[n++] = 'X'; reso[n++] = ' '; emit_uint(img.width); reso[n++] = '\n'; reso[n] = '\0';
    put_str(out, reso);

    const bool use_rle = img.width >= 8U && img.width <= 0x7FFFU;
    Array<crd::u8> chans(a); chans.resize(static_cast<crd::usize>(img.width) * 4U);
    for (crd::u32 y = 0; y < img.height; ++y)
    {
        for (crd::u32 x = 0; x < img.width; ++x)
        {
            crd::u8 rgbe[4];
            float_to_rgbe(img.at(x, y, 0), img.at(x, y, 1), img.at(x, y, 2), rgbe);
            for (int c = 0; c < 4; ++c) { chans[static_cast<crd::usize>(c) * img.width + x] = rgbe[c]; } // deinterleaved per channel
        }
        if (use_rle)
        {
            put_u8(out, 2U); put_u8(out, 2U);
            put_u8(out, static_cast<crd::u8>((img.width >> 8U) & 0xFFU)); put_u8(out, static_cast<crd::u8>(img.width & 0xFFU));
            for (crd::u32 ch = 0; ch < 4U; ++ch) { rle_encode_channel(out, chans.data() + static_cast<crd::usize>(ch) * img.width, img.width); }
        }
        else
        {
            for (crd::u32 x = 0; x < img.width; ++x) { for (int c = 0; c < 4; ++c) { put_u8(out, chans[static_cast<crd::usize>(c) * img.width + x]); } }
        }
    }
    return out;
}

// ── PFM decode ──────────────────────────────────────────────────────────────────────────────────────────────────────────
HdrError hdr_decode_pfm(ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a)
{
    (void)a;
    const crd::u8* p   = bytes.data();
    const crd::u8* end = bytes.data() + bytes.size();
    if (bytes.size() < 2U || p[0] != static_cast<crd::u8>('P')) { return HdrError::BadMagic; }
    const bool color = p[1] == static_cast<crd::u8>('F');
    const bool gray  = p[1] == static_cast<crd::u8>('f');
    if (!color && !gray) { return HdrError::BadMagic; }
    const crd::u32 ch = color ? 3U : 1U;

    // Three whitespace-separated tokens after the magic: width, height, scale (sign = endianness).
    const crd::u8* q = p + 2;
    auto skip_ws = [&]() { while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) { ++q; } };
    auto read_uint = [&](crd::u32& v) -> bool { skip_ws(); crd::u32 x = 0; bool any = false; while (q < end && *q >= static_cast<crd::u8>('0') && *q <= static_cast<crd::u8>('9')) { x = x * 10U + static_cast<crd::u32>(*q - static_cast<crd::u8>('0')); ++q; any = true; } v = x; return any; };
    crd::u32 w = 0; crd::u32 h = 0;
    if (!read_uint(w) || !read_uint(h)) { return HdrError::BadHeader; }
    // scale line: optional sign, digits, optional '.'; the SIGN is what matters (endianness).
    skip_ws();
    bool little_endian = true;
    if (q < end && *q == static_cast<crd::u8>('-')) { little_endian = true; ++q; }
    else if (q < end && *q == static_cast<crd::u8>('+')) { little_endian = false; ++q; }
    else { little_endian = false; }
    while (q < end && *q != static_cast<crd::u8>('\n')) { ++q; } // consume the rest of the scale token/line
    if (q < end) { ++q; } // the single whitespace after scale (the '\n')
    if (w == 0 || h == 0) { return HdrError::BadHeader; }
    if (w > kMaxDim || h > kMaxDim) { return HdrError::TooLarge; }

    const crd::u64 need = static_cast<crd::u64>(w) * h * ch * 4U;
    if (static_cast<crd::u64>(end - q) < need) { return HdrError::Truncated; }

    out.resize(w, h, ch);
    // PFM rows are stored BOTTOM-to-TOP; flip to our top-to-bottom convention.
    for (crd::u32 row = 0; row < h; ++row)
    {
        const crd::u32 y = h - 1U - row; // source row `row` (from bottom) → dest row y (from top)
        for (crd::u32 x = 0; x < w; ++x)
        {
            for (crd::u32 c = 0; c < ch; ++c)
            {
                out.at(x, y, c) = read_f32(q, little_endian);
                q += 4;
            }
        }
    }
    return HdrError::Ok;
}

// ── PFM encode ──────────────────────────────────────────────────────────────────────────────────────────────────────────
Array<crd::u8> hdr_encode_pfm(const HdrImage& img, bool little_endian, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    if (!img.valid()) { return out; }
    put_str(out, img.channels == 3U ? "PF\n" : "Pf\n");
    char hdr[64]; int n = 0;
    auto emit_uint = [&](crd::u32 v) { char tmp[12]; int t = 0; if (v == 0) { tmp[t++] = '0'; } while (v > 0) { tmp[t++] = static_cast<char>('0' + (v % 10U)); v /= 10U; } while (t > 0) { hdr[n++] = tmp[--t]; } };
    emit_uint(img.width); hdr[n++] = ' '; emit_uint(img.height); hdr[n++] = '\n'; hdr[n] = '\0';
    put_str(out, hdr);
    put_str(out, little_endian ? "-1.0\n" : "1.0\n");

    for (crd::u32 row = 0; row < img.height; ++row)
    {
        const crd::u32 y = img.height - 1U - row; // bottom-to-top
        for (crd::u32 x = 0; x < img.width; ++x)
        {
            for (crd::u32 c = 0; c < img.channels; ++c)
            {
                const crd::f32 v = img.at(x, y, c);
                if (little_endian) { put_f32_le(out, v); }
                else { crd::u32 bits = 0; std::memcpy(&bits, &v, 4); for (int i = 3; i >= 0; --i) { put_u8(out, static_cast<crd::u8>((bits >> (8U * i)) & 0xFFU)); } }
            }
        }
    }
    return out;
}

// ── CRDR round-trip ─────────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
constexpr crd::u32 kHdriFourcc = make_fourcc('H', 'D', 'R', 'I');
constexpr crd::u32 kPixfFourcc = make_fourcc('P', 'I', 'X', 'F');
} // namespace

Array<crd::u8> hdr_to_crdr(const HdrImage& img, ResourceId id, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    if (!img.valid()) { return out; }
    CrdrWriter writer(a, id, kHdriFourcc);
    // HEAD (16 bytes): width, height, channels, format-tag(=1 float32).
    Array<crd::u8> head(a);
    put_u32_le(head, img.width); put_u32_le(head, img.height); put_u32_le(head, img.channels); put_u32_le(head, 1U);
    writer.add_chunk(kFourCC_HEAD, {head.data(), head.size()});
    // PIXF: raw little-endian f32 pixels.
    Array<crd::u8> pix(a);
    for (crd::usize i = 0; i < img.pixels.size(); ++i) { put_f32_le(pix, img.pixels[i]); }
    writer.add_chunk(kPixfFourcc, {pix.data(), pix.size()});
    return writer.finish();
}

HdrError hdr_from_crdr(ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a)
{
    CrdrFile file(a);
    if (crdr_read(bytes, file, a) != CrdrError::Ok) { return HdrError::BadData; }
    if (file.type_fourcc != kHdriFourcc) { return HdrError::BadMagic; }
    const CrdrChunk* head = crdr_find_chunk(file, kFourCC_HEAD);
    const CrdrChunk* pix  = crdr_find_chunk(file, kPixfFourcc);
    if (head == nullptr || pix == nullptr || head->payload.size() < 16U) { return HdrError::BadHeader; }
    crd::u32 w = 0; crd::u32 h = 0; crd::u32 ch = 0;
    std::memcpy(&w, head->payload.data() + 0, 4); std::memcpy(&h, head->payload.data() + 4, 4); std::memcpy(&ch, head->payload.data() + 8, 4);
    if (w == 0 || h == 0 || (ch != 1U && ch != 3U)) { return HdrError::BadHeader; }
    if (w > kMaxDim || h > kMaxDim) { return HdrError::TooLarge; }
    const crd::u64 need = static_cast<crd::u64>(w) * h * ch * 4U;
    if (pix->payload.size() < need) { return HdrError::Truncated; }
    out.resize(w, h, ch);
    for (crd::usize i = 0; i < out.pixels.size(); ++i) { out.pixels[i] = read_f32(pix->payload.data() + i * 4U, true); }
    return HdrError::Ok;
}

} // namespace crd::resources
