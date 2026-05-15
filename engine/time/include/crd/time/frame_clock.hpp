#pragma once

// ---------------------------------------------------------------------------
// crd-time -- FrameClock: per-frame timing with fixed-step accumulator
// (Detour D-006).
//
// Three roles:
//   1. Variable-step "render" delta: time between successive `tick()` calls.
//      Used by render path + animation interpolation.
//   2. Fixed-step "simulation" accumulator: collects time into discrete
//      ticks at a configured rate (default 60 Hz). `step_count_pending()`
//      reports how many fixed steps the simulation should run this frame.
//      Used by eylem fixed-step + deterministic replay.
//   3. Interpolation alpha: fractional progress between the last and next
//      fixed step, in [0, 1]. Used by the render path to interpolate
//      rigid-body state for smooth rendering between physics steps.
//
// This is the "Glenn Fiedler integrator" form (Gaffer-on-Games
// "Fix Your Timestep"): accumulate render delta into a fixed-step queue,
// alpha bridges the visual gap.
//
// Usage:
//   crd::time::FrameClock clock{crd::time::Duration{1.0 / 60.0}};  // 60 Hz fixed-step
//   while (running)
//   {
//       clock.tick();
//       // Run physics N times (zero or more).
//       while (clock.consume_fixed_step())
//       {
//           eylem.step(clock.fixed_step_duration());
//       }
//       // Render with interpolation alpha.
//       const f64 alpha = clock.alpha();
//       renderer.render(interpolate(state_prev, state_curr, alpha));
//   }
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/time/clocks.hpp>
#include <crd/time/duration.hpp>
#include <crd/time/instant.hpp>

namespace crd::time
{

class FrameClock
{
public:
    // Construct with a fixed-step duration (default 60 Hz = ~16.667 ms).
    // The first `tick()` seeds the variable-step delta to zero (so consumers
    // don't see a giant first-frame spike representing engine startup).
    explicit FrameClock(Duration fixed_step = Duration{1.0 / 60.0}) noexcept
        : m_fixed_step(fixed_step), m_start(MonotonicClock::now()), m_last_tick(m_start)
    {
    }

    // Advance to the next frame. Computes variable-step delta + accumulates
    // into the fixed-step queue.
    void tick() noexcept
    {
        const Instant now_ = MonotonicClock::now();
        if (!m_seeded)
        {
            // First tick: zero out the delta so the engine doesn't see a
            // multi-second jump from construction-to-first-tick.
            m_last_tick = now_;
            m_seeded = true;
            m_delta = kZeroDuration;
        }
        else
        {
            m_delta = now_ - m_last_tick;
            m_last_tick = now_;
        }
        m_accumulator += m_delta;
        m_frame_count++;
    }

    // Re-anchor the clock as if freshly constructed. Total/delta/frame-count
    // all back to zero.
    void reset() noexcept
    {
        m_start = MonotonicClock::now();
        m_last_tick = m_start;
        m_delta = kZeroDuration;
        m_accumulator = kZeroDuration;
        m_frame_count = 0;
        m_seeded = false;
    }

    // The variable-step delta since the previous tick (zero on first tick).
    [[nodiscard]] Duration delta() const noexcept { return m_delta; }

    // Total wall-time since construction (or last reset()).
    [[nodiscard]] Duration total() const noexcept { return MonotonicClock::now() - m_start; }

    // Number of frames completed (each tick increments).
    [[nodiscard]] crd::u64 frame_count() const noexcept { return m_frame_count; }

    // Convenience accessors (drop-in compat with the former
    // crd::platform::FrameClock API while consumers migrate to Duration).
    [[nodiscard]] crd::f64 delta_seconds() const noexcept { return m_delta.value; }
    [[nodiscard]] crd::f64 total_seconds() const noexcept { return total().value; }

    // The fixed-step duration this clock is configured for.
    [[nodiscard]] Duration fixed_step_duration() const noexcept { return m_fixed_step; }

    // Try to consume one fixed step from the accumulator. Returns true if
    // a step was consumed (the simulation should run one tick); false if
    // the accumulator is below the step threshold.
    //
    // Typical usage: while (clock.consume_fixed_step()) { simulate(); }
    [[nodiscard]] bool consume_fixed_step() noexcept
    {
        if (m_accumulator.value >= m_fixed_step.value)
        {
            m_accumulator -= m_fixed_step;
            m_fixed_step_count++;
            return true;
        }
        return false;
    }

    // Number of completed fixed steps since construction (or reset).
    [[nodiscard]] crd::u64 fixed_step_count() const noexcept { return m_fixed_step_count; }

    // Interpolation alpha in [0, 1]: fractional progress between the most
    // recently completed fixed step and the next pending one. Used by the
    // render path to interpolate physics state for smooth rendering.
    [[nodiscard]] crd::f64 alpha() const noexcept
    {
        if (m_fixed_step.value <= 0.0)
        {
            return 0.0;
        }
        const crd::f64 a = m_accumulator.value / m_fixed_step.value;
        return (a < 0.0) ? 0.0 : (a > 1.0 ? 1.0 : a);
    }

    // Accumulator value (debug introspection — how much time is "owed" the
    // simulation but not yet stepped).
    [[nodiscard]] Duration accumulator() const noexcept { return m_accumulator; }

    // Reconfigure the fixed-step rate at runtime (e.g. user switches from
    // 60 Hz to 144 Hz physics). The accumulator stays; the next consume_fixed_step
    // call uses the new threshold.
    void set_fixed_step_duration(Duration new_step) noexcept { m_fixed_step = new_step; }

private:
    Duration m_fixed_step;
    Instant  m_start;
    Instant  m_last_tick;
    Duration m_delta{};
    Duration m_accumulator{};
    crd::u64 m_frame_count = 0;
    crd::u64 m_fixed_step_count = 0;
    bool     m_seeded = false;
};

} // namespace crd::time
