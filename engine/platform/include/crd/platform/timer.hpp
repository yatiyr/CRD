#pragma once

#include <crd/core/types.hpp>

#include <chrono>

namespace crd::platform
{
// Timer — a simple monotonic stopwatch built on std::chrono::steady_clock.
//
// Deliberately decoupled from GLFW: GLFW exposes glfwGetTime() but tying
// engine timing to the windowing backend would force a Window/Context to
// exist before anything could measure elapsed time. steady_clock is
// monotonic, never jumps, and is available without any backend.
//
// Construction starts the clock. `reset()` re-anchors the start point.
// `elapsed_*()` returns the time since the most recent start/reset.
class Timer
{
public:
    Timer() noexcept;

    void reset() noexcept;

    [[nodiscard]] crd::f64 elapsed_seconds() const noexcept;
    [[nodiscard]] crd::f64 elapsed_milliseconds() const noexcept;
    [[nodiscard]] crd::u64 elapsed_nanoseconds() const noexcept;

private:
    std::chrono::steady_clock::time_point m_start{};
};

// FrameClock — per-frame timing facade for the main loop.
//
// Call `tick()` once at the top of every frame. After the call, the
// "delta" accessors return the time between the previous and the
// current tick. The first call seeds the clock; its delta is reported
// as zero so the caller doesn't see a giant first-frame spike that
// represents engine startup time.
//
// Total time is measured from FrameClock construction (not from the
// first tick), and frame_count() reports the number of completed
// ticks.
class FrameClock
{
public:
    FrameClock() noexcept;

    // Advance to the next frame. Updates delta and total accessors.
    void tick() noexcept;

    [[nodiscard]] crd::f64 delta_seconds() const noexcept { return m_delta_seconds; }
    [[nodiscard]] crd::f64 total_seconds() const noexcept;
    [[nodiscard]] crd::u64 frame_count() const noexcept { return m_frame_count; }

    // Reset the clock back to a freshly-constructed state. Total time,
    // delta, and frame count all return to zero.
    void reset() noexcept;

private:
    std::chrono::steady_clock::time_point m_start{};
    std::chrono::steady_clock::time_point m_last_tick{};
    crd::f64 m_delta_seconds = 0.0;
    crd::u64 m_frame_count = 0;
    bool m_seeded = false;
};
} // namespace crd::platform
