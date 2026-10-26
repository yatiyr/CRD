#pragma once

// The block functions of the two counter-based generators (Salmon, Moraes, Dror and Shaw, SC'11, the Random123
// generators): Philox4x32-10 and Threefry4x64-20, and Threefry's counter step. They are pure functions of (counter,
// key), so a block exists independently of who computes it, in what order, on how many workers.
//
// General code (philox.hpp, threefry.hpp) and the per-level stats kernels (../../../../kernels/stats_kernels.cpp)
// both compile them, so they live in the unit's level namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5) and use only integer
// operators: no copy compiled for a higher ISA shares a symbol with general code's.

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::stats
{

// One 128-bit Philox block: 4 lanes of u32.
struct PhiloxBlock
{
    crd::u32 v[4];
};

// One 256-bit Threefry block: 4 lanes of u64.
struct ThreefryBlock
{
    crd::u64 v[4];
};

inline namespace CRD_MATH_LEVEL_NS
{

// Philox4x32-10. Counter lanes are (c0, c1, c2, c3); key lanes (k0, k1).
[[nodiscard]] constexpr PhiloxBlock philox4x32(const crd::u32 (&counter)[4], const crd::u32 (&key)[2]) noexcept
{
    constexpr crd::u32 m0 = 0xD2511F53U; // PHILOX_M4x32_0
    constexpr crd::u32 m1 = 0xCD9E8D57U; // PHILOX_M4x32_1
    constexpr crd::u32 w0 = 0x9E3779B9U; // PHILOX_W32_0 (golden ratio)
    constexpr crd::u32 w1 = 0xBB67AE85U; // PHILOX_W32_1 (sqrt(3)-1)

    crd::u32 c0 = counter[0];
    crd::u32 c1 = counter[1];
    crd::u32 c2 = counter[2];
    crd::u32 c3 = counter[3];
    crd::u32 k0 = key[0];
    crd::u32 k1 = key[1];

    for (int round = 0; round < 10; ++round)
    {
        if (round > 0) // the key schedule bumps between rounds (9 bumps for 10 rounds)
        {
            k0 += w0;
            k1 += w1;
        }
        const crd::u64 p0 = static_cast<crd::u64>(m0) * c0;
        const crd::u64 p1 = static_cast<crd::u64>(m1) * c2;
        const crd::u32 n0 = static_cast<crd::u32>(p1 >> 32) ^ c1 ^ k0;
        const crd::u32 n2 = static_cast<crd::u32>(p0 >> 32) ^ c3 ^ k1;
        c1 = static_cast<crd::u32>(p1);
        c3 = static_cast<crd::u32>(p0);
        c0 = n0;
        c2 = n2;
    }
    return PhiloxBlock{{c0, c1, c2, c3}};
}

// The Threefry rotation constants of the 4x64 variant, per round mod 8.
inline constexpr int kThreefryRot[8][2] = {{14, 16}, {52, 57}, {23, 40}, {5, 37},
                                           {25, 33}, {46, 12}, {58, 22}, {32, 32}};

// The Skein key-schedule parity word.
inline constexpr crd::u64 kThreefryParity = 0x1BD11BDAA9FC1A22ULL;

// Threefry4x64-20.
[[nodiscard]] constexpr ThreefryBlock threefry4x64(const crd::u64 (&ctr)[4], const crd::u64 (&key)[4]) noexcept
{
    const crd::u64 ks[5] = {key[0], key[1], key[2], key[3], kThreefryParity ^ key[0] ^ key[1] ^ key[2] ^ key[3]};
    crd::u64 x0 = ctr[0] + ks[0];
    crd::u64 x1 = ctr[1] + ks[1];
    crd::u64 x2 = ctr[2] + ks[2];
    crd::u64 x3 = ctr[3] + ks[3];
    for (int r = 0; r < 20; ++r)
    {
        const int m = r & 7;
        x0 += x1;
        x1 = (x1 << kThreefryRot[m][0]) | (x1 >> (64 - kThreefryRot[m][0]));
        x1 ^= x0;
        x2 += x3;
        x3 = (x3 << kThreefryRot[m][1]) | (x3 >> (64 - kThreefryRot[m][1]));
        x3 ^= x2;
        const crd::u64 t = x1; // the N=4 word permutation (0,3,2,1): swap x1 and x3
        x1 = x3;
        x3 = t;
        if ((r & 3) == 3) // key injection every 4 rounds
        {
            const int inj = (r + 1) / 4;
            x0 += ks[inj % 5];
            x1 += ks[(inj + 1) % 5];
            x2 += ks[(inj + 2) % 5];
            x3 += ks[(inj + 3) % 5] + static_cast<crd::u64>(inj);
        }
    }
    return ThreefryBlock{{x0, x1, x2, x3}};
}

// Advances a 256-bit Threefry counter (little-endian words) by n blocks, carrying through every word.
constexpr void threefry_counter_add(crd::u64 (&ctr)[4], crd::u64 n) noexcept
{
    ctr[0] += n;
    crd::u64 carry = ctr[0] < n ? 1U : 0U;
    for (int w = 1; w < 4; ++w)
    {
        ctr[w] += carry;
        carry = (carry != 0U && ctr[w] == 0U) ? 1U : 0U;
    }
}

} // namespace CRD_MATH_LEVEL_NS

} // namespace crd::hesap::stats
