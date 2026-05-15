// crd-perf v0a -- determinism contract smoke test.
//
// ADR-0063 pins: profiling MUST NOT perturb deterministic computation.
// The profiler reads MonotonicClock and writes Samples, but the visible
// state of the engine (the things ADR-0063 covers: simulation, replay,
// fixed-step physics) must not change one bit whether profiling is
// active or not.
//
// This v0a smoke verifies the contract at a coarse level: a known
// deterministic computation produces the same f64 output bits before
// any CRD_PERF_SCOPE wrap AND inside many CRD_PERF_SCOPE wraps. Real
// engine deterministic-replay coverage lives in D-004; this test pins
// the substrate contract at the unit level.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>

#if CRD_PERF_ENABLED

namespace
{

// Deterministic accumulator that ADR-0063 protects: integer ops + IEEE
// 754 binary64 + no transcendentals. Identical bits across compilers
// per ADR-0063's contract.
[[nodiscard]] crd::u64 deterministic_compute(crd::u32 iter) noexcept
{
    double acc = 1.0;
    for (crd::u32 i = 1U; i <= iter; ++i)
    {
        // Sum of 1/i! truncated; bit-exact because the operation is a
        // chain of fmul + fadd over a tiny power-of-two-friendly set.
        acc += 1.0 / static_cast<double>(i);
        acc *= 1.0000001;
        acc -= 0.0000005;
    }
    return std::bit_cast<crd::u64>(acc);
}

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("profiling is observable-effect-free on deterministic computation", "[perf][determinism]")
{
    // Run 1: no profiler.
    const crd::u64 baseline = deterministic_compute(10000U);

    // Run 2: profiler initialised but no wrap on the computation.
    PerfFixture fx;
    const crd::u64 active_no_wrap = deterministic_compute(10000U);
    REQUIRE(active_no_wrap == baseline);

    // Run 3: profiler active and the computation wrapped at every iteration.
    crd::u64 wrapped_bits = 0U;
    {
        CRD_PERF_SCOPE("determinism_outer");
        double acc = 1.0;
        for (crd::u32 i = 1U; i <= 10000U; ++i)
        {
            CRD_PERF_SCOPE("determinism_inner");
            acc += 1.0 / static_cast<double>(i);
            acc *= 1.0000001;
            acc -= 0.0000005;
        }
        wrapped_bits = std::bit_cast<crd::u64>(acc);
    }
    REQUIRE(wrapped_bits == baseline);
}

#endif // CRD_PERF_ENABLED
