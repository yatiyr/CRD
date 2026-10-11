#pragma once

// Workload oracles: warm/cold, soak, repeated-reload and shutdown runs with one changed stressor at a time, seeded
// failure injection, progress and measured slowdowns (DIAG.10c).
//
// A workload is something the engine does over and over: init and shut down the job system, reload a pack, run a
// long offline computation. Run once, it shows little; run many times, it shows what a single run hides: a result
// that changes after the first (warm) run, a handle or thread left behind by each shutdown, a failure that does not
// recover, a run that stops making progress, a run that gets slower the longer it goes. `run_workload` drives one
// such run and judges it with the probes below (docs/design/runtime-diagnostics.md, DIAG.10c).
//
// Shape of a run. `cycles` times: setup, then `iterations` iterations, then teardown. Iteration 0 of each cycle is the
// cold one (just after setup: nothing cached, nothing resident); the rest are warm. Every iteration reports whether it
// succeeded, its result signature and, when it knows it, the signature it should have; one whose expected signature
// is unknown must equal the reference, the signature of cycle 0's cold iteration, so a warm or a later cycle's run that
// computes something else is caught. A lifetime-ledger window brackets each cycle, setup to teardown, so whatever a
// cycle leaves behind is named by class.
//
// Seeded failure injection. When the spec gives an injection hook, iteration i of cycle c is injected exactly when
// `workload_injects(seed, permille, c, i, iterations)` says so: a pure function of its arguments, never iteration 0
// (the cold reference) and never the last (so a clean iteration follows every injected one). The hook arms a fault
// before the iteration and reports after it how many fired. An injected iteration must report failure (the fault was
// surfaced) and keep its expected signature when it states one (a failure keeps the last good result); the next
// iteration must succeed with its expected result (it recovered). A fault that never fired proves nothing about the
// path it was meant to reach and is reported as such.
//
// One changed stressor at a time. A spec names its stressors (`StressorSet`: the iteration and cycle counts, threads,
// the fault plan, a memory budget, the reload cadence, the payload size, an injected delay). A finding is attributed
// to a stressor only by comparing a variant with a clean baseline that differs from it in exactly that stressor
// (`attribute_finding`); a variant that changes two stressors at once, or a baseline that is not clean, attributes
// nothing.
//
// Progress and slowdowns are measured before they are classified. `progress_report` judges readings (when, how much
// done): a run stalls when no advance is observed for longer than its bound. `slowdown_report` compares two windows of
// durations by their quartiles: it says Slower only when the later window's median exceeds the earlier one's by more
// than the tolerance AND the two windows' quartiles do not overlap, and it classifies nothing from too few samples, a
// baseline too short to time or a baseline whose own spread is too wide. A workload's own slowdown report compares its
// first and last warm clean iterations. Wall-clock verdicts depend on the machine; hosted lanes judge progress, results
// and lifetimes, and record the timing verdict as evidence.
//
// Everything here is integer arithmetic over caller data; `run_workload` reads a steady clock and the lifetime ledger.
// No allocation: the result holds its samples in fixed arrays (kWorkloadMaxSamples).

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/types.hpp>

namespace crd
{

// ---- Stressors --------------------------------------------------------------------------------------------------

// What a workload variant may change, one at a time.
// NOLINTNEXTLINE(performance-enum-size)
enum class Stressor : crd::u8
{
    Iterations,    // iterations per cycle (a soak is a long one)
    Cycles,        // setup/teardown cycles (a shutdown workload repeats them)
    Threads,       // worker threads
    FaultPlan,     // the seeded fault plan (0: none; otherwise an id of the plan, its seed and share)
    MemoryBudget,  // a memory budget in bytes (0: none)
    ReloadCadence, // iterations between reloads (0: never)
    PayloadBytes,  // the size of the data each iteration handles
    Delay,         // an injected delay in nanoseconds (0: none)
};
inline constexpr crd::usize kStressorCount = 8U;
[[nodiscard]] const char* stressor_name(Stressor s) noexcept;

struct StressorSet
{
    crd::u64 values[kStressorCount]{};

    void set(Stressor s, crd::u64 value) noexcept
    {
        values[static_cast<crd::usize>(s)] = value;
    }
    [[nodiscard]] crd::u64 of(Stressor s) const noexcept
    {
        return values[static_cast<crd::usize>(s)];
    }
};

// NOLINTNEXTLINE(performance-enum-size)
enum class StressorChange : crd::u8
{
    Identical,      // the variant is the baseline
    OneChanged,     // exactly one stressor differs: findings can be attributed to it
    SeveralChanged, // more than one differs: nothing can be attributed
};
[[nodiscard]] const char* stressor_change_name(StressorChange c) noexcept;

struct StressorComparison
{
    StressorChange change  = StressorChange::Identical;
    Stressor       changed = Stressor::Iterations; // the stressor, when exactly one changed
    crd::u32       mask    = 0U;                   // bit i set when stressor i differs
};

[[nodiscard]] StressorComparison compare_stressors(const StressorSet& baseline, const StressorSet& variant) noexcept;

// ---- Slowdown -----------------------------------------------------------------------------------------------------

// The most samples one window can be judged over (a longer series must be windowed by the caller).
inline constexpr crd::usize kSlowdownMaxSamples = 1024U;

// Order statistics of one window, by nearest rank on the sorted samples: the q-th quartile is element
// floor((n - 1) * q / 4).
struct DurationSummary
{
    crd::u32 samples = 0U;
    crd::u64 min     = 0U;
    crd::u64 p25     = 0U;
    crd::u64 median  = 0U;
    crd::u64 p75     = 0U;
    crd::u64 max     = 0U;
};

// False (and `out` left with only `samples` set) when `count` is 0 or above kSlowdownMaxSamples.
bool summarize_durations(const crd::u64* ns, crd::usize count, DurationSummary& out) noexcept;

struct SlowdownPolicy
{
    crd::u32 min_samples        = 8U;    // fewer in either window: Unmeasured
    crd::u64 min_baseline_ns    = 1000U; // a baseline median below this is too short to time: Unmeasured
    crd::u32 tolerance_permille = 250U;  // a median moving less than this share is Steady
    crd::u32 noise_permille     = 1000U; // a baseline whose interquartile range exceeds this share of its median: Noisy
};

// The worst finding last.
// NOLINTNEXTLINE(performance-enum-size)
enum class SlowdownVerdict : crd::u8
{
    Unmeasured,   // too few samples, or a baseline too short to time: nothing is classified
    Noisy,        // the baseline's own spread is too wide to classify against
    Steady,       // the median moved less than the tolerance
    Faster,       // the median fell by more than the tolerance and the quartiles separate
    Inconclusive, // the median moved by more than the tolerance but the quartiles overlap
    Slower,       // the median rose by more than the tolerance and the quartiles separate
};
[[nodiscard]] const char* slowdown_verdict_name(SlowdownVerdict v) noexcept;

struct SlowdownReport
{
    SlowdownVerdict verdict = SlowdownVerdict::Unmeasured;
    DurationSummary baseline;
    DurationSummary candidate;
    crd::u64        ratio_permille = 0U; // candidate median * 1000 / baseline median (saturated; 0 when unmeasured)
};

// Judge `candidate` against `baseline`, each a window of durations in nanoseconds.
[[nodiscard]] SlowdownReport slowdown_report(const crd::u64* baseline, crd::usize baseline_count,
                                             const crd::u64* candidate, crd::usize candidate_count,
                                             const SlowdownPolicy& policy) noexcept;

// One line, e.g. "slower: median 2400 ns -> 9100 ns (3791 permille), quartiles separate"; returns the length written
// (at most capacity - 1, always terminated when capacity > 0).
crd::usize format_slowdown_report(const SlowdownReport& r, char* out, crd::usize capacity) noexcept;

// ---- Progress -----------------------------------------------------------------------------------------------------

struct ProgressReading
{
    crd::u64 at_ns = 0U; // when, from any fixed origin
    crd::u64 done  = 0U; // how much work had completed by then (monotonic)
};

struct ProgressPolicy
{
    crd::u64 stall_ns     = 0U; // no advance for longer than this is a stall; 0: Unmeasured
    crd::u64 target       = 0U; // the work the run must complete; 0: no target
    crd::u32 min_readings = 2U;
};

// The worst finding last.
// NOLINTNEXTLINE(performance-enum-size)
enum class ProgressVerdict : crd::u8
{
    Unmeasured,  // too few readings, no stall bound, or readings out of time order
    Progressing, // advances never more than the bound apart, and the target (if any) reached
    Unfinished,  // never stalled, but ended below the target
    Stalled,     // no advance for longer than the bound
    Regressed,   // the completed count went back: the progress counter itself is wrong
};
[[nodiscard]] const char* progress_verdict_name(ProgressVerdict v) noexcept;

struct ProgressReport
{
    ProgressVerdict verdict      = ProgressVerdict::Unmeasured;
    crd::u32        readings     = 0U;
    crd::u64        longest_gap  = 0U; // the longest time between consecutive advances (or the start and the first)
    crd::u64        stalled_from = 0U; // at_ns of the last advance before the first stall
    crd::u64        done         = 0U; // the last reading's count
};

[[nodiscard]] ProgressReport progress_report(const ProgressReading* readings, crd::usize count,
                                             const ProgressPolicy& policy) noexcept;

// ---- Workloads ----------------------------------------------------------------------------------------------------

inline constexpr crd::usize kWorkloadMaxSamples = 1024U; // iterations timed per run; later ones are judged, not timed

// What one iteration reports.
struct WorkloadIteration
{
    bool     ok             = false; // the iteration did what it was asked
    crd::u64 signature      = 0U;    // a digest of its result (or of the state a failure kept)
    crd::u64 expected       = 0U;    // the signature it should have, when `expected_known`
    bool     expected_known = false; // false: it must equal the reference (cycle 0's cold signature)
    crd::u64 progress       = 1U;    // work units it completed
};

using WorkloadSetup     = bool (*)(void* user, crd::u32 cycle) noexcept;
using WorkloadIterate   = void (*)(void* user, crd::u32 cycle, crd::u32 iteration, bool injected,
                                 WorkloadIteration& out) noexcept;
using WorkloadTeardown  = void (*)(void* user, crd::u32 cycle) noexcept;
using WorkloadArmFault  = void (*)(void* user, crd::u32 cycle, crd::u32 iteration) noexcept;
using WorkloadDisarm    = crd::u32 (*)(void* user) noexcept; // returns how many faults fired since the arm

struct WorkloadSpec
{
    const char*      name       = "";
    StressorSet      stressors;
    crd::u32         cycles     = 1U;
    crd::u32         iterations = 1U;
    void*            user       = nullptr;
    WorkloadSetup    setup      = nullptr; // optional
    WorkloadIterate  iterate    = nullptr; // required
    WorkloadTeardown teardown   = nullptr; // optional
    WorkloadArmFault arm        = nullptr; // with `disarm`: seeded failure injection
    WorkloadDisarm   disarm     = nullptr;
    crd::u64         fault_seed    = 0U;
    crd::u32         fault_permille = 0U; // share of eligible iterations injected (1000: all of them)
    crd::u64         stall_ns       = 0U; // the progress bound; 0: progress is not judged
    LifetimeLedger*  ledger         = nullptr; // nullptr: the process-wide ledger
};

// Whether iteration `iteration` of cycle `cycle` is injected: never iteration 0 nor the last of a cycle, only odd
// iterations (so the next one is clean), and then when splitmix64 of (seed, cycle, iteration) falls below the share.
[[nodiscard]] bool workload_injects(crd::u64 seed, crd::u32 permille, crd::u32 cycle, crd::u32 iteration,
                                    crd::u32 iterations) noexcept;

// The worst finding last.
// NOLINTNEXTLINE(performance-enum-size)
enum class WorkloadVerdict : crd::u8
{
    Clean,           // every check passed
    FaultNotFired,   // an injected iteration's fault never fired: the injection proved nothing
    Stalled,         // progress stalled (see the progress report)
    Leaked,          // a cycle left something behind or underflowed the ledger (see the worst cycle's window)
    IterationFailed, // a clean iteration failed
    NotRecovered,    // the iteration after an injected one failed or computed something else
    FaultMissed,     // a fault fired and the iteration reported success: the failure was not surfaced
    Diverged,        // an iteration's signature differs from what it should be: a wrong result
    SetupFailed,     // a cycle could not be set up: nothing after it ran
};
[[nodiscard]] const char* workload_verdict_name(WorkloadVerdict v) noexcept;

struct WorkloadResult
{
    WorkloadVerdict verdict = WorkloadVerdict::Clean;
    StressorSet     stressors;
    crd::u32        finding_cycle     = 0U; // where the verdict's first occurrence was found
    crd::u32        finding_iteration = 0U;

    crd::u32 cycles_run          = 0U;
    crd::u64 iterations_run      = 0U;
    crd::u64 failed_iterations   = 0U; // clean iterations that failed
    crd::u64 injected_iterations = 0U;
    crd::u64 faults_fired        = 0U;
    crd::u64 detected            = 0U; // injected iterations whose fault fired and that reported failure
    crd::u64 recovered           = 0U; // iterations after an injected one that succeeded with the expected result
    crd::u64 diverged            = 0U;

    crd::u64 reference = 0U; // cycle 0's cold signature

    LifetimeReport worst_cycle; // the worst cycle window (setup to teardown)
    crd::u32       worst_cycle_index = 0U;

    ProgressReport progress;
    SlowdownReport slowdown; // first versus last warm clean iterations: evidence, not a verdict

    crd::u64 cold_ns[kWorkloadMaxSamples]{}; // each cycle's iteration 0 (up to kWorkloadMaxSamples cycles)
    crd::u32 cold_count = 0U;
    crd::u64 warm_ns[kWorkloadMaxSamples]{}; // warm clean iterations in run order (excluding recovery iterations)
    crd::u32 warm_count = 0U;
};

// Run `spec` and judge it. A spec without `iterate`, with zero cycles or iterations, or with only one of `arm` and
// `disarm` runs nothing and answers SetupFailed.
void run_workload(const WorkloadSpec& spec, WorkloadResult& out) noexcept;

// One line, e.g. "reload-soak: leaked in cycle 2 (outstanding: thread +1); 3 cycles, 96 iterations, 12 injected, 12
// detected, 12 recovered; progress progressing; timing steady (1043 permille)".
crd::usize format_workload_result(const char* name, const WorkloadResult& r, char* out, crd::usize capacity) noexcept;

// ---- Attribution --------------------------------------------------------------------------------------------------

// NOLINTNEXTLINE(performance-enum-size)
enum class FindingAttribution : crd::u8
{
    NoNewFinding,    // the variant is clean too
    Attributed,      // the baseline is clean, the variant is not, and exactly one stressor differs
    BaselineFailing, // the baseline itself is not clean: nothing can be attributed
    NotOneStressor,  // the variant changes no stressor or several: nothing can be attributed
};
[[nodiscard]] const char* finding_attribution_name(FindingAttribution a) noexcept;

struct AttributionReport
{
    FindingAttribution attribution = FindingAttribution::NoNewFinding;
    Stressor           stressor    = Stressor::Iterations; // when Attributed
    WorkloadVerdict    finding     = WorkloadVerdict::Clean;
    StressorComparison comparison;
};

[[nodiscard]] AttributionReport attribute_finding(const WorkloadResult& baseline,
                                                  const WorkloadResult& variant) noexcept;

} // namespace crd
