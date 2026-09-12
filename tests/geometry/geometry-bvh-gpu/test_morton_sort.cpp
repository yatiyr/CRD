// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-b1 -- CPU stable LSD radix sort of Morton-code
// pairs. Tests follow the v9-prereq-test-harness + advisor-TDD discipline:
//
//   1. CALIBRATION FIRST -- hand-rolled u32 input with manually-computed
//      expected sorted output. If this fails, the radix is broken and
//      every downstream test is meaningless.
//   2. Trivial-shape sanity -- empty / single / already-sorted / reverse.
//   3. STABILITY discriminator -- all-equal keys preserve input index
//      order (the phase contract: "equal Morton codes => lower input
//      index wins").
//   4. Bullet-proof oracle -- random N=10000 cross-checked against
//      `crd::containers::sort` with a lexicographic `(code, index)`
//      comparator. Byte-identical output is the radix correctness proof.
//   5. Determinism -- same input, two runs, byte-identical results.
//   6. Pair-integrity sieve -- every output index is valid, no dupes,
//      no drops.
//   7. u64 suite -- same shape exercises (mirrored).
//   8. u64-specific upper-passes discriminator -- pairs share low 32 bits
//      but differ in upper 32. If passes 5-8 weren't wired up, this test
//      would silently fail.
//   9. CRD_PERF_BUDGET_LE 1M -- tiered debug vs NDEBUG per
//      feedback_v9_gpu_sanity_harness.
//  10. Integration with compute_morton_codes_cpu -- end-to-end LBVH
//      pipeline candidate (AABBs -> codes -> sorted pairs).
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/bvh_gpu/morton_sort.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/measure.hpp>

#include <cstdint>
#include <cstring>
#include <random>

namespace
{

using crd::geometry::bvh_gpu::MortonPair;
using crd::geometry::bvh_gpu::sort_morton_pairs;

// Helper: pack a std::initializer_list of codes into a TlsfAllocator-bound
// Array. Tests use this so input lifetime + allocator binding match the
// shipping code path.
template <typename KeyT>
crd::containers::Array<KeyT> codes_array(std::initializer_list<KeyT> in,
                                          crd::memory::IAllocator* alloc)
{
    crd::containers::Array<KeyT> out(alloc);
    out.reserve(in.size());
    for (KeyT k : in)
    {
        out.push_back(k);
    }
    return out;
}

template <typename KeyT>
crd::containers::ConstSpan<KeyT> view_of(const crd::containers::Array<KeyT>& a)
{
    return crd::containers::ConstSpan<KeyT>{a.data(), a.size()};
}

} // namespace

// =========================================================================
// CALIBRATION FIRST -- hand-rolled u32 input with manually-computed sort
// =========================================================================

TEST_CASE("sort_morton_pairs u32 calibration: 5 hand-rolled pairs sort + stable",
          "[sort][cpu][calibration]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // Input codes: [5, 2, 8, 2, 1] at indices [0, 1, 2, 3, 4].
    // Sorted ascending by code, equal-code pairs in ascending index order:
    //   1 -> {1, 4}
    //   2 -> {2, 1} (lower input index than {2,3} -- the stability discriminator)
    //   2 -> {2, 3}
    //   5 -> {5, 0}
    //   8 -> {8, 2}
    const auto in  = codes_array<crd::u32>({5U, 2U, 8U, 2U, 1U}, &alloc);
    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);

    REQUIRE(out.size() == 5U);

    CHECK(out[0].code  == 1U);   CHECK(out[0].index == 4U);
    CHECK(out[1].code  == 2U);   CHECK(out[1].index == 1U);   // stable
    CHECK(out[2].code  == 2U);   CHECK(out[2].index == 3U);   // stable
    CHECK(out[3].code  == 5U);   CHECK(out[3].index == 0U);
    CHECK(out[4].code  == 8U);   CHECK(out[4].index == 2U);
}

// =========================================================================
// Trivial-shape sanity
// =========================================================================

TEST_CASE("sort_morton_pairs u32 empty input -> empty output", "[sort][cpu]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::ConstSpan<crd::u32> empty{};
    const auto out = sort_morton_pairs<crd::u32>(empty, &alloc);
    CHECK(out.empty());
}

TEST_CASE("sort_morton_pairs u32 single element -> {code, 0}", "[sort][cpu]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    const auto in  = codes_array<crd::u32>({42U}, &alloc);
    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(out.size() == 1U);
    CHECK(out[0].code  == 42U);
    CHECK(out[0].index == 0U);
}

TEST_CASE("sort_morton_pairs u32 already-sorted ascending: identity (with ascending indices)",
          "[sort][cpu]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    const auto in  = codes_array<crd::u32>({1U, 2U, 3U, 4U, 5U}, &alloc);
    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(out.size() == 5U);
    for (crd::u32 i = 0; i < 5U; ++i)
    {
        CHECK(out[i].code  == i + 1U);
        CHECK(out[i].index == i);
    }
}

TEST_CASE("sort_morton_pairs u32 reverse-sorted input is correctly sorted",
          "[sort][cpu]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    const auto in  = codes_array<crd::u32>({5U, 4U, 3U, 2U, 1U}, &alloc);
    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(out.size() == 5U);
    CHECK(out[0].code == 1U); CHECK(out[0].index == 4U);
    CHECK(out[1].code == 2U); CHECK(out[1].index == 3U);
    CHECK(out[2].code == 3U); CHECK(out[2].index == 2U);
    CHECK(out[3].code == 4U); CHECK(out[3].index == 1U);
    CHECK(out[4].code == 5U); CHECK(out[4].index == 0U);
}

// =========================================================================
// STABILITY discriminator -- all-equal keys preserve input index order
// =========================================================================

TEST_CASE("sort_morton_pairs u32 all-equal keys: index ordering preserved (stable)",
          "[sort][cpu][stability]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    const auto in  = codes_array<crd::u32>(
        {42U, 42U, 42U, 42U, 42U, 42U, 42U, 42U}, &alloc);
    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(out.size() == 8U);
    for (crd::u32 i = 0; i < 8U; ++i)
    {
        CHECK(out[i].code  == 42U);
        CHECK(out[i].index == i); // monotonic-ascending -- the phase contract
    }
}

TEST_CASE("sort_morton_pairs u32 partial-tie keys: ties resolved by input index",
          "[sort][cpu][stability]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // Pattern: alternating 7s and 3s. After sort, all 3s first (with their
    // original indices in ascending order), then all 7s (likewise).
    const auto in  = codes_array<crd::u32>(
        {7U, 3U, 7U, 3U, 7U, 3U, 7U, 3U}, &alloc);
    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(out.size() == 8U);

    // First four are 3s at original indices 1, 3, 5, 7.
    CHECK(out[0].code == 3U); CHECK(out[0].index == 1U);
    CHECK(out[1].code == 3U); CHECK(out[1].index == 3U);
    CHECK(out[2].code == 3U); CHECK(out[2].index == 5U);
    CHECK(out[3].code == 3U); CHECK(out[3].index == 7U);

    // Last four are 7s at original indices 0, 2, 4, 6.
    CHECK(out[4].code == 7U); CHECK(out[4].index == 0U);
    CHECK(out[5].code == 7U); CHECK(out[5].index == 2U);
    CHECK(out[6].code == 7U); CHECK(out[6].index == 4U);
    CHECK(out[7].code == 7U); CHECK(out[7].index == 6U);
}

// =========================================================================
// Bullet-proof oracle: random 10000, cross-check against crd::containers::sort
// =========================================================================

namespace
{

template <typename KeyT>
[[nodiscard]] crd::containers::Array<MortonPair<KeyT>>
oracle_sort_pairs(crd::containers::ConstSpan<KeyT> codes,
                  crd::memory::IAllocator* alloc)
{
    // Reference implementation: build pairs, sort by (code, index)
    // lexicographic. The lexicographic comparator self-documents the
    // stability contract -- same output as sort-by-code-only on input
    // pre-ordered by index, by construction.
    crd::containers::Array<MortonPair<KeyT>> out(alloc);
    out.resize(codes.size());
    for (crd::usize i = 0U; i < codes.size(); ++i)
    {
        out[i].code  = codes[i];
        out[i].index = static_cast<crd::u32>(i);
    }
    crd::containers::sort(out.data(), out.data() + out.size(),
        [](const MortonPair<KeyT>& a, const MortonPair<KeyT>& b)
        {
            if (a.code != b.code) return a.code < b.code;
            return a.index < b.index;
        });
    return out;
}

} // namespace

TEST_CASE("sort_morton_pairs u32 random 10000: byte-identical to oracle",
          "[sort][cpu][oracle]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    constexpr crd::usize n = 10000U;
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(n);

    std::mt19937 rng(0xCEEDCEEDU);
    // Mix of unique-ish codes and forced ties (mod a small number) to
    // exercise both pure-LSD-radix correctness and stability under ties.
    std::uniform_int_distribution<crd::u32> dist(0U, 4095U);
    for (crd::usize i = 0; i < n; ++i) { in[i] = dist(rng); }

    const auto codes_span = view_of(in);
    const auto got      = sort_morton_pairs<crd::u32>(codes_span, &alloc);
    const auto expected = oracle_sort_pairs<crd::u32>(codes_span, &alloc);

    REQUIRE(got.size() == expected.size());
    // Byte-identical (memcmp over the AoS pair array). If any code or index
    // differs, the radix is broken or unstable.
    CHECK(std::memcmp(got.data(), expected.data(),
                      n * sizeof(MortonPair<crd::u32>)) == 0);
}

// =========================================================================
// Determinism: same input twice, byte-identical results
// =========================================================================

TEST_CASE("sort_morton_pairs u32 determinism: byte-identical across runs",
          "[sort][cpu][determinism]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    constexpr crd::usize n = 8192U;
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(n);
    std::mt19937 rng(0xD37E12CEU);
    for (crd::usize i = 0; i < n; ++i) { in[i] = static_cast<crd::u32>(rng()); }

    const auto a = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    const auto b = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(),
                      n * sizeof(MortonPair<crd::u32>)) == 0);
}

// =========================================================================
// Pair-integrity sieve: every output index is valid, no dupes, no drops
// =========================================================================

TEST_CASE("sort_morton_pairs u32 pair integrity: indices form a permutation",
          "[sort][cpu][integrity]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    constexpr crd::usize n = 5000U;
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(n);
    std::mt19937 rng(0xA5A5A5A5U);
    for (crd::usize i = 0; i < n; ++i) { in[i] = static_cast<crd::u32>(rng()); }

    const auto out = sort_morton_pairs<crd::u32>(view_of(in), &alloc);
    REQUIRE(out.size() == n);

    // Sieve: every index in [0, N) must appear exactly once.
    crd::containers::Array<crd::u8> seen(&alloc);
    seen.resize(n, crd::u8{0});
    for (const auto& p : out)
    {
        REQUIRE(p.index < n);
        REQUIRE(seen[p.index] == 0U);   // no duplicate
        seen[p.index] = 1U;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        CHECK(seen[i] == 1U);            // no drop
    }

    // Output is sorted ascending: for every consecutive pair, code[i] <= code[i+1].
    for (crd::usize i = 1; i < out.size(); ++i)
    {
        CHECK(out[i - 1].code <= out[i].code);
    }
}

// =========================================================================
// u64 suite (mirrored)
// =========================================================================

TEST_CASE("sort_morton_pairs u64 calibration: 5 hand-rolled pairs sort + stable",
          "[sort][cpu][calibration][u64]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const auto in  = codes_array<std::uint64_t>(
        {5ULL, 2ULL, 8ULL, 2ULL, 1ULL}, &alloc);
    const auto out = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);

    REQUIRE(out.size() == 5U);
    CHECK(out[0].code == 1ULL); CHECK(out[0].index == 4U);
    CHECK(out[1].code == 2ULL); CHECK(out[1].index == 1U);
    CHECK(out[2].code == 2ULL); CHECK(out[2].index == 3U);
    CHECK(out[3].code == 5ULL); CHECK(out[3].index == 0U);
    CHECK(out[4].code == 8ULL); CHECK(out[4].index == 2U);
}

TEST_CASE("sort_morton_pairs u64 empty and single", "[sort][cpu][u64]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    {
        crd::containers::ConstSpan<std::uint64_t> empty{};
        const auto out = sort_morton_pairs<std::uint64_t>(empty, &alloc);
        CHECK(out.empty());
    }
    {
        const auto in  = codes_array<std::uint64_t>({0xDEADBEEFCAFEBABEULL}, &alloc);
        const auto out = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
        REQUIRE(out.size() == 1U);
        CHECK(out[0].code  == 0xDEADBEEFCAFEBABEULL);
        CHECK(out[0].index == 0U);
    }
}

TEST_CASE("sort_morton_pairs u64 all-equal preserves index order",
          "[sort][cpu][stability][u64]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    const auto in = codes_array<std::uint64_t>(
        {0xFEEDFACEFEEDFACEULL, 0xFEEDFACEFEEDFACEULL, 0xFEEDFACEFEEDFACEULL,
         0xFEEDFACEFEEDFACEULL}, &alloc);
    const auto out = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
    REQUIRE(out.size() == 4U);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK(out[i].code  == 0xFEEDFACEFEEDFACEULL);
        CHECK(out[i].index == i);
    }
}

// -- u64-specific upper-passes discriminator --
//
// Three pairs that share the LOW 32 bits exactly (0xFFFFFFFFULL) but differ
// in the UPPER 32 bits (0x1, 0x3, 0x2). If passes 5-8 of the radix were
// wired wrong -- e.g. a `shift = pass * 8` arithmetic mistake -- the upper
// bytes would be ignored and all three pairs would tie on the low 32 bits.
// This test FAILS loudly under any upper-bit bug.
TEST_CASE("sort_morton_pairs u64 upper-32-bits discriminator: passes 5-8 wired correctly",
          "[sort][cpu][u64][upper-bits]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    const auto in = codes_array<std::uint64_t>(
        {(std::uint64_t{0x1ULL} << 32U) | 0xFFFFFFFFULL,   // index 0, upper=0x1
         (std::uint64_t{0x3ULL} << 32U) | 0xFFFFFFFFULL,   // index 1, upper=0x3
         (std::uint64_t{0x2ULL} << 32U) | 0xFFFFFFFFULL},  // index 2, upper=0x2
        &alloc);
    const auto out = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
    REQUIRE(out.size() == 3U);
    // Sorted by upper bits: 0x1 < 0x2 < 0x3 -> indices [0, 2, 1].
    CHECK(out[0].index == 0U);
    CHECK(out[1].index == 2U);
    CHECK(out[2].index == 1U);
}

TEST_CASE("sort_morton_pairs u64 random 10000: byte-identical to oracle",
          "[sort][cpu][oracle][u64]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    constexpr crd::usize n = 10000U;
    crd::containers::Array<std::uint64_t> in(&alloc);
    in.resize(n);

    std::mt19937_64 rng(0xC0DEC0DEC0DEC0DEULL);
    // Mix full-range u64 with forced-low-32-bit ties to exercise both
    // upper-byte passes + stability.
    for (crd::usize i = 0; i < n; ++i)
    {
        if ((i & 0x7U) == 0U)
        {
            // Force the low 32 bits to a small set -- upper bits drive order.
            in[i] = (rng() << 32U) | (i & 0xFFU);
        }
        else
        {
            in[i] = rng();
        }
    }

    const auto got      = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
    const auto expected = oracle_sort_pairs<std::uint64_t>(view_of(in), &alloc);

    REQUIRE(got.size() == expected.size());
    CHECK(std::memcmp(got.data(), expected.data(),
                      n * sizeof(MortonPair<std::uint64_t>)) == 0);
}

TEST_CASE("sort_morton_pairs u64 determinism", "[sort][cpu][determinism][u64]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    constexpr crd::usize n = 8192U;
    crd::containers::Array<std::uint64_t> in(&alloc);
    in.resize(n);
    std::mt19937_64 rng(0xBEEFBEEFBEEFBEEFULL);
    for (crd::usize i = 0; i < n; ++i) { in[i] = rng(); }

    const auto a = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
    const auto b = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(),
                      n * sizeof(MortonPair<std::uint64_t>)) == 0);
}

TEST_CASE("sort_morton_pairs u64 pair integrity", "[sort][cpu][integrity][u64]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    constexpr crd::usize n = 5000U;
    crd::containers::Array<std::uint64_t> in(&alloc);
    in.resize(n);
    std::mt19937_64 rng(0xAA55AA55AA55AA55ULL);
    for (crd::usize i = 0; i < n; ++i) { in[i] = rng(); }

    const auto out = sort_morton_pairs<std::uint64_t>(view_of(in), &alloc);
    REQUIRE(out.size() == n);

    crd::containers::Array<crd::u8> seen(&alloc);
    seen.resize(n, crd::u8{0});
    for (const auto& p : out)
    {
        REQUIRE(p.index < n);
        REQUIRE(seen[p.index] == 0U);
        seen[p.index] = 1U;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        CHECK(seen[i] == 1U);
    }

    for (crd::usize i = 1; i < out.size(); ++i)
    {
        CHECK(out[i - 1].code <= out[i].code);
    }
}

// =========================================================================
// Perf budget -- 1M elements; tiered debug vs NDEBUG per
// feedback_v9_gpu_sanity_harness ("Don't ship a budget you'll trip on
// win-debug"). NDEBUG budget is realistic-shipping with ~4x headroom; debug
// budget is safety-net for iterator-checks + debug-allocator overhead.
// =========================================================================

TEST_CASE("sort_morton_pairs u32 perf budget: 1M elements within tiered budget",
          "[sort][cpu][perf]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U * 1024U);
    constexpr crd::usize n = 1U * 1000U * 1000U;
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(n);
    std::mt19937 rng(0x12345678U);
    for (crd::usize i = 0; i < n; ++i) { in[i] = static_cast<crd::u32>(rng()); }
    const auto codes_span = view_of(in);

    #ifdef NDEBUG
        constexpr double budget_ms = 20.0;
    #else
        constexpr double budget_ms = 2000.0;
    #endif
    CRD_PERF_BUDGET_LE("sort_morton_pairs_u32_1m", budget_ms, [&]{
        const auto out = sort_morton_pairs<crd::u32>(codes_span, &alloc);
        REQUIRE(out.size() == n);
    });
    (void)budget_ms; // CRD_ASSERT_MSG compiles out under NDEBUG; silence C4189.
}

TEST_CASE("sort_morton_pairs u64 perf budget: 1M elements within tiered budget",
          "[sort][cpu][perf][u64]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U * 1024U);
    constexpr crd::usize n = 1U * 1000U * 1000U;
    crd::containers::Array<std::uint64_t> in(&alloc);
    in.resize(n);
    std::mt19937_64 rng(0x87654321DEADBEEFULL);
    for (crd::usize i = 0; i < n; ++i) { in[i] = rng(); }
    const auto codes_span = view_of(in);

    #ifdef NDEBUG
        constexpr double budget_ms = 40.0;
    #else
        constexpr double budget_ms = 4000.0;
    #endif
    CRD_PERF_BUDGET_LE("sort_morton_pairs_u64_1m", budget_ms, [&]{
        const auto out = sort_morton_pairs<std::uint64_t>(codes_span, &alloc);
        REQUIRE(out.size() == n);
    });
    (void)budget_ms;
}

// =========================================================================
// End-to-end integration: compute_morton_codes_cpu -> sort_morton_pairs.
// Mirrors the real LBVH pipeline order (v9a-a -> v9a-b1).
// =========================================================================

TEST_CASE("sort_morton_pairs integrates with compute_morton_codes_cpu",
          "[sort][cpu][integration]")
{
    using crd::geometry::bvh_gpu::compute_morton_codes_cpu;
    using crd::geometry::primitives::AABB3;
    using crd::math::Vec3;

    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    // 8 corner AABBs in the unit cube -- same shape as the morton calibration.
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    for (int iz = 0; iz < 2; ++iz)
        for (int iy = 0; iy < 2; ++iy)
            for (int ix = 0; ix < 2; ++ix)
            {
                Vec3<crd::f32> c{
                    ix == 0 ? 0.0F : 0.999F,
                    iy == 0 ? 0.0F : 0.999F,
                    iz == 0 ? 0.0F : 0.999F,
                };
                aabbs.push_back({c, c});
            }

    const auto codes = compute_morton_codes_cpu(
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size()),
        &alloc);
    REQUIRE(codes.size() == 8U);

    const auto pairs = sort_morton_pairs<crd::u32>(view_of(codes), &alloc);
    REQUIRE(pairs.size() == 8U);

    // Codes monotonic ascending.
    for (crd::usize i = 1; i < pairs.size(); ++i)
    {
        CHECK(pairs[i - 1].code <= pairs[i].code);
    }

    // Indices form a permutation of [0, 8).
    crd::containers::Array<crd::u8> seen(&alloc);
    seen.resize(8U, crd::u8{0});
    for (const auto& p : pairs)
    {
        REQUIRE(p.index < 8U);
        seen[p.index] = 1U;
    }
    for (int i = 0; i < 8; ++i) { CHECK(seen[i] == 1U); }
}
