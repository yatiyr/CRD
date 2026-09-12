// crd-perf v0b -- Set vs Add semantics.

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

TEST_CASE("Set-kind i64 counter overwrites on each write", "[perf][counter][set]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("set.i64", crd::perf::CounterKind::Set);
    crd::perf::counter_set_i64(id, 42);
    CHECK(crd::perf::counter_current_i64(id) == 42);
    crd::perf::counter_set_i64(id, 7);
    CHECK(crd::perf::counter_current_i64(id) == 7);
}

TEST_CASE("Add-kind i64 counter accumulates", "[perf][counter][add]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("add.i64", crd::perf::CounterKind::Add);
    crd::perf::counter_add_i64(id, 10);
    crd::perf::counter_add_i64(id, 5);
    crd::perf::counter_add_i64(id, 3);
    CHECK(crd::perf::counter_current_i64(id) == 18);
}

TEST_CASE("Set-kind f64 counter round-trip", "[perf][counter][set][f64]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_f64("set.f64", crd::perf::CounterKind::Set);
    crd::perf::counter_set_f64(id, 1.25);
    CHECK(crd::perf::counter_current_f64(id) == 1.25);
    crd::perf::counter_set_f64(id, -0.5);
    CHECK(crd::perf::counter_current_f64(id) == -0.5);
}

TEST_CASE("Add-kind f64 counter accumulates", "[perf][counter][add][f64]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_f64("add.f64", crd::perf::CounterKind::Add);
    crd::perf::counter_add_f64(id, 1.5);
    crd::perf::counter_add_f64(id, 0.5);
    crd::perf::counter_add_f64(id, 2.0);
    CHECK(crd::perf::counter_current_f64(id) == 4.0);
}

TEST_CASE("Set-kind Duration counter round-trip", "[perf][counter][set][duration]")
{
    using namespace crd::units::literals;
    PerfFixture fx;
    const auto id = crd::perf::register_counter_duration("set.dur", crd::perf::CounterKind::Set);
    crd::perf::counter_set_duration(id, 1.5_s);
    CHECK(crd::perf::counter_current_duration(id).value == 1.5);
    crd::perf::counter_set_duration(id, 16.667_ms);
    CHECK(crd::perf::counter_current_duration(id).value == Catch::Approx(0.016667).margin(1e-6));
}

TEST_CASE("Add-kind Duration counter accumulates", "[perf][counter][add][duration]")
{
    using namespace crd::units::literals;
    PerfFixture fx;
    const auto id = crd::perf::register_counter_duration("add.dur", crd::perf::CounterKind::Add);
    crd::perf::counter_add_duration(id, 10.0_ms);
    crd::perf::counter_add_duration(id, 5.0_ms);
    crd::perf::counter_add_duration(id, 1.0_ms);
    CHECK(crd::perf::counter_current_duration(id).value == Catch::Approx(0.016).margin(1e-6));
}

TEST_CASE("counter writes on invalid id are no-ops", "[perf][counter][robustness]")
{
    PerfFixture fx;
    crd::perf::counter_set_i64(crd::perf::kInvalidCounterId, 99);
    crd::perf::counter_add_f64(crd::perf::kInvalidCounterId, 1.0);
    // Should not crash; counter_count stays 0.
    CHECK(crd::perf::counter_count() == 0U);
}

#endif // CRD_PERF_ENABLED
