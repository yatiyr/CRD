#pragma once

// crd-hesap-stats v12-e — PCG64-DXSM (Melissa O'Neill; NumPy's default BitGenerator since 1.17/1.26): a 128-bit LCG
// with the DXSM (double-xorshift-multiply) output permutation + a cheap 64-bit multiplier. Excellent statistical
// quality, 2^128 streams via the increment. Gated vs NumPy PCG64DXSM.random_raw at a known internal state.

#include <crd/hesap/stats/bitgen.hpp> // mul128
#include <crd/hesap/stats/splitmix64.hpp>

#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
class Pcg64Dxsm
{
public:
    // Convenience seeding (NOT NumPy-SeedSequence-compatible): 128-bit state from SplitMix64, odd increment from stream.
    explicit Pcg64Dxsm(crd::u64 seed, crd::u64 stream = 0) noexcept
    {
        SplitMix64 sm(seed);
        m_ih = sm.next_u64();
        m_il = (stream << 1) | 1U; // increment must be odd
        m_sh = sm.next_u64();
        m_sl = sm.next_u64();
        step();
    }

    // Direct-state construction (matches NumPy's 128-bit state/inc — for bit-exact cross-checks).
    [[nodiscard]] static Pcg64Dxsm from_state(crd::u64 state_hi, crd::u64 state_lo, crd::u64 inc_hi,
                                              crd::u64 inc_lo) noexcept
    {
        Pcg64Dxsm g;
        g.m_sh = state_hi;
        g.m_sl = state_lo;
        g.m_ih = inc_hi;
        g.m_il = inc_lo | 1U;
        return g;
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        crd::u64 hi = m_sh; // DXSM output from the CURRENT state, then advance
        const crd::u64 lo = m_sl | 1U;
        hi ^= hi >> 32;
        hi *= kCheapMul;
        hi ^= hi >> 48;
        hi *= lo;
        step();
        return hi;
    }

private:
    Pcg64Dxsm() noexcept = default;
    static constexpr crd::u64 kCheapMul = 0xda942042e4dd58b5ULL; // DXSM cheap multiplier (step + output)

    void step() noexcept // state = state * kCheapMul (128×64) + inc (128)
    {
        crd::u64 phi = 0;
        const crd::u64 plo = mul128(m_sl, kCheapMul, phi);
        crd::u64 nhi = phi + m_sh * kCheapMul;
        crd::u64 nlo = plo + m_il;
        nhi += m_ih + (nlo < plo ? 1U : 0U); // carry from the low add
        m_sl = nlo;
        m_sh = nhi;
    }

    crd::u64 m_sh = 0;
    crd::u64 m_sl = 0;
    crd::u64 m_ih = 0;
    crd::u64 m_il = 1;
};

} // namespace crd::hesap::stats
