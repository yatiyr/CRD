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

// The key schedule: the four key words and their parity word.
constexpr void threefry_key_schedule(const crd::u64 (&key)[4], crd::u64 (&ks)[5]) noexcept
{
    for (int w = 0; w < 4; ++w)
    {
        ks[w] = key[w];
    }
    ks[4] = kThreefryParity ^ key[0] ^ key[1] ^ key[2] ^ key[3];
}

template <int R> constexpr crd::u64 threefry_rotl(crd::u64 x) noexcept
{
    return (x << R) | (x >> (64 - R));
}

// One round on one block with the rotation pair of round M mod 8, then the word permutation (0, 3, 2, 1): swap x1 and
// x3. The rounds are templates so every rotation is an immediate: a runtime round loop shifted by a register and ran
// 2.5x slower.
template <int M> constexpr void threefry_round_block(crd::u64 (&x)[4]) noexcept
{
    x[0] += x[1];
    const crd::u64 x1 = threefry_rotl<kThreefryRot[M][0]>(x[1]) ^ x[0];
    x[2] += x[3];
    x[1] = threefry_rotl<kThreefryRot[M][1]>(x[3]) ^ x[2];
    x[3] = x1;
}

// Key injection number S (0 before the first round, then every four rounds) into one block.
template <int S> constexpr void threefry_inject_block(crd::u64 (&x)[4], const crd::u64 (&ks)[5]) noexcept
{
    x[0] += ks[S % 5];
    x[1] += ks[(S + 1) % 5];
    x[2] += ks[(S + 2) % 5];
    x[3] += ks[(S + 3) % 5] + static_cast<crd::u64>(S);
}

// One round on blocks B .. G - 1 of G independent blocks. The blocks unfold by template recursion, not a loop: GCC 13
// vectorizes a loop over the blocks at -O3 into shuffled 128-bit registers, 3.7x slower than the scalar blocks.
template <int M, int G, int B = 0> constexpr void threefry_round(crd::u64 (&x)[G][4]) noexcept
{
    if constexpr (B < G)
    {
        threefry_round_block<M>(x[B]);
        threefry_round<M, G, B + 1>(x);
    }
}

// Key injection S into blocks B .. G - 1.
template <int S, int G, int B = 0> constexpr void threefry_inject(crd::u64 (&x)[G][4], const crd::u64 (&ks)[5]) noexcept
{
    if constexpr (B < G)
    {
        threefry_inject_block<S>(x[B], ks);
        threefry_inject<S, G, B + 1>(x, ks);
    }
}

// Four rounds from round M mod 8, then key injection S.
template <int M, int S, int G> constexpr void threefry_rounds(crd::u64 (&x)[G][4], const crd::u64 (&ks)[5]) noexcept
{
    threefry_round<M, G>(x);
    threefry_round<M + 1, G>(x);
    threefry_round<M + 2, G>(x);
    threefry_round<M + 3, G>(x);
    threefry_inject<S, G>(x, ks);
}

// The 20 rounds of Threefry4x64-20 on G independent blocks after the first key injection: x holds each block's
// counter plus the key on entry and the block on return. Several blocks give the out-of-order core independent chains
// to overlap; adding the key to the counters before the rounds, not as a first injection, lets the compiler share the
// words the blocks have in common (1.06x on three blocks).
template <int G> constexpr void threefry4x64_rounds_of(crd::u64 (&x)[G][4], const crd::u64 (&ks)[5]) noexcept
{
    threefry_rounds<0, 1, G>(x, ks);
    threefry_rounds<4, 2, G>(x, ks);
    threefry_rounds<0, 3, G>(x, ks);
    threefry_rounds<4, 4, G>(x, ks);
    threefry_rounds<0, 5, G>(x, ks);
}

// Threefry4x64-20.
[[nodiscard]] constexpr ThreefryBlock threefry4x64(const crd::u64 (&ctr)[4], const crd::u64 (&key)[4]) noexcept
{
    crd::u64 ks[5] = {};
    threefry_key_schedule(key, ks);
    crd::u64 x[1][4] = {{ctr[0] + ks[0], ctr[1] + ks[1], ctr[2] + ks[2], ctr[3] + ks[3]}};
    threefry4x64_rounds_of<1>(x, ks);
    return ThreefryBlock{{x[0][0], x[0][1], x[0][2], x[0][3]}};
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
