#pragma once

// crd-hesap-stats v12-e — xoshiro256** and xoshiro256++ (Blackman & Vigna 2018): 256-bit-state, period 2^256−1,
// the modern fast all-purpose PRNGs. Seeded by SplitMix64 (the author-recommended seeding). jump()/long_jump()
// advance 2^128 / 2^192 steps for non-overlapping parallel streams. KAT-anchored: state {1,2,3,4} → first ** output
// rotl(2·5,7)·9 = 11520. Gated vs a faithful reference + run-twice determinism moat.

#include <crd/hesap/stats/bitgen.hpp>
#include <crd/hesap/stats/splitmix64.hpp>

#include <crd/core/types.hpp>

namespace crd::hesap::stats
{
namespace detail
{
// Shared 256-bit state + LFSR update; the two variants differ only in the scrambler.
struct Xoshiro256State
{
    crd::u64 s[4];

    explicit Xoshiro256State(crd::u64 seed) noexcept
    {
        SplitMix64 sm(seed);
        for (crd::u64& v : s)
        {
            v = sm.next_u64();
        }
    }
    Xoshiro256State(crd::u64 s0, crd::u64 s1, crd::u64 s2, crd::u64 s3) noexcept : s{s0, s1, s2, s3} {}

    void advance() noexcept
    {
        const crd::u64 t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl64(s[3], 45);
    }

    // jump by 2^128 (jp = long-jump table) — for parallel non-overlapping streams.
    void jump(const crd::u64 (&jp)[4]) noexcept
    {
        crd::u64 s0 = 0;
        crd::u64 s1 = 0;
        crd::u64 s2 = 0;
        crd::u64 s3 = 0;
        for (const crd::u64 word : jp)
        {
            for (int b = 0; b < 64; ++b)
            {
                if ((word & (crd::u64{1} << b)) != 0U)
                {
                    s0 ^= s[0];
                    s1 ^= s[1];
                    s2 ^= s[2];
                    s3 ^= s[3];
                }
                advance();
            }
        }
        s[0] = s0;
        s[1] = s1;
        s[2] = s2;
        s[3] = s3;
    }
};
inline constexpr crd::u64 kXoshiroJump[4] = {0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL, 0xa9582618e03fc9aaULL,
                                             0x39abdc4529b1661cULL};
inline constexpr crd::u64 kXoshiroLongJump[4] = {0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL, 0x77710069854ee241ULL,
                                                 0x39109bb02acbe635ULL};
} // namespace detail

class Xoshiro256ss // xoshiro256** (general-purpose; mul-rotl-mul scrambler)
{
public:
    explicit Xoshiro256ss(crd::u64 seed) noexcept : m_st(seed) {}
    [[nodiscard]] static Xoshiro256ss from_state(crd::u64 s0, crd::u64 s1, crd::u64 s2, crd::u64 s3) noexcept
    {
        Xoshiro256ss g(0);
        g.m_st = detail::Xoshiro256State(s0, s1, s2, s3);
        return g;
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        const crd::u64 result = rotl64(m_st.s[1] * 5, 7) * 9;
        m_st.advance();
        return result;
    }
    void jump() noexcept { m_st.jump(detail::kXoshiroJump); }
    void long_jump() noexcept { m_st.jump(detail::kXoshiroLongJump); }

private:
    detail::Xoshiro256State m_st;
};

class Xoshiro256pp // xoshiro256++ (add-rotl-add scrambler; faster, for floats)
{
public:
    explicit Xoshiro256pp(crd::u64 seed) noexcept : m_st(seed) {}

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        const crd::u64 result = rotl64(m_st.s[0] + m_st.s[3], 23) + m_st.s[0];
        m_st.advance();
        return result;
    }
    void jump() noexcept { m_st.jump(detail::kXoshiroJump); }
    void long_jump() noexcept { m_st.jump(detail::kXoshiroLongJump); }

private:
    detail::Xoshiro256State m_st;
};

} // namespace crd::hesap::stats
