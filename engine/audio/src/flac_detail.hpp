#pragma once

// flac_detail.hpp — GEO-10: the FLAC codec's shared plumbing — MSB-first bit reader/writer, CRC-8 (poly 0x07)
// and CRC-16 (poly 0x8005) per the FLAC spec, and OUR OWN MD5 (RFC 1321 — STREAMINFO carries the MD5 of the
// unencoded PCM; the decoder verifies, the encoder stamps). Internal to crd-audio (the MED band lifts what it
// needs when the codec platform grows).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <cstring>

namespace crd::audio::detail
{

// ── CRCs (FLAC frame header CRC-8, whole-frame CRC-16) ─────────────────────────────────────────────────────────

[[nodiscard]] constexpr crd::u8 crc8_update(crd::u8 crc, crd::u8 byte) noexcept
{
    crc ^= byte;
    for (int i = 0; i < 8; ++i)
    {
        crc = (crc & 0x80U) != 0 ? static_cast<crd::u8>((crc << 1U) ^ 0x07U) : static_cast<crd::u8>(crc << 1U);
    }
    return crc;
}

[[nodiscard]] constexpr crd::u16 crc16_update(crd::u16 crc, crd::u8 byte) noexcept
{
    crc ^= static_cast<crd::u16>(byte) << 8U;
    for (int i = 0; i < 8; ++i)
    {
        crc = (crc & 0x8000U) != 0 ? static_cast<crd::u16>((crc << 1U) ^ 0x8005U)
                                   : static_cast<crd::u16>(crc << 1U);
    }
    return crc;
}

// ── MD5 (RFC 1321) ─────────────────────────────────────────────────────────────────────────────────────────────

struct Md5
{
    crd::u32 a = 0x67452301U;
    crd::u32 b = 0xEFCDAB89U;
    crd::u32 c = 0x98BADCFEU;
    crd::u32 d = 0x10325476U;
    crd::u64 total = 0;
    crd::u8  buf[64] = {};
    crd::u32 buf_len = 0;

    void update(const crd::u8* data, crd::usize n) noexcept
    {
        total += n;
        while (n > 0)
        {
            const crd::u32 take = static_cast<crd::u32>(n < 64U - buf_len ? n : 64U - buf_len);
            std::memcpy(buf + buf_len, data, take);
            buf_len += take;
            data += take;
            n -= take;
            if (buf_len == 64)
            {
                block(buf);
                buf_len = 0;
            }
        }
    }

    void final(crd::u8 out[16]) noexcept
    {
        const crd::u64 bits = total * 8;
        const crd::u8  pad  = 0x80;
        update(&pad, 1);
        const crd::u8 zero = 0;
        while (buf_len != 56) { update(&zero, 1); }
        crd::u8 len_le[8];
        for (int i = 0; i < 8; ++i) { len_le[i] = static_cast<crd::u8>((bits >> (8U * i)) & 0xFFU); }
        update(len_le, 8);
        const crd::u32 regs[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j) { out[i * 4 + j] = static_cast<crd::u8>((regs[i] >> (8U * j)) & 0xFFU); }
        }
    }

private:
    static constexpr crd::u32 kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static constexpr crd::u32 kK[64] = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
        0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
        0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
        0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
        0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};

    void block(const crd::u8* p) noexcept
    {
        crd::u32 m[16];
        for (int i = 0; i < 16; ++i)
        {
            m[i] = static_cast<crd::u32>(p[i * 4]) | (static_cast<crd::u32>(p[i * 4 + 1]) << 8U) |
                   (static_cast<crd::u32>(p[i * 4 + 2]) << 16U) | (static_cast<crd::u32>(p[i * 4 + 3]) << 24U);
        }
        crd::u32 ra = a;
        crd::u32 rb = b;
        crd::u32 rc = c;
        crd::u32 rd = d;
        for (crd::u32 i = 0; i < 64; ++i)
        {
            crd::u32 f = 0;
            crd::u32 g = 0;
            if (i < 16)
            {
                f = (rb & rc) | (~rb & rd);
                g = i;
            }
            else if (i < 32)
            {
                f = (rd & rb) | (~rd & rc);
                g = (5 * i + 1) % 16;
            }
            else if (i < 48)
            {
                f = rb ^ rc ^ rd;
                g = (3 * i + 5) % 16;
            }
            else
            {
                f = rc ^ (rb | ~rd);
                g = (7 * i) % 16;
            }
            const crd::u32 tmp = rd;
            rd                 = rc;
            rc                 = rb;
            const crd::u32 x   = ra + f + kK[i] + m[g];
            rb += (x << kS[i]) | (x >> (32U - kS[i]));
            ra = tmp;
        }
        a += ra;
        b += rb;
        c += rc;
        d += rd;
    }
};

// ── MSB-first bit reader ───────────────────────────────────────────────────────────────────────────────────────

struct BitReader
{
    const crd::u8* data = nullptr;
    crd::usize     size = 0;
    crd::usize     byte = 0;
    crd::u32       bit  = 0; // 0..7, MSB first
    bool           ok   = true;

    [[nodiscard]] crd::u64 read_bits(crd::u32 n) noexcept // n <= 57
    {
        crd::u64 v = 0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (byte >= size)
            {
                ok = false;
                return 0;
            }
            v = (v << 1U) | ((data[byte] >> (7U - bit)) & 1U);
            if (++bit == 8)
            {
                bit = 0;
                ++byte;
            }
        }
        return v;
    }

    [[nodiscard]] crd::i64 read_signed(crd::u32 n) noexcept
    {
        const crd::u64 u = read_bits(n);
        if (n == 0) { return 0; }
        const crd::u64 sign = 1ULL << (n - 1U);
        return (u & sign) != 0 ? static_cast<crd::i64>(u | ~((sign << 1U) - 1ULL)) : static_cast<crd::i64>(u);
    }

    [[nodiscard]] crd::u32 read_unary() noexcept
    {
        crd::u32 q = 0;
        while (ok && read_bits(1) == 0)
        {
            if (++q > 1'000'000U) // a hostile stream cannot spin forever
            {
                ok = false;
                return 0;
            }
        }
        return q;
    }

    [[nodiscard]] crd::u64 read_utf8() noexcept // the FLAC frame-number coding (up to 36 bits, 7 bytes)
    {
        const crd::u64 b0 = read_bits(8);
        if (!ok) { return 0; }
        if ((b0 & 0x80U) == 0) { return b0; }
        crd::u32 extra = 0;
        crd::u64 mask  = 0x40;
        while ((b0 & mask) != 0 && extra < 7)
        {
            ++extra;
            mask >>= 1U;
        }
        if (extra == 0 || extra > 6)
        {
            ok = false;
            return 0;
        }
        crd::u64 v = b0 & (mask - 1U);
        for (crd::u32 i = 0; i < extra; ++i)
        {
            const crd::u64 bx = read_bits(8);
            if ((bx & 0xC0U) != 0x80U)
            {
                ok = false;
                return 0;
            }
            v = (v << 6U) | (bx & 0x3FU);
        }
        return v;
    }

    void align() noexcept
    {
        if (bit != 0)
        {
            bit = 0;
            ++byte;
        }
    }
};

// ── MSB-first bit writer ───────────────────────────────────────────────────────────────────────────────────────

struct BitWriter
{
    crd::containers::Array<crd::u8>* out = nullptr;
    crd::u8                          acc = 0;
    crd::u32                         bit = 0;

    void write_bits(crd::u64 v, crd::u32 n) noexcept // n <= 57
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            acc = static_cast<crd::u8>((acc << 1U) | ((v >> (n - 1U - i)) & 1U));
            if (++bit == 8)
            {
                out->push_back(acc);
                acc = 0;
                bit = 0;
            }
        }
    }

    void write_unary(crd::u32 q) noexcept
    {
        for (crd::u32 i = 0; i < q; ++i) { write_bits(0, 1); }
        write_bits(1, 1);
    }

    // the FLAC frame-number coding: a k-byte group (k = 1..7) carries 7-k lead payload bits + 6 per
    // continuation = 5k+1 bits total (36 bits max at k=7)
    void write_utf8(crd::u64 v) noexcept
    {
        if (v < 0x80)
        {
            write_bits(v, 8);
            return;
        }
        crd::u32 k = 2;
        while (k < 7 && v >= (1ULL << (5U * k + 1U))) { ++k; }
        const crd::u32 lead_bits = 7U - k;
        const crd::u8  lead_mask = static_cast<crd::u8>(0xFFU << (8U - k)); // k ones then a zero
        write_bits(static_cast<crd::u64>(lead_mask) |
                       ((v >> (6U * (k - 1U))) & ((1ULL << lead_bits) - 1ULL)),
                   8);
        for (crd::u32 i = 1; i < k; ++i)
        {
            write_bits(0x80ULL | ((v >> (6U * (k - 1U - i))) & 0x3FULL), 8);
        }
    }

    void align_zero() noexcept
    {
        while (bit != 0) { write_bits(0, 1); }
    }
};

} // namespace crd::audio::detail
