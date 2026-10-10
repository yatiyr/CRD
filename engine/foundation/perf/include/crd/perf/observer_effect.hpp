#pragma once

// crd-perf -- observer-effect experiments on a failing reproduction (DIAG.9c).
//
// Instrumentation can change what it observes: a recorder that takes a lock, allocates, reads a file or simply takes
// time can move a race, shift a seeded fault onto another call or hide the failure it was attached to catch. Scope
// timing alone cannot rule that out. An observer-effect experiment runs the same reproduction (a fault plan, a
// schedule script, a recorded input) at three instrumentation levels and compares what failed:
//
//   off    no flight recorder at all: the natural, uninstrumented run (the run gets a null recorder);
//   basic  a fresh FlightRecorder at FlightDetail::Basic (Warning and above, every trigger armed);
//   full   a fresh FlightRecorder at FlightDetail::Full (every event).
//
// Each level makes K runs. The natural runs come first and are kept: they must all fail, and fail the same way, before
// anything instrumented runs (a reproduction that does not fail answers NotReproduced; one whose natural runs disagree
// answers NaturalUnstable, since an observer cannot be judged against an unstable baseline). Then every instrumented
// run must fail exactly as the natural failure did, or the level changed the outcome: ObserverEffect, naming the
// level. A level that reproduced but whose run delivered no capture is InstrumentFailure: the instrument missed the
// failure it was attached to catch, which is a failure of the instrument, never a pass. Only when every level
// reproduced in every run and every instrumented run captured it is the verdict Consistent.
//
// "The same way" is the caller's failure signature (which op, site, hit, frame or status failed), compared exactly. It
// is never a time: each level's elapsed run times are reported as information (the cost of the level) and judged by
// nothing here.
//
// The run receives the level's recorder (nullptr at off) and attaches it to its own instruments (a FaultInjector, a
// FlightFrameWatch, an allocator watch); it must detach it before returning and must not set the recorder's sink,
// which the experiment uses to collect captures. After the run returns the experiment polls the recorder and shuts it
// down, which delivers a capture whose post window is still open (marked shutdown and truncated). The first capture of
// each level's first run is kept as its evidence, beside the natural outcome.
//
// The recorder allocates its ring and its captures from the default allocator, never through an allocator the run
// injects faults into, so an allocation-failure plan counts the same hits at every level by construction. Contract:
// docs/design/runtime-diagnostics.md (DIAG.9c); ADR-0133.

#include <crd/perf/flight_recorder.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{

enum class ObserverLevel : crd::u8
{
    Off   = 0U,
    Basic = 1U,
    Full  = 2U,
};

inline constexpr crd::u32 kObserverLevelCount = 3U;
inline constexpr crd::u32 kObserverMaxRuns    = 64U; // runs per level at most

// "off", "basic", "full".
[[nodiscard]] cont::StringView observer_level_name(ObserverLevel l) noexcept;

// What one run showed. Two runs failed the same way only when both failed with equal signatures.
struct ObservedRun
{
    bool     failed    = false;
    crd::u64 signature = 0U;
};

// One run of the reproduction at a level. `recorder` is nullptr at off.
using ObserverRunOnce = void (*)(void* user, FlightRecorder* recorder, ObservedRun& out);

enum class ObserverVerdict : crd::u8
{
    Consistent        = 0U,
    NotReproduced     = 1U,
    NaturalUnstable   = 2U,
    ObserverEffect    = 3U,
    InstrumentFailure = 4U,
};

// "consistent", "not-reproduced", "natural-unstable", "observer-effect", "instrument-failure".
[[nodiscard]] cont::StringView observer_verdict_name(ObserverVerdict v) noexcept;

struct ObserverLevelResult
{
    ObserverLevel level         = ObserverLevel::Off;
    crd::u32      runs          = 0U;
    crd::u32      failed_runs   = 0U;
    crd::u32      agreeing_runs = 0U; // runs whose outcome equals the natural one (at off: the first natural run's)
    ObservedRun   first;              // the level's first run
    ObservedRun   differing;          // the first run that did not agree (meaningful when agreeing_runs < runs)
    crd::u32      captured_runs  = 0U; // instrumented: runs that delivered at least one capture
    crd::u64      captures       = 0U; // instrumented: captures delivered over every run
    crd::u64      recorded       = 0U; // instrumented: events the first run's recorder recorded
    crd::u64      filtered       = 0U; // instrumented: events the first run's recorder left out (Basic)
    crd::u64      min_elapsed_ns = 0U; // the level's cost, as information: never judged
    crd::u64      max_elapsed_ns = 0U;
    bool          has_capture    = false;
    FlightCapture capture; // instrumented: the first capture of the level's first run, its evidence
};

struct ObserverExperiment
{
    ObserverVerdict     verdict = ObserverVerdict::NotReproduced;
    ObserverLevel       culprit = ObserverLevel::Off; // the level an ObserverEffect or InstrumentFailure names
    ObservedRun         natural;                      // the natural outcome, kept beside every capture
    ObserverLevelResult levels[kObserverLevelCount];  // indexed by ObserverLevel; levels[0] holds the natural runs
    cont::String        reason;                       // the verdict in words
};

// Run the experiment: K = runs_per_level (clamped to [1, kObserverMaxRuns]) natural runs, then K at basic and K at
// full, each instrumented run on a fresh recorder with `base` at that level's detail. A `base` policy the recorder
// refuses runs nothing and answers InstrumentFailure.
void run_observer_experiment(ObserverRunOnce run, void* user, const FlightPolicy& base, crd::u32 runs_per_level,
                             ObserverExperiment& out);

// One JSON object: the verdict, the natural outcome, and per level its runs, agreement, captures, cost and (for an
// instrumented level) its capture's summary (flight_capture_summary_json) and event count.
void observer_experiment_summary_json(const ObserverExperiment& e, cont::String& out);

} // namespace crd::perf
