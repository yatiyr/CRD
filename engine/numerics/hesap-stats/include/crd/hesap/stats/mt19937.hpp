#pragma once

// crd-hesap-stats v12-e — MT19937 (Matsumoto-Nishimura): the classic Mersenne Twister, period 2^19937−1, for
// reproducibility/compat (it's the historical default in C++ <random>, NumPy legacy, MATLAB). Faithful `mt19937ar.c`
// (init_genrand + init_by_array + tempering). Gated vs the canonical published KAT: init_by_array({0x123,0x234,
// 0x345,0x456}) → 1067595299, 955945823, 477289528, … next_u64 combines two tempered u32 (lo, hi).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
class Mt19937
{
public:
    explicit Mt19937(crd::u32 seed) noexcept { init_genrand(seed); }

    // Knuth-style array seeding (`init_by_array`) — the canonical KAT entry point + the NumPy-legacy seeding.
    [[nodiscard]] static Mt19937 from_array(crd::containers::Span<const crd::u32> key) noexcept
    {
        Mt19937 g(0);
        g.init_by_array(key);
        return g;
    }

    [[nodiscard]] crd::u32 next_u32() noexcept
    {
        if (m_mti >= kN)
        {
            generate();
        }
        crd::u32 y = m_mt[m_mti++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9D2C5680U;
        y ^= (y << 15) & 0xEFC60000U;
        y ^= y >> 18;
        return y;
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        const crd::u64 lo = next_u32();
        const crd::u64 hi = next_u32();
        return (hi << 32) | lo;
    }

private:
    static constexpr int kN = 624;
    static constexpr int kM = 397;
    static constexpr crd::u32 kMatrixA = 0x9908B0DFU;
    static constexpr crd::u32 kUpper = 0x80000000U;
    static constexpr crd::u32 kLower = 0x7FFFFFFFU;

    void init_genrand(crd::u32 s) noexcept
    {
        m_mt[0] = s;
        for (int i = 1; i < kN; ++i)
        {
            m_mt[i] = 1812433253U * (m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) + static_cast<crd::u32>(i);
        }
        m_mti = kN;
    }

    void init_by_array(crd::containers::Span<const crd::u32> key) noexcept
    {
        init_genrand(19650218U);
        const auto len = static_cast<int>(key.size());
        int i = 1;
        int j = 0;
        for (int k = (kN > len ? kN : len); k != 0; --k)
        {
            m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1664525U)) + key[j] + static_cast<crd::u32>(j);
            if (++i >= kN)
            {
                m_mt[0] = m_mt[kN - 1];
                i = 1;
            }
            if (++j >= len)
            {
                j = 0;
            }
        }
        for (int k = kN - 1; k != 0; --k)
        {
            m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1566083941U)) - static_cast<crd::u32>(i);
            if (++i >= kN)
            {
                m_mt[0] = m_mt[kN - 1];
                i = 1;
            }
        }
        m_mt[0] = 0x80000000U;
    }

    void generate() noexcept
    {
        for (int kk = 0; kk < kN - kM; ++kk)
        {
            const crd::u32 y = (m_mt[kk] & kUpper) | (m_mt[kk + 1] & kLower);
            m_mt[kk] = m_mt[kk + kM] ^ (y >> 1) ^ ((y & 1U) != 0U ? kMatrixA : 0U);
        }
        for (int kk = kN - kM; kk < kN - 1; ++kk)
        {
            const crd::u32 y = (m_mt[kk] & kUpper) | (m_mt[kk + 1] & kLower);
            m_mt[kk] = m_mt[kk + (kM - kN)] ^ (y >> 1) ^ ((y & 1U) != 0U ? kMatrixA : 0U);
        }
        const crd::u32 y = (m_mt[kN - 1] & kUpper) | (m_mt[0] & kLower);
        m_mt[kN - 1] = m_mt[kM - 1] ^ (y >> 1) ^ ((y & 1U) != 0U ? kMatrixA : 0U);
        m_mti = 0;
    }

    crd::u32 m_mt[kN];
    int m_mti = kN + 1;
};

} // namespace crd::hesap::stats
