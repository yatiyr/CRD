// crd-perf v0b -- counter registration + dedup + info round-trip.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("register_counter_i64 returns valid handle", "[perf][counter][register]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("test.alpha", crd::perf::CounterKind::Set);
    CHECK(id.is_valid());
    CHECK(crd::perf::counter_count() == 1U);

    const auto info = crd::perf::counter_info(id);
    CHECK(std::strcmp(info.name, "test.alpha") == 0);
    CHECK(info.kind == crd::perf::CounterKind::Set);
    CHECK(info.type == crd::perf::CounterType::I64);
}

TEST_CASE("register_counter dedups same name+kind+type", "[perf][counter][dedup]")
{
    PerfFixture fx;
    const auto a = crd::perf::register_counter_i64("same.name", crd::perf::CounterKind::Set);
    const auto b = crd::perf::register_counter_i64("same.name", crd::perf::CounterKind::Set);
    CHECK(a.value == b.value);
    CHECK(crd::perf::counter_count() == 1U);
}

TEST_CASE("register_counter distinguishes by kind", "[perf][counter][dedup]")
{
    PerfFixture fx;
    const auto a = crd::perf::register_counter_i64("draws", crd::perf::CounterKind::Set);
    const auto b = crd::perf::register_counter_i64("draws", crd::perf::CounterKind::Add);
    CHECK(a.value != b.value);
    CHECK(crd::perf::counter_count() == 2U);
}

TEST_CASE("register_counter distinguishes by type", "[perf][counter][dedup]")
{
    PerfFixture fx;
    const auto i = crd::perf::register_counter_i64("temp", crd::perf::CounterKind::Set);
    const auto f = crd::perf::register_counter_f64("temp", crd::perf::CounterKind::Set);
    const auto d = crd::perf::register_counter_duration("temp", crd::perf::CounterKind::Set);
    CHECK(i.value != f.value);
    CHECK(f.value != d.value);
    CHECK(i.value != d.value);
    CHECK(crd::perf::counter_count() == 3U);
}

TEST_CASE("register_counter on inactive profiler returns invalid", "[perf][counter][register]")
{
    // No init.
    CHECK_FALSE(crd::perf::is_active());
    const auto id = crd::perf::register_counter_i64("x", crd::perf::CounterKind::Set);
    CHECK_FALSE(id.is_valid());
}

#endif // CRD_PERF_ENABLED
