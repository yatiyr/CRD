// crd-time -- Deadline + sleep primitives tests.

#include <crd/time/deadline.hpp>
#include <crd/time/stopwatch.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::time;
using namespace crd::units::literals;
using crd::f64;
} // namespace

TEST_CASE("Deadline: future deadline not expired", "[d-006][deadline]")
{
    Deadline d = Deadline::from_now(1.0_s);
    CHECK(!d.expired());
    CHECK(d.remaining().value > 0.0);
}

TEST_CASE("Deadline: past deadline expired", "[d-006][deadline]")
{
    Instant past{0};  // epoch
    Deadline d{past};
    CHECK(d.expired());
    CHECK(d.remaining().value < 0.0);
}

TEST_CASE("Deadline: explicit Instant ctor", "[d-006][deadline]")
{
    Instant future = MonotonicClock::now() + 5.0_s;
    Deadline d{future};
    CHECK(d.when() == future);
}

TEST_CASE("sleep_for: zero duration returns immediately",
          "[d-006][sleep]")
{
    Stopwatch sw;
    sleep_for(Duration{0.0});
    CHECK(sw.elapsed().value < 0.01);  // < 10 ms (should be near-instant)
}

TEST_CASE("sleep_for: 10 ms actually sleeps at least 10 ms",
          "[d-006][sleep]")
{
    Stopwatch sw;
    sleep_for(10.0_ms);
    Duration elapsed = sw.elapsed();
    // sleep_for is "at least"; allow oversleep but assert minimum.
    CHECK(elapsed.value >= 0.009);  // 9 ms minimum (1 ms slack for measurement)
}

TEST_CASE("sleep_until: past deadline returns immediately",
          "[d-006][sleep]")
{
    Stopwatch sw;
    Deadline d{Instant{0}};  // epoch (in the past)
    sleep_until(d);
    CHECK(sw.elapsed().value < 0.01);
}

TEST_CASE("yield_thread does not crash",
          "[d-006][sleep]")
{
    yield_thread();
    CHECK(true);
}
