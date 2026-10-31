// DIAG.9b -- seeded, authorized fault injection. A FaultInjector arms a seeded plan only for a grant holding Inject and
// only in a build with the fault seams (every build but shipping); a fired fault drives an existing seam's failure
// path, is recorded into the flight recorder with a recipe whose one-hit plan reproduces it, and fires an
// InjectedFault trigger. On a shipping lane every arm is refused Unavailable and the decorators pass calls through:
// those cases assert that branch instead.
//
// The I/O sites are driven end to end through crd-platform's reads in crd-platform-tests and through the resource
// manager's load and reload paths in crd-resources-tests; here the hook's decisions are checked directly.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/core/fault_seam.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;

constexpr cp::DiagAuthoritySet kReadRecord =
    cp::authority_bit(cp::DiagAuthority::Read) | cp::authority_bit(cp::DiagAuthority::Record);
constexpr cp::DiagAuthoritySet kInjectRead =
    cp::authority_bit(cp::DiagAuthority::Inject) | cp::authority_bit(cp::DiagAuthority::Read);

cp::FaultPlan plan_of(cont::StringView text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(text, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

cp::FlightPolicy small_policy()
{
    cp::FlightPolicy p;
    p.capacity       = 32U;
    p.pre_events     = 16U;
    p.post_events    = 2U;
    p.post_window_ns = 0U;
    return p;
}

// The one-hit plan a recipe names after "minimal=" (up to the next space).
cont::String minimal_of(const cont::String& recipe)
{
    const cont::StringView view{recipe.data(), recipe.size()};
    const cont::StringView key{"minimal="};
    const crd::usize       at = view.find(key);
    REQUIRE(at != cont::StringView::npos);
    const crd::usize begin = at + key.size();
    crd::usize       end   = view.find(' ', begin);
    if (end == cont::StringView::npos)
    {
        end = view.size();
    }
    return cont::String{view.substr(begin, end - begin)};
}
} // namespace

TEST_CASE("fault plan: the text round-trips and malformed or invalid plans are refused", "[perf][diag][diag9b][fault]")
{
    const cp::FaultPlan plan =
        plan_of("seed=42;allocation-failure@3+1%1000000;partial-read@0+0%250000=16;delayed-completion@2+5%1000000="
                "30000000");
    CHECK(plan.seed == 42U);
    CHECK(plan.trigger_flight);
    REQUIRE(plan.rule_count == 3U);
    CHECK(plan.rules[0].site == cp::FaultSite::AllocationFailure);
    CHECK(plan.rules[0].first_hit == 3U);
    CHECK(plan.rules[1].hit_limit == 0U);
    CHECK(plan.rules[1].probability_ppm == 250000U);
    CHECK(plan.rules[1].value == 16U);
    CHECK(plan.rules[2].value == 30000000U);

    cont::String text;
    cp::fault_plan_to_text(plan, text);
    CHECK(text == "seed=42;allocation-failure@3+1%1000000;partial-read@0+0%250000=16;"
                  "delayed-completion@2+5%1000000=30000000");
    const cp::FaultPlan again = plan_of(cont::StringView{text.data(), text.size()});
    cont::String        text_again;
    cp::fault_plan_to_text(again, text_again);
    CHECK(text_again == text);
    CHECK_FALSE(plan_of("seed=1;notrigger;failed-read@0+1%1000000").trigger_flight);

    const char* const refused[] = {
        "",                                                // no seed
        "allocation-failure@0+1%1000000",                  // no seed first
        "seed=1",                                          // no rules
        "seed=x;failed-read@0+1%1000000",                  // seed not decimal
        "seed=1;failed-read@0+1%1000000;",                 // an empty trailing rule
        "seed=1;no-such-site@0+1%1000000",                 // unknown site
        "seed=1;failed-read@0+1%0",                        // zero probability
        "seed=1;failed-read@0+1%1000001",                  // probability above always
        "seed=1;failed-read@0+1%1000000=5",                // a value on a site that takes none
        "seed=1;delayed-completion@0+1%1000000",           // a delay of zero
        "seed=1;delayed-completion@0+1%1000000=10000000001", // a delay above 10 s
        "seed=1;failed-read@18446744073709551615+2%1000000", // first + limit overflows
        "seed=1;failed-read@0+1",                          // no probability
        "seed=1;notrigger;notrigger;failed-read@0+1%1000000", // notrigger only once, second
        "seed=99999999999999999999;failed-read@0+1%1000000",  // seed above u64
    };
    for (const char* t : refused)
    {
        cp::FaultPlan untouched;
        untouched.seed = 7U;
        cont::String reason;
        INFO(t);
        CHECK_FALSE(cp::parse_fault_plan(cont::StringView{t}, untouched, reason));
        CHECK_FALSE(reason.empty());
        CHECK(untouched.seed == 7U);
    }
    cp::FaultPlan nine;
    nine.rule_count = 9U;
    CHECK(cp::validate_fault_plan(nine) == cp::FaultPlanError::NoRules);
    cp::FaultPlan wrong_version = plan;
    wrong_version.schema_version = 2U;
    CHECK(cp::validate_fault_plan(wrong_version) == cp::FaultPlanError::UnknownVersion);

    // The longest valid plan (every number at its widest) is written whole and reads back the same.
    cp::FaultPlan longest;
    longest.seed           = ~0ULL;
    longest.trigger_flight = false;
    longest.rule_count     = cp::kFaultMaxRules;
    for (cp::FaultRule& r : longest.rules)
    {
        r = cp::FaultRule{cp::FaultSite::PartialRead, ~0ULL >> 1U, ~0ULL >> 1U, cp::kFaultPpmAlways - 1U, ~0ULL};
    }
    REQUIRE(cp::validate_fault_plan(longest) == cp::FaultPlanError::None);
    cont::String longest_text;
    cp::fault_plan_to_text(longest, longest_text);
    CHECK(longest_text.size() <= cp::kFaultMaxPlanText);
    CHECK(longest_text.size() > 600U);
    const cp::FaultPlan longest_again = plan_of(cont::StringView{longest_text.data(), longest_text.size()});
    cont::String        longest_again_text;
    cp::fault_plan_to_text(longest_again, longest_again_text);
    CHECK(longest_again_text == longest_text);
    CHECK_FALSE(longest_again.trigger_flight);
}

TEST_CASE("fault plan: firing is a pure function of seed, site and hit", "[perf][diag][diag9b][fault]")
{
    const cp::FaultPlan a  = plan_of("seed=11;partial-read@100+1000%300000=4");
    const cp::FaultPlan a2 = plan_of("seed=11;partial-read@100+1000%300000=4");
    const cp::FaultPlan b  = plan_of("seed=12;partial-read@100+1000%300000=4");

    crd::u32 fired_a = 0U;
    crd::u32 differ  = 0U;
    for (crd::u64 hit = 0U; hit < 1200U; ++hit)
    {
        const bool fa = cp::fault_plan_fires(a, cp::FaultSite::PartialRead, hit);
        CHECK(fa == cp::fault_plan_fires(a2, cp::FaultSite::PartialRead, hit));
        CHECK_FALSE(cp::fault_plan_fires(a, cp::FaultSite::FailedRead, hit)); // another site never fires
        if (hit < 100U || hit >= 1100U)
        {
            CHECK_FALSE(fa); // outside the covered hits
        }
        fired_a += fa ? 1U : 0U;
        differ += (fa != cp::fault_plan_fires(b, cp::FaultSite::PartialRead, hit)) ? 1U : 0U;
    }
    // 30 % of 1000 covered hits, seeded: a loose band, and another seed picks other hits.
    CHECK(fired_a > 200U);
    CHECK(fired_a < 400U);
    CHECK(differ > 100U);

    crd::u32 rule = 99U;
    const cp::FaultPlan always = plan_of("seed=5;failed-read@4+2%1000000;failed-read@0+0%1000000");
    REQUIRE(cp::fault_plan_fires(always, cp::FaultSite::FailedRead, 4U, &rule));
    CHECK(rule == 0U);
    REQUIRE(cp::fault_plan_fires(always, cp::FaultSite::FailedRead, 6U, &rule));
    CHECK(rule == 1U);
}

TEST_CASE("fault injector: arming needs the inject grant and a build with fault seams", "[perf][diag][diag9b][fault]")
{
    const cp::FaultPlan plan = plan_of("seed=3;allocation-failure@0+0%1000000");
    cp::FaultInjector   injector;

    CHECK(injector.arm(plan, kReadRecord) == cp::FaultArmStatus::Unauthorized);
    CHECK(injector.arm(plan, cp::kDiagAllAuthorities & ~cp::authority_bit(cp::DiagAuthority::Inject)) ==
          cp::FaultArmStatus::Unauthorized);
    CHECK_FALSE(injector.armed());

    crd::memory::LinearAllocator linear(4096U, nullptr, "fault-linear");
    cp::FaultAllocator           faulty(&linear, &injector);

    if (crd::fault_seams_compiled())
    {
        cp::FaultPlan         bad = plan;
        cp::FaultPlanError    error = cp::FaultPlanError::None;
        bad.rules[0].probability_ppm = 0U;
        CHECK(injector.arm(bad, kInjectRead, &error) == cp::FaultArmStatus::Rejected);
        CHECK(error == cp::FaultPlanError::BadProbability);
        CHECK_FALSE(injector.armed());

        REQUIRE(injector.arm(plan, kInjectRead) == cp::FaultArmStatus::Armed);
        CHECK(injector.armed());
        CHECK(injector.generation() == 1U);
        CHECK(faulty.try_allocate(64U, 16U) == nullptr);
        CHECK(faulty.injected() == 1U);
        CHECK(injector.hits(cp::FaultSite::AllocationFailure) == 1U);
        CHECK(injector.fired(cp::FaultSite::AllocationFailure) == 1U);
        // allocate() is never failed: its contract is non-null or fatal.
        void* p = faulty.allocate(64U, 16U);
        CHECK(p != nullptr);
        CHECK(faulty.owns(p));
        faulty.deallocate(p);
        CHECK(injector.hits(cp::FaultSite::AllocationFailure) == 1U);

        injector.disarm();
        CHECK_FALSE(injector.armed());
        void* q = faulty.try_allocate(64U, 16U);
        CHECK(q != nullptr);
        CHECK(injector.hits(cp::FaultSite::AllocationFailure) == 1U); // a disarmed injector counts nothing
        faulty.deallocate(q);
    }
    else
    {
        CHECK(injector.arm(plan, kInjectRead) == cp::FaultArmStatus::Unavailable);
        CHECK_FALSE(injector.armed());
        CHECK(injector.generation() == 0U);
        void* q = faulty.try_allocate(64U, 16U);
        CHECK(q != nullptr);
        CHECK(faulty.injected() == 0U);
        CHECK(injector.hits(cp::FaultSite::AllocationFailure) == 0U);
        faulty.deallocate(q);
    }
}

TEST_CASE("fault commands: fault.arm is refused without inject, and unavailable in a shipping build",
          "[perf][diag][diag9b][fault]")
{
    cp::FaultInjector     injector;
    cp::DiagServiceConfig config;

    cp::DiagArg     plan_arg{"plan", "seed=9;allocation-failure@1+1%1000000"};
    cp::DiagRequest arm;
    arm.command = "fault.arm";
    arm.args    = cont::ConstSpan<cp::DiagArg>{&plan_arg, 1U};
    cp::DiagRequest status;
    status.command = "fault.status";
    cp::DiagRequest disarm;
    disarm.command = "fault.disarm";

    {
        cp::DiagCommandService service{kReadRecord, config};
        REQUIRE(cp::register_fault_commands(service, injector));
        const crd::u64       runs_before = service.handler_runs();
        const cp::DiagResult refused     = service.execute(arm);
        CHECK(refused.status == cp::DiagStatus::Unauthorized);
        CHECK(service.execute(disarm).status == cp::DiagStatus::Unauthorized);
        CHECK(service.handler_runs() == runs_before);
        CHECK_FALSE(injector.armed());

        const cp::DiagResult seen = service.execute(status);
        REQUIRE(seen.status == cp::DiagStatus::Ok);
        CHECK(std::strstr(seen.json.c_str(), "\"armed\":false") != nullptr);
        CHECK(seen.total == cp::kFaultSiteCount);
    }

    cp::DiagCommandService service{kInjectRead, config};
    REQUIRE(cp::register_fault_commands(service, injector));

    cp::DiagArg     bad_arg{"plan", "seed=9;allocation-failure@1+1%0"};
    cp::DiagRequest bad = arm;
    bad.args            = cont::ConstSpan<cp::DiagArg>{&bad_arg, 1U};
    const cp::DiagResult rejected = service.execute(bad);
    CHECK(rejected.status == cp::DiagStatus::BadArgument);
    CHECK(cont::StringView{rejected.reason.data(), rejected.reason.size()}.find("bad-probability") !=
          cont::StringView::npos);

    const cp::DiagResult armed = service.execute(arm);
    if (crd::fault_seams_compiled())
    {
        REQUIRE(armed.status == cp::DiagStatus::Ok);
        CHECK(injector.armed());
        CHECK(std::strstr(armed.json.c_str(), "\"compiled\":true,\"armed\":true") != nullptr);
        CHECK(std::strstr(armed.json.c_str(), "\"plan\":\"seed=9;allocation-failure@1+1%1000000\"") != nullptr);
        REQUIRE(service.execute(disarm).status == cp::DiagStatus::Ok);
        CHECK_FALSE(injector.armed());
    }
    else
    {
        CHECK(armed.status == cp::DiagStatus::Unavailable);
        CHECK(cont::StringView{armed.reason.data(), armed.reason.size()}.find("compiled out") !=
              cont::StringView::npos);
        CHECK_FALSE(injector.armed());
        const cp::DiagResult seen = service.execute(status);
        REQUIRE(seen.status == cp::DiagStatus::Ok);
        CHECK(std::strstr(seen.json.c_str(), "\"compiled\":false,\"armed\":false") != nullptr);
    }
}

TEST_CASE("fault injector: a fired allocation fault triggers the flight recorder with a reproducing recipe",
          "[perf][diag][diag9b][fault]")
{
    if (!crd::fault_seams_compiled())
    {
        cp::FaultInjector injector;
        CHECK(injector.arm(plan_of("seed=7;allocation-failure@2+1%1000000"), kInjectRead) ==
              cp::FaultArmStatus::Unavailable);
        return; // the shipping branch: nothing can be armed, so nothing fires (asserted in the arming case too)
    }

    cp::FlightRecorder recorder;
    REQUIRE(recorder.init(small_policy()) == cp::FlightPolicyStatus::Committed);
    cp::FaultInjector injector;
    injector.attach_flight_recorder(&recorder);
    crd::memory::LinearAllocator linear(1U << 16U, nullptr, "fault-linear");
    cp::FaultAllocator           faulty(&linear, &injector);

    REQUIRE(injector.arm(plan_of("seed=7;allocation-failure@2+1%1000000"), kInjectRead) == cp::FaultArmStatus::Armed);
    crd::u32 failed_at = 99U;
    for (crd::u32 i = 0U; i < 5U; ++i)
    {
        recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"before"}, i, 0U);
        if (faulty.try_allocate(32U, 16U) == nullptr)
        {
            failed_at = i;
            break;
        }
    }
    CHECK(failed_at == 2U);
    recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, 100U, 0U);
    recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, 101U, 0U); // fills the post window

    cp::FlightCapture c;
    REQUIRE(recorder.take_capture(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::InjectedFault);
    CHECK(c.trigger.code == cp::kFlightCodeFaultInjected);
    CHECK(c.trigger.value_a == static_cast<crd::u64>(cp::FaultSite::AllocationFailure));
    CHECK(c.trigger.value_b == 2U);
    REQUIRE(c.replay_available);
    const cont::StringView recipe{c.replay.data(), c.replay.size()};
    CHECK(recipe.find("fault-plan/1 fired=allocation-failure#2 minimal=seed=7;allocation-failure@2+1%1000000") == 0U);
    CHECK(recipe.find(" plan=seed=7;allocation-failure@2+1%1000000") != cont::StringView::npos);
    // The fault itself is the last event before the trigger, after the three "before" events and two allocations.
    REQUIRE(c.pre_retained >= 1U);
    const cp::FlightEvent& injected = c.events[c.pre_retained - 1U];
    CHECK(injected.code == cp::kFlightCodeFaultInjected);
    CHECK(injected.value_b == 2U);
    CHECK(c.events[0].code == 0x0042U);

    cont::String last;
    injector.last_recipe(last);
    CHECK(last == c.replay);

    // The recipe's one-hit plan reproduces the same failure on a fresh run.
    const cont::String minimal = minimal_of(c.replay);
    cp::FaultInjector  again;
    REQUIRE(again.arm(plan_of(cont::StringView{minimal.data(), minimal.size()}), kInjectRead) ==
            cp::FaultArmStatus::Armed);
    cp::FaultAllocator replayed(&linear, &again);
    crd::u32           replay_failed_at = 99U;
    for (crd::u32 i = 0U; i < 5U; ++i)
    {
        if (replayed.try_allocate(32U, 16U) == nullptr)
        {
            replay_failed_at = i;
            break;
        }
    }
    CHECK(replay_failed_at == 2U);

    // While armed, the plan owns the recorder's recipe even before anything fires; disarming leaves no recipe, since
    // neither the plan's nor a fault's reproduces a later failure.
    const auto manual_capture = [&recorder](cp::FlightCapture& out)
    {
        cp::FlightTriggerInput input;
        input.kind    = cp::FlightTrigger::Manual;
        input.code    = cp::kFlightCodeManual;
        input.message = cont::StringView{"manual"};
        REQUIRE(recorder.trigger(input) == cp::FlightTriggerResult::Started);
        recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, 0U, 0U);
        recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, 1U, 0U);
        REQUIRE(recorder.take_capture(out));
    };
    REQUIRE(injector.arm(plan_of("seed=8;allocation-failure@50+1%1000000"), kInjectRead) == cp::FaultArmStatus::Armed);
    cp::FlightCapture armed_capture;
    manual_capture(armed_capture);
    CHECK(armed_capture.replay_available);
    CHECK(armed_capture.replay == "fault-plan/1 plan=seed=8;allocation-failure@50+1%1000000");
    injector.disarm();
    cp::FlightCapture disarmed_capture;
    manual_capture(disarmed_capture);
    CHECK_FALSE(disarmed_capture.replay_available);
    CHECK(cont::StringView{disarmed_capture.replay.data(), disarmed_capture.replay.size()}.find("disarmed") !=
          cont::StringView::npos);
    recorder.shutdown();
}

TEST_CASE("fault injector: the I/O hook counts each read at every eligible site and applies the first that fires",
          "[perf][diag][diag9b][fault]")
{
    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK_FALSE(crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &injector));
        CHECK(crd::consult_io_fault(crd::IoFaultOp::AsyncRead, "x", 1U, 0U, 8U).action ==
              crd::IoFaultAction::Proceed);
        return;
    }

    const auto ask = [&injector](crd::IoFaultOp op)
    {
        return cp::FaultInjector::io_hook(&injector, op, "pack.crdr", 9U, 0U, 64U);
    };
    CHECK(ask(crd::IoFaultOp::ReadRange).action == crd::IoFaultAction::Proceed); // disarmed: nothing counted
    CHECK(injector.hits(cp::FaultSite::FailedRead) == 0U);

    REQUIRE(injector.arm(plan_of("seed=1;failed-read@1+1%1000000;partial-read@1+2%1000000=5;"
                                 "cancelled-completion@0+1%1000000;delayed-completion@1+1%1000000=2000000"),
                         kInjectRead) == cp::FaultArmStatus::Armed);
    // Read 0 (sync): nothing covers it; completion sites are not eligible.
    CHECK(ask(crd::IoFaultOp::ReadWhole).action == crd::IoFaultAction::Proceed);
    CHECK(injector.hits(cp::FaultSite::CancelledCompletion) == 0U);
    // Read 1 (async): failed-read, partial-read and cancelled-completion (its hit 0) all fire; failed-read applies.
    CHECK(ask(crd::IoFaultOp::AsyncRead).action == crd::IoFaultAction::Fail);
    // Read 2 (async): partial-read's second covered hit applies with its value; delayed-completion (hit 1) is shadowed.
    const crd::IoFault partial = ask(crd::IoFaultOp::AsyncRead);
    CHECK(partial.action == crd::IoFaultAction::Short);
    CHECK(partial.value == 5U);
    // Read 3 (async): nothing left.
    CHECK(ask(crd::IoFaultOp::AsyncRead).action == crd::IoFaultAction::Proceed);

    CHECK(injector.hits(cp::FaultSite::FailedRead) == 4U);
    CHECK(injector.hits(cp::FaultSite::PartialRead) == 4U);
    CHECK(injector.hits(cp::FaultSite::CancelledCompletion) == 3U);
    CHECK(injector.hits(cp::FaultSite::DelayedCompletion) == 3U);
    CHECK(injector.fired(cp::FaultSite::FailedRead) == 1U);
    CHECK(injector.fired(cp::FaultSite::PartialRead) == 1U);
    CHECK(injector.fired(cp::FaultSite::CancelledCompletion) == 0U);
    CHECK(injector.fired(cp::FaultSite::DelayedCompletion) == 0U);

    REQUIRE(injector.arm(plan_of("seed=1;cancelled-completion@0+1%1000000;delayed-completion@1+1%1000000=2000000"),
                         kInjectRead) == cp::FaultArmStatus::Armed);
    CHECK(ask(crd::IoFaultOp::AsyncRead).action == crd::IoFaultAction::Cancel);
    const crd::IoFault delayed = ask(crd::IoFaultOp::AsyncRead);
    CHECK(delayed.action == crd::IoFaultAction::Delay);
    CHECK(delayed.value == 2000000U);

    // flipped-read (HW-RESILIENCE.1) counts every read, ranks after partial-read and before the completion sites, and
    // carries its bit as the value.
    REQUIRE(injector.arm(plan_of("seed=1;partial-read@0+1%1000000=5;flipped-read@0+2%1000000=77;"
                                 "cancelled-completion@0+1%1000000"),
                         kInjectRead) == cp::FaultArmStatus::Armed);
    CHECK(ask(crd::IoFaultOp::AsyncRead).action == crd::IoFaultAction::Short); // read 0: partial-read applies
    const crd::IoFault flipped = ask(crd::IoFaultOp::ReadRange);               // read 1: flipped-read's second hit
    CHECK(flipped.action == crd::IoFaultAction::Flip);
    CHECK(flipped.value == 77U);
    CHECK(ask(crd::IoFaultOp::ReadWhole).action == crd::IoFaultAction::Proceed);
    CHECK(injector.hits(cp::FaultSite::FlippedRead) == 3U);
    CHECK(injector.fired(cp::FaultSite::FlippedRead) == 1U);

    // Installed on the core seam, a consult reaches the injector; one hook at a time; uninstall restores Proceed.
    REQUIRE(injector.arm(plan_of("seed=1;failed-read@0+1%1000000"), kInjectRead) == cp::FaultArmStatus::Armed);
    REQUIRE(crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &injector));
    CHECK_FALSE(crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &injector));
    CHECK(crd::consult_io_fault(crd::IoFaultOp::ReadRange, "a", 1U, 0U, 8U).action == crd::IoFaultAction::Fail);
    crd::uninstall_io_fault_hook();
    REQUIRE(injector.arm(plan_of("seed=1;failed-read@0+1%1000000"), kInjectRead) == cp::FaultArmStatus::Armed);
    CHECK(crd::consult_io_fault(crd::IoFaultOp::ReadRange, "a", 1U, 0U, 8U).action == crd::IoFaultAction::Proceed);
    CHECK(injector.hits(cp::FaultSite::FailedRead) == 0U);

    // A recipe longer than the recorder's bound is clipped visibly ("..."), after the fault and its one-hit plan.
    cont::String long_plan("seed=1;failed-read@0+1%1000000");
    for (crd::u32 i = 0U; i < 7U; ++i)
    {
        long_plan.append(";partial-read@9223372036854775807+9223372036854775807%1=18446744073709551615");
    }
    REQUIRE(injector.arm(plan_of(cont::StringView{long_plan.data(), long_plan.size()}), kInjectRead) ==
            cp::FaultArmStatus::Armed);
    CHECK(ask(crd::IoFaultOp::ReadRange).action == crd::IoFaultAction::Fail);
    cont::String recipe;
    injector.last_recipe(recipe);
    const cont::StringView view{recipe.data(), recipe.size()};
    CHECK(view.size() == cp::kFaultRecipeBytes - 1U);
    CHECK(view.find("fault-plan/1 fired=failed-read#0 minimal=seed=1;failed-read@0+1%1000000 plan=seed=1;") == 0U);
    CHECK(view.substr(view.size() - 3U) == "...");
}

// A fired read names the file it failed in its flight event (the causal input), by its last path component only, so
// a host's directories never reach the evidence; a name longer than the event's bound is clipped and flagged.
TEST_CASE("fault injector: a fired read's event names its file, never its directory", "[perf][diag][diag9b][fault]")
{
    if (!crd::fault_seams_compiled())
    {
        cp::FaultInjector injector;
        CHECK(injector.arm(plan_of("seed=3;failed-read@0+3%1000000"), kInjectRead) == cp::FaultArmStatus::Unavailable);
        return;
    }

    cp::FlightRecorder recorder;
    REQUIRE(recorder.init(small_policy()) == cp::FlightPolicyStatus::Committed);
    cp::FaultInjector injector;
    injector.attach_flight_recorder(&recorder);
    REQUIRE(injector.arm(plan_of("seed=3;failed-read@0+3%1000000"), kInjectRead) == cp::FaultArmStatus::Armed);

    const char forward[] = "D:/private-root/assets/hero.bin.meta";
    const char back[]    = R"(C:\private-root\cache\0123.crdr)";
    cont::String long_path("/private-root/");
    for (crd::u32 i = 0U; i < 150U; ++i)
    {
        long_path.push_back(static_cast<char>('a' + (i % 26U)));
    }
    CHECK(cp::FaultInjector::io_hook(&injector, crd::IoFaultOp::ReadWhole, forward, sizeof(forward) - 1U, 0U, 0U)
              .action == crd::IoFaultAction::Fail);
    CHECK(cp::FaultInjector::io_hook(&injector, crd::IoFaultOp::ReadRange, back, sizeof(back) - 1U, 0U, 8U).action ==
          crd::IoFaultAction::Fail);
    CHECK(cp::FaultInjector::io_hook(&injector, crd::IoFaultOp::ReadWhole, long_path.data(), long_path.size(), 0U, 0U)
              .action == crd::IoFaultAction::Fail);

    cp::FlightCapture capture;
    REQUIRE(recorder.take_capture(capture)); // the third fault's event closed the two-event post window
    cont::Array<const cp::FlightEvent*> faults;
    for (const cp::FlightEvent& e : capture.events)
    {
        if (e.code == cp::kFlightCodeFaultInjected)
        {
            faults.push_back(&e);
        }
    }
    REQUIRE(faults.size() == 3U);
    CHECK(faults[0]->message_view() == "injected failed-read at hit 0 (seed 3) on hero.bin.meta");
    CHECK(faults[1]->message_view() == "injected failed-read at hit 1 (seed 3) on 0123.crdr");
    CHECK_FALSE(faults[0]->message_truncated);
    CHECK_FALSE(faults[1]->message_truncated);
    CHECK(faults[2]->message_truncated);
    CHECK(faults[2]->message_view().find("injected failed-read at hit 2 (seed 3) on abcdefghij") == 0U);
    for (const cp::FlightEvent* e : faults)
    {
        CHECK(e->message_view().find("private-root") == cont::StringView::npos);
        CHECK(e->value_a == static_cast<crd::u64>(cp::FaultSite::FailedRead));
    }
    injector.disarm();
    recorder.shutdown();
}

// The GPU seam's hook: each consulted submission is one device-loss hit, and a firing hit answers LoseDevice with the
// same evidence as any other site. The providers' own loss paths are driven end to end in crd-gpu-context-vulkan-tests
// and crd-gpu-context-dx12-tests ("DIAG.9b: an injected device loss ...").
TEST_CASE("fault injector: the GPU hook loses the device at the plan's submission", "[perf][diag][diag9b][fault]")
{
    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=3;device-loss@1+1%1000000"), kInjectRead) == cp::FaultArmStatus::Unavailable);
        CHECK_FALSE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
        CHECK(crd::consult_gpu_fault(crd::GpuFaultOp::Submit, "submit") == crd::GpuFaultAction::Proceed);
        return;
    }

    // The site is named, parsed and takes no value.
    cp::FaultSite site = cp::FaultSite::AllocationFailure;
    REQUIRE(cp::parse_fault_site(cont::StringView{"device-loss"}, site));
    CHECK(site == cp::FaultSite::DeviceLoss);
    CHECK(cp::fault_site_name(cp::FaultSite::DeviceLoss) == "device-loss");
    cp::FaultPlan  valued;
    cont::String   reason;
    CHECK_FALSE(cp::parse_fault_plan(cont::StringView{"seed=3;device-loss@0+1%1000000=4"}, valued, reason));

    cp::FlightRecorder recorder;
    REQUIRE(recorder.init(small_policy()) == cp::FlightPolicyStatus::Committed);
    injector.attach_flight_recorder(&recorder);

    // Disarmed, the hook answers Proceed and counts nothing.
    CHECK(cp::FaultInjector::gpu_hook(&injector, crd::GpuFaultOp::Submit, "submit") == crd::GpuFaultAction::Proceed);
    CHECK(injector.hits(cp::FaultSite::DeviceLoss) == 0U);

    REQUIRE(injector.arm(plan_of("seed=3;device-loss@1+1%1000000"), kInjectRead) == cp::FaultArmStatus::Armed);
    REQUIRE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
    CHECK_FALSE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
    crd::GpuFaultAction answers[3] = {};
    for (crd::u32 i = 0U; i < 3U; ++i)
    {
        recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"frame"}, i, 0U);
        answers[i] = crd::consult_gpu_fault(crd::GpuFaultOp::Submit, "compute submit");
    }
    CHECK(answers[0] == crd::GpuFaultAction::Proceed);
    CHECK(answers[1] == crd::GpuFaultAction::LoseDevice);
    CHECK(answers[2] == crd::GpuFaultAction::Proceed);
    CHECK(injector.hits(cp::FaultSite::DeviceLoss) == 3U);
    CHECK(injector.fired(cp::FaultSite::DeviceLoss) == 1U);
    CHECK(injector.hits(cp::FaultSite::FailedRead) == 0U); // a submission is not a read
    recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, 100U, 0U); // fills the post window

    cp::FlightCapture c;
    REQUIRE(recorder.take_capture(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::InjectedFault);
    CHECK(c.trigger.value_a == static_cast<crd::u64>(cp::FaultSite::DeviceLoss));
    CHECK(c.trigger.value_b == 1U);
    REQUIRE(c.replay_available);
    const cont::StringView recipe{c.replay.data(), c.replay.size()};
    CHECK(recipe.find("fault-plan/1 fired=device-loss#1 minimal=seed=3;device-loss@1+1%1000000") == 0U);
    // Before the trigger: both frames that preceded the lost submission, then the fault itself.
    REQUIRE(c.pre_retained == 3U);
    CHECK(c.events[0].value_a == 0U);
    CHECK(c.events[1].value_a == 1U);
    CHECK(c.events[2].code == cp::kFlightCodeFaultInjected);

    // Uninstalled, a consult no longer reaches the injector.
    crd::uninstall_gpu_fault_hook();
    CHECK(crd::consult_gpu_fault(crd::GpuFaultOp::Submit, "compute submit") == crd::GpuFaultAction::Proceed);
    CHECK(injector.hits(cp::FaultSite::DeviceLoss) == 3U);
    injector.disarm();
    recorder.shutdown();
}

TEST_CASE("fault injector: concurrent hits fire exactly the plan's set of ordinals", "[perf][diag][diag9b][fault]")
{
    cp::FaultInjector   injector;
    const cp::FaultPlan plan = plan_of("seed=77;allocation-failure@0+0%200000");
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan, kInjectRead) == cp::FaultArmStatus::Unavailable);
        return;
    }
    REQUIRE(injector.arm(plan, kInjectRead) == cp::FaultArmStatus::Armed);

    constexpr crd::u32    thread_count     = 4U;
    constexpr crd::u32    calls_per_thread = 1000U;
    std::atomic<crd::u64> fired_hit_sum{0U};
    std::atomic<crd::u32> fired_count{0U};
    std::thread           threads[thread_count];
    for (std::thread& t : threads)
    {
        t = std::thread(
            [&]
            {
                for (crd::u32 i = 0U; i < calls_per_thread; ++i)
                {
                    const cp::FaultDecision d = injector.decide(cp::FaultSite::AllocationFailure);
                    if (d.fire)
                    {
                        fired_hit_sum.fetch_add(d.hit, std::memory_order_relaxed);
                        fired_count.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
            });
    }
    for (std::thread& t : threads)
    {
        t.join();
    }

    crd::u64 expected_sum   = 0U;
    crd::u32 expected_count = 0U;
    for (crd::u64 hit = 0U; hit < static_cast<crd::u64>(thread_count) * calls_per_thread; ++hit)
    {
        if (cp::fault_plan_fires(plan, cp::FaultSite::AllocationFailure, hit))
        {
            expected_sum += hit;
            ++expected_count;
        }
    }
    CHECK(injector.hits(cp::FaultSite::AllocationFailure) ==
          static_cast<crd::u64>(thread_count) * calls_per_thread);
    CHECK(fired_count.load() == expected_count);
    CHECK(fired_hit_sum.load() == expected_sum);
    CHECK(injector.fired(cp::FaultSite::AllocationFailure) == expected_count);
}
