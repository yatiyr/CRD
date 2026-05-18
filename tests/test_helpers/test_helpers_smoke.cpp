// ---------------------------------------------------------------------------
// crd-test-helpers smoke — exercises the CPU-only helpers introduced in
// Phase 3.1.7 v9-prereq-test-harness (2026-05-18):
//   - crd::test::ulp_compare<float>
//   - crd::test::bit_compare<int>
//   - crd::test::gpu_determinism_check (with mock dispatch)
//   - crd::perf::measure_ms + CRD_PERF_BUDGET_LE
//
// ValidationCapture (the 5th deliverable) requires a live VkInstance and
// is exercised by GPU smoke tests in the upcoming v9 GPU slices, not
// here — keeping this binary headless preserves CTest-on-CI runnability.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/measure.hpp>
#include <crd/test_helpers/gpu_compare.hpp>
#include <crd/test_helpers/gpu_determinism.hpp>

#include <chrono>
#include <cmath>
#include <thread>

TEST_CASE("ulp_compare matches identical f32 spans", "[test_helpers][ulp]")
{
    const float a[] = {1.0F, 2.0F, 3.0F, -4.5F, 0.0F};
    const float b[] = {1.0F, 2.0F, 3.0F, -4.5F, 0.0F};
    const auto r    = crd::test::ulp_compare(
        crd::containers::ConstSpan<float>{a, 5},
        crd::containers::ConstSpan<float>{b, 5});
    REQUIRE(r.ok);
    REQUIRE(r.compared_count == 5);
}

TEST_CASE("ulp_compare detects 1-ULP drift", "[test_helpers][ulp]")
{
    float       a = 1.0F;
    crd::u32    a_bits{};
    std::memcpy(&a_bits, &a, sizeof(a_bits));
    const crd::u32 b_bits = a_bits + 1U; // 1 ULP above 1.0F
    float          b{};
    std::memcpy(&b, &b_bits, sizeof(b));

    const float cpu[] = {a};
    const float gpu[] = {b};

    // max_ulp = 0 → fails.
    const auto r0 = crd::test::ulp_compare(
        crd::containers::ConstSpan<float>{cpu, 1},
        crd::containers::ConstSpan<float>{gpu, 1},
        /*max_ulp*/ 0U);
    REQUIRE_FALSE(r0.ok);
    REQUIRE(r0.ulp_diff == 1);
    REQUIRE(r0.first_mismatch_index == 0);

    // max_ulp = 1 → passes.
    const auto r1 = crd::test::ulp_compare(
        crd::containers::ConstSpan<float>{cpu, 1},
        crd::containers::ConstSpan<float>{gpu, 1},
        /*max_ulp*/ 1U);
    REQUIRE(r1.ok);
}

TEST_CASE("ulp_compare handles +0/-0 as equal", "[test_helpers][ulp]")
{
    const float a[] = { 0.0F };
    const float b[] = { -0.0F };
    const auto r = crd::test::ulp_compare(
        crd::containers::ConstSpan<float>{a, 1},
        crd::containers::ConstSpan<float>{b, 1},
        /*max_ulp*/ 0U);
    REQUIRE(r.ok);
}

TEST_CASE("ulp_compare rejects NaN", "[test_helpers][ulp]")
{
    const float a[] = { std::nanf("") };
    const float b[] = { std::nanf("") };
    const auto r = crd::test::ulp_compare(
        crd::containers::ConstSpan<float>{a, 1},
        crd::containers::ConstSpan<float>{b, 1},
        /*max_ulp*/ 1000U);
    REQUIRE_FALSE(r.ok);
}

TEST_CASE("bit_compare matches identical int spans", "[test_helpers][bit]")
{
    const crd::i32 a[] = {1, 2, 3, 4, 5};
    const crd::i32 b[] = {1, 2, 3, 4, 5};
    const auto r = crd::test::bit_compare(
        crd::containers::ConstSpan<crd::i32>{a, 5},
        crd::containers::ConstSpan<crd::i32>{b, 5});
    REQUIRE(r.ok);
    REQUIRE(r.compared_count == 5);
}

TEST_CASE("bit_compare reports first mismatch index", "[test_helpers][bit]")
{
    const crd::u32 a[] = {1U, 2U, 3U, 4U, 5U};
    const crd::u32 b[] = {1U, 2U, 99U, 4U, 5U};
    const auto r = crd::test::bit_compare(
        crd::containers::ConstSpan<crd::u32>{a, 5},
        crd::containers::ConstSpan<crd::u32>{b, 5});
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.first_mismatch_index == 2);
    REQUIRE(r.cpu_value == 3U);
    REQUIRE(r.gpu_value == 99U);
}

TEST_CASE("gpu_determinism_check passes for byte-stable mock", "[test_helpers][determinism]")
{
    crd::containers::Array<crd::u8> backing;
    backing.resize(64);
    for (crd::usize i = 0; i < backing.size(); ++i)
    {
        backing[i] = static_cast<crd::u8>(i * 7U + 3U);
    }
    int dispatch_count = 0;
    const bool ok = crd::test::gpu_determinism_check(
        [&]{ ++dispatch_count; /* deterministic: no-op refill */ },
        [&]() -> crd::containers::ConstSpan<crd::u8> {
            return { backing.data(), backing.size() };
        },
        /*rounds*/ 4);
    REQUIRE(ok);
    REQUIRE(dispatch_count == 4);
}

TEST_CASE("gpu_determinism_check fails for non-deterministic mock", "[test_helpers][determinism]")
{
    crd::containers::Array<crd::u8> backing;
    backing.resize(16);
    int dispatch_count = 0;
    const bool ok = crd::test::gpu_determinism_check(
        [&]{
            // Each "dispatch" flips byte 5 — simulating an atomics race.
            ++dispatch_count;
            backing[5] = static_cast<crd::u8>(dispatch_count);
        },
        [&]() -> crd::containers::ConstSpan<crd::u8> {
            return { backing.data(), backing.size() };
        },
        /*rounds*/ 3);
    REQUIRE_FALSE(ok);
}

TEST_CASE("measure_ms returns a positive duration", "[test_helpers][perf]")
{
    const double dur_ms = crd::perf::measure_ms([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    REQUIRE(dur_ms >= 0.5);   // generous floor — accounts for scheduler jitter
    REQUIRE(dur_ms <= 200.0); // generous ceiling — Windows scheduler can wake late
}

TEST_CASE("CRD_PERF_BUDGET_LE passes for a fast lambda", "[test_helpers][perf]")
{
    // 1-second budget for a no-op — generous so this doesn't flake under
    // CI load. Exists purely to prove the macro compiles + evaluates the
    // lambda + accepts the budget; v9 slices set realistic budgets per kernel.
    CRD_PERF_BUDGET_LE("trivial_noop", 1000.0, [&]{
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) { x += i; }
        (void)x;
    });
    SUCCEED("CRD_PERF_BUDGET_LE accepted the budget");
}
