#pragma once

// crd-net -- scheduled link handovers (NET-PATH; contract: docs/design/network-foundation.md#handover).
// Source: V. Kamel, J. Zhao, D. Li, J. Pan, "StarQUIC: Tuning Congestion Control Algorithms for QUIC over LEO
// Satellite Networks", LEONET 2024 (doi 10.1145/3636534.3696227), read in full 2026-11-21.
//
//   schedule   some links hand over on a timetable: a Starlink user terminal moves to another satellite every 15 s,
//              at seconds 12, 27, 42 and 57 of each UTC minute, and loses packets and gains delay for about 100 ms
//              each time. A HandoverSchedule names those events on the connection's own clock: one every period_ns,
//              one at phase_ns modulo the period, and a packet sent from before_ns ahead of an event to after_ns past
//              it may cross it (covered).
//   use        QuicConnection (quic_connection.hpp) holds a covered packet's loss and delay from its congestion
//              controller, its persistent congestion and its path MTU search, while loss detection, retransmission
//              and the RTT estimate behind the timers stay as they are; the window may still grow. A schedule is
//              local, authored configuration: a peer never sets one.
//   bounds     the guards of one event cover at most an eighth of its period, so a schedule can never hide more than
//              that share of a path's losses: congestion outside the guards is answered as on any path.
//   time       every call takes the time in nanoseconds; nothing reads a clock. starlink_handover_schedule() takes a
//              reading of the connection's clock and of UTC made together (the terminal's own NTP server gives one).

#include <crd/core/types.hpp>

namespace crd::net
{
inline constexpr u64 kHandoverMinPeriodNs = u64{1000000000};    // 1 s
inline constexpr u64 kHandoverMaxPeriodNs = u64{3600000000000}; // an hour
inline constexpr u64 kHandoverMaxShare = 8U;                    // the guards cover at most period / 8

// Starlink's timetable (the paper's section 3.2): every 15 s at second 12 of the period, so 12, 27, 42 and 57 of
// each UTC minute, with the guards it measured best (100 ms each side, its Figure 2).
inline constexpr u64 kStarlinkHandoverPeriodNs = u64{15000000000};
inline constexpr u64 kStarlinkHandoverSecondNs = u64{12000000000};
inline constexpr u64 kStarlinkHandoverGuardNs = u64{100000000};

// Events every period_ns at phase_ns on the connection's clock (below the period); a period of 0 is none, with every
// field 0.
struct HandoverSchedule
{
    u64 period_ns = 0U;
    u64 phase_ns = 0U;
    u64 before_ns = 0U;
    u64 after_ns = 0U;

    [[nodiscard]] constexpr bool operator==(const HandoverSchedule&) const noexcept = default;
};

// None, or a period from kHandoverMinPeriodNs to kHandoverMaxPeriodNs, a phase below it, and guards together at most
// an eighth of it.
[[nodiscard]] constexpr bool handover_schedule_valid(const HandoverSchedule& schedule) noexcept
{
    if (schedule.period_ns == 0U)
    {
        return schedule.phase_ns == 0U && schedule.before_ns == 0U && schedule.after_ns == 0U;
    }
    const u64 share = schedule.period_ns / kHandoverMaxShare;
    return schedule.period_ns >= kHandoverMinPeriodNs && schedule.period_ns <= kHandoverMaxPeriodNs &&
           schedule.phase_ns < schedule.period_ns && schedule.before_ns <= share &&
           schedule.after_ns <= share - schedule.before_ns;
}

// Whether a packet sent at `now_ns` may cross an event of `schedule` (valid): from before_ns ahead of one to after_ns
// past it, both ends included.
[[nodiscard]] constexpr bool handover_covers(const HandoverSchedule& schedule, u64 now_ns) noexcept
{
    if (schedule.period_ns == 0U)
    {
        return false;
    }
    const u64 since = (now_ns % schedule.period_ns + schedule.period_ns - schedule.phase_ns) % schedule.period_ns;
    return since <= schedule.after_ns || schedule.period_ns - since <= schedule.before_ns;
}

// Starlink's timetable on the connection's clock, from `now_ns` on it and `utc_ns` (nanoseconds since 1970 in UTC, as
// POSIX counts them: no leap seconds, so each minute starts at a multiple of 60 s) read at the same instant. A packet
// reaches the terminal's link `lead_ns` after it leaves this sender (0 for the terminal's own side, about the one-way
// delay to the terminal for its server; the paper's sender sat by the point of presence), so its events come that much
// earlier here.
[[nodiscard]] constexpr HandoverSchedule starlink_handover_schedule(u64 now_ns, u64 utc_ns, u64 lead_ns = 0U,
                                                                    u64 before_ns = kStarlinkHandoverGuardNs,
                                                                    u64 after_ns = kStarlinkHandoverGuardNs) noexcept
{
    constexpr u64 period = kStarlinkHandoverPeriodNs;
    // UTC's time since its last event, then the connection's time of that event less the lead.
    const u64 since = (utc_ns % period + period - kStarlinkHandoverSecondNs) % period;
    HandoverSchedule schedule;
    schedule.period_ns = period;
    schedule.phase_ns = (now_ns % period + 2U * period - since - lead_ns % period) % period;
    schedule.before_ns = before_ns;
    schedule.after_ns = after_ns;
    return schedule;
}

} // namespace crd::net
