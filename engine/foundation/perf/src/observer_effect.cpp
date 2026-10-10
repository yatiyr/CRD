// crd-perf -- observer-effect experiments on a failing reproduction (DIAG.9c). See crd/perf/observer_effect.hpp.

#include <crd/perf/observer_effect.hpp>

#include "json_writer.hpp"

#include <cstdio>
#include <initializer_list>
#include <mutex>

namespace crd::perf
{
namespace
{
using detail::append_json_escaped;
using detail::append_u64;

// Collects the captures one instrumented run delivers. The sink may run on any thread the run polls from.
struct Collector
{
    std::mutex     mutex;
    FlightCapture* keep      = nullptr; // where the first capture goes (nullptr: count only)
    bool           kept      = false;
    crd::u64       delivered = 0U;
};

void collect(const FlightCapture& capture, void* user)
{
    auto&                             c = *static_cast<Collector*>(user);
    const std::lock_guard<std::mutex> guard(c.mutex);
    ++c.delivered;
    if (c.keep != nullptr && !c.kept)
    {
        *c.keep = capture;
        c.kept  = true;
    }
}

bool same_outcome(const ObservedRun& a, const ObservedRun& b) noexcept
{
    return a.failed == b.failed && (!a.failed || a.signature == b.signature);
}

void note_elapsed(ObserverLevelResult& level, crd::u64 elapsed) noexcept
{
    if (level.runs == 1U || elapsed < level.min_elapsed_ns)
    {
        level.min_elapsed_ns = elapsed;
    }
    if (elapsed > level.max_elapsed_ns)
    {
        level.max_elapsed_ns = elapsed;
    }
}

// Judge one run against `reference`, counting it into `level`.
void count_run(ObserverLevelResult& level, const ObservedRun& got, const ObservedRun& reference) noexcept
{
    if (level.runs == 1U)
    {
        level.first = got;
    }
    if (got.failed)
    {
        ++level.failed_runs;
    }
    if (same_outcome(got, reference))
    {
        ++level.agreeing_runs;
    }
    else if (level.agreeing_runs + 1U == level.runs)
    {
        level.differing = got; // the first run that did not agree
    }
}

void describe_outcome(cont::String& out, const ObservedRun& r)
{
    if (!r.failed)
    {
        out.append("passed");
        return;
    }
    char text[48];
    (void)std::snprintf(text, sizeof(text), "failed with signature %llu", static_cast<unsigned long long>(r.signature));
    out.append(text);
}

// Append "<a> of <b>" (the counts a verdict's reason names).
void append_counts(cont::String& out, crd::u32 a, crd::u32 b)
{
    char buffer[32];
    (void)std::snprintf(buffer, sizeof(buffer), "%u of %u", static_cast<unsigned>(a), static_cast<unsigned>(b));
    out.append(buffer);
}

// K runs at an instrumented level, each on a fresh recorder. False when the recorder refused the policy.
bool run_instrumented(ObserverRunOnce run, void* user, const FlightPolicy& policy, crd::u32 k,
                      const ObservedRun& natural, ObserverLevelResult& level, cont::String& reason)
{
    for (crd::u32 i = 0U; i < k; ++i)
    {
        FlightRecorder           recorder;
        const FlightPolicyStatus status = recorder.init(policy);
        if (status != FlightPolicyStatus::Committed)
        {
            reason.append("the recorder refused the ");
            reason.append(observer_level_name(level.level));
            reason.append(" policy: ");
            reason.append(flight_policy_status_name(status));
            return false;
        }
        Collector collector;
        collector.keep = (i == 0U) ? &level.capture : nullptr;
        recorder.set_sink(&collect, &collector);

        ObservedRun    got;
        const crd::u64 begin = diagnostic_now_ns();
        run(user, &recorder, got);
        const crd::u64 end = diagnostic_now_ns();

        (void)recorder.poll();
        if (i == 0U)
        {
            level.recorded = recorder.total_recorded();
            level.filtered = recorder.total_filtered();
        }
        recorder.shutdown(); // delivers a capture whose post window is still open

        ++level.runs;
        note_elapsed(level, end >= begin ? end - begin : 0U);
        count_run(level, got, natural);
        const std::lock_guard<std::mutex> guard(collector.mutex);
        level.captures += collector.delivered;
        if (collector.delivered > 0U)
        {
            ++level.captured_runs;
        }
        if (i == 0U)
        {
            level.has_capture = collector.kept;
        }
    }
    return true;
}
} // namespace

cont::StringView observer_level_name(ObserverLevel l) noexcept
{
    switch (l)
    {
    case ObserverLevel::Off:   return cont::StringView{"off"};
    case ObserverLevel::Basic: return cont::StringView{"basic"};
    case ObserverLevel::Full:  return cont::StringView{"full"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView observer_verdict_name(ObserverVerdict v) noexcept
{
    switch (v)
    {
    case ObserverVerdict::Consistent:        return cont::StringView{"consistent"};
    case ObserverVerdict::NotReproduced:     return cont::StringView{"not-reproduced"};
    case ObserverVerdict::NaturalUnstable:   return cont::StringView{"natural-unstable"};
    case ObserverVerdict::ObserverEffect:    return cont::StringView{"observer-effect"};
    case ObserverVerdict::InstrumentFailure: return cont::StringView{"instrument-failure"};
    }
    return cont::StringView{"unknown"};
}

void run_observer_experiment(ObserverRunOnce run, void* user, const FlightPolicy& base, crd::u32 runs_per_level,
                             ObserverExperiment& out)
{
    out = ObserverExperiment(); // not {}: copy-list-initialization cannot use the explicit String constructor
    for (crd::u32 l = 0U; l < kObserverLevelCount; ++l)
    {
        out.levels[l].level = static_cast<ObserverLevel>(l);
    }
    crd::u32 k = runs_per_level;
    if (k == 0U)
    {
        k = 1U;
    }
    if (k > kObserverMaxRuns)
    {
        k = kObserverMaxRuns;
    }

    FlightPolicy basic = base;
    basic.detail       = FlightDetail::Basic;
    FlightPolicy full  = base;
    full.detail        = FlightDetail::Full;
    for (const FlightPolicy* p : {&basic, &full})
    {
        const FlightPolicyStatus status = validate_flight_policy(*p);
        if (status != FlightPolicyStatus::Committed)
        {
            out.verdict = ObserverVerdict::InstrumentFailure;
            out.culprit = (p == &basic) ? ObserverLevel::Basic : ObserverLevel::Full;
            out.reason.append("the flight policy is refused (");
            out.reason.append(flight_policy_status_name(status));
            out.reason.append("); nothing ran");
            return;
        }
    }

    // The natural runs: no recorder at all. The first is the reference the others, and every level, are held to.
    ObserverLevelResult& off = out.levels[static_cast<crd::u32>(ObserverLevel::Off)];
    for (crd::u32 i = 0U; i < k; ++i)
    {
        ObservedRun    got;
        const crd::u64 begin = diagnostic_now_ns();
        run(user, nullptr, got);
        const crd::u64 end = diagnostic_now_ns();
        ++off.runs;
        note_elapsed(off, end >= begin ? end - begin : 0U);
        count_run(off, got, i == 0U ? got : off.first);
    }
    out.natural = off.first;
    if (off.agreeing_runs != off.runs)
    {
        out.verdict = ObserverVerdict::NaturalUnstable;
        out.reason.append("the natural runs disagree (");
        append_counts(out.reason, off.agreeing_runs, off.runs);
        out.reason.append(" matched the first); the observer cannot be judged");
        return;
    }
    if (!out.natural.failed)
    {
        out.verdict = ObserverVerdict::NotReproduced;
        out.reason.append("the natural runs passed (");
        append_counts(out.reason, off.runs, off.runs);
        out.reason.append("); there is no failure to observe");
        return;
    }

    ObserverLevelResult& at_basic = out.levels[static_cast<crd::u32>(ObserverLevel::Basic)];
    ObserverLevelResult& at_full  = out.levels[static_cast<crd::u32>(ObserverLevel::Full)];
    if (!run_instrumented(run, user, basic, k, out.natural, at_basic, out.reason) ||
        !run_instrumented(run, user, full, k, out.natural, at_full, out.reason))
    {
        out.verdict = ObserverVerdict::InstrumentFailure;
        out.culprit = (at_basic.runs < k) ? ObserverLevel::Basic : ObserverLevel::Full;
        return;
    }

    // A changed outcome outranks a missed capture: it says the instrument altered the thing it observed.
    for (const ObserverLevelResult* level : {&at_basic, &at_full})
    {
        if (level->agreeing_runs != level->runs)
        {
            out.verdict = ObserverVerdict::ObserverEffect;
            out.culprit = level->level;
            out.reason.append("at ");
            out.reason.append(observer_level_name(level->level));
            out.reason.push_back(' ');
            append_counts(out.reason, level->runs - level->agreeing_runs, level->runs);
            out.reason.append(" runs differed from the natural failure: ");
            describe_outcome(out.reason, level->differing);
            out.reason.append(" where the natural run ");
            describe_outcome(out.reason, out.natural);
            return;
        }
    }
    for (const ObserverLevelResult* level : {&at_basic, &at_full})
    {
        if (level->captured_runs != level->runs || !level->has_capture)
        {
            out.verdict = ObserverVerdict::InstrumentFailure;
            out.culprit = level->level;
            out.reason.append("at ");
            out.reason.append(observer_level_name(level->level));
            out.reason.append(" the failure reproduced but only ");
            append_counts(out.reason, level->captured_runs, level->runs);
            out.reason.append(" runs delivered a capture");
            return;
        }
    }
    out.verdict = ObserverVerdict::Consistent;
    const crd::u32 captured = at_basic.captured_runs + at_full.captured_runs;
    char           text[128];
    (void)std::snprintf(text, sizeof(text),
                        "every level failed the natural way in all %u runs; every instrumented run captured it (%u)",
                        static_cast<unsigned>(k), static_cast<unsigned>(captured));
    out.reason.append(text);
}

void observer_experiment_summary_json(const ObserverExperiment& e, cont::String& out)
{
    const auto key = [&out](const char* name, bool comma)
    {
        if (comma)
        {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(name);
        out.append("\":");
    };
    const auto str = [&out](cont::StringView v)
    {
        out.push_back('"');
        append_json_escaped(out, v);
        out.push_back('"');
    };
    const auto outcome = [&](const ObservedRun& r)
    {
        out.append("{\"failed\":");
        out.append(r.failed ? "true" : "false");
        key("signature", true);
        append_u64(out, r.signature);
        out.push_back('}');
    };

    out.push_back('{');
    key("verdict", false);
    str(observer_verdict_name(e.verdict));
    key("culprit", true);
    str(observer_level_name(e.culprit));
    key("reason", true);
    str(cont::StringView{e.reason.c_str(), e.reason.size()});
    key("natural", true);
    outcome(e.natural);
    key("levels", true);
    out.push_back('[');
    for (crd::u32 l = 0U; l < kObserverLevelCount; ++l)
    {
        const ObserverLevelResult& r = e.levels[l];
        if (l > 0U)
        {
            out.push_back(',');
        }
        out.push_back('{');
        key("level", false);
        str(observer_level_name(r.level));
        key("runs", true);
        append_u64(out, r.runs);
        key("failed_runs", true);
        append_u64(out, r.failed_runs);
        key("agreeing_runs", true);
        append_u64(out, r.agreeing_runs);
        key("first", true);
        outcome(r.first);
        key("min_elapsed_ns", true);
        append_u64(out, r.min_elapsed_ns);
        key("max_elapsed_ns", true);
        append_u64(out, r.max_elapsed_ns);
        if (r.level != ObserverLevel::Off)
        {
            key("captured_runs", true);
            append_u64(out, r.captured_runs);
            key("captures", true);
            append_u64(out, r.captures);
            key("recorded", true);
            append_u64(out, r.recorded);
            key("filtered", true);
            append_u64(out, r.filtered);
            key("capture", true);
            if (r.has_capture)
            {
                flight_capture_summary_json(r.capture, out);
                key("capture_events", true);
                append_u64(out, r.capture.events.size());
            }
            else
            {
                out.append("null");
            }
        }
        out.push_back('}');
    }
    out.append("]}");
}

} // namespace crd::perf
