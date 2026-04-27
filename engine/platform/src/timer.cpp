#include <crd/platform/timer.hpp>

namespace crd::platform
{
namespace
{
using clock = std::chrono::steady_clock;

[[nodiscard]] crd::f64 to_seconds(clock::duration d) noexcept
{
    return std::chrono::duration<crd::f64>(d).count();
}
} // namespace

Timer::Timer() noexcept : m_start(clock::now()) {}

void Timer::reset() noexcept
{
    m_start = clock::now();
}

crd::f64 Timer::elapsed_seconds() const noexcept
{
    return to_seconds(clock::now() - m_start);
}

crd::f64 Timer::elapsed_milliseconds() const noexcept
{
    return std::chrono::duration<crd::f64, std::milli>(clock::now() - m_start).count();
}

crd::u64 Timer::elapsed_nanoseconds() const noexcept
{
    return static_cast<crd::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - m_start).count());
}

FrameClock::FrameClock() noexcept : m_start(clock::now()), m_last_tick(m_start) {}

void FrameClock::tick() noexcept
{
    const clock::time_point now = clock::now();
    if (!m_seeded)
    {
        // First tick: do not report a giant delta that's really just engine
        // startup time. Seed the cadence and report zero for this frame.
        m_last_tick = now;
        m_delta_seconds = 0.0;
        m_seeded = true;
    }
    else
    {
        m_delta_seconds = to_seconds(now - m_last_tick);
        m_last_tick = now;
    }
    ++m_frame_count;
}

crd::f64 FrameClock::total_seconds() const noexcept
{
    return to_seconds(clock::now() - m_start);
}

void FrameClock::reset() noexcept
{
    const clock::time_point now = clock::now();
    m_start = now;
    m_last_tick = now;
    m_delta_seconds = 0.0;
    m_frame_count = 0;
    m_seeded = false;
}
} // namespace crd::platform
