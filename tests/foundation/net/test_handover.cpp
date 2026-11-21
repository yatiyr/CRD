// crd-net -- scheduled link handovers (NET-PATH; docs/design/network-foundation.md#handover): which schedules are
// valid (none, the period's bounds, a phase below it, the guards' share), which send times a schedule covers (both
// guards' ends included, the instant past each excluded, every period, on a clock near its wrap), and Starlink's
// timetable built from a reading of the connection's clock and UTC: seconds 12, 27, 42 and 57 of each minute, the
// guards and a sender's lead.

#include <crd/net/handover.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kSecond = 1000U * kMs;

HandoverSchedule schedule(u64 period, u64 phase, u64 before, u64 after)
{
    HandoverSchedule out;
    out.period_ns = period;
    out.phase_ns = phase;
    out.before_ns = before;
    out.after_ns = after;
    return out;
}

// 2026-11-21T00:00:00Z, in POSIX nanoseconds: a minute's start.
constexpr u64 kUtcMinute = u64{1795219200} * kSecond;
static_assert(kUtcMinute % (60U * kSecond) == 0U);
} // namespace

TEST_CASE("handover schedules: none, the period's bounds, a phase below it and guards within an eighth of it",
          "[net][net-path][handover]")
{
    CHECK(handover_schedule_valid(HandoverSchedule{}));
    CHECK_FALSE(handover_schedule_valid(schedule(0U, 1U, 0U, 0U))); // none names nothing else
    CHECK_FALSE(handover_schedule_valid(schedule(0U, 0U, 1U, 0U)));
    CHECK_FALSE(handover_schedule_valid(schedule(0U, 0U, 0U, 1U)));
    CHECK(handover_schedule_valid(schedule(kHandoverMinPeriodNs, 0U, 0U, 0U)));
    CHECK_FALSE(handover_schedule_valid(schedule(kHandoverMinPeriodNs - 1U, 0U, 0U, 0U)));
    CHECK(handover_schedule_valid(schedule(kHandoverMaxPeriodNs, kHandoverMaxPeriodNs - 1U, 0U, 0U)));
    CHECK_FALSE(handover_schedule_valid(schedule(kHandoverMaxPeriodNs + 1U, 0U, 0U, 0U)));
    CHECK_FALSE(handover_schedule_valid(schedule(15U * kSecond, 15U * kSecond, 0U, 0U)));
    // 15 s: the guards may cover 1875 ms together, split as they like.
    const u64 share = 15U * kSecond / kHandoverMaxShare;
    CHECK(handover_schedule_valid(schedule(15U * kSecond, 0U, share, 0U)));
    CHECK(handover_schedule_valid(schedule(15U * kSecond, 0U, 0U, share)));
    CHECK(handover_schedule_valid(schedule(15U * kSecond, 0U, share - kMs, kMs)));
    CHECK_FALSE(handover_schedule_valid(schedule(15U * kSecond, 0U, share, 1U)));
    CHECK_FALSE(handover_schedule_valid(schedule(15U * kSecond, 0U, share + 1U, 0U)));
    CHECK_FALSE(handover_schedule_valid(schedule(15U * kSecond, 0U, ~u64{0}, 1U))); // no wrap past the share
    CHECK(handover_schedule_valid(starlink_handover_schedule(0U, 0U)));
}

TEST_CASE("handover schedules: a send time is covered from the guard before an event to the guard after it, every "
          "period, on a clock near its wrap",
          "[net][net-path][handover]")
{
    CHECK_FALSE(handover_covers(HandoverSchedule{}, 0U));
    CHECK_FALSE(handover_covers(HandoverSchedule{}, 12U * kSecond));

    const HandoverSchedule s = schedule(15U * kSecond, 12U * kSecond, 100U * kMs, 50U * kMs);
    REQUIRE(handover_schedule_valid(s));
    for (u64 k = 0U; k < 4U; ++k)
    {
        const u64 event = 12U * kSecond + k * 15U * kSecond;
        CHECK(handover_covers(s, event));
        CHECK(handover_covers(s, event - 100U * kMs));
        CHECK_FALSE(handover_covers(s, event - 100U * kMs - 1U));
        CHECK(handover_covers(s, event + 50U * kMs));
        CHECK_FALSE(handover_covers(s, event + 50U * kMs + 1U));
        CHECK_FALSE(handover_covers(s, event + 7U * kSecond));
    }
    CHECK_FALSE(handover_covers(s, 0U));
    // Phase 0: the guard before the event at 0 wraps into the period before it.
    const HandoverSchedule zero = schedule(kSecond, 0U, 10U * kMs, 10U * kMs);
    CHECK(handover_covers(zero, 0U));
    CHECK(handover_covers(zero, 990U * kMs));
    CHECK_FALSE(handover_covers(zero, 989U * kMs));
    CHECK(handover_covers(zero, 10U * kMs));
    CHECK_FALSE(handover_covers(zero, 11U * kMs));
    // No guards: the instant alone.
    const HandoverSchedule instant = schedule(kSecond, 5U, 0U, 0U);
    CHECK(handover_covers(instant, 5U));
    CHECK(handover_covers(instant, kSecond + 5U));
    CHECK_FALSE(handover_covers(instant, 4U));
    CHECK_FALSE(handover_covers(instant, 6U));
    // Far along the clock: the remainder carries the phase, nothing overflows.
    const HandoverSchedule hour = schedule(kHandoverMaxPeriodNs, kHandoverMaxPeriodNs - 1U, kSecond, kSecond);
    const u64 late = ~u64{0} - ~u64{0} % kHandoverMaxPeriodNs - 1U; // an event's time near the wrap
    REQUIRE(late % kHandoverMaxPeriodNs == hour.phase_ns);
    CHECK(handover_covers(hour, late));
    CHECK(handover_covers(hour, late - kSecond));
    CHECK_FALSE(handover_covers(hour, late - kSecond - 1U));
    CHECK(handover_covers(hour, late + kSecond));
    CHECK_FALSE(handover_covers(hour, late + kSecond + 1U));
}

TEST_CASE("handover schedules: Starlink's timetable falls on seconds 12, 27, 42 and 57 of each UTC minute on any "
          "clock, earlier by a sender's lead",
          "[net][net-path][handover]")
{
    // The connection's clock reads `local` when UTC reads `utc`: a clock started at boot, an arbitrary offset.
    const u64 local = 987654321987U;
    const u64 utc = kUtcMinute + 33U * kSecond + 250U * kMs;
    const HandoverSchedule s = starlink_handover_schedule(local, utc);
    REQUIRE(handover_schedule_valid(s));
    CHECK(s.period_ns == kStarlinkHandoverPeriodNs);
    CHECK(s.before_ns == kStarlinkHandoverGuardNs);
    CHECK(s.after_ns == kStarlinkHandoverGuardNs);
    // The local time of UTC `at` past the minute.
    const auto here = [&](u64 at)
    {
        return local + (kUtcMinute + at) - utc;
    };
    for (const u64 second : {12U, 27U, 42U, 57U, 72U})
    {
        CHECK(handover_covers(s, here(second * kSecond)));
        CHECK(handover_covers(s, here(second * kSecond - 100U * kMs)));
        CHECK_FALSE(handover_covers(s, here(second * kSecond - 100U * kMs - 1U)));
        CHECK(handover_covers(s, here(second * kSecond + 100U * kMs)));
        CHECK_FALSE(handover_covers(s, here(second * kSecond + 100U * kMs + 1U)));
    }
    CHECK_FALSE(handover_covers(s, here(13U * kSecond)));
    CHECK_FALSE(handover_covers(s, here(20U * kSecond)));
    CHECK_FALSE(handover_covers(s, here(0U)));
    // A reading taken at an event, or on a clock behind UTC, gives the same timetable.
    CHECK(starlink_handover_schedule(here(27U * kSecond), kUtcMinute + 27U * kSecond) == s);
    CHECK(starlink_handover_schedule(local - 5U * kSecond, utc - 5U * kSecond) == s);

    // A server 30 ms from the terminal covers what it sends 30 ms before each event; asymmetric guards.
    const HandoverSchedule lead = starlink_handover_schedule(local, utc, 30U * kMs, 150U * kMs, 50U * kMs);
    REQUIRE(handover_schedule_valid(lead));
    CHECK(handover_covers(lead, here(42U * kSecond - 30U * kMs)));
    CHECK(handover_covers(lead, here(42U * kSecond - 180U * kMs)));
    CHECK_FALSE(handover_covers(lead, here(42U * kSecond - 181U * kMs)));
    CHECK(handover_covers(lead, here(42U * kSecond + 20U * kMs)));
    CHECK_FALSE(handover_covers(lead, here(42U * kSecond + 21U * kMs)));
    // A lead of whole periods is none.
    CHECK(starlink_handover_schedule(local, utc, 2U * kStarlinkHandoverPeriodNs) == s);
}
