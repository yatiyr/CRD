// crd-time -- FrameClock tests.

#include <crd/time/frame_clock.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::time;
using namespace crd::units::literals;
using crd::f64;
using crd::u64;
} // namespace

TEST_CASE("FrameClock: default ctor uses 60 Hz fixed step",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    CHECK(clock.fixed_step_duration().value == 1.0 / 60.0);
    CHECK(clock.frame_count() == 0);
    CHECK(clock.fixed_step_count() == 0);
}

TEST_CASE("FrameClock: first tick() has zero delta (no startup spike)",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    clock.tick();
    CHECK(clock.delta().value == 0.0);
    CHECK(clock.frame_count() == 1);
}

TEST_CASE("FrameClock: second tick produces positive delta",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    clock.tick();
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i)
    {
        sink += i;
    }
    (void)sink;
    clock.tick();
    CHECK(clock.delta().value > 0.0);
    CHECK(clock.frame_count() == 2);
}

TEST_CASE("FrameClock: configurable fixed step",
          "[d-006][frame-clock]")
{
    FrameClock clock{Duration{1.0 / 120.0}};  // 120 Hz
    CHECK(clock.fixed_step_duration().value == 1.0 / 120.0);
}

TEST_CASE("FrameClock: consume_fixed_step false when accumulator empty",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    clock.tick();  // seed only (zero delta)
    CHECK(!clock.consume_fixed_step());
    CHECK(clock.fixed_step_count() == 0);
}

TEST_CASE("FrameClock: alpha is in [0, 1]",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    clock.tick();
    f64 a = clock.alpha();
    CHECK(a >= 0.0);
    CHECK(a <= 1.0);
}

TEST_CASE("FrameClock: reset zeroes counters",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    clock.tick();
    clock.tick();
    CHECK(clock.frame_count() == 2);
    clock.reset();
    CHECK(clock.frame_count() == 0);
    CHECK(clock.fixed_step_count() == 0);
    CHECK(clock.delta().value == 0.0);
    CHECK(clock.accumulator().value == 0.0);
}

TEST_CASE("FrameClock: set_fixed_step_duration changes step rate",
          "[d-006][frame-clock]")
{
    FrameClock clock;
    CHECK(clock.fixed_step_duration().value == 1.0 / 60.0);
    clock.set_fixed_step_duration(Duration{1.0 / 144.0});
    CHECK(clock.fixed_step_duration().value == 1.0 / 144.0);
}

// Note: testing the actual consume_fixed_step + alpha interaction requires
// a real-time wait or a mocked clock. Skipped here; the algorithm is
// straightforward accumulator math and gets exercised in the eylem v1c+
// integration smoke when geometry phase completes.
