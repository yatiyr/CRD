// crd-time -- MonotonicClock / WallClock / CycleCounter tests.

#include <crd/time/clocks.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::time;
using namespace crd::units::literals;
using crd::f64;
using crd::u64;
} // namespace

TEST_CASE("MonotonicClock: now() returns a positive instant", "[d-006][clocks]")
{
    Instant t = MonotonicClock::now();
    CHECK(t.ns_since_epoch() > 0);
}

TEST_CASE("MonotonicClock: now() is monotonically non-decreasing",
          "[d-006][clocks][monotonic]")
{
    Instant a = MonotonicClock::now();
    Instant b = MonotonicClock::now();
    Instant c = MonotonicClock::now();
    CHECK(a <= b);
    CHECK(b <= c);
}

TEST_CASE("MonotonicClock: elapsed between two now() calls is positive Duration",
          "[d-006][clocks][monotonic]")
{
    Instant a = MonotonicClock::now();
    // Do at least some work so the clock advances.
    volatile int sink = 0;
    for (int i = 0; i < 1000; ++i)
    {
        sink += i;
    }
    (void)sink;
    Instant b = MonotonicClock::now();
    Duration elapsed = b - a;
    CHECK(elapsed.value >= 0.0);
}

TEST_CASE("WallClock: now() returns a recent year (post-2020)",
          "[d-006][clocks][wall]")
{
    Instant t = WallClock::now();
    // Unix epoch + ~50 years ~ 1.58e18 ns (2020 onwards).
    CHECK(t.ns_since_epoch() > static_cast<crd::i64>(1'500'000'000) * 1'000'000'000);
}

TEST_CASE("CycleCounter: now() returns a non-zero u64",
          "[d-006][clocks][cycle]")
{
    u64 c = CycleCounter::now();
    CHECK(c > 0);
}

TEST_CASE("CycleCounter: two consecutive reads are non-decreasing",
          "[d-006][clocks][cycle]")
{
    u64 a = CycleCounter::now();
    u64 b = CycleCounter::now();
    // On invariant-TSC hardware (every modern x86) this is monotonic.
    CHECK(b >= a);
}

TEST_CASE("CycleCounter: calibrate() returns a positive cycles-per-second",
          "[d-006][clocks][cycle][calibrate]")
{
    // Use a small calibration window so the test runs fast.
    f64 cycles_per_sec = CycleCounter::calibrate(Duration{0.005});
    // Sanity bounds: any modern CPU is 1+ GHz, less than 100 GHz.
    CHECK(cycles_per_sec > 0.0);
    CHECK(cycles_per_sec > 1.0e8);   // > 100 MHz (very conservative)
    CHECK(cycles_per_sec < 1.0e11);  // < 100 GHz
}

TEST_CASE("CycleCounter::cycles_to_duration: basic conversion",
          "[d-006][clocks][cycle]")
{
    // 3e9 cycles at 3e9 Hz = 1 second.
    constexpr f64 kCyclesPerSec = 3.0e9;
    Duration d = CycleCounter::cycles_to_duration(3'000'000'000U, kCyclesPerSec);
    CHECK(d.value == 1.0);
}
