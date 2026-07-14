#include "hdr_piz.hpp"

#include <crd/core/types.hpp>

#include <algorithm> // make_heap/push_heap/pop_heap (algorithms, not containers)
#include <cstring>

// hdr_piz.cpp — our own OpenEXR PIZ codec: the 2-D wavelet (ImfWav), the canonical-Huffman+RLE codec (ImfHuf), the
// value bitmap/LUT, and the block glue (ImfPizCompressor). Algorithms per the published OpenEXR format; code is ours.

namespace crd::resources::piz_detail
{

using crd::containers::Array;

namespace
{

// ── little-endian scalar I/O ─────────────────────────────────────────────────────────────────────────────────────────────
[[nodiscard]] crd::u16 ld16(const crd::u8* p) noexcept { return static_cast<crd::u16>(static_cast<crd::u32>(p[0]) | (static_cast<crd::u32>(p[1]) << 8U)); }
[[nodiscard]] crd::u32 ld32(const crd::u8* p) noexcept { return static_cast<crd::u32>(p[0]) | (static_cast<crd::u32>(p[1]) << 8U) | (static_cast<crd::u32>(p[2]) << 16U) | (static_cast<crd::u32>(p[3]) << 24U); }
void put16(Array<crd::u8>& o, crd::u16 v) { o.push_back(static_cast<crd::u8>(v & 0xFFU)); o.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU)); }
void put32(Array<crd::u8>& o, crd::u32 v) { for (int i = 0; i < 4; ++i) { o.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU)); } }

// ── value bitmap + LUT (ImfPizCompressor) ────────────────────────────────────────────────────────────────────────────────
constexpr int kUShortRange = 1 << 16;
constexpr int kBitmapSize  = kUShortRange >> 3; // 8192

void bitmap_from_data(const crd::u16* data, crd::usize n, crd::u8* bitmap, crd::u16& min_nz, crd::u16& max_nz)
{
    for (int i = 0; i < kBitmapSize; ++i) { bitmap[i] = 0; }
    for (crd::usize i = 0; i < n; ++i) { bitmap[data[i] >> 3U] |= static_cast<crd::u8>(1U << (data[i] & 7U)); }
    bitmap[0] &= static_cast<crd::u8>(~1U); // zero is implicit
    min_nz = static_cast<crd::u16>(kBitmapSize - 1);
    max_nz = 0;
    for (int i = 0; i < kBitmapSize; ++i)
    {
        if (bitmap[i] != 0U) { if (min_nz > i) { min_nz = static_cast<crd::u16>(i); } if (max_nz < i) { max_nz = static_cast<crd::u16>(i); } }
    }
}
[[nodiscard]] crd::u16 forward_lut(const crd::u8* bitmap, crd::u16* lut)
{
    int k = 0;
    for (int i = 0; i < kUShortRange; ++i)
    {
        if (i == 0 || (bitmap[i >> 3] & (1U << (i & 7))) != 0U) { lut[i] = static_cast<crd::u16>(k++); }
        else { lut[i] = 0; }
    }
    return static_cast<crd::u16>(k - 1);
}
[[nodiscard]] crd::u16 reverse_lut(const crd::u8* bitmap, crd::u16* lut)
{
    int k = 0;
    for (int i = 0; i < kUShortRange; ++i) { if (i == 0 || (bitmap[i >> 3] & (1U << (i & 7))) != 0U) { lut[k++] = static_cast<crd::u16>(i); } }
    const int n = k - 1;
    while (k < kUShortRange) { lut[k++] = 0; }
    return static_cast<crd::u16>(n);
}
void apply_lut(const crd::u16* lut, crd::u16* data, crd::usize n) { for (crd::usize i = 0; i < n; ++i) { data[i] = lut[data[i]]; } }

// ── 2-D wavelet (ImfWav) ─────────────────────────────────────────────────────────────────────────────────────────────────
constexpr int kNbits = 16;
constexpr int kAOffset = 1 << (kNbits - 1);
constexpr int kMOffset = 1 << (kNbits - 1);
constexpr int kModMask = (1 << kNbits) - 1;

inline void wenc14(crd::u16 a, crd::u16 b, crd::u16& l, crd::u16& h) { const short as = static_cast<short>(a); const short bs = static_cast<short>(b); const short ms = static_cast<short>((as + bs) >> 1); const short ds = static_cast<short>(as - bs); l = static_cast<crd::u16>(ms); h = static_cast<crd::u16>(ds); }
inline void wdec14(crd::u16 l, crd::u16 h, crd::u16& a, crd::u16& b) { const short ls = static_cast<short>(l); const short hs = static_cast<short>(h); const int hi = hs; const int ai = ls + (hi & 1) + (hi >> 1); const short as = static_cast<short>(ai); const short bs = static_cast<short>(ai - hi); a = static_cast<crd::u16>(as); b = static_cast<crd::u16>(bs); }
inline void wenc16(crd::u16 a, crd::u16 b, crd::u16& l, crd::u16& h) { const int ao = (a + kAOffset) & kModMask; int m = (ao + b) >> 1; int d = ao - b; if (d < 0) { m = (m + kMOffset) & kModMask; } d &= kModMask; l = static_cast<crd::u16>(m); h = static_cast<crd::u16>(d); }
inline void wdec16(crd::u16 l, crd::u16 h, crd::u16& a, crd::u16& b) { const int m = l; const int d = h; const int bb = (m - (d >> 1)) & kModMask; const int aa = (d + bb - kAOffset) & kModMask; b = static_cast<crd::u16>(bb); a = static_cast<crd::u16>(aa); }

void wav2_encode(crd::u16* in, int nx, int ox, int ny, int oy, crd::u16 mx)
{
    const bool w14 = mx < (1 << 14);
    const int  n   = (nx > ny) ? ny : nx;
    int        p   = 1;
    int        p2  = 2;
    while (p2 <= n)
    {
        crd::u16* py  = in;
        crd::u16* ey  = in + oy * (ny - p2);
        const int oy1 = oy * p; const int oy2 = oy * p2; const int ox1 = ox * p; const int ox2 = ox * p2;
        crd::u16  i00 = 0; crd::u16 i01 = 0; crd::u16 i10 = 0; crd::u16 i11 = 0;
        for (; py <= ey; py += oy2)
        {
            crd::u16* px = py;
            crd::u16* ex = py + ox * (nx - p2);
            for (; px <= ex; px += ox2)
            {
                crd::u16* p01 = px + ox1; crd::u16* p10 = px + oy1; crd::u16* p11 = p10 + ox1;
                if (w14) { wenc14(*px, *p01, i00, i01); wenc14(*p10, *p11, i10, i11); wenc14(i00, i10, *px, *p10); wenc14(i01, i11, *p01, *p11); }
                else     { wenc16(*px, *p01, i00, i01); wenc16(*p10, *p11, i10, i11); wenc16(i00, i10, *px, *p10); wenc16(i01, i11, *p01, *p11); }
            }
            if ((nx & p) != 0) { crd::u16* p10 = px + oy1; if (w14) { wenc14(*px, *p10, i00, *p10); } else { wenc16(*px, *p10, i00, *p10); } *px = i00; }
        }
        if ((ny & p) != 0)
        {
            crd::u16* px = py; crd::u16* ex = py + ox * (nx - p2);
            for (; px <= ex; px += ox2) { crd::u16* p01 = px + ox1; if (w14) { wenc14(*px, *p01, i00, *p01); } else { wenc16(*px, *p01, i00, *p01); } *px = i00; }
        }
        p = p2; p2 <<= 1;
    }
}
void wav2_decode(crd::u16* in, int nx, int ox, int ny, int oy, crd::u16 mx)
{
    const bool w14 = mx < (1 << 14);
    const int  n   = (nx > ny) ? ny : nx;
    int        p   = 1;
    while (p <= n) { p <<= 1; }
    p >>= 1;
    int p2 = p;
    p >>= 1;
    while (p >= 1)
    {
        crd::u16* py  = in;
        crd::u16* ey  = in + oy * (ny - p2);
        const int oy1 = oy * p; const int oy2 = oy * p2; const int ox1 = ox * p; const int ox2 = ox * p2;
        crd::u16  i00 = 0; crd::u16 i01 = 0; crd::u16 i10 = 0; crd::u16 i11 = 0;
        for (; py <= ey; py += oy2)
        {
            crd::u16* px = py;
            crd::u16* ex = py + ox * (nx - p2);
            for (; px <= ex; px += ox2)
            {
                crd::u16* p01 = px + ox1; crd::u16* p10 = px + oy1; crd::u16* p11 = p10 + ox1;
                if (w14) { wdec14(*px, *p10, i00, i10); wdec14(*p01, *p11, i01, i11); wdec14(i00, i01, *px, *p01); wdec14(i10, i11, *p10, *p11); }
                else     { wdec16(*px, *p10, i00, i10); wdec16(*p01, *p11, i01, i11); wdec16(i00, i01, *px, *p01); wdec16(i10, i11, *p10, *p11); }
            }
            if ((nx & p) != 0) { crd::u16* p10 = px + oy1; if (w14) { wdec14(*px, *p10, i00, *p10); } else { wdec16(*px, *p10, i00, *p10); } *px = i00; }
        }
        if ((ny & p) != 0)
        {
            crd::u16* px = py; crd::u16* ex = py + ox * (nx - p2);
            for (; px <= ex; px += ox2) { crd::u16* p01 = px + ox1; if (w14) { wdec14(*px, *p01, i00, *p01); } else { wdec16(*px, *p01, i00, *p01); } *px = i00; }
        }
        p2 = p; p >>= 1;
    }
}

// ── canonical-Huffman codec (ImfHuf) ─────────────────────────────────────────────────────────────────────────────────────
constexpr int kHufEncBits = 16;
constexpr int kHufDecBits = 14;
constexpr int kHufEncSize = (1 << kHufEncBits) + 1; // 65537
constexpr int kHufDecSize = 1 << kHufDecBits;       // 16384
constexpr int kHufDecMask = kHufDecSize - 1;
constexpr int kShortZeroRun = 59;
constexpr int kLongZeroRun  = 63;
constexpr int kShortestLong = 2 + kLongZeroRun - kShortZeroRun; // 6
constexpr int kLongestLong  = 255 + kShortestLong;              // 261

inline crd::u64 huf_len(crd::u64 code) noexcept { return code & 63U; }
inline crd::u64 huf_code(crd::u64 code) noexcept { return code >> 6U; }

struct HufDec { int len; int lit; int* p; };

void huf_canonical(crd::u64* hcode)
{
    crd::u64 n[59]; for (int i = 0; i <= 58; ++i) { n[i] = 0; }
    for (int i = 0; i < kHufEncSize; ++i) { n[hcode[i]] += 1; }
    crd::u64 c = 0;
    for (int i = 58; i > 0; --i) { const crd::u64 nc = ((c + n[i]) >> 1); n[i] = c; c = nc; }
    for (int i = 0; i < kHufEncSize; ++i) { const int l = static_cast<int>(hcode[i]); if (l > 0) { hcode[i] = static_cast<crd::u64>(l) | (n[l]++ << 6U); } }
}

// --- decode ---
struct BitIn { crd::u64 c = 0; int lc = 0; const crd::u8* in; const crd::u8* end; };
inline void get_char(BitIn& b) { const crd::u8 v = b.in < b.end ? *b.in : 0U; ++b.in; b.c = (b.c << 8U) | v; b.lc += 8; }
inline crd::u64 get_bits(int nbits, BitIn& b) { while (b.lc < nbits) { get_char(b); } b.lc -= nbits; return (b.c >> b.lc) & ((1ULL << nbits) - 1ULL); }

[[nodiscard]] bool huf_unpack_enc_table(BitIn& b, int im, int imx, crd::u64* hcode)
{
    for (; im <= imx; ++im)
    {
        const crd::u64 l = hcode[im] = get_bits(6, b);
        if (l == static_cast<crd::u64>(kLongZeroRun))
        {
            int zerun = static_cast<int>(get_bits(8, b)) + kShortestLong;
            if (im + zerun > imx + 1) { return false; }
            while (zerun-- > 0) { hcode[im++] = 0; }
            --im;
        }
        else if (l >= static_cast<crd::u64>(kShortZeroRun))
        {
            int zerun = static_cast<int>(l) - kShortZeroRun + 2;
            if (im + zerun > imx + 1) { return false; }
            while (zerun-- > 0) { hcode[im++] = 0; }
            --im;
        }
    }
    huf_canonical(hcode);
    return true;
}
[[nodiscard]] bool huf_build_dec_table(const crd::u64* hcode, int im, int imx, HufDec* hdec, Array<int>& pool, Array<int>& long_count, Array<int>& off)
{
    for (int i = 0; i < kHufDecSize; ++i) { hdec[i].len = 0; hdec[i].lit = 0; hdec[i].p = nullptr; }
    long_count.resize(kHufDecSize); for (int i = 0; i < kHufDecSize; ++i) { long_count[i] = 0; }
    for (int i = im; i <= imx; ++i)
    {
        const crd::u64 c = huf_code(hcode[i]); const int l = static_cast<int>(huf_len(hcode[i]));
        if (l == 0) { continue; }
        if ((c >> l) != 0U) { return false; }
        if (l > kHufDecBits) { ++long_count[static_cast<int>(c >> (l - kHufDecBits))]; }
        else
        {
            HufDec* pl = hdec + (c << (kHufDecBits - l));
            for (crd::u64 k = (1ULL << (kHufDecBits - l)); k > 0U; --k, ++pl) { if (pl->len != 0 || pl->p != nullptr) { return false; } pl->len = l; pl->lit = i; }
        }
    }
    crd::usize total = 0; for (int i = 0; i < kHufDecSize; ++i) { total += static_cast<crd::usize>(long_count[i]); }
    pool.resize(total > 0 ? total : 1);
    off.resize(kHufDecSize);
    crd::usize acc = 0;
    for (int i = 0; i < kHufDecSize; ++i) { off[i] = static_cast<int>(acc); if (long_count[i] > 0) { hdec[i].p = pool.data() + acc; hdec[i].lit = 0; hdec[i].len = 0; acc += static_cast<crd::usize>(long_count[i]); } }
    for (int i = im; i <= imx; ++i)
    {
        const int l = static_cast<int>(huf_len(hcode[i]));
        if (l <= kHufDecBits) { continue; }
        const int pfx = static_cast<int>(huf_code(hcode[i]) >> (l - kHufDecBits));
        hdec[pfx].p[hdec[pfx].lit] = i;
        ++hdec[pfx].lit;
    }
    return true;
}
[[nodiscard]] bool get_code(int po, int rlc, BitIn& b, crd::u16*& out, const crd::u16* ob, const crd::u16* oe)
{
    if (po == rlc)
    {
        if (b.lc < 8) { get_char(b); }
        b.lc -= 8;
        crd::u8 cs = static_cast<crd::u8>(b.c >> b.lc);
        if (out + cs > oe) { return false; }
        if (out - 1 < ob) { return false; }
        const crd::u16 s = out[-1];
        while (cs-- > 0U) { *out++ = s; }
    }
    else if (out < oe) { *out++ = static_cast<crd::u16>(po); }
    else { return false; }
    return true;
}
[[nodiscard]] bool huf_decode(const crd::u64* hcode, const HufDec* hdec, const crd::u8* in, int ni, int rlc, int no, crd::u16* out)
{
    BitIn b; b.in = in; b.end = in + (static_cast<crd::usize>(ni) + 7U) / 8U;
    const crd::u16* oe = out + no;
    const crd::u16* ob = out;
    while (b.in < b.end)
    {
        get_char(b);
        while (b.lc >= kHufDecBits)
        {
            const HufDec pl = hdec[(b.c >> (b.lc - kHufDecBits)) & kHufDecMask];
            if (pl.len != 0)
            {
                b.lc -= pl.len;
                if (b.lc < 0) { return false; }
                if (!get_code(pl.lit, rlc, b, out, ob, oe)) { return false; }
            }
            else
            {
                if (pl.p == nullptr) { return false; }
                int j = 0;
                for (; j < pl.lit; ++j)
                {
                    const int l = static_cast<int>(huf_len(hcode[pl.p[j]]));
                    while (b.lc < l && b.in < b.end) { get_char(b); }
                    if (b.lc >= l && huf_code(hcode[pl.p[j]]) == ((b.c >> (b.lc - l)) & ((1ULL << l) - 1ULL)))
                    {
                        b.lc -= l;
                        if (!get_code(pl.p[j], rlc, b, out, ob, oe)) { return false; }
                        break;
                    }
                }
                if (j == pl.lit) { return false; }
            }
        }
    }
    const int i = (8 - ni) & 7;
    b.c >>= static_cast<crd::u32>(i);
    b.lc -= i;
    while (b.lc > 0)
    {
        const HufDec pl = hdec[(b.c << (kHufDecBits - b.lc)) & kHufDecMask];
        if (pl.len != 0) { b.lc -= pl.len; if (b.lc < 0) { return false; } if (!get_code(pl.lit, rlc, b, out, ob, oe)) { return false; } }
        else { return false; }
    }
    return (out - ob) == no;
}
[[nodiscard]] bool huf_uncompress(const crd::u8* compressed, int ncompressed, crd::u16* raw, int nraw, crd::memory::IAllocator* a)
{
    if (ncompressed < 20) { return nraw == 0; }
    const int im    = static_cast<int>(ld32(compressed));
    const int imx    = static_cast<int>(ld32(compressed + 4));
    const int nbits = static_cast<int>(ld32(compressed + 12));
    if (im < 0 || im >= kHufEncSize || imx < 0 || imx >= kHufEncSize) { return false; }
    const crd::u8* ptr    = compressed + 20;
    const crd::u8* ce     = compressed + ncompressed;
    const crd::u64 nbytes = (static_cast<crd::u64>(nbits) + 7U) / 8U;
    if (ptr + nbytes > ce) { return false; }

    Array<crd::u64> freq(a); freq.resize(kHufEncSize); for (int i = 0; i < kHufEncSize; ++i) { freq[i] = 0; }
    Array<HufDec>   hdec(a); hdec.resize(kHufDecSize);
    Array<int>      pool(a); Array<int> long_count(a); Array<int> off(a);

    BitIn b; b.in = ptr; b.end = ce; b.c = 0; b.lc = 0;
    if (!huf_unpack_enc_table(b, im, imx, freq.data())) { return false; }
    const crd::u8* data = b.in; // table consumed up to here
    if (nbits > 8 * static_cast<int>(ce - data)) { return false; }
    if (!huf_build_dec_table(freq.data(), im, imx, hdec.data(), pool, long_count, off)) { return false; }
    return huf_decode(freq.data(), hdec.data(), data, nbits, imx, nraw, raw);
}

// --- encode ---
struct BitOut { crd::u64 c = 0; int lc = 0; crd::u8* out; };
inline void out_bits(int nbits, crd::u64 bits, BitOut& b) { b.c <<= static_cast<crd::u32>(nbits); b.lc += nbits; b.c |= bits; while (b.lc >= 8) { *b.out++ = static_cast<crd::u8>(b.c >> (b.lc -= 8)); } }
inline void out_code(crd::u64 code, BitOut& b) { out_bits(static_cast<int>(huf_len(code)), huf_code(code), b); }
inline void send_code(crd::u64 scode, int run, crd::u64 rcode, BitOut& b)
{
    if (huf_len(scode) + huf_len(rcode) + 8 < huf_len(scode) * static_cast<crd::u64>(run)) { out_code(scode, b); out_code(rcode, b); out_bits(8, static_cast<crd::u64>(run), b); }
    else { while (run-- >= 0) { out_code(scode, b); } }
}
struct FHeapCmp { bool operator()(const crd::u64* x, const crd::u64* y) const noexcept { return *x > *y; } };
void huf_build_enc_table(crd::u64* frq, int* im, int* imx, Array<crd::u64*>& fheap, Array<int>& hlink, Array<crd::u64>& scode)
{
    fheap.resize(kHufEncSize); hlink.resize(kHufEncSize); scode.resize(kHufEncSize);
    *im = 0; while (frq[*im] == 0U) { ++(*im); }
    int nf = 0;
    for (int i = *im; i < kHufEncSize; ++i) { hlink[i] = i; if (frq[i] != 0U) { fheap[nf++] = &frq[i]; *imx = i; } }
    ++(*imx); frq[*imx] = 1; fheap[nf++] = &frq[*imx];
    std::make_heap(fheap.data(), fheap.data() + nf, FHeapCmp());
    for (int i = 0; i < kHufEncSize; ++i) { scode[i] = 0; }
    while (nf > 1)
    {
        const int mm = static_cast<int>(fheap[0] - frq);
        std::pop_heap(fheap.data(), fheap.data() + nf, FHeapCmp()); --nf;
        const int m = static_cast<int>(fheap[0] - frq);
        std::pop_heap(fheap.data(), fheap.data() + nf, FHeapCmp());
        frq[m] += frq[mm];
        std::push_heap(fheap.data(), fheap.data() + nf, FHeapCmp());
        for (int j = m;; j = hlink[j]) { ++scode[j]; if (hlink[j] == j) { hlink[j] = mm; break; } }
        for (int j = mm;; j = hlink[j]) { ++scode[j]; if (hlink[j] == j) { break; } }
    }
    huf_canonical(scode.data());
    for (int i = 0; i < kHufEncSize; ++i) { frq[i] = scode[i]; }
}
[[nodiscard]] crd::usize huf_pack_enc_table(const crd::u64* hcode, int im, int imx, crd::u8* p0)
{
    BitOut b; b.out = p0;
    for (; im <= imx; ++im)
    {
        const int l = static_cast<int>(huf_len(hcode[im]));
        if (l == 0)
        {
            int zerun = 1;
            while (im < imx && zerun < kLongestLong) { if (huf_len(hcode[im + 1]) > 0U) { break; } ++im; ++zerun; }
            if (zerun >= 2)
            {
                if (zerun >= kShortestLong) { out_bits(6, static_cast<crd::u64>(kLongZeroRun), b); out_bits(8, static_cast<crd::u64>(zerun - kShortestLong), b); }
                else { out_bits(6, static_cast<crd::u64>(kShortZeroRun + zerun - 2), b); }
                continue;
            }
        }
        out_bits(6, static_cast<crd::u64>(l), b);
    }
    if (b.lc > 0) { *b.out++ = static_cast<crd::u8>(b.c << (8 - b.lc)); }
    return static_cast<crd::usize>(b.out - p0);
}
[[nodiscard]] int huf_encode(const crd::u64* hcode, const crd::u16* in, int ni, int rlc, crd::u8* out)
{
    BitOut b; b.out = out;
    int s = in[0]; int cs = 0;
    for (int i = 1; i < ni; ++i) { if (s == in[i] && cs < 255) { ++cs; } else { send_code(hcode[s], cs, hcode[rlc], b); cs = 0; } s = in[i]; }
    send_code(hcode[s], cs, hcode[rlc], b);
    if (b.lc != 0) { *b.out = static_cast<crd::u8>((b.c << (8 - b.lc)) & 0xFFU); }
    return static_cast<int>(b.out - out) * 8 + b.lc;
}
[[nodiscard]] crd::usize huf_compress(const crd::u16* raw, int nraw, crd::u8* compressed, crd::memory::IAllocator* a)
{
    if (nraw == 0) { return 0; }
    Array<crd::u64> freq(a); freq.resize(kHufEncSize); for (int i = 0; i < kHufEncSize; ++i) { freq[i] = 0; }
    for (int i = 0; i < nraw; ++i) { ++freq[raw[i]]; }
    Array<crd::u64*> fheap(a); Array<int> hlink(a); Array<crd::u64> scode(a);
    int im = 0; int imx = 0;
    huf_build_enc_table(freq.data(), &im, &imx, fheap, hlink, scode);
    crd::u8*     table_start = compressed + 20;
    const crd::usize table_len = huf_pack_enc_table(freq.data(), im, imx, table_start);
    crd::u8*     data_start  = table_start + table_len;
    const int    nbits       = huf_encode(freq.data(), raw, nraw, imx, data_start);
    const crd::usize data_len = (static_cast<crd::usize>(nbits) + 7U) / 8U;
    // write the 20-byte header directly
    auto w32 = [&](crd::u8* p, crd::u32 v) { for (int i = 0; i < 4; ++i) { p[i] = static_cast<crd::u8>((v >> (8U * i)) & 0xFFU); } };
    w32(compressed + 0, static_cast<crd::u32>(im));
    w32(compressed + 4, static_cast<crd::u32>(imx));
    w32(compressed + 8, static_cast<crd::u32>(table_len));
    w32(compressed + 12, static_cast<crd::u32>(nbits));
    w32(compressed + 16, 0U);
    return static_cast<crd::usize>(data_start - compressed) + data_len;
}

} // namespace

// ── PIZ block glue ───────────────────────────────────────────────────────────────────────────────────────────────────────
bool piz_uncompress(const crd::u8* data, crd::usize dsz, crd::u32 width, crd::u32 nlines, const crd::u8* word_counts, crd::u32 nchan, crd::u8* out_raw, crd::usize out_raw_size, crd::memory::IAllocator* a)
{
    if (dsz < 4U) { return false; }
    const crd::u8* p   = data;
    const crd::u8* end = data + dsz;
    const crd::u16 min_nz = ld16(p); const crd::u16 max_nz = ld16(p + 2); p += 4;
    if (max_nz >= kBitmapSize) { return false; }
    Array<crd::u8> bitmap(a); bitmap.resize(kBitmapSize); for (int i = 0; i < kBitmapSize; ++i) { bitmap[i] = 0; }
    if (min_nz <= max_nz)
    {
        const crd::usize nb = static_cast<crd::usize>(max_nz) - min_nz + 1U;
        if (p + nb > end) { return false; }
        std::memcpy(bitmap.data() + min_nz, p, nb); p += nb;
    }
    Array<crd::u16> lut(a); lut.resize(kUShortRange);
    const crd::u16 max_value = reverse_lut(bitmap.data(), lut.data());
    if (p + 4 > end) { return false; }
    const crd::u32 length = ld32(p); p += 4;
    if (p + length > end) { return false; }

    crd::usize sum_wc = 0; for (crd::u32 c = 0; c < nchan; ++c) { sum_wc += word_counts[c]; }
    const crd::usize nwords = static_cast<crd::usize>(width) * nlines * sum_wc;
    if (out_raw_size < nwords * 2U) { return false; }
    Array<crd::u16> wav(a); wav.resize(nwords > 0 ? nwords : 1);
    if (!huf_uncompress(p, static_cast<int>(length), wav.data(), static_cast<int>(nwords), a)) { return false; }

    // OpenEXR order: wavelet-decode every channel → apply the reverse LUT over the WHOLE buffer → interleave to scanlines.
    const crd::usize scan_words = static_cast<crd::usize>(width) * sum_wc;
    crd::usize wav_base = 0;
    for (crd::u32 c = 0; c < nchan; ++c)
    {
        const int wc = word_counts[c];
        for (int j = 0; j < wc; ++j) { wav2_decode(wav.data() + wav_base + static_cast<crd::usize>(j), static_cast<int>(width), wc, static_cast<int>(nlines), wc * static_cast<int>(width), max_value); }
        wav_base += static_cast<crd::usize>(width) * nlines * static_cast<crd::usize>(wc);
    }
    apply_lut(lut.data(), wav.data(), nwords);
    wav_base = 0;
    crd::usize raw_off = 0;
    for (crd::u32 c = 0; c < nchan; ++c)
    {
        const int wc = word_counts[c];
        for (crd::u32 y = 0; y < nlines; ++y)
        {
            const crd::usize row = wav_base + static_cast<crd::usize>(y) * width * static_cast<crd::usize>(wc);
            const crd::usize dst = (static_cast<crd::usize>(y) * scan_words + raw_off) * 2U;
            for (crd::usize i = 0; i < static_cast<crd::usize>(width) * wc; ++i) { const crd::u16 v = wav[row + i]; out_raw[dst + i * 2U] = static_cast<crd::u8>(v & 0xFFU); out_raw[dst + i * 2U + 1U] = static_cast<crd::u8>((v >> 8U) & 0xFFU); }
        }
        wav_base += static_cast<crd::usize>(width) * nlines * static_cast<crd::usize>(wc);
        raw_off  += static_cast<crd::usize>(width) * static_cast<crd::usize>(wc);
    }
    return true;
}

Array<crd::u8> piz_compress(const crd::u8* raw, crd::usize raw_size, crd::u32 width, crd::u32 nlines, const crd::u8* word_counts, crd::u32 nchan, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    crd::usize sum_wc = 0; for (crd::u32 c = 0; c < nchan; ++c) { sum_wc += word_counts[c]; }
    const crd::usize nwords = static_cast<crd::usize>(width) * nlines * sum_wc;
    if (raw_size < nwords * 2U) { return out; }
    Array<crd::u16> wav(a); wav.resize(nwords > 0 ? nwords : 1);

    // de-interleave raw → channel-planar wav
    crd::usize wav_base = 0; crd::usize raw_off = 0;
    const crd::usize scan_words = static_cast<crd::usize>(width) * sum_wc;
    for (crd::u32 c = 0; c < nchan; ++c)
    {
        const int wc = word_counts[c];
        for (crd::u32 y = 0; y < nlines; ++y)
        {
            const crd::usize row = wav_base + static_cast<crd::usize>(y) * width * static_cast<crd::usize>(wc);
            const crd::usize src = (static_cast<crd::usize>(y) * scan_words + raw_off) * 2U;
            for (crd::usize i = 0; i < static_cast<crd::usize>(width) * wc; ++i) { wav[row + i] = ld16(raw + src + i * 2U); }
        }
        wav_base += static_cast<crd::usize>(width) * nlines * static_cast<crd::usize>(wc);
        raw_off  += static_cast<crd::usize>(width) * static_cast<crd::usize>(wc);
    }

    Array<crd::u8> bitmap(a); bitmap.resize(kBitmapSize);
    crd::u16 min_nz = 0; crd::u16 max_nz = 0;
    bitmap_from_data(wav.data(), nwords, bitmap.data(), min_nz, max_nz);
    Array<crd::u16> lut(a); lut.resize(kUShortRange);
    const crd::u16 max_value = forward_lut(bitmap.data(), lut.data());
    apply_lut(lut.data(), wav.data(), nwords);

    // wavelet encode per channel
    wav_base = 0;
    for (crd::u32 c = 0; c < nchan; ++c)
    {
        const int wc = word_counts[c];
        for (int j = 0; j < wc; ++j) { wav2_encode(wav.data() + wav_base + static_cast<crd::usize>(j), static_cast<int>(width), wc, static_cast<int>(nlines), wc * static_cast<int>(width), max_value); }
        wav_base += static_cast<crd::usize>(width) * nlines * static_cast<crd::usize>(wc);
    }

    put16(out, min_nz); put16(out, max_nz);
    if (min_nz <= max_nz) { for (int i = min_nz; i <= max_nz; ++i) { out.push_back(bitmap[i]); } }

    Array<crd::u8> comp(a); comp.resize(nwords * 4U + kHufEncSize * 2U + 1024U);
    const crd::usize clen = huf_compress(wav.data(), static_cast<int>(nwords), comp.data(), a);
    put32(out, static_cast<crd::u32>(clen));
    for (crd::usize i = 0; i < clen; ++i) { out.push_back(comp[i]); }
    return out;
}

} // namespace crd::resources::piz_detail
