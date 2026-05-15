#pragma once

// ---------------------------------------------------------------------------
// crd-time -- DeterministicClock: integer-tick clock for replay + lockstep
// (Detour D-006).
//
// Unlike MonotonicClock (which reports wall-clock-ish time and is non-
// reproducible across runs), DeterministicClock is driven SOLELY by explicit
// `tick()` / `advance(N)` calls. Its state is a `u64 tick_count` + a fixed
// `Duration tick_period` — together giving an exact `Duration` "now".
//
// Use cases:
//   - Deterministic replay (D-004): the replay scenario records ticks +
//     inputs; on playback, `tick()` is called manually in lockstep with the
//     replay log.
//   - Networked lockstep (Phase 4.2): all clients run on the same number of
//     ticks; the clock advances only when ALL clients have submitted their
//     inputs for that tick.
//   - Test / fuzz harnesses: drive simulation forward in known increments.
//
// Default tick period: 1/60 s (matches the typical fixed-step physics rate).
// Configurable per-instance.
//
// **Why integer ticks (not Duration accumulator).** Bit-exact reproducibility
// across compilers / SIMD widths / OSes requires integer arithmetic. The
// "now" Duration is derived ON DEMAND from `tick_count * tick_period`, never
// accumulated as f64. This is the ADR-0063 determinism contract applied to
// time itself.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/time/duration.hpp>

namespace crd::time
{

class DeterministicClock
{
public:
    explicit constexpr DeterministicClock(Duration tick_period = Duration{1.0 / 60.0}) noexcept
        : m_tick_period(tick_period)
    {
    }

    // Advance one tick.
    constexpr void tick() noexcept { ++m_tick_count; }

    // Advance N ticks.
    constexpr void advance(crd::u64 num_ticks) noexcept { m_tick_count += num_ticks; }

    // Reset to tick 0.
    constexpr void reset() noexcept { m_tick_count = 0; }

    // Current tick number (0 at construction).
    [[nodiscard]] constexpr crd::u64 tick_count() const noexcept { return m_tick_count; }

    // The tick period (Duration between consecutive ticks).
    [[nodiscard]] constexpr Duration tick_period() const noexcept { return m_tick_period; }

    // "Now" as a Duration since tick 0. Always equals
    // `tick_count * tick_period` exactly — integer multiply * f64 (one
    // rounding step at the last moment).
    [[nodiscard]] constexpr Duration elapsed() const noexcept
    {
        return Duration{static_cast<crd::f64>(m_tick_count) * m_tick_period.value};
    }

    // Reconfigure the tick rate (rare; typically locked at construction
    // for deterministic replay).
    constexpr void set_tick_period(Duration new_period) noexcept { m_tick_period = new_period; }

private:
    Duration m_tick_period;
    crd::u64 m_tick_count = 0;
};

} // namespace crd::time
