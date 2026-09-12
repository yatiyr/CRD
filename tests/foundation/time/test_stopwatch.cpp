// crd-time -- Stopwatch + ScopedStopwatch tests.

#include <crd/time/stopwatch.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::time;
using namespace crd::units::literals;
} // namespace

TEST_CASE("Stopwatch: default-constructed is running", "[d-006][stopwatch]")
{
    Stopwatch sw;
    CHECK(sw.running());
    CHECK(sw.elapsed().value >= 0.0);
}

TEST_CASE("Stopwatch: elapsed grows over time", "[d-006][stopwatch]")
{
    Stopwatch sw;
    Duration first = sw.elapsed();
    // Burn some cycles.
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i)
    {
        sink += i;
    }
    (void)sink;
    Duration second = sw.elapsed();
    CHECK(second.value >= first.value);
}

TEST_CASE("Stopwatch: stop() freezes elapsed", "[d-006][stopwatch]")
{
    Stopwatch sw;
    volatile int sink = 0;
    for (int i = 0; i < 1000; ++i)
    {
        sink += i;
    }
    (void)sink;
    sw.stop();
    CHECK(!sw.running());

    Duration first = sw.elapsed();
    for (int i = 0; i < 100000; ++i)
    {
        sink += i;
    }
    (void)sink;
    Duration second = sw.elapsed();
    // Frozen — should be identical.
    CHECK(first.value == second.value);
}

TEST_CASE("Stopwatch: reset() returns to running + zero", "[d-006][stopwatch]")
{
    Stopwatch sw;
    volatile int sink = 0;
    for (int i = 0; i < 1000; ++i)
    {
        sink += i;
    }
    (void)sink;
    sw.stop();
    sw.reset();
    CHECK(sw.running());
    // Elapsed should be very small (just the post-reset overhead).
    CHECK(sw.elapsed().value < 0.1);  // less than 100 ms
}

TEST_CASE("ScopedStopwatch: callback fires on scope exit with elapsed Duration",
          "[d-006][stopwatch][scoped]")
{
    Duration captured;
    bool fired = false;
    {
        ScopedStopwatch sw{[&](Duration d) noexcept {
            captured = d;
            fired = true;
        }};
        volatile int sink = 0;
        for (int i = 0; i < 1000; ++i)
        {
            sink += i;
        }
        (void)sink;
    }
    CHECK(fired);
    CHECK(captured.value >= 0.0);
}

TEST_CASE("CRD_TIME_SCOPED_STOPWATCH_AS macro captures duration",
          "[d-006][stopwatch][macro]")
{
    Duration elapsed{99.0};  // initial sentinel
    {
        CRD_TIME_SCOPED_STOPWATCH_AS(work, elapsed);
        volatile int sink = 0;
        for (int i = 0; i < 1000; ++i)
        {
            sink += i;
        }
        (void)sink;
    }
    // After scope exit, `elapsed` should have been overwritten with a real
    // (small, non-negative) duration.
    CHECK(elapsed.value != 99.0);
    CHECK(elapsed.value >= 0.0);
    CHECK(elapsed.value < 1.0);  // < 1 second for a trivial loop
}
