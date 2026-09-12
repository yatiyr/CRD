#include <crd/resources/deflate.hpp>

#include <crd/core/types.hpp>

// deflate.cpp — our own RFC-1951 DEFLATE + RFC-1950 zlib. inflate = a puff-style decoder (stored/fixed/dynamic Huffman +
// LZ77); deflate = a single fixed-Huffman block with hash-chain LZ77 matching.

namespace crd::resources
{

using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{

// RFC 1951 length/distance tables.
constexpr crd::u16 kLenBase[29]  = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr crd::u8  kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr crd::u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr crd::u8  kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr crd::u8  kClOrder[19]  = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
constexpr crd::usize kWindowSize = 32768; // DEFLATE max back-reference distance

// ── bit reader (DEFLATE is LSB-first) ──────────────────────────────────────────────────────────────────────────────────
struct BitReader
{
    const crd::u8* p;
    const crd::u8* end;
    crd::u32       acc   = 0;
    int            nbits = 0;
    bool           err   = false;

    int getbit()
    {
        if (nbits == 0) { if (p < end) { acc = *p++; } else { acc = 0; err = true; } nbits = 8; }
        const int b = static_cast<int>(acc & 1U); acc >>= 1U; --nbits; return b;
    }
    crd::u32 getbits(int n) { crd::u32 v = 0; for (int i = 0; i < n; ++i) { v |= static_cast<crd::u32>(getbit()) << i; } return v; }
    void align() { nbits = 0; }
};

// ── canonical Huffman decode table (puff-style: counts + sorted symbols) ────────────────────────────────────────────────
struct Huff
{
    crd::u16 count[16]  = {};
    crd::u16 symbol[288] = {};
};
void huff_build(Huff& h, const crd::u8* lengths, int n)
{
    for (int i = 0; i < 16; ++i) { h.count[i] = 0; }
    for (int s = 0; s < n; ++s) { ++h.count[lengths[s]]; }
    h.count[0] = 0;
    crd::u16 offs[16] = {}; offs[1] = 0;
    for (int i = 1; i < 15; ++i) { offs[i + 1] = static_cast<crd::u16>(offs[i] + h.count[i]); }
    for (int s = 0; s < n; ++s) { if (lengths[s] != 0U) { h.symbol[offs[lengths[s]]++] = static_cast<crd::u16>(s); } }
}
[[nodiscard]] int huff_decode(BitReader& br, const Huff& h)
{
    int code = 0; int first = 0; int index = 0;
    for (int len = 1; len <= 15; ++len)
    {
        code |= br.getbit();
        const int cnt = h.count[len];
        if (code - first < cnt) { return h.symbol[index + (code - first)]; }
        index += cnt; first += cnt; first <<= 1; code <<= 1;
    }
    return -1;
}
void build_fixed(Huff& lit, Huff& dist)
{
    crd::u8 fl[288];
    for (int i = 0; i < 144; ++i) { fl[i] = 8; }
    for (int i = 144; i < 256; ++i) { fl[i] = 9; }
    for (int i = 256; i < 280; ++i) { fl[i] = 7; }
    for (int i = 280; i < 288; ++i) { fl[i] = 8; }
    huff_build(lit, fl, 288);
    crd::u8 fd[30]; for (int i = 0; i < 30; ++i) { fd[i] = 5; }
    huff_build(dist, fd, 30);
}

// ── bit writer + canonical encode ──────────────────────────────────────────────────────────────────────────────────────
struct BitWriter
{
    Array<crd::u8>* out;
    crd::u32        acc   = 0;
    int             nbits = 0;
    void putbits(crd::u32 v, int n) { acc |= (v & ((1U << n) - 1U)) << nbits; nbits += n; while (nbits >= 8) { out->push_back(static_cast<crd::u8>(acc & 0xFFU)); acc >>= 8U; nbits -= 8; } }
    void put_code(crd::u32 code, int len) { for (int i = len - 1; i >= 0; --i) { putbits((code >> i) & 1U, 1); } } // canonical code is MSB-first
    void flush() { if (nbits > 0) { out->push_back(static_cast<crd::u8>(acc & 0xFFU)); acc = 0; nbits = 0; } }
};
void canonical_codes(const crd::u8* len, int n, crd::u16* codes)
{
    crd::u16 bl[16] = {}; for (int s = 0; s < n; ++s) { ++bl[len[s]]; } bl[0] = 0;
    crd::u16 next[16] = {}; crd::u16 code = 0;
    for (int bits = 1; bits <= 15; ++bits) { code = static_cast<crd::u16>((code + bl[bits - 1]) << 1); next[bits] = code; }
    for (int s = 0; s < n; ++s) { codes[s] = len[s] != 0U ? next[len[s]]++ : 0U; }
}

} // namespace

// ── Adler-32 ────────────────────────────────────────────────────────────────────────────────────────────────────────────
crd::u32 adler32(ConstSpan<crd::u8> data) noexcept
{
    crd::u32 a = 1; crd::u32 b = 0;
    for (crd::usize i = 0; i < data.size(); ++i) { a = (a + data[i]) % 65521U; b = (b + a) % 65521U; }
    return (b << 16U) | a;
}

// ── inflate ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
bool inflate_raw(ConstSpan<crd::u8> in, Array<crd::u8>& out)
{
    BitReader br{in.data(), in.data() + in.size()};
    int bfinal = 0;
    do
    {
        bfinal        = br.getbit();
        const int bt  = static_cast<int>(br.getbits(2));
        if (br.err) { return false; }
        if (bt == 0) // stored
        {
            br.align();
            if (br.end - br.p < 4) { return false; }
            const crd::u32 len  = static_cast<crd::u32>(br.p[0]) | (static_cast<crd::u32>(br.p[1]) << 8U);
            const crd::u32 nlen = static_cast<crd::u32>(br.p[2]) | (static_cast<crd::u32>(br.p[3]) << 8U);
            br.p += 4;
            if ((len ^ 0xFFFFU) != nlen) { return false; }
            if (static_cast<crd::usize>(br.end - br.p) < len) { return false; }
            for (crd::u32 i = 0; i < len; ++i) { out.push_back(*br.p++); }
        }
        else if (bt == 1 || bt == 2)
        {
            Huff lit; Huff dist;
            if (bt == 1) { build_fixed(lit, dist); }
            else
            {
                const int hlit  = static_cast<int>(br.getbits(5)) + 257;
                const int hdist = static_cast<int>(br.getbits(5)) + 1;
                const int hclen = static_cast<int>(br.getbits(4)) + 4;
                if (hlit > 286 || hdist > 30) { return false; }
                crd::u8 cl[19] = {};
                for (int i = 0; i < hclen; ++i) { cl[kClOrder[i]] = static_cast<crd::u8>(br.getbits(3)); }
                Huff clh; huff_build(clh, cl, 19);
                crd::u8 lengths[288 + 32] = {};
                int idx = 0;
                while (idx < hlit + hdist)
                {
                    const int sym = huff_decode(br, clh);
                    if (sym < 0 || br.err) { return false; }
                    if (sym < 16) { lengths[idx++] = static_cast<crd::u8>(sym); }
                    else if (sym == 16) { if (idx == 0) { return false; } int r = static_cast<int>(br.getbits(2)) + 3; const crd::u8 pv = lengths[idx - 1]; while (r-- > 0 && idx < hlit + hdist) { lengths[idx++] = pv; } }
                    else if (sym == 17) { int r = static_cast<int>(br.getbits(3)) + 3; while (r-- > 0 && idx < hlit + hdist) { lengths[idx++] = 0; } }
                    else { int r = static_cast<int>(br.getbits(7)) + 11; while (r-- > 0 && idx < hlit + hdist) { lengths[idx++] = 0; } }
                }
                huff_build(lit, lengths, hlit);
                huff_build(dist, lengths + hlit, hdist);
            }
            while (true)
            {
                const int sym = huff_decode(br, lit);
                if (sym < 0 || br.err) { return false; }
                if (sym == 256) { break; }
                if (sym < 256) { out.push_back(static_cast<crd::u8>(sym)); }
                else
                {
                    const int ls = sym - 257;
                    if (ls >= 29) { return false; }
                    const int length = static_cast<int>(kLenBase[ls]) + static_cast<int>(br.getbits(kLenExtra[ls]));
                    const int dsym   = huff_decode(br, dist);
                    if (dsym < 0 || dsym >= 30) { return false; }
                    const int distance = static_cast<int>(kDistBase[dsym]) + static_cast<int>(br.getbits(kDistExtra[dsym]));
                    if (static_cast<crd::usize>(distance) > out.size()) { return false; }
                    const crd::usize start = out.size() - static_cast<crd::usize>(distance);
                    // NB: copy to a local first — push_back(out[k]) aliases the container; a realloc mid-copy would read a
                    // dangling reference (self-reference UAF). Index-fetch each byte into `b` before pushing.
                    for (int i = 0; i < length; ++i) { const crd::u8 b = out[start + static_cast<crd::usize>(i)]; out.push_back(b); }
                }
            }
        }
        else { return false; }
        if (br.err) { return false; }
    } while (bfinal == 0);
    return true;
}

// ── deflate (single fixed-Huffman block + hash-chain LZ77) ──────────────────────────────────────────────────────────────
Array<crd::u8> deflate_raw(ConstSpan<crd::u8> in, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    BitWriter      bw{&out};
    bw.putbits(1, 1); // bfinal
    bw.putbits(1, 2); // btype = 01 (fixed)

    crd::u8 fl[288];
    for (int i = 0; i < 144; ++i) { fl[i] = 8; }
    for (int i = 144; i < 256; ++i) { fl[i] = 9; }
    for (int i = 256; i < 280; ++i) { fl[i] = 7; }
    for (int i = 280; i < 288; ++i) { fl[i] = 8; }
    crd::u16 fl_codes[288]; canonical_codes(fl, 288, fl_codes);
    crd::u8 fd[30]; for (int i = 0; i < 30; ++i) { fd[i] = 5; }
    crd::u16 fd_codes[30]; canonical_codes(fd, 30, fd_codes);

    const crd::u8* d = in.data();
    const crd::usize n = in.size();

    Array<crd::i32> head(a); head.resize(1U << 15U); for (crd::usize i = 0; i < head.size(); ++i) { head[i] = -1; }
    Array<crd::i32> prev(a); prev.resize(n > 0 ? n : 1);
    auto hash3 = [&](crd::usize i) -> crd::u32 { return ((static_cast<crd::u32>(d[i]) << 10U) ^ (static_cast<crd::u32>(d[i + 1]) << 5U) ^ static_cast<crd::u32>(d[i + 2])) & 0x7FFFU; };
    auto insert = [&](crd::usize i) { if (i + 3 <= n) { const crd::u32 h = hash3(i); prev[i] = head[h]; head[h] = static_cast<crd::i32>(i); } };
    auto emit_len = [&](int length) { int s = 28; while (s > 0 && kLenBase[s] > length) { --s; } bw.put_code(fl_codes[257 + s], fl[257 + s]); bw.putbits(static_cast<crd::u32>(length - kLenBase[s]), kLenExtra[s]); };
    auto emit_dist = [&](int dst) { int s = 29; while (s > 0 && kDistBase[s] > dst) { --s; } bw.put_code(fd_codes[s], 5); bw.putbits(static_cast<crd::u32>(dst - kDistBase[s]), kDistExtra[s]); };

    crd::usize i = 0;
    while (i < n)
    {
        int        best_len  = 0;
        crd::usize best_dist = 0;
        if (i + 3 <= n)
        {
            const crd::u32 h = hash3(i);
            crd::i32 chain = head[h]; int tries = 0;
            crd::usize maxl = n - i; if (maxl > 258U) { maxl = 258U; }
            while (chain >= 0 && tries < 128)
            {
                const crd::usize j = static_cast<crd::usize>(chain);
                if (i - j > kWindowSize) { break; }
                crd::usize l = 0; while (l < maxl && d[j + l] == d[i + l]) { ++l; }
                if (static_cast<int>(l) > best_len) { best_len = static_cast<int>(l); best_dist = i - j; if (l >= 258U) { break; } }
                chain = prev[j]; ++tries;
            }
        }
        if (best_len >= 3)
        {
            emit_len(best_len);
            emit_dist(static_cast<int>(best_dist));
            for (int k = 0; k < best_len; ++k) { insert(i + static_cast<crd::usize>(k)); }
            i += static_cast<crd::usize>(best_len);
        }
        else { bw.put_code(fl_codes[d[i]], fl[d[i]]); insert(i); ++i; }
    }
    bw.put_code(fl_codes[256], fl[256]); // end of block
    bw.flush();
    return out;
}

// ── zlib wrapper ────────────────────────────────────────────────────────────────────────────────────────────────────────
bool zlib_inflate(ConstSpan<crd::u8> in, Array<crd::u8>& out)
{
    if (in.size() < 6U) { return false; }
    const crd::u8 cmf = in[0]; const crd::u8 flg = in[1];
    if ((cmf & 0x0FU) != 8U) { return false; }                                  // must be DEFLATE
    if (((static_cast<crd::u32>(cmf) << 8U) | flg) % 31U != 0U) { return false; } // header checksum
    if ((flg & 0x20U) != 0U) { return false; }                                  // preset dictionary unsupported
    const crd::usize start = out.size();
    const ConstSpan<crd::u8> body{in.data() + 2, in.size() - 6};                 // strip 2 header + 4 adler
    if (!inflate_raw(body, out)) { return false; }
    const crd::u32 want = (static_cast<crd::u32>(in[in.size() - 4]) << 24U) | (static_cast<crd::u32>(in[in.size() - 3]) << 16U) | (static_cast<crd::u32>(in[in.size() - 2]) << 8U) | in[in.size() - 1];
    return adler32({out.data() + start, out.size() - start}) == want;
}
Array<crd::u8> zlib_deflate(ConstSpan<crd::u8> in, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    out.push_back(0x78U); out.push_back(0x9CU); // CMF/FLG (32K window, deflate, default level; %31 == 0)
    Array<crd::u8> body = deflate_raw(in, a);
    for (crd::usize i = 0; i < body.size(); ++i) { out.push_back(body[i]); }
    const crd::u32 ad = adler32(in);
    out.push_back(static_cast<crd::u8>((ad >> 24U) & 0xFFU)); out.push_back(static_cast<crd::u8>((ad >> 16U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((ad >> 8U) & 0xFFU)); out.push_back(static_cast<crd::u8>(ad & 0xFFU));
    return out;
}

} // namespace crd::resources
