#pragma once

// ---------------------------------------------------------------------------
// crd-time -- Stopwatch (Detour D-006).
//
// Simple monotonic stopwatch backed by MonotonicClock. Two forms:
//
//   Stopwatch sw;
//   sw.start();
//   do_work();
//   Duration d = sw.elapsed();   // continues running
//   sw.stop();                   // freezes elapsed
//   sw.reset();                  // back to zero
//
//   {
//       ScopedStopwatch sw{[](Duration d) { CRD_LOG_INFO("foo took {}ms", d.value_in<Millisecond>()); }};
//       do_work();
//   }  // ~ScopedStopwatch fires the callback with the elapsed Duration
//
// Or via the macro:
//   void some_function()
//   {
//       CRD_TIME_SCOPED_STOPWATCH(my_timer);  // creates a ScopedStopwatch
//       do_work();                             // measured
//   }
//
// The macro version writes the elapsed Duration to a caller-provided lambda
// or output object (D-003 profiler hooks here).
// ---------------------------------------------------------------------------

#include <crd/time/clocks.hpp>
#include <crd/time/duration.hpp>
#include <crd/time/instant.hpp>

namespace crd::time
{

// ===========================================================================
// Stopwatch -- explicit start/stop/elapsed/reset.
// ===========================================================================

class Stopwatch
{
public:
    // Construction starts the clock (matches the v1 platform::Timer
    // convention). Call `reset()` to re-anchor.
    Stopwatch() noexcept : m_start(MonotonicClock::now()), m_stopped_elapsed{} {}

    // Re-anchor the start point to NOW. Resets any frozen "stopped" state.
    void start() noexcept
    {
        m_start = MonotonicClock::now();
        m_running = true;
        m_stopped_elapsed = kZeroDuration;
    }

    // Freeze the elapsed value at the current moment. Subsequent calls to
    // `elapsed()` return the frozen value until `start()` or `reset()` is called.
    void stop() noexcept
    {
        if (m_running)
        {
            m_stopped_elapsed = MonotonicClock::now() - m_start;
            m_running = false;
        }
    }

    // Reset back to a freshly-constructed state (running, elapsed = 0).
    void reset() noexcept
    {
        m_start = MonotonicClock::now();
        m_running = true;
        m_stopped_elapsed = kZeroDuration;
    }

    [[nodiscard]] Duration elapsed() const noexcept
    {
        if (m_running)
        {
            return MonotonicClock::now() - m_start;
        }
        return m_stopped_elapsed;
    }

    // Convenience accessors (drop-in compat with the former
    // crd::platform::Timer API while consumers migrate to Duration).
    [[nodiscard]] crd::f64 elapsed_seconds() const noexcept { return elapsed().value; }
    [[nodiscard]] crd::f64 elapsed_milliseconds() const noexcept { return elapsed().value * 1000.0; }
    [[nodiscard]] crd::u64 elapsed_nanoseconds() const noexcept
    {
        return static_cast<crd::u64>(elapsed().value * 1.0e9);
    }

    [[nodiscard]] bool running() const noexcept { return m_running; }

private:
    Instant  m_start;
    Duration m_stopped_elapsed{};
    bool     m_running = true;
};

// ===========================================================================
// ScopedStopwatch -- RAII stopwatch that delivers the elapsed Duration to a
// callback on destruction.
// ===========================================================================

template <typename Callback>
class ScopedStopwatch
{
public:
    explicit ScopedStopwatch(Callback cb) noexcept
        : m_start(MonotonicClock::now()), m_callback(static_cast<Callback&&>(cb))
    {
    }

    ScopedStopwatch(const ScopedStopwatch&) = delete;
    ScopedStopwatch& operator=(const ScopedStopwatch&) = delete;
    ScopedStopwatch(ScopedStopwatch&&) = delete;
    ScopedStopwatch& operator=(ScopedStopwatch&&) = delete;

    ~ScopedStopwatch() noexcept
    {
        const Duration elapsed = MonotonicClock::now() - m_start;
        m_callback(elapsed);
    }

private:
    Instant  m_start;
    Callback m_callback;
};

// Deduction guide so users can write `ScopedStopwatch{lambda}` without
// spelling the callback type.
template <typename Callback>
ScopedStopwatch(Callback) -> ScopedStopwatch<Callback>;

} // namespace crd::time

// ===========================================================================
// Macros
// ===========================================================================

// CRD_TIME_SCOPED_STOPWATCH(name) -- declares a ScopedStopwatch in the
// enclosing scope that writes elapsed to a Duration variable `name_elapsed`.
// On scope exit, `name_elapsed` holds the elapsed duration.
//
// Usage:
//   void load_asset()
//   {
//       crd::time::Duration load_time_elapsed;
//       CRD_TIME_SCOPED_STOPWATCH_AS(load_time, load_time_elapsed);
//       // ... actual work ...
//   }
//   // Now `load_time_elapsed` holds the duration.
#define CRD_TIME_SCOPED_STOPWATCH_AS(name, out_duration) \
    crd::time::ScopedStopwatch name##_crd_sw{[&](crd::time::Duration d) noexcept { (out_duration) = d; }}

// CRD_TIME_SCOPED_STOPWATCH(callback) -- declares a ScopedStopwatch in the
// enclosing scope that fires `callback` with the elapsed Duration on scope
// exit. Useful for one-line profiler hooks.
//
// Usage:
//   CRD_TIME_SCOPED_STOPWATCH([](crd::time::Duration d) { CRD_LOG_INFO("ms: {}", d.value * 1000.0); });
#define CRD_TIME_SCOPED_STOPWATCH(callback) \
    crd::time::ScopedStopwatch CRD_TIME_INTERNAL_CAT(crd_sw_, __LINE__){callback}

#define CRD_TIME_INTERNAL_CAT_INNER(a, b) a##b
#define CRD_TIME_INTERNAL_CAT(a, b) CRD_TIME_INTERNAL_CAT_INNER(a, b)
