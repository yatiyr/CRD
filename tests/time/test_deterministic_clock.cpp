// crd-time -- DeterministicClock tests.

#include <crd/time/deterministic_clock.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::time;
using namespace crd::units::literals;
using crd::f64;
using crd::u64;
} // namespace

TEST_CASE("DeterministicClock: default 60 Hz tick rate", "[d-006][det-clock]")
{
    DeterministicClock clock;
    CHECK(clock.tick_count() == 0);
    CHECK(clock.tick_period().value == 1.0 / 60.0);
    CHECK(clock.elapsed().value == 0.0);
}

TEST_CASE("DeterministicClock: tick() advances by one", "[d-006][det-clock]")
{
    DeterministicClock clock;
    clock.tick();
    CHECK(clock.tick_count() == 1);
    CHECK(clock.elapsed().value == 1.0 / 60.0);
}

TEST_CASE("DeterministicClock: advance(N) advances by N", "[d-006][det-clock]")
{
    DeterministicClock clock;
    clock.advance(60);  // 1 second at 60 Hz
    CHECK(clock.tick_count() == 60);
    CHECK(clock.elapsed().value == 60.0 / 60.0);
}

TEST_CASE("DeterministicClock: custom tick period", "[d-006][det-clock]")
{
    DeterministicClock clock{Duration{0.5}};  // 2 Hz
    clock.advance(4);
    CHECK(clock.tick_count() == 4);
    CHECK(clock.elapsed().value == 2.0);
}

TEST_CASE("DeterministicClock: reset()", "[d-006][det-clock]")
{
    DeterministicClock clock;
    clock.advance(100);
    clock.reset();
    CHECK(clock.tick_count() == 0);
    CHECK(clock.elapsed().value == 0.0);
}

TEST_CASE("DeterministicClock: constexpr-evaluable", "[d-006][det-clock][constexpr]")
{
    constexpr DeterministicClock k_clock{Duration{1.0 / 120.0}};
    STATIC_REQUIRE(k_clock.tick_count() == 0);
    STATIC_REQUIRE(k_clock.tick_period().value == 1.0 / 120.0);
}

TEST_CASE("DeterministicClock: bit-exact reproducibility for same tick count",
          "[d-006][det-clock][determinism]")
{
    DeterministicClock a;
    DeterministicClock b;
    for (u64 i = 0; i < 1000; ++i)
    {
        a.tick();
        b.tick();
    }
    // Bit-exact elapsed Duration regardless of wall-clock state.
    CHECK(a.elapsed().value == b.elapsed().value);
    CHECK(a.tick_count() == b.tick_count());
}

TEST_CASE("DeterministicClock: set_tick_period at runtime",
          "[d-006][det-clock]")
{
    DeterministicClock clock;
    clock.advance(10);
    clock.set_tick_period(Duration{0.01});
    // tick_count preserved; "elapsed" now uses new period.
    CHECK(clock.tick_count() == 10);
    CHECK(clock.elapsed().value == 0.1);
}
