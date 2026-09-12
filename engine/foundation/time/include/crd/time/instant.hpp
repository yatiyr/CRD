#pragma once

// ---------------------------------------------------------------------------
// crd-time -- Instant: monotonic time point (Detour D-006).
//
// An `Instant` is an opaque absolute time point on the monotonic clock. It
// supports comparison and the difference-yields-Duration arithmetic:
//
//   Instant a = MonotonicClock::now();
//   do_work();
//   Instant b = MonotonicClock::now();
//   Duration elapsed = b - a;  // crd-units::Time<f64> in seconds
//
//   Instant deadline = a + 1.0_s;  // future instant
//   if (MonotonicClock::now() > deadline) { /* timed out */ }
//
// Cannot be constructed from a raw scalar at the public API surface (the
// inner `i64` nanosecond representation is implementation-defined). Get
// one from `MonotonicClock::now()` / `WallClock::now()`.
//
// Internal representation: `i64 ns_since_epoch` (where "epoch" is
// implementation-defined and monotonic — typically system boot for
// steady_clock-backed Instant). Sufficient range: ~292 years at nanosecond
// resolution. Way more than any game/sim session.
//
// Mixed-clock arithmetic (subtracting an Instant from a different clock) is
// **undefined behaviour** — both Instants must come from the same clock.
// Pinned by convention; not enforced at the type level (would require
// per-clock distinct types, which is overkill for v1).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/time/duration.hpp>
#include <crd/units/literals.hpp>

#include <compare>

namespace crd::time
{

class Instant
{
public:
    // Default construction = the zero/null Instant (i.e. epoch). Useful as
    // a sentinel — `Instant t{};` makes a "before-everything" point.
    constexpr Instant() noexcept = default;

    // Construct from raw nanoseconds (used by clock implementations only).
    // Public so impls can build one; consumers should use Clock::now().
    explicit constexpr Instant(crd::i64 ns) noexcept : m_ns(ns) {}

    // Raw access (escape hatch for serialisation, replay log entries, etc.).
    [[nodiscard]] constexpr crd::i64 ns_since_epoch() const noexcept { return m_ns; }

    // Arithmetic: Instant - Instant = Duration
    [[nodiscard]] constexpr Duration operator-(Instant rhs) const noexcept
    {
        const crd::f64 delta_ns = static_cast<crd::f64>(m_ns - rhs.m_ns);
        return Duration{delta_ns * 1.0e-9};
    }

    // Arithmetic: Instant + Duration = Instant (forward in time)
    [[nodiscard]] constexpr Instant operator+(Duration d) const noexcept
    {
        const crd::i64 delta_ns = static_cast<crd::i64>(d.value * 1.0e9);
        return Instant{m_ns + delta_ns};
    }

    // Arithmetic: Instant - Duration = Instant (backward in time)
    [[nodiscard]] constexpr Instant operator-(Duration d) const noexcept
    {
        const crd::i64 delta_ns = static_cast<crd::i64>(d.value * 1.0e9);
        return Instant{m_ns - delta_ns};
    }

    constexpr Instant& operator+=(Duration d) noexcept
    {
        const crd::i64 delta_ns = static_cast<crd::i64>(d.value * 1.0e9);
        m_ns += delta_ns;
        return *this;
    }
    constexpr Instant& operator-=(Duration d) noexcept
    {
        const crd::i64 delta_ns = static_cast<crd::i64>(d.value * 1.0e9);
        m_ns -= delta_ns;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(Instant rhs) const noexcept { return m_ns == rhs.m_ns; }
    [[nodiscard]] constexpr auto operator<=>(Instant rhs) const noexcept { return m_ns <=> rhs.m_ns; }

private:
    crd::i64 m_ns = 0;
};

} // namespace crd::time
