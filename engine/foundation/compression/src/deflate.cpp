#include <crd/compression/deflate.hpp>
#include <crd/core/types.hpp>

#include <cstring>

// deflate.cpp — our own RFC-1951 DEFLATE + RFC-1950 zlib. inflate = a puff-style decoder (stored, fixed and dynamic
// Huffman + LZ77) into a buffer of the exact decoded size, which is its window, with zlib's validity rules; deflate = a
// single fixed-Huffman block with hash-chain LZ77 matching.

namespace crd::compression
{

using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;

namespace
{

// RFC 1951 length/distance tables.
constexpr crd::u16 kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr crd::u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr crd::u16 kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr crd::u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr crd::u8 kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
constexpr crd::usize kWindowSize = 32768;   // DEFLATE max back-reference distance
constexpr crd::u32 kMaxCodeBits = 15U;      // the longest Huffman code
constexpr crd::usize kLitCodes = 288U;      // the literal/length alphabet (286 and 287 occur only in the fixed code)
constexpr crd::usize kLitCodesUsed = 286U;  // the most literal/length codes a dynamic block may declare
constexpr crd::usize kDistCodes = 30U;      // the distance alphabet
constexpr crd::usize kFixedDistCodes = 32U; // the fixed code's five-bit distance codes (30 and 31 never occur)

// The fixed Huffman code's lengths (RFC 1951 §3.2.6), shared by the encoder and the decoder.
struct FixedLengths
{
    crd::u8 lit[kLitCodes] = {};
    crd::u8 dist[kFixedDistCodes] = {};
};
[[nodiscard]] constexpr FixedLengths make_fixed_lengths() noexcept
{
    FixedLengths f;
    // 0-143 eight bits, 144-255 nine, 256-279 seven, 280-287 eight.
    for (crd::usize s = 0; s < kLitCodes; ++s)
    {
        f.lit[s] = 8U;
    }
    for (crd::usize s = 144U; s < 256U; ++s)
    {
        f.lit[s] = 9U;
    }
    for (crd::usize s = 256U; s < 280U; ++s)
    {
        f.lit[s] = 7U;
    }
    for (crd::usize s = 0; s < kFixedDistCodes; ++s)
    {
        f.dist[s] = 5U;
    }
    return f;
}
constexpr FixedLengths kFixed = make_fixed_lengths();

// ── bit reader (DEFLATE is LSB-first)
// ──────────────────────────────────────────────────────────────────────────────────
// Bits are pulled a byte at a time, so after any read fewer than 8 buffered bits remain, all from the last byte
// pulled: aligning to a byte drops them and `p` is the next whole byte. A read past the input sets `err` and returns 0.
class BitReader
{
public:
    explicit BitReader(ConstSpan<crd::u8> in) noexcept : m_p(in.data()), m_end(in.data() + in.size()) {}

    // `n` (at most 16) bits, the first read in the lowest bit.
    [[nodiscard]] crd::u32 bits(crd::u32 n) noexcept
    {
        while (m_count < n)
        {
            if (m_p == m_end)
            {
                m_err = true;
                return 0U;
            }
            m_buf |= static_cast<crd::u32>(*m_p++) << m_count;
            m_count += 8U;
        }
        const crd::u32 v = m_buf & ((1U << n) - 1U);
        m_buf >>= n;
        m_count -= n;
        return v;
    }
    void align() noexcept
    {
        m_buf = 0U;
        m_count = 0U;
    }
    // The next `n` whole bytes after an `align`, or nullptr (and `err`) when fewer remain.
    [[nodiscard]] const crd::u8* take(crd::usize n) noexcept
    {
        if (static_cast<crd::usize>(m_end - m_p) < n)
        {
            m_err = true;
            return nullptr;
        }
        const crd::u8* at = m_p;
        m_p += n;
        return at;
    }
    [[nodiscard]] bool err() const noexcept { return m_err; }

private:
    const crd::u8* m_p;
    const crd::u8* m_end;
    crd::u32 m_buf = 0U;
    crd::u32 m_count = 0U;
    bool m_err = false;
};

// ── canonical Huffman decode table (puff-style: counts + sorted symbols)
// ────────────────────────────────────────────────
struct Huff
{
    crd::u16 count[kMaxCodeBits + 1U] = {};
    crd::u16 symbol[kLitCodes] = {};
};

// Build from code lengths (each at most 15). zlib's rules: an over-subscribed code is refused; an incomplete one is
// refused too, except a single one-bit code in a literal/length or distance tree (its unused code then decodes to an
// error); a tree without codes builds, and decoding with it is an error.
[[nodiscard]] constexpr bool huff_build(Huff& h, const crd::u8* lengths, crd::usize n, bool single_code_ok) noexcept
{
    for (crd::u16& c : h.count)
    {
        c = 0U;
    }
    for (crd::usize s = 0; s < n; ++s)
    {
        ++h.count[lengths[s]];
    }
    if (h.count[0] == n)
    {
        return true;
    }
    crd::i32 left = 1;
    for (crd::u32 len = 1; len <= kMaxCodeBits; ++len)
    {
        left = 2 * left - static_cast<crd::i32>(h.count[len]);
        if (left < 0)
        {
            return false;
        }
    }
    if (left > 0 && !(single_code_ok && h.count[1] == 1U && h.count[0] == n - 1U))
    {
        return false;
    }
    crd::u16 offs[kMaxCodeBits + 1U] = {};
    for (crd::u32 len = 1; len < kMaxCodeBits; ++len)
    {
        offs[len + 1U] = static_cast<crd::u16>(offs[len] + h.count[len]);
    }
    for (crd::usize s = 0; s < n; ++s)
    {
        if (lengths[s] != 0U)
        {
            h.symbol[offs[lengths[s]]++] = static_cast<crd::u16>(s);
        }
    }
    return true;
}

// The fixed code's trees, built once at compile time.
struct FixedTrees
{
    Huff lit;
    Huff dist;
    bool complete = false;
};
[[nodiscard]] constexpr FixedTrees make_fixed_trees() noexcept
{
    FixedTrees t;
    t.complete =
        huff_build(t.lit, kFixed.lit, kLitCodes, false) && huff_build(t.dist, kFixed.dist, kFixedDistCodes, false);
    return t;
}
constexpr FixedTrees kFixedTrees = make_fixed_trees();
static_assert(kFixedTrees.complete, "the fixed literal/length and distance codes are complete");

// The next symbol, or -1 past the longest code or the input.
[[nodiscard]] crd::i32 huff_decode(BitReader& br, const Huff& h) noexcept
{
    crd::i32 code = 0;
    crd::i32 first = 0;
    crd::i32 index = 0;
    for (crd::u32 len = 1; len <= kMaxCodeBits; ++len)
    {
        code |= static_cast<crd::i32>(br.bits(1U));
        const crd::i32 cnt = h.count[len];
        if (code - first < cnt)
        {
            return br.err() ? -1 : h.symbol[index + (code - first)];
        }
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

// The literal/length and distance trees of a dynamic block (§3.2.7).
[[nodiscard]] bool read_dynamic_trees(BitReader& br, Huff& lit, Huff& dist) noexcept
{
    const crd::u32 hlit = br.bits(5U) + 257U;
    const crd::u32 hdist = br.bits(5U) + 1U;
    const crd::u32 hclen = br.bits(4U) + 4U;
    if (br.err() || hlit > kLitCodesUsed || hdist > kDistCodes)
    {
        return false;
    }
    crd::u8 cl[19] = {};
    for (crd::u32 i = 0; i < hclen; ++i)
    {
        cl[kClOrder[i]] = static_cast<crd::u8>(br.bits(3U));
    }
    Huff clh;
    if (br.err() || !huff_build(clh, cl, 19U, false))
    {
        return false;
    }
    crd::u8 lengths[kLitCodesUsed + kDistCodes] = {};
    crd::u32 idx = 0;
    while (idx < hlit + hdist)
    {
        const crd::i32 sym = huff_decode(br, clh);
        if (sym < 0)
        {
            return false;
        }
        if (sym < 16)
        {
            lengths[idx++] = static_cast<crd::u8>(sym);
            continue;
        }
        crd::u8 value = 0U;
        crd::u32 repeat = 0U;
        if (sym == 16) // the previous length 3 to 6 times
        {
            if (idx == 0U)
            {
                return false;
            }
            value = lengths[idx - 1U];
            repeat = 3U + br.bits(2U);
        }
        else if (sym == 17) // zero 3 to 10 times
        {
            repeat = 3U + br.bits(3U);
        }
        else // 18: zero 11 to 138 times
        {
            repeat = 11U + br.bits(7U);
        }
        if (br.err() || idx + repeat > hlit + hdist)
        {
            return false;
        }
        for (crd::u32 r = 0; r < repeat; ++r)
        {
            lengths[idx++] = value;
        }
    }
    if (lengths[256] == 0U) // the end-of-block code must exist
    {
        return false;
    }
    return huff_build(lit, lengths, hlit, true) && huff_build(dist, lengths + hlit, hdist, true);
}

// One Huffman-coded block into `out` from `position`; every write stays inside `out`, every reference inside what
// this stream decoded. It works on local copies of the reader and the position: a byte store into `out` may alias
// anything, and would otherwise send both through memory on every byte.
[[nodiscard]] bool inflate_codes(BitReader& reader, const Huff& lit, const Huff& dist, Span<crd::u8> out,
                                 crd::usize& position) noexcept
{
    BitReader br = reader;
    crd::usize pos = position;
    while (true)
    {
        const crd::i32 sym = huff_decode(br, lit);
        if (sym < 0)
        {
            return false;
        }
        if (sym < 256)
        {
            if (pos == out.size())
            {
                return false;
            }
            out[pos++] = static_cast<crd::u8>(sym);
            continue;
        }
        if (sym == 256)
        {
            reader = br;
            position = pos;
            return true;
        }
        const crd::usize ls = static_cast<crd::usize>(sym) - 257U;
        if (ls >= 29U)
        {
            return false;
        }
        const crd::usize length = kLenBase[ls] + br.bits(kLenExtra[ls]);
        const crd::i32 dsym = huff_decode(br, dist);
        if (dsym < 0 || dsym >= 30)
        {
            return false;
        }
        const auto ds = static_cast<crd::usize>(dsym);
        const crd::usize distance = kDistBase[ds] + br.bits(kDistExtra[ds]);
        if (br.err() || distance > pos || length > out.size() - pos)
        {
            return false;
        }
        for (crd::usize i = 0; i < length; ++i, ++pos) // byte by byte: an overlapping copy repeats the run
        {
            out[pos] = out[pos - distance];
        }
    }
}

// ── bit writer + canonical encode
// ──────────────────────────────────────────────────────────────────────────────────────
struct BitWriter
{
    Array<crd::u8>* out;
    crd::u32 acc = 0;
    int nbits = 0;
    void putbits(crd::u32 v, int n)
    {
        acc |= (v & ((1U << n) - 1U)) << nbits;
        nbits += n;
        while (nbits >= 8)
        {
            out->push_back(static_cast<crd::u8>(acc & 0xFFU));
            acc >>= 8U;
            nbits -= 8;
        }
    }
    void put_code(crd::u32 code, int len)
    {
        for (int i = len - 1; i >= 0; --i)
        {
            putbits((code >> i) & 1U, 1);
        }
    } // canonical code is MSB-first
    void flush()
    {
        if (nbits > 0)
        {
            out->push_back(static_cast<crd::u8>(acc & 0xFFU));
            acc = 0;
            nbits = 0;
        }
    }
};
constexpr void canonical_codes(const crd::u8* len, crd::usize n, crd::u16* codes) noexcept
{
    crd::u16 bl[16] = {};
    for (crd::usize s = 0; s < n; ++s)
    {
        ++bl[len[s]];
    }
    bl[0] = 0;
    crd::u16 next[16] = {};
    crd::u16 code = 0;
    for (int bits = 1; bits <= 15; ++bits)
    {
        code = static_cast<crd::u16>((code + bl[bits - 1]) << 1);
        next[bits] = code;
    }
    for (crd::usize s = 0; s < n; ++s)
    {
        codes[s] = len[s] != 0U ? next[len[s]]++ : 0U;
    }
}

// The fixed code's canonical codes, built once at compile time for the encoder.
struct FixedCodes
{
    crd::u16 lit[kLitCodes] = {};
    crd::u16 dist[kFixedDistCodes] = {};
};
[[nodiscard]] constexpr FixedCodes make_fixed_codes() noexcept
{
    FixedCodes c;
    canonical_codes(kFixed.lit, kLitCodes, c.lit);
    canonical_codes(kFixed.dist, kFixedDistCodes, c.dist);
    return c;
}
constexpr FixedCodes kFixedCodes = make_fixed_codes();

} // namespace

// ── Adler-32
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────
crd::u32 adler32(ConstSpan<crd::u8> data) noexcept
{
    crd::u32 a = 1;
    crd::u32 b = 0;
    for (crd::usize i = 0; i < data.size(); ++i)
    {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

// ── inflate
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
bool inflate_raw(ConstSpan<crd::u8> in, Span<crd::u8> out) noexcept
{
    BitReader br(in);
    crd::usize pos = 0;
    crd::u32 bfinal = 0;
    do
    {
        bfinal = br.bits(1U);
        const crd::u32 btype = br.bits(2U);
        if (br.err())
        {
            return false;
        }
        if (btype == 0U) // stored
        {
            br.align();
            const crd::u8* head = br.take(4U);
            if (head == nullptr)
            {
                return false;
            }
            const crd::u32 len = static_cast<crd::u32>(head[0]) | (static_cast<crd::u32>(head[1]) << 8U);
            const crd::u32 nlen = static_cast<crd::u32>(head[2]) | (static_cast<crd::u32>(head[3]) << 8U);
            if ((len ^ 0xFFFFU) != nlen || len > out.size() - pos)
            {
                return false;
            }
            const crd::u8* body = br.take(len);
            if (body == nullptr)
            {
                return false;
            }
            if (len != 0U) // an empty output's data may be null, which memcpy may not be given
            {
                std::memcpy(out.data() + pos, body, len);
                pos += len;
            }
        }
        else if (btype == 1U)
        {
            if (!inflate_codes(br, kFixedTrees.lit, kFixedTrees.dist, out, pos))
            {
                return false;
            }
        }
        else if (btype == 2U)
        {
            Huff lit;
            Huff dist;
            if (!read_dynamic_trees(br, lit, dist) || !inflate_codes(br, lit, dist, out, pos))
            {
                return false;
            }
        }
        else // 3: reserved
        {
            return false;
        }
    } while (bfinal == 0U);
    return pos == out.size();
}

// ── deflate (single fixed-Huffman block + hash-chain LZ77)
// ──────────────────────────────────────────────────────────────
Array<crd::u8> deflate_raw(ConstSpan<crd::u8> in, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    BitWriter bw{&out};
    bw.putbits(1, 1); // bfinal
    bw.putbits(1, 2); // btype = 01 (fixed)

    const auto& fl = kFixed.lit;
    const auto& fl_codes = kFixedCodes.lit;
    const auto& fd_codes = kFixedCodes.dist;

    const crd::u8* d = in.data();
    const crd::usize n = in.size();

    Array<crd::i32> head(a);
    head.resize(1U << 15U);
    for (crd::usize i = 0; i < head.size(); ++i)
    {
        head[i] = -1;
    }
    Array<crd::i32> prev(a);
    prev.resize(n > 0 ? n : 1);
    auto hash3 = [&](crd::usize i) -> crd::u32
    {
        return ((static_cast<crd::u32>(d[i]) << 10U) ^ (static_cast<crd::u32>(d[i + 1]) << 5U) ^
                static_cast<crd::u32>(d[i + 2])) &
               0x7FFFU;
    };
    auto insert = [&](crd::usize i)
    {
        if (i + 3 <= n)
        {
            const crd::u32 h = hash3(i);
            prev[i] = head[h];
            head[h] = static_cast<crd::i32>(i);
        }
    };
    auto emit_len = [&](int length)
    {
        int s = 28;
        while (s > 0 && kLenBase[s] > length)
        {
            --s;
        }
        bw.put_code(fl_codes[257 + s], fl[257 + s]);
        bw.putbits(static_cast<crd::u32>(length - kLenBase[s]), kLenExtra[s]);
    };
    auto emit_dist = [&](int dst)
    {
        int s = 29;
        while (s > 0 && kDistBase[s] > dst)
        {
            --s;
        }
        bw.put_code(fd_codes[s], 5);
        bw.putbits(static_cast<crd::u32>(dst - kDistBase[s]), kDistExtra[s]);
    };

    crd::usize i = 0;
    while (i < n)
    {
        int best_len = 0;
        crd::usize best_dist = 0;
        if (i + 3 <= n)
        {
            const crd::u32 h = hash3(i);
            crd::i32 chain = head[h];
            int tries = 0;
            crd::usize maxl = n - i;
            if (maxl > 258U)
            {
                maxl = 258U;
            }
            while (chain >= 0 && tries < 128)
            {
                const crd::usize j = static_cast<crd::usize>(chain);
                if (i - j > kWindowSize)
                {
                    break;
                }
                crd::usize l = 0;
                while (l < maxl && d[j + l] == d[i + l])
                {
                    ++l;
                }
                if (static_cast<int>(l) > best_len)
                {
                    best_len = static_cast<int>(l);
                    best_dist = i - j;
                    if (l >= 258U)
                    {
                        break;
                    }
                }
                chain = prev[j];
                ++tries;
            }
        }
        if (best_len >= 3)
        {
            emit_len(best_len);
            emit_dist(static_cast<int>(best_dist));
            for (int k = 0; k < best_len; ++k)
            {
                insert(i + static_cast<crd::usize>(k));
            }
            i += static_cast<crd::usize>(best_len);
        }
        else
        {
            bw.put_code(fl_codes[d[i]], fl[d[i]]);
            insert(i);
            ++i;
        }
    }
    bw.put_code(fl_codes[256], fl[256]); // end of block
    bw.flush();
    return out;
}

// ── zlib wrapper
// ────────────────────────────────────────────────────────────────────────────────────────────────────────
bool zlib_inflate(ConstSpan<crd::u8> in, Span<crd::u8> out) noexcept
{
    if (in.size() < 6U)
    {
        return false;
    }
    const crd::u8 cmf = in[0];
    const crd::u8 flg = in[1];
    if ((cmf & 0x0FU) != 8U || (cmf >> 4U) > 7U) // DEFLATE with a window of at most 32 KiB
    {
        return false;
    }
    if (((static_cast<crd::u32>(cmf) << 8U) | flg) % 31U != 0U) // header checksum
    {
        return false;
    }
    if ((flg & 0x20U) != 0U) // preset dictionary unsupported
    {
        return false;
    }
    if (!inflate_raw(in.subspan(2U, in.size() - 6U), out)) // between the header and the Adler-32
    {
        return false;
    }
    const crd::u8* t = in.data() + in.size() - 4U;
    const crd::u32 want = (static_cast<crd::u32>(t[0]) << 24U) | (static_cast<crd::u32>(t[1]) << 16U) |
                          (static_cast<crd::u32>(t[2]) << 8U) | t[3];
    return adler32(out) == want;
}
Array<crd::u8> zlib_deflate(ConstSpan<crd::u8> in, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    out.push_back(0x78U);
    out.push_back(0x9CU); // CMF/FLG (32K window, deflate, default level; %31 == 0)
    Array<crd::u8> body = deflate_raw(in, a);
    for (crd::usize i = 0; i < body.size(); ++i)
    {
        out.push_back(body[i]);
    }
    const crd::u32 ad = adler32(in);
    out.push_back(static_cast<crd::u8>((ad >> 24U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((ad >> 16U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((ad >> 8U) & 0xFFU));
    out.push_back(static_cast<crd::u8>(ad & 0xFFU));
    return out;
}

} // namespace crd::compression
