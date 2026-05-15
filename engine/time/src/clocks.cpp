// crd-time -- clock implementations (Detour D-006).

#include <crd/time/clocks.hpp>

#include <chrono>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace crd::time
{

// ===========================================================================
// MonotonicClock
// ===========================================================================

Instant MonotonicClock::now() noexcept
{
    const auto tp = std::chrono::steady_clock::now();
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return Instant{static_cast<crd::i64>(since_epoch)};
}

// ===========================================================================
// WallClock
// ===========================================================================

Instant WallClock::now() noexcept
{
    const auto tp = std::chrono::system_clock::now();
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return Instant{static_cast<crd::i64>(since_epoch)};
}

// ===========================================================================
// CycleCounter
// ===========================================================================

crd::u64 CycleCounter::now() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<crd::u64>(hi) << 32) | static_cast<crd::u64>(lo);
#elif defined(__aarch64__)
    crd::u64 val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Fallback: derive a "cycle"-ish counter from the monotonic clock's
    // nanoseconds. Not great but at least monotonic.
    return static_cast<crd::u64>(MonotonicClock::now().ns_since_epoch());
#endif
}

crd::f64 CycleCounter::calibrate(Duration calibration_window) noexcept
{
    // Spin-loop calibration: measure cycles per unit of wall-clock time.
    const Instant wall_start = MonotonicClock::now();
    const crd::u64 cyc_start = CycleCounter::now();
    const Instant wall_end_target = wall_start + calibration_window;

    while (MonotonicClock::now() < wall_end_target)
    {
        // Busy-wait. The calibration window should be small (default 10 ms).
    }

    const crd::u64 cyc_end = CycleCounter::now();
    const Instant wall_end = MonotonicClock::now();

    const Duration elapsed = wall_end - wall_start;
    const crd::u64 cycles = cyc_end - cyc_start;

    if (elapsed.value <= 0.0)
    {
        return 0.0;  // degenerate (should not happen with sane inputs)
    }
    return static_cast<crd::f64>(cycles) / elapsed.value;
}

} // namespace crd::time
