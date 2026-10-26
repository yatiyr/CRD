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
//   TickClock -- MonotonicClock's timeline read from the cycle counter: for
//     the profiler's and the diagnostics' per-event timestamps (below).
//
//   CycleCounter -- the CPU cycle counter (rdtsc on x86, cntvct_el0 on
//     ARM64). ~1 ns resolution but caveats: NOT monotonic across cores on
//     older CPUs; can vary with CPU frequency on cores without
//     invariant-TSC (modern Intel/AMD all have it). Use for: very-fine-
//     grain profiler hot-path timing where 50ns of `now()` overhead is too
//     much. Caller is responsible for pinning the measurement to a single
//     core for correctness on non-invariant-TSC hardware.
//
// Every clock returns `Instant` (monotonic-clock-relative absolute time),
// EXCEPT CycleCounter which returns raw `u64` cycles + a one-time
// calibration to convert to Duration.
// ---------------------------------------------------------------------------

#include <crd/core/build_config.hpp>
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
// TickClock -- MonotonicClock's timeline, read from the cycle counter.
//
// The profiler's zones and frame marks and the diagnostic clock read the time at every event (DIAG.11b clause (8):
// against Tracy's zones, a Cerid zone spent its difference on its two MonotonicClock reads). MonotonicClock is
// std::chrono::steady_clock: QueryPerformanceCounter and a division on Windows (10 MHz there), clock_gettime on Linux.
// TickClock reads the cycle counter (one instruction, the CPU's resolution) and maps it onto MonotonicClock's
// nanoseconds: an anchor (ticks, nanoseconds) and a slope, both measured against MonotonicClock.
//
//   - The first reads return MonotonicClock itself while the counter is calibrated against it (kCalibrationNs of
//     MonotonicClock time from the first read); so does every read on a CPU whose counter is not invariant (x86-64
//     without CPUID's invariant-TSC bit; on ARM64 the generic timer always is). No read waits for a calibration.
//   - Afterwards a read is the counter scaled from the anchor. The reader that finds the anchor older than
//     kReanchorNs re-anchors it at MonotonicClock (one MonotonicClock read per period, whichever thread reads), with
//     the slope measured over everything since the calibration began, so the map stays within the slope's error of
//     MonotonicClock and that error shrinks as the baseline grows.
//   - It never steps back: a re-anchor never moves the map below where it stood, and when the map ran ahead of
//     MonotonicClock the next period runs slower until MonotonicClock catches up.
// ===========================================================================

class TickClock
{
public:
    // MonotonicClock's calibration span before the counter is used, and the longest an anchor serves.
    static constexpr crd::i64 kCalibrationNs = 2'000'000;
    static constexpr crd::i64 kReanchorNs    = 8'000'000;

    // Nanoseconds on MonotonicClock's timeline (Instant::ns_since_epoch's).
    [[nodiscard]] static crd::i64 now_ns() noexcept;
    // Calibrate now rather than on the first reads: returns once reads come from the counter (or never can), after at
    // most kCalibrationNs of MonotonicClock reads the first time in a process and at once afterwards. A run that
    // turns per-event timestamps on calls it (the profiler's init, the diagnostic host's start).
    static void warm() noexcept;
    [[nodiscard]] static Instant  now() noexcept { return Instant{now_ns()}; }

    // Whether this CPU's counter may be mapped (invariant), and whether reads come from it now (false while it is
    // calibrated, and for good where it may not be).
    [[nodiscard]] static bool counter_usable() noexcept;
    [[nodiscard]] static bool counter_mapped() noexcept;

#if CRD_ENABLE_ASSERTS
    // Test seams, compiled only where asserts are (never in a shipping build), for a single-threaded test.
    // Map the counter as `ns = ns_at + (ticks - ticks_at) * ns_per_tick`, re-anchoring no more (a deliberately wrong
    // slope shows whether a consumer reads this clock).
    static void test_force_map(crd::u64 ticks_at, crd::i64 ns_at, crd::f64 ns_per_tick) noexcept;
    // Start over: the next read calibrates again.
    static void test_reset() noexcept;
#endif
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
