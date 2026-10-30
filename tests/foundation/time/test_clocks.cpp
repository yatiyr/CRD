// crd-time -- MonotonicClock / WallClock / CycleCounter / TickClock tests.

#include <crd/core/platform.hpp>
#include <crd/time/clocks.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#if CRD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // GetProcessAffinityMask, SetThreadAffinityMask
#elif CRD_OS_LINUX
#include <sched.h> // sched_getaffinity, sched_setaffinity
#endif

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
// Read TickClock between two MonotonicClock reads until a reading starts `span_ns` of MonotonicClock time after the
// first one ended: every reading must lie in its bracket widened by `slack_ns`, and none may step back. Returns the
// number of readings, or 0 on the first miss. The span counts from the first reading's end and up to a reading's
// start, so neither the first read's one-time work (the CPU selection pins the thread to every processor in turn) nor
// a preemption inside the last bracket shortens the reads: the last one runs at least `span_ns` past the clock's
// first reading (2026-10-30: a span from before the first read let one slow read on a loaded runner end the case
// while the clock still calibrated).
crd::u64 bracket_reads(crd::i64 span_ns, crd::i64 slack_ns, crd::i64& worst_ns, bool& stepped_back)
{
    crd::i64 first_end = 0;
    crd::i64 last      = 0;
    crd::u64 reads     = 0U;
    worst_ns           = 0;
    stepped_back       = false;
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
        if (reads == 1U)
        {
            first_end = after;
        }
        else if (before - first_end >= span_ns)
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
    INFO("worst distance from the MonotonicClock bracket: " << worst << " ns, reads: " << reads);
    CHECK(reads > 0U);
    CHECK_FALSE(back);
    // The counter is used exactly where it may be: the last read ran past the calibration.
    CHECK(TickClock::counter_mapped() == TickClock::counter_usable());
}

namespace
{
// The processors this process may run on (at most 64), for a reader that moves itself between them.
struct Processors
{
    crd::u32 ids[64] = {}; // NOLINT(modernize-avoid-c-arrays)
    crd::u32 count   = 0U;
};

Processors allowed_processors()
{
    Processors p;
#if CRD_OS_WINDOWS
    DWORD_PTR process = 0U;
    DWORD_PTR system  = 0U;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &process, &system) == 0)
    {
        return p;
    }
    static_assert(sizeof(DWORD_PTR) * 8U <= 64U, "a process affinity mask fits the list");
    for (crd::u32 id = 0U; id < sizeof(DWORD_PTR) * 8U; ++id)
    {
        if (((process >> id) & 1U) != 0U)
        {
            p.ids[p.count++] = id;
        }
    }
#elif CRD_OS_LINUX
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0)
    {
        return p;
    }
    for (crd::u32 id = 0U; id < 64U; ++id)
    {
        if (CPU_ISSET(static_cast<int>(id), &set))
        {
            p.ids[p.count++] = id;
        }
    }
#endif
    return p;
}

// Move the calling thread onto processor `id` alone; true when the OS took it (the thread then runs there).
bool move_to(crd::u32 id)
{
#if CRD_OS_WINDOWS
    return ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(1) << id) != 0U;
#elif CRD_OS_LINUX
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(id), &set);
    return ::sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)id;
    return false;
#endif
}

struct ConcurrentReads
{
    crd::u64 reads = 0U;
    crd::u64 steps = 0U; // a thread's reading below its previous one
    crd::u64 moves = 0U; // the readers' moves to another processor
};

// `threads` readers read TickClock for `span_ns` of MonotonicClock time from their start, and on until the first map is
// published where the counter may be mapped (at most 10 s: a reader that waited long for a processor must still cross
// the hand-off); each counts its own readings that lie below its previous one. With `migrate`, every 64 reads a reader
// moves itself to the next processor this process may run on (reader i starts on the i-th), so consecutive reads of
// one thread come from different processors' counters and the calibration's hand-off and the re-anchors happen while
// threads move.
ConcurrentReads concurrent_reads(int threads, crd::i64 span_ns, bool migrate)
{
    constexpr int         max_threads = 8;
    const Processors      processors  = allowed_processors();
    std::atomic<crd::u64> reads{0U};
    std::atomic<crd::u64> steps{0U};
    std::atomic<crd::u64> moves{0U};
    const bool            usable  = TickClock::counter_usable();
    const crd::i64        start   = MonotonicClock::now().ns_since_epoch();
    const crd::i64        end     = start + span_ns;
    const crd::i64        give_up = start + 10'000'000'000;
    std::thread           workers[max_threads]; // NOLINT(modernize-avoid-c-arrays)
    const int             count = threads < max_threads ? threads : max_threads;
    for (int t = 0; t < count; ++t)
    {
        workers[t] = std::thread(
            [&, t]
            {
                crd::u32 next      = processors.count > 0U ? static_cast<crd::u32>(t) % processors.count : 0U;
                crd::u64 own_moves = 0U;
                if (migrate && processors.count > 1U && move_to(processors.ids[next]))
                {
                    ++own_moves;
                }
                crd::i64 last      = TickClock::now_ns();
                crd::u64 own_reads = 0U;
                crd::u64 own_steps = 0U;
                for (;;)
                {
                    const crd::i64 mono = MonotonicClock::now().ns_since_epoch();
                    if (mono >= give_up || (mono >= end && (!usable || TickClock::counter_mapped())))
                    {
                        break;
                    }
                    for (int i = 0; i < 64; ++i)
                    {
                        const crd::i64 now = TickClock::now_ns();
                        if (now < last)
                        {
                            ++own_steps;
                        }
                        last = now;
                        ++own_reads;
                    }
                    if (migrate && processors.count > 1U)
                    {
                        next = (next + 1U) % processors.count;
                        if (move_to(processors.ids[next]))
                        {
                            ++own_moves;
                        }
                    }
                }
                reads.fetch_add(own_reads, std::memory_order_relaxed);
                steps.fetch_add(own_steps, std::memory_order_relaxed);
                moves.fetch_add(own_moves, std::memory_order_relaxed);
            });
    }
    for (int t = 0; t < count; ++t)
    {
        workers[t].join();
    }
    return ConcurrentReads{reads.load(), steps.load(), moves.load()};
}
} // namespace

// A fresh process (CTest runs each case in its own) calibrates inside this case: the readers cross the hand-off from
// MonotonicClock to the first map and three re-anchors (the windows-shipping step of 2026-10-26 was at the hand-off).
TEST_CASE("TickClock: no thread's reads step back while any thread re-anchors", "[diag11b][clocks][tick]")
{
#if CRD_ENABLE_ASSERTS
    TickClock::test_reset(); // calibrate from here, whatever ran before in this process
#endif
    const ConcurrentReads r = concurrent_reads(4, 3 * TickClock::kReanchorNs, false);
    CHECK(r.reads > 0U);
    CHECK(r.steps == 0U);
    // The case crossed the hand-off: the counter is mapped exactly where it may be.
    CHECK(TickClock::counter_mapped() == TickClock::counter_usable());
}

TEST_CASE("TickClock: no thread's reads step back while threads move between processors", "[diag11b][clocks][tick]")
{
#if CRD_ENABLE_ASSERTS
    TickClock::test_reset();
#endif
    // Every 64 reads a reader moves to another processor: a counter that disagreed between processors by more than
    // 64 reads' worth would step the mover back, through the hand-off and three re-anchors.
    const ConcurrentReads r = concurrent_reads(4, 3 * TickClock::kReanchorNs, true);
    INFO("moves between processors: " << r.moves << ", reads: " << r.reads);
    CHECK(r.reads > 0U);
    CHECK(r.steps == 0U);
    if (allowed_processors().count > 1U)
    {
        CHECK(r.moves > 0U);
    }
    CHECK(TickClock::counter_mapped() == TickClock::counter_usable());
}

#if CRD_ENABLE_ASSERTS
TEST_CASE("TickClock: no thread's reads step back across many hand-offs to the first map", "[diag11b][clocks][tick]")
{
    // The hand-off happens once a process; here it is repeated: each round starts the clock over with no reader
    // running, then four readers read through the calibration and past the first map. With readers returning
    // MonotonicClock readings during the publication (the fault of 2026-10-26, restored as a test of this case) 200
    // rounds stepped back 3.9 times on average on the i9-14900K, so 400 rounds miss it with odds under one in a
    // thousand.
    constexpr int rounds = 400;
    crd::u64      steps   = 0U;
    crd::u64      reads   = 0U;
    int           mapped  = 0;
    for (int round = 0; round < rounds; ++round)
    {
        TickClock::test_reset();
        const ConcurrentReads r = concurrent_reads(4, TickClock::kCalibrationNs + 1'000'000, false);
        steps += r.steps;
        reads += r.reads;
        mapped += TickClock::counter_mapped() ? 1 : 0;
    }
    TickClock::test_reset();
    CHECK(reads > 0U);
    CHECK(steps == 0U);
    CHECK(mapped == (TickClock::counter_usable() ? rounds : 0));
}
#endif

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

TEST_CASE("TickClock: prefer_system before the first read keeps every read on MonotonicClock",
          "[diag11b][clocks][tick]")
{
    TickClock::test_reset();
    CHECK(TickClock::prefer_system());
    CHECK(TickClock::prefer_system()); // asked again: still MonotonicClock's
    TickClock::warm();                 // nothing to calibrate: returns at once
    CHECK_FALSE(TickClock::counter_mapped());
    // Past a calibration and a re-anchor period every read is a MonotonicClock read: inside its bracket exactly.
    crd::i64       worst = 0;
    bool           back  = false;
    const crd::u64 reads = bracket_reads(TickClock::kCalibrationNs + TickClock::kReanchorNs, 0, worst, back);
    INFO("worst distance from the MonotonicClock bracket: " << worst << " ns");
    CHECK(reads > 0U);
    CHECK_FALSE(back);
    CHECK_FALSE(TickClock::counter_mapped());
    // Once the clock was read it stays the clock it is (a mapped clock moved to MonotonicClock could step back): only
    // a counter that may not be mapped leaves it on MonotonicClock.
    TickClock::test_reset();
    (void)TickClock::now_ns();
    CHECK(TickClock::prefer_system() == !TickClock::counter_usable());
    TickClock::warm();
    CHECK(TickClock::counter_mapped() == TickClock::counter_usable());
    TickClock::test_reset();
}

TEST_CASE("TickClock: a counter behind its calibration's start leaves every read on MonotonicClock for good",
          "[diag11b][clocks][tick]")
{
    // The counter stands below the origin when the calibration ends (processors whose counters disagree although CPUID
    // calls them invariant): the clock settles on MonotonicClock instead of calibrating on every read.
    TickClock::test_calibrate_from(~u64{0});
    TickClock::warm(); // returns after one calibration span
    CHECK_FALSE(TickClock::counter_mapped());
    CHECK_FALSE(TickClock::counter_usable());
    CHECK(TickClock::prefer_system()); // reads are MonotonicClock's for good
    crd::i64       worst = 0;
    bool           back  = false;
    const crd::u64 reads = bracket_reads(TickClock::kCalibrationNs + TickClock::kReanchorNs, 0, worst, back);
    INFO("worst distance from the MonotonicClock bracket: " << worst << " ns");
    CHECK(reads > 0U);
    CHECK_FALSE(back);
    CHECK_FALSE(TickClock::counter_mapped());
    // A fresh calibration of the same counter maps it again where CPUID calls it invariant.
    TickClock::test_reset();
    TickClock::warm();
    CHECK(TickClock::counter_mapped() == TickClock::counter_usable());
    TickClock::test_reset();
}
#endif
