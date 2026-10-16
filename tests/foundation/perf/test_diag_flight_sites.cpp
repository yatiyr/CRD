// DIAG.11b -- the cost contract of a flight-recorder event site (crd/perf/flight_recorder.hpp).
//
// The modes contract (docs/design/runtime-diagnostics.md, "Modes, budgets and evidence of cost") says a disabled event
// site must not evaluate an expensive payload or change what the program does, and that hot recording does no
// blocking. These cases hold the two structural halves of that, which no timing can show:
//
// - side-effect controls: CRD_FLIGHT_RECORD's message and scalars are counted as they are evaluated. With no recorder,
//   with a recorder that is not initialised and at a detail that leaves the event out, the count stays zero and the
//   program's own state is unchanged; where the recorder keeps the event, each is evaluated exactly once, and the
//   recorder expression is evaluated once in every case. A detail that leaves the event out still counts it as
//   filtered, exactly as a direct record() does.
// - no waiting: an event the active detail leaves out is decided before the ring mutex. While another thread holds the
//   ring (the test seam), a Basic recorder's informational record() and admit() return; a Warning, which the recorder
//   keeps, cannot complete until the ring is released (the positive control that the ring really is held).
//
// The measured cost of each mode is the overhead board's (scripts/diag-overhead.py, docs/bench); these cases only
// state what must hold on every lane. Contract: docs/design/runtime-diagnostics.md (DIAG.11b); ADR-0133.

#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/containers/string_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
using crd::u32;
using crd::u64;

constexpr cp::EventCode kSiteCode = 0x0042U;

struct SiteCounts
{
    u32 recorder = 0U; // evaluations of the recorder expression
    u32 message  = 0U; // evaluations of the message expression
    u32 scalars  = 0U; // evaluations of value_a and value_b
};

cp::FlightRecorder* recorder_of(cp::FlightRecorder* r, SiteCounts& n)
{
    ++n.recorder;
    return r;
}

cont::StringView message_of(SiteCounts& n)
{
    ++n.message;
    return cont::StringView{"payload"};
}

u64 scalar_of(SiteCounts& n, u64 v)
{
    ++n.scalars;
    return v;
}

// One site with every argument counted.
void site(cp::FlightRecorder* r, cp::Severity severity, SiteCounts& n)
{
    CRD_FLIGHT_RECORD(recorder_of(r, n), kSiteCode, severity, message_of(n), scalar_of(n, 7U), scalar_of(n, 9U));
}

cp::FlightPolicy policy(cp::FlightDetail detail)
{
    cp::FlightPolicy p;
    p.capacity       = 64U;
    p.pre_events     = 32U;
    p.post_events    = 4U;
    p.post_window_ns = 0U;
    p.detail         = detail;
    return p;
}
} // namespace

TEST_CASE("diag 11b sites: a disabled flight site evaluates no payload and changes nothing",
          "[perf][diag][diag11b][sites]")
{
    // No recorder at all: the off mode.
    SiteCounts none;
    site(nullptr, cp::Severity::Info, none);
    site(nullptr, cp::Severity::FatalInvariant, none);
    CHECK(none.recorder == 2U);
    CHECK(none.message == 0U);
    CHECK(none.scalars == 0U);

    // A recorder that is not initialised (before init, after shutdown): nothing evaluated, nothing counted.
    cp::FlightRecorder idle;
    SiteCounts         before_init;
    site(&idle, cp::Severity::Warning, before_init);
    CHECK(before_init.recorder == 1U);
    CHECK(before_init.message == 0U);
    CHECK(before_init.scalars == 0U);
    CHECK(idle.total_recorded() == 0U);
    CHECK(idle.total_filtered() == 0U);
    CHECK_FALSE(idle.admit(cp::Severity::FatalInvariant));

    // Basic leaves informational events out: not evaluated, not recorded, counted as filtered.
    cp::FlightRecorder basic;
    REQUIRE(basic.init(policy(cp::FlightDetail::Basic)) == cp::FlightPolicyStatus::Committed);
    SiteCounts info;
    for (u32 i = 0U; i < 5U; ++i)
    {
        site(&basic, cp::Severity::Info, info);
    }
    CHECK(info.recorder == 5U);
    CHECK(info.message == 0U);
    CHECK(info.scalars == 0U);
    CHECK(basic.total_recorded() == 0U);
    CHECK(basic.total_filtered() == 5U);

    // ... and keeps a warning, whose payload is evaluated exactly once.
    SiteCounts warning;
    site(&basic, cp::Severity::Warning, warning);
    CHECK(warning.recorder == 1U);
    CHECK(warning.message == 1U);
    CHECK(warning.scalars == 2U);
    CHECK(basic.total_recorded() == 1U);
    CHECK(basic.total_filtered() == 5U);

    // A direct record() of an informational event is filtered and counted the same way.
    basic.record(kSiteCode, cp::Severity::Info, cont::StringView{"direct"}, 1U, 2U);
    CHECK(basic.total_recorded() == 1U);
    CHECK(basic.total_filtered() == 6U);

    // Full keeps every event: each argument evaluated once per site.
    cp::FlightRecorder full;
    REQUIRE(full.init(policy(cp::FlightDetail::Full)) == cp::FlightPolicyStatus::Committed);
    SiteCounts kept;
    site(&full, cp::Severity::Info, kept);
    site(&full, cp::Severity::Info, kept);
    CHECK(kept.recorder == 2U);
    CHECK(kept.message == 2U);
    CHECK(kept.scalars == 4U);
    CHECK(full.total_recorded() == 2U);
    CHECK(full.total_filtered() == 0U);

    // A reload to Basic changes what the site admits from the next event on.
    REQUIRE(full.reload_policy(policy(cp::FlightDetail::Basic)) == cp::FlightPolicyStatus::Committed);
    SiteCounts reloaded;
    site(&full, cp::Severity::Info, reloaded);
    CHECK(reloaded.message == 0U);
    CHECK(full.total_filtered() == 1U);

    // After shutdown the site is off again.
    full.shutdown();
    SiteCounts after;
    site(&full, cp::Severity::RecoverableError, after);
    CHECK(after.recorder == 1U);
    CHECK(after.message == 0U);
    CHECK(after.scalars == 0U);
}

#if CRD_ENABLE_ASSERTS
namespace
{
constexpr auto kSettle = std::chrono::milliseconds(50);
constexpr auto kBound  = std::chrono::seconds(10);

struct HeldRing
{
    cp::FlightRecorder* recorder          = nullptr;
    bool                filtered_returned = false; // the informational record() and admit() returned while held
    std::atomic<bool>   admitted{true};            // what admit(Info) answered
    bool                warning_waited = false;    // the warning had not completed while the ring was held
    std::atomic<bool>   filtered_done{false};
    std::atomic<bool>   warning_done{false};
    std::thread         filtered;
    std::thread         warning;
};

bool wait_for(const std::atomic<bool>& flag)
{
    const auto until = std::chrono::steady_clock::now() + kBound;
    while (!flag.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() > until)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void record_filtered(HeldRing* held)
{
    held->recorder->record(kSiteCode, cp::Severity::Info, cont::StringView{"info"}, 0U, 0U);
    held->admitted.store(held->recorder->admit(cp::Severity::Info), std::memory_order_relaxed);
    held->filtered_done.store(true, std::memory_order_release);
}

void record_warning(HeldRing* held)
{
    held->recorder->record(kSiteCode, cp::Severity::Warning, cont::StringView{"warning"}, 0U, 0U);
    held->warning_done.store(true, std::memory_order_release);
}

// Runs while this thread holds the recorder's ring mutex. Both threads are joined after it returns, so a record() that
// wrongly waits on the ring fails the case instead of deadlocking it.
void while_ring_held(void* user)
{
    auto* const held        = static_cast<HeldRing*>(user);
    held->filtered          = std::thread(&record_filtered, held);
    held->filtered_returned = wait_for(held->filtered_done);

    // The positive control: an event the recorder keeps takes the ring, so it cannot complete while the ring is held.
    held->warning = std::thread(&record_warning, held);
    std::this_thread::sleep_for(kSettle);
    held->warning_waited = !held->warning_done.load(std::memory_order_acquire);
}
} // namespace

TEST_CASE("diag 11b sites: an event the detail leaves out never waits on the ring", "[perf][diag][diag11b][sites]")
{
    cp::FlightRecorder basic;
    REQUIRE(basic.init(policy(cp::FlightDetail::Basic)) == cp::FlightPolicyStatus::Committed);
    HeldRing held;
    held.recorder = &basic;
    REQUIRE(basic.test_with_ring_held(&while_ring_held, &held));
    held.filtered.join(); // the ring is free again, so both complete
    held.warning.join();
    CHECK(held.filtered_returned);
    CHECK(held.filtered_done.load(std::memory_order_acquire));
    CHECK_FALSE(held.admitted.load(std::memory_order_relaxed));
    CHECK(held.warning_waited);
    CHECK(held.warning_done.load(std::memory_order_acquire));
    CHECK(basic.total_filtered() == 2U);
    CHECK(basic.total_recorded() == 1U);
}
#endif
