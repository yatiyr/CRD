// crd-hesap-stats v12-e — the BitGenerator RNG suite, bit-exact gated vs published KATs / NumPy + determinism moat.

#include <crd/hesap/stats/stats.hpp>

#include "rng_refs.inc"

#include <catch2/catch_test_macros.hpp>

namespace st = crd::hesap::stats;
using crd::u32;
using crd::u64;

// The concept holds for every engine.
static_assert(st::BitGenerator<st::SplitMix64>);
static_assert(st::BitGenerator<st::Xoshiro256ss>);
static_assert(st::BitGenerator<st::Xoshiro256pp>);
static_assert(st::BitGenerator<st::Sfc64>);
static_assert(st::BitGenerator<st::Pcg64Dxsm>);
static_assert(st::BitGenerator<st::ThreefryRng>);

TEST_CASE("rng: SplitMix64 vs published seed-0 KAT", "[v12-e][stats][rng]")
{
    st::SplitMix64 g(0);
    CHECK(g.next_u64() == 0xE220A8397B1DCDAFULL);
    CHECK(g.next_u64() == 0x6E789E6AA1B965F4ULL);
    CHECK(g.next_u64() == 0x06C45D188009454FULL);
    CHECK(g.next_u64() == 0xF88BB8A8724C81ECULL);
}

TEST_CASE("rng: xoshiro256** anchor + reference; xoshiro256++ reference", "[v12-e][stats][rng]")
{
    // Independent anchor: state {1,2,3,4} ⇒ first ** output = rotl(2·5,7)·9 = 11520.
    auto a = st::Xoshiro256ss::from_state(1, 2, 3, 4);
    CHECK(a.next_u64() == 11520ULL);

    st::Xoshiro256ss g(ref_xo_seed);
    for (u64 expected : ref_xoss_out)
    {
        CHECK(g.next_u64() == expected);
    }
    st::Xoshiro256pp g2(ref_xo_seed);
    for (u64 expected : ref_xopp_out)
    {
        CHECK(g2.next_u64() == expected);
    }
}

TEST_CASE("rng: SFC64 vs NumPy random_raw (set state)", "[v12-e][stats][rng]")
{
    auto g = st::Sfc64::from_state(ref_sfc_a, ref_sfc_b, ref_sfc_c, ref_sfc_w);
    for (u64 expected : ref_sfc_out)
    {
        CHECK(g.next_u64() == expected);
    }
}

TEST_CASE("rng: PCG64-DXSM vs NumPy random_raw (set state)", "[v12-e][stats][rng]")
{
    auto g = st::Pcg64Dxsm::from_state(ref_pcg_shi, ref_pcg_slo, ref_pcg_ihi, ref_pcg_ilo);
    for (u64 expected : ref_pcg_out)
    {
        CHECK(g.next_u64() == expected);
    }
}

TEST_CASE("rng: Threefry4x64-20 vs Random123 published KAT", "[v12-e][stats][rng]")
{
    constexpr u64 zero[4] = {0, 0, 0, 0};
    constexpr auto b0 = st::threefry4x64(zero, zero); // constant-evaluated: the block function stays constexpr
    CHECK(b0.v[0] == 0x09218EBDE6C85537ULL);
    CHECK(b0.v[1] == 0x55941F5266D86105ULL);
    CHECK(b0.v[2] == 0x4BD25E16282434DCULL);
    CHECK(b0.v[3] == 0xEE29EC846BD2E40BULL);
    constexpr u64 ones[4] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
    const auto b1 = st::threefry4x64(ones, ones);
    CHECK(b1.v[0] == 0x29C24097942BBA1BULL);
    CHECK(b1.v[1] == 0x0371BBFB0F6F4E11ULL);
    CHECK(b1.v[2] == 0x3C231FFA33F83A1CULL);
    CHECK(b1.v[3] == 0xCD29113FDE32D168ULL);
    constexpr u64 pi_ctr[4] = {0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL, 0xA4093822299F31D0ULL,
                               0x082EFA98EC4E6C89ULL};
    constexpr u64 pi_key[4] = {0x452821E638D01377ULL, 0xBE5466CF34E90C6CULL, 0xBE5466CF34E90C6CULL,
                               0xC0AC29B7C97C50DDULL};
    const auto b2 = st::threefry4x64(pi_ctr, pi_key);
    CHECK(b2.v[0] == 0xA7E8FDE591651BD9ULL);
    CHECK(b2.v[1] == 0xBAAFD0C30138319BULL);
    CHECK(b2.v[2] == 0x84A5C1A729E685B9ULL);
    CHECK(b2.v[3] == 0x901D406CCEBC1BA4ULL);
    // Counter-RNG wrapper: seek(n) must equal sequential draws.
    st::ThreefryRng r(0xABCDEF, 0x12345);
    u64 seq[6];
    for (u64& x : seq)
    {
        x = r.next_u64();
    }
    st::ThreefryRng r2(0xABCDEF, 0x12345);
    r2.seek(3);
    CHECK(r2.next_u64() == seq[3]);
}

TEST_CASE("rng: Threefry seek continues the sequential stream past the sought block", "[v12-e][stats][rng]")
{
    u64 seq[40];
    st::ThreefryRng r(0xABCDEF, 0x12345);
    for (u64& x : seq)
    {
        x = r.next_u64();
    }
    for (u64 n = 0; n < 12U; ++n)
    {
        st::ThreefryRng a(0xABCDEF, 0x12345);
        a.seek(n);
        u64 drawn = 0;
        for (u64 i = n; i < 40U; ++i)
        {
            drawn += a.next_u64() == seq[i] ? 0U : 1U;
        }
        st::ThreefryRng b(0xABCDEF, 0x12345);
        b.seek(n);
        u64 filled[40];
        b.fill(crd::containers::Span<u64>(filled, 40U - n));
        u64 bulk = 0;
        for (u64 i = n; i < 40U; ++i)
        {
            bulk += filled[i - n] == seq[i] ? 0U : 1U;
        }
        INFO("seek " << n);
        CHECK(drawn == 0U);
        CHECK(bulk == 0U);
    }
}

TEST_CASE("rng: MT19937 vs canonical mt19937ar init_by_array KAT", "[v12-e][stats][rng]")
{
    const u32 key[4] = {0x123, 0x234, 0x345, 0x456};
    auto g = st::Mt19937::from_array(crd::containers::Span<const u32>(key, 4));
    for (u64 e : ref_mt_out) // NumPy legacy RandomState (init_by_array) raw output = the canonical mt19937ar KAT
    {
        CHECK(static_cast<u64>(g.next_u32()) == e);
    }
}

TEST_CASE("rng: SIMD bulk fill is bit-identical to scalar next_u64", "[v12-e][stats][rng][moat][kernels]")
{
    constexpr int n = 1003; // not a multiple of 16 ⇒ exercises the scalar tail too
    u64 buf[n];

    SECTION("every length to 40, after every partial consumption")
    {
        u64 bad = 0;
        for (u32 consumed = 0; consumed < 9U; ++consumed)
        {
            for (int len = 0; len <= 40; ++len)
            {
                st::PhiloxRng pa(11, 0xFFFFFFFFULL);
                st::PhiloxRng pb(11, 0xFFFFFFFFULL);
                pa.jump_to_block(0xFFFFFFFDULL); // the fill crosses the 2^32 word wrap of the position
                pb.jump_to_block(0xFFFFFFFDULL);
                st::ThreefryRng ta(12, 13);
                st::ThreefryRng tb(12, 13);
                ta.seek(0xFFFFFFFFFFFFFFF5ULL); // the last blocks seek reaches (the kernel case tests the carries)
                tb.seek(0xFFFFFFFFFFFFFFF5ULL);
                for (u32 c = 0; c < consumed; ++c)
                {
                    (void)pa.next_u32(); // an odd number of u32 draws leaves Philox mid-u64
                    (void)pb.next_u32();
                    (void)ta.next_u64();
                    (void)tb.next_u64();
                }
                pa.fill(crd::containers::Span<u64>(buf, static_cast<crd::usize>(len)));
                for (int i = 0; i < len; ++i)
                {
                    bad += buf[i] == pb.next_u64() ? 0U : 1U;
                }
                CHECK(pa.next_u64() == pb.next_u64()); // and both continue from the same place
                ta.fill(crd::containers::Span<u64>(buf, static_cast<crd::usize>(len)));
                for (int i = 0; i < len; ++i)
                {
                    bad += buf[i] == tb.next_u64() ? 0U : 1U;
                }
                CHECK(ta.next_u64() == tb.next_u64());
            }
        }
        CHECK(bad == 0U);
    }

    SECTION("Philox")
    {
        st::PhiloxRng a(0xABCDEF, 0x123456);
        a.fill(crd::containers::Span<u64>(buf, n));
        st::PhiloxRng b(0xABCDEF, 0x123456);
        for (u64 v : buf)
        {
            REQUIRE(v == b.next_u64());
        }
    }
    SECTION("Threefry")
    {
        st::ThreefryRng a(0xABCDEF, 0x123456);
        a.fill(crd::containers::Span<u64>(buf, n));
        st::ThreefryRng b(0xABCDEF, 0x123456);
        for (u64 v : buf)
        {
            REQUIRE(v == b.next_u64());
        }
    }
    SECTION("Philox after partial consumption")
    {
        st::PhiloxRng a(7, 9);
        st::PhiloxRng b(7, 9);
        (void)a.next_u64();
        (void)b.next_u64(); // both consume one ⇒ buffer partially drained
        a.fill(crd::containers::Span<u64>(buf, n));
        for (u64 v : buf)
        {
            REQUIRE(v == b.next_u64());
        }
    }
}

TEST_CASE("rng: determinism moat + uniform range", "[v12-e][stats][rng][moat]")
{
    st::Xoshiro256ss a(42);
    st::Xoshiro256ss b(42);
    for (int i = 0; i < 1000; ++i)
    {
        REQUIRE(a.next_u64() == b.next_u64()); // same seed ⇒ bit-identical stream
    }
    st::Pcg64Dxsm p(7);
    for (int i = 0; i < 10000; ++i)
    {
        const double u = st::next_double(p);
        REQUIRE(u >= 0.0);
        REQUIRE(u < 1.0);
    }
    // Lemire bounded stays in range.
    st::Sfc64 s(99);
    for (int i = 0; i < 10000; ++i)
    {
        REQUIRE(st::bounded(s, 1000U) < 1000U);
    }
}
