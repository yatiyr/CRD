#pragma once

// Minimal SHA-1 implementation for UUID v5 content-addressed resource IDs.
// Based on RFC 3174. NOT intended for security use — only content dedup.
//
// sha1_compute(a, na, b, nb) hashes the two-part message [a||b] without
// allocating a temporary concatenated buffer. This is the call site for UUID
// v5: a = namespace bytes (16), b = content bytes.

#include <crd/core/types.hpp>

#include <array>
#include <cstring>

namespace crd::resources::detail
{

namespace sha1_detail
{
inline constexpr crd::u32 rol32(crd::u32 x, crd::u32 n) noexcept
{
    return (x << n) | (x >> (32U - n));
}

// Process one 64-byte block. h[0..4] are the running hash state.
inline void process_block(const crd::u8* block, crd::u32 (&h)[5]) noexcept
{
    crd::u32 w[80];
    for (crd::u32 i = 0U; i < 16U; ++i)
    {
        w[i] = (static_cast<crd::u32>(block[i * 4U    ]) << 24U)
             | (static_cast<crd::u32>(block[i * 4U + 1U]) << 16U)
             | (static_cast<crd::u32>(block[i * 4U + 2U]) <<  8U)
             |  static_cast<crd::u32>(block[i * 4U + 3U]);
    }
    for (crd::u32 i = 16U; i < 80U; ++i)
    {
        w[i] = rol32(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
    }

    crd::u32 a = h[0];
    crd::u32 b = h[1];
    crd::u32 c = h[2];
    crd::u32 d = h[3];
    crd::u32 e = h[4];

    for (crd::u32 i = 0U; i < 80U; ++i)
    {
        crd::u32 f = 0U;
        crd::u32 k = 0U;
        if (i < 20U)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999U;
        }
        else if (i < 40U)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        }
        else if (i < 60U)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }

        const crd::u32 temp = rol32(a, 5U) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30U);
        b = a;
        a = temp;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}
} // namespace sha1_detail

// Compute SHA-1 of the two-part message [part_a (na bytes) || part_b (nb bytes)].
// Returns the 20-byte digest.
inline std::array<crd::u8, 20> sha1_compute(
    const void* part_a, crd::usize na,
    const void* part_b, crd::usize nb) noexcept
{
    crd::u32 h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEu, 0x10325476U, 0xC3D2E1F0U};

    const crd::usize total_len = na + nb;
    const crd::u64   total_bits = static_cast<crd::u64>(total_len) * 8U;

    const auto* pa = static_cast<const crd::u8*>(part_a);
    const auto* pb = static_cast<const crd::u8*>(part_b);

    // Process the message 64 bytes at a time, stitching part_a and part_b.
    crd::usize pos = 0; // position in the logical [pa||pb] stream
    crd::u8    block[64];

    auto fill_block = [&]() noexcept
    {
        for (crd::usize i = 0U; i < 64U; ++i)
        {
            if (pos < na)
            {
                block[i] = pa[pos];
            }
            else if (pos < total_len)
            {
                block[i] = pb[pos - na];
            }
            else
            {
                block[i] = 0U;
            }
            ++pos;
        }
    };

    // Full 64-byte blocks from the combined message.
    const crd::usize full_blocks = total_len / 64U;
    for (crd::usize blk = 0U; blk < full_blocks; ++blk)
    {
        fill_block();
        sha1_detail::process_block(block, h);
    }

    // Remaining bytes.
    const crd::usize remaining = total_len - full_blocks * 64U;

    // Build the padding block(s).
    crd::u8 pad[128] = {};
    crd::usize pad_pos = 0U;
    for (crd::usize i = 0U; i < remaining; ++i)
    {
        const crd::usize src = full_blocks * 64U + i;
        pad[pad_pos++] = (src < na) ? pa[src] : pb[src - na];
    }
    pad[pad_pos] = 0x80U;

    // Write 64-bit big-endian bit length at byte 56 of the final block.
    auto write_length = [&](crd::u8* block_buf) noexcept
    {
        for (int i = 0; i < 8; ++i)
        {
            block_buf[63 - i] = static_cast<crd::u8>(total_bits >> (static_cast<crd::u32>(i) * 8U));
        }
    };

    if (remaining < 56U)
    {
        write_length(pad);
        sha1_detail::process_block(pad, h);
    }
    else
    {
        // Two padding blocks needed.
        sha1_detail::process_block(pad, h);
        crd::u8 pad2[64] = {};
        write_length(pad2);
        sha1_detail::process_block(pad2, h);
    }

    // Pack hash words into the output digest (big-endian).
    std::array<crd::u8, 20> digest{};
    for (int word = 0; word < 5; ++word)
    {
        for (int byte_idx = 0; byte_idx < 4; ++byte_idx)
        {
            digest[static_cast<crd::usize>(word * 4 + byte_idx)] =
                static_cast<crd::u8>(h[word] >> (24U - static_cast<crd::u32>(byte_idx) * 8U));
        }
    }
    return digest;
}

} // namespace crd::resources::detail
