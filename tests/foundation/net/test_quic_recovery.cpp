// crd-net -- QUIC acknowledgements and loss recovery (4.2a; docs/design/network-foundation.md#quic-recovery): the
// receive ranges and when an ACK is owed (RFC 9000 section 13.2), the ACK frames written and read back, RFC 9002's
// RTT estimator worked by hand, loss by both thresholds, the probe timeout and its backoff, the Application space held
// until confirmation, the client's anti-deadlock probe, the amplification-blocked server, key discards, persistent
// congestion and acknowledgements of numbers never sent (RFC 9000 sections 13.1 and 21.4).

#include <crd/net/quic_recovery.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::crypto::QuicSide;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kMaxAckDelay = 25U * kMs;

// An ACK frame as read_quic_frame hands it over: written from ranges (largest first), then read.
struct AckFrame
{
    u8        bytes[512] = {};
    QuicFrame frame;

    AckFrame(ConstSpan<QuicAckRange> ranges, u64 delay = 0U)
    {
        const usize written = write_quic_ack(ranges, delay, nullptr, Span<u8>(bytes, sizeof(bytes)));
        REQUIRE(written != 0U);
        WireReader in(ConstSpan<u8>(bytes, written));
        REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
        REQUIRE(frame.kind == QuicFrameKind::Ack);
    }
    [[nodiscard]] const QuicAck& ack() const { return frame.ack; }
};

AckFrame ack_of(u64 smallest, u64 largest, u64 delay = 0U)
{
    const QuicAckRange range{smallest, largest};
    return AckFrame(ConstSpan<QuicAckRange>(&range, 1U), delay);
}

// The events in order: 'a' acknowledged, 'l' lost, 'd' discarded, with the packet numbers.
struct Events final : QuicRecoveryEvents
{
    char  kinds[64]   = {};
    u64   numbers[64] = {};
    usize count       = 0U;

    void on_acked(QuicSpace /*space*/, const QuicSentPacket& packet) noexcept override { add('a', packet); }
    void on_lost(QuicSpace /*space*/, const QuicSentPacket& packet) noexcept override { add('l', packet); }
    void on_discarded(QuicSpace /*space*/, const QuicSentPacket& packet) noexcept override { add('d', packet); }

    void add(char kind, const QuicSentPacket& packet) noexcept
    {
        if (count < 64U)
        {
            kinds[count]   = kind;
            numbers[count] = packet.number;
            ++count;
        }
    }
    [[nodiscard]] usize of(char kind) const
    {
        usize n = 0U;
        for (usize i = 0U; i < count; ++i)
        {
            n += (kinds[i] == kind) ? 1U : 0U;
        }
        return n;
    }
};

QuicSentPacket packet(u64 number, u64 sent_ns, bool ack_eliciting = true, u32 bytes = 1200U)
{
    QuicSentPacket out;
    out.number        = number;
    out.sent_ns       = sent_ns;
    out.ack_eliciting = ack_eliciting;
    out.in_flight     = ack_eliciting;
    out.bytes         = bytes;
    out.token         = static_cast<u32>(number);
    return out;
}

struct Fixture
{
    QuicSentEntry initial[16];
    QuicSentEntry handshake[16];
    QuicSentEntry application[64];
    QuicRecovery  recovery;
    Events        events;

    explicit Fixture(QuicSide side = QuicSide::Client)
        : recovery(QuicRecoveryConfig{side}, Span<QuicSentEntry>(initial, 16U), Span<QuicSentEntry>(handshake, 16U),
                   Span<QuicSentEntry>(application, 64U))
    {
    }
    void send(QuicSpace space, u64 number, u64 sent_ns, bool ack_eliciting = true)
    {
        REQUIRE(recovery.on_sent(space, packet(number, sent_ns, ack_eliciting)) == QuicRecoveryError::None);
    }
    QuicAckOutcome ack(QuicSpace space, u64 smallest, u64 largest, u64 now_ns, u64 delay = 0U)
    {
        const AckFrame frame = ack_of(smallest, largest, delay);
        return recovery.on_ack(space, frame.ack(), now_ns, events);
    }
    // A confirmed client whose first RTT sample is `rtt` (taken at `rtt`, from packet 0 of the Application space).
    void confirmed_with_rtt(u64 rtt)
    {
        recovery.confirm_handshake(0U);
        send(QuicSpace::Application, 0U, 0U);
        REQUIRE(ack(QuicSpace::Application, 0U, 0U, rtt).rtt_sampled);
    }
};

bool same_ranges(ConstSpan<QuicAckRange> got, std::initializer_list<QuicAckRange> want)
{
    if (got.size() != want.size())
    {
        return false;
    }
    usize i = 0U;
    for (const QuicAckRange& range : want)
    {
        if (got[i].smallest != range.smallest || got[i].largest != range.largest)
        {
            return false;
        }
        ++i;
    }
    return true;
}
} // namespace

TEST_CASE("quic recovery: received numbers merge into ranges, largest first", "[net][quic][recovery]")
{
    QuicAckTracker tracker;
    tracker.reset(QuicSpace::Application);
    for (const u64 number : {0U, 1U, 2U, 5U, 4U, 9U, 7U})
    {
        CHECK(tracker.on_received(number, false, QuicEcn::NotEct, 0U, kMaxAckDelay));
    }
    CHECK(same_ranges(tracker.ranges(), {{9U, 9U}, {7U, 7U}, {4U, 5U}, {0U, 2U}}));
    CHECK(tracker.on_received(3U, false, QuicEcn::NotEct, 0U, kMaxAckDelay)); // joins two ranges
    CHECK(tracker.on_received(8U, false, QuicEcn::NotEct, 0U, kMaxAckDelay));
    CHECK(same_ranges(tracker.ranges(), {{7U, 9U}, {0U, 5U}}));
    CHECK_FALSE(tracker.on_received(4U, true, QuicEcn::Ce, 0U, kMaxAckDelay)); // a duplicate changes nothing
    CHECK(tracker.ack_deadline_ns() == kNeverNs);
    CHECK(tracker.ecn().ce == 0U);
    CHECK(same_ranges(tracker.ranges(), {{7U, 9U}, {0U, 5U}}));
}

TEST_CASE("quic recovery: full ranges drop the smallest and leave out anything older", "[net][quic][recovery]")
{
    QuicAckTracker tracker;
    tracker.reset(QuicSpace::Application);
    for (u64 i = 0U; i < kQuicAckRanges; ++i)
    {
        CHECK(tracker.on_received(10U + 2U * i, false, QuicEcn::NotEct, 0U, kMaxAckDelay));
    }
    REQUIRE(tracker.ranges().size() == kQuicAckRanges);
    CHECK(tracker.on_received(1U, false, QuicEcn::NotEct, 0U, kMaxAckDelay)); // older than all: left out
    CHECK(tracker.ranges()[kQuicAckRanges - 1U].smallest == 10U);
    CHECK(tracker.on_received(500U, false, QuicEcn::NotEct, 0U, kMaxAckDelay)); // the smallest range makes room
    CHECK(tracker.ranges().size() == kQuicAckRanges);
    CHECK(tracker.ranges()[0].largest == 500U);
    CHECK(tracker.ranges()[kQuicAckRanges - 1U].smallest == 12U);
}

TEST_CASE("quic recovery: when an ACK is owed (RFC 9000 13.2.1 and 13.2.2)", "[net][quic][recovery]")
{
    QuicAckTracker app;
    app.reset(QuicSpace::Application);
    CHECK(app.on_received(0U, false, QuicEcn::NotEct, 100U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == kNeverNs); // non-ack-eliciting packets owe nothing
    CHECK(app.on_received(1U, true, QuicEcn::NotEct, 200U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == 200U + kMaxAckDelay);
    CHECK(app.on_received(2U, true, QuicEcn::NotEct, 300U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == 300U); // the second ack-eliciting packet

    u8 room[64];
    REQUIRE(app.write(Span<u8>(room, sizeof(room)), 400U, 3U).bytes != 0U);
    CHECK(app.ack_deadline_ns() == kNeverNs);
    CHECK(app.on_received(4U, true, QuicEcn::NotEct, 500U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == 500U); // a gap before it
    REQUIRE(app.write(Span<u8>(room, sizeof(room)), 500U, 3U).bytes != 0U);
    CHECK(app.on_received(3U, true, QuicEcn::NotEct, 600U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == 600U); // out of order
    REQUIRE(app.write(Span<u8>(room, sizeof(room)), 600U, 3U).bytes != 0U);
    CHECK(app.on_received(5U, true, QuicEcn::Ce, 700U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == 700U); // congestion experienced
    REQUIRE(app.write(Span<u8>(room, sizeof(room)), 700U, 3U).bytes != 0U);
    CHECK(app.on_received(6U, true, QuicEcn::Ect0, 800U, kMaxAckDelay));
    CHECK(app.ack_deadline_ns() == 800U + kMaxAckDelay); // in order, the first since the last ACK

    for (const QuicSpace space : {QuicSpace::Initial, QuicSpace::Handshake})
    {
        QuicAckTracker tracker;
        tracker.reset(space);
        CHECK(tracker.on_received(0U, true, QuicEcn::NotEct, 50U, kMaxAckDelay));
        CHECK(tracker.ack_deadline_ns() == 50U); // Initial and Handshake: at once
    }
}

TEST_CASE("quic recovery: ACK frames carry the ranges, the delay and the ECN counts", "[net][quic][recovery]")
{
    QuicAckTracker tracker;
    tracker.reset(QuicSpace::Application);
    for (const u64 number : {0U, 1U, 4U, 5U, 9U})
    {
        CHECK(tracker.on_received(number, true, QuicEcn::NotEct, number * kMs, kMaxAckDelay));
    }
    u8                 room[64];
    const QuicAckWrite written = tracker.write(Span<u8>(room, sizeof(room)), 9U * kMs + 800000U, 3U);
    REQUIRE(written.bytes != 0U);
    CHECK(written.largest == 9U);
    WireReader in(ConstSpan<u8>(room, written.bytes));
    QuicFrame  frame;
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK_FALSE(frame.ack.ecn);
    CHECK(frame.ack.delay == 100U); // 800 us since packet 9 arrived, scaled down by 2^3
    QuicAckRanges walk(frame.ack);
    QuicAckRange  range;
    QuicAckRange  seen[4];
    usize         count = 0U;
    while (walk.next(range) && count < 4U)
    {
        seen[count++] = range;
    }
    CHECK(same_ranges(ConstSpan<QuicAckRange>(seen, count), {{9U, 9U}, {4U, 5U}, {0U, 1U}}));

    // Short room: the largest ranges that fit; no room for even one: nothing, and the ACK stays owed.
    CHECK(tracker.on_received(11U, true, QuicEcn::Ect1, 11U * kMs, kMaxAckDelay));
    CHECK(tracker.on_received(12U, true, QuicEcn::Ce, 12U * kMs, kMaxAckDelay));
    CHECK(tracker.write(Span<u8>(room, 3U), 12U * kMs, 3U).bytes == 0U);
    CHECK(tracker.ack_deadline_ns() == 11U * kMs); // owed at once since 11: 10 is missing
    const usize one = quic_ack_bytes(ConstSpan<QuicAckRange>(tracker.ranges().data(), 1U), 0U,
                                     &tracker.ecn());
    REQUIRE(one != 0U);
    const QuicAckWrite trimmed = tracker.write(Span<u8>(room, one), 12U * kMs, 3U);
    REQUIRE(trimmed.bytes == one);
    WireReader again(ConstSpan<u8>(room, trimmed.bytes));
    REQUIRE(read_quic_frame(again, frame) == QuicTransportError::NoError);
    CHECK(frame.ack.ecn);
    CHECK(frame.ack.ect0 == 0U);
    CHECK(frame.ack.ect1 == 1U);
    CHECK(frame.ack.ce == 1U);
    CHECK(frame.ack.largest == 12U);
    CHECK(frame.ack.first_range == 1U);
    CHECK(frame.ack.range_count == 0U);
}

TEST_CASE("quic recovery: an acknowledged ACK frame retires its ranges (RFC 9000 13.2.4)", "[net][quic][recovery]")
{
    QuicAckTracker tracker;
    tracker.reset(QuicSpace::Application);
    for (const u64 number : {0U, 1U, 2U, 5U, 6U, 7U, 9U})
    {
        CHECK(tracker.on_received(number, false, QuicEcn::NotEct, 0U, kMaxAckDelay));
    }
    tracker.on_ack_acknowledged(5U);
    CHECK(same_ranges(tracker.ranges(), {{9U, 9U}, {6U, 7U}}));
    tracker.on_ack_acknowledged(20U); // the largest received number always stays
    CHECK(same_ranges(tracker.ranges(), {{9U, 9U}}));
}

TEST_CASE("quic recovery: the RTT estimator follows RFC 9002 section 5", "[net][quic][recovery]")
{
    QuicRtt rtt;
    CHECK(rtt.smoothed_ns == kQuicInitialRttNs);
    CHECK(rtt.variance_ns == kQuicInitialRttNs / 2U);
    CHECK(rtt.probe_base_ns() == kQuicInitialRttNs * 3U);
    rtt.sample(100U * kMs, 40U * kMs, false, kMaxAckDelay, 7U);
    CHECK(rtt.min_ns == 100U * kMs); // the first sample ignores the delay
    CHECK(rtt.smoothed_ns == 100U * kMs);
    CHECK(rtt.variance_ns == 50U * kMs);
    CHECK(rtt.first_sample_ns == 7U);

    rtt.sample(120U * kMs, 10U * kMs, false, kMaxAckDelay, 8U); // adjusted 110 ms
    CHECK(rtt.variance_ns == 40U * kMs);
    CHECK(rtt.smoothed_ns == 101250000U);
    CHECK(rtt.first_sample_ns == 7U);

    QuicRtt below = rtt;
    below.sample(105U * kMs, 10U * kMs, false, kMaxAckDelay, 9U); // 105 - 10 would pass under min_rtt: kept
    CHECK(below.smoothed_ns == (7U * 101250000U + 105U * kMs) / 8U);

    QuicRtt open = rtt;
    open.sample(200U * kMs, 50U * kMs, false, kMaxAckDelay, 9U); // not capped before confirmation: adjusted 150
    CHECK(open.smoothed_ns == (7U * 101250000U + 150U * kMs) / 8U);
    QuicRtt capped = rtt;
    capped.sample(200U * kMs, 50U * kMs, true, kMaxAckDelay, 9U); // capped at max_ack_delay: adjusted 175
    CHECK(capped.smoothed_ns == (7U * 101250000U + 175U * kMs) / 8U);
    CHECK(capped.min_ns == 100U * kMs);
    capped.sample(80U * kMs, 0U, true, kMaxAckDelay, 10U);
    CHECK(capped.min_ns == 80U * kMs);
}

TEST_CASE("quic recovery: an RTT sample needs the largest newly acknowledged and an ack-eliciting packet",
          "[net][quic][recovery]")
{
    Fixture f;
    f.send(QuicSpace::Initial, 0U, 0U);
    f.send(QuicSpace::Initial, 1U, 10U * kMs, false);
    const QuicAckOutcome only_ack = f.ack(QuicSpace::Initial, 1U, 1U, 50U * kMs);
    CHECK(only_ack.acked_packets == 1U);
    CHECK_FALSE(only_ack.rtt_sampled); // the largest is not ack-eliciting
    const QuicAckOutcome again = f.ack(QuicSpace::Initial, 0U, 1U, 60U * kMs);
    CHECK(again.acked_packets == 1U);
    CHECK_FALSE(again.rtt_sampled); // the largest was not newly acknowledged
    CHECK_FALSE(f.recovery.rtt().sampled());

    f.send(QuicSpace::Initial, 2U, 100U * kMs);
    const QuicAckOutcome sampled = f.ack(QuicSpace::Initial, 2U, 2U, 130U * kMs, 1000U);
    CHECK(sampled.rtt_sampled);
    CHECK(f.recovery.rtt().latest_ns == 30U * kMs);
    CHECK(f.recovery.rtt().first_sample_ns == 130U * kMs);
    const QuicAckOutcome stale = f.ack(QuicSpace::Initial, 0U, 2U, 140U * kMs);
    CHECK(stale.acked_packets == 0U); // nothing new: no sample, no events
    CHECK(f.events.of('a') == 3U);
}

TEST_CASE("quic recovery: the ACK delay per space (ignored in Initial, exponent 3 in Handshake, the peer's after)",
          "[net][quic][recovery]")
{
    for (const QuicSpace space : {QuicSpace::Initial, QuicSpace::Handshake, QuicSpace::Application})
    {
        Fixture f(QuicSide::Server);
        f.recovery.set_peer_parameters(kMaxAckDelay, 0U, 0U);
        f.send(space, 0U, 0U);
        REQUIRE(f.ack(space, 0U, 0U, 100U * kMs).rtt_sampled);
        f.send(space, 1U, 200U * kMs);
        REQUIRE(f.ack(space, 1U, 1U, 330U * kMs, 2500U).rtt_sampled); // 2.5 ms raw at exponent 0, 20 ms at 3
        u64 adjusted = 130U * kMs;
        if (space == QuicSpace::Handshake)
        {
            adjusted -= 20U * kMs;
        }
        else if (space == QuicSpace::Application)
        {
            adjusted -= 2500U * 1000U;
        }
        CHECK(f.recovery.rtt().smoothed_ns == (7U * 100U * kMs + adjusted) / 8U);
    }
}

TEST_CASE("quic recovery: acknowledging a number never sent is a protocol violation", "[net][quic][recovery]")
{
    Fixture f;
    f.send(QuicSpace::Application, 0U, 0U);
    f.send(QuicSpace::Application, 1U, 0U);
    f.send(QuicSpace::Application, 3U, 0U); // 2 skipped (RFC 9000 section 21.4)
    const u64 in_flight = f.recovery.bytes_in_flight();
    CHECK(f.ack(QuicSpace::Application, 0U, 4U, kMs).error == QuicRecoveryError::AckOfUnsent);
    CHECK(f.ack(QuicSpace::Application, 2U, 3U, kMs).error == QuicRecoveryError::AckOfUnsent);
    CHECK(f.recovery.bytes_in_flight() == in_flight);
    CHECK(f.recovery.largest_acked(QuicSpace::Application) == kQuicNoPacket);
    CHECK(f.events.count == 0U);

    const QuicAckRange split[2] = {{3U, 3U}, {0U, 1U}};
    const AckFrame     around(ConstSpan<QuicAckRange>(split, 2U));
    const QuicAckOutcome fine = f.recovery.on_ack(QuicSpace::Application, around.ack(), kMs, f.events);
    CHECK(fine.error == QuicRecoveryError::None);
    CHECK(fine.acked_packets == 3U);
    CHECK(f.recovery.bytes_in_flight() == 0U);
    // Past the last number sent stays refused once everything held has settled.
    CHECK(f.ack(QuicSpace::Application, 4U, 4U, kMs).error == QuicRecoveryError::AckOfUnsent);
}

TEST_CASE("quic recovery: loss by the packet threshold, then by the time threshold", "[net][quic][recovery]")
{
    Fixture f;
    f.confirmed_with_rtt(100U * kMs); // srtt 100 ms: the loss delay is 112.5 ms
    for (u64 n = 1U; n <= 5U; ++n)
    {
        f.send(QuicSpace::Application, n, 200U * kMs + n * kMs);
    }
    const QuicAckOutcome out = f.ack(QuicSpace::Application, 5U, 5U, 300U * kMs);
    CHECK(out.lost.packets == 2U); // 1 and 2: three or more below the largest acknowledged
    CHECK(out.lost.bytes == 2U * 1200U);
    CHECK(out.lost.newest_sent_ns == 202U * kMs);
    CHECK_FALSE(out.lost.persistent_congestion);
    // Lost before acknowledged (appendix A.7).
    REQUIRE(f.events.count == 4U);
    CHECK(f.events.kinds[1] == 'l');
    CHECK(f.events.kinds[2] == 'l');
    CHECK(f.events.kinds[3] == 'a');
    CHECK(f.events.numbers[3] == 5U);
    // 3 and 4 wait for the time threshold: 9/8 of max(srtt, latest_rtt) after they were sent.
    const u64 loss_delay = f.recovery.rtt().smoothed_ns * 9U / 8U;
    const u64 due        = 203U * kMs + loss_delay;
    CHECK(f.recovery.loss_timer_ns() == due);
    CHECK(f.recovery.on_timeout(due - 1U, f.events).kind == QuicTimeoutKind::None);
    const QuicTimeout timeout = f.recovery.on_timeout(due, f.events);
    CHECK(timeout.kind == QuicTimeoutKind::Loss);
    CHECK(timeout.space == QuicSpace::Application);
    CHECK(timeout.lost.packets == 1U); // 3 now; 4 a millisecond later
    CHECK(f.recovery.loss_timer_ns() == 204U * kMs + loss_delay);
    CHECK(f.recovery.pto_count() == 0U);
    // A late acknowledgement of a packet declared lost settles nothing more.
    CHECK(f.ack(QuicSpace::Application, 1U, 1U, due).acked_packets == 0U);
}

TEST_CASE("quic recovery: the probe timeout, its backoff and its reset", "[net][quic][recovery]")
{
    Fixture f;
    f.recovery.set_peer_parameters(kMaxAckDelay, 3U, 0U);
    f.confirmed_with_rtt(100U * kMs); // srtt 100, rttvar 50: period 100 + 200 + 25 ms
    f.send(QuicSpace::Application, 1U, 500U * kMs);
    const u64 period = 325U * kMs;
    CHECK(f.recovery.probe_period_ns(QuicSpace::Application) == period);
    CHECK(f.recovery.loss_timer_ns() == 500U * kMs + period);
    const QuicTimeout first = f.recovery.on_timeout(500U * kMs + period, f.events);
    CHECK(first.kind == QuicTimeoutKind::Probe);
    CHECK(first.space == QuicSpace::Application);
    CHECK(first.probes == 2U);
    CHECK(first.lost.packets == 0U); // a timeout declares nothing lost
    CHECK(f.recovery.pto_count() == 1U);
    CHECK(f.recovery.loss_timer_ns() == 500U * kMs + 2U * period); // doubled, still from the last sent
    f.send(QuicSpace::Application, 2U, 900U * kMs);
    CHECK(f.recovery.loss_timer_ns() == 900U * kMs + 2U * period);
    CHECK(f.ack(QuicSpace::Application, 2U, 2U, 1000U * kMs).acked_packets == 1U);
    CHECK(f.recovery.pto_count() == 0U);
}

TEST_CASE("quic recovery: the Application space's probe timeout waits for confirmation", "[net][quic][recovery]")
{
    Fixture f(QuicSide::Server);
    f.send(QuicSpace::Application, 0U, 0U);
    CHECK(f.recovery.loss_timer_ns() == kNeverNs);
    f.send(QuicSpace::Handshake, 0U, 30U * kMs);
    CHECK(f.recovery.loss_timer_ns() == 30U * kMs + 3U * kQuicInitialRttNs); // no max_ack_delay outside Application
    f.recovery.confirm_handshake(20U * kMs);
    CHECK(f.recovery.loss_timer_ns() == 3U * kQuicInitialRttNs + kMaxAckDelay); // the earlier Application packet
}

TEST_CASE("quic recovery: a client's anti-deadlock probe before the server validated it", "[net][quic][recovery]")
{
    Fixture f;
    f.send(QuicSpace::Initial, 0U, 0U);
    REQUIRE(f.ack(QuicSpace::Initial, 0U, 0U, 100U * kMs).acked_packets == 1U);
    CHECK(f.recovery.bytes_in_flight() == 0U);
    const u64 period = f.recovery.probe_period_ns(QuicSpace::Initial);
    CHECK(f.recovery.loss_timer_ns() == 100U * kMs + period); // nothing in flight, yet armed
    f.send(QuicSpace::Initial, 1U, 150U * kMs, false);          // an ACK-only packet moves no timer
    CHECK(f.recovery.loss_timer_ns() == 100U * kMs + period);
    const QuicTimeout first = f.recovery.on_timeout(100U * kMs + period, f.events);
    CHECK(first.kind == QuicTimeoutKind::AntiDeadlock);
    CHECK(first.space == QuicSpace::Initial); // a padded Initial
    CHECK(first.probes == 1U);
    f.recovery.set_handshake_keys(100U * kMs + period);
    const u64   later  = f.recovery.loss_timer_ns();
    const QuicTimeout second = f.recovery.on_timeout(later, f.events);
    CHECK(second.kind == QuicTimeoutKind::AntiDeadlock);
    CHECK(second.space == QuicSpace::Handshake);
    CHECK(f.recovery.pto_count() == 2U);
    // An ACK in a Handshake packet: the server has validated the client, which then waits.
    f.send(QuicSpace::Handshake, 0U, later);
    REQUIRE(f.ack(QuicSpace::Handshake, 0U, 0U, later + kMs).acked_packets == 1U);
    CHECK(f.recovery.pto_count() == 0U);
    CHECK(f.recovery.loss_timer_ns() == kNeverNs);
}

TEST_CASE("quic recovery: a server blocked by the amplification limit waits for a datagram",
          "[net][quic][recovery]")
{
    Fixture f(QuicSide::Server);
    f.send(QuicSpace::Initial, 0U, 0U);
    const u64 armed = f.recovery.loss_timer_ns();
    CHECK(armed == 3U * kQuicInitialRttNs);
    f.recovery.set_amplification_blocked(true, kMs);
    CHECK(f.recovery.loss_timer_ns() == kNeverNs);
    CHECK(f.recovery.on_timeout(armed, f.events).kind == QuicTimeoutKind::None);
    f.recovery.set_amplification_blocked(false, armed + kMs); // already due (appendix A.6)
    CHECK(f.recovery.loss_timer_ns() == armed);
    CHECK(f.recovery.on_timeout(armed + kMs, f.events).kind == QuicTimeoutKind::Probe);
}

TEST_CASE("quic recovery: discarding keys drops the space's packets from flight (RFC 9002 6.4)",
          "[net][quic][recovery]")
{
    Fixture f;
    f.send(QuicSpace::Initial, 0U, 0U);
    f.send(QuicSpace::Initial, 1U, 0U);
    f.send(QuicSpace::Handshake, 0U, kMs);
    CHECK(f.recovery.on_received(QuicSpace::Initial, 0U, true, QuicEcn::NotEct, 0U));
    REQUIRE(f.recovery.on_timeout(f.recovery.loss_timer_ns(), f.events).kind == QuicTimeoutKind::Probe);
    CHECK(f.recovery.pto_count() == 1U);
    REQUIRE(f.recovery.discard(QuicSpace::Initial, 2U * kMs, f.events));
    CHECK(f.events.of('d') == 2U);
    CHECK(f.recovery.bytes_in_flight() == 1200U);
    CHECK(f.recovery.pto_count() == 0U);
    CHECK(f.recovery.ack_deadline_ns(QuicSpace::Initial) == kNeverNs);
    CHECK(f.recovery.received(QuicSpace::Initial).ranges().empty());
    CHECK_FALSE(f.recovery.can_send(QuicSpace::Initial));
    CHECK(f.recovery.on_sent(QuicSpace::Initial, packet(2U, 0U)) == QuicRecoveryError::Discarded);
    CHECK(f.ack(QuicSpace::Initial, 0U, 0U, 3U * kMs).error == QuicRecoveryError::Discarded);
    CHECK_FALSE(f.recovery.on_received(QuicSpace::Initial, 1U, true, QuicEcn::NotEct, 0U));
    u8 room[32];
    CHECK(f.recovery.write_ack(QuicSpace::Initial, Span<u8>(room, sizeof(room)), 0U).bytes == 0U);
    CHECK(f.recovery.loss_timer_ns() == kMs + 3U * kQuicInitialRttNs); // the Handshake packet
    CHECK_FALSE(f.recovery.discard(QuicSpace::Application, 0U, f.events));
}

TEST_CASE("quic recovery: persistent congestion needs a span, no acknowledgement between and an RTT sample",
          "[net][quic][recovery]")
{
    const auto run = [](bool ack_between, u64 spacing)
    {
        Fixture f;
        f.confirmed_with_rtt(10U * kMs);
        const u64 start = 20U * kMs;
        for (u64 n = 1U; n <= 4U; ++n)
        {
            f.send(QuicSpace::Application, n, start + (n - 1U) * spacing);
        }
        f.send(QuicSpace::Application, 8U, start + 4U * spacing);
        // One ACK frame for 8 alone, or for 8 and 2: the same RTT sample either way.
        const QuicAckRange ranges[2] = {{8U, 8U}, {2U, 2U}};
        const AckFrame     frame(ConstSpan<QuicAckRange>(ranges, ack_between ? 2U : 1U));
        return f.recovery.on_ack(QuicSpace::Application, frame.ack(), start + 4U * spacing + kMs, f.events).lost;
    };
    Fixture probe;
    probe.confirmed_with_rtt(10U * kMs);
    const u64 duration = probe.recovery.persistent_congestion_ns();
    CHECK(duration == (10U * kMs + 20U * kMs + kMaxAckDelay) * 3U);

    const QuicLoss wide = run(false, duration / 2U);
    CHECK(wide.packets == 4U);
    CHECK(wide.persistent_congestion); // 1 to 4 span 1.5 durations
    const QuicLoss split = run(true, duration / 2U);
    CHECK(split.packets == 3U);
    CHECK_FALSE(split.persistent_congestion); // 2 acknowledged between: 1 alone, 3 to 4 too short
    CHECK_FALSE(run(false, duration / 4U).persistent_congestion); // 0.75 durations

    // Packets sent before the first RTT sample never count.
    Fixture early;
    early.recovery.confirm_handshake(0U);
    early.send(QuicSpace::Application, 0U, 0U);
    early.send(QuicSpace::Application, 1U, 5U * kQuicInitialRttNs);
    early.send(QuicSpace::Application, 5U, 10U * kQuicInitialRttNs);
    const QuicAckOutcome out = early.ack(QuicSpace::Application, 5U, 5U, 10U * kQuicInitialRttNs + kMs);
    CHECK(out.lost.packets == 2U);
    CHECK_FALSE(out.lost.persistent_congestion);
}

TEST_CASE("quic recovery: an acknowledged packet that carried an ACK retires those ranges", "[net][quic][recovery]")
{
    Fixture f;
    for (u64 n = 0U; n < 6U; ++n)
    {
        CHECK(f.recovery.on_received(QuicSpace::Initial, n * 2U, true, QuicEcn::NotEct, n));
    }
    u8                 room[128];
    const QuicAckWrite written = f.recovery.write_ack(QuicSpace::Initial, Span<u8>(room, sizeof(room)), 10U);
    REQUIRE(written.bytes != 0U);
    CHECK(written.largest == 10U);
    QuicSentPacket carrier = packet(0U, 10U);
    carrier.carried_ack    = written.largest;
    REQUIRE(f.recovery.on_sent(QuicSpace::Initial, carrier) == QuicRecoveryError::None);
    CHECK(f.recovery.on_received(QuicSpace::Initial, 12U, true, QuicEcn::NotEct, 20U));
    REQUIRE(f.ack(QuicSpace::Initial, 0U, 0U, 30U).acked_packets == 1U);
    CHECK(same_ranges(f.recovery.received(QuicSpace::Initial).ranges(), {{12U, 12U}}));
}

TEST_CASE("quic recovery: the storage bounds what is in flight; numbers only rise", "[net][quic][recovery]")
{
    QuicSentEntry initial[1];
    QuicSentEntry handshake[1];
    QuicSentEntry application[3];
    QuicRecovery  recovery(QuicRecoveryConfig{QuicSide::Server}, Span<QuicSentEntry>(initial, 1U),
                           Span<QuicSentEntry>(handshake, 1U), Span<QuicSentEntry>(application, 3U));
    Events        events;
    for (u64 n = 0U; n < 3U; ++n)
    {
        REQUIRE(recovery.on_sent(QuicSpace::Application, packet(n, n)) == QuicRecoveryError::None);
    }
    CHECK_FALSE(recovery.can_send(QuicSpace::Application));
    CHECK(recovery.on_sent(QuicSpace::Application, packet(3U, 3U)) == QuicRecoveryError::Full);
    // Acknowledging the middle packet frees nothing until the oldest settles; the ring then wraps.
    CHECK(recovery.on_ack(QuicSpace::Application, ack_of(1U, 1U).ack(), kMs, events).acked_packets == 1U);
    CHECK_FALSE(recovery.can_send(QuicSpace::Application));
    CHECK(recovery.on_ack(QuicSpace::Application, ack_of(0U, 0U).ack(), kMs, events).acked_packets == 1U);
    CHECK(recovery.can_send(QuicSpace::Application));
    CHECK(recovery.on_sent(QuicSpace::Application, packet(2U, 4U)) == QuicRecoveryError::BadNumber);
    REQUIRE(recovery.on_sent(QuicSpace::Application, packet(7U, 4U)) == QuicRecoveryError::None);
    REQUIRE(recovery.on_sent(QuicSpace::Application, packet(8U, 5U)) == QuicRecoveryError::None);
    CHECK(recovery.on_ack(QuicSpace::Application, ack_of(5U, 7U).ack(), kMs, events).error ==
          QuicRecoveryError::AckOfUnsent); // 3 to 6 were skipped
    const QuicAckOutcome late = recovery.on_ack(QuicSpace::Application, ack_of(7U, 8U).ack(), kMs, events);
    CHECK(late.acked_packets == 2U);
    CHECK(late.lost.packets == 1U); // 2, six below the largest acknowledged
    CHECK(recovery.bytes_in_flight() == 0U);
    CHECK(recovery.can_send(QuicSpace::Application));
    CHECK(recovery.on_sent(QuicSpace::Application, packet(crd::crypto::kQuicMaxPacketNumber + 1U, 6U)) ==
          QuicRecoveryError::BadNumber);
}

TEST_CASE("quic recovery: the next timer is the earliest of loss detection and every owed ACK",
          "[net][quic][recovery]")
{
    Fixture f(QuicSide::Server);
    CHECK(f.recovery.next_timer_ns() == kNeverNs);
    CHECK(f.recovery.on_received(QuicSpace::Application, 0U, true, QuicEcn::NotEct, 5U * kMs));
    CHECK(f.recovery.next_timer_ns() == 5U * kMs + kMaxAckDelay);
    CHECK(f.recovery.on_received(QuicSpace::Handshake, 0U, true, QuicEcn::NotEct, 6U * kMs));
    CHECK(f.recovery.next_timer_ns() == 6U * kMs);
    u8 room[32];
    REQUIRE(f.recovery.write_ack(QuicSpace::Handshake, Span<u8>(room, sizeof(room)), 6U * kMs).bytes != 0U);
    CHECK(f.recovery.next_timer_ns() == 5U * kMs + kMaxAckDelay);
    f.send(QuicSpace::Initial, 0U, 0U);
    CHECK(f.recovery.next_timer_ns() == 5U * kMs + kMaxAckDelay);
}
