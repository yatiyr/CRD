// crd-perf v0a -- ring overflow drops new samples + bumps the counter.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture()
    {
        crd::perf::InitConfig cfg{};
        cfg.per_thread_ring_slots = 16U; // tiny, deterministic
        crd::perf::init(cfg);
    }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("ring saturates at slot count, drops record bump", "[perf][overflow]")
{
    PerfFixture fx;
    constexpr crd::u32 kAttempts = 100U;
    const auto id = crd::perf::intern_name("overflow_test");

    for (crd::u32 i = 0U; i < kAttempts; ++i)
    {
        const auto tok = crd::perf::push_region(id);
        crd::perf::pop_region(id, tok);
    }

    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    CHECK(view.size == 16U);            // exactly the slot count
    CHECK(view.dropped == kAttempts - 16U);
}

TEST_CASE("clear_samples lets the ring re-fill from zero", "[perf][overflow][clear]")
{
    PerfFixture fx;
    const auto id = crd::perf::intern_name("refill_test");

    for (crd::u32 i = 0U; i < 32U; ++i)
    {
        const auto tok = crd::perf::push_region(id);
        crd::perf::pop_region(id, tok);
    }
    REQUIRE(crd::perf::thread_samples(crd::perf::current_thread_index()).size == 16U);
    crd::perf::clear_samples();
    CHECK(crd::perf::thread_samples(crd::perf::current_thread_index()).size == 0U);
    CHECK(crd::perf::thread_samples(crd::perf::current_thread_index()).dropped == 0U);

    for (crd::u32 i = 0U; i < 5U; ++i)
    {
        const auto tok = crd::perf::push_region(id);
        crd::perf::pop_region(id, tok);
    }
    CHECK(crd::perf::thread_samples(crd::perf::current_thread_index()).size == 5U);
}

#endif // CRD_PERF_ENABLED
