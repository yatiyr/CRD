// DIAG.10c workload oracles: the crd-core probes behind the warm/cold, soak, reload and shutdown workloads. Declared
// contract (docs/design/runtime-diagnostics.md, DIAG.10c):
//   - a variant changes one stressor at a time; a finding is attributed only against a clean baseline that differs in
//     exactly that stressor;
//   - a slowdown is classified only from measurements: enough samples in both windows, a baseline long enough to time
//     and narrow enough to judge, a median outside the tolerance AND quartiles that separate; otherwise Unmeasured,
//     Noisy, Steady or Inconclusive;
//   - progress stalls when no advance is seen for longer than its bound; a count that goes back is Regressed;
//   - a workload run is judged per iteration (cold reference, expected results, injected faults detected and recovered
//     from) and per cycle (the lifetime-ledger window from setup to teardown), worst finding first.
// Every duration below is synthetic, so no verdict depends on the machine, except the stall case, which sleeps far
// longer than its bound. The OS thread oracle is checked against a thread this test starts and joins.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/os_resource_probe.hpp>
#include <crd/core/workload_probe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace
{
using crd::FindingAttribution;
using crd::LifetimeClass;
using crd::LifetimeVerdict;
using crd::OsPresence;
using crd::ProgressReading;
using crd::ProgressVerdict;
using crd::SlowdownVerdict;
using crd::Stressor;
using crd::StressorChange;
using crd::WorkloadVerdict;

// n durations around `value` into out; returns the count.
crd::usize fill(crd::u64* out, crd::usize n, crd::u64 value)
{
    for (crd::usize i = 0U; i < n; ++i)
    {
        out[i] = value + (i % 4U) * 10U; // a little spread, well inside any tolerance
    }
    return n;
}

// ---- A scripted workload ------------------------------------------------------------------------------------------

constexpr crd::u64 kGood = 0xC0FFEEULL;

struct Script
{
    crd::LifetimeLedger ledger;
    crd::u32            fail_cycle      = ~0U; // a clean iteration that fails
    crd::u32            fail_iteration  = ~0U;
    crd::u32            wrong_cycle     = ~0U; // a clean iteration that computes something else
    crd::u32            wrong_iteration = ~0U;
    bool                miss_faults     = false; // injected iterations report success
    bool                faults_fire     = true;  // disarm reports a fired fault
    bool                break_recovery  = false; // the iteration after an injected one fails
    bool                leak            = false; // teardown leaves its thread behind
    crd::u32            setup_fails_at  = ~0U;
    crd::u32            sleep_iteration = ~0U; // cycle 0's iteration that sleeps
    crd::u32            sleep_ms        = 0U;
    crd::u32            armed           = 0U;
    bool                last_injected   = false;
};

bool script_setup(void* user, crd::u32 cycle) noexcept
{
    auto* s = static_cast<Script*>(user);
    if (cycle == s->setup_fails_at)
    {
        return false;
    }
    s->ledger.acquired(LifetimeClass::Thread);
    return true;
}

void script_iterate(void* user, crd::u32 cycle, crd::u32 iteration, bool injected,
                    crd::WorkloadIteration& out) noexcept
{
    auto* s = static_cast<Script*>(user);
    if (cycle == 0U && iteration == s->sleep_iteration)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(s->sleep_ms));
    }
    out.signature = kGood;
    out.ok        = true;
    if (injected)
    {
        // A failure keeps the last good result and says so.
        out.ok             = s->miss_faults;
        out.expected       = kGood;
        out.expected_known = true;
    }
    else if (s->last_injected && s->break_recovery)
    {
        out.ok = false;
    }
    if (cycle == s->fail_cycle && iteration == s->fail_iteration)
    {
        out.ok = false;
    }
    if (cycle == s->wrong_cycle && iteration == s->wrong_iteration)
    {
        out.signature = kGood + 1U;
    }
    s->last_injected = injected;
}

void script_teardown(void* user, crd::u32 /*cycle*/) noexcept
{
    auto* s = static_cast<Script*>(user);
    if (!s->leak)
    {
        (void)s->ledger.released(LifetimeClass::Thread);
    }
}

void script_arm(void* user, crd::u32 /*cycle*/, crd::u32 /*iteration*/) noexcept
{
    ++static_cast<Script*>(user)->armed;
}

crd::u32 script_disarm(void* user) noexcept
{
    return static_cast<Script*>(user)->faults_fire ? 1U : 0U;
}

crd::WorkloadSpec script_spec(Script& s)
{
    crd::WorkloadSpec spec;
    spec.name           = "script";
    spec.cycles         = 2U;
    spec.iterations     = 6U;
    spec.user           = &s;
    spec.setup          = &script_setup;
    spec.iterate        = &script_iterate;
    spec.teardown       = &script_teardown;
    spec.arm            = &script_arm;
    spec.disarm         = &script_disarm;
    spec.fault_seed     = 7U;
    spec.fault_permille = 1000U;
    spec.stall_ns       = 60ULL * 1000ULL * 1000ULL * 1000ULL; // a minute: no scripted iteration comes near it
    spec.ledger         = &s.ledger;
    spec.stressors.set(Stressor::Cycles, spec.cycles);
    spec.stressors.set(Stressor::Iterations, spec.iterations);
    spec.stressors.set(Stressor::FaultPlan, 1U);
    return spec;
}

crd::WorkloadResult run(const crd::WorkloadSpec& spec)
{
    crd::WorkloadResult r;
    crd::run_workload(spec, r);
    char line[512];
    (void)crd::format_workload_result(spec.name, r, line, sizeof(line));
    UNSCOPED_INFO(line); // shown with the caller's next assertion
    return r;
}
} // namespace

TEST_CASE("diag10c: a variant is attributable only when it changes exactly one stressor",
          "[core][workload][diag10c]")
{
    crd::StressorSet baseline;
    baseline.set(Stressor::Threads, 2U);
    baseline.set(Stressor::Iterations, 64U);

    crd::StressorSet same = baseline;
    crd::StressorComparison c = crd::compare_stressors(baseline, same);
    CHECK(c.change == StressorChange::Identical);
    CHECK(c.mask == 0U);

    crd::StressorSet threads = baseline;
    threads.set(Stressor::Threads, 4U);
    c = crd::compare_stressors(baseline, threads);
    CHECK(c.change == StressorChange::OneChanged);
    CHECK(c.changed == Stressor::Threads);
    CHECK(c.mask == (1U << static_cast<crd::u32>(Stressor::Threads)));

    crd::StressorSet two = threads;
    two.set(Stressor::Delay, 1000U);
    c = crd::compare_stressors(baseline, two);
    CHECK(c.change == StressorChange::SeveralChanged);
    const crd::u32 both =
        (1U << static_cast<crd::u32>(Stressor::Threads)) | (1U << static_cast<crd::u32>(Stressor::Delay));
    CHECK(c.mask == both);

    CHECK(std::strcmp(crd::stressor_name(Stressor::FaultPlan), "fault-plan") == 0);
    CHECK(std::strcmp(crd::stressor_change_name(StressorChange::SeveralChanged), "several-changed") == 0);
}

TEST_CASE("diag10c: a slowdown is classified only from enough, timeable, separable measurements",
          "[core][workload][diag10c]")
{
    crd::u64                  base[16];
    crd::u64                  cand[16];
    const crd::SlowdownPolicy policy; // 8 samples, 1000 ns floor, 250 permille tolerance, 1000 permille noise

    // Steady: the same population.
    (void)fill(base, 16U, 10000U);
    (void)fill(cand, 16U, 10500U);
    crd::SlowdownReport r = crd::slowdown_report(base, 16U, cand, 16U, policy);
    CHECK(r.verdict == SlowdownVerdict::Steady);
    CHECK(r.baseline.samples == 16U);
    CHECK(r.baseline.min == 10000U);
    CHECK(r.baseline.max == 10030U);
    CHECK(r.baseline.median == 10010U); // sorted index 7 of 16: values 10000 x4, 10010 x4, ...
    CHECK(r.ratio_permille == 1049U);   // 10510 * 1000 / 10010

    // Slower: three times the median, and the windows do not overlap.
    (void)fill(cand, 16U, 30000U);
    r = crd::slowdown_report(base, 16U, cand, 16U, policy);
    CHECK(r.verdict == SlowdownVerdict::Slower);
    CHECK(r.ratio_permille == 2998U); // 30010 * 1000 / 10010
    char line[256];
    (void)crd::format_slowdown_report(r, line, sizeof(line));
    CHECK(std::strcmp(line, "slower (16 and 16 samples): median 10010 ns -> 30010 ns (2998 permille), quartiles "
                            "10000-10020 ns -> 30000-30020 ns") == 0);

    // Faster: a third.
    (void)fill(cand, 16U, 3000U);
    CHECK(crd::slowdown_report(base, 16U, cand, 16U, policy).verdict == SlowdownVerdict::Faster);

    // Inconclusive: the median rose past the tolerance, but the candidate's lower quartile is inside the baseline's.
    for (crd::usize i = 0U; i < 16U; ++i)
    {
        cand[i] = i < 6U ? 9000U : 16000U;
    }
    r = crd::slowdown_report(base, 16U, cand, 16U, policy);
    CHECK(r.candidate.median == 16000U);
    CHECK(r.candidate.p25 == 9000U);
    CHECK(r.verdict == SlowdownVerdict::Inconclusive);
    // ... and the same overlap on the way down.
    for (crd::usize i = 0U; i < 16U; ++i)
    {
        cand[i] = i < 10U ? 5000U : 10020U;
    }
    CHECK(crd::slowdown_report(base, 16U, cand, 16U, policy).verdict == SlowdownVerdict::Inconclusive);

    // Noisy: the baseline's interquartile range is wider than its median.
    for (crd::usize i = 0U; i < 16U; ++i)
    {
        base[i] = 9000U;
        if (i < 10U)
        {
            base[i] = i < 6U ? 1000U : 2000U;
        }
    }
    (void)fill(cand, 16U, 30000U);
    r = crd::slowdown_report(base, 16U, cand, 16U, policy);
    CHECK(r.baseline.p75 - r.baseline.p25 > r.baseline.median);
    CHECK(r.verdict == SlowdownVerdict::Noisy);

    // Unmeasured: one sample, seven samples, a baseline below the clock floor, a window too long to judge.
    (void)fill(base, 16U, 10000U);
    r = crd::slowdown_report(base, 1U, cand, 1U, policy);
    CHECK(r.verdict == SlowdownVerdict::Unmeasured);
    CHECK(r.ratio_permille == 0U);
    (void)crd::format_slowdown_report(r, line, sizeof(line));
    CHECK(std::strcmp(line, "unmeasured (1 and 1 samples)") == 0);
    CHECK(crd::slowdown_report(base, 16U, cand, 7U, policy).verdict == SlowdownVerdict::Unmeasured);
    CHECK(crd::slowdown_report(base, 7U, cand, 16U, policy).verdict == SlowdownVerdict::Unmeasured);
    crd::u64 fast[16];
    (void)fill(fast, 16U, 500U);
    CHECK(crd::slowdown_report(fast, 16U, cand, 16U, policy).verdict == SlowdownVerdict::Unmeasured);
    CHECK(crd::slowdown_report(base, crd::kSlowdownMaxSamples + 1U, cand, 16U, policy).verdict ==
          SlowdownVerdict::Unmeasured);
    CHECK(crd::slowdown_report(nullptr, 16U, cand, 16U, policy).verdict == SlowdownVerdict::Unmeasured);

    // Saturation: an absurd candidate median never wraps the ratio.
    (void)fill(cand, 16U, ~crd::u64{0} - 100U);
    r = crd::slowdown_report(base, 16U, cand, 16U, policy);
    CHECK(r.verdict == SlowdownVerdict::Slower);
    CHECK(r.ratio_permille == (~crd::u64{0}) / 10010U);

    CHECK(std::strcmp(crd::slowdown_verdict_name(SlowdownVerdict::Inconclusive), "inconclusive") == 0);
}

TEST_CASE("diag10c: progress stalls when no advance is seen for longer than its bound",
          "[core][workload][diag10c]")
{
    crd::ProgressPolicy policy;
    policy.stall_ns = 100U;
    policy.target   = 4U;

    const ProgressReading steady[] = {{0U, 0U}, {50U, 1U}, {120U, 2U}, {200U, 3U}, {290U, 4U}, {900U, 4U}};
    crd::ProgressReport   r        = crd::progress_report(steady, 6U, policy);
    CHECK(r.verdict == ProgressVerdict::Progressing);
    CHECK(r.longest_gap == 90U); // the reading after the target is not judged
    CHECK(r.done == 4U);
    CHECK(r.readings == 6U);

    // A reading without an advance still counts toward the gap: the run stopped after its third unit.
    const ProgressReading stopped[] = {{0U, 0U}, {50U, 1U}, {100U, 2U}, {150U, 3U}, {200U, 3U}, {260U, 3U}};
    r = crd::progress_report(stopped, 6U, policy);
    CHECK(r.verdict == ProgressVerdict::Stalled);
    CHECK(r.stalled_from == 150U);
    CHECK(r.longest_gap == 110U);

    // A gap between two advances.
    const ProgressReading gap[] = {{0U, 0U}, {50U, 1U}, {400U, 2U}, {450U, 4U}};
    r = crd::progress_report(gap, 4U, policy);
    CHECK(r.verdict == ProgressVerdict::Stalled);
    CHECK(r.stalled_from == 50U);
    CHECK(r.longest_gap == 350U);

    // Never stalled, never finished.
    const ProgressReading short_run[] = {{0U, 0U}, {50U, 1U}, {100U, 2U}};
    CHECK(crd::progress_report(short_run, 3U, policy).verdict == ProgressVerdict::Unfinished);

    // A count that goes back is a broken counter, worse than a stall.
    const ProgressReading back[] = {{0U, 0U}, {50U, 2U}, {500U, 1U}, {550U, 4U}};
    CHECK(crd::progress_report(back, 4U, policy).verdict == ProgressVerdict::Regressed);

    // Unmeasured: one reading, no bound, time going back.
    CHECK(crd::progress_report(steady, 1U, policy).verdict == ProgressVerdict::Unmeasured);
    crd::ProgressPolicy unbounded = policy;
    unbounded.stall_ns            = 0U;
    CHECK(crd::progress_report(stopped, 6U, unbounded).verdict == ProgressVerdict::Unmeasured);
    const ProgressReading disordered[] = {{0U, 0U}, {50U, 1U}, {40U, 2U}, {90U, 4U}};
    CHECK(crd::progress_report(disordered, 4U, policy).verdict == ProgressVerdict::Unmeasured);
    CHECK(crd::progress_report(nullptr, 4U, policy).verdict == ProgressVerdict::Unmeasured);

    CHECK(std::strcmp(crd::progress_verdict_name(ProgressVerdict::Regressed), "regressed") == 0);
}

TEST_CASE("diag10c: injection is seeded, skips the cold and last iterations, and leaves a clean one after each",
          "[core][workload][diag10c]")
{
    crd::u32 injected = 0U;
    for (crd::u32 i = 0U; i < 64U; ++i)
    {
        const bool a = crd::workload_injects(99U, 500U, 3U, i, 64U);
        CHECK(a == crd::workload_injects(99U, 500U, 3U, i, 64U)); // a pure function
        if (a)
        {
            ++injected;
            CHECK(i % 2U == 1U);
            CHECK(i + 1U < 64U);
            CHECK_FALSE(crd::workload_injects(99U, 500U, 3U, i + 1U, 64U));
        }
    }
    CHECK(injected > 0U);
    CHECK(injected < 32U);
    CHECK_FALSE(crd::workload_injects(99U, 1000U, 0U, 0U, 64U));
    CHECK_FALSE(crd::workload_injects(99U, 1000U, 0U, 63U, 64U));
    CHECK_FALSE(crd::workload_injects(99U, 1000U, 0U, 5U, 6U)); // the last iteration
    CHECK(crd::workload_injects(99U, 1000U, 0U, 3U, 6U));
    CHECK_FALSE(crd::workload_injects(99U, 0U, 0U, 3U, 6U));
}

TEST_CASE("diag10c: a clean workload detects and recovers from every injected fault and balances every cycle",
          "[core][workload][diag10c]")
{
    Script                    s;
    const crd::WorkloadSpec   spec = script_spec(s);
    const crd::WorkloadResult r    = run(spec);
    CHECK(r.verdict == WorkloadVerdict::Clean);
    CHECK(r.cycles_run == 2U);
    CHECK(r.iterations_run == 12U);
    CHECK(r.injected_iterations == 4U); // iterations 1 and 3 of each cycle
    CHECK(s.armed == 4U);
    CHECK(r.faults_fired == 4U);
    CHECK(r.detected == 4U);
    CHECK(r.recovered == 4U);
    CHECK(r.failed_iterations == 0U);
    CHECK(r.reference == kGood);
    CHECK(r.cold_count == 2U);
    CHECK(r.warm_count == 2U); // iteration 5 of each cycle: 1 and 3 injected, 2 and 4 recovering
    CHECK(r.worst_cycle.verdict == LifetimeVerdict::Balanced);
    CHECK(r.progress.verdict == ProgressVerdict::Progressing);
    CHECK(r.progress.done == 8U); // only successful iterations progress
    CHECK(r.slowdown.verdict == SlowdownVerdict::Unmeasured); // one warm sample per window

    char line[512];
    (void)crd::format_workload_result("script", r, line, sizeof(line));
    CHECK(std::strstr(line, "script: clean (worst cycle 0: balanced); 2 cycles, 12 iterations, 4 injected, "
                            "4 detected, 4 recovered; progress progressing; timing unmeasured") == line);
}

TEST_CASE("diag10c: each workload finding is named, located and ranked", "[core][workload][diag10c]")
{
    SECTION("a clean iteration that computes something else diverged")
    {
        Script s;
        s.wrong_cycle     = 1U;
        s.wrong_iteration = 5U;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::Diverged);
        CHECK(r.finding_cycle == 1U);
        CHECK(r.finding_iteration == 5U);
        CHECK(r.diverged == 1U);
    }
    SECTION("an injected iteration that loses the last good result diverged")
    {
        Script s;
        s.wrong_cycle     = 0U;
        s.wrong_iteration = 3U;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::Diverged);
        CHECK(r.finding_iteration == 3U);
    }
    SECTION("a clean iteration that fails")
    {
        Script s;
        s.fail_cycle     = 1U;
        s.fail_iteration = 5U;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::IterationFailed);
        CHECK(r.finding_cycle == 1U);
        CHECK(r.failed_iterations == 1U);
    }
    SECTION("the iteration after an injected one fails")
    {
        Script s;
        s.break_recovery            = true;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::NotRecovered);
        CHECK(r.finding_cycle == 0U);
        CHECK(r.finding_iteration == 2U);
        CHECK(r.recovered == 0U);
    }
    SECTION("an injected fault that fired and was not surfaced")
    {
        Script s;
        s.miss_faults               = true;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::FaultMissed);
        CHECK(r.finding_iteration == 1U);
        CHECK(r.detected == 0U);
    }
    SECTION("an injection that never fired proves nothing")
    {
        Script s;
        s.faults_fire               = false;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::FaultNotFired);
        CHECK(r.faults_fired == 0U);
    }
    SECTION("a teardown that leaves its thread behind leaked")
    {
        Script s;
        s.leak                      = true;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::Leaked);
        CHECK(r.finding_cycle == 0U);
        CHECK(r.finding_iteration == 6U); // after the cycle's last iteration
        CHECK(r.worst_cycle.verdict == LifetimeVerdict::Outstanding);
        CHECK(r.worst_cycle.live_change(LifetimeClass::Thread) == 1);
    }
    SECTION("a teardown that releases what it never acquired underflowed the ledger")
    {
        Script            s;
        crd::WorkloadSpec spec = script_spec(s);
        spec.setup             = nullptr; // nothing acquired; the teardown still releases
        const crd::WorkloadResult r = run(spec);
        CHECK(r.verdict == WorkloadVerdict::Leaked);
        CHECK(r.worst_cycle.verdict == LifetimeVerdict::Underflowed);
    }
    SECTION("a setup that fails stops the run")
    {
        Script s;
        s.setup_fails_at            = 1U;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::SetupFailed);
        CHECK(r.finding_cycle == 1U);
        CHECK(r.cycles_run == 1U);
        CHECK(r.iterations_run == 6U);
    }
    SECTION("an incomplete spec runs nothing")
    {
        Script            s;
        crd::WorkloadSpec spec = script_spec(s);
        spec.disarm            = nullptr;
        CHECK(run(spec).verdict == WorkloadVerdict::SetupFailed);
        spec        = script_spec(s);
        spec.cycles = 0U;
        CHECK(run(spec).iterations_run == 0U);
        spec         = script_spec(s);
        spec.iterate = nullptr;
        CHECK(run(spec).verdict == WorkloadVerdict::SetupFailed);
    }
    SECTION("a worse finding outranks an earlier, milder one")
    {
        Script s;
        s.fail_cycle      = 0U;
        s.fail_iteration  = 5U; // IterationFailed first
        s.wrong_cycle     = 1U;
        s.wrong_iteration = 5U; // then Diverged
        s.leak            = true;
        const crd::WorkloadResult r = run(script_spec(s));
        CHECK(r.verdict == WorkloadVerdict::Diverged);
        CHECK(r.finding_cycle == 1U);
    }
    CHECK(std::strcmp(crd::workload_verdict_name(WorkloadVerdict::FaultNotFired), "fault-not-fired") == 0);
}

TEST_CASE("diag10c: an iteration that stops progress for longer than the bound stalls the workload",
          "[core][workload][diag10c]")
{
    Script s;
    s.sleep_iteration      = 4U;
    s.sleep_ms             = 400U;
    crd::WorkloadSpec spec = script_spec(s);
    spec.stall_ns          = 100ULL * 1000ULL * 1000ULL; // 100 ms, a quarter of the sleep
    spec.stressors.set(Stressor::Delay, 400ULL * 1000ULL * 1000ULL);
    const crd::WorkloadResult r = run(spec);
    CHECK(r.verdict == WorkloadVerdict::Stalled);
    CHECK(r.progress.verdict == ProgressVerdict::Stalled);
    CHECK(r.progress.longest_gap >= 400ULL * 1000ULL * 1000ULL);
    CHECK(r.injected_iterations == 4U); // the rest of the run still ran and was judged
    CHECK(r.detected == 4U);
}

TEST_CASE("diag10c: a finding is attributed to the one stressor a variant changed, against a clean baseline",
          "[core][workload][diag10c]")
{
    Script                    base_script;
    const crd::WorkloadSpec   base_spec = script_spec(base_script);
    const crd::WorkloadResult baseline  = run(base_spec);
    REQUIRE(baseline.verdict == WorkloadVerdict::Clean);

    // The variant that leaks also changes one stressor: it is blamed on that stressor.
    Script variant_script;
    variant_script.leak         = true;
    crd::WorkloadSpec leak_spec = script_spec(variant_script);
    leak_spec.stressors.set(Stressor::Cycles, 3U);
    leak_spec.cycles                  = 3U;
    const crd::WorkloadResult leaking = run(leak_spec);
    crd::AttributionReport    a       = crd::attribute_finding(baseline, leaking);
    CHECK(a.attribution == FindingAttribution::Attributed);
    CHECK(a.stressor == Stressor::Cycles);
    CHECK(a.finding == WorkloadVerdict::Leaked);

    // A clean variant has nothing to attribute.
    Script            clean_script;
    crd::WorkloadSpec clean_spec = script_spec(clean_script);
    clean_spec.stressors.set(Stressor::Iterations, 8U);
    clean_spec.iterations = 8U;
    a                     = crd::attribute_finding(baseline, run(clean_spec));
    CHECK(a.attribution == FindingAttribution::NoNewFinding);

    // Two stressors at once: nothing is attributed, even though the variant failed.
    leak_spec.stressors.set(Stressor::Threads, 8U);
    a = crd::attribute_finding(baseline, run(leak_spec));
    CHECK(a.attribution == FindingAttribution::NotOneStressor);
    CHECK(a.comparison.change == StressorChange::SeveralChanged);
    // ... nor with none.
    CHECK(crd::attribute_finding(baseline, leaking).comparison.change == StressorChange::OneChanged);
    Script same_script;
    same_script.leak = true;
    a                = crd::attribute_finding(baseline, run(script_spec(same_script)));
    CHECK(a.attribution == FindingAttribution::NotOneStressor);

    // A baseline that is not clean attributes nothing.
    const crd::WorkloadResult& failing_baseline = leaking;
    const crd::WorkloadResult& clean_variant    = baseline;
    a                                           = crd::attribute_finding(failing_baseline, clean_variant);
    CHECK(a.attribution == FindingAttribution::BaselineFailing);

    CHECK(std::strcmp(crd::finding_attribution_name(FindingAttribution::NotOneStressor), "not-one-stressor") == 0);
}

TEST_CASE("diag10c: the OS sees a thread this process started until it has been joined",
          "[core][lifetime][workload][diag10c]")
{
    const crd::OsThreadIdentity self = crd::os_current_thread_identity();
    REQUIRE(self.known);
    CHECK(crd::os_thread_alive(self).presence == OsPresence::Present);

    // The same id with another start time is a different thread: the probe must not confuse them.
    crd::OsThreadIdentity forged = self;
    forged.start += 1U;
    CHECK(crd::os_thread_alive(forged).presence == OsPresence::Absent);

    std::atomic<bool>     release{false};
    std::atomic<bool>     ready{false};
    crd::OsThreadIdentity worker;
    std::thread           t(
        [&]
        {
            worker = crd::os_current_thread_identity();
            ready.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    while (!ready.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(worker.known);
    CHECK(worker.id != self.id);
    crd::OsProbeResult r = crd::os_thread_alive(worker);
    INFO("reason: " << r.reason);
    CHECK(r.presence == OsPresence::Present);
    release.store(true, std::memory_order_release);

    // Ended is ended: once the thread has returned, the OS reports it gone even while this thread still holds it
    // unjoined (Windows keeps its object alive through the std::thread's handle).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (crd::os_thread_alive(worker).presence == OsPresence::Present && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(crd::os_thread_alive(worker).presence == OsPresence::Absent);
    t.join();
    CHECK(crd::os_thread_alive(worker).presence == OsPresence::Absent);

    // An identity that was never read is never judged.
    r = crd::os_thread_alive(crd::OsThreadIdentity{});
    CHECK(r.presence == OsPresence::Unavailable);
    CHECK(std::strlen(r.reason) > 0U);
    CHECK(std::strcmp(crd::lifetime_class_name(LifetimeClass::Thread), "thread") == 0);
}

TEST_CASE("diag10c: a thread asked about while it exits reads as present or absent, never unavailable",
          "[core][lifetime][workload][diag10c]")
{
    // The kernel can reap a task between the probe's open() and read() of its stat file; that is a thread gone, not
    // an unreadable one. Many short threads probed without pause make the window likely on every run.
    crd::usize unavailable = 0U;
    for (crd::usize i = 0U; i < 300U; ++i)
    {
        std::atomic<bool>     ready{false};
        crd::OsThreadIdentity worker;
        std::thread           t(
            [&]
            {
                worker = crd::os_current_thread_identity();
                ready.store(true, std::memory_order_release);
            });
        while (!ready.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        OsPresence presence = OsPresence::Present;
        while (presence == OsPresence::Present)
        {
            presence = crd::os_thread_alive(worker).presence;
        }
        t.join();
        if (presence != OsPresence::Absent)
        {
            ++unavailable;
        }
    }
    CHECK(unavailable == 0U);
}
