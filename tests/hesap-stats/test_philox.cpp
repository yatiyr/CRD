// crd-hesap-stats — Philox4x32-10 (the v12-pull for v7-i). Validates: (1) THE gate — the PUBLISHED Random123
// known-answer vectors (zero, all-ones, and the pi-digits counter/key lines; together they probe both round
// multipliers, both key-schedule constants, and the 10-round structure — an implementation matching all three
// cannot be coincidentally wrong); (2) counter-based PURITY (same (counter,key) twice ⇒ identical; adjacent
// counters ⇒ different blocks); (3) the PhiloxRng wrapper's self-consistency (sequential draws == the pure
// block function at successive positions; jump_to_block == sequential O(1) random access; stream separation);
// (4) uniform-conversion ranges + first/second-moment sanity on a fixed seed (deterministic, no flake);
// (5) the deterministic Fisher-Yates shuffle (permutation property + seed-reproducibility).

#include <crd/containers/array.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace st = crd::hesap::stats;

TEST_CASE("philox4x32-10: the Random123 known-answer vectors", "[hesap][stats][rng]")
{
    {
        const crd::u32 ctr[4] = {0U, 0U, 0U, 0U};
        const crd::u32 key[2] = {0U, 0U};
        const st::PhiloxBlock b = st::philox4x32(ctr, key);
        CHECK(b.v[0] == 0x6627E8D5U);
        CHECK(b.v[1] == 0xE169C58DU);
        CHECK(b.v[2] == 0xBC57AC4CU);
        CHECK(b.v[3] == 0x9B00DBD8U);
    }
    {
        const crd::u32 ctr[4] = {0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};
        const crd::u32 key[2] = {0xFFFFFFFFU, 0xFFFFFFFFU};
        const st::PhiloxBlock b = st::philox4x32(ctr, key);
        CHECK(b.v[0] == 0x408F276DU);
        CHECK(b.v[1] == 0x41C83B0EU);
        CHECK(b.v[2] == 0xA20BC7C6U);
        CHECK(b.v[3] == 0x6D5451FDU);
    }
    {
        // The pi-digits line: counter = first 16 hex digits of pi's fraction, key = the next 8.
        const crd::u32 ctr[4] = {0x243F6A88U, 0x85A308D3U, 0x13198A2EU, 0x03707344U};
        const crd::u32 key[2] = {0xA4093822U, 0x299F31D0U};
        const st::PhiloxBlock b = st::philox4x32(ctr, key);
        CHECK(b.v[0] == 0xD16CFE09U);
        CHECK(b.v[1] == 0x94FDCCEBU);
        CHECK(b.v[2] == 0x5001E420U);
        CHECK(b.v[3] == 0x24126EA1U);
    }
}

TEST_CASE("philox4x32: counter-based purity", "[hesap][stats][rng]")
{
    const crd::u32 key[2] = {0xDEADBEEFU, 0x12345678U};
    const crd::u32 c0[4] = {7U, 0U, 0U, 0U};
    const crd::u32 c1[4] = {8U, 0U, 0U, 0U};
    const st::PhiloxBlock a = st::philox4x32(c0, key);
    const st::PhiloxBlock b = st::philox4x32(c0, key);
    const st::PhiloxBlock c = st::philox4x32(c1, key);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(a.v[i] == b.v[i]); // pure function of (counter, key)
    }
    bool any_diff = false;
    for (int i = 0; i < 4; ++i)
    {
        any_diff = any_diff || (a.v[i] != c.v[i]);
    }
    CHECK(any_diff); // adjacent counters ⇒ different blocks
}

TEST_CASE("PhiloxRng: wrapper self-consistency + random access + streams", "[hesap][stats][rng]")
{
    const crd::u64 seed = 0x0123456789ABCDEFULL;
    st::PhiloxRng rng(seed, /*stream=*/3);

    // Sequential draws == the pure block function at successive block positions.
    for (crd::u64 blk = 0; blk < 4; ++blk)
    {
        const crd::u32 ctr[4] = {static_cast<crd::u32>(blk), 0U, 3U, 0U};
        const crd::u32 key[2] = {static_cast<crd::u32>(seed), static_cast<crd::u32>(seed >> 32)};
        const st::PhiloxBlock expect = st::philox4x32(ctr, key);
        for (int lane = 0; lane < 4; ++lane)
        {
            CHECK(rng.next_u32() == expect.v[lane]);
        }
    }

    // O(1) random access: jump_to_block reproduces the sequential values at that position.
    st::PhiloxRng seq(seed, 3);
    crd::u32 vals[32]; // blocks 0..7 drawn sequentially
    for (int i = 0; i < 32; ++i)
    {
        vals[i] = seq.next_u32();
    }
    st::PhiloxRng jumper(seed, 3);
    jumper.jump_to_block(5);
    CHECK(jumper.next_u32() == vals[20]); // block 5, lane 0
    jumper.jump_to_block(2);
    CHECK(jumper.next_u32() == vals[8]); // block 2, lane 0

    // Stream separation: same seed, different stream ⇒ a different sequence.
    st::PhiloxRng s0(seed, 0);
    st::PhiloxRng s1(seed, 1);
    bool any_diff = false;
    for (int i = 0; i < 8; ++i)
    {
        any_diff = any_diff || (s0.next_u32() != s1.next_u32());
    }
    CHECK(any_diff);
}

TEST_CASE("PhiloxRng: uniform conversions in range + moment sanity (fixed seed)", "[hesap][stats][rng]")
{
    st::PhiloxRng rng(42);
    crd::f64 sum = 0.0;
    crd::f64 sum_sq = 0.0;
    const int n = 100000;
    for (int i = 0; i < n; ++i)
    {
        const crd::f64 x = rng.next_f64();
        REQUIRE(x >= 0.0);
        REQUIRE(x < 1.0);
        sum += x;
        sum_sq += x * x;
    }
    const crd::f64 mean = sum / n;
    const crd::f64 var = sum_sq / n - mean * mean;
    CHECK(std::fabs(mean - 0.5) < 0.01);       // E[U(0,1)] = 1/2
    CHECK(std::fabs(var - 1.0 / 12.0) < 0.01); // Var[U(0,1)] = 1/12

    st::PhiloxRng rf(43);
    for (int i = 0; i < 10000; ++i)
    {
        const crd::f32 x = rf.next_f32();
        REQUIRE(x >= 0.0F);
        REQUIRE(x < 1.0F);
    }

    st::PhiloxRng rb(44);
    for (int i = 0; i < 10000; ++i)
    {
        REQUIRE(rb.next_below(7) < 7U);
    }
}

TEST_CASE("stats shuffle: deterministic Fisher-Yates permutation", "[hesap][stats][rng]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::usize n = 257;
    crd::containers::Array<crd::u32> a(&alloc);
    crd::containers::Array<crd::u32> b(&alloc);
    crd::containers::Array<bool> seen(&alloc);
    a.resize(n);
    b.resize(n);
    seen.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        a[i] = static_cast<crd::u32>(i);
        b[i] = static_cast<crd::u32>(i);
        seen[i] = false;
    }

    st::PhiloxRng r1(1234, 9);
    st::PhiloxRng r2(1234, 9);
    st::shuffle<crd::u32>({a.data(), n}, r1);
    st::shuffle<crd::u32>({b.data(), n}, r2);

    bool identical = true;
    bool moved = false;
    for (crd::usize i = 0; i < n; ++i)
    {
        identical = identical && (a[i] == b[i]);
        moved = moved || (a[i] != static_cast<crd::u32>(i));
        REQUIRE(a[i] < n);
        REQUIRE_FALSE(seen[a[i]]); // each element exactly once ⇒ a permutation
        seen[a[i]] = true;
    }
    CHECK(identical); // same (seed, stream) ⇒ the same permutation, bit-exact
    CHECK(moved);     // and it actually shuffles (P(identity) ≈ 1/257! — impossible)

    // A different seed gives a different permutation (overwhelmingly).
    crd::containers::Array<crd::u32> c(&alloc);
    c.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        c[i] = static_cast<crd::u32>(i);
    }
    st::PhiloxRng r3(9999, 9);
    st::shuffle<crd::u32>({c.data(), n}, r3);
    bool differs = false;
    for (crd::usize i = 0; i < n; ++i)
    {
        differs = differs || (c[i] != a[i]);
    }
    CHECK(differs);
}
