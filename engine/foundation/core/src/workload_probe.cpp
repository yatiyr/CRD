#include <crd/core/workload_probe.hpp>

#include <chrono>
#include <cstdio>

namespace crd
{

namespace
{
constexpr crd::u64 kU64Max = ~crd::u64{0};

crd::u64 mul_sat(crd::u64 a, crd::u64 b) noexcept
{
    if (a != 0U && b > kU64Max / a)
    {
        return kU64Max;
    }
    return a * b;
}

crd::u64 now_ns() noexcept
{
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<crd::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

crd::u64 splitmix64(crd::u64 x) noexcept
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

crd::usize append_text(char* out, crd::usize capacity, crd::usize length, const char* text) noexcept
{
    if (capacity == 0U)
    {
        return 0U;
    }
    while (*text != '\0' && length + 1U < capacity)
    {
        out[length] = *text;
        ++length;
        ++text;
    }
    out[length] = '\0';
    return length;
}

crd::usize append_number(char* out, crd::usize capacity, crd::usize length, crd::u64 value) noexcept
{
    char digits[24];
    (void)std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(value));
    return append_text(out, capacity, length, digits);
}

// Text, a number, text: the shape of every field of the one-line reports.
crd::usize append_field(char* out, crd::usize capacity, crd::usize length, const char* before, crd::u64 value,
                        const char* after) noexcept
{
    length = append_text(out, capacity, length, before);
    length = append_number(out, capacity, length, value);
    return append_text(out, capacity, length, after);
}

// Ranks of the worst-last enums, compared as integers so no comparison reads as a producer of a value.
template <typename E>
constexpr crd::u32 rank(E e) noexcept
{
    return static_cast<crd::u32>(e);
}

crd::u32 lifetime_rank(LifetimeVerdict v) noexcept
{
    switch (v) // Underflowed is worse than Outstanding, which is worse than Drained
    {
    case LifetimeVerdict::Balanced:    return 0U;
    case LifetimeVerdict::Drained:     return 1U;
    case LifetimeVerdict::Outstanding: return 2U;
    case LifetimeVerdict::Underflowed: return 3U;
    }
    return 0U;
}

// The streaming form of progress_report: one reading at a time.
class ProgressTracker
{
public:
    explicit ProgressTracker(const ProgressPolicy& policy) noexcept : m_policy(policy) {}

    // Feed one reading; true when this reading is the first to show a stall.
    bool feed(const ProgressReading& r) noexcept
    {
        bool first_stall = false;
        if (m_report.readings != 0U)
        {
            if (r.at_ns < m_last.at_ns)
            {
                m_disordered = true;
            }
            else if (r.done < m_last.done)
            {
                m_regressed = true;
            }
            else if (!m_reached)
            {
                const crd::u64 gap = r.at_ns - m_last_advance;
                if (gap > m_report.longest_gap)
                {
                    m_report.longest_gap = gap;
                }
                if (m_policy.stall_ns != 0U && gap > m_policy.stall_ns && !m_stalled)
                {
                    m_stalled             = true;
                    first_stall           = true;
                    m_report.stalled_from = m_last_advance;
                }
                if (r.done > m_last.done)
                {
                    m_last_advance = r.at_ns;
                }
            }
        }
        else
        {
            m_last_advance = r.at_ns;
        }
        if (m_policy.target != 0U && r.done >= m_policy.target)
        {
            m_reached = true;
        }
        m_last = r;
        ++m_report.readings;
        m_report.done = r.done;
        return first_stall;
    }

    [[nodiscard]] ProgressReport finish() const noexcept
    {
        ProgressReport out = m_report;
        const crd::u32 min = m_policy.min_readings < 2U ? 2U : m_policy.min_readings;
        if (out.readings < min || m_policy.stall_ns == 0U || m_disordered)
        {
            out.verdict = ProgressVerdict::Unmeasured;
        }
        else if (m_regressed)
        {
            out.verdict = ProgressVerdict::Regressed;
        }
        else if (m_stalled)
        {
            out.verdict = ProgressVerdict::Stalled;
        }
        else if (m_policy.target != 0U && out.done < m_policy.target)
        {
            out.verdict = ProgressVerdict::Unfinished;
        }
        else
        {
            out.verdict = ProgressVerdict::Progressing;
        }
        return out;
    }

private:
    ProgressPolicy  m_policy;
    ProgressReport  m_report;
    ProgressReading m_last;
    crd::u64        m_last_advance = 0U;
    bool            m_disordered   = false;
    bool            m_regressed    = false;
    bool            m_stalled      = false;
    bool            m_reached      = false;
};
} // namespace

// ---- Names --------------------------------------------------------------------------------------------------------

const char* stressor_name(Stressor s) noexcept
{
    switch (s) // no default: every stressor is named
    {
    case Stressor::Iterations:    return "iterations";
    case Stressor::Cycles:        return "cycles";
    case Stressor::Threads:       return "threads";
    case Stressor::FaultPlan:     return "fault-plan";
    case Stressor::MemoryBudget:  return "memory-budget";
    case Stressor::ReloadCadence: return "reload-cadence";
    case Stressor::PayloadBytes:  return "payload-bytes";
    case Stressor::Delay:         return "delay";
    }
    return "unknown";
}

const char* stressor_change_name(StressorChange c) noexcept
{
    switch (c) // no default: every change is named
    {
    case StressorChange::Identical:      return "identical";
    case StressorChange::OneChanged:     return "one-changed";
    case StressorChange::SeveralChanged: return "several-changed";
    }
    return "unknown";
}

const char* slowdown_verdict_name(SlowdownVerdict v) noexcept
{
    switch (v) // no default: every verdict is named
    {
    case SlowdownVerdict::Unmeasured:   return "unmeasured";
    case SlowdownVerdict::Noisy:        return "noisy";
    case SlowdownVerdict::Steady:       return "steady";
    case SlowdownVerdict::Faster:       return "faster";
    case SlowdownVerdict::Inconclusive: return "inconclusive";
    case SlowdownVerdict::Slower:       return "slower";
    }
    return "unknown";
}

const char* progress_verdict_name(ProgressVerdict v) noexcept
{
    switch (v) // no default: every verdict is named
    {
    case ProgressVerdict::Unmeasured:  return "unmeasured";
    case ProgressVerdict::Progressing: return "progressing";
    case ProgressVerdict::Unfinished:  return "unfinished";
    case ProgressVerdict::Stalled:     return "stalled";
    case ProgressVerdict::Regressed:   return "regressed";
    }
    return "unknown";
}

const char* workload_verdict_name(WorkloadVerdict v) noexcept
{
    switch (v) // no default: every verdict is named
    {
    case WorkloadVerdict::Clean:           return "clean";
    case WorkloadVerdict::FaultNotFired:   return "fault-not-fired";
    case WorkloadVerdict::Stalled:         return "stalled";
    case WorkloadVerdict::Leaked:          return "leaked";
    case WorkloadVerdict::IterationFailed: return "iteration-failed";
    case WorkloadVerdict::NotRecovered:    return "not-recovered";
    case WorkloadVerdict::FaultMissed:     return "fault-missed";
    case WorkloadVerdict::Diverged:        return "diverged";
    case WorkloadVerdict::SetupFailed:     return "setup-failed";
    }
    return "unknown";
}

const char* finding_attribution_name(FindingAttribution a) noexcept
{
    switch (a) // no default: every attribution is named
    {
    case FindingAttribution::NoNewFinding:    return "no-new-finding";
    case FindingAttribution::Attributed:      return "attributed";
    case FindingAttribution::BaselineFailing: return "baseline-failing";
    case FindingAttribution::NotOneStressor:  return "not-one-stressor";
    }
    return "unknown";
}

// ---- Stressors ----------------------------------------------------------------------------------------------------

StressorComparison compare_stressors(const StressorSet& baseline, const StressorSet& variant) noexcept
{
    StressorComparison out;
    crd::u32           changed = 0U;
    for (crd::usize i = 0U; i < kStressorCount; ++i)
    {
        if (baseline.values[i] != variant.values[i])
        {
            out.mask |= 1U << static_cast<crd::u32>(i);
            out.changed = static_cast<Stressor>(i);
            ++changed;
        }
    }
    if (changed == 0U)
    {
        out.change  = StressorChange::Identical;
        out.changed = Stressor::Iterations;
    }
    else if (changed == 1U)
    {
        out.change = StressorChange::OneChanged;
    }
    else
    {
        out.change  = StressorChange::SeveralChanged;
        out.changed = Stressor::Iterations;
    }
    return out;
}

// ---- Slowdown -----------------------------------------------------------------------------------------------------

bool summarize_durations(const crd::u64* ns, crd::usize count, DurationSummary& out) noexcept
{
    out         = DurationSummary{};
    out.samples = count > 0xFFFFFFFFU ? 0xFFFFFFFFU : static_cast<crd::u32>(count);
    if (ns == nullptr || count == 0U || count > kSlowdownMaxSamples)
    {
        return false;
    }
    crd::u64 sorted[kSlowdownMaxSamples];
    for (crd::usize i = 0U; i < count; ++i)
    {
        // Insertion sort: at most kSlowdownMaxSamples, judged once per window.
        const crd::u64 v = ns[i];
        crd::usize     j = i;
        while (j > 0U && sorted[j - 1U] > v)
        {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = v;
    }
    const crd::usize last = count - 1U;
    out.min               = sorted[0];
    out.p25               = sorted[(last * 1U) / 4U];
    out.median            = sorted[(last * 2U) / 4U];
    out.p75               = sorted[(last * 3U) / 4U];
    out.max               = sorted[last];
    return true;
}

SlowdownReport slowdown_report(const crd::u64* baseline, crd::usize baseline_count, const crd::u64* candidate,
                               crd::usize candidate_count, const SlowdownPolicy& policy) noexcept
{
    SlowdownReport out;
    const bool     b_ok = summarize_durations(baseline, baseline_count, out.baseline);
    const bool     c_ok = summarize_durations(candidate, candidate_count, out.candidate);
    const crd::u64 floor = policy.min_baseline_ns == 0U ? 1U : policy.min_baseline_ns;
    if (!b_ok || !c_ok || out.baseline.samples < policy.min_samples || out.candidate.samples < policy.min_samples ||
        out.baseline.median < floor)
    {
        out.verdict = SlowdownVerdict::Unmeasured;
        return out;
    }
    const DurationSummary& b = out.baseline;
    const DurationSummary& c = out.candidate;
    out.ratio_permille       = mul_sat(c.median, 1000U) / b.median;
    if (mul_sat(b.p75 - b.p25, 1000U) > mul_sat(b.median, policy.noise_permille))
    {
        out.verdict = SlowdownVerdict::Noisy;
        return out;
    }
    const crd::u64 widened = 1000U + static_cast<crd::u64>(policy.tolerance_permille);
    const bool     up      = mul_sat(c.median, 1000U) > mul_sat(b.median, widened);
    const bool     down    = mul_sat(c.median, widened) < mul_sat(b.median, 1000U);
    if (up)
    {
        out.verdict = c.p25 > b.p75 ? SlowdownVerdict::Slower : SlowdownVerdict::Inconclusive;
    }
    else if (down)
    {
        out.verdict = c.p75 < b.p25 ? SlowdownVerdict::Faster : SlowdownVerdict::Inconclusive;
    }
    else
    {
        out.verdict = SlowdownVerdict::Steady;
    }
    return out;
}

crd::usize format_slowdown_report(const SlowdownReport& r, char* out, crd::usize capacity) noexcept
{
    if (capacity == 0U)
    {
        return 0U;
    }
    out[0]            = '\0';
    crd::usize length = append_text(out, capacity, 0U, slowdown_verdict_name(r.verdict));
    length            = append_field(out, capacity, length, " (", r.baseline.samples, " and ");
    length            = append_field(out, capacity, length, "", r.candidate.samples, " samples)");
    if (r.verdict == SlowdownVerdict::Unmeasured)
    {
        return length;
    }
    length = append_field(out, capacity, length, ": median ", r.baseline.median, " ns -> ");
    length = append_field(out, capacity, length, "", r.candidate.median, " ns (");
    length = append_field(out, capacity, length, "", r.ratio_permille, " permille), quartiles ");
    length = append_field(out, capacity, length, "", r.baseline.p25, "-");
    length = append_field(out, capacity, length, "", r.baseline.p75, " ns -> ");
    length = append_field(out, capacity, length, "", r.candidate.p25, "-");
    return append_field(out, capacity, length, "", r.candidate.p75, " ns");
}

// ---- Progress -----------------------------------------------------------------------------------------------------

ProgressReport progress_report(const ProgressReading* readings, crd::usize count,
                               const ProgressPolicy& policy) noexcept
{
    ProgressTracker tracker(policy);
    for (crd::usize i = 0U; readings != nullptr && i < count; ++i)
    {
        (void)tracker.feed(readings[i]);
    }
    return tracker.finish();
}

// ---- Workloads ----------------------------------------------------------------------------------------------------

bool workload_injects(crd::u64 seed, crd::u32 permille, crd::u32 cycle, crd::u32 iteration,
                      crd::u32 iterations) noexcept
{
    if (permille == 0U || iteration == 0U || iteration + 1U >= iterations || (iteration & 1U) == 0U)
    {
        return false;
    }
    if (permille >= 1000U)
    {
        return true;
    }
    const crd::u64 h = splitmix64(seed ^ splitmix64((static_cast<crd::u64>(cycle) << 32U) | iteration));
    return h % 1000U < permille;
}

namespace
{
// Keeps the worst verdict and where it was first seen.
void note(WorkloadResult& out, WorkloadVerdict v, crd::u32 cycle, crd::u32 iteration) noexcept
{
    if (rank(v) > rank(out.verdict))
    {
        out.verdict           = v;
        out.finding_cycle     = cycle;
        out.finding_iteration = iteration;
    }
}
} // namespace

void run_workload(const WorkloadSpec& spec, WorkloadResult& out) noexcept
{
    out           = WorkloadResult{};
    out.stressors = spec.stressors;
    if (spec.iterate == nullptr || spec.cycles == 0U || spec.iterations == 0U ||
        (spec.arm == nullptr) != (spec.disarm == nullptr))
    {
        note(out, WorkloadVerdict::SetupFailed, 0U, 0U);
        return;
    }
    LifetimeLedger& ledger = spec.ledger != nullptr ? *spec.ledger : lifetime_ledger();
    ProgressPolicy  progress_policy;
    progress_policy.stall_ns = spec.stall_ns;
    ProgressTracker progress(progress_policy);
    const crd::u64  origin = now_ns();
    crd::u64        done   = 0U;
    (void)progress.feed(ProgressReading{0U, 0U});
    bool have_reference = false;

    for (crd::u32 c = 0U; c < spec.cycles; ++c)
    {
        const LifetimeSnapshot before = ledger.snapshot();
        if (spec.setup != nullptr && !spec.setup(spec.user, c))
        {
            note(out, WorkloadVerdict::SetupFailed, c, 0U);
            break;
        }
        ++out.cycles_run;
        bool previous_injected = false;
        for (crd::u32 i = 0U; i < spec.iterations; ++i)
        {
            const bool injected = spec.arm != nullptr &&
                                  workload_injects(spec.fault_seed, spec.fault_permille, c, i, spec.iterations);
            if (injected)
            {
                spec.arm(spec.user, c, i);
            }
            WorkloadIteration it;
            const crd::u64    start = now_ns();
            spec.iterate(spec.user, c, i, injected, it);
            const crd::u64 elapsed = now_ns() - start;
            crd::u32       fired   = 0U;
            if (injected)
            {
                fired = spec.disarm(spec.user);
                ++out.injected_iterations;
                out.faults_fired += fired;
            }
            ++out.iterations_run;

            if (!have_reference && c == 0U && i == 0U && !it.expected_known && it.ok)
            {
                out.reference  = it.signature;
                have_reference = true;
            }
            const crd::u64 expected = it.expected_known ? it.expected : out.reference;
            const bool     judged   = it.expected_known || (it.ok && have_reference);
            const bool     matches  = !judged || it.signature == expected;

            if (injected)
            {
                if (!matches)
                {
                    ++out.diverged;
                    note(out, WorkloadVerdict::Diverged, c, i);
                }
                if (fired == 0U)
                {
                    note(out, WorkloadVerdict::FaultNotFired, c, i);
                }
                else if (it.ok)
                {
                    note(out, WorkloadVerdict::FaultMissed, c, i);
                }
                else
                {
                    ++out.detected;
                }
            }
            else if (previous_injected)
            {
                if (it.ok && matches)
                {
                    ++out.recovered;
                }
                else
                {
                    note(out, WorkloadVerdict::NotRecovered, c, i);
                }
            }
            else
            {
                if (!it.ok)
                {
                    ++out.failed_iterations;
                    note(out, WorkloadVerdict::IterationFailed, c, i);
                }
                if (!matches)
                {
                    ++out.diverged;
                    note(out, WorkloadVerdict::Diverged, c, i);
                }
            }

            if (i == 0U)
            {
                if (out.cold_count < kWorkloadMaxSamples)
                {
                    out.cold_ns[out.cold_count] = elapsed;
                    ++out.cold_count;
                }
            }
            else if (!injected && !previous_injected && it.ok && matches && out.warm_count < kWorkloadMaxSamples)
            {
                out.warm_ns[out.warm_count] = elapsed;
                ++out.warm_count;
            }

            if (it.ok)
            {
                done += it.progress;
            }
            if (progress.feed(ProgressReading{now_ns() - origin, done}))
            {
                note(out, WorkloadVerdict::Stalled, c, i);
            }
            previous_injected = injected;
        }
        if (spec.teardown != nullptr)
        {
            spec.teardown(spec.user, c);
        }
        const LifetimeReport window = lifetime_report(before, ledger.snapshot());
        if (out.cycles_run == 1U || lifetime_rank(window.verdict) > lifetime_rank(out.worst_cycle.verdict))
        {
            out.worst_cycle       = window;
            out.worst_cycle_index = c;
        }
        if (window.outstanding != 0U || window.underflowed != 0U)
        {
            note(out, WorkloadVerdict::Leaked, c, spec.iterations);
        }
    }

    out.progress = progress.finish();
    const crd::usize half = out.warm_count / 2U;
    out.slowdown = slowdown_report(out.warm_ns, half, out.warm_ns + (out.warm_count - half), half, SlowdownPolicy{});
}

crd::usize format_workload_result(const char* name, const WorkloadResult& r, char* out, crd::usize capacity) noexcept
{
    if (capacity == 0U)
    {
        return 0U;
    }
    out[0]            = '\0';
    crd::usize length = append_text(out, capacity, 0U, name != nullptr ? name : "");
    length            = append_text(out, capacity, length, ": ");
    length            = append_text(out, capacity, length, workload_verdict_name(r.verdict));
    if (rank(r.verdict) != 0U)
    {
        length = append_field(out, capacity, length, " at cycle ", r.finding_cycle, " iteration ");
        length = append_number(out, capacity, length, r.finding_iteration);
    }
    char window[160];
    (void)format_lifetime_report(r.worst_cycle, window, sizeof(window));
    length = append_field(out, capacity, length, " (worst cycle ", r.worst_cycle_index, ": ");
    length = append_text(out, capacity, length, window);
    length = append_field(out, capacity, length, "); ", r.cycles_run, " cycles, ");
    length = append_field(out, capacity, length, "", r.iterations_run, " iterations, ");
    length = append_field(out, capacity, length, "", r.injected_iterations, " injected, ");
    length = append_field(out, capacity, length, "", r.detected, " detected, ");
    length = append_field(out, capacity, length, "", r.recovered, " recovered");
    length = append_text(out, capacity, length, "; progress ");
    length = append_text(out, capacity, length, progress_verdict_name(r.progress.verdict));
    length = append_text(out, capacity, length, "; timing ");
    char timing[160];
    (void)format_slowdown_report(r.slowdown, timing, sizeof(timing));
    return append_text(out, capacity, length, timing);
}

// ---- Attribution --------------------------------------------------------------------------------------------------

AttributionReport attribute_finding(const WorkloadResult& baseline, const WorkloadResult& variant) noexcept
{
    AttributionReport out;
    out.finding    = variant.verdict;
    out.comparison = compare_stressors(baseline.stressors, variant.stressors);
    if (rank(baseline.verdict) != 0U)
    {
        out.attribution = FindingAttribution::BaselineFailing;
    }
    else if (out.comparison.change != StressorChange::OneChanged)
    {
        out.attribution = FindingAttribution::NotOneStressor;
    }
    else if (rank(variant.verdict) == 0U)
    {
        out.attribution = FindingAttribution::NoNewFinding;
    }
    else
    {
        out.attribution = FindingAttribution::Attributed;
        out.stressor    = out.comparison.changed;
    }
    return out;
}

} // namespace crd
