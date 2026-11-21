// crd-net -- a QUIC path's sending state (NET-PATH; docs/design/network-foundation.md#multipath): what start()
// builds and refuses, each recovery event's meaning (a lost PMTU probe and a handover's loss are no congestion
// signal, a spurious loss's data was delivered), ECN validation and CE, the PMTU search's size reaching the
// controller, persistent congestion, a move that keeps or restarts the state, a live policy swap, the handover
// schedule and learning, the pacer's timer, a Retry's reset, and two paths' controls never touching each other.

#include <crd/net/quic_path_control.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kSecond = 1000U * kMs;

QuicPath path_v4(u8 last, crd::u16 port = 4433U)
{
    return QuicPath{0U, Endpoint{IpAddress::v4(192, 0, 2, last), port}};
}

QuicPath path_v6()
{
    const u8 bytes[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return QuicPath{0U, Endpoint{IpAddress::v6(bytes), 4433U}};
}

QuicSentPacket packet(u64 number, u64 sent_ns, u32 bytes = 1200U, Ecn ecn = Ecn::NotEct)
{
    QuicSentPacket sent;
    sent.number = number;
    sent.sent_ns = sent_ns;
    sent.bytes = bytes;
    sent.ack_eliciting = true;
    sent.in_flight = true;
    sent.ecn = ecn;
    return sent;
}

QuicRtt rtt_of(u64 rtt_ns)
{
    QuicRtt rtt;
    rtt.latest_ns = rtt_ns;
    rtt.adjusted_ns = rtt_ns;
    rtt.min_ns = rtt_ns;
    rtt.smoothed_ns = rtt_ns;
    rtt.variance_ns = rtt_ns / 2U;
    rtt.first_sample_ns = rtt_ns;
    return rtt;
}

QuicAck ack_of(u64 largest, u64 ect0, u64 ce)
{
    QuicAck ack;
    ack.largest = largest;
    ack.ecn = true;
    ack.ect0 = ect0;
    ack.ce = ce;
    return ack;
}

QuicAckOutcome outcome_of(u64 ect0, u64 newest_sent_ns)
{
    QuicAckOutcome outcome;
    outcome.advanced = true;
    outcome.acked_packets = ect0;
    outcome.ect0_packets = ect0;
    outcome.newest_sent_ns = newest_sent_ns;
    return outcome;
}

// Sends `packet` on `control` as the connection does: stamped, then counted in flight.
void send(QuicPathControl& control, QuicSentPacket& sent, u64 in_flight, u64 now_ns)
{
    sent.delivery = control.stamp(sent.bytes, in_flight + sent.bytes, now_ns);
    control.on_sent(QuicSpace::Application, sent, true, in_flight + sent.bytes, rtt_of(50U * kMs), now_ns);
}

struct Two
{
    QuicPathControl a;
    QuicPathControl b;

    explicit Two(const QuicPathControlConfig& config = QuicPathControlConfig{})
    {
        REQUIRE(a.start(config, path_v4(1), 1U));
        REQUIRE(b.start(config, path_v4(2), 2U));
    }
};
} // namespace

TEST_CASE("A path control starts from its configuration and refuses what it cannot build", "[path_control]")
{
    QuicPathControl control;
    CHECK_FALSE(control.started());
    QuicPathControlConfig invalid;
    invalid.congestion.minimum_packets = 0U;
    CHECK_FALSE(control.start(invalid, path_v4(1), 1U));
    QuicPathControlConfig small;
    small.base_datagram_bytes = 1100U;
    CHECK_FALSE(control.start(small, path_v4(1), 1U));
    QuicPathControlConfig inverted;
    inverted.base_datagram_bytes = 1400U;
    inverted.max_datagram_bytes = 1300U;
    CHECK_FALSE(control.start(inverted, path_v4(1), 1U));
    CHECK_FALSE(control.started());

    QuicPathControlConfig config;
    config.congestion.algorithm = CongestionAlgorithm::Prague;
    config.base_datagram_bytes = kQuicIpv6BaseDatagram;
    config.handover = HandoverSchedule{15U * kSecond, 12U * kSecond, 100U * kMs, 100U * kMs};
    REQUIRE(control.start(config, path_v4(1), 1U));
    CHECK(control.started());
    CHECK(control.path() == path_v4(1));
    CHECK(control.datagram_bytes() == kQuicPmtuFloor); // the base is confirmed by a probe first (RFC 8899)
    CHECK(control.path_mtu().state() == QuicPmtuState::Base);
    CHECK(control.congestion().algorithm() == CongestionAlgorithm::Prague);
    CHECK(control.marking(0U, 300U * kMs) == Ecn::Ect1); // Prague marks ECT(1) while testing
    CHECK(control.handover() == config.handover);
    CHECK(control.covered(12U * kSecond));
    CHECK_FALSE(control.covered(6U * kSecond));
    CHECK(control.pace_timer_ns() == kNeverNs);

    // Started again: the old state goes, the new configuration's comes.
    QuicPathControlConfig plain;
    plain.ecn = false;
    REQUIRE(control.start(plain, path_v4(3), 1U));
    CHECK(control.congestion().algorithm() == CongestionAlgorithm::Cubic);
    CHECK(control.marking(0U, 300U * kMs) == Ecn::NotEct);
    CHECK(control.ecn().state() == QuicEcnState::Off);
    CHECK(control.handover().period_ns == 0U);
    CHECK(control.datagram_bytes() == kQuicPmtuFloor);
}

TEST_CASE("A path's loss reduces its own window and no other path's", "[path_control][multipath]")
{
    Two two;
    const u64 initial = two.a.congestion().window();
    REQUIRE(initial == two.b.congestion().window());
    QuicSentPacket sent = packet(0U, 10U * kMs);
    send(two.a, sent, 0U, 10U * kMs);
    const QuicPathSignals signals = two.a.on_lost(QuicSpace::Application, sent, 100U * kMs);
    CHECK_FALSE(signals.handover_loss);
    CHECK_FALSE(signals.handover_lock);
    CHECK(two.a.congestion().window() < initial);
    CHECK(two.a.delivery().lost() == sent.bytes);
    CHECK(two.b.congestion().window() == initial);
    CHECK(two.b.delivery().lost() == 0U);

    // A spurious loss: its data was delivered after all.
    two.a.on_spurious(QuicSpace::Application, sent, 100U * kMs, 120U * kMs);
    CHECK(two.a.delivery().delivered() == sent.bytes);
    CHECK(two.b.delivery().delivered() == 0U);

    // Persistent congestion collapses one path's window to its floor.
    QuicLoss persistent;
    persistent.persistent_congestion = true;
    two.b.end_detection(persistent, 200U * kMs);
    CHECK(two.b.congestion().window() == 2U * kQuicPmtuFloor);
    CHECK(two.a.congestion().window() > 2U * kQuicPmtuFloor);

    // Recovery started over (a Retry): congestion control does too.
    two.b.reset(300U * kMs);
    CHECK(two.b.congestion().window() == initial);
}

TEST_CASE("A lost PMTU probe and a handover's loss are no congestion signal", "[path_control][multipath]")
{
    QuicPathControlConfig config;
    config.handover = HandoverSchedule{15U * kSecond, 12U * kSecond, 100U * kMs, 100U * kMs};
    Two two(config);
    const u64 initial = two.a.congestion().window();

    const u32 probe = two.a.probe(0U, false);
    REQUIRE(probe > kQuicPmtuFloor);
    two.a.on_probe_sent(probe);
    QuicSentPacket probed = packet(0U, 1U * kMs, probe);
    probed.path_probe = true;
    const QuicPathSignals lost_probe = two.a.on_lost(QuicSpace::Application, probed, 50U * kMs);
    CHECK_FALSE(lost_probe.handover_loss);
    CHECK(two.a.congestion().window() == initial);
    CHECK(two.a.path_mtu().counters().probes_lost == 1U);
    CHECK(two.b.path_mtu().counters().probes_sent == 0U);
    CHECK(two.b.path_mtu().counters().probes_lost == 0U);

    QuicSentPacket covered = packet(1U, 12U * kSecond);
    covered.handover = two.b.covered(covered.sent_ns);
    REQUIRE(covered.handover);
    send(two.b, covered, 0U, covered.sent_ns);
    const QuicPathSignals lost_covered = two.b.on_lost(QuicSpace::Application, covered, 12U * kSecond + 50U * kMs);
    CHECK(lost_covered.handover_loss);
    CHECK(two.b.congestion().window() == initial);
    CHECK(two.b.delivery().lost() == 0U);
}

TEST_CASE("A path's ECN validation and CE are its own", "[path_control][multipath]")
{
    Two two;
    const u64 initial = two.a.congestion().window();
    for (u64 number = 0U; number < 3U; ++number)
    {
        REQUIRE(two.a.marking(number * kMs, 300U * kMs) == Ecn::Ect0);
        QuicSentPacket sent = packet(number, number * kMs, 1200U, Ecn::Ect0);
        send(two.a, sent, number * 1200U, number * kMs);
    }
    two.a.on_ack_begin();
    QuicPathSignals signals = two.a.on_ack_end(QuicSpace::Application, ack_of(2U, 3U, 0U), outcome_of(3U, 2U * kMs),
                                               rtt_of(50U * kMs), 0U, 60U * kMs);
    CHECK_FALSE(signals.ecn_signal);
    CHECK(two.a.ecn().state() == QuicEcnState::Capable);
    CHECK(two.b.ecn().state() == QuicEcnState::Testing);

    QuicSentPacket marked = packet(3U, 70U * kMs, 1200U, Ecn::Ect0);
    send(two.a, marked, 0U, 70U * kMs);
    two.a.on_ack_begin();
    signals = two.a.on_ack_end(QuicSpace::Application, ack_of(3U, 3U, 1U), outcome_of(1U, 70U * kMs), rtt_of(50U * kMs),
                               0U, 130U * kMs);
    CHECK(signals.ecn_signal);
    CHECK(two.a.congestion().window() < initial);
    CHECK(two.b.congestion().window() == initial);

    // A path whose counts go missing fails validation and is marked no more; the other keeps testing.
    QuicSentPacket unreported = packet(0U, 1U * kMs, 1200U, Ecn::Ect0);
    send(two.b, unreported, 0U, 1U * kMs);
    QuicAck without;
    without.largest = 0U;
    two.b.on_ack_begin();
    QuicAckOutcome outcome = outcome_of(1U, 1U * kMs);
    static_cast<void>(two.b.on_ack_end(QuicSpace::Application, without, outcome, rtt_of(50U * kMs), 0U, 60U * kMs));
    CHECK(two.b.ecn().state() == QuicEcnState::Failed);
    CHECK(two.b.marking(70U * kMs, 300U * kMs) == Ecn::NotEct);
    CHECK(two.a.marking(140U * kMs, 300U * kMs) == Ecn::Ect0);
}

TEST_CASE("A path's MTU search sets its own datagram size", "[path_control][multipath]")
{
    Two two;
    const u32 probe = two.a.probe(0U, false);
    REQUIRE(probe > kQuicPmtuFloor);
    two.a.on_probe_sent(probe);
    QuicSentPacket probed = packet(0U, 1U * kMs, probe);
    probed.path_probe = true;
    two.a.on_acked(QuicSpace::Application, probed, kNeverNs, 50U * kMs);
    CHECK(two.a.path_mtu().size() == probe);
    CHECK(two.a.datagram_bytes() == kQuicPmtuFloor); // the controller hears it once the frame's detection ends
    two.a.on_ack_begin();
    static_cast<void>(
        two.a.on_ack_end(QuicSpace::Application, QuicAck{}, QuicAckOutcome{}, rtt_of(50U * kMs), 0U, 50U * kMs));
    CHECK(two.a.datagram_bytes() == probe);
    CHECK(two.b.datagram_bytes() == kQuicPmtuFloor);

    // The peer's ceiling is each path's, and a move keeps it.
    two.b.limit(1300U);
    CHECK(two.b.path_mtu().ceiling() == 1300U);
    CHECK(two.a.path_mtu().ceiling() == kQuicDefaultMaxDatagram);
    REQUIRE(two.b.move(path_v4(9), kQuicPmtuFloor, 3U));
    CHECK(two.b.path_mtu().ceiling() == 1300U);

    // A Packet Too Big quoting a packet of the path lowers no other path's search.
    const u64 before = two.b.path_mtu().counters().hints + two.b.path_mtu().counters().hints_ignored;
    two.a.on_too_big(probe, kQuicPmtuFloor + 8U, 60U * kMs);
    CHECK(two.a.path_mtu().counters().hints + two.a.path_mtu().counters().hints_ignored == 1U);
    CHECK(two.b.path_mtu().counters().hints + two.b.path_mtu().counters().hints_ignored == before);
    CHECK(two.a.datagram_bytes() == two.a.path_mtu().size());
}

TEST_CASE("A move keeps a path's state for a new port and starts it over otherwise", "[path_control][multipath]")
{
    QuicPathControlConfig config;
    config.handover = HandoverSchedule{15U * kSecond, 12U * kSecond, 100U * kMs, 100U * kMs};
    QuicPathControl control;
    REQUIRE(control.start(config, path_v4(1), 1U));
    const u64 initial = control.congestion().window();
    QuicSentPacket sent = packet(0U, 10U * kMs);
    send(control, sent, 0U, 10U * kMs);
    static_cast<void>(control.on_lost(QuicSpace::Application, sent, 100U * kMs));
    const u64 reduced = control.congestion().window();
    REQUIRE(reduced < initial);

    CHECK_FALSE(control.move(path_v4(1), kQuicPmtuFloor, 2U));        // the same path
    CHECK_FALSE(control.move(path_v4(1, 5000U), kQuicPmtuFloor, 2U)); // a NAT rebinding: only the port changed
    CHECK(control.path() == path_v4(1, 5000U));
    CHECK(control.congestion().window() == reduced);
    CHECK(control.handover() == config.handover);

    control.arm_pace(500U * kMs);
    CHECK(control.move(path_v6(), kQuicIpv6BaseDatagram, 2U)); // another address: everything starts over
    CHECK(control.path() == path_v6());
    CHECK(control.congestion().window() == initial);
    CHECK(control.datagram_bytes() == kQuicPmtuFloor);
    CHECK(control.path_mtu().state() == QuicPmtuState::Base); // the new family's base, to confirm
    CHECK(control.delivery().lost() == 0U);
    CHECK(control.handover().period_ns == 0U); // the schedule was the old link's
    CHECK(control.ecn().state() == QuicEcnState::Testing);
    CHECK(control.pace_timer_ns() == kNeverNs);
}

TEST_CASE("A path's policy swaps live and its ECN codepoint follows", "[path_control]")
{
    QuicPathControl control;
    REQUIRE(control.start(QuicPathControlConfig{}, path_v4(1), 1U));
    QuicSentPacket sent = packet(0U, 10U * kMs);
    send(control, sent, 0U, 10U * kMs);
    static_cast<void>(control.on_lost(QuicSpace::Application, sent, 100U * kMs));
    const u64 window = control.congestion().window();

    CongestionPolicy invalid;
    invalid.minimum_packets = 0U;
    CHECK_FALSE(control.set_congestion(invalid, 2U));
    CHECK(control.congestion().algorithm() == CongestionAlgorithm::Cubic);

    CongestionPolicy prague;
    prague.algorithm = CongestionAlgorithm::Prague;
    REQUIRE(control.set_congestion(prague, 2U));
    CHECK(control.congestion().algorithm() == CongestionAlgorithm::Prague);
    CHECK(control.congestion().window() == window);
    CHECK(control.marking(120U * kMs, 300U * kMs) == Ecn::Ect1);
    // A move restarts on the swapped policy.
    REQUIRE(control.move(path_v4(2), kQuicPmtuFloor, 3U));
    CHECK(control.congestion().algorithm() == CongestionAlgorithm::Prague);
}

TEST_CASE("A path's handover schedule and learning exclude each other", "[path_control]")
{
    QuicPathControl control;
    REQUIRE(control.start(QuicPathControlConfig{}, path_v4(1), 1U));
    CHECK_FALSE(control.set_handover(HandoverSchedule{kHandoverMinPeriodNs / 2U, 0U, 0U, 0U}));
    CHECK_FALSE(control.set_handover_learning(HandoverLearning{15U * kSecond, 15U * kSecond, 0U}));
    const HandoverSchedule schedule{15U * kSecond, 12U * kSecond, 100U * kMs, 100U * kMs};
    REQUIRE(control.set_handover(schedule));
    CHECK(control.handover() == schedule);
    const HandoverLearning learning{15U * kSecond, 100U * kMs, 100U * kMs};
    REQUIRE(control.set_handover_learning(learning));
    CHECK(control.handover().period_ns == 0U); // learning clears the schedule in force
    CHECK(control.handover_learning() == learning);
    REQUIRE(control.set_handover(schedule));
    CHECK(control.handover_learning().period_ns == 0U); // an authored schedule ends learning
    REQUIRE(control.set_handover(HandoverSchedule{}));
    CHECK_FALSE(control.covered(12U * kSecond));
}

TEST_CASE("A path's pacer timer keeps the earliest release, and an uncounted packet leaves the pacer", "[path_control]")
{
    QuicPathControl control;
    REQUIRE(control.start(QuicPathControlConfig{}, path_v4(1), 1U));
    control.arm_pace(300U);
    control.arm_pace(500U);
    CHECK(control.pace_timer_ns() == 300U);
    control.arm_pace(200U);
    CHECK(control.pace_timer_ns() == 200U);
    control.disarm_pace();
    CHECK(control.pace_timer_ns() == kNeverNs);

    const QuicRtt rtt = rtt_of(50U * kMs);
    const u64 now = 10U * kMs;
    const u64 open = control.release_ns(now, rtt);
    QuicSentPacket probe = packet(0U, now);
    probe.off_path = true;
    control.on_sent(QuicSpace::Application, probe, false, 0U, rtt, now); // a path validation's: not counted
    CHECK(control.release_ns(now, rtt) == open);
    u64 in_flight = 0U;
    u64 number = 1U;
    while (control.release_ns(now, rtt) <= now && number < 1000U)
    {
        QuicSentPacket sent = packet(number++, now);
        send(control, sent, in_flight, now);
        in_flight += sent.bytes;
    }
    CHECK(control.release_ns(now, rtt) > now); // the burst spent, the pacer holds the next
}
