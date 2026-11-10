// crd-net -- delivery rate sampling (NET-PATH; docs/design/network-foundation.md#bbr, draft-ietf-ccwg-bbr-07 section
// 4.1): each field of a sample by hand on a paced flight, the longer interval deciding against an ACK burst and a slow
// sender, a sample under the min RTT kept for its rounds but given no rate, the most recently sent packet chosen with
// ties broken by order, losses counted from a packet's stamp, application-limited phases that end once their bytes
// are delivered, intervals that never span an idle period, and a reset.

#include <crd/net/delivery_rate.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kBytes = 1200U;

struct Sent
{
    DeliveryStamp stamp;
    u64 sent_ns = 0U;
    u64 order = 0U;
};

// Sends `count` packets `gap_ns` apart from `start_ns`, `in_flight` bytes out before the first.
void send(DeliveryRateSampler& sampler, Sent* out, u64 count, u64 start_ns, u64 gap_ns, u64 in_flight, u64 order = 0U)
{
    for (u64 i = 0U; i < count; ++i)
    {
        out[i].sent_ns = start_ns + i * gap_ns;
        out[i].order = order + i;
        out[i].stamp = sampler.on_sent(kBytes, in_flight + (i + 1U) * kBytes, out[i].sent_ns);
    }
}

void ack(DeliveryRateSampler& sampler, const Sent* packets, u64 count, u64 now_ns, u64 min_rtt_ns = 0U)
{
    sampler.on_ack_begin();
    for (u64 i = 0U; i < count; ++i)
    {
        sampler.on_acked(packets[i].stamp, kBytes, packets[i].sent_ns, packets[i].order, now_ns);
    }
    sampler.on_ack_end(min_rtt_ns);
}

} // namespace

TEST_CASE("delivery rate: a paced flight acknowledged one by one samples its sending rate",
          "[net][net-path][delivery-rate]")
{
    DeliveryRateSampler sampler;
    Sent p[4];
    send(sampler, p, 4U, 0U, kMs, 0U);
    CHECK(p[0].stamp.in_flight == kBytes);
    CHECK(p[3].stamp.in_flight == 4U * kBytes);
    CHECK(p[3].stamp.first_sent_ns == 0U);
    CHECK(p[3].stamp.delivered == 0U);

    // The first: nothing was delivered before it, so its interval is its 10 ms round trip.
    ack(sampler, &p[0], 1U, 10U * kMs);
    DeliverySample s = sampler.sample();
    CHECK(s.acked);
    CHECK(s.prior_delivered == 0U);
    CHECK(s.delivered == kBytes);
    CHECK(s.interval_ns == 10U * kMs);
    CHECK(s.rate == kBytes * 100U);
    CHECK(s.newly_acked == kBytes);
    CHECK(s.tx_in_flight == kBytes);
    CHECK(sampler.delivered() == kBytes);

    // The next ones each add a packet over a millisecond, as they left.
    ack(sampler, &p[1], 1U, 11U * kMs);
    s = sampler.sample();
    CHECK(s.delivered == 2U * kBytes);
    CHECK(s.interval_ns == 11U * kMs); // its stamp's delivery time is 0: nothing was delivered when it left
    ack(sampler, &p[2], 1U, 12U * kMs);
    ack(sampler, &p[3], 1U, 13U * kMs);
    s = sampler.sample();
    CHECK(s.prior_delivered == 0U);
    CHECK(s.tx_in_flight == 4U * kBytes);

    // A packet sent after deliveries began measures from the delivery its stamp saw.
    Sent q[1];
    send(sampler, q, 1U, 13U * kMs, 0U, 0U, 10U);
    CHECK(q[0].stamp.delivered == 4U * kBytes);
    CHECK(q[0].stamp.delivered_ns == 13U * kMs);
    ack(sampler, q, 1U, 23U * kMs);
    s = sampler.sample();
    CHECK(s.delivered == kBytes);
    CHECK(s.interval_ns == 10U * kMs);
    CHECK(s.rate == kBytes * 100U);
}

TEST_CASE("delivery rate: the longer of the send and acknowledgement intervals decides", "[net][net-path][delivery-rate]")
{
    // Ten packets sent over 9 ms, all acknowledged in one burst: the rate cannot read faster than they left.
    {
        DeliveryRateSampler sampler;
        Sent warm[1];
        send(sampler, warm, 1U, 0U, 0U, 0U);
        ack(sampler, warm, 1U, 10U * kMs);
        Sent p[10];
        send(sampler, p, 10U, 10U * kMs, kMs, 0U, 1U);
        ack(sampler, p, 10U, 30U * kMs);
        const DeliverySample& s = sampler.sample();
        CHECK(s.newly_acked == 10U * kBytes);
        CHECK(s.delivered == 10U * kBytes);
        CHECK(s.interval_ns == 20U * kMs); // the ACK interval from 10 ms, longer than the 9 ms send interval
        CHECK(s.rate == 10U * kBytes * 50U);
    }
    // ACKs arriving faster than the data left (a shrinking queue): the send interval is the longer one.
    {
        DeliveryRateSampler sampler;
        Sent r[3];
        send(sampler, &r[0], 1U, 0U, 0U, 0U);
        send(sampler, &r[1], 1U, 10U * kMs, 0U, kBytes, 1U);
        ack(sampler, &r[0], 1U, 15U * kMs);
        send(sampler, &r[2], 1U, 20U * kMs, 0U, kBytes, 2U);
        ack(sampler, &r[1], 1U, 25U * kMs);
        ack(sampler, &r[2], 1U, 28U * kMs);
        const DeliverySample& s = sampler.sample();
        CHECK(s.prior_delivered == kBytes);
        CHECK(s.delivered == 2U * kBytes);
        CHECK(s.interval_ns == 20U * kMs); // r[2] left 20 ms after r[0]; its ACK came 13 ms after r[0]'s
        CHECK(s.rate == kBytes * 100U);
    }
}

TEST_CASE("delivery rate: an interval under the min RTT keeps its round but gives no rate",
          "[net][net-path][delivery-rate]")
{
    DeliveryRateSampler sampler;
    Sent p[1];
    send(sampler, p, 1U, 0U, 0U, 0U);
    ack(sampler, p, 1U, 5U * kMs, 8U * kMs);
    const DeliverySample& s = sampler.sample();
    CHECK(s.acked);
    CHECK(s.delivered == kBytes);
    CHECK(s.interval_ns == 5U * kMs);
    CHECK(s.rate == 0U);
    // An ACK frame of nothing in flight is no sample at all.
    sampler.on_ack_begin();
    sampler.on_ack_end(0U);
    CHECK_FALSE(sampler.sample().acked);
    CHECK(sampler.sample().rate == 0U);
}

TEST_CASE("delivery rate: the most recently sent acknowledged packet makes the sample, ties broken by order",
          "[net][net-path][delivery-rate]")
{
    DeliveryRateSampler sampler;
    Sent p[3];
    send(sampler, p, 2U, 0U, 0U, 0U, 5U); // one send time: a coalesced datagram's two packets
    send(sampler, &p[2], 1U, 0U, 0U, 2U * kBytes, 1U);
    sampler.on_ack_begin();
    sampler.on_acked(p[2].stamp, kBytes, p[2].sent_ns, p[2].order, 10U * kMs);
    sampler.on_acked(p[1].stamp, kBytes, p[1].sent_ns, p[1].order, 10U * kMs);
    sampler.on_acked(p[0].stamp, kBytes, p[0].sent_ns, p[0].order, 10U * kMs);
    sampler.on_ack_end(0U);
    // Order 6 beats orders 1 and 5 at the same send time: its stamp's two-packet flight is the sample's.
    CHECK(sampler.sample().tx_in_flight == 2U * kBytes);
    CHECK(sampler.sample().newly_acked == 3U * kBytes);
    // A later frame acknowledging an older packet keeps the newest it saw.
    Sent late[1];
    send(sampler, late, 1U, 20U * kMs, 0U, 0U, 7U);
    sampler.on_ack_begin();
    sampler.on_acked(late[0].stamp, kBytes, late[0].sent_ns, late[0].order, 30U * kMs);
    sampler.on_acked(p[0].stamp, kBytes, p[0].sent_ns, p[0].order, 30U * kMs);
    sampler.on_ack_end(0U);
    CHECK(sampler.sample().prior_delivered == 3U * kBytes);
}

TEST_CASE("delivery rate: losses count from each packet's stamp", "[net][net-path][delivery-rate]")
{
    DeliveryRateSampler sampler;
    Sent p[4];
    send(sampler, p, 4U, 0U, kMs, 0U);
    sampler.on_lost(kBytes); // p[0]
    CHECK(sampler.lost() == kBytes);
    Sent q[1];
    send(sampler, q, 1U, 5U * kMs, 0U, 3U * kBytes, 4U);
    CHECK(q[0].stamp.lost == kBytes);
    sampler.on_lost(kBytes); // p[1]
    ack(sampler, &p[2], 1U, 12U * kMs);
    CHECK(sampler.sample().lost == 2U * kBytes); // both losses came after p[2] left
    ack(sampler, q, 1U, 15U * kMs);
    CHECK(sampler.sample().lost == kBytes); // one came after q left
}

TEST_CASE("delivery rate: an application-limited phase marks samples until its bytes are delivered",
          "[net][net-path][delivery-rate]")
{
    DeliveryRateSampler sampler;
    Sent p[2];
    send(sampler, p, 2U, 0U, kMs, 0U);
    CHECK_FALSE(sampler.app_limited());
    sampler.on_app_limited(2U * kBytes); // two packets out and nothing more to send
    CHECK(sampler.app_limited());
    Sent q[1];
    send(sampler, q, 1U, 3U * kMs, 0U, 2U * kBytes, 2U);
    CHECK(q[0].stamp.app_limited);
    ack(sampler, p, 2U, 10U * kMs);
    CHECK(sampler.app_limited()); // 2400 delivered is not past the 2400 the phase ends after
    CHECK_FALSE(sampler.sample().app_limited);
    ack(sampler, q, 1U, 13U * kMs);
    CHECK_FALSE(sampler.app_limited());
    CHECK(sampler.sample().app_limited);
    // Nothing delivered and nothing in flight still marks a phase.
    DeliveryRateSampler idle;
    idle.on_app_limited(0U);
    CHECK(idle.app_limited());
}

TEST_CASE("delivery rate: an interval never spans an idle period, and a reset starts over",
          "[net][net-path][delivery-rate]")
{
    DeliveryRateSampler sampler;
    Sent p[1];
    send(sampler, p, 1U, 0U, 0U, 0U);
    ack(sampler, p, 1U, 10U * kMs);
    sampler.on_lost(kBytes);
    // A second of idleness, then one packet: its intervals start when it left.
    Sent q[1];
    send(sampler, q, 1U, 1010U * kMs, 0U, 0U, 1U);
    CHECK(q[0].stamp.first_sent_ns == 1010U * kMs);
    CHECK(q[0].stamp.delivered_ns == 1010U * kMs);
    ack(sampler, q, 1U, 1020U * kMs);
    CHECK(sampler.sample().interval_ns == 10U * kMs);
    CHECK(sampler.sample().rate == kBytes * 100U);

    sampler.on_app_limited(0U);
    sampler.reset();
    CHECK(sampler.delivered() == 0U);
    CHECK(sampler.lost() == 0U);
    CHECK_FALSE(sampler.app_limited());
    CHECK_FALSE(sampler.sample().acked);
    // A stamp from before the reset (1200 bytes lost, 1200 delivered) reads no loss and no delivery.
    ack(sampler, q, 1U, 1030U * kMs);
    CHECK(sampler.sample().lost == 0U);
    CHECK(sampler.sample().delivered == 0U);
}
