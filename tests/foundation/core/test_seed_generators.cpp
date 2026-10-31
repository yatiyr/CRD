#include <catch2/catch_test_macros.hpp>
#include <crd/core/fnv1a.hpp>
#include <crd/core/seed_generators.hpp>

// The engine's one SplitMix64, MMIX LCG step and hash combiner (capabilities random.seed-generators and hash.combine):
// the published SplitMix64 stream, and every shape the moved copies had, written out as they were, against the owner's
// composition of it over a sweep of inputs. Equal on every input, so each moved site keeps its bits.

namespace
{
// The copies as they were before the move.
crd::u64 reference_finalizer(crd::u64 z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
crd::u64 reference_step_hash(crd::u64 z) // ceir's input draw, perf's fault draw, core's workload probe, count-min seeds
{
    z += 0x9E3779B97F4A7C15ULL;
    return reference_finalizer(z);
}
crd::u64 reference_indexed(crd::u64 seed, crd::u32 i) // hesap-eigen's starting vectors
{
    return reference_finalizer(seed + (static_cast<crd::u64>(i) + 1) * 0x9E3779B97F4A7C15ULL);
}
crd::u64 reference_xor_mix(crd::u64 x) // containers' hash_u64
{
    x ^= 0x9E3779B97F4A7C15ULL;
    return reference_finalizer(x);
}
crd::u64 reference_lcg(crd::u64 st) // hesap-dense's SVD sketch, hesap-fft's sparse FFT, hesap-iterative's IDR(s)
{
    return st * 6364136223846793005ULL + 1442695040888963407ULL;
}
crd::u64 reference_combine(crd::u64 h, crd::u64 v) // lod's policy identity, scene's archetype mask hash
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
    return h;
}

// The sweep: the edges and a Weyl sequence through the whole 64-bit range.
constexpr crd::u64 kEdges[] = {0U, 1U, 2U, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
                               0x9E3779B97F4A7C15ULL};
template <typename F> void for_each_input(F f)
{
    for (const crd::u64 v : kEdges)
    {
        f(v);
    }
    crd::u64 v = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < 4096; ++i)
    {
        v += 0xD1B54A32D192ED03ULL;
        f(v);
    }
}

constexpr crd::u32 kIndices[] = {0U, 1U, 7U, 0xFFFFFFFFU};

// Output n of the stream seeded with 0, by stepping the generator.
constexpr crd::u64 stepped_output_of_seed_zero(int n)
{
    crd::u64 state = 0U;
    crd::u64 out = 0U;
    for (int i = 0; i <= n; ++i)
    {
        out = crd::splitmix64_next(state);
    }
    return out;
}
} // namespace

TEST_CASE("SplitMix64 gives the published seed-0 stream", "[core][seed-generators]")
{
    STATIC_REQUIRE(stepped_output_of_seed_zero(0) == 0xE220A8397B1DCDAFULL);
    STATIC_REQUIRE(stepped_output_of_seed_zero(1) == 0x6E789E6AA1B965F4ULL);
    STATIC_REQUIRE(stepped_output_of_seed_zero(2) == 0x06C45D188009454FULL);
    STATIC_REQUIRE(crd::splitmix64_at(0U, 0U) == 0xE220A8397B1DCDAFULL);
    STATIC_REQUIRE(crd::splitmix64_at(0U, 1U) == 0x6E789E6AA1B965F4ULL);
    STATIC_REQUIRE(crd::splitmix64_at(0U, 2U) == 0x06C45D188009454FULL);
    STATIC_REQUIRE(crd::splitmix64_mix(crd::kGoldenRatio64) == 0xE220A8397B1DCDAFULL);
    // The bare finalizer keeps 0 fixed: the reason containers' hash_u64 xors the golden ratio in first.
    STATIC_REQUIRE(crd::splitmix64_mix(0U) == 0U);
    crd::u64 state = 0U;
    CHECK(crd::splitmix64_next(state) == 0xE220A8397B1DCDAFULL);
    CHECK(state == crd::kGoldenRatio64);
}

TEST_CASE("SplitMix64's indexed output is the stream's", "[core][seed-generators]")
{
    for_each_input(
        [](crd::u64 seed)
        {
            crd::u64 state = seed;
            for (crd::u64 n = 0; n < 8U; ++n)
            {
                CHECK(crd::splitmix64_at(seed, n) == crd::splitmix64_next(state));
            }
        });
}

TEST_CASE("SplitMix64 keeps the bits of every moved shape", "[core][seed-generators]")
{
    for_each_input(
        [](crd::u64 v)
        {
            CHECK(crd::splitmix64_mix(v) == reference_finalizer(v));
            CHECK(crd::splitmix64_at(v, 0U) == reference_step_hash(v));
            CHECK(crd::splitmix64_mix(v ^ crd::kGoldenRatio64) == reference_xor_mix(v));
            for (const crd::u32 i : kIndices)
            {
                CHECK(crd::splitmix64_at(v, i) == reference_indexed(v, i));
            }
        });
}

TEST_CASE("The MMIX LCG step is Knuth's, modulo 2^64", "[core][seed-generators]")
{
    STATIC_REQUIRE(crd::kMmixLcgMultiplier == 6364136223846793005ULL);
    STATIC_REQUIRE(crd::kMmixLcgIncrement == 1442695040888963407ULL);
    crd::u64 state = 0U;
    CHECK(crd::mmix_lcg_next(state) == crd::kMmixLcgIncrement);
    CHECK(state == crd::kMmixLcgIncrement);
    // The all-ones state wraps: (2^64 - 1) * a + c = c - a (mod 2^64).
    state = 0xFFFFFFFFFFFFFFFFULL;
    CHECK(crd::mmix_lcg_next(state) == crd::kMmixLcgIncrement - crd::kMmixLcgMultiplier);
    for_each_input(
        [](crd::u64 v)
        {
            crd::u64 state_owner = v;
            crd::u64 state_reference = v;
            for (int i = 0; i < 4; ++i)
            {
                state_reference = reference_lcg(state_reference);
                CHECK(crd::mmix_lcg_next(state_owner) == state_reference);
            }
            CHECK(state_owner == state_reference);
        });
}

TEST_CASE("hash_combine is the golden-ratio combiner", "[core][seed-generators]")
{
    STATIC_REQUIRE(crd::hash_combine(0U, 0U) == crd::kGoldenRatio64);
    STATIC_REQUIRE(crd::hash_combine(0U, 5U) == crd::kGoldenRatio64 + 5U);
    // Order matters: a key folded from (a, b) differs from (b, a).
    CHECK(crd::hash_combine(crd::hash_combine(0U, 1U), 2U) != crd::hash_combine(crd::hash_combine(0U, 2U), 1U));
    for_each_input(
        [](crd::u64 v)
        {
            for (const crd::u64 h : kEdges)
            {
                CHECK(crd::hash_combine(h, v) == reference_combine(h, v));
                CHECK(crd::hash_combine(v, h) == reference_combine(v, h));
            }
        });
}
