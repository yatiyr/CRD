#pragma once

// ---------------------------------------------------------------------------
// crd-time -- clocks (Detour D-006).
//
// Three clock types, three use cases:
//
//   MonotonicClock -- the workhorse. Never jumps backward. Immune to wall-
//     clock changes (DST, NTP corrections, manual user clock edits). Use
//     for: measuring intervals, frame delta, profiler, replay timestamps,
//     deadlines. Backed by std::chrono::steady_clock.
//
//   WallClock -- the human clock. Reflects civil time. CAN jump (DST, NTP).
//     Use for: log timestamps, save-file timestamps, "what time is it now"
//     UI display. Backed by std::chrono::system_clock.
//
//   CycleCounter -- the CPU cycle counter (rdtsc on x86, cntvct_el0 on
//     ARM64). ~1 ns resolution but caveats: NOT monotonic across cores on
//     older CPUs; can vary with CPU frequency on cores without
//     invariant-TSC (modern Intel/AMD all have it). Use for: very-fine-
//     grain profiler hot-path timing where 50ns of `now()` overhead is too
//     much. Caller is responsible for pinning the measurement to a single
//     core for correctness on non-invariant-TSC hardware.
//
// All three return `Instant` (monotonic-clock-relative absolute time),
// EXCEPT CycleCounter which returns raw `u64` cycles + a one-time
// calibration to convert to Duration.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/time/duration.hpp>
#include <crd/time/instant.hpp>

namespace crd::time
{

// ===========================================================================
// MonotonicClock -- the canonical clock for the engine.
// ===========================================================================

class MonotonicClock
{
public:
    // Get the current monotonic time. Never jumps. Resolution: ~100 ns on
    // Windows (QueryPerformanceCounter), ~1 ns on Linux (clock_gettime).
    [[nodiscard]] static Instant now() noexcept;
};

// ===========================================================================
// WallClock -- civil time (use for log + save-file timestamps; NOT for
// measuring intervals).
// ===========================================================================

class WallClock
{
public:
    // Get the current wall-clock time. Can jump backward (NTP correction,
    // DST, user clock change). NEVER use for measuring intervals — use
    // MonotonicClock for that.
    //
    // The returned Instant uses Unix epoch (1970-01-01 UTC) as t=0.
    [[nodiscard]] static Instant now() noexcept;
};

// ===========================================================================
// CycleCounter -- raw CPU cycles (fastest possible "now"; coarse-grained).
// ===========================================================================

class CycleCounter
{
public:
    // Read the CPU cycle counter. Cost: ~5 cycles (~1-2 ns on modern x86).
    //
    // Caveats:
    //   - May vary across cores on older non-invariant-TSC CPUs. Pin the
    //     measurement to a single core for accuracy on those (modern Intel
    //     i5/i7/Xeon Nehalem+ and AMD Bulldozer+ all have invariant TSC,
    //     so this is mostly a non-issue today).
    //   - Resolution: nominal CPU clock period (~0.3 ns at 3 GHz).
    //   - Frequency may NOT match the CPU's variable boost frequency on
    //     non-invariant-TSC parts.
    //
    // To convert to Duration, callers compute the calibration factor via
    // `calibrate()` (done once at startup; takes ~10 ms of wall-time).
    [[nodiscard]] static crd::u64 now() noexcept;

    // Measure the cycle-counter-per-second rate by sampling MonotonicClock
    // for the given calibration duration. Default 10 ms.
    //
    // Returns cycles-per-second (the rate at which `now()` increments).
    // Multiply (delta_cycles * (1.0 / cycles_per_second)) to get Duration.
    //
    // Call once at engine startup; cache the result in your profiler.
    [[nodiscard]] static crd::f64 calibrate(Duration calibration_window = Duration{0.01}) noexcept;

    // Convenience: convert a cycle-count delta to a Duration using a
    // pre-computed cycles-per-second rate.
    [[nodiscard]] static constexpr Duration cycles_to_duration(crd::u64 delta_cycles,
                                                                  crd::f64 cycles_per_second) noexcept
    {
        return Duration{static_cast<crd::f64>(delta_cycles) / cycles_per_second};
    }
};

} // namespace crd::time
