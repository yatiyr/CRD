// crd-net -- Prague (NET-PATH; docs/design/network-foundation.md#prague, draft-briscoe-iccrg-prague-congestion-
// control-04, RFC 9331 section 4.3): the policy rules and ECT(1); the first CE leaving slow start by half (alpha
// starts at 1); CE then taking the window down by alpha / 2 once a round trip, alpha the moving fraction of marked
// packets with gain 1/16 against the real-valued average; marked bytes growing nothing; a loss halving the window,
// from where it stood before a mark's reduction within that round trip; growth independent of the round trip after
// 500 rounds, with alpha and CWR on the 25 ms virtual round trip, and Reno's growth on a path without ECN; a classic
// sawtooth queue with marks turning the reduction to ABE's; a Retry starting over; and over a simulated bottleneck
// with an L4S step, a full link and a queue near the step where CUBIC and NewReno leave the link short.

#include "bottleneck_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <utility>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using namespace crd::net;
using crd::net::testing::Path;

constexpr u64 kMs = 1000000U;
constexpr u64 kMss = 1200U;

CongestionPolicy prague_policy()
{
    CongestionPolicy policy;
    policy.algorithm = CongestionAlgorithm::Prague;
    return policy;
}

// A Prague sender driven round by round, a bulk sender the window holds back: a round sends what the window holds
// (or `packets`) a microsecond after the last ACK frame and acknowledges it `rtt_ns` later in `frames` ACK frames,
// each with an RTT sample and ECN feedback for its packets.
class Driver
{
public:
    explicit Driver(const CongestionPolicy& policy = prague_policy())
    {
        ctl = make_congestion(policy, kMss, nullptr, m_slot, m_delivery, 1U);
        REQUIRE(ctl != nullptr);
    }
    ~Driver() { ctl->~CongestionController(); }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    struct Round
    {
        u64 ce = 0U;      // CE packets in the first frame
        u64 ce_rest = 0U; // and in each later one
        u64 rtt_ns = 20U * kMs;
        u64 frames = 1U;
        u64 packets = 0U;  // 0: what the window holds
        u64 queue_ns = 0U; // each frame's sample adds up to this, rising across the round
        bool ecn = true;
    };

    void round(const Round& r)
    {
        const u64 n = (r.packets != 0U) ? r.packets : ((ctl->window() / kMss > 0U) ? ctl->window() / kMss : 1U);
        REQUIRE(n <= kMaxPackets);
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
        ctl->on_limited(true, now);
        now = first_sent + r.rtt_ns;
        const u64 per_frame = (n + r.frames - 1U) / r.frames;
        for (u64 frame = 0U; frame * per_frame < n; ++frame)
        {
            const u64 begin = frame * per_frame;
            const u64 end = (begin + per_frame < n) ? begin + per_frame : n;
            m_delivery.on_ack_begin();
            for (u64 i = begin; i < end; ++i)
            {
                in_flight -= kMss;
                m_delivery.on_acked(m_packets[i].delivery, kMss, m_packets[i].sent_ns, m_packets[i].number, now);
                ctl->on_acked(m_packets[i], now);
            }
            rtt.sample(r.rtt_ns + r.queue_ns * frame / r.frames, 0U, true, 0U, now);
            m_delivery.on_ack_end(rtt.min_ns);
            if (r.ecn)
            {
                ctl->on_ecn(end - begin, (frame == 0U) ? r.ce : r.ce_rest, m_packets[end - 1U].sent_ns, now);
            }
            ctl->on_ack_end(rtt, true, in_flight, now);
        }
    }

    void rounds(usize count, const Round& r)
    {
        for (usize i = 0U; i < count; ++i)
        {
            round(r);
        }
    }

    [[nodiscard]] u64 last_sent_ns() const { return m_packets[0].sent_ns; }

    CongestionController* ctl = nullptr;
    QuicRtt rtt;
    u64 now = 0U;
    u64 in_flight = 0U;

private:
    static constexpr usize kMaxPackets = 4096U;
    static constexpr u64 kSendGapNs = 1000U;

    CongestionSlot m_slot;
    DeliveryRateSampler m_delivery;
    std::unique_ptr<CongestionPacket[]> m_packets = std::make_unique<CongestionPacket[]>(kMaxPackets);
    u64 m_number = 0U;
};

// The window after a mark's reduction from `window` at `alpha` (2^-20).
u64 reduced(u64 window, u64 alpha)
{
    return window - window * alpha / (2U * kCongestionAlphaOne);
}

} // namespace

TEST_CASE("Prague: the policy rules, ECT(1) and alpha starting at one", "[net][net-path][prague]")
{
    CongestionPolicy policy = prague_policy();
    CHECK(congestion_policy_valid(policy));
    CHECK(congestion_codepoint(CongestionAlgorithm::Prague) == Ecn::Ect1);
    for (const CongestionAlgorithm classic :
         {CongestionAlgorithm::NewReno, CongestionAlgorithm::Cubic, CongestionAlgorithm::Bbr})
    {
        CHECK(congestion_codepoint(classic) == Ecn::Ect0);
    }
    policy.pacing = false; // the draft's section 2.5.1: Prague must pace
    CHECK_FALSE(congestion_policy_valid(policy));
    policy = prague_policy();
    policy.version = 2U; // interface version 3's
    CHECK_FALSE(congestion_policy_valid(policy));

    Driver d;
    CHECK(d.ctl->algorithm() == CongestionAlgorithm::Prague);
    const CongestionState state = d.ctl->state();
    CHECK(state.window == 10U * kMss);
    CHECK(state.alpha == kCongestionAlphaOne);
    CHECK(state.classic == 0U);
    CHECK(state.phase == CongestionPhase::SlowStart);
    // Paced at twice the window per round trip in early slow start, with the initial window's burst before a sample.
    QuicRtt rtt;
    rtt.smoothed_ns = 10U * kMs;
    CHECK(d.ctl->pacing_rate(rtt) == 2U * 10U * kMss * 100U);
    CHECK(d.ctl->burst() == 10U * kMss);
}

TEST_CASE(
    "Prague: the first CE halves the window, then CE takes alpha / 2 once a round trip, alpha the moving fraction",
    "[net][net-path][prague]")
{
    Driver d;
    d.rounds(3U, {});
    CHECK(d.ctl->state().phase == CongestionPhase::SlowStart);
    const u64 grown = d.ctl->window();
    CHECK(grown == 80U * kMss); // slow start doubled it each round
    // The first CE: alpha is 1, so the window halves (section 2.3.2) and slow start ends.
    Driver::Round marked;
    marked.ce = 1U;
    d.round(marked);
    CongestionState state = d.ctl->state();
    CHECK(state.reductions == 1U);
    CHECK(state.slow_start_threshold == grown / 2U);
    // CWR keeps growing (section 2.4.3): the frame's acknowledged bytes, at most a window, less its one marked
    // datagram, grow the halved window.
    CHECK(state.window == grown / 2U + (grown / 2U - kMss) * kMss / (grown / 2U));
    CHECK(state.phase == CongestionPhase::Avoidance);
    // Alpha moved by g toward 1/80 at the round's end.
    double alpha = 1.0 - (1.0 - 1.0 / 80.0) / 16.0;
    CHECK(std::fabs(static_cast<double>(state.alpha) / kCongestionAlphaOne - alpha) < 1e-6);

    // A second CE for packets sent before the reduction is inside CWR: no second reduction.
    const u64 window = d.ctl->window();
    d.ctl->on_ecn(1U, 1U, d.last_sent_ns(), d.now);
    CHECK(d.ctl->window() == window);
    CHECK(d.ctl->state().reductions == 1U);

    // Forty rounds with a quarter of the packets marked: each round's first CE reduces by the alpha of that moment, and
    // alpha follows the real-valued average.
    for (int i = 0; i < 40; ++i)
    {
        const u64 before = d.ctl->window();
        const u32 a = d.ctl->state().alpha;
        Driver::Round quarter;
        quarter.packets = 80U;
        quarter.ce = 20U;
        d.round(quarter);
        const u64 cut = reduced(before, a);
        // The window grows after the reduction by its unmarked bytes, under a datagram.
        CHECK(d.ctl->window() >= cut);
        CHECK(d.ctl->window() <= cut + kMss);
        alpha += (0.25 - alpha) / 16.0;
    }
    state = d.ctl->state();
    CHECK(state.reductions == 41U);
    CHECK(std::fabs(static_cast<double>(state.alpha) / kCongestionAlphaOne - alpha) < 1e-4);
    CHECK(state.alpha > kCongestionAlphaOne / 4U);
    CHECK(state.alpha < kCongestionAlphaOne / 3U);
    CHECK(state.undone == 0U);
}

TEST_CASE("Prague: marked bytes grow nothing while unmarked ones grow a datagram a round trip",
          "[net][net-path][prague]")
{
    // Two senders the same but for a round's second ACK frame: its packets, sent before the first frame's reduction,
    // arrive inside CWR all marked for one and unmarked for the other. The marked ones grow nothing.
    const auto split = [](bool marked)
    {
        Driver d;
        d.rounds(3U, {});
        Driver::Round first;
        first.ce = 1U;
        d.round(first);
        Driver::Round two;
        two.frames = 2U;
        two.ce = 1U;
        two.ce_rest = marked ? d.ctl->window() / kMss / 2U : 0U;
        d.round(two);
        CHECK(d.ctl->state().reductions == 2U);
        return d.ctl->window();
    };
    const u64 after_marked = split(true);
    const u64 after_clean = split(false);
    CHECK(after_clean - after_marked >= kMss / 2U); // half a window's datagrams grow nearly one
    CHECK(after_clean - after_marked <= kMss);

    // Over rounds without marks, avoidance grows one datagram a round (Reno's, before 500 rounds).
    Driver d;
    d.rounds(3U, {});
    Driver::Round first;
    first.ce = 1U;
    d.round(first);
    const u64 start = d.ctl->window();
    d.rounds(20U, {});
    CHECK(d.ctl->window() >= start + 19U * kMss);
    CHECK(d.ctl->window() <= start + 21U * kMss);
}

TEST_CASE("Prague: a loss halves the window, and within a mark's round trip halves the window before the mark",
          "[net][net-path][prague]")
{
    Driver d;
    d.rounds(4U, {});
    Driver::Round marked;
    marked.ce = 1U;
    d.round(marked);
    d.rounds(30U, {}); // unmarked rounds: alpha falls by 15/16 each, the window grows
    const u64 before = d.ctl->window();
    const u32 alpha = d.ctl->state().alpha;
    CHECK(alpha < kCongestionAlphaOne / 4U);
    CHECK(before > 100U * kMss);
    // The mark's reduction (for a packet sent after the last), then a loss of a packet sent after it, inside the same
    // round trip.
    d.ctl->on_ecn(1U, 1U, d.now + 1U, d.now + 1U);
    const u64 cut = d.ctl->window();
    CHECK(cut == reduced(before, alpha));
    CongestionPacket lost;
    lost.bytes = kMss;
    lost.number = 1U << 20U;
    lost.sent_ns = d.now + 1U;
    lost.space = QuicSpace::Application;
    d.ctl->on_lost(lost, d.now + 2U * kMs);
    CHECK(d.ctl->window() == before / 2U); // the two together reduce as Reno's one does
    CHECK(d.ctl->state().phase == CongestionPhase::Recovery);

    // Long after any mark, a loss halves the window itself.
    Driver later;
    later.rounds(4U, {});
    later.round(marked);
    later.rounds(5U, {});
    const u64 window = later.ctl->window();
    lost.sent_ns = later.now + 1U;
    later.ctl->on_lost(lost, later.now + 2U * kMs);
    CHECK(later.ctl->window() == window / 2U);
    // A Retry starts over: the initial window and alpha 1.
    later.ctl->reset(later.now);
    CHECK(later.ctl->window() == 10U * kMss);
    CHECK(later.ctl->state().alpha == kCongestionAlphaOne);
}

TEST_CASE("Prague: after 500 rounds growth no longer depends on the round trip; without ECN it stays Reno's",
          "[net][net-path][prague]")
{
    // Window growth over 100 unmarked round trips after the transition.
    const auto growth = [](u64 rtt_ns, bool ecn)
    {
        Driver d;
        Driver::Round round;
        round.rtt_ns = rtt_ns;
        round.ecn = ecn;
        d.rounds(3U, round);
        if (ecn)
        {
            Driver::Round first = round;
            first.ce = 1U;
            d.round(first);
        }
        else
        {
            d.ctl->on_ecn_unavailable();
            CongestionPacket lost;
            lost.bytes = kMss;
            lost.space = QuicSpace::Application;
            d.ctl->on_lost(lost, d.now - 1U); // leaves slow start
        }
        round.packets = 50U; // each round acknowledges 50 datagrams whatever the window
        d.rounds(510U, round);
        const u64 start = d.ctl->window();
        d.rounds(100U, round);
        return d.ctl->window() - start;
    };
    const u64 g5 = growth(5U * kMs, true);
    const u64 g25 = growth(25U * kMs, true);
    const u64 g50 = growth(50U * kMs, true);
    const u64 reno5 = growth(5U * kMs, false);
    INFO("growth 5 ms " << g5 << ", 25 ms " << g25 << ", 50 ms " << g50 << ", 5 ms without ECN " << reno5);
    CHECK(g25 > 10U * kMss);
    CHECK(g50 == g25);               // at or above the virtual round trip: Reno's
    CHECK(reno5 * 20U >= g25 * 19U); // without ECN: Reno's at any round trip
    CHECK(reno5 * 20U <= g25 * 21U);
    // (5 / 25)^2 of it at 5 ms.
    CHECK(g5 * 25U * 10U >= g25 * 9U);
    CHECK(g5 * 25U * 10U <= g25 * 11U);

    // Alpha and CWR on the virtual round trip: with marks every round, alpha moves and the window is reduced every
    // fifth 5 ms round, and every round at 25 ms.
    const auto periods = [](u64 rtt_ns)
    {
        Driver d;
        Driver::Round round;
        round.rtt_ns = rtt_ns;
        round.packets = 50U;
        Driver::Round other = round;
        round.ce = 1U;
        other.ce = 3U; // alternating fractions keep alpha moving
        d.rounds(3U, {});
        for (int i = 0; i < 255; ++i)
        {
            d.round(round);
            d.round(other);
        }
        u32 moves = 0U;
        u32 alpha = d.ctl->state().alpha;
        const u64 reductions = d.ctl->state().reductions;
        for (int i = 0; i < 50; ++i)
        {
            for (const Driver::Round& r : {round, other})
            {
                d.round(r);
                moves += (d.ctl->state().alpha != alpha) ? 1U : 0U;
                alpha = d.ctl->state().alpha;
            }
        }
        return std::pair<u32, u64>{moves, d.ctl->state().reductions - reductions};
    };
    const auto [moves5, cuts5] = periods(5U * kMs);
    const auto [moves25, cuts25] = periods(25U * kMs);
    CHECK(moves5 == 20U);
    CHECK(cuts5 == 20U);
    CHECK(moves25 == 100U);
    CHECK(cuts25 == 100U);
}

TEST_CASE("Prague: a classic sawtooth queue with marks turns the reduction to ABE's", "[net][net-path][prague]")
{
    Driver d;
    d.rounds(3U, {});
    // Per-packet ACK frames, so the detector has its samples: a queue from 0 to 20 ms each round (a classic AQM's
    // sawtooth), a CE every round.
    Driver::Round classic;
    classic.rtt_ns = 20U * kMs;
    classic.queue_ns = 20U * kMs;
    classic.packets = 40U;
    classic.frames = 40U;
    classic.ce = 1U;
    d.rounds(300U, classic);
    CongestionState state = d.ctl->state();
    CHECK(state.classic == u32{1} << 16U);
    // Alpha is small by now (1 in 40), yet a mark takes ABE's 30% (alpha 0.6).
    CHECK(state.alpha < kCongestionAlphaOne / 8U);
    const u64 before = d.ctl->window();
    d.ctl->on_ecn(1U, 1U, d.now + 1U, d.now + kMs);
    CHECK(d.ctl->window() == reduced(before, kCongestionAlphaOne * 3U / 5U));

    // The same marks over an L4S queue (0.5 ms) leave it scalable.
    Driver l4s;
    l4s.rounds(3U, {});
    classic.queue_ns = 500000U;
    l4s.rounds(300U, classic);
    CHECK(l4s.ctl->state().classic == 0U);
}

namespace
{
using crd::net::testing::kBottleneckSecond;
using crd::net::testing::Link;
using crd::net::testing::run_flows;

// Bulk flows over a bottleneck of kStepRate bytes per second.
constexpr u64 kStepRate = 10000000U;

struct FlowResult
{
    u64 goodput = 0U; // bytes per second over the measured span
    u64 max_queue = 0U;
    u64 dropped = 0U; // from the start
    CongestionState state;
};

// One flow of `algorithm` over a `rtt_ms` round trip with a buffer of two BDPs and a CE step at `step` bytes,
// `warm_ms` to settle and then `measure_ms`.
FlowResult run_step(CongestionAlgorithm algorithm, u64 rtt_ms, u64 step, u64 warm_ms = 2000U, u64 measure_ms = 6000U)
{
    CongestionPolicy policy;
    policy.algorithm = algorithm;
    const u64 bdp = kStepRate / 1000U * rtt_ms;
    Path path(policy, kStepRate, rtt_ms * kMs / 2U, 2U * bdp);
    path.set_ce_queue(step);
    path.run(warm_ms * kMs);
    const u64 start = path.now;
    const u64 acked = path.acked_bytes;
    FlowResult out;
    path.run(start + measure_ms * kMs,
             [&]
             {
                 const u64 queue = path.queue_bytes();
                 out.max_queue = (queue > out.max_queue) ? queue : out.max_queue;
             });
    out.goodput = (path.acked_bytes - acked) * kBottleneckSecond / (path.now - start);
    out.dropped = path.dropped;
    out.state = path.ctl->state();
    return out;
}

// Two bulk flows sharing one link of kStepRate with a buffer of `buffer` bytes and CE from `step` (a ramp of `ramp`
// bytes above it, or a step), each with its own round trip, `warm_ms` to settle and then `measure_ms`.
struct Pair
{
    FlowResult a;
    FlowResult b;
    u64 max_queue = 0U;
    u64 dropped = 0U; // both flows' over the measured span
};

Pair run_pair(CongestionAlgorithm a, u64 a_rtt_ms, CongestionAlgorithm b, u64 b_rtt_ms, u64 buffer, u64 step,
              u64 warm_ms, u64 measure_ms, u64 ramp = 0U)
{
    Link link{kStepRate, buffer, step, ramp};
    CongestionPolicy policy_a;
    policy_a.algorithm = a;
    CongestionPolicy policy_b;
    policy_b.algorithm = b;
    Path flow_a(policy_a, link, a_rtt_ms * kMs / 2U, 7U);
    Path flow_b(policy_b, link, b_rtt_ms * kMs / 2U, 11U);
    run_flows({&flow_a, &flow_b}, warm_ms * kMs, [] {});
    const u64 start = flow_a.now;
    const u64 acked_a = flow_a.acked_bytes;
    const u64 acked_b = flow_b.acked_bytes;
    const u64 dropped = flow_a.dropped + flow_b.dropped;
    Pair out;
    run_flows({&flow_a, &flow_b}, start + measure_ms * kMs,
              [&]
              {
                  const u64 queue = link.queue_bytes(flow_a.now > flow_b.now ? flow_a.now : flow_b.now);
                  out.max_queue = (queue > out.max_queue) ? queue : out.max_queue;
              });
    out.a.goodput = (flow_a.acked_bytes - acked_a) * kBottleneckSecond / (measure_ms * kMs);
    out.b.goodput = (flow_b.acked_bytes - acked_b) * kBottleneckSecond / (measure_ms * kMs);
    out.a.dropped = flow_a.dropped;
    out.b.dropped = flow_b.dropped;
    out.dropped = flow_a.dropped + flow_b.dropped - dropped;
    out.a.state = flow_a.ctl->state();
    out.b.state = flow_b.ctl->state();
    return out;
}

} // namespace

TEST_CASE("Prague: an L4S step keeps the queue at the step and the link full, where CUBIC and NewReno fall short",
          "[net][net-path][prague]")
{
    // 10 MB/s, 20 ms round trip (a 200 KB BDP), a buffer of two BDPs and a CE step at 1 ms (10 KB), measured from
    // 2 s (Prague still growing back from its slow start's exit) and after 20 s.
    static constexpr u64 kStep = kStepRate / 1000U;
    const FlowResult prague = run_step(CongestionAlgorithm::Prague, 20U, kStep);
    const FlowResult cubic = run_step(CongestionAlgorithm::Cubic, 20U, kStep);
    const FlowResult reno = run_step(CongestionAlgorithm::NewReno, 20U, kStep);
    const FlowResult prague_settled = run_step(CongestionAlgorithm::Prague, 20U, kStep, 20000U);
    const FlowResult cubic_settled = run_step(CongestionAlgorithm::Cubic, 20U, kStep, 20000U);
    const FlowResult reno_settled = run_step(CongestionAlgorithm::NewReno, 20U, kStep, 20000U);
    INFO("Prague " << prague.goodput << " B/s, queue " << prague.max_queue << "; CUBIC " << cubic.goodput
                   << " B/s, queue " << cubic.max_queue << "; NewReno " << reno.goodput << " B/s");
    CHECK(prague.dropped == 0U);
    CHECK(prague.goodput >= kStepRate * 92U / 100U);
    CHECK(prague.max_queue <= kStep + 2U * kMss); // the step and the datagram or two in flight to it
    CHECK(cubic.goodput < prague.goodput);
    CHECK(reno.goodput < cubic.goodput);
    INFO("settled: Prague " << prague_settled.goodput << " B/s, CUBIC " << cubic_settled.goodput << " B/s, NewReno "
                            << reno_settled.goodput << " B/s");
    CHECK(prague_settled.goodput >= kStepRate * 99U / 100U);
    CHECK(prague_settled.max_queue <= kStep + 2U * kMss);
    CHECK(cubic_settled.goodput < prague_settled.goodput);
    CHECK(reno_settled.goodput < cubic_settled.goodput);
}

TEST_CASE("Prague: settled, L4S steps of 1 and 2 ms fill the link at 20 and 50 ms; a classic-depth one is detected",
          "[net][net-path][prague]")
{
    // Twenty seconds to settle: at 50 ms the first CE in slow start leaves a quarter of the BDP, which a datagram a
    // round trip takes 15 s to grow back. Settled and paced, even a 1 ms step (a fiftieth of the 50 ms BDP) keeps 99%
    // of the link: reductions by alpha / 2 seldom empty the queue (DCTCP's K >= C RTT / 7 is for synchronised,
    // unpaced windows). Steps of 5 and 10 ms are classic AQM depths: the detector turns the reduction to ABE's 30%,
    // which leaves a link short below 3/7 of the BDP (92% at 50 ms and a 5 ms step).
    for (const u64 rtt_ms : {20U, 50U})
    {
        u64 previous = 0U;
        for (const u64 step_ms : {1U, 2U, 5U, 10U})
        {
            const u64 step = kStepRate / 1000U * step_ms;
            const FlowResult prague = run_step(CongestionAlgorithm::Prague, rtt_ms, step, 20000U);
            INFO(rtt_ms << " ms, step " << step_ms << " ms: " << prague.goodput << " B/s, queue " << prague.max_queue
                        << ", alpha " << prague.state.alpha << ", classic " << prague.state.classic);
            CHECK(prague.dropped == 0U);
            CHECK(prague.max_queue <= step + 2U * kMss);
            if (step_ms <= 2U)
            {
                CHECK(prague.state.classic == 0U);
                CHECK(prague.goodput >= kStepRate * 99U / 100U);
                CHECK(prague.goodput > previous);
                previous = prague.goodput;
            }
            else
            {
                CHECK(prague.state.classic == u32{1} << 16U);
                CHECK(prague.goodput >= kStepRate * 92U / 100U);
            }
        }
    }
}

TEST_CASE("Prague: two flows through an L4S queue share it, and RTT independence narrows round trips of 5 and 20 ms",
          "[net][net-path][prague]")
{
    static constexpr u64 kStep = kStepRate / 1000U;
    static constexpr u64 kBuffer = 2U * kStepRate / 50U; // two BDPs at 20 ms
    const auto share = [](const Pair& pair) { return pair.a.goodput + pair.b.goodput; };

    const Pair same = run_pair(CongestionAlgorithm::Prague, 20U, CongestionAlgorithm::Prague, 20U, kBuffer, kStep,
                               4000U, 8000U);
    INFO("same RTT: " << same.a.goodput << " and " << same.b.goodput << " B/s, queue " << same.max_queue);
    CHECK(share(same) >= kStepRate * 97U / 100U);
    CHECK(same.a.goodput * 100U >= same.b.goodput * 95U);
    CHECK(same.b.goodput * 100U >= same.a.goodput * 95U);
    CHECK(same.max_queue <= kStep + 3U * kMss);
    CHECK(same.a.dropped + same.b.dropped == 0U);

    // Past 500 rounds of the 20 ms flow, both grow and reduce on the 25 ms virtual round trip. Through a step the
    // shorter flow's marks lag less, so it still takes more, but under a third of NewReno's ratio; through a ramp (CE
    // with a probability rising across 2 ms, as a DualQ's L4S queue may mark) the two come within 1.6 to 1.
    const Pair step = run_pair(CongestionAlgorithm::Prague, 5U, CongestionAlgorithm::Prague, 20U, kBuffer, kStep,
                               15000U, 15000U);
    const Pair reno = run_pair(CongestionAlgorithm::NewReno, 5U, CongestionAlgorithm::NewReno, 20U, kBuffer, kStep,
                               15000U, 15000U);
    const Pair ramp = run_pair(CongestionAlgorithm::Prague, 5U, CongestionAlgorithm::Prague, 20U, kBuffer, kStep / 2U,
                               30000U, 15000U, 2U * kStep);
    INFO("step: Prague " << step.a.goodput << " and " << step.b.goodput << " B/s (alpha " << step.a.state.alpha
                         << " and " << step.b.state.alpha << "), NewReno " << reno.a.goodput << " and "
                         << reno.b.goodput << " B/s; ramp: Prague " << ramp.a.goodput << " and " << ramp.b.goodput
                         << " B/s, queue " << ramp.max_queue);
    CHECK(share(step) >= kStepRate * 97U / 100U);
    CHECK(share(ramp) >= kStepRate * 97U / 100U);
    CHECK(reno.a.goodput >= 10U * reno.b.goodput);
    CHECK(step.a.goodput * 3U <= reno.a.goodput * step.b.goodput / reno.b.goodput);
    CHECK(ramp.a.goodput * 10U <= ramp.b.goodput * 16U);
    CHECK(ramp.max_queue <= 3U * kStep);
}

TEST_CASE("Prague: beside CUBIC in one classic ECN queue it detects the AQM and stays within twice CUBIC's share",
          "[net][net-path][prague]")
{
    // A single queue marking past 5 or 15 ms, as a classic AQM without flow isolation does, both flows at 20 ms. The
    // detector turns Prague's reduction to ABE's 30%; TR-BB-2019-002 section 8 takes a Prague to CUBIC-ECN ratio
    // within 0.5 to 2 as Reno-friendly, where a scalable answer alone reaches about 16. NewReno with ECN beside the
    // same CUBIC is the classic reference.
    for (const u64 step_ms : {5U, 15U})
    {
        const u64 step = kStepRate / 1000U * step_ms;
        const Pair pair = run_pair(CongestionAlgorithm::Prague, 20U, CongestionAlgorithm::Cubic, 20U,
                                   2U * kStepRate / 50U, step, 10000U, 20000U);
        const Pair reno = run_pair(CongestionAlgorithm::NewReno, 20U, CongestionAlgorithm::Cubic, 20U,
                                   2U * kStepRate / 50U, step, 10000U, 20000U);
        INFO("step " << step_ms << " ms: Prague " << pair.a.goodput << " B/s (classic " << pair.a.state.classic
                     << ", alpha " << pair.a.state.alpha << "), CUBIC " << pair.b.goodput << " B/s, queue "
                     << pair.max_queue << "; NewReno " << reno.a.goodput << " beside CUBIC " << reno.b.goodput);
        CHECK(pair.a.state.classic == u32{1} << 16U);
        CHECK(pair.a.goodput <= 2U * pair.b.goodput);
        CHECK(2U * pair.a.goodput >= pair.b.goodput);
        CHECK(pair.a.goodput + pair.b.goodput >= kStepRate * 97U / 100U);
        CHECK(pair.max_queue <= step + 3U * kMss);
        CHECK(pair.dropped == 0U);
        CHECK(reno.a.goodput <= 2U * reno.b.goodput);
        CHECK(2U * reno.a.goodput >= reno.b.goodput);
    }
}
