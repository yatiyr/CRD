#include <crd/platform/timer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("Timer: starts at (approximately) zero", "[platform][timer]")
{
    crd::platform::Timer t;
    // The clock has been running for a tiny amount of time since construction.
    // We accept anything under 50 ms as 'just started'.
    REQUIRE(t.elapsed_seconds() >= 0.0);
    REQUIRE(t.elapsed_milliseconds() < 50.0);
}

TEST_CASE("Timer: elapsed grows monotonically", "[platform][timer]")
{
    crd::platform::Timer t;
    const crd::f64 first = t.elapsed_seconds();
    std::this_thread::sleep_for(2ms);
    const crd::f64 second = t.elapsed_seconds();
    REQUIRE(second >= first);
    REQUIRE(second >= 0.001); // at least 1 ms passed
}

TEST_CASE("Timer: reset moves the anchor forward", "[platform][timer]")
{
    crd::platform::Timer t;
    std::this_thread::sleep_for(5ms);
    REQUIRE(t.elapsed_milliseconds() >= 4.0);

    t.reset();
    // Right after reset, elapsed should be close to zero again.
    REQUIRE(t.elapsed_milliseconds() < 5.0);
}

TEST_CASE("Timer: nanoseconds match seconds within tolerance", "[platform][timer]")
{
    crd::platform::Timer t;
    std::this_thread::sleep_for(5ms);
    const crd::f64 secs = t.elapsed_seconds();
    const crd::u64 nanos = t.elapsed_nanoseconds();

    // nanos sampled microscopically after secs, so it must be >= secs in ns.
    const crd::f64 secs_as_ns = secs * 1.0e9;
    REQUIRE(static_cast<crd::f64>(nanos) >= secs_as_ns - 1.0);
}

TEST_CASE("FrameClock: first tick reports zero delta", "[platform][frameclock]")
{
    crd::platform::FrameClock clock;
    REQUIRE(clock.frame_count() == 0U);
    REQUIRE(clock.delta_seconds() == 0.0);

    clock.tick();
    REQUIRE(clock.frame_count() == 1U);
    REQUIRE(clock.delta_seconds() == 0.0); // first tick seeds, no spike
}

TEST_CASE("FrameClock: subsequent ticks report real deltas", "[platform][frameclock]")
{
    crd::platform::FrameClock clock;
    clock.tick(); // seed

    std::this_thread::sleep_for(3ms);
    clock.tick();
    REQUIRE(clock.frame_count() == 2U);
    REQUIRE(clock.delta_seconds() >= 0.002);
    REQUIRE(clock.delta_seconds() < 1.0); // sanity upper bound

    std::this_thread::sleep_for(3ms);
    clock.tick();
    REQUIRE(clock.frame_count() == 3U);
    REQUIRE(clock.delta_seconds() >= 0.002);
}

TEST_CASE("FrameClock: total_seconds tracks construction time", "[platform][frameclock]")
{
    crd::platform::FrameClock clock;
    std::this_thread::sleep_for(5ms);
    const crd::f64 total = clock.total_seconds();
    REQUIRE(total >= 0.004);
    REQUIRE(total < 1.0);
}

TEST_CASE("FrameClock: reset zeroes everything", "[platform][frameclock]")
{
    crd::platform::FrameClock clock;
    clock.tick();
    std::this_thread::sleep_for(3ms);
    clock.tick();
    REQUIRE(clock.frame_count() == 2U);
    REQUIRE(clock.delta_seconds() > 0.0);

    clock.reset();
    REQUIRE(clock.frame_count() == 0U);
    REQUIRE(clock.delta_seconds() == 0.0);
    REQUIRE(clock.total_seconds() < 0.005); // freshly reset

    // After reset, the next tick should still be a seed (zero delta).
    clock.tick();
    REQUIRE(clock.delta_seconds() == 0.0);
    REQUIRE(clock.frame_count() == 1U);
}
