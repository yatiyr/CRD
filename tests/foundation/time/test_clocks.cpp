// crd-time -- MonotonicClock / WallClock / CycleCounter / TickClock tests.

#include <crd/time/clocks.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

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
    constexpr f64 k_cycles_per_sec = 3.0e9;
    Duration d = CycleCounter::cycles_to_duration(3'000'000'000U, k_cycles_per_sec);
    CHECK(d.value == 1.0);
}

// ---- TickClock (DIAG.11b clause (8)) ----------------------------------------------------------------------------

namespace
{
// Read TickClock between two MonotonicClock reads for `span_ns` of MonotonicClock time: every reading must lie in its
// bracket widened by `slack_ns`, and none may step back. Returns the number of readings, or 0 on the first miss.
crd::u64 bracket_reads(crd::i64 span_ns, crd::i64 slack_ns, crd::i64& worst_ns, bool& stepped_back)
{
    const crd::i64 start = MonotonicClock::now().ns_since_epoch();
    crd::i64       last  = 0;
    crd::u64       reads = 0U;
    worst_ns             = 0;
    stepped_back         = false;
    for (;;)
    {
        const crd::i64 before = MonotonicClock::now().ns_since_epoch();
        const crd::i64 tick   = TickClock::now_ns();
        const crd::i64 after  = MonotonicClock::now().ns_since_epoch();
        stepped_back          = stepped_back || tick < last;
        last                  = tick;
        crd::i64       miss   = 0;
        if (tick < before)
        {
            miss = before - tick;
        }
        else if (tick > after)
        {
            miss = tick - after;
        }
        worst_ns              = miss > worst_ns ? miss : worst_ns;
        if (miss > slack_ns)
        {
            return 0U;
        }
        ++reads;
        if (after - start >= span_ns)
        {
            return reads;
        }
    }
}
} // namespace

TEST_CASE("TickClock: reads stay on MonotonicClock's timeline through calibration and re-anchors",
          "[diag11b][clocks][tick]")
{
#if CRD_ENABLE_ASSERTS
    TickClock::test_reset(); // calibrate from here, whatever ran before in this process
#endif
    // Through the calibration (MonotonicClock itself) and four re-anchors: within 20 us of a bracket that
    // MonotonicClock's own resolution (100 ns on Windows) already widens.
    crd::i64       worst = 0;
    bool           back  = false;
    const crd::u64 reads =
        bracket_reads(TickClock::kCalibrationNs + (4 * TickClock::kReanchorNs), 20'000, worst, back);
    INFO("worst distance from the MonotonicClock bracket: " << worst << " ns");
    CHECK(reads > 0U);
    CHECK_FALSE(back);
    // The counter is used exactly where it may be.
    CHECK(TickClock::counter_mapped() == TickClock::counter_usable());
}

TEST_CASE("TickClock: no thread's reads step back while any thread re-anchors", "[diag11b][clocks][tick]")
{
    constexpr int thread_count = 4;
    std::atomic<int> stepped_back{0};
    std::atomic<crd::u64> reads{0U};
    const crd::i64 end = MonotonicClock::now().ns_since_epoch() + (3 * TickClock::kReanchorNs);
    std::thread workers[thread_count]; // NOLINT(modernize-avoid-c-arrays)
    for (std::thread& worker : workers)
    {
        worker = std::thread(
            [&]
            {
                crd::i64 last  = TickClock::now_ns();
                crd::u64 count = 0U;
                while (MonotonicClock::now().ns_since_epoch() < end)
                {
                    for (int i = 0; i < 256; ++i)
                    {
                        const crd::i64 now = TickClock::now_ns();
                        if (now < last)
                        {
                            stepped_back.fetch_add(1, std::memory_order_relaxed);
                        }
                        last = now;
                        ++count;
                    }
                }
                reads.fetch_add(count, std::memory_order_relaxed);
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    CHECK(reads.load() > 0U);
    CHECK(stepped_back.load() == 0);
}

#if CRD_ENABLE_ASSERTS
TEST_CASE("TickClock: a read is the counter scaled from the anchor", "[diag11b][clocks][tick]")
{
    // A map no MonotonicClock read could give: 1,000 ns per tick from 10^15 ns.
    const u64 at = CycleCounter::now();
    TickClock::test_force_map(at, 1'000'000'000'000'000, 1000.0);
    const crd::i64 first  = TickClock::now_ns();
    const u64      ticks  = CycleCounter::now() - at;
    const crd::i64 second = TickClock::now_ns();
    TickClock::test_reset();
    CHECK(TickClock::counter_mapped() == false);
    CHECK(first >= 1'000'000'000'000'000);
    CHECK(second > first);
    // The second read came after `ticks` had passed: at least that many microseconds past the anchor.
    CHECK(second - 1'000'000'000'000'000 >= static_cast<crd::i64>(ticks) * 1000);
}
#endif
