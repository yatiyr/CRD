#pragma once

// ---------------------------------------------------------------------------
// crd-time -- Deadline + sleep primitives (Detour D-006).
//
// A `Deadline` is an Instant (in the MonotonicClock domain) by which some
// work must complete. Used for:
//   - Async resource loading timeouts.
//   - Network RTT deadlines (Phase 4.2).
//   - Per-frame budget guards ("if I have less than 2 ms of frame budget
//     left, defer this work").
//   - Job scheduler "no later than" hints.
//
// SleepFor / SleepUntil block the calling thread. YieldThread is a hint to
// the scheduler to give up the current quantum but stay runnable.
// ---------------------------------------------------------------------------

#include <crd/time/clocks.hpp>
#include <crd/time/duration.hpp>
#include <crd/time/instant.hpp>

namespace crd::time
{

// ===========================================================================
// Deadline -- an Instant in the future (or past).
// ===========================================================================

class Deadline
{
public:
    // Construct a Deadline at the given absolute Instant.
    explicit constexpr Deadline(Instant when) noexcept : m_when(when) {}

    // Construct a Deadline `offset` Duration from now.
    [[nodiscard]] static Deadline from_now(Duration offset) noexcept
    {
        return Deadline{MonotonicClock::now() + offset};
    }

    // The absolute Instant this deadline fires at.
    [[nodiscard]] constexpr Instant when() const noexcept { return m_when; }

    // Has the deadline passed?
    [[nodiscard]] bool expired() const noexcept { return MonotonicClock::now() >= m_when; }

    // Time until the deadline. Returns a negative-valued Duration if the
    // deadline has already passed.
    [[nodiscard]] Duration remaining() const noexcept { return m_when - MonotonicClock::now(); }

private:
    Instant m_when;
};

// ===========================================================================
// Sleep / yield primitives
// ===========================================================================

// Sleep the current thread for `duration` (or longer — the OS may oversleep).
// Implementation: std::this_thread::sleep_for under the hood. Resolution:
// typically 1 ms on Windows, microseconds on Linux.
//
// For sub-millisecond precision, prefer a spin-wait loop on MonotonicClock::now().
void sleep_for(Duration duration) noexcept;

// Sleep the current thread until `deadline.when()`. Returns immediately if
// the deadline has already passed.
void sleep_until(Deadline deadline) noexcept;

// Hint to the OS scheduler to yield the current quantum but stay runnable.
// Implementation: std::this_thread::yield.
void yield_thread() noexcept;

} // namespace crd::time
