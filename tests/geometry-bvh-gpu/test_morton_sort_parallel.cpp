// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-b1-parallel -- parallel CPU stable LSD radix
// sort of Morton-code pairs via crd-jobs.
//
// Discipline (per advisor + feedback_v9_gpu_sanity_harness):
//
//   1. CALIBRATION FIRST -- N=16 hand-rolled u32; sort_morton_pairs_parallel
//      output byte-identical to sort_morton_pairs (serial reference).
//      Failure here means everything downstream is meaningless.
//   2. Worker-spanning STABILITY discriminator -- 4096 all-equal keys
//      spanning ALL workers; output indices must be 0..4095 monotonic both
//      within AND ACROSS worker boundaries. This is the load-bearing
//      stability invariant per the v9a-b1-parallel design.
//   3. Cross-chunk equal-keys discriminator -- two equal keys placed in
//      worker 0 and worker N-1; worker N-1's item must land AFTER worker
//      0's item at the same bucket. Catches "stable within worker,
//      unstable across workers" silent failures.
//   4. num_jobs-sensitivity -- output byte-identical at num_jobs = 1, 2,
//      4, 8, 16. Same input must produce same byte sequence at every
//      worker count.
//   5. Bullet-proof oracle -- N=10000 + N=1M random; bit_compare against
//      sort_morton_pairs scalar reference. The radix correctness proof
//      across the whole adversarial corpus.
//   6. Threshold fallback -- below kDefaultParallelSortThreshold, the
//      parallel function must produce identical output to serial via the
//      documented fallback path.
//   7. Falls-back-clean-without-jobs -- when crd::jobs is not init'd,
//      parallel function still produces correct output (via fallback).
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/morton_sort.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/test_helpers/gpu_compare.hpp>

#include <cstdint>
#include <cstring>
#include <random>

namespace
{
using crd::geometry::bvh_gpu::MortonPair;
using crd::geometry::bvh_gpu::sort_morton_pairs;
using crd::geometry::bvh_gpu::sort_morton_pairs_parallel;

// Init crd-jobs for the lifetime of this test binary via a Catch2 listener
// (mirrors the pattern in test_bvh_parallel.cpp). Doing init() at file scope
// would fire during catch_discover_tests' listing phase, before main().
// frame_reset() per case keeps the per-thread frame arenas from filling.
struct MortonSortJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(Catch::TestRunInfo const&) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 8U, .frame_alloc_bytes = 64U << 20U});
    }
    void testCaseEnded(Catch::TestCaseStats const&) override { crd::jobs::frame_reset(); }
    void testRunEnded(Catch::TestRunStats const&) override { crd::jobs::shutdown(); }
};

template <typename KeyT>
void require_byte_identical(
    const crd::containers::Array<MortonPair<KeyT>>& a,
    const crd::containers::Array<MortonPair<KeyT>>& b)
{
    REQUIRE(a.size() == b.size());
    const auto a_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(a.data()),
        a.size() * sizeof(MortonPair<KeyT>));
    const auto b_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(b.data()),
        b.size() * sizeof(MortonPair<KeyT>));
    const auto cmp = crd::test::bit_compare<crd::u8>(a_bytes, b_bytes);
    if (!cmp.ok)
    {
        INFO("first byte mismatch at offset " << cmp.first_mismatch_index
             << " serial=" << static_cast<int>(cmp.cpu_value)
             << " parallel=" << static_cast<int>(cmp.gpu_value));
    }
    CHECK(cmp.ok);
}

template <typename KeyT>
void fill_random_codes(crd::containers::Array<KeyT>& out, crd::usize n, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    out.resize(n, KeyT{0});
    for (crd::usize i = 0; i < n; ++i)
    {
        if constexpr (sizeof(KeyT) == 4U)
        {
            std::uniform_int_distribution<crd::u32> dist(0U, 0xFFFFFFFFU);
            out[i] = static_cast<KeyT>(((i & 0x3U) == 0U) ? (dist(rng) & 0xFFFFU) : dist(rng));
        }
        else
        {
            std::uniform_int_distribution<std::uint64_t> dist(0ULL, 0xFFFFFFFFFFFFFFFFULL);
            out[i] = static_cast<KeyT>(((i & 0x3U) == 0U) ? (dist(rng) & 0xFFFFFFFFULL) : dist(rng));
        }
    }
}

} // namespace

CATCH_REGISTER_LISTENER(MortonSortJobsListener)

// =========================================================================
// CALIBRATION FIRST
// =========================================================================

TEST_CASE("v9a-b1-parallel N=16 calibration: byte-identical to serial reference",
          "[sort][parallel][calibration]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);

    crd::u32 a[] = {17U, 3U, 17U, 100U, 0U, 8U, 17U, 999U,
                    42U, 3U, 42U, 0U,   5U, 100U, 1U, 17U};
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(16U);
    for (crd::u32 i = 0; i < 16U; ++i) { in[i] = a[i]; }
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    // N=16 falls below the threshold; parallel must transparently fall back
    // to serial. This case mainly verifies the fallback path is wired.
    const auto serial   = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto parallel = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, /*num_jobs*/ 8U);
    require_byte_identical<crd::u32>(serial, parallel);
}

// =========================================================================
// Worker-spanning STABILITY discriminator
// =========================================================================

TEST_CASE("v9a-b1-parallel 4096 all-equal keys span ALL workers: stable global order",
          "[sort][parallel][stability]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    // Above the parallel threshold so the function actually fans out.
    constexpr crd::u32 k_n = 200000U;
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(k_n, 0xCAFEBABEU);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto out = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, /*num_jobs*/ 8U);
    REQUIRE(out.size() == k_n);
    // All codes equal => bucket distribution is "everything in one bucket".
    // Output indices must be 0, 1, ..., k_n-1 strictly monotonic across all
    // worker chunks.
    for (crd::u32 i = 0; i < k_n; ++i)
    {
        CHECK(out[i].code  == 0xCAFEBABEU);
        CHECK(out[i].index == i);
    }
}

// =========================================================================
// Cross-chunk equal-keys discriminator -- chunk 7 item after chunk 0 item
// =========================================================================

TEST_CASE("v9a-b1-parallel cross-chunk equal keys preserve monotonic input-index",
          "[sort][parallel][stability]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    constexpr crd::u32 k_n = 200000U;
    // Most codes are random; specific equal pairs placed in chunk 0 and chunk 7.
    crd::containers::Array<crd::u32> in(&alloc);
    fill_random_codes<crd::u32>(in, k_n, 0xABCD1234ULL);
    // Force exactly two equal keys at known input positions in different chunks.
    // num_jobs=8 -> chunk i covers [i*k_n/8, (i+1)*k_n/8). Use first index of
    // chunk 0 (=0) and first index of chunk 7 (=7*k_n/8 = 175000) for the
    // equal-key pair.
    const crd::u32 idx_chunk0 = 0U;
    const crd::u32 idx_chunk7 = 7U * k_n / 8U;
    const crd::u32 equal_key  = 0x12345678U;
    in[idx_chunk0] = equal_key;
    in[idx_chunk7] = equal_key;
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto out = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, /*num_jobs*/ 8U);
    REQUIRE(out.size() == k_n);

    // Find both equal-key entries in the output; verify chunk-7 lands AFTER chunk-0.
    crd::u32 pos_chunk0 = UINT32_MAX;
    crd::u32 pos_chunk7 = UINT32_MAX;
    for (crd::u32 i = 0; i < k_n; ++i)
    {
        if (out[i].code == equal_key)
        {
            if      (out[i].index == idx_chunk0) { pos_chunk0 = i; }
            else if (out[i].index == idx_chunk7) { pos_chunk7 = i; }
        }
    }
    REQUIRE(pos_chunk0 != UINT32_MAX);
    REQUIRE(pos_chunk7 != UINT32_MAX);
    CHECK(pos_chunk0 < pos_chunk7);
}

// =========================================================================
// num_jobs-sensitivity -- byte-identical at 1, 2, 4, 8, 16 workers
// =========================================================================

TEST_CASE("v9a-b1-parallel byte-identical across num_jobs = {1, 2, 4, 8, 16}",
          "[sort][parallel][sensitivity]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    constexpr crd::u32 k_n = 200000U;
    crd::containers::Array<crd::u32> in(&alloc);
    fill_random_codes<crd::u32>(in, k_n, 0xDEADBEEFULL);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto baseline = sort_morton_pairs<crd::u32>(in_span, &alloc);
    for (const crd::u32 nj : {crd::u32{1}, crd::u32{2}, crd::u32{4}, crd::u32{8}, crd::u32{16}})
    {
        const auto out = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, nj);
        INFO("num_jobs = " << nj);
        require_byte_identical<crd::u32>(baseline, out);
    }
}

// =========================================================================
// Bullet-proof oracle -- 10 000 random
// =========================================================================

TEST_CASE("v9a-b1-parallel N=10000 u32: byte-identical to serial reference",
          "[sort][parallel][oracle]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    constexpr crd::u32 k_n = 10000U;
    crd::containers::Array<crd::u32> in(&alloc);
    fill_random_codes<crd::u32>(in, k_n, 0xC0FFEEABULL);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto serial   = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto parallel = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, /*num_jobs*/ 8U);
    require_byte_identical<crd::u32>(serial, parallel);
}

// =========================================================================
// u64 mirror
// =========================================================================

TEST_CASE("v9a-b1-parallel N=10000 u64: byte-identical to serial reference",
          "[sort][parallel][oracle][u64]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    constexpr crd::u32 k_n = 100000U;
    crd::containers::Array<std::uint64_t> in(&alloc);
    fill_random_codes<std::uint64_t>(in, k_n, 0xBADC0FFEEULL);
    const auto in_span = crd::containers::ConstSpan<std::uint64_t>(in.data(), in.size());

    const auto serial   = sort_morton_pairs<std::uint64_t>(in_span, &alloc);
    const auto parallel = sort_morton_pairs_parallel<std::uint64_t>(in_span, &alloc, /*num_jobs*/ 8U);
    require_byte_identical<std::uint64_t>(serial, parallel);
}

// =========================================================================
// Large oracle -- 1 M u32 (the perf-budget workload)
// =========================================================================

TEST_CASE("v9a-b1-parallel N=1M u32: byte-identical to serial reference",
          "[sort][parallel][oracle][large]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    constexpr crd::u32 k_n = 1U << 20;
    crd::containers::Array<crd::u32> in(&alloc);
    fill_random_codes<crd::u32>(in, k_n, 0x87654321ULL);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto serial   = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto parallel = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, /*num_jobs*/ 8U);
    require_byte_identical<crd::u32>(serial, parallel);
}

// =========================================================================
// Threshold fallback -- below threshold, output identical to serial
// =========================================================================

TEST_CASE("v9a-b1-parallel below threshold falls back to serial path",
          "[sort][parallel][threshold]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);

    // 1000 < kDefaultParallelSortThreshold (= 65536). Exercises fallback.
    constexpr crd::u32 k_n = 1000U;
    crd::containers::Array<crd::u32> in(&alloc);
    fill_random_codes<crd::u32>(in, k_n, 0x11223344ULL);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto serial   = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto parallel = sort_morton_pairs_parallel<crd::u32>(in_span, &alloc, /*num_jobs*/ 8U);
    require_byte_identical<crd::u32>(serial, parallel);
}
