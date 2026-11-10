// crd-net -- SCReAM v2 (NET-PATH; docs/design/network-foundation.md#scream, draft-ietf-ccwg-rfc8298bis-screamv2-03):
// the policy rules and codepoints; the reference window's start, its room in flight and the target rate and pacing
// that follow it; growth bounded by the flight and stopped at the maximum rate, with relaxed pacing there; a loss
// taking 0.7 and classic CE 0.8 at most once a smoothed RTT; L4S CE by alpha / 2; a queue past half the delay target
// reducing in proportion while a short one never does; link losses tolerated below a 1% rate; a risen base delay
// taken as the new base with the rate halved meanwhile; live swaps and a Retry; and over a simulated bottleneck: a
// bulk sender holding a deep drop-tail queue near its target where CUBIC fills it, a media source following a link
// that halves, an L4S step, and a share beside CUBIC that the delay target's adjustment keeps.

#include "bottleneck_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
using crd::u64;
using crd::usize;
using namespace crd::net;
using crd::net::testing::Flow;
using crd::net::testing::kBottleneckSecond;

constexpr u64 kMs = 1000000U;
constexpr u64 kMss = 1200U;

CongestionPolicy scream_policy()
{
    CongestionPolicy policy;
    policy.algorithm = CongestionAlgorithm::Scream;
    return policy;
}

// A SCReAM sender driven round by round: a round sends `packets` datagrams (0: what the reference window holds) and
// acknowledges them `rtt_ns` later in one ACK frame with its RTT sample and, on an ECN path, `ce` of them marked; the
// last `lost` of them are declared lost instead.
class Driver
{
public:
    explicit Driver(const CongestionPolicy& policy = scream_policy())
    {
        ctl = make_congestion(policy, kMss, nullptr, m_slot, m_delivery, 1U);
        REQUIRE(ctl != nullptr);
    }
    ~Driver() { ctl->~CongestionController(); }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    struct Round
    {
        u64 rtt_ns = 20U * kMs;
        u64 packets = 0U;
        u64 lost = 0U;
        u64 ce = 0U;
        bool ecn = false;
    };

    void round(const Round& r)
    {
        const u64 window = (ctl->state().window / kMss > 0U) ? ctl->state().window / kMss : 1U;
        const u64 n = (r.packets != 0U) ? r.packets : window;
        REQUIRE(n <= kMaxPackets);
        REQUIRE(r.lost <= n);
        now += kSendGapNs;
        const u64 first_sent = now;
        for (u64 i = 0U; i < n; ++i)
        {
            CongestionPacket& p = m_packets[i];
            p = CongestionPacket{};
            p.bytes = kMss;
            p.number = m_number++;
            p.sent_ns = now;
            p.space = QuicSpace::Application;
            in_flight += kMss;
            p.delivery = m_delivery.on_sent(kMss, in_flight, now);
            ctl->on_sent(p, in_flight, now);
        }
        now = first_sent + r.rtt_ns;
        m_delivery.on_ack_begin();
        for (u64 i = 0U; i < n; ++i)
        {
            in_flight -= kMss;
            if (i >= n - r.lost)
            {
                m_delivery.on_lost(kMss);
                ctl->on_lost(m_packets[i], now);
                continue;
            }
            m_delivery.on_acked(m_packets[i].delivery, kMss, m_packets[i].sent_ns, m_packets[i].number, now);
            ctl->on_acked(m_packets[i], now);
        }
        rtt.sample(r.rtt_ns, 0U, true, 0U, now);
        m_delivery.on_ack_end(rtt.min_ns);
        if (r.ecn)
        {
            ctl->on_ecn(n - r.lost, r.ce, m_packets[n - 1U].sent_ns, now);
        }
        ctl->on_ack_end(rtt, true, in_flight, now);
    }

    void rounds(usize count, const Round& r)
    {
        for (usize i = 0U; i < count; ++i)
        {
            round(r);
        }
    }

    [[nodiscard]] u64 ref() const { return ctl->state().window; }

    // A round of `packets` (0: the reference window's) with `lost` of them lost and `ce` marked on an ECN path.
    [[nodiscard]] static Round of(u64 packets, u64 lost = 0U, u64 ce = 0U, bool ecn = false, u64 rtt_ns = 20U * kMs)
    {
        Round r;
        r.packets = packets;
        r.lost = lost;
        r.ce = ce;
        r.ecn = ecn;
        r.rtt_ns = rtt_ns;
        return r;
    }

    CongestionController* ctl = nullptr;
    QuicRtt rtt;
    u64 now = 0U;
    u64 in_flight = 0U;

private:
    static constexpr usize kMaxPackets = 8192U;
    static constexpr u64 kSendGapNs = 1000U;

    CongestionSlot m_slot;
    DeliveryRateSampler m_delivery;
    std::unique_ptr<CongestionPacket[]> m_packets = std::make_unique<CongestionPacket[]>(kMaxPackets);
    u64 m_number = 0U;
};

// The target rate section 4.4 gives a reference window of `ref` bytes over a smoothed RTT of `srtt_ns`: the window per
// RTT, less min(0.2, max(0, MSS / ref - 0.1)), times MSS / (MSS + 20), over 1.2.
u64 expected_target(u64 ref, u64 srtt_ns)
{
    const u64 ratio = (kMss << 16U) / ref;
    const u64 tenth = (u64{1} << 16U) / 10U;
    const u64 fifth = (u64{1} << 16U) / 5U;
    const u64 over = (ratio > tenth) ? ratio - tenth : 0U;
    const u64 small = (over < fifth) ? over : fifth;
    u64 rate = ref * kBottleneckSecond / srtt_ns;
    rate = rate * ((u64{1} << 16U) - small) >> 16U;
    return rate * (kMss * 100U) / ((kMss + 20U) * 120U);
}

struct FlowResult
{
    u64 goodput = 0U;   // bytes per second over the measured span
    u64 max_queue = 0U; // bytes, over the measured span
    u64 dropped = 0U;   // over the measured span
    CongestionState state;
};

// A flow alone through a link of `rate` with a `buffer` byte drop-tail queue (CE past `ce_queue` bytes when set),
// `rtt_ms` of round trip, `warm_ms` to settle and then `measure_ms`.
FlowResult run_alone(const CongestionPolicy& policy, u64 rate, u64 rtt_ms, u64 buffer, u64 warm_ms, u64 measure_ms,
                     u64 ce_queue = 0U)
{
    Flow flow(policy, rate, rtt_ms * kMs / 2U, buffer);
    flow.set_ce_queue(ce_queue);
    flow.run(warm_ms * kMs);
    const u64 start = flow.now;
    const u64 acked = flow.acked_bytes;
    const u64 dropped = flow.dropped;
    FlowResult out;
    flow.run(start + measure_ms * kMs,
             [&]
             {
                 const u64 queue = flow.queue_bytes();
                 out.max_queue = (queue > out.max_queue) ? queue : out.max_queue;
             });
    out.goodput = (flow.acked_bytes - acked) * kBottleneckSecond / (measure_ms * kMs);
    out.dropped = flow.dropped - dropped;
    out.state = flow.ctl->state();
    return out;
}

} // namespace

TEST_CASE("SCReAM: the policy rules and its codepoints", "[net][net-path][scream]")
{
    CongestionPolicy policy = scream_policy();
    CHECK(congestion_policy_valid(policy));
    CHECK(congestion_codepoint(policy) == Ecn::Ect0);
    policy.scalable_ecn = true;
    CHECK(congestion_codepoint(policy) == Ecn::Ect1);
    const auto refused = [](void (*change)(CongestionPolicy&))
    {
        CongestionPolicy p = scream_policy();
        change(p);
        return !congestion_policy_valid(p);
    };
    CHECK(refused([](CongestionPolicy& p) { p.pacing = false; })); // section 4.3.2
    CHECK(refused([](CongestionPolicy& p) { p.version = 3U; }));   // interface version 4's
    CHECK(refused([](CongestionPolicy& p) { p.queue_delay_target_ns = kMs - 1U; }));
    CHECK(refused([](CongestionPolicy& p) { p.queue_delay_target_ns = kScreamQueueDelayTargetMaxNs + 1U; }));
    CHECK(refused(
        [](CongestionPolicy& p)
        {
            p.target_rate_min = 2000U;
            p.target_rate_max = 1000U;
        }));
    policy = scream_policy();
    policy.target_rate_min = 1000U;
    policy.target_rate_max = 1000U;
    policy.queue_delay_target_ns = kScreamQueueDelayTargetMaxNs;
    CHECK(congestion_policy_valid(policy));
    policy.target_rate_max = 0U; // no maximum
    policy.target_rate_min = 1000000U;
    CHECK(congestion_policy_valid(policy));
}

TEST_CASE("SCReAM: the reference window's start, its room in flight, and the target rate and pacing it gives",
          "[net][net-path][scream]")
{
    Driver d;
    CHECK(d.ctl->algorithm() == CongestionAlgorithm::Scream);
    const CongestionState start = d.ctl->state();
    CHECK(start.window == 12000U); // RFC 9002's initial window, above MIN_REF_WND
    CHECK(start.slow_start_threshold == start.window);
    CHECK(start.phase == CongestionPhase::Avoidance);
    CHECK(start.queue_delay_target_ns == kScreamQueueDelayTargetNs);
    CHECK(d.ctl->window() == 3U * 12000U); // REF_WND_OVERHEAD_MAX while the delay is steady
    CHECK(start.target_rate == expected_target(12000U, kQuicInitialRttNs));
    CHECK(d.ctl->pacing_rate(QuicRtt{}) == start.target_rate * 3U / 2U);
    CHECK(d.ctl->burst() == 12000U); // before a round trip is known, the initial window

    // A short window still keeps two datagrams in flight, and MIN_REF_WND holds against a smaller policy minimum.
    CongestionPolicy small = scream_policy();
    small.initial_packets = 2U;
    Driver tiny(small);
    CHECK(tiny.ref() == 3000U);
    CHECK(tiny.ctl->window() == 9000U);

    // Uncongested rounds grow the window; the target follows the window and the smoothed RTT.
    d.rounds(40U, {});
    const CongestionState grown = d.ctl->state();
    CHECK(grown.window > start.window);
    CHECK(grown.reductions == 0U);
    CHECK(grown.queue_delay_ns == 0U);
    CHECK(grown.target_rate == expected_target(grown.window, d.rtt.smoothed_ns));
    CHECK(d.ctl->pacing_rate(d.rtt) == grown.target_rate * 3U / 2U);
    CHECK(d.ctl->burst() >= 2U * kMss);
}

TEST_CASE("SCReAM: growth stays within the flight's headroom and stops at the maximum rate, where pacing relaxes",
          "[net][net-path][scream]")
{
    // A source sending a fixed four datagrams a round grows the reference window from MIN_REF_WND up to MSS + 1.5
    // flights and no further; a window already past that bound does not grow at all.
    CongestionPolicy small = scream_policy();
    small.initial_packets = 2U;
    Driver d(small);
    d.rounds(200U, Driver::of(4U));
    CHECK(d.ref() > 3000U);
    CHECK(d.ref() <= kMss + 4U * kMss * 3U / 2U);
    Driver wide;
    wide.rounds(200U, Driver::of(4U));
    CHECK(wide.ref() == 12000U);

    // A maximum rate stops growth once the target meets it; pacing then relaxes up to four times.
    CongestionPolicy capped = scream_policy();
    capped.target_rate_min = 150000U;
    capped.target_rate_max = 200000U;
    Driver c(capped);
    c.rounds(400U, {});
    const CongestionState held = c.ctl->state();
    CHECK(held.target_rate == capped.target_rate_max);
    const u64 window = held.window;
    c.rounds(50U, {});
    CHECK(c.ref() == window);
    CHECK(c.ctl->pacing_rate(c.rtt) == capped.target_rate_max * 3U / 2U * 4U);
    // The minimum holds the target up when the window per RTT falls below it.
    c.ctl->on_persistent_congestion(c.now);
    CHECK(c.ref() == 3000U);
    CHECK(c.ctl->state().target_rate == capped.target_rate_min);
}

TEST_CASE("SCReAM: a loss takes 0.7 and classic CE 0.8, at most once a smoothed RTT", "[net][net-path][scream]")
{
    Driver d;
    d.rounds(60U, {});
    const u64 before = d.ref();
    const u64 reductions = d.ctl->state().reductions;
    d.round(Driver::of(0U, 1U));
    const u64 after_loss = d.ref();
    CHECK(d.ctl->state().reductions == reductions + 1U);
    CHECK(after_loss >= before * 45875U / 65536U); // BETA_LOSS, then this frame's growth after it
    CHECK(after_loss <= before * 45875U / 65536U + 2U * kMss);
    // A second loss in the same smoothed RTT, of a packet sent after the reaction: held until an RTT passes.
    d.now += kMs;
    CongestionPacket late;
    late.bytes = kMss;
    late.number = 1000000U;
    late.sent_ns = d.now;
    late.space = QuicSpace::Application;
    d.ctl->on_lost(late, d.now);
    QuicRtt rtt = d.rtt;
    d.ctl->on_ack_end(rtt, false, d.in_flight, d.now);
    CHECK(d.ctl->state().reductions == reductions + 1U);
    d.round({}); // a round trip later the held loss reacts
    CHECK(d.ctl->state().reductions == reductions + 2U);

    // Classic CE on an ECN path takes BETA_ECN, 0.8.
    Driver e;
    e.rounds(60U, Driver::of(0U, 0U, 0U, true));
    const u64 marked_before = e.ref();
    e.round(Driver::of(0U, 0U, 1U, true));
    CHECK(e.ref() >= marked_before * 52429U / 65536U);
    CHECK(e.ref() <= marked_before * 52429U / 65536U + 2U * kMss);
    CHECK(e.ctl->state().alpha > 0U);
}

TEST_CASE("SCReAM: L4S CE reduces by alpha / 2, alpha moving by 1/16 toward the marked fraction",
          "[net][net-path][scream]")
{
    CongestionPolicy policy = scream_policy();
    policy.scalable_ecn = true;
    Driver d(policy);
    d.rounds(60U, Driver::of(20U, 0U, 0U, true));
    CHECK(d.ctl->state().alpha == 0U);
    // Every round marks half its packets: alpha climbs toward 0.5 by 1/16 a round (each round is past 10 ms).
    u64 alpha = 0U;
    for (int i = 0; i < 8; ++i)
    {
        d.round(Driver::of(20U, 0U, 10U, true));
        const u64 next = d.ctl->state().alpha;
        CHECK(next == alpha - alpha / 16U + (kCongestionAlphaOne / 2U) / 16U);
        alpha = next;
    }
    // Each round reduces once, by at most alpha / 2 (less near the last maximum), gentler than classic's 0.8.
    const u64 before = d.ref();
    const u64 reductions = d.ctl->state().reductions;
    d.round(Driver::of(20U, 0U, 10U, true));
    const u64 now_alpha = d.ctl->state().alpha;
    CHECK(d.ctl->state().reductions == reductions + 1U);
    CHECK(d.ref() >= before - before * now_alpha / (2U * kCongestionAlphaOne));
    CHECK(d.ref() > before * 52429U / 65536U);
}

TEST_CASE("SCReAM: a queue past half the delay target reduces in proportion; a shorter one never does",
          "[net][net-path][scream]")
{
    // 20 ms of base and 25 ms of queue: under the 30 ms half target, no reduction in 300 rounds.
    Driver calm;
    calm.rounds(20U, {});
    calm.rounds(150U, Driver::of(0U, 0U, 0U, false, 45U * kMs));
    CHECK(calm.ctl->state().reductions == 0U);
    CHECK(calm.ctl->state().queue_delay_ns >= 24U * kMs); // the slow attack's average, from below
    CHECK(calm.ctl->state().queue_delay_ns <= 25U * kMs);
    // 40 ms and 55 ms of queue reduce, the deeper by more: (q - 30) / 30 / 2 of the window each smoothed RTT.
    u64 kept[2] = {};
    const u64 queues[2] = {40U, 55U};
    for (usize i = 0U; i < 2U; ++i)
    {
        Driver d;
        d.rounds(100U, {});
        const u64 before = d.ref();
        d.rounds(20U, Driver::of(0U, 0U, 0U, false, (20U + queues[i]) * kMs));
        CHECK(d.ctl->state().reductions > 0U);
        kept[i] = d.ref() * 1000U / before;
    }
    CHECK(kept[1] < kept[0]);
}

TEST_CASE("SCReAM: tolerating link loss, a lone loss with a short queue keeps the window; a 1% rate reduces",
          "[net][net-path][scream]")
{
    CongestionPolicy tolerant = scream_policy();
    tolerant.tolerate_link_loss = true;
    Driver d(tolerant);
    d.rounds(60U, {});
    d.round(Driver::of(0U, 1U));
    CHECK(d.ctl->state().reductions == 0U);
    // Losses past LOSS_RATE_THRESHOLD's average do reduce.
    for (int i = 0; i < 20 && d.ctl->state().reductions == 0U; ++i)
    {
        d.round(Driver::of(50U, 5U));
    }
    CHECK(d.ctl->state().reductions > 0U);
    // So does a lone loss while the queue builds past a quarter of the target.
    Driver q(tolerant);
    q.rounds(60U, {});
    // 20 ms queued, under half the target: no reduction of its own.
    q.rounds(30U, Driver::of(0U, 0U, 0U, false, 40U * kMs));
    CHECK(q.ctl->state().reductions == 0U);
    q.round(Driver::of(0U, 1U, 0U, false, 40U * kMs));
    CHECK(q.ctl->state().reductions == 1U);
}

TEST_CASE("SCReAM: a base delay that rose is taken as the new base, the target rate halved meanwhile",
          "[net][net-path][scream]")
{
    // The path's propagation delay rises from 20 to 60 ms (a route change): the queue delay reads 40 ms until the
    // long average of each round's least queue delay passes a quarter of the target, then the base starts over.
    Driver d;
    d.rounds(100U, {});
    bool halved = false;
    bool rebased = false;
    for (int i = 0; i < 2000 && !rebased; ++i)
    {
        d.round(Driver::of(0U, 0U, 0U, false, 60U * kMs));
        const CongestionState state = d.ctl->state();
        if (state.queue_delay_ns < 30U * kMs && i > 10)
        {
            rebased = true;
            halved = state.target_rate <= expected_target(state.window, d.rtt.smoothed_ns) / 2U + 1U;
        }
    }
    CHECK(rebased);
    CHECK(halved);
    // Past the hold, the target is the window's again.
    d.rounds(30U, Driver::of(0U, 0U, 0U, false, 60U * kMs));
    CHECK(d.ctl->state().target_rate == expected_target(d.ref(), d.rtt.smoothed_ns));
    CHECK(d.ctl->state().queue_delay_ns <= kMs);
}

TEST_CASE("SCReAM: live swaps keep the window both ways; a Retry and a datagram size change start it over",
          "[net][net-path][scream]")
{
    Flow flow(CongestionPolicy{}, 2000000U, 10U * kMs, 400000U);
    flow.run(2000U * kMs);
    const CongestionState cubic = flow.ctl->state();
    flow.swap(scream_policy());
    CHECK(flow.ctl->algorithm() == CongestionAlgorithm::Scream);
    CHECK(flow.ctl->state().window == cubic.window);
    CHECK(flow.ctl->state().reductions == cubic.reductions);
    CHECK(flow.ctl->state().recovery_start_ns == cubic.recovery_start_ns);
    flow.run(4000U * kMs);
    const CongestionState scream = flow.ctl->state();
    flow.swap(CongestionPolicy{});
    CHECK(flow.ctl->state().window == scream.window);
    CHECK(flow.ctl->state().slow_start_threshold == scream.window);
    CHECK(flow.ctl->state().phase == CongestionPhase::Avoidance);

    Driver d;
    d.rounds(50U, {});
    const u64 grown = d.ref();
    CHECK(grown > 12000U);
    d.ctl->set_max_datagram(1500U); // a grown window is kept
    CHECK(d.ref() == grown);
    d.ctl->reset(d.now); // a Retry: the initial window of 1500-byte datagrams
    CHECK(d.ref() == 14720U);
    d.ctl->set_max_datagram(1200U); // at the initial window: the new datagram's
    CHECK(d.ref() == 12000U);
}

TEST_CASE("SCReAM: a bulk sender holds a deep drop-tail queue near its delay target, where CUBIC fills it",
          "[net][net-path][scream]")
{
    // 10 Mbit/s, 40 ms round trip, half a second of buffer (bufferbloat), measured after 30 s.
    static constexpr u64 kRate = 1250000U;
    static constexpr u64 kBuffer = kRate / 2U;
    const FlowResult scream = run_alone(scream_policy(), kRate, 40U, kBuffer, 30000U, 30000U);
    const FlowResult cubic = run_alone(CongestionPolicy{}, kRate, 40U, kBuffer, 30000U, 30000U);
    const u64 target_bytes = kRate * kScreamQueueDelayTargetNs / kBottleneckSecond;
    INFO("SCReAM " << scream.goodput << " B/s, queue " << scream.max_queue << " (" << scream.state.queue_delay_ns
                   << " ns averaged), reductions " << scream.state.reductions << "; CUBIC " << cubic.goodput
                   << " B/s, queue " << cubic.max_queue << ", dropped " << cubic.dropped);
    CHECK(scream.dropped == 0U);
    CHECK(scream.max_queue <= target_bytes);
    CHECK(scream.goodput >= kRate * 90U / 100U);
    CHECK(cubic.max_queue >= 4U * scream.max_queue);
}

TEST_CASE("SCReAM: a media source follows a link that halves, its queue drained within seconds",
          "[net][net-path][scream]")
{
    // The source's target settles at the link's rate, a little over or under it as the queue moves around half the
    // delay target, and its queue peaks a feedback loop's lag past the target: the average reacts, not the sample.
    static constexpr u64 kRate = 1250000U;
    static constexpr u64 kTargetQueue = kRate * kScreamQueueDelayTargetNs / kBottleneckSecond;
    Flow flow(scream_policy(), kRate, 20U * kMs, kRate / 2U);
    flow.set_media(20U * kMs); // a frame each 20 ms of the target rate
    flow.run(20000U * kMs);
    u64 settled_queue = 0U;
    flow.run(40000U * kMs,
             [&] { settled_queue = (flow.queue_bytes() > settled_queue) ? flow.queue_bytes() : settled_queue; });
    const CongestionState settled = flow.ctl->state();
    INFO("settled target " << settled.target_rate << " B/s, queue delay " << settled.queue_delay_ns << " ns, queue "
                           << settled_queue << " B, backlog " << flow.max_backlog);
    CHECK(settled.target_rate >= kRate * 60U / 100U);
    CHECK(settled.target_rate <= kRate * 110U / 100U);
    CHECK(settled_queue <= kTargetQueue * 5U / 4U);
    CHECK(flow.dropped == 0U);
    // The link halves: within 5 s the target falls to the new rate and the queue to the same bound.
    flow.set_rate(kRate / 2U);
    flow.max_backlog = 0U;
    const u64 halved_at = flow.now;
    flow.run(halved_at + 5000U * kMs);
    u64 max_queue = 0U;
    flow.run(halved_at + 20000U * kMs,
             [&] { max_queue = (flow.queue_bytes() > max_queue) ? flow.queue_bytes() : max_queue; });
    const CongestionState after = flow.ctl->state();
    INFO("after the drop: target " << after.target_rate << " B/s, queue " << max_queue << " B, backlog "
                                   << flow.max_backlog);
    CHECK(after.target_rate <= kRate / 2U * 110U / 100U);
    CHECK(after.target_rate >= kRate / 2U * 60U / 100U);
    CHECK(max_queue <= kTargetQueue / 2U * 5U / 4U);
    CHECK(flow.dropped == 0U);
}

TEST_CASE("SCReAM: in L4S mode an L4S step keeps the queue near the step", "[net][net-path][scream]")
{
    static constexpr u64 kRate = 1250000U;
    static constexpr u64 kStep = kRate / 1000U; // 1 ms
    CongestionPolicy policy = scream_policy();
    policy.scalable_ecn = true;
    const FlowResult l4s = run_alone(policy, kRate, 40U, kRate / 2U, 30000U, 30000U, kStep);
    INFO("L4S SCReAM " << l4s.goodput << " B/s, queue " << l4s.max_queue << ", alpha " << l4s.state.alpha);
    CHECK(l4s.dropped == 0U);
    CHECK(l4s.goodput >= kRate * 85U / 100U);
    CHECK(l4s.max_queue <= kStep + 4U * kMss);
}

TEST_CASE("SCReAM: beside CUBIC in a deep drop-tail queue the delay target's adjustment keeps a share",
          "[net][net-path][scream]")
{
    static constexpr u64 kRate = 1250000U;
    const auto share = [](bool compete)
    {
        crd::net::testing::Link link{kRate, kRate / 4U};
        CongestionPolicy scream = scream_policy();
        scream.compete = compete;
        Flow a(scream, link, 20U * kMs, 7U);
        Flow b(CongestionPolicy{}, link, 20U * kMs, 11U);
        crd::net::testing::run_flows({&a, &b}, 30000U * kMs, [] {});
        const u64 acked_a = a.acked_bytes;
        const u64 acked_b = b.acked_bytes;
        crd::net::testing::run_flows({&a, &b}, 60000U * kMs, [] {});
        const u64 got_a = a.acked_bytes - acked_a;
        const u64 got_b = b.acked_bytes - acked_b;
        INFO("compete " << compete << ": SCReAM " << got_a << " B, CUBIC " << got_b << " B, delay target "
                        << a.ctl->state().queue_delay_target_ns);
        return got_a * 1000U / (got_a + got_b);
    };
    const u64 adjusted = share(true);
    const u64 fixed = share(false);
    INFO("SCReAM's share in permille: " << adjusted << " adjusting its target, " << fixed << " with it fixed");
    CHECK(adjusted >= 150U);
    CHECK(adjusted > 2U * fixed);
}
