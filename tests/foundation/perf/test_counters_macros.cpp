// crd-perf v0b -- CRD_PERF_COUNTER_* macros work end-to-end.

#include <crd/perf/perf.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("CRD_PERF_COUNTER_SET_I64 registers + writes", "[perf][counter][macro]")
{
    PerfFixture fx;
    CRD_PERF_COUNTER_SET_I64("macro.set.i64", 99);
    CHECK(crd::perf::counter_count() == 1U);
    CHECK(crd::perf::counter_current_i64(crd::perf::CounterId{0}) == 99);
}

TEST_CASE("CRD_PERF_COUNTER_ADD_I64 accumulates", "[perf][counter][macro]")
{
    PerfFixture fx;
    for (int i = 0; i < 5; ++i)
    {
        CRD_PERF_COUNTER_ADD_I64("macro.add.i64", 4);
    }
    CHECK(crd::perf::counter_current_i64(crd::perf::CounterId{0}) == 20);
}

TEST_CASE("CRD_PERF_COUNTER_SET_DURATION + ADD_DURATION work", "[perf][counter][macro][duration]")
{
    using namespace crd::units::literals;
    PerfFixture fx;
    CRD_PERF_COUNTER_SET_DURATION("macro.frame.cpu_ms", 16.667_ms);
    CRD_PERF_COUNTER_ADD_DURATION("macro.io.total", 1.0_ms);
    CRD_PERF_COUNTER_ADD_DURATION("macro.io.total", 2.5_ms);
    CHECK(crd::perf::counter_count() == 2U);

    const auto cpu = crd::perf::counter_current_duration(crd::perf::CounterId{0});
    const auto io  = crd::perf::counter_current_duration(crd::perf::CounterId{1});
    CHECK(cpu.value == Catch::Approx(0.016667).margin(1e-6));
    CHECK(io.value  == Catch::Approx(0.0035).margin(1e-6));
}

TEST_CASE("macro caches CounterId in TU-local static (only one registration per call site)",
          "[perf][counter][macro][cache]")
{
    PerfFixture fx;
    for (int i = 0; i < 1000; ++i)
    {
        CRD_PERF_COUNTER_ADD_I64("macro.cached", 1);
    }
    // One call site -> one registration regardless of how many times we hit it.
    CHECK(crd::perf::counter_count() == 1U);
    CHECK(crd::perf::counter_current_i64(crd::perf::CounterId{0}) == 1000);
}

#endif // CRD_PERF_ENABLED
