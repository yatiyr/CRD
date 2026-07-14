#include <crd/resources/hdr_image.hpp>

#include "hdr_piz.hpp"

#include <crd/core/types.hpp>
#include <crd/resources/deflate.hpp>

#include <cstring>

// hdr_exr.cpp — B-hdr-c: OUR OWN OpenEXR codec (scanline, single-part). Step 1: the container + NONE + RLE compression +
// our own IEEE half↔float. ZIP (our own DEFLATE) = B-hdr-c step 2; PIZ (wavelet + Huffman) = step 3.
//
// EXR file: magic(0x01312f76) · version(2 + flags) · header attributes (name\0 type\0 i32-size value …, empty-name \0
// terminates) · offset table (i64 per scanline block) · blocks (i32 y, i32 dataSize, data). Required attrs: channels(chlist),
// compression(u8), dataWindow/displayWindow(box2i), lineOrder(u8), pixelAspectRatio(f32), screenWindowCenter(v2f),
// screenWindowWidth(f32). Channels are alphabetical (B,G,R); each scanline stores channels planar in that order.
// RLE/ZIP first REORDER (split into two interleaved halves) + PREDICTOR (byte delta +128) the raw bytes, then byte-compress.

namespace crd::resources
{

using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{

constexpr crd::u32 kExrMagic  = 0x01312F76U;
constexpr crd::u32 kMaxDimExr = 65536U;

// ── IEEE half ↔ float (our own; round-to-nearest-even) ──────────────────────────────────────────────────────────────────
[[nodiscard]] crd::f32 half_to_float(crd::u16 h) noexcept
{
    const crd::u32 sign = (static_cast<crd::u32>(h) & 0x8000U) << 16U;
    crd::u32       exp  = (static_cast<crd::u32>(h) >> 10U) & 0x1FU;
    crd::u32       mant = static_cast<crd::u32>(h) & 0x3FFU;
    crd::u32       f    = 0;
    if (exp == 0U)
    {
        if (mant != 0U)
        {
            exp = 1U;
            while ((mant & 0x400U) == 0U) { mant <<= 1U; --exp; }
            mant &= 0x3FFU;
            f = sign | ((exp + (127U - 15U)) << 23U) | (mant << 13U);
        }
        else { f = sign; }
    }
    else if (exp == 0x1FU) { f = sign | (0xFFU << 23U) | (mant << 13U); }
    else { f = sign | ((exp + (127U - 15U)) << 23U) | (mant << 13U); }
    crd::f32 out = 0.0F; std::memcpy(&out, &f, 4);
    return out;
}
[[nodiscard]] crd::u16 float_to_half(crd::f32 v) noexcept
{
    crd::u32 f = 0; std::memcpy(&f, &v, 4);
    const crd::u16 sign = static_cast<crd::u16>((f >> 16U) & 0x8000U);
    const crd::u32 fexp = (f >> 23U) & 0xFFU;
    const crd::u32 mant = f & 0x7FFFFFU;
    const int      exp  = static_cast<int>(fexp) - 127 + 15;
    if (fexp == 0xFFU) { return static_cast<crd::u16>(sign | 0x7C00U | (mant != 0U ? 0x200U : 0U)); } // inf/nan
    if (exp >= 0x1F) { return static_cast<crd::u16>(sign | 0x7C00U); }                                 // overflow → inf
    if (exp <= 0)
    {
        if (exp < -10) { return sign; } // → ±0
        const crd::u32 m     = (mant | 0x800000U);
        const int      shift = 14 - exp;
        crd::u32       hm    = m >> static_cast<crd::u32>(shift);
        const crd::u32 rem   = m & ((1U << static_cast<crd::u32>(shift)) - 1U);
        const crd::u32 halfb = 1U << static_cast<crd::u32>(shift - 1);
        if (rem > halfb || (rem == halfb && (hm & 1U) != 0U)) { ++hm; }
        return static_cast<crd::u16>(sign | hm);
    }
    crd::u16       h   = static_cast<crd::u16>(sign | (static_cast<crd::u32>(exp) << 10U) | (mant >> 13U));
    const crd::u32 rem = mant & 0x1FFFU;
    if (rem > 0x1000U || (rem == 0x1000U && (h & 1U) != 0U)) { ++h; } // round to nearest even (carry into exp is correct)
    return h;
}

// ── little-endian byte I/O ───────────────────────────────────────────────────────────────────────────────────────────────
void put_u8(Array<crd::u8>& o, crd::u8 v) { o.push_back(v); }
void put_u32(Array<crd::u8>& o, crd::u32 v) { for (int i = 0; i < 4; ++i) { o.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU)); } }
void put_i32(Array<crd::u8>& o, crd::i32 v) { put_u32(o, static_cast<crd::u32>(v)); }
void put_u64(Array<crd::u8>& o, crd::u64 v) { for (int i = 0; i < 8; ++i) { o.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU)); } }
void put_f32(Array<crd::u8>& o, crd::f32 v) { crd::u32 b = 0; std::memcpy(&b, &v, 4); put_u32(o, b); }
void put_cstr(Array<crd::u8>& o, const char* s) { for (const char* p = s; *p != '\0'; ++p) { o.push_back(static_cast<crd::u8>(*p)); } o.push_back(0U); }
[[nodiscard]] crd::u32 get_u32(const crd::u8* p) noexcept { crd::u32 v = 0; for (int i = 0; i < 4; ++i) { v |= static_cast<crd::u32>(p[i]) << (8U * i); } return v; }
[[nodiscard]] crd::i32 get_i32(const crd::u8* p) noexcept { return static_cast<crd::i32>(get_u32(p)); }
[[nodiscard]] crd::u64 get_u64(const crd::u8* p) noexcept { crd::u64 v = 0; for (int i = 0; i < 8; ++i) { v |= static_cast<crd::u64>(p[i]) << (8U * i); } return v; }
[[nodiscard]] crd::u16 get_u16(const crd::u8* p) noexcept { return static_cast<crd::u16>(static_cast<crd::u32>(p[0]) | (static_cast<crd::u32>(p[1]) << 8U)); }

// ── EXR reorder + predictor (the RLE/ZIP pre-transform) ─────────────────────────────────────────────────────────────────
void exr_reorder_predict(const crd::u8* raw, crd::usize n, Array<crd::u8>& tmp)
{
    tmp.resize(n);
    // reorder: split into two interleaved halves
    crd::usize t1 = 0; crd::usize t2 = (n + 1U) / 2U; crd::usize i = 0;
    while (true) { if (i < n) { tmp[t1++] = raw[i++]; } else { break; } if (i < n) { tmp[t2++] = raw[i++]; } else { break; } }
    // predictor: byte delta (+ 128+256, keep low byte)
    if (n > 0U)
    {
        int p = tmp[0];
        for (crd::usize k = 1; k < n; ++k) { const int cur = tmp[k]; const int d = cur - p + (128 + 256); p = cur; tmp[k] = static_cast<crd::u8>(d); }
    }
}
void exr_unpredict_reorder(crd::u8* tmp, crd::usize n, crd::u8* out)
{
    // predictor undo
    for (crd::usize k = 1; k < n; ++k) { const int d = static_cast<int>(tmp[k - 1]) + static_cast<int>(tmp[k]) - 128; tmp[k] = static_cast<crd::u8>(d); }
    // reorder undo
    crd::usize t1 = 0; crd::usize t2 = (n + 1U) / 2U; crd::usize i = 0;
    while (true) { if (i < n) { out[i++] = tmp[t1++]; } else { break; } if (i < n) { out[i++] = tmp[t2++]; } else { break; } }
}

// ── EXR byte-RLE (signed-count format) ───────────────────────────────────────────────────────────────────────────────────
void exr_rle_compress(const crd::u8* in, crd::usize n, Array<crd::u8>& out)
{
    crd::usize i = 0;
    while (i < n)
    {
        crd::usize run = 1;
        while (i + run < n && in[i + run] == in[i] && run < 128U) { ++run; }
        if (run >= 3U)
        {
            put_u8(out, static_cast<crd::u8>(run - 1U)); // positive count-1 ∈ [2,127]
            put_u8(out, in[i]);
            i += run;
        }
        else
        {
            const crd::usize lit_start = i;
            while (i < n)
            {
                crd::usize r = 1; while (i + r < n && in[i + r] == in[i] && r < 3U) { ++r; }
                if (r >= 3U) { break; }
                ++i;
                if (i - lit_start >= 128U) { break; }
            }
            const crd::usize lit = i - lit_start;
            put_u8(out, static_cast<crd::u8>(static_cast<int>(-static_cast<int>(lit)))); // negative literal count
            for (crd::usize k = 0; k < lit; ++k) { put_u8(out, in[lit_start + k]); }
        }
    }
}
[[nodiscard]] bool exr_rle_decompress(const crd::u8* in, crd::usize in_n, crd::u8* out, crd::usize out_n)
{
    crd::usize ip = 0; crd::usize op = 0;
    while (op < out_n)
    {
        if (ip >= in_n) { return false; }
        const crd::u8 cb = in[ip++];
        const int     c  = cb < 128U ? static_cast<int>(cb) : static_cast<int>(cb) - 256; // signed count
        if (c < 0)
        {
            const crd::usize lit = static_cast<crd::usize>(-c);
            if (ip + lit > in_n || op + lit > out_n) { return false; }
            for (crd::usize k = 0; k < lit; ++k) { out[op++] = in[ip++]; }
        }
        else
        {
            const crd::usize run = static_cast<crd::usize>(c) + 1U;
            if (ip >= in_n || op + run > out_n) { return false; }
            const crd::u8 v = in[ip++];
            for (crd::usize k = 0; k < run; ++k) { out[op++] = v; }
        }
    }
    return true;
}

// bytes-per-sample for an EXR pixelType (0=UINT,1=HALF,2=FLOAT).
[[nodiscard]] crd::u32 exr_bpp(crd::i32 pt) noexcept { return pt == 1 ? 2U : 4U; }

} // namespace

// ── encode ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
Array<crd::u8> hdr_encode_exr(const HdrImage& img, ExrPixelType pixel_type, ExrCompression compression, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    if (!img.valid()) { return out; }
    const bool     is_half = pixel_type == ExrPixelType::Half;
    const crd::u32 bpp     = is_half ? 2U : 4U;
    const crd::i32 pt      = is_half ? 1 : 2;             // HALF or FLOAT
    const crd::u32 nch     = img.channels;               // 3 (RGB→B,G,R) or 1 (Y)
    const crd::u8  comp    = static_cast<crd::u8>(compression);

    // Header.
    put_u32(out, kExrMagic);
    put_u32(out, 2U); // version 2, flags 0 (scanline, single-part)

    // channels (chlist) — alphabetical: RGB → B,G,R ; gray → Y.
    put_cstr(out, "channels"); put_cstr(out, "chlist");
    Array<crd::u8> ch(a);
    const char* names3[3] = {"B", "G", "R"};
    if (nch == 3U) { for (const char* nm : names3) { put_cstr(ch, nm); put_i32(ch, pt); put_u8(ch, 0U); put_u8(ch, 0U); put_u8(ch, 0U); put_u8(ch, 0U); put_i32(ch, 1); put_i32(ch, 1); } }
    else { put_cstr(ch, "Y"); put_i32(ch, pt); put_u8(ch, 0U); put_u8(ch, 0U); put_u8(ch, 0U); put_u8(ch, 0U); put_i32(ch, 1); put_i32(ch, 1); }
    put_u8(ch, 0U); // empty-name terminator
    put_i32(out, static_cast<crd::i32>(ch.size())); for (crd::usize i = 0; i < ch.size(); ++i) { out.push_back(ch[i]); }

    put_cstr(out, "compression"); put_cstr(out, "compression"); put_i32(out, 1); put_u8(out, comp);
    put_cstr(out, "dataWindow"); put_cstr(out, "box2i"); put_i32(out, 16); put_i32(out, 0); put_i32(out, 0); put_i32(out, static_cast<crd::i32>(img.width) - 1); put_i32(out, static_cast<crd::i32>(img.height) - 1);
    put_cstr(out, "displayWindow"); put_cstr(out, "box2i"); put_i32(out, 16); put_i32(out, 0); put_i32(out, 0); put_i32(out, static_cast<crd::i32>(img.width) - 1); put_i32(out, static_cast<crd::i32>(img.height) - 1);
    put_cstr(out, "lineOrder"); put_cstr(out, "lineOrder"); put_i32(out, 1); put_u8(out, 0U); // INCREASING_Y
    put_cstr(out, "pixelAspectRatio"); put_cstr(out, "float"); put_i32(out, 4); put_f32(out, 1.0F);
    put_cstr(out, "screenWindowCenter"); put_cstr(out, "v2f"); put_i32(out, 8); put_f32(out, 0.0F); put_f32(out, 0.0F);
    put_cstr(out, "screenWindowWidth"); put_cstr(out, "float"); put_i32(out, 4); put_f32(out, 1.0F);
    put_u8(out, 0U); // end-of-header

    // Offset table: one i64 per scanline block. NONE/RLE = 1 line per block; ZIP = 16; PIZ = 32.
    crd::u32 lpb = 1U;
    if (compression == ExrCompression::Zip) { lpb = 16U; }
    else if (compression == ExrCompression::Piz) { lpb = 32U; }
    const crd::u32   blocks = (img.height + lpb - 1U) / lpb;
    crd::u8          word_counts[3] = {static_cast<crd::u8>(bpp / 2U), static_cast<crd::u8>(bpp / 2U), static_cast<crd::u8>(bpp / 2U)};
    const crd::usize ot_pos = out.size();
    for (crd::u32 b = 0; b < blocks; ++b) { put_u64(out, 0U); } // placeholder, patched below

    // channel order for planar layout (matches the chlist order). RGB: B(src2), G(src1), R(src0). gray: Y(src0).
    const crd::u32   src_for_ch[3] = {2U, 1U, 0U};
    const crd::usize scan_bytes    = static_cast<crd::usize>(img.width) * nch * bpp;

    Array<crd::u8> raw(a); Array<crd::u8> tmp(a); Array<crd::u8> comp_buf(a);
    Array<crd::u64> offsets(a); offsets.resize(blocks);
    // append `src` (compressed) if smaller than `raw`, else store `raw` uncompressed; write the i32 dataSize first.
    const auto emit_payload = [&](const Array<crd::u8>& src) {
        if (src.size() >= raw.size()) { put_i32(out, static_cast<crd::i32>(raw.size())); for (crd::usize i = 0; i < raw.size(); ++i) { out.push_back(raw[i]); } }
        else { put_i32(out, static_cast<crd::i32>(src.size())); for (crd::usize i = 0; i < src.size(); ++i) { out.push_back(src[i]); } }
    };
    for (crd::u32 b = 0; b < blocks; ++b)
    {
        offsets[b] = out.size();
        const crd::u32 y0     = b * lpb;
        const crd::u32 nlines = (y0 + lpb <= img.height) ? lpb : (img.height - y0);
        raw.resize(scan_bytes * nlines);
        crd::usize w = 0;
        for (crd::u32 ly = 0; ly < nlines; ++ly)
        {
            const crd::u32 y = y0 + ly;
            for (crd::u32 c = 0; c < nch; ++c)
            {
                const crd::u32 src = nch == 3U ? src_for_ch[c] : 0U;
                for (crd::u32 x = 0; x < img.width; ++x)
                {
                    const crd::f32 v = img.at(x, y, src);
                    if (is_half) { const crd::u16 h = float_to_half(v); raw[w++] = static_cast<crd::u8>(h & 0xFFU); raw[w++] = static_cast<crd::u8>((h >> 8U) & 0xFFU); }
                    else { crd::u32 bits = 0; std::memcpy(&bits, &v, 4); raw[w++] = static_cast<crd::u8>(bits & 0xFFU); raw[w++] = static_cast<crd::u8>((bits >> 8U) & 0xFFU); raw[w++] = static_cast<crd::u8>((bits >> 16U) & 0xFFU); raw[w++] = static_cast<crd::u8>((bits >> 24U) & 0xFFU); }
                }
            }
        }
        put_i32(out, static_cast<crd::i32>(y0)); // block first-scanline y
        if (compression == ExrCompression::None) { put_i32(out, static_cast<crd::i32>(raw.size())); for (crd::usize i = 0; i < raw.size(); ++i) { out.push_back(raw[i]); } }
        else if (compression == ExrCompression::Rle) { exr_reorder_predict(raw.data(), raw.size(), tmp); comp_buf.resize(0); exr_rle_compress(tmp.data(), tmp.size(), comp_buf); emit_payload(comp_buf); }
        else if (compression == ExrCompression::Zip) { exr_reorder_predict(raw.data(), raw.size(), tmp); comp_buf = zlib_deflate({tmp.data(), tmp.size()}, a); emit_payload(comp_buf); }
        else { comp_buf = piz_detail::piz_compress(raw.data(), raw.size(), img.width, nlines, word_counts, nch, a); emit_payload(comp_buf); } // PIZ
    }
    for (crd::u32 b = 0; b < blocks; ++b) { const crd::u64 v = offsets[b]; for (int i = 0; i < 8; ++i) { out[ot_pos + static_cast<crd::usize>(b) * 8U + i] = static_cast<crd::u8>((v >> (8U * i)) & 0xFFU); } }
    return out;
}

// ── decode ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
HdrError hdr_decode_exr(ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a)
{
    const crd::u8* p   = bytes.data();
    const crd::u8* end = bytes.data() + bytes.size();
    if (bytes.size() < 8U || get_u32(p) != kExrMagic) { return HdrError::BadMagic; }
    const crd::u32 ver = get_u32(p + 4);
    if ((ver & 0xFFU) != 2U) { return HdrError::Unsupported; }
    if (((ver >> 9U) & 1U) != 0U || ((ver >> 11U) & 1U) != 0U || ((ver >> 12U) & 1U) != 0U) { return HdrError::Unsupported; } // tiled/multipart/deep
    const crd::u8* q = p + 8;

    // Parse header attributes.
    struct Chan { char name[32]; crd::i32 type; };
    Chan     chans[8]; crd::u32 nchan = 0;
    crd::u8  comp = 0; bool have_comp = false;
    crd::i32 x_min = 0; crd::i32 y_min = 0; crd::i32 x_max = -1; crd::i32 y_max = -1; bool have_dw = false;
    crd::u8  line_order = 0;

    auto read_cstr = [&](const crd::u8*& r, char* buf, crd::usize cap) -> bool {
        crd::usize n = 0;
        while (r < end && *r != 0U) { if (n + 1 < cap) { buf[n++] = static_cast<char>(*r); } ++r; }
        if (r >= end) { return false; }
        ++r; buf[n] = '\0'; return true;
    };

    while (true)
    {
        if (q >= end) { return HdrError::Truncated; }
        if (*q == 0U) { ++q; break; } // end of header
        char name[64]; char type[64];
        if (!read_cstr(q, name, sizeof(name))) { return HdrError::Truncated; }
        if (name[0] == '\0') { break; }
        if (!read_cstr(q, type, sizeof(type))) { return HdrError::Truncated; }
        if (end - q < 4) { return HdrError::Truncated; }
        const crd::i32 sz = get_i32(q); q += 4;
        if (sz < 0 || end - q < sz) { return HdrError::Truncated; }
        const crd::u8* val = q;
        if (std::strcmp(name, "channels") == 0)
        {
            const crd::u8* c = val; const crd::u8* cend = val + sz;
            while (c < cend && *c != 0U)
            {
                char cn[64]; if (!read_cstr(c, cn, sizeof(cn))) { return HdrError::BadHeader; }
                if (cend - c < 16) { return HdrError::BadHeader; }
                const crd::i32 ctype = get_i32(c); c += 16; // type + pLinear/reserved(4) + xSampling(4) + ySampling(4)
                if (nchan < 8U) { std::memset(chans[nchan].name, 0, sizeof(chans[nchan].name)); crd::usize cl = 0; while (cn[cl] != '\0' && cl + 1 < sizeof(chans[nchan].name)) { chans[nchan].name[cl] = cn[cl]; ++cl; } chans[nchan].type = ctype; ++nchan; }
            }
        }
        else if (std::strcmp(name, "compression") == 0 && sz >= 1) { comp = *val; have_comp = true; }
        else if (std::strcmp(name, "dataWindow") == 0 && sz >= 16) { x_min = get_i32(val); y_min = get_i32(val + 4); x_max = get_i32(val + 8); y_max = get_i32(val + 12); have_dw = true; }
        else if (std::strcmp(name, "lineOrder") == 0 && sz >= 1) { line_order = *val; }
        q += sz;
    }
    if (!have_comp || !have_dw || nchan == 0U) { return HdrError::BadHeader; }
    if (comp != 0U && comp != 1U && comp != 2U && comp != 3U && comp != 4U) { return HdrError::Unsupported; } // NONE/RLE/ZIPS/ZIP/PIZ
    if (line_order > 1U) { return HdrError::Unsupported; }
    const crd::i64 wl = static_cast<crd::i64>(x_max) - x_min + 1;
    const crd::i64 hl = static_cast<crd::i64>(y_max) - y_min + 1;
    if (wl <= 0 || hl <= 0 || wl > kMaxDimExr || hl > kMaxDimExr) { return HdrError::BadHeader; }
    const crd::u32 w = static_cast<crd::u32>(wl); const crd::u32 h = static_cast<crd::u32>(hl);

    // Map channels → R/G/B slot; compute the planar byte offset of each channel within a scanline.
    // The chlist is alphabetical, so scanline layout order == chans[] order.
    int slot[8]; crd::u32 bpp[8]; crd::usize plane_off[8]; crd::usize scan_bytes = 0;
    bool has_rgb[3] = {false, false, false}; bool has_y = false;
    for (crd::u32 i = 0; i < nchan; ++i)
    {
        if (chans[i].type != 1 && chans[i].type != 2) { return HdrError::Unsupported; } // HALF/FLOAT only
        bpp[i]       = exr_bpp(chans[i].type);
        plane_off[i] = scan_bytes;
        scan_bytes  += static_cast<crd::usize>(w) * bpp[i];
        slot[i]      = -1;
        if (std::strcmp(chans[i].name, "R") == 0) { slot[i] = 0; has_rgb[0] = true; }
        else if (std::strcmp(chans[i].name, "G") == 0) { slot[i] = 1; has_rgb[1] = true; }
        else if (std::strcmp(chans[i].name, "B") == 0) { slot[i] = 2; has_rgb[2] = true; }
        else if (std::strcmp(chans[i].name, "Y") == 0) { slot[i] = 0; has_y = true; }
    }
    crd::u32 nch_out = 0U;
    if (has_rgb[0] && has_rgb[1] && has_rgb[2]) { nch_out = 3U; }
    else if (has_y) { nch_out = 1U; }
    if (nch_out == 0U) { return HdrError::Unsupported; }
    out.resize(w, h, nch_out);

    // Offset table (one i64 per block; ZIP = 16 lines/block, PIZ = 32, others = 1), then blocks.
    crd::u32 lpb = 1U;
    if (comp == 3U) { lpb = 16U; }
    else if (comp == 4U) { lpb = 32U; }
    const crd::u32 blocks = (h + lpb - 1U) / lpb;
    crd::u8 word_counts[8]; for (crd::u32 i = 0; i < nchan; ++i) { word_counts[i] = static_cast<crd::u8>(bpp[i] / 2U); } // PIZ words/sample
    if (end - q < static_cast<crd::isize>(static_cast<crd::usize>(blocks) * 8U)) { return HdrError::Truncated; }
    const crd::u8* ot = q;
    Array<crd::u8> tmp(a); Array<crd::u8> raw(a);

    for (crd::u32 b = 0; b < blocks; ++b)
    {
        const crd::u64 off = get_u64(ot + static_cast<crd::usize>(b) * 8U);
        if (off + 8U > bytes.size()) { return HdrError::BadData; }
        const crd::u8* blk = p + off;
        const crd::i32 y   = get_i32(blk);
        const crd::i32 dsz = get_i32(blk + 4);
        if (dsz < 0 || blk + 8 + dsz > end) { return HdrError::BadData; }
        const crd::u8* data   = blk + 8;
        const crd::u32 y0     = static_cast<crd::u32>(y - y_min);
        if (y0 >= h) { return HdrError::BadData; }
        const crd::u32   nlines      = (y0 + lpb <= h) ? lpb : (h - y0);
        const crd::usize block_bytes = scan_bytes * nlines;

        const crd::u8* blockptr = nullptr;
        if (comp == 0U || static_cast<crd::usize>(dsz) == block_bytes) // NONE, or stored-raw (compression didn't help)
        {
            if (static_cast<crd::usize>(dsz) != block_bytes) { return HdrError::BadData; }
            blockptr = data;
        }
        else if (comp == 1U) // RLE
        {
            tmp.resize(block_bytes);
            if (!exr_rle_decompress(data, static_cast<crd::usize>(dsz), tmp.data(), block_bytes)) { return HdrError::BadData; }
            raw.resize(block_bytes); exr_unpredict_reorder(tmp.data(), block_bytes, raw.data());
            blockptr = raw.data();
        }
        else if (comp == 4U) // PIZ — our own wavelet + Huffman
        {
            raw.resize(block_bytes);
            if (!piz_detail::piz_uncompress(data, static_cast<crd::usize>(dsz), w, nlines, word_counts, nchan, raw.data(), block_bytes, a)) { return HdrError::BadData; }
            blockptr = raw.data();
        }
        else // ZIPS (2) / ZIP (3) — our own DEFLATE
        {
            tmp.resize(0);
            if (!zlib_inflate({data, static_cast<crd::usize>(dsz)}, tmp) || tmp.size() != block_bytes) { return HdrError::BadData; }
            raw.resize(block_bytes); exr_unpredict_reorder(tmp.data(), block_bytes, raw.data());
            blockptr = raw.data();
        }

        for (crd::u32 ly = 0; ly < nlines; ++ly)
        {
            const crd::u8* rowbytes = blockptr + static_cast<crd::usize>(ly) * scan_bytes;
            const crd::u32 dst_y    = y0 + ly;
            for (crd::u32 i = 0; i < nchan; ++i)
            {
                if (slot[i] < 0) { continue; }
                const crd::u8* pl = rowbytes + plane_off[i];
                for (crd::u32 x = 0; x < w; ++x)
                {
                    crd::f32 v = 0.0F;
                    if (chans[i].type == 1) { v = half_to_float(get_u16(pl + static_cast<crd::usize>(x) * 2U)); }
                    else { const crd::u32 bits = get_u32(pl + static_cast<crd::usize>(x) * 4U); std::memcpy(&v, &bits, 4); }
                    out.at(x, dst_y, static_cast<crd::u32>(slot[i])) = v;
                }
            }
        }
    }
    return HdrError::Ok;
}

} // namespace crd::resources
