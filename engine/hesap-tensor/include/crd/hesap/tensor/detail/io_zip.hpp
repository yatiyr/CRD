#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-l io detail: CRC-32 + RFC 1951 DEFLATE decompressor
// (read side) for the .npz zip container.
//
// Scope (deliberate): npz WRITE emits STORED members (exactly what np.savez
// does — method 0), so no compressor lives here; npz READ must also accept
// DEFLATE members (np.savez_compressed — method 8), so a full RFC 1951
// inflate does. Decoding is the canonical-Huffman counts/offsets walk of
// RFC 1951 §3.2.2 (the puff.c reference structure); the destination size is
// known exactly from the zip directory, so the LZ77 window is the
// destination buffer itself — no ring buffer, no scratch allocation.
//
// HOME-> flag: if the engine ever gains a general compression module, this
// inflate (and crc32) is the seed — move it there and re-point this include.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <cstring> // memcpy (LE-host scalar reads, no unaligned UB)

namespace crd::hesap::tensor::iodetail
{

// ---- little-endian scalar peeks/pokes (LE-host posture, memcpy for alignment) ----

[[nodiscard]] inline crd::u16 rd_u16(const crd::u8* p) noexcept
{
    crd::u16 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
[[nodiscard]] inline crd::u32 rd_u32(const crd::u8* p) noexcept
{
    crd::u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
[[nodiscard]] inline crd::u64 rd_u64(const crd::u8* p) noexcept
{
    crd::u64 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline void wr_u16(crd::u8* p, crd::u16 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}
inline void wr_u32(crd::u8* p, crd::u32 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}
inline void wr_u64(crd::u8* p, crd::u64 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}

// ---- CRC-32 (zip / ISO 3309, reflected poly 0xEDB88320), table at compile time ----

struct Crc32Table
{
    crd::u32 t[256];
};

[[nodiscard]] constexpr Crc32Table make_crc32_table() noexcept
{
    Crc32Table tab{};
    for (crd::u32 i = 0; i < 256U; ++i)
    {
        crd::u32 c = i;
        for (crd::u32 k = 0; k < 8U; ++k)
        {
            c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        tab.t[i] = c;
    }
    return tab;
}

inline constexpr Crc32Table kCrc32Table = make_crc32_table();

[[nodiscard]] inline crd::u32 crc32(crd::containers::ConstSpan<crd::u8> data) noexcept
{
    crd::u32 c = 0xFFFFFFFFU;
    for (const crd::u8 b : data)
    {
        c = kCrc32Table.t[(c ^ b) & 0xFFU] ^ (c >> 8U);
    }
    return c ^ 0xFFFFFFFFU;
}

// ---- RFC 1951 inflate -----------------------------------------------------

namespace inflatedetail
{

// LSB-first bit reader over the compressed byte span.
class BitReader
{
public:
    explicit BitReader(crd::containers::ConstSpan<crd::u8> src) noexcept : m_src(src) {}

    // Read `need` (<= 16) bits LSB-first; sets fail() past end of input.
    [[nodiscard]] crd::u32 bits(crd::u32 need) noexcept
    {
        while (m_bitcnt < need)
        {
            if (m_pos >= m_src.size())
            {
                m_fail = true;
                return 0U;
            }
            m_bitbuf |= static_cast<crd::u32>(m_src[m_pos++]) << m_bitcnt;
            m_bitcnt += 8U;
        }
        const crd::u32 v = m_bitbuf & ((1U << need) - 1U);
        m_bitbuf >>= need;
        m_bitcnt -= need;
        return v;
    }

    void align_to_byte() noexcept
    {
        m_bitbuf = 0U;
        m_bitcnt = 0U;
    }

    [[nodiscard]] crd::usize pos() const noexcept { return m_pos; }
    void set_pos(crd::usize p) noexcept { m_pos = p; }
    [[nodiscard]] crd::usize remaining() const noexcept { return m_src.size() - m_pos; }
    [[nodiscard]] const crd::u8* at(crd::usize p) const noexcept { return m_src.data() + p; }
    [[nodiscard]] bool fail() const noexcept { return m_fail; }
    void set_fail() noexcept { m_fail = true; }

private:
    crd::containers::ConstSpan<crd::u8> m_src;
    crd::usize m_pos = 0;
    crd::u32 m_bitbuf = 0;
    crd::u32 m_bitcnt = 0;
    bool m_fail = false;
};

inline constexpr crd::u32 kMaxBits = 15U;    // longest Huffman code length
inline constexpr crd::u32 kMaxLitLen = 288U; // literal/length alphabet size
inline constexpr crd::u32 kMaxDist = 30U;    // distance alphabet size

// Canonical Huffman decode tables: count of codes per length + symbols in
// canonical (length-major, symbol-minor) order.
struct Huffman
{
    crd::u16 count[kMaxBits + 1U];
    crd::u16 symbol[kMaxLitLen];
};

// Build from code lengths; rejects over-subscribed codes (incomplete codes
// are legal per the RFC for degenerate distance trees — decode() walks off
// them and errors, matching the reference decoder's behaviour).
[[nodiscard]] inline bool huffman_build(Huffman& h, const crd::u16* lengths, crd::u32 n) noexcept
{
    for (crd::u32 l = 0; l <= kMaxBits; ++l)
    {
        h.count[l] = 0U;
    }
    for (crd::u32 s = 0; s < n; ++s)
    {
        if (lengths[s] > kMaxBits)
        {
            return false;
        }
        ++h.count[lengths[s]];
    }
    if (h.count[0] == n) // no codes at all — legal only for an unused tree
    {
        return true;
    }
    crd::i32 left = 1; // over-subscription check (one slot at length 0)
    for (crd::u32 l = 1; l <= kMaxBits; ++l)
    {
        left <<= 1;
        left -= static_cast<crd::i32>(h.count[l]);
        if (left < 0)
        {
            return false;
        }
    }
    crd::u16 offs[kMaxBits + 1U];
    offs[1] = 0U;
    for (crd::u32 l = 1; l < kMaxBits; ++l)
    {
        offs[l + 1U] = static_cast<crd::u16>(offs[l] + h.count[l]);
    }
    for (crd::u32 s = 0; s < n; ++s)
    {
        if (lengths[s] != 0U)
        {
            h.symbol[offs[lengths[s]]++] = static_cast<crd::u16>(s);
        }
    }
    return true;
}

// Canonical decode: accumulate bits MSB-of-code-first, tracking the first
// code and symbol-table offset per length (RFC 1951 §3.2.2 reference walk).
[[nodiscard]] inline crd::i32 huffman_decode(BitReader& br, const Huffman& h) noexcept
{
    crd::i32 code = 0;
    crd::i32 first = 0;
    crd::i32 index = 0;
    for (crd::u32 len = 1; len <= kMaxBits; ++len)
    {
        code |= static_cast<crd::i32>(br.bits(1U));
        if (br.fail())
        {
            return -1;
        }
        const crd::i32 cnt = static_cast<crd::i32>(h.count[len]);
        if (code - cnt < first)
        {
            return static_cast<crd::i32>(h.symbol[index + (code - first)]);
        }
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1; // ran past the longest code — corrupt stream
}

// length/distance extra-bit tables (RFC 1951 §3.2.5)
inline constexpr crd::u16 kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr crd::u16 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr crd::u16 kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                           33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                           1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
inline constexpr crd::u16 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decode one Huffman-coded block into dst at write position `out`.
[[nodiscard]] inline bool inflate_codes(BitReader& br, const Huffman& lit, const Huffman& dist,
                                        crd::containers::Span<crd::u8> dst, crd::usize& out) noexcept
{
    for (;;)
    {
        const crd::i32 sym = huffman_decode(br, lit);
        if (sym < 0)
        {
            return false;
        }
        if (sym < 256) // literal byte
        {
            if (out >= dst.size())
            {
                return false; // output overrun — corrupt or size mismatch
            }
            dst[out++] = static_cast<crd::u8>(sym);
            continue;
        }
        if (sym == 256) // end of block
        {
            return true;
        }
        const crd::u32 li = static_cast<crd::u32>(sym - 257);
        if (li >= 29U)
        {
            return false;
        }
        const crd::usize len = static_cast<crd::usize>(kLenBase[li]) + br.bits(kLenExtra[li]);
        const crd::i32 dsym = huffman_decode(br, dist);
        if (dsym < 0 || dsym >= static_cast<crd::i32>(kMaxDist))
        {
            return false;
        }
        const crd::usize d =
            static_cast<crd::usize>(kDistBase[dsym]) + br.bits(kDistExtra[static_cast<crd::u32>(dsym)]);
        if (br.fail() || d > out || out + len > dst.size())
        {
            return false;
        }
        // byte-by-byte copy — overlapping copies are the LZ77 semantics
        for (crd::usize i = 0; i < len; ++i)
        {
            dst[out] = dst[out - d];
            ++out;
        }
    }
}

} // namespace inflatedetail

// Decompress a raw DEFLATE stream (RFC 1951, no zlib/gzip wrapper — the zip
// member payload) into dst. dst.size() must be the exact uncompressed size
// (known from the zip directory). Returns false on any malformed stream or
// size mismatch. noexcept, allocation-free.
[[nodiscard]] inline bool inflate(crd::containers::ConstSpan<crd::u8> src, crd::containers::Span<crd::u8> dst) noexcept
{
    using namespace inflatedetail;
    BitReader br(src);
    crd::usize out = 0;
    for (;;)
    {
        const crd::u32 bfinal = br.bits(1U);
        const crd::u32 btype = br.bits(2U);
        if (br.fail())
        {
            return false;
        }
        if (btype == 0U) // stored block
        {
            br.align_to_byte();
            if (br.remaining() < 4U)
            {
                return false;
            }
            const crd::u16 len = rd_u16(br.at(br.pos()));
            const crd::u16 nlen = rd_u16(br.at(br.pos() + 2U));
            br.set_pos(br.pos() + 4U);
            if (static_cast<crd::u16>(~len) != nlen || br.remaining() < len || out + len > dst.size())
            {
                return false;
            }
            if (len > 0U)
            {
                std::memcpy(dst.data() + out, br.at(br.pos()), len);
                br.set_pos(br.pos() + len);
                out += len;
            }
        }
        else if (btype == 1U || btype == 2U)
        {
            Huffman lit;
            Huffman dist;
            if (btype == 1U) // fixed Huffman tables (§3.2.6)
            {
                crd::u16 lengths[kMaxLitLen];
                for (crd::u32 s = 0; s < 144U; ++s)
                {
                    lengths[s] = 8U;
                }
                for (crd::u32 s = 144U; s < 256U; ++s)
                {
                    lengths[s] = 9U;
                }
                for (crd::u32 s = 256U; s < 280U; ++s)
                {
                    lengths[s] = 7U;
                }
                for (crd::u32 s = 280U; s < kMaxLitLen; ++s)
                {
                    lengths[s] = 8U;
                }
                if (!huffman_build(lit, lengths, kMaxLitLen))
                {
                    return false;
                }
                for (crd::u32 s = 0; s < kMaxDist; ++s)
                {
                    lengths[s] = 5U;
                }
                if (!huffman_build(dist, lengths, kMaxDist))
                {
                    return false;
                }
            }
            else // dynamic tables (§3.2.7)
            {
                static constexpr crd::u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                const crd::u32 hlit = br.bits(5U) + 257U;
                const crd::u32 hdist = br.bits(5U) + 1U;
                const crd::u32 hclen = br.bits(4U) + 4U;
                if (br.fail() || hlit > kMaxLitLen || hdist > kMaxDist)
                {
                    return false;
                }
                crd::u16 cl_lengths[19] = {};
                for (crd::u32 i = 0; i < hclen; ++i)
                {
                    cl_lengths[order[i]] = static_cast<crd::u16>(br.bits(3U));
                }
                Huffman cl;
                if (br.fail() || !huffman_build(cl, cl_lengths, 19U))
                {
                    return false;
                }
                crd::u16 lengths[kMaxLitLen + kMaxDist] = {};
                crd::u32 idx = 0;
                while (idx < hlit + hdist)
                {
                    const crd::i32 sym = huffman_decode(br, cl);
                    if (sym < 0)
                    {
                        return false;
                    }
                    if (sym < 16) // literal code length
                    {
                        lengths[idx++] = static_cast<crd::u16>(sym);
                        continue;
                    }
                    crd::u32 repeat = 0;
                    crd::u16 value = 0;
                    if (sym == 16) // repeat previous length 3..6
                    {
                        if (idx == 0U)
                        {
                            return false;
                        }
                        value = lengths[idx - 1U];
                        repeat = 3U + br.bits(2U);
                    }
                    else if (sym == 17) // repeat zero 3..10
                    {
                        repeat = 3U + br.bits(3U);
                    }
                    else // 18: repeat zero 11..138
                    {
                        repeat = 11U + br.bits(7U);
                    }
                    if (br.fail() || idx + repeat > hlit + hdist)
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
                if (!huffman_build(lit, lengths, hlit) || !huffman_build(dist, lengths + hlit, hdist))
                {
                    return false;
                }
            }
            if (!inflate_codes(br, lit, dist, dst, out))
            {
                return false;
            }
        }
        else // btype == 3 — reserved
        {
            return false;
        }
        if (bfinal != 0U)
        {
            break;
        }
    }
    return out == dst.size();
}

} // namespace crd::hesap::tensor::iodetail
