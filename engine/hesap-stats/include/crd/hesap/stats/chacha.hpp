#pragma once

// crd-hesap-stats v12-g — ChaCha20 counter-based CRNG (Bernstein; RFC 8439). A cryptographic-strength stream: 512-bit
// state (constants + 256-bit key + 64-bit counter + 64-bit nonce), 20 ARX rounds → 16 u32 per block. PURE in
// (counter, key, nonce) ⇒ seekable + the determinism moat. Gated vs the RFC 8439 all-zero-key test vector.

#include <crd/hesap/stats/bitgen.hpp> // rotl (u32 variant below)
#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
class ChaCha20Rng
{
public:
    explicit ChaCha20Rng(crd::u64 seed, crd::u64 stream = 0) noexcept
    {
        // Key = SplitMix64-expanded seed (8 u32); nonce = stream.
        crd::u64 s = seed;
        for (int i = 0; i < 4; ++i)
        {
            s += 0x9E3779B97F4A7C15ULL;
            crd::u64 z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z ^= z >> 31;
            m_key[2 * i] = static_cast<crd::u32>(z);
            m_key[2 * i + 1] = static_cast<crd::u32>(z >> 32);
        }
        m_nonce[0] = static_cast<crd::u32>(stream);
        m_nonce[1] = static_cast<crd::u32>(stream >> 32);
    }

    // Direct-key construction (for the RFC KAT).
    [[nodiscard]] static ChaCha20Rng from_key(const crd::u32 (&key)[8], crd::u64 counter, const crd::u32 (&nonce)[2]) noexcept
    {
        ChaCha20Rng g(0);
        for (int i = 0; i < 8; ++i)
        {
            g.m_key[i] = key[i];
        }
        g.m_counter = counter;
        g.m_nonce[0] = nonce[0];
        g.m_nonce[1] = nonce[1];
        return g;
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        if (m_idx >= 16U)
        {
            refill();
        }
        const crd::u64 lo = m_block[m_idx++];
        const crd::u64 hi = m_block[m_idx++];
        return (hi << 32) | lo;
    }

private:
    static crd::u32 rotl(crd::u32 x, int k) noexcept { return (x << k) | (x >> (32 - k)); }
    static void qr(crd::u32& a, crd::u32& b, crd::u32& c, crd::u32& d) noexcept
    {
        a += b;
        d = rotl(d ^ a, 16);
        c += d;
        b = rotl(b ^ c, 12);
        a += b;
        d = rotl(d ^ a, 8);
        c += d;
        b = rotl(b ^ c, 7);
    }

    void refill() noexcept
    {
        crd::u32 s[16] = {0x61707865U,
                          0x3320646EU,
                          0x79622D32U,
                          0x6B206574U, // "expand 32-byte k"
                          m_key[0],   m_key[1], m_key[2], m_key[3], m_key[4], m_key[5], m_key[6], m_key[7],
                          static_cast<crd::u32>(m_counter),
                          static_cast<crd::u32>(m_counter >> 32),
                          m_nonce[0],
                          m_nonce[1]};
        crd::u32 x[16];
        for (int i = 0; i < 16; ++i)
        {
            x[i] = s[i];
        }
        for (int i = 0; i < 10; ++i) // 20 rounds = 10 column+diagonal double-rounds
        {
            qr(x[0], x[4], x[8], x[12]);
            qr(x[1], x[5], x[9], x[13]);
            qr(x[2], x[6], x[10], x[14]);
            qr(x[3], x[7], x[11], x[15]);
            qr(x[0], x[5], x[10], x[15]);
            qr(x[1], x[6], x[11], x[12]);
            qr(x[2], x[7], x[8], x[13]);
            qr(x[3], x[4], x[9], x[14]);
        }
        for (int i = 0; i < 16; ++i)
        {
            m_block[i] = x[i] + s[i];
        }
        ++m_counter;
        m_idx = 0;
    }

    crd::u32 m_key[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    crd::u32 m_nonce[2] = {0, 0};
    crd::u64 m_counter = 0;
    crd::u32 m_block[16] = {0};
    crd::u32 m_idx = 16U;
};

} // namespace crd::hesap::stats
