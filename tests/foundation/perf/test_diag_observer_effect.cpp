// DIAG.9c -- flight-recorder detail levels and observer-effect experiments (crd/perf/observer_effect.hpp).
//
// The detail cases drive a real FlightRecorder: a Basic policy keeps Warning and above, leaves informational events
// (frame boundaries among them) out and counts them, still fires every trigger, and says its level in the capture,
// the flight record and the summary.
//
// The experiment cases run a reproduction off (no recorder: the natural run), basic and full, and check the verdict
// against outcomes known before the experiment runs. Two reproductions use the engine's own instruments: a frame
// stream with one long gap, judged by its own frame budget and observed through FlightFrameWatch (no fault seams, so it
// runs on every lane, shipping included), and an allocation-failure plan armed on a real FaultInjector behind a
// FaultAllocator, observed through the injector's flight evidence (a shipping lane refuses the arm, so the natural run
// passes and the experiment answers not-reproduced). Their instrumented runs must fail exactly as the natural run did.
// The observer-effect verdict itself is shown with reproductions perturbed on purpose: each is synthetic, says so in
// its name, and models an instrument that consumes the resource the failure depends on.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9c); ADR-0133.

#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/perf/flight_triggers.hpp>
#include <crd/perf/observer_effect.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
using crd::u32;
using crd::u64;
using crd::usize;

constexpr cp::DiagAuthoritySet kInject = cp::authority_bit(cp::DiagAuthority::Inject);
constexpr u64                  kMs     = 1000000U;

bool contains(const cont::String& text, const char* needle)
{
    return std::strstr(text.c_str(), needle) != nullptr;
}

cp::FlightPolicy small_policy(cp::FlightDetail detail)
{
    cp::FlightPolicy p;
    p.capacity           = 64U;
    p.pre_events         = 32U;
    p.post_events        = 4U;
    p.post_window_ns     = 0U;
    p.hitch_threshold_ns = 33U * kMs;
    p.detail             = detail;
    return p;
}

cp::FlightTriggerInput manual(const char* message)
{
    cp::FlightTriggerInput in;
    in.kind    = cp::FlightTrigger::Manual;
    in.code    = cp::kFlightCodeManual;
    in.message = cont::StringView{message};
    return in;
}

bool has_code(const cp::FlightCapture& c, cp::EventCode code)
{
    for (usize i = 0U; i < c.events.size(); ++i)
    {
        if (c.events[i].code == code)
        {
            return true;
        }
    }
    return false;
}

usize count_code(const cp::FlightCapture& c, cp::EventCode code)
{
    usize n = 0U;
    for (usize i = 0U; i < c.events.size(); ++i)
    {
        n += (c.events[i].code == code) ? 1U : 0U;
    }
    return n;
}

// ---- reproductions ---------------------------------------------------------------------------------------------

// A frame stream: 24 frames on a 16 ms cadence, frame 17 starting after a 60 ms gap. The run fails when a frame
// interval exceeds its 33 ms budget, its signature the first such frame. With a recorder the stream is also fed to a
// FlightFrameWatch, whose hitch trigger is what the instrumented level captures.
constexpr u32 kFrames   = 24U;
constexpr u32 kGapFrame = 17U;

struct FrameStream
{
    // Synthetic: with a recorder at `perturb_at` (basic: at both levels; full: at full only) the gap comes one frame
    // early, as an instrument that cost the frame before it its budget would make it.
    bool             perturb    = false;
    cp::FlightDetail perturb_at = cp::FlightDetail::Basic;
    // Synthetic: every n-th run of the chosen kind (natural or instrumented) has no gap and passes (0: never).
    u32  flaky_every        = 0U;
    bool flaky_instrumented = false;
    u32  runs               = 0U;
    u32  natural_runs       = 0U;
    u32  instrumented_runs  = 0U;
};

void frame_stream(void* user, cp::FlightRecorder* recorder, cp::ObservedRun& out)
{
    auto&     s   = *static_cast<FrameStream*>(user);
    const u32 run = (recorder == nullptr) ? s.natural_runs++ : s.instrumented_runs++;
    ++s.runs;
    u32 gap = kGapFrame;
    if (s.perturb && recorder != nullptr &&
        (s.perturb_at == cp::FlightDetail::Basic || recorder->policy().detail == cp::FlightDetail::Full))
    {
        gap = kGapFrame - 1U; // an instrument that costs the frame before the gap its budget
    }
    const bool skip_gap = s.flaky_every != 0U && (run % s.flaky_every) == s.flaky_every - 1U &&
                          s.flaky_instrumented == (recorder != nullptr);

    cp::FlightFrameWatch watch(recorder);
    u64                  t        = 1000U * kMs;
    u64                  previous = 0U;
    out                           = cp::ObservedRun{};
    for (u32 f = 0U; f < kFrames; ++f)
    {
        t += (f == gap && !skip_gap) ? 60U * kMs : 16U * kMs;
        if (recorder != nullptr)
        {
            (void)watch.note_frame(t, t + 4U * kMs);
        }
        if (f > 0U && t - previous > 33U * kMs && !out.failed)
        {
            out.failed    = true;
            out.signature = f;
        }
        previous = t;
    }
}

// An allocation-failure plan on a real injector behind a FaultAllocator: 12 frames, one 16-byte attempt each; the run
// fails at the first attempt that comes back null, its signature that frame. With a recorder the injector records the
// fault and triggers, and the frames go to a FlightFrameWatch.
struct AllocatorStream
{
    cp::FaultInjector injector;
    cp::FaultPlan     plan;
    u32               arms_refused = 0U;
    bool              observer_allocates = false; // synthetic: an instrument that takes one attempt from the stream
};

void allocator_stream(void* user, cp::FlightRecorder* recorder, cp::ObservedRun& out)
{
    auto& s = *static_cast<AllocatorStream*>(user);
    out     = cp::ObservedRun{};
    s.injector.attach_flight_recorder(recorder);
    if (s.injector.arm(s.plan, kInject) != cp::FaultArmStatus::Armed)
    {
        ++s.arms_refused;
        s.injector.attach_flight_recorder(nullptr);
        return;
    }
    crd::memory::LinearAllocator linear(1U << 12U, nullptr, "observer-linear");
    cp::FaultAllocator           faulty(&linear, &s.injector);
    cp::FlightFrameWatch         watch(recorder);
    if (s.observer_allocates && recorder != nullptr)
    {
        (void)faulty.try_allocate(16U); // the instrument's own bookkeeping, from the allocator under test
    }
    u64 t = 1000U * kMs;
    for (u32 f = 0U; f < 12U; ++f)
    {
        t += 16U * kMs;
        if (recorder != nullptr)
        {
            (void)watch.note_frame(t, t + 4U * kMs);
        }
        if (faulty.try_allocate(16U) == nullptr && !out.failed)
        {
            out.failed    = true;
            out.signature = f;
        }
    }
    s.injector.disarm();
    s.injector.attach_flight_recorder(nullptr);
}

cp::FaultPlan plan_of(const char* text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(cont::StringView{text}, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

const cp::ObserverLevelResult& at(const cp::ObserverExperiment& e, cp::ObserverLevel l)
{
    return e.levels[static_cast<u32>(l)];
}
} // namespace

TEST_CASE("diag 9c detail: a basic policy keeps warnings and triggers and counts the informational events it drops",
          "[perf][diag][diag9c][observer]")
{
    cp::FlightPolicy bad = small_policy(cp::FlightDetail::Basic);
    bad.detail           = static_cast<cp::FlightDetail>(2U);
    CHECK(cp::validate_flight_policy(bad) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    CHECK(cp::flight_detail_name(cp::FlightDetail::Basic) == "basic");
    CHECK(cp::FlightPolicy{}.detail == cp::FlightDetail::Full);

    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(cp::FlightDetail::Basic)) == cp::FlightPolicyStatus::Committed);
    for (u32 i = 0U; i < 5U; ++i)
    {
        r.record(0x0042U, cp::Severity::Info, cont::StringView{"progress"}, i, 0U);
    }
    r.record(0x0043U, cp::Severity::Warning, cont::StringView{"anomaly"}, 1U, 0U);
    r.record(0x0044U, cp::Severity::RecoverableError, cont::StringView{"failed and recovered"}, 2U, 0U);
    CHECK(r.total_recorded() == 2U);
    CHECK(r.total_filtered() == 5U);

    REQUIRE(r.trigger(manual("basic")) == cp::FlightTriggerResult::Started);
    for (u32 i = 0U; i < 10U; ++i)
    {
        r.record(0x0042U, cp::Severity::Info, cont::StringView{"progress"}, i, 0U);
    }
    CHECK(r.capture_pending()); // informational events do not fill a basic post window
    for (u32 i = 0U; i < 4U; ++i)
    {
        r.record(0x0045U, cp::Severity::Warning, cont::StringView{"after"}, i, 0U);
    }
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(c.policy.detail == cp::FlightDetail::Basic);
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK(c.pre_retained == 2U);
    CHECK(c.post_retained == 4U);
    REQUIRE(c.events.size() == 6U);
    for (usize i = 0U; i < c.events.size(); ++i)
    {
        CHECK(c.events[i].severity >= cp::Severity::Warning);
    }
    CHECK(r.total_filtered() == 15U);

    // A hitch still triggers at basic, the frames themselves are left out, and the trigger carries the interval.
    cp::FlightRecorder frames;
    cp::FlightPolicy   quiet = small_policy(cp::FlightDetail::Basic);
    quiet.post_window_ns     = kMs; // nothing basic keeps arrives after the hitch: the time bound closes the window
    REQUIRE(frames.init(quiet) == cp::FlightPolicyStatus::Committed);
    cp::FlightFrameWatch watch(&frames);
    u64                  t = 1000U * kMs;
    for (u32 f = 0U; f < 6U; ++f)
    {
        t += (f == 4U) ? 50U * kMs : 16U * kMs;
        CHECK(watch.note_frame(t, t + 4U * kMs) == (f == 4U));
    }
    CHECK(frames.total_recorded() == 0U);
    CHECK(frames.total_filtered() == 6U);
    REQUIRE(frames.poll(cp::diagnostic_now_ns() + 10U * kMs));
    cp::FlightCapture hitch;
    REQUIRE(frames.take_capture(hitch));
    CHECK(hitch.trigger.kind == cp::FlightTrigger::FrameHitch);
    CHECK(hitch.trigger.value_a == 50U * kMs);
    CHECK(hitch.end == cp::FlightEnd::PostWindowElapsed);
    CHECK(hitch.events.empty());

    // Full keeps every event, as the recorder always did.
    cp::FlightRecorder full;
    REQUIRE(full.init(small_policy(cp::FlightDetail::Full)) == cp::FlightPolicyStatus::Committed);
    full.record(0x0042U, cp::Severity::Info, cont::StringView{"progress"}, 0U, 0U);
    CHECK(full.total_recorded() == 1U);
    CHECK(full.total_filtered() == 0U);
}

TEST_CASE("diag 9c detail: the level round-trips through a flight record, and a record naming no level is refused",
          "[perf][diag][diag9c][observer]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(cp::FlightDetail::Basic)) == cp::FlightPolicyStatus::Committed);
    r.record(0x0043U, cp::Severity::Warning, cont::StringView{"anomaly"}, 1U, 0U);
    REQUIRE(r.trigger(manual("round trip")) == cp::FlightTriggerResult::Started);
    for (u32 i = 0U; i < 4U; ++i)
    {
        r.record(0x0045U, cp::Severity::Warning, cont::StringView{"after"}, i, 0U);
    }
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));

    const cont::Array<crd::u8> payload = cp::encode_flight_record(c);
    REQUIRE(payload.size() > 8U);
    CHECK(payload[11] == 1U); // the byte after the flags (magic, version, kind, end, flags): the level
    const cp::FlightRecordRead back =
        cp::decode_flight_record(cont::ConstSpan<crd::u8>{payload.data(), payload.size()});
    REQUIRE(back.ok);
    CHECK(back.capture.policy.detail == cp::FlightDetail::Basic);
    cont::String json;
    cp::flight_capture_summary_json(c, json);
    CHECK(contains(json, "\"detail\":\"basic\""));

    // A full capture writes 0 there, the byte every record before detail levels held: those records read as full.
    cp::FlightCapture full = c;
    full.policy.detail     = cp::FlightDetail::Full;
    const cont::Array<crd::u8> full_payload = cp::encode_flight_record(full);
    CHECK(full_payload[11] == 0U);
    const cp::FlightRecordRead full_back =
        cp::decode_flight_record(cont::ConstSpan<crd::u8>{full_payload.data(), full_payload.size()});
    REQUIRE(full_back.ok);
    CHECK(full_back.capture.policy.detail == cp::FlightDetail::Full);

    cont::Array<crd::u8> bad = payload;
    bad[11]                  = 2U;
    const cp::FlightRecordRead refused = cp::decode_flight_record(cont::ConstSpan<crd::u8>{bad.data(), bad.size()});
    CHECK_FALSE(refused.ok);
    CHECK(refused.error == "unknown detail level");
}

TEST_CASE("diag 9c observer: a frame-budget failure fails the same way off, basic and full, each level capturing it",
          "[perf][diag][diag9c][observer]")
{
    FrameStream            stream;
    cp::ObserverExperiment e;
    cp::run_observer_experiment(&frame_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, e);
    INFO(e.reason.c_str());
    CHECK(e.verdict == cp::ObserverVerdict::Consistent);
    CHECK(stream.runs == 9U);

    // The natural failure is kept beside the captures.
    CHECK(e.natural.failed);
    CHECK(e.natural.signature == kGapFrame);
    const cp::ObserverLevelResult& off   = at(e, cp::ObserverLevel::Off);
    const cp::ObserverLevelResult& basic = at(e, cp::ObserverLevel::Basic);
    const cp::ObserverLevelResult& full  = at(e, cp::ObserverLevel::Full);
    CHECK(off.runs == 3U);
    CHECK(off.agreeing_runs == 3U);
    CHECK_FALSE(off.has_capture);
    for (const cp::ObserverLevelResult* level : {&basic, &full})
    {
        CHECK(level->runs == 3U);
        CHECK(level->agreeing_runs == 3U);
        CHECK(level->captured_runs == 3U);
        REQUIRE(level->has_capture);
        CHECK(level->capture.trigger.kind == cp::FlightTrigger::FrameHitch);
        CHECK(level->capture.trigger.value_a == 60U * kMs);
        CHECK(level->max_elapsed_ns >= level->min_elapsed_ns);
    }

    // Basic leaves every frame out and counts them; full keeps the frames up to the hitch and the post window.
    CHECK(basic.capture.policy.detail == cp::FlightDetail::Basic);
    CHECK(basic.recorded == 0U);
    CHECK(basic.filtered == kFrames);
    CHECK(basic.capture.events.empty());
    CHECK(full.capture.policy.detail == cp::FlightDetail::Full);
    CHECK(full.filtered == 0U);
    // The window closed when its 4 post events landed; the 2 frames after it are counted, not recorded.
    CHECK(full.recorded == kGapFrame + 1U + 4U);
    CHECK(full.capture.end == cp::FlightEnd::PostEventsFull);
    CHECK(full.capture.dropped_while_complete == kFrames - (kGapFrame + 1U + 4U));
    CHECK(count_code(full.capture, cp::kFlightCodeFrame) == kGapFrame + 1U + 4U);
    CHECK(full.capture.events[kGapFrame].value_a == 60U * kMs); // the hitch frame ends the pre window

    cont::String json;
    cp::observer_experiment_summary_json(e, json);
    INFO(json.c_str());
    CHECK(contains(json, "\"verdict\":\"consistent\""));
    CHECK(contains(json, "\"natural\":{\"failed\":true,\"signature\":17}"));
    CHECK(contains(json, "\"level\":\"basic\""));
    CHECK(contains(json, "\"detail\":\"basic\""));
}

TEST_CASE("diag 9c observer: an allocation-failure plan on a real injector is consistent at every level",
          "[perf][diag][diag9c][observer]")
{
    AllocatorStream stream;
    stream.plan = plan_of("seed=0;allocation-failure@7+1%1000000");
    cp::ObserverExperiment e;
    cp::run_observer_experiment(&allocator_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, e);
    INFO(e.reason.c_str());
    if (!crd::fault_seams_compiled())
    {
        // Shipping: the arm is refused, the natural run cannot fail, and nothing instrumented runs.
        CHECK(e.verdict == cp::ObserverVerdict::NotReproduced);
        CHECK(stream.arms_refused == 3U);
        CHECK(at(e, cp::ObserverLevel::Basic).runs == 0U);
        return;
    }
    CHECK(stream.arms_refused == 0U);
    CHECK(e.verdict == cp::ObserverVerdict::Consistent);
    CHECK(e.natural.failed);
    CHECK(e.natural.signature == 7U);

    // Both captures hold the injected fault; basic drops the frames around it, full keeps them.
    const cp::ObserverLevelResult& basic = at(e, cp::ObserverLevel::Basic);
    const cp::ObserverLevelResult& full  = at(e, cp::ObserverLevel::Full);
    REQUIRE(basic.has_capture);
    REQUIRE(full.has_capture);
    for (const cp::FlightCapture* c : {&basic.capture, &full.capture})
    {
        CHECK(c->trigger.kind == cp::FlightTrigger::InjectedFault);
        CHECK(c->trigger.value_b == 7U);
        CHECK(count_code(*c, cp::kFlightCodeFaultInjected) == 1U);
        CHECK(c->replay_available);
    }
    CHECK_FALSE(has_code(basic.capture, cp::kFlightCodeFrame));
    CHECK(basic.filtered == 12U);
    CHECK(count_code(full.capture, cp::kFlightCodeFrame) == 12U);
    for (usize i = 0U; i < basic.capture.events.size(); ++i)
    {
        CHECK(has_code(full.capture, basic.capture.events[i].code)); // basic's events are a subset of full's
    }

    // A plan that does not trigger the recorder still fails the run, but no level captures it: the instrument missed
    // the failure, which is its failure, never a pass.
    stream.plan = plan_of("seed=0;notrigger;allocation-failure@7+1%1000000");
    cp::ObserverExperiment missed;
    cp::run_observer_experiment(&allocator_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, missed);
    INFO(missed.reason.c_str());
    CHECK(missed.verdict == cp::ObserverVerdict::InstrumentFailure);
    CHECK(missed.culprit == cp::ObserverLevel::Basic);
    CHECK(at(missed, cp::ObserverLevel::Basic).agreeing_runs == 3U);
    CHECK(at(missed, cp::ObserverLevel::Basic).captured_runs == 0U);
    CHECK(missed.natural.signature == 7U);

    // Synthetic perturbation: an instrument that takes one attempt from the allocator under test moves the fault onto
    // the frame before; the experiment names the level, and the natural failure stays as it was.
    stream.plan               = plan_of("seed=0;allocation-failure@7+1%1000000");
    stream.observer_allocates = true;
    cp::ObserverExperiment moved;
    cp::run_observer_experiment(&allocator_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, moved);
    INFO(moved.reason.c_str());
    CHECK(moved.verdict == cp::ObserverVerdict::ObserverEffect);
    CHECK(moved.culprit == cp::ObserverLevel::Basic);
    CHECK(moved.natural.signature == 7U);
    CHECK(at(moved, cp::ObserverLevel::Basic).differing.signature == 6U);
    CHECK(at(moved, cp::ObserverLevel::Full).runs == 3U);
}

TEST_CASE("diag 9c observer: synthetic perturbations, unstable baselines and refused policies get their verdicts",
          "[perf][diag][diag9c][observer]")
{
    SECTION("synthetic: an instrument at full only moves the failure")
    {
        FrameStream stream;
        stream.perturb    = true;
        stream.perturb_at = cp::FlightDetail::Full;
        cp::ObserverExperiment e;
        cp::run_observer_experiment(&frame_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, e);
        INFO(e.reason.c_str());
        CHECK(e.verdict == cp::ObserverVerdict::ObserverEffect);
        CHECK(e.culprit == cp::ObserverLevel::Full);
        CHECK(at(e, cp::ObserverLevel::Basic).agreeing_runs == 3U);
        CHECK(at(e, cp::ObserverLevel::Full).agreeing_runs == 0U);
        CHECK(at(e, cp::ObserverLevel::Full).differing.signature == kGapFrame - 1U);
        CHECK(e.natural.signature == kGapFrame);
        CHECK(contains(e.reason, "at full 3 of 3 runs differed"));
    }
    SECTION("synthetic: an instrumented level that fails in only some runs is an observer effect")
    {
        // The natural runs are stable; every third instrumented run passes, so basic fails in 2 of its 3 runs.
        FrameStream stream;
        stream.flaky_every        = 3U;
        stream.flaky_instrumented = true;
        cp::ObserverExperiment e;
        cp::run_observer_experiment(&frame_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, e);
        INFO(e.reason.c_str());
        CHECK(e.verdict == cp::ObserverVerdict::ObserverEffect);
        CHECK(e.culprit == cp::ObserverLevel::Basic);
        CHECK(at(e, cp::ObserverLevel::Off).agreeing_runs == 3U);
        CHECK(at(e, cp::ObserverLevel::Basic).agreeing_runs == 2U);
        CHECK_FALSE(at(e, cp::ObserverLevel::Basic).differing.failed);
        CHECK(contains(e.reason, "at basic 1 of 3 runs differed from the natural failure: passed"));
    }
    SECTION("synthetic: natural runs that disagree leave the observer unjudged")
    {
        FrameStream stream;
        stream.flaky_every = 3U; // the third natural run passes
        cp::ObserverExperiment e;
        cp::run_observer_experiment(&frame_stream, &stream, small_policy(cp::FlightDetail::Full), 3U, e);
        INFO(e.reason.c_str());
        CHECK(e.verdict == cp::ObserverVerdict::NaturalUnstable);
        CHECK(at(e, cp::ObserverLevel::Off).agreeing_runs == 2U);
        CHECK(at(e, cp::ObserverLevel::Basic).runs == 0U);
        CHECK(stream.instrumented_runs == 0U);
    }
    SECTION("a reproduction that passes, and a policy the recorder refuses")
    {
        FrameStream stream;
        cp::FlightPolicy refused = small_policy(cp::FlightDetail::Full);
        refused.capacity         = 1U;
        cp::ObserverExperiment e;
        cp::run_observer_experiment(&frame_stream, &stream, refused, 3U, e);
        CHECK(e.verdict == cp::ObserverVerdict::InstrumentFailure);
        CHECK(stream.runs == 0U);
        CHECK(contains(e.reason, "rejected-out-of-bounds"));

        const auto passing = [](void*, cp::FlightRecorder*, cp::ObservedRun& out)
        {
            out = cp::ObservedRun{};
        };
        cp::ObserverExperiment quiet;
        cp::run_observer_experiment(passing, nullptr, small_policy(cp::FlightDetail::Full), 3U, quiet);
        CHECK(quiet.verdict == cp::ObserverVerdict::NotReproduced);
        CHECK(at(quiet, cp::ObserverLevel::Off).runs == 3U);
        CHECK(at(quiet, cp::ObserverLevel::Full).runs == 0U);
    }
    SECTION("a policy that masks the failure's trigger misses it at every level")
    {
        FrameStream      stream;
        cp::FlightPolicy masked = small_policy(cp::FlightDetail::Full);
        masked.trigger_mask     = cp::flight_trigger_bit(cp::FlightTrigger::Manual);
        cp::ObserverExperiment e;
        cp::run_observer_experiment(&frame_stream, &stream, masked, 2U, e);
        INFO(e.reason.c_str());
        CHECK(e.verdict == cp::ObserverVerdict::InstrumentFailure);
        CHECK(e.culprit == cp::ObserverLevel::Basic);
        CHECK(at(e, cp::ObserverLevel::Full).captured_runs == 0U);
        CHECK(contains(e.reason, "only 0 of 2 runs delivered a capture"));
    }
}
