// crd-net -- GCC (NET-PATH; docs/design/network-foundation.md#gcc, draft-ietf-rmcat-gcc-02 as libwebrtc's send side
// runs it): the policy rules, the start and its pacing; the delay-based rate control (8% a second far from the link's
// capacity, half a packet per response time near it, 0.85 of the acknowledged rate on overuse at most once a clamped
// RTT, the rate halved before any acknowledged rate, held on underuse); the loss controller (following the delay's
// rate in its start phase, 1 - p / 2 past 10% at most once 300 ms and an RTT, holding from 2% to 10%, CE as a loss,
// fractions over 20 packets); the frames' own samples when the peer reports no receive times, a peer clock's offset
// changing nothing; the window, persistent congestion, a Retry, datagram sizes and live swaps; and over a simulated
// bottleneck: a media source near the link's rate in a sawtooth whose queue stays under CUBIC's, a link that halves,
// the same without receive timestamps, random loss, and its share beside CUBIC.

#include "bottleneck_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>

namespace
{
using crd::i64;
using crd::u64;
using crd::usize;
using namespace crd::net;
using crd::net::testing::Flow;
using crd::net::testing::kBottleneckSecond;

constexpr u64 kMs = 1000000U;
constexpr u64 kMss = 1200U;
constexpr u64 kStartRate = 12000U * kBottleneckSecond / kQuicInitialRttNs; // the initial window over 333 ms

CongestionPolicy gcc_policy()
{
    CongestionPolicy policy;
    policy.algorithm = CongestionAlgorithm::Gcc;
    return policy;
}

// A GCC sender driven frame by frame: packets leave at the controller's pacing rate (or every `gap_ns`), cross a path
// of `one_way_ns` each way and a queue that grows by `growth_ns` with each packet (negative: it drains), and every
// `packets` of them are acknowledged in one ACK frame when the last one's acknowledgement arrives. With stamps, each
// carries its receive time on a clock `offset_ns` ahead; the last `lost` of a frame are declared lost and `ce` of its
// packets were marked on an ECN path.
class Driver
{
public:
    explicit Driver(const CongestionPolicy& policy = gcc_policy())
    {
        ctl = make_congestion(policy, kMss, nullptr, m_slot, m_delivery, 1U);
        REQUIRE(ctl != nullptr);
    }
    ~Driver() { ctl->~CongestionController(); }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    struct Frame
    {
        u64 packets = 10U;
        u64 gap_ns = 0U; // 0: the pacing rate's interval
        u64 one_way_ns = 20U * kMs;
        i64 growth_ns = 0;
        u64 lost = 0U;
        u64 ce = 0U;
        bool stamps = true;
        u64 offset_ns = 0U;
    };

    void frame(const Frame& f)
    {
        REQUIRE(f.packets <= kMaxPackets);
        REQUIRE(f.lost <= f.packets);
        u64 last_received = 0U;
        for (u64 i = 0U; i < f.packets; ++i)
        {
            const u64 rate = ctl->pacing_rate(rtt);
            const u64 gap = (f.gap_ns != 0U) ? f.gap_ns : kMss * kBottleneckSecond / ((rate != 0U) ? rate : 1U);
            m_send_ns += gap;
            const i64 queue = static_cast<i64>(m_queue_ns) + f.growth_ns;
            m_queue_ns = (queue > 0) ? static_cast<u64>(queue) : 0U;
            CongestionPacket& p = m_packets[i];
            p = CongestionPacket{};
            p.bytes = kMss;
            p.number = m_number++;
            p.sent_ns = m_send_ns;
            p.space = QuicSpace::Application;
            in_flight += kMss;
            p.delivery = m_delivery.on_sent(kMss, in_flight, m_send_ns);
            ctl->on_sent(p, in_flight, m_send_ns);
            last_received = m_send_ns + f.one_way_ns + m_queue_ns;
            p.received_ns = f.stamps ? last_received + f.offset_ns : kNeverNs;
        }
        const u64 ack_ns = last_received + f.one_way_ns;
        now = (ack_ns > now) ? ack_ns : now;
        m_delivery.on_ack_begin();
        for (u64 i = 0U; i < f.packets; ++i)
        {
            in_flight -= kMss;
            if (i >= f.packets - f.lost)
            {
                m_delivery.on_lost(kMss);
                ctl->on_lost(m_packets[i], now);
                continue;
            }
            m_delivery.on_acked(m_packets[i].delivery, kMss, m_packets[i].sent_ns, m_packets[i].number, now);
            ctl->on_acked(m_packets[i], now);
        }
        const u64 last = f.packets - f.lost - 1U;
        rtt.sample(now - m_packets[last].sent_ns, 0U, true, 0U, now);
        m_delivery.on_ack_end(rtt.min_ns);
        if (f.ce != 0U)
        {
            ctl->on_ecn(f.packets - f.lost, f.ce, m_packets[last].sent_ns, now);
        }
        ctl->on_ack_end(rtt, f.lost < f.packets, in_flight, now);
        m_oldest_sent_ns = m_packets[0].sent_ns;
    }

    // Frames until `until_ns`.
    void run(u64 until_ns, const Frame& f)
    {
        while (now < until_ns)
        {
            frame(f);
        }
    }

    // Frames until the controller reduces, at most `limit_ns` of them.
    [[nodiscard]] bool until_reduced(u64 limit_ns, const Frame& f)
    {
        const u64 before = state().reductions;
        while (now < limit_ns)
        {
            frame(f);
            if (state().reductions > before)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] CongestionState state() const { return ctl->state(); }
    // The largest RTT of the last frame: from its first packet's sending to its acknowledgement.
    [[nodiscard]] u64 frame_rtt() const { return now - m_oldest_sent_ns; }

    CongestionController* ctl = nullptr;
    QuicRtt rtt;
    u64 now = 0U;
    u64 in_flight = 0U;

private:
    static constexpr usize kMaxPackets = 1024U;

    CongestionSlot m_slot;
    DeliveryRateSampler m_delivery;
    std::unique_ptr<CongestionPacket[]> m_packets = std::make_unique<CongestionPacket[]>(kMaxPackets);
    u64 m_number = 0U;
    u64 m_send_ns = 0U;
    u64 m_queue_ns = 0U;
    u64 m_oldest_sent_ns = 0U;
};

Driver::Frame frame_of(u64 packets, u64 lost = 0U, u64 ce = 0U)
{
    Driver::Frame f;
    f.packets = packets;
    f.gap_ns = 2U * kMs; // 40 ms frames, shorter than the 300 ms between loss decreases
    f.lost = lost;
    f.ce = ce;
    return f;
}

Driver::Frame growing(i64 growth_ns, bool stamps = true)
{
    Driver::Frame f;
    f.growth_ns = growth_ns;
    f.stamps = stamps;
    return f;
}

TEST_CASE("GCC: the policy rules, its codepoint, and the start rate, window and pacing", "[net][net-path][gcc]")
{
    CongestionPolicy policy = gcc_policy();
    CHECK(congestion_policy_valid(policy));
    CHECK(congestion_codepoint(policy) == Ecn::Ect0); // CE answers as a loss: a classic sender
    policy.version = 4U;
    CHECK_FALSE(congestion_policy_valid(policy)); // version 5's
    policy = gcc_policy();
    policy.pacing = false;
    CHECK_FALSE(congestion_policy_valid(policy)); // its rate is a pacing rate
    policy = gcc_policy();
    policy.target_rate_min = 60000U;
    policy.target_rate_max = 50000U;
    CHECK_FALSE(congestion_policy_valid(policy));

    Driver d;
    CHECK(d.ctl->algorithm() == CongestionAlgorithm::Gcc);
    const CongestionState start = d.state();
    CHECK(start.window == 12000U);
    CHECK(d.ctl->window() == 12000U);
    CHECK(start.target_rate == kStartRate);
    CHECK(start.bandwidth == kStartRate);
    CHECK(start.phase == CongestionPhase::Avoidance);
    CHECK(start.reductions == 0U);
    CHECK(d.ctl->pacing_rate(d.rtt) == kStartRate * 250U / 100U); // 2.5 times before any feedback
    CHECK(d.ctl->burst() == 12000U);
    d.frame({});
    CHECK(d.ctl->pacing_rate(d.rtt) == d.state().target_rate * 110U / 100U); // 1.1 times after
    CHECK(d.ctl->burst() >= 2U * kMss);

    // The policy's bounds hold the target: the start rate is raised to the minimum and growth stops at the maximum.
    policy = gcc_policy();
    policy.target_rate_min = 50000U;
    policy.target_rate_max = 60000U;
    Driver bounded(policy);
    CHECK(bounded.state().target_rate == 50000U);
    bounded.run(30000U * kMs, {});
    CHECK(bounded.state().target_rate == 60000U);
    CHECK(bounded.state().reductions == 0U);
}

TEST_CASE("GCC: far from the link's capacity the rate grows 8% a second, the loss controller following",
          "[net][net-path][gcc]")
{
    // A steady path: each frame's step is section 5.5's max(rate * (1.08^dt - 1), 1 kbit/s), restated here in double.
    Driver d;
    d.frame({});
    double expected = static_cast<double>(d.state().bandwidth);
    u64 change_ns = d.now;
    CHECK(d.state().bandwidth == kStartRate + 125U); // Hold to Increase: no time has passed, so the floor
    while (d.now < 8000U * kMs) // past that, this bursty sender's acknowledged rate caps it at 1.5 times its own
    {
        d.frame({});
        const double dt = static_cast<double>(d.now - change_ns) / 1e9;
        expected += std::fmax(expected * (std::pow(1.08, std::fmin(dt, 1.0)) - 1.0), 125.0);
        change_ns = d.now;
        REQUIRE(d.state().target_rate <= d.state().bandwidth);
    }
    const double got = static_cast<double>(d.state().bandwidth);
    INFO("after 8 s: " << got << " B/s, restated " << expected);
    CHECK(std::fabs(got - expected) <= expected * 0.005);
    CHECK(got >= static_cast<double>(kStartRate) * 1.6); // 1.08^8 is 1.85
    // Past the start phase the loss controller grows to 1.08 times the last second's least: close behind.
    CHECK(d.state().target_rate * 100U >= d.state().bandwidth * 90U);
    CHECK(d.state().reductions == 0U);
}

TEST_CASE("GCC: an overuse decreases to 0.85 of the acknowledged rate, then holds, then grows additively",
          "[net][net-path][gcc]")
{
    Driver d;
    d.run(3000U * kMs, {});
    const CongestionState before = d.state();
    const u64 pace = d.ctl->pacing_rate(d.rtt); // what the acknowledged rate measures
    REQUIRE(d.until_reduced(6000U * kMs, growing(3000000)));
    const CongestionState reduced = d.state();
    INFO("before " << before.bandwidth << " B/s at a pace of " << pace << ", after " << reduced.bandwidth);
    CHECK(reduced.bandwidth < before.bandwidth);
    CHECK(reduced.bandwidth * 100U >= pace * 85U * 75U / 100U); // the acknowledged rate trails as the queue grows
    CHECK(reduced.bandwidth * 100U <= pace * 85U * 110U / 100U);
    CHECK(reduced.recovery_start_ns == d.now);
    CHECK(reduced.phase == CongestionPhase::Recovery);
    CHECK(reduced.target_rate <= reduced.bandwidth);

    // A continuing overuse never raises the rate, and a further decrease needs the acknowledged rate below it.
    for (int i = 0; i < 20; ++i)
    {
        const u64 rate = d.state().bandwidth;
        d.frame(growing(3000000));
        REQUIRE(d.state().bandwidth <= rate);
    }

    // The queue drains: once the trend reads underuse, the rate holds where it is.
    for (int i = 0; i < 4; ++i)
    {
        d.frame(growing(-3000000));
    }
    const u64 held = d.state().bandwidth;
    d.frame(growing(-3000000));
    d.frame(growing(-3000000));
    CHECK(d.state().bandwidth == held);

    // Once steady again it grows, now additively: half a packet per response time (twice the RTT and 100 ms), far
    // below 8% a second at this rate.
    d.run(d.now + 2000U * kMs, {});
    const u64 from = d.state().bandwidth;
    const u64 from_ns = d.now;
    d.run(from_ns + 1000U * kMs, {});
    const u64 grown = d.state().bandwidth - from;
    const u64 frame_bytes = from / 30U;
    const u64 packet = frame_bytes / ((frame_bytes + 1199U) / 1200U);
    const u64 per_second = packet * kBottleneckSecond / (2U * (d.rtt.smoothed_ns + 100U * kMs));
    const u64 expected = ((per_second > 500U) ? per_second : 500U) * (d.now - from_ns) / kBottleneckSecond;
    INFO("additive growth " << grown << " B/s over " << (d.now - from_ns) << " ns, restated " << expected);
    CHECK(grown * 100U >= expected * 80U);
    CHECK(grown * 100U <= expected * 120U);
}

TEST_CASE("GCC: an overuse before any acknowledged rate halves the rate", "[net][net-path][gcc]")
{
    // Steep growth in the first frames, a packet each 6 ms (a group each), before the acknowledged rate's first
    // 500 ms window closes.
    Driver d;
    Driver::Frame steep = growing(5000000);
    steep.gap_ns = 6U * kMs;
    u64 before = d.state().bandwidth;
    while (d.state().reductions == 0U && d.now < 500U * kMs)
    {
        before = d.state().bandwidth;
        d.frame(steep);
    }
    REQUIRE(d.state().reductions == 1U);
    CHECK(d.state().bandwidth == before / 2U);
    CHECK(d.state().target_rate == before / 2U);
}

TEST_CASE("GCC: the loss controller decreases past 10%, holds from 2% to 10%, grows below, and takes CE as loss",
          "[net][net-path][gcc]")
{
    Driver d;
    d.run(3000U * kMs, frame_of(20U));
    const CongestionState before = d.state();

    // 3 of 20 lost: p = 38 / 256, the target to (512 - 38) / 512 of itself.
    d.frame(frame_of(20U, 3U));
    const CongestionState cut = d.state();
    CHECK(cut.target_rate == before.target_rate * (512U - 38U) / 512U);
    CHECK(cut.reductions == before.reductions + 1U);
    // Again within 300 ms and an RTT: held.
    d.frame(frame_of(20U, 3U));
    CHECK(d.state().target_rate == cut.target_rate);
    CHECK(d.state().reductions == cut.reductions);
    // 1 of 20 (5%): held, however long.
    d.run(d.now + 1000U * kMs, frame_of(20U, 1U));
    CHECK(d.state().target_rate == cut.target_rate);
    // After the interval, CE on 3 of 20 cuts as a loss would.
    d.frame(frame_of(20U, 0U, 3U));
    CHECK(d.state().target_rate == cut.target_rate * (512U - 38U) / 512U);
    CHECK(d.state().reductions == cut.reductions + 1U);
    // A fraction needs 20 packets: 2 lost of 10, twice, is 4 of 20.
    const u64 cut_again = d.state().target_rate;
    d.run(d.now + 1000U * kMs, frame_of(20U, 1U));
    d.frame(frame_of(10U, 2U));
    CHECK(d.state().target_rate == cut_again); // 10 packets: no fraction yet
    d.frame(frame_of(10U, 2U));
    CHECK(d.state().target_rate == cut_again * (512U - 51U) / 512U);
    // No loss: 1.08 times the last second's least and 1 kbit/s.
    const u64 least = d.state().target_rate;
    d.frame(frame_of(20U));
    CHECK(d.state().target_rate == least * 108U / 100U + 125U);
}

TEST_CASE("GCC: with no receive timestamps each frame's RTT sample feeds the trend; a peer clock's offset is nothing",
          "[net][net-path][gcc]")
{
    Driver plain;
    plain.run(3000U * kMs, growing(0, false));
    REQUIRE(plain.until_reduced(8000U * kMs, growing(3000000, false)));
    CHECK(plain.state().bandwidth < plain.state().target_rate * 2U);

    // The same path with stamps on a clock 10^15 ns ahead, and on one in step: the same rates, to the byte.
    Driver::Frame ahead = growing(0);
    ahead.offset_ns = u64{1000000000000000};
    Driver::Frame ahead_growing = growing(3000000);
    ahead_growing.offset_ns = ahead.offset_ns;
    Driver a;
    Driver b;
    a.run(3000U * kMs, ahead);
    b.run(3000U * kMs, growing(0));
    for (int i = 0; i < 100; ++i)
    {
        a.frame(ahead_growing);
        b.frame(growing(3000000));
        REQUIRE(a.state().bandwidth == b.state().bandwidth);
        REQUIRE(a.state().target_rate == b.state().target_rate);
    }
    CHECK(a.state().reductions > 0U);
}

TEST_CASE("GCC: the window holds the target over the RTT and 350 ms; persistent congestion, a Retry, sizes and swaps",
          "[net][net-path][gcc]")
{
    Driver d;
    d.frame({});
    const u64 first = d.state().target_rate * (d.frame_rtt() + 350U * kMs) / kBottleneckSecond;
    CHECK(d.ctl->window() == ((first > 3000U) ? first : 3000U));
    d.run(5000U * kMs, {});
    const CongestionState grown = d.state();
    CHECK(d.ctl->window() >= grown.target_rate * 350U / 1000U);
    CHECK(d.ctl->window() <= grown.target_rate * (d.frame_rtt() + 350U * kMs) / kBottleneckSecond * 6U / 5U);

    d.ctl->on_persistent_congestion(d.now);
    CHECK(d.state().bandwidth == grown.bandwidth / 2U);
    CHECK(d.state().target_rate == grown.target_rate / 2U);
    CHECK(d.ctl->window() == grown.window / 2U);
    CHECK(d.state().reductions == grown.reductions + 1U);

    d.ctl->reset(d.now); // a Retry starts over
    CHECK(d.state().target_rate == kStartRate);
    CHECK(d.ctl->window() == 12000U);
    d.ctl->set_max_datagram(1500U); // at the initial window: the new datagram's
    CHECK(d.ctl->window() == 14720U);

    // Swapped in from CUBIC: the window carries over and the rate is that window per the initial RTT.
    CongestionState cubic;
    cubic.window = 333000U;
    CongestionSlot slot;
    DeliveryRateSampler delivery;
    CongestionController* swapped = make_congestion(gcc_policy(), kMss, &cubic, slot, delivery, 1U);
    REQUIRE(swapped != nullptr);
    CHECK(swapped->window() == 333000U);
    CHECK(swapped->state().target_rate == 1000000U);
    // A target rate carries over as it is, and GCC's state seeds a CUBIC with its window.
    CongestionState media;
    media.window = 30000U;
    media.target_rate = 70000U;
    swapped->~CongestionController();
    swapped = make_congestion(gcc_policy(), kMss, &media, slot, delivery, 1U);
    REQUIRE(swapped != nullptr);
    CHECK(swapped->state().target_rate == 70000U);
    const CongestionState held = swapped->state();
    swapped->~CongestionController();
    swapped = make_congestion(CongestionPolicy{}, kMss, &held, slot, delivery, 1U);
    REQUIRE(swapped != nullptr);
    CHECK(swapped->window() == held.window);
    swapped->~CongestionController();
}

// ---- Over a simulated bottleneck ----

constexpr u64 kRate = 1250000U; // 10 Mbit/s

struct Settled
{
    u64 target = 0U;
    u64 goodput = 0U;
    u64 max_queue = 0U;
    u64 mean_queue = 0U; // over the ACK frames
    u64 dropped = 0U;
    u64 reductions = 0U;
};

// Runs `flow` for `warm_ms`, then measures `measure_ms`.
Settled settle(Flow& flow, u64 warm_ms, u64 measure_ms)
{
    flow.run(flow.now + warm_ms * kMs);
    const u64 start = flow.now;
    const u64 acked = flow.acked_bytes;
    const u64 dropped = flow.dropped;
    const u64 reductions = flow.ctl->state().reductions;
    Settled out;
    u64 sum = 0U;
    u64 frames = 0U;
    flow.run(start + measure_ms * kMs,
             [&]
             {
                 const u64 queue = flow.queue_bytes();
                 out.max_queue = (queue > out.max_queue) ? queue : out.max_queue;
                 sum += queue;
                 ++frames;
             });
    out.target = flow.ctl->state().target_rate;
    out.goodput = (flow.acked_bytes - acked) * kBottleneckSecond / (measure_ms * kMs);
    out.mean_queue = (frames != 0U) ? sum / frames : 0U;
    out.dropped = flow.dropped - dropped;
    out.reductions = flow.ctl->state().reductions - reductions;
    return out;
}

TEST_CASE("GCC: a media source runs near the link's rate in a sawtooth whose queue stays under CUBIC's",
          "[net][net-path][gcc]")
{
    // 10 Mbit/s, 40 ms round trip, half a second of buffer. From 288 kbit/s at 8% a second the climb takes about a
    // minute (libwebrtc probes; this GCC does not yet). Near the capacity it learned, it grows half a packet per
    // response time, and an excess under 2.5% of the link stays below the detector's least threshold (6 over 60
    // deltas and a gain of 4) while the queue builds: a slow sawtooth, measured over two minutes.
    Flow flow(gcc_policy(), kRate, 20U * kMs, kRate / 2U);
    flow.set_media(20U * kMs);
    const Settled gcc = settle(flow, 60000U, 120000U);
    Flow cubic(CongestionPolicy{}, kRate, 20U * kMs, kRate / 2U);
    const Settled bulk = settle(cubic, 30000U, 30000U);
    INFO("GCC target " << gcc.target << " B/s, goodput " << gcc.goodput << ", queue " << gcc.mean_queue << " B mean, "
                       << gcc.max_queue << " B at most, dropped " << gcc.dropped << ", reductions " << gcc.reductions
                       << "; CUBIC queue " << bulk.mean_queue << " B mean, " << bulk.max_queue << " B at most");
    CHECK(gcc.dropped == 0U);
    CHECK(gcc.reductions >= 1U);
    CHECK(gcc.goodput >= kRate * 85U / 100U);
    CHECK(gcc.mean_queue * 4U <= bulk.mean_queue);
    CHECK(gcc.max_queue < bulk.max_queue);
}

TEST_CASE("GCC: a media source follows a link that halves, its queue drained", "[net][net-path][gcc]")
{
    Flow flow(gcc_policy(), kRate, 20U * kMs, kRate / 2U);
    flow.set_media(20U * kMs);
    flow.run(90000U * kMs);
    flow.set_rate(kRate / 2U);
    const Settled after = settle(flow, 5000U, 20000U);
    INFO("after the drop: target " << after.target << " B/s, goodput " << after.goodput << ", queue "
                                   << after.mean_queue << " B mean, " << after.max_queue << " B at most");
    CHECK(after.target <= kRate / 2U * 110U / 100U);
    CHECK(after.goodput >= kRate / 2U * 70U / 100U);
    CHECK(after.mean_queue <= kRate / 2U * 60U / 1000U);
    CHECK(flow.dropped == 0U);
}

TEST_CASE("GCC: without receive timestamps, or on a peer clock far ahead, the bottleneck runs the same",
          "[net][net-path][gcc]")
{
    // Only the forward queue varies here and each packet has its own ACK frame, so a frame's sample (sent time and
    // RTT less ACK delay) is its receive time plus the return path: the same deltas, the same rates to the byte.
    const auto run = [](bool stamps, u64 offset_ns)
    {
        Flow flow(gcc_policy(), kRate, 20U * kMs, kRate / 2U);
        flow.set_timestamps(stamps, offset_ns);
        flow.set_media(20U * kMs);
        return settle(flow, 60000U, 60000U);
    };
    const Settled stamped = run(true, 0U);
    const Settled plain = run(false, 0U);
    const Settled ahead = run(true, u64{1000000000000000000}); // whole microseconds: the trend's unit
    INFO("stamped: target " << stamped.target << " B/s, goodput " << stamped.goodput << "; without: target "
                            << plain.target << ", goodput " << plain.goodput);
    CHECK(plain.target == stamped.target);
    CHECK(plain.goodput == stamped.goodput);
    CHECK(plain.max_queue == stamped.max_queue);
    CHECK(ahead.target == stamped.target);
    CHECK(ahead.max_queue == stamped.max_queue);
}

TEST_CASE("GCC: random loss under 2% leaves the rate alone; 15% takes it down", "[net][net-path][gcc]")
{
    Flow light(gcc_policy(), kRate, 20U * kMs, kRate / 2U);
    light.set_loss(10U); // 1%
    light.set_media(20U * kMs);
    const Settled one = settle(light, 60000U, 60000U);
    Flow heavy(gcc_policy(), kRate, 20U * kMs, kRate / 2U);
    heavy.set_loss(150U);
    heavy.set_media(20U * kMs);
    const Settled fifteen = settle(heavy, 60000U, 30000U);
    INFO("1% loss: target " << one.target << " B/s, goodput " << one.goodput << "; 15%: target " << fifteen.target);
    CHECK(one.goodput >= kRate * 70U / 100U);
    CHECK(fifteen.target <= kRate / 10U);
}

TEST_CASE("GCC: beside CUBIC in a deep drop-tail queue a delay-based controller yields", "[net][net-path][gcc]")
{
    // No compete switch as SCReAM's: the delay CUBIC builds reads as overuse, so GCC's share is small. Recorded, not
    // tuned: libwebrtc's GCC does the same, and a call beside a bulk download is what SCReAM's switch is for.
    crd::net::testing::Link link{kRate, kRate / 4U};
    Flow a(gcc_policy(), link, 20U * kMs, 7U);
    Flow b(CongestionPolicy{}, link, 20U * kMs, 11U);
    a.set_media(20U * kMs);
    crd::net::testing::run_flows({&a, &b}, 60000U * kMs, [] {});
    const u64 acked_a = a.acked_bytes;
    const u64 acked_b = b.acked_bytes;
    crd::net::testing::run_flows({&a, &b}, 90000U * kMs, [] {});
    const u64 got_a = a.acked_bytes - acked_a;
    const u64 got_b = b.acked_bytes - acked_b;
    INFO("GCC " << got_a << " B, CUBIC " << got_b << " B, GCC's target " << a.ctl->state().target_rate);
    CHECK(got_a + got_b >= kRate * 30U * 90U / 100U);
    CHECK(got_a > 0U);
}

} // namespace
