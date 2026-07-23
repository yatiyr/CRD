// jpeg_image.cpp — the owned baseline JPEG decoder. See jpeg_image.hpp for the coverage contract.

#include <crd/resources/jpeg_image.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>

#include <cmath>
#include <cstring>

namespace crd::resources
{
namespace
{

constexpr crd::u32 kMaxDim = 16384;

// zigzag order: coefficient index → natural 8×8 position
constexpr crd::u8 kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ── Huffman table (canonical, from DHT's BITS/HUFFVAL) ─────────────────────────────────────────────────────────────────

struct Huff
{
    crd::u16 mincode[17] = {}; // per bit-length: the smallest code
    crd::i32 valptr[17]  = {}; // per bit-length: index of the first value
    crd::u16 maxcode[17] = {}; // per bit-length: the largest code (0xFFFF = none at this length)
    crd::u8  values[256] = {};
    bool     present     = false;

    void build(const crd::u8* bits, const crd::u8* vals, crd::u32 nvals) noexcept
    {
        crd::u16 code = 0;
        crd::u32 k    = 0;
        for (int l = 1; l <= 16; ++l)
        {
            valptr[l]  = static_cast<crd::i32>(k);
            mincode[l] = code;
            code       = static_cast<crd::u16>(code + bits[l - 1]);
            maxcode[l] = bits[l - 1] != 0U ? static_cast<crd::u16>(code - 1U) : crd::u16{0xFFFF};
            code       = static_cast<crd::u16>(code << 1U);
            k += bits[l - 1];
        }
        for (crd::u32 i = 0; i < nvals && i < 256U; ++i) { values[i] = vals[i]; }
        present = true;
    }
};

// ── the entropy-coded bit reader (0xFF00 stuffing; stops at markers) ───────────────────────────────────────────────────

struct BitReader
{
    const crd::u8* p;
    const crd::u8* end;
    crd::u32       acc  = 0;
    int            bits = 0;

    // pull one bit; -1 on a marker/end (the caller treats it as truncation)
    [[nodiscard]] int bit() noexcept
    {
        if (bits == 0)
        {
            if (p >= end) { return -1; }
            crd::u8 b = *p++;
            if (b == 0xFFU)
            {
                if (p >= end) { return -1; }
                const crd::u8 next = *p++;
                if (next != 0x00U) // a real marker inside entropy data: RSTn handled by the caller; else stop
                {
                    p -= 2;
                    return -1;
                }
            }
            acc  = b;
            bits = 8;
        }
        --bits;
        return static_cast<int>((acc >> static_cast<crd::u32>(bits)) & 1U);
    }

    // decode one Huffman symbol (T.81 F.2.2.3); -1 on failure
    [[nodiscard]] int decode(const Huff& h) noexcept
    {
        crd::u32 code = 0;
        for (int l = 1; l <= 16; ++l)
        {
            const int b = bit();
            if (b < 0) { return -1; }
            code = (code << 1U) | static_cast<crd::u32>(b);
            if (h.maxcode[l] != 0xFFFFU && code <= h.maxcode[l])
            {
                return h.values[h.valptr[l] + static_cast<crd::i32>(code - h.mincode[l])];
            }
        }
        return -1;
    }

    // receive-and-extend: an s-bit magnitude, sign-extended per T.81 F.2.2.1
    [[nodiscard]] bool receive_extend(int s, int& out) noexcept
    {
        int v = 0;
        for (int i = 0; i < s; ++i)
        {
            const int b = bit();
            if (b < 0) { return false; }
            v = (v << 1) | b;
        }
        if (s > 0 && v < (1 << (s - 1))) { v += 1 - (1 << s); } // negative branch
        out = v;
        return true;
    }

    void reset_at(const crd::u8* np) noexcept
    {
        p    = np;
        acc  = 0;
        bits = 0;
    }
};

// ── the exact separable float IDCT (deterministic; clarity over speed — decode-time only) ──────────────────────────────

struct IdctTable
{
    crd::f32 c[8][8]; // c[u][x] = C(u)/2 · cos((2x+1)uπ/16)

    IdctTable() noexcept
    {
        constexpr crd::f64 pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u)
        {
            const crd::f64 cu = u == 0 ? 0.35355339059327373 : 0.5; // 1/(2√2) : 1/2
            for (int x = 0; x < 8; ++x)
            {
                c[u][x] = static_cast<crd::f32>(cu * crd::math::cos((2.0 * x + 1.0) * u * pi / 16.0));
            }
        }
    }
};

void idct8x8(const crd::i32* coef, crd::u8* out /*64*/) noexcept
{
    static const IdctTable kT;
    crd::f32               tmp[64];
    for (int y = 0; y < 8; ++y) // rows: tmp[y][x] = Σu c[u][x]·coef[y][u]
    {
        for (int x = 0; x < 8; ++x)
        {
            crd::f32 s = 0.0F;
            for (int u = 0; u < 8; ++u) { s += kT.c[u][x] * static_cast<crd::f32>(coef[y * 8 + u]); }
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; ++x) // columns + level shift + clamp
    {
        for (int y = 0; y < 8; ++y)
        {
            crd::f32 s = 0.0F;
            for (int v = 0; v < 8; ++v) { s += kT.c[v][y] * tmp[v * 8 + x]; }
            const int r = static_cast<int>(std::lround(s)) + 128;
            int       v = r;
            if (v < 0) { v = 0; }
            if (v > 255) { v = 255; }
            out[y * 8 + x] = static_cast<crd::u8>(v);
        }
    }
}

struct Component
{
    crd::u8  id      = 0;
    crd::u8  h       = 1;
    crd::u8  v       = 1;
    crd::u8  quant   = 0;
    crd::u8  dc_tbl  = 0;
    crd::u8  ac_tbl  = 0;
    int      dc_pred = 0;
    crd::u32 bw      = 0; // plane width/height in pixels (component resolution)
    crd::u32 bh      = 0;
};

} // namespace

bool jpeg_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    return bytes.size() >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U && bytes[2] == 0xFFU;
}

// NOLINTBEGIN(readability-function-size) — the marker loop + MCU scan is one coherent decode pass; splitting it scatters
// the shared state (tables, components, reader) across parameter lists without making any piece independently reusable.
LdrError jpeg_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a)
{
    out.width  = 0;
    out.height = 0;
    out.pixels.clear();
    if (!jpeg_sniff(bytes)) { return LdrError::BadMagic; }

    crd::u16  qt[4][64] = {};
    Huff      hdc[4];
    Huff      hac[4];
    Component comp[3];
    crd::u32  ncomp            = 0;
    crd::u32  w                = 0;
    crd::u32  h                = 0;
    crd::u32  restart_interval = 0;
    const crd::u8* p           = bytes.data() + 2;
    const crd::u8* end         = bytes.data() + bytes.size();
    const crd::u8* scan_start  = nullptr;

    // ── marker segments up to SOS ──────────────────────────────────────────────────────────────────────────────────────
    while (p + 4 <= end)
    {
        if (p[0] != 0xFFU) { return LdrError::BadData; }
        const crd::u8 marker = p[1];
        p += 2;
        if (marker == 0xD8U) { continue; }                                       // stray SOI
        if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) { continue; } // standalone
        if (p + 2 > end) { return LdrError::Truncated; }
        const crd::u32 seg_len = (static_cast<crd::u32>(p[0]) << 8U) | p[1];
        if (seg_len < 2U || p + seg_len > end) { return LdrError::Truncated; }
        const crd::u8* seg = p + 2;
        const crd::u32 n   = seg_len - 2U;

        if (marker == 0xDBU) // DQT
        {
            crd::u32 off = 0;
            while (off < n)
            {
                const crd::u8 pq = seg[off] >> 4U;
                const crd::u8 tq = seg[off] & 15U;
                if (tq > 3U || pq > 1U) { return LdrError::BadHeader; }
                ++off;
                const crd::u32 need = pq == 1U ? 128U : 64U;
                if (off + need > n) { return LdrError::Truncated; }
                for (int i = 0; i < 64; ++i)
                {
                    qt[tq][i] = pq == 1U ? static_cast<crd::u16>((seg[off + 2U * i] << 8U) | seg[off + 2U * i + 1U])
                                         : seg[off + i];
                }
                off += need;
            }
        }
        else if (marker == 0xC4U) // DHT
        {
            crd::u32 off = 0;
            while (off + 17U <= n)
            {
                const crd::u8 tc = seg[off] >> 4U;
                const crd::u8 th = seg[off] & 15U;
                if (tc > 1U || th > 3U) { return LdrError::BadHeader; }
                crd::u32 nv = 0;
                for (int i = 0; i < 16; ++i) { nv += seg[off + 1U + i]; }
                if (off + 17U + nv > n || nv > 256U) { return LdrError::Truncated; }
                (tc == 0U ? hdc[th] : hac[th]).build(seg + off + 1U, seg + off + 17U, nv);
                off += 17U + nv;
            }
        }
        else if (marker == 0xC0U || marker == 0xC1U) // SOF0 baseline / SOF1 extended sequential (same decode path)
        {
            if (n < 6U) { return LdrError::Truncated; }
            if (seg[0] != 8U) { return LdrError::Unsupported; } // 12-bit precision
            h     = (static_cast<crd::u32>(seg[1]) << 8U) | seg[2];
            w     = (static_cast<crd::u32>(seg[3]) << 8U) | seg[4];
            ncomp = seg[5];
            if (w == 0U || h == 0U) { return LdrError::BadHeader; }
            if (w > kMaxDim || h > kMaxDim) { return LdrError::TooLarge; }
            if (ncomp != 1U && ncomp != 3U) { return LdrError::Unsupported; } // CMYK (4) — named, not silent
            if (n < 6U + ncomp * 3U) { return LdrError::Truncated; }
            for (crd::u32 c = 0; c < ncomp; ++c)
            {
                comp[c].id    = seg[6U + c * 3U];
                comp[c].h     = seg[7U + c * 3U] >> 4U;
                comp[c].v     = seg[7U + c * 3U] & 15U;
                comp[c].quant = seg[8U + c * 3U];
                if (comp[c].h == 0U || comp[c].h > 4U || comp[c].v == 0U || comp[c].v > 4U || comp[c].quant > 3U)
                {
                    return LdrError::BadHeader;
                }
            }
        }
        else if (marker == 0xC2U || (marker >= 0xC5U && marker <= 0xCFU && marker != 0xC8U))
        {
            return LdrError::Unsupported; // progressive / arithmetic / hierarchical — BY NAME
        }
        else if (marker == 0xDDU) // DRI
        {
            if (n < 2U) { return LdrError::Truncated; }
            restart_interval = (static_cast<crd::u32>(seg[0]) << 8U) | seg[1];
        }
        else if (marker == 0xDAU) // SOS
        {
            if (w == 0U) { return LdrError::BadHeader; } // SOS before SOF
            if (n < 1U + ncomp * 2U + 3U) { return LdrError::Truncated; }
            if (seg[0] != ncomp) { return LdrError::Unsupported; } // non-interleaved multi-scan
            for (crd::u32 c = 0; c < ncomp; ++c)
            {
                const crd::u8 cid = seg[1U + c * 2U];
                bool          hit = false;
                for (crd::u32 k = 0; k < ncomp; ++k)
                {
                    if (comp[k].id == cid)
                    {
                        comp[k].dc_tbl = seg[2U + c * 2U] >> 4U;
                        comp[k].ac_tbl = seg[2U + c * 2U] & 15U;
                        hit            = true;
                    }
                }
                if (!hit) { return LdrError::BadHeader; }
            }
            scan_start = p + seg_len;
            break;
        }
        // APPn / COM / others: skip
        p += seg_len;
    }
    if (scan_start == nullptr) { return LdrError::Truncated; }

    // ── the interleaved MCU scan ───────────────────────────────────────────────────────────────────────────────────────
    crd::u32 hmax = 1;
    crd::u32 vmax = 1;
    for (crd::u32 c = 0; c < ncomp; ++c)
    {
        hmax = comp[c].h > hmax ? comp[c].h : hmax;
        vmax = comp[c].v > vmax ? comp[c].v : vmax;
    }
    const crd::u32 mcux = (w + hmax * 8U - 1U) / (hmax * 8U);
    const crd::u32 mcuy = (h + vmax * 8U - 1U) / (vmax * 8U);

    // per-component pixel planes at component resolution
    crd::containers::Array<crd::u8> planes[3] = {crd::containers::Array<crd::u8>(a), crd::containers::Array<crd::u8>(a),
                                                 crd::containers::Array<crd::u8>(a)};
    for (crd::u32 c = 0; c < ncomp; ++c)
    {
        comp[c].bw = mcux * comp[c].h * 8U;
        comp[c].bh = mcuy * comp[c].v * 8U;
        planes[c].resize(static_cast<crd::usize>(comp[c].bw) * comp[c].bh, 0);
    }

    BitReader br{scan_start, end};
    crd::u32  mcu_count = 0;
    for (crd::u32 my = 0; my < mcuy; ++my)
    {
        for (crd::u32 mx = 0; mx < mcux; ++mx)
        {
            if (restart_interval != 0U && mcu_count != 0U && (mcu_count % restart_interval) == 0U)
            {
                // expect an RSTn marker: realign to the byte boundary and consume it
                const crd::u8* q = br.p;
                while (q + 1 < end && !(q[0] == 0xFFU && q[1] >= 0xD0U && q[1] <= 0xD7U)) { ++q; }
                if (q + 1 >= end) { return LdrError::Truncated; }
                br.reset_at(q + 2);
                for (crd::u32 c = 0; c < ncomp; ++c) { comp[c].dc_pred = 0; }
            }
            ++mcu_count;
            for (crd::u32 c = 0; c < ncomp; ++c)
            {
                if (!hdc[comp[c].dc_tbl].present || !hac[comp[c].ac_tbl].present || qt[comp[c].quant][0] == 0U)
                {
                    return LdrError::BadHeader;
                }
                for (crd::u32 by = 0; by < comp[c].v; ++by)
                {
                    for (crd::u32 bx = 0; bx < comp[c].h; ++bx)
                    {
                        crd::i32 coef[64] = {};
                        const int t       = br.decode(hdc[comp[c].dc_tbl]); // DC: category + diff
                        if (t < 0) { return LdrError::Truncated; }
                        int diff = 0;
                        if (t > 0 && !br.receive_extend(t, diff)) { return LdrError::Truncated; }
                        comp[c].dc_pred += diff;
                        coef[0] = comp[c].dc_pred * qt[comp[c].quant][0];
                        for (int k = 1; k < 64;) // AC: run/size symbols
                        {
                            const int rs = br.decode(hac[comp[c].ac_tbl]);
                            if (rs < 0) { return LdrError::Truncated; }
                            const int r = rs >> 4;
                            const int s = rs & 15;
                            if (s == 0)
                            {
                                if (r == 15)
                                {
                                    k += 16; // ZRL
                                    continue;
                                }
                                break; // EOB
                            }
                            k += r;
                            if (k > 63) { return LdrError::BadData; }
                            int v = 0;
                            if (!br.receive_extend(s, v)) { return LdrError::Truncated; }
                            coef[kZigzag[k]] = v * qt[comp[c].quant][k];
                            ++k;
                        }
                        crd::u8 block[64];
                        idct8x8(coef, block);
                        // place into the component plane
                        const crd::u32 px0 = (mx * comp[c].h + bx) * 8U;
                        const crd::u32 py0 = (my * comp[c].v + by) * 8U;
                        for (int yy = 0; yy < 8; ++yy)
                        {
                            std::memcpy(planes[c].data() + static_cast<crd::usize>(py0 + static_cast<crd::u32>(yy)) * comp[c].bw + px0,
                                        block + yy * 8, 8);
                        }
                    }
                }
            }
        }
    }

    // ── upsample + color convert → RGBA8 ───────────────────────────────────────────────────────────────────────────────
    out.width            = w;
    out.height           = h;
    out.source_channels  = static_cast<crd::u8>(ncomp);
    out.source_bit_depth = 8;
    out.pixels.resize(static_cast<crd::usize>(w) * h * 4U, 0);
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x)
        {
            crd::u8* d = out.pixels.data() + (static_cast<crd::usize>(y) * w + x) * 4U;
            if (ncomp == 1U)
            {
                const crd::u8 g = planes[0][static_cast<crd::usize>(y) * comp[0].bw + x];
                d[0]            = g;
                d[1]            = g;
                d[2]            = g;
                d[3]            = 255;
                continue;
            }
            // per-component sample replication from the component-resolution planes
            const auto sample = [&](crd::u32 c) noexcept -> crd::f32 {
                const crd::u32 sx = x * comp[c].h / hmax;
                const crd::u32 sy = y * comp[c].v / vmax;
                return static_cast<crd::f32>(planes[c][static_cast<crd::usize>(sy) * comp[c].bw + sx]);
            };
            const crd::f32 yy = sample(0);
            const crd::f32 cb = sample(1) - 128.0F;
            const crd::f32 cr = sample(2) - 128.0F;
            const crd::f32 rf = yy + 1.402F * cr; // JFIF full-range BT.601
            const crd::f32 gf = yy - 0.344136F * cb - 0.714136F * cr;
            const crd::f32 bf = yy + 1.772F * cb;
            const auto     c8 = [](crd::f32 v) noexcept {
                int r = static_cast<int>(std::lround(v));
                if (r < 0) { r = 0; }
                if (r > 255) { r = 255; }
                return static_cast<crd::u8>(r);
            };
            d[0] = c8(rf);
            d[1] = c8(gf);
            d[2] = c8(bf);
            d[3] = 255;
        }
    }
    return LdrError::Ok;
}
// NOLINTEND(readability-function-size)

} // namespace crd::resources
