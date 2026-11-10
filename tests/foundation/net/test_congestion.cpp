// crd-net -- congestion control and pacing (NET-PATH; docs/design/network-foundation.md#congestion): the policy's
// bounds and RFC 9002 section 7.2's initial window, slow start and NewReno's avoidance (appendix B) with one reduction
// per recovery period, persistent congestion, no growth while the window was not what held sending back (section
// 7.8), HyStart++ (RFC 9406) into and out of its conservative phase and on to avoidance, CUBIC (RFC 9438) against its
// curve worked in floating point, its Reno-friendly region, fast convergence and idle periods, the undo of a period
// whose every loss proved spurious and never of a mark, a live swap carrying the window and the recovery period, a
// Retry's reset, and the pacer's exact credit, burst, debt and release times.

#include <crd/net/congestion.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kSecond = 1000U * kMs;
constexpr u64 kMss = 1200U;

CongestionPacket packet(u64 number, u64 sent_ns, u64 bytes = kMss, QuicSpace space = QuicSpace::Application)
{
    CongestionPacket out;
    out.bytes = bytes;
    out.number = number;
    out.sent_ns = sent_ns;
    out.space = space;
    return out;
}

QuicRtt rtt_of(u64 latest_ns, u64 smoothed_ns)
{
    QuicRtt rtt;
    rtt.latest_ns = latest_ns;
    rtt.min_ns = latest_ns;
    rtt.smoothed_ns = smoothed_ns;
    return rtt;
}

CongestionPolicy policy_of(CongestionAlgorithm algorithm, bool hystart = false, bool pacing = true)
{
    CongestionPolicy policy;
    policy.algorithm = algorithm;
    policy.hystart = hystart;
    policy.pacing = pacing;
    return policy;
}

// A controller in its own slot, destroyed with it.
struct Controller
{
    CongestionSlot slot;
    CongestionController* ctl = nullptr;

    explicit Controller(const CongestionPolicy& policy, u64 mss = kMss, const CongestionState* seed = nullptr)
    {
        ctl = make_congestion(policy, mss, seed, slot);
        REQUIRE(ctl != nullptr);
    }
    ~Controller() { ctl->~CongestionController(); }
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    CongestionController* operator->() const { return ctl; }

    // Sends packets [first, last] at `now`, in flight from `in_flight_before`.
    void send(u64 first, u64 last, u64 now, u64 in_flight_before = 0U) const
    {
        for (u64 n = first; n <= last; ++n)
        {
            ctl->on_sent(packet(n, now), in_flight_before + (n - first + 1U) * kMss, now);
        }
    }

    // One ACK frame acknowledging `number`, sent at `sent_ns`, with an RTT sample.
    void ack(u64 number, u64 sent_ns, u64 now, u64 latest_ns, u64 smoothed_ns = 100U * kMs) const
    {
        ctl->on_acked(packet(number, sent_ns), now);
        ctl->on_ack_end(rtt_of(latest_ns, smoothed_ns), true, now);
    }
};

} // namespace

TEST_CASE("congestion: policies out of range are refused and the initial window follows RFC 9002 section 7.2",
          "[net][net-path][congestion]")
{
    CHECK(congestion_policy_valid(CongestionPolicy{}));
    const auto refused = [](auto change)
    {
        CongestionPolicy policy;
        change(policy);
        CongestionSlot slot;
        CHECK_FALSE(congestion_policy_valid(policy));
        CHECK(make_congestion(policy, kMss, nullptr, slot) == nullptr);
    };
    refused([](CongestionPolicy& p) { p.version = kCongestionInterfaceVersion + 1U; });
    refused([](CongestionPolicy& p) { p.algorithm = static_cast<CongestionAlgorithm>(7U); });
    refused([](CongestionPolicy& p) { p.minimum_packets = 1U; });
    refused([](CongestionPolicy& p) { p.initial_packets = 1U; });
    refused([](CongestionPolicy& p) { p.initial_packets = kCongestionMaxInitialPackets + 1U; });
    refused([](CongestionPolicy& p) { p.pacing_gain = kCongestionMinPacingGain - 1U; });
    refused([](CongestionPolicy& p) { p.pacing_gain = kCongestionMaxPacingGain + 1U; });
    CongestionSlot slot;
    CHECK(make_congestion(CongestionPolicy{}, kMss - 1U, nullptr, slot) == nullptr);
    CHECK(make_congestion(CongestionPolicy{}, 65536U, nullptr, slot) == nullptr);

    // min(10 * mss, max(14720, 2 * mss)), and the same per packet for other counts.
    CHECK(Controller(CongestionPolicy{})->window() == 12000U);
    CHECK(Controller(CongestionPolicy{}, 1500U)->window() == 14720U);
    CongestionPolicy two;
    two.initial_packets = 2U;
    CHECK(Controller(two)->window() == 2400U);
    CongestionPolicy many;
    many.initial_packets = 32U;
    CHECK(Controller(many)->window() == 38400U);
    const Controller fresh(CongestionPolicy{});
    CHECK(fresh->algorithm() == CongestionAlgorithm::Cubic);
    CHECK(fresh->burst() == 12000U);
    CHECK(fresh->state().phase == CongestionPhase::SlowStart);
    CHECK(fresh->state().slow_start_threshold == kCongestionMaxWindow);
    CHECK(fresh->state().recovery_start_ns == kNeverNs);
}

TEST_CASE("congestion: a new datagram size resizes the initial and minimum windows, and an initial window follows it",
          "[net][net-path][congestion][quic-pmtu]")
{
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::NewReno, CongestionAlgorithm::Cubic})
    {
        INFO("algorithm " << static_cast<int>(algorithm));
        // Still at its initial window: it takes the new size's (RFC 9002 section 7.2).
        const Controller fresh(policy_of(algorithm));
        REQUIRE(fresh->window() == 12000U);
        fresh->set_max_datagram(1472U);
        CHECK(fresh->window() == 14720U);
        CHECK(fresh->burst() == 14720U);
        fresh->set_max_datagram(9000U);
        CHECK(fresh->window() == 18000U); // min(10 * 9000, max(14720, 2 * 9000))
        fresh->set_max_datagram(1U);      // below QUIC's floor: the floor
        CHECK(fresh->window() == 12000U);

        // A window that grew keeps its bytes; the minimum follows the size, also after persistent congestion.
        const Controller grown(policy_of(algorithm));
        grown.send(0U, 9U, 0U);
        for (u64 n = 0U; n <= 9U; ++n)
        {
            grown.ack(n, 0U, 100U * kMs, 100U * kMs);
        }
        const u64 window = grown->window();
        REQUIRE(window > 12000U);
        grown->set_max_datagram(1472U);
        CHECK(grown->window() == window);
        grown->on_persistent_congestion(200U * kMs);
        CHECK(grown->window() == 2U * 1472U);
        grown->set_max_datagram(kMss); // a black hole: back to the floor
        CHECK(grown->window() == 2U * 1472U);
        grown->on_persistent_congestion(300U * kMs);
        CHECK(grown->window() == 2U * kMss);
        // A grown size raises a window below the new minimum to it, and the threshold with it.
        grown->set_max_datagram(9000U);
        CHECK(grown->window() == 2U * 9000U);
        CHECK(grown->state().slow_start_threshold >= 2U * 9000U);
    }
}

TEST_CASE("congestion: the pacing rate is the gain times the window per smoothed RTT", "[net][net-path][congestion]")
{
    const Controller reno(policy_of(CongestionAlgorithm::NewReno));
    CHECK(reno->pacing_rate(rtt_of(100U * kMs, 100U * kMs)) == 150000U); // 1.25 * 12000 bytes / 0.1 s
    CHECK(reno->pacing_rate(rtt_of(0U, 0U)) == 0U);                       // no time to spread over: unpaced
    CongestionPolicy doubled = policy_of(CongestionAlgorithm::NewReno);
    doubled.pacing_gain = 200U;
    CHECK(Controller(doubled)->pacing_rate(rtt_of(kMs, 10U * kMs)) == 2400000U);
    CHECK(Controller(policy_of(CongestionAlgorithm::NewReno, false, false))->pacing_rate(rtt_of(kMs, kMs)) == 0U);
    // The largest window at the largest gain over 1 ns does not wrap.
    CongestionState huge;
    huge.window = kCongestionMaxWindow;
    CongestionPolicy fastest = policy_of(CongestionAlgorithm::Cubic);
    fastest.pacing_gain = kCongestionMaxPacingGain;
    const Controller big(fastest, kMss, &huge);
    CHECK(big->window() == kCongestionMaxWindow);
    CHECK(big->pacing_rate(rtt_of(1U, 1U)) == kCongestionMaxWindow * 4U * 1000000000U);
}

TEST_CASE("congestion: NewReno grows by what is acknowledged, then by a datagram per window, and only when the window "
          "held sending back",
          "[net][net-path][congestion]")
{
    Controller reno(policy_of(CongestionAlgorithm::NewReno));
    // Two packets do not fill the window: their acknowledgement grows nothing (RFC 9002 section 7.8).
    reno.send(0U, 1U, 0U);
    reno.ack(0U, 0U, 100U * kMs, 100U * kMs);
    reno.ack(1U, 0U, 100U * kMs, 100U * kMs);
    CHECK(reno->window() == 12000U);
    CHECK_FALSE(reno->state().limited);
    // A send the window held back lets it grow.
    reno->on_limited(100U * kMs);
    reno.send(2U, 2U, 100U * kMs);
    reno.ack(2U, 100U * kMs, 200U * kMs, 100U * kMs);
    CHECK(reno->window() == 13200U);
    // Filling the window is being held by it too.
    reno.send(3U, 13U, 200U * kMs);
    CHECK(reno->state().limited);
    for (u64 n = 3U; n <= 13U; ++n)
    {
        reno.ack(n, 200U * kMs, 300U * kMs, 100U * kMs);
    }
    CHECK(reno->window() == 13200U + 11U * kMss);

    // A loss halves it once per recovery period; acknowledgements of packets sent before the period grow nothing.
    const u64 window = reno->window();
    reno->on_lost(packet(14U, 250U * kMs), 400U * kMs);
    CHECK(reno->window() == window / 2U);
    CHECK(reno->state().slow_start_threshold == window / 2U);
    CHECK(reno->state().phase == CongestionPhase::Recovery);
    CHECK(reno->state().recovery_start_ns == 400U * kMs);
    CHECK(reno->state().reductions == 1U);
    reno->on_lost(packet(15U, 399U * kMs), 401U * kMs); // sent before the period began
    CHECK(reno->window() == window / 2U);
    CHECK(reno->state().reductions == 1U);
    reno.ack(16U, 300U * kMs, 410U * kMs, 100U * kMs);
    CHECK(reno->window() == window / 2U);
    CHECK(reno->state().phase == CongestionPhase::Recovery);

    // A packet sent after the period began ends it; avoidance adds mss * acked / window, its remainder carried.
    reno->on_limited(410U * kMs);
    reno.send(17U, 18U, 410U * kMs, 10000U);
    const u64 half = window / 2U;
    reno.ack(17U, 410U * kMs, 510U * kMs, 100U * kMs);
    CHECK(reno->state().phase == CongestionPhase::Avoidance);
    const u64 first = half + kMss * kMss / half;
    CHECK(reno->window() == first);
    reno.ack(18U, 410U * kMs, 511U * kMs, 100U * kMs);
    CHECK(reno->window() == first + (kMss * kMss + kMss * kMss % half) / first);

    // Persistent congestion: the minimum window, the period over, slow start again.
    reno->on_persistent_congestion(600U * kMs);
    CHECK(reno->window() == 2U * kMss);
    CHECK(reno->state().phase == CongestionPhase::SlowStart);
    CHECK(reno->state().recovery_start_ns == kNeverNs);
    CHECK(reno->state().slow_start_threshold == half);
}

TEST_CASE("congestion: slow start without pacing grows by at most eight datagrams an acknowledgement under HyStart++",
          "[net][net-path][congestion]")
{
    for (const bool pacing : {true, false})
    {
        Controller reno(policy_of(CongestionAlgorithm::NewReno, true, pacing));
        reno.send(0U, 9U, 0U);
        for (u64 n = 0U; n <= 9U; ++n)
        {
            reno->on_acked(packet(n, 0U), 100U * kMs);
        }
        reno->on_ack_end(rtt_of(100U * kMs, 100U * kMs), true, 100U * kMs);
        CHECK(reno->window() == (pacing ? 24000U : 12000U + 8U * kMss));
    }
}

TEST_CASE("congestion: HyStart++ enters its conservative phase on a rise in the round's minimum RTT, leaves it when "
          "the rise goes, and reaches avoidance after five rounds",
          "[net][net-path][congestion]")
{
    for (const bool resume : {false, true})
    {
        Controller reno(policy_of(CongestionAlgorithm::NewReno, true));
        // Round 1 is the first packet; round 2 the rest of the first flight, every sample 100 ms.
        reno.send(0U, 9U, 0U);
        for (u64 n = 0U; n <= 9U; ++n)
        {
            reno.ack(n, 0U, 100U * kMs, 100U * kMs);
        }
        CHECK(reno->window() == 24000U);
        // Round 3 at 120 ms: past 100 + 12.5 ms once eight samples are in, so CSS from the eighth on (a quarter).
        reno.send(10U, 19U, 100U * kMs);
        reno->on_limited(100U * kMs);
        reno.ack(10U, 100U * kMs, 220U * kMs, 120U * kMs); // ends round 2, whose minimum stays 100 ms
        CHECK(reno->state().phase == CongestionPhase::SlowStart);
        for (u64 n = 11U; n <= 17U; ++n)
        {
            reno.ack(n, 100U * kMs, 220U * kMs, 120U * kMs);
        }
        CHECK(reno->state().phase == CongestionPhase::SlowStart);
        CHECK(reno->window() == 33600U);
        reno.ack(18U, 100U * kMs, 220U * kMs, 120U * kMs);
        CHECK(reno->state().phase == CongestionPhase::ConservativeSlowStart);
        CHECK(reno->window() == 33900U);
        reno.ack(19U, 100U * kMs, 220U * kMs, 120U * kMs);
        CHECK(reno->window() == 34200U);
        CHECK(reno->state().slow_start_threshold == kCongestionMaxWindow);

        if (resume)
        {
            // A sample under the baseline once the round holds eight: the rise was not lasting.
            reno.send(20U, 29U, 220U * kMs);
            reno->on_limited(220U * kMs);
            reno.ack(20U, 220U * kMs, 320U * kMs, 100U * kMs);
            CHECK(reno->state().phase == CongestionPhase::SlowStart);
            CHECK(reno->window() == 35400U);
            continue;
        }
        // Five CSS rounds, a quarter of every acknowledged byte each, then avoidance with the threshold there.
        u64 now = 220U * kMs;
        for (u64 first = 20U; first <= 50U; first += 10U)
        {
            reno.send(first, first + 9U, now);
            reno->on_limited(now);
            for (u64 n = first; n <= first + 9U; ++n)
            {
                reno.ack(n, now, now + 120U * kMs, 120U * kMs);
            }
            now += 120U * kMs;
            CHECK(reno->state().phase == CongestionPhase::ConservativeSlowStart);
        }
        reno.send(60U, 69U, now);
        reno->on_limited(now);
        reno.ack(60U, now, now + 120U * kMs, 120U * kMs);
        CHECK(reno->state().phase == CongestionPhase::Avoidance);
        CHECK(reno->window() == 34200U + 41U * 300U);
        CHECK(reno->state().slow_start_threshold == reno->window());
        reno.ack(61U, now, now + 121U * kMs, 120U * kMs);
        CHECK(reno->window() == 46500U + kMss * kMss / 46500U);
        // A loss sets the threshold: HyStart++ runs in the first slow start alone, so a round of RTTs far above the
        // last round's grows by every byte.
        reno->on_lost(packet(62U, now), now + 200U * kMs);
        reno->on_persistent_congestion(now + 200U * kMs);
        reno.send(70U, 79U, now + 300U * kMs);
        for (u64 n = 70U; n <= 79U; ++n)
        {
            reno.ack(n, now + 300U * kMs, now + 600U * kMs, 300U * kMs);
        }
        CHECK(reno->state().phase == CongestionPhase::SlowStart);
        CHECK(reno->window() == 2U * kMss + 10U * kMss);
    }
}

TEST_CASE("congestion: CUBIC follows RFC 9438's curve from W_max, in the integers every platform computes alike",
          "[net][net-path][congestion]")
{
    // Window 120000 bytes, then a loss: W_max 120000, the window 0.7 of it, K = cbrt(36000 / 1200 / 0.4) s = 4.2172 s
    // (4318 steps of 2^-10 s). The windows below are what the per-acknowledgement rule of sections 4.2 to 4.5 gives;
    // the curve itself agrees with 0.4 * (t - K)^3 * 1200 + 120000 in doubles within one byte at every point checked
    // (86501 at t + RTT = 0.1 s, 105462 at 1.1 s, 119331 at 3.1 s, 120330 at 5.1 s, 123203 at 6.1 s).
    CongestionState seed;
    seed.window = 120000U;
    Controller cubic(policy_of(CongestionAlgorithm::Cubic), kMss, &seed);
    cubic->on_sent(packet(0U, kSecond), 120000U, kSecond);
    cubic->on_lost(packet(0U, kSecond), 2U * kSecond);
    CHECK(cubic->window() == 84000U);
    CHECK(cubic->state().slow_start_threshold == 84000U);
    const u64 start = 3U * kSecond;
    u64 number = 1U;
    const auto step = [&](u64 now)
    {
        cubic->on_limited(now);
        cubic->on_sent(packet(number, now), 60000U, now);
        cubic.ack(number, now, now, 100U * kMs);
        ++number;
    };
    step(start);
    CHECK(cubic->window() == 84035U);
    CHECK(cubic->state().phase == CongestionPhase::Avoidance);
    for (u64 i = 1U; i <= 600U; ++i)
    {
        step(start + i * 10U * kMs);
        if (i == 100U)
        {
            CHECK(cubic->window() == 95027U); // concave, toward W_max
        }
        if (i == 300U)
        {
            CHECK(cubic->window() == 114314U);
        }
        if (i == 422U)
        {
            CHECK(cubic->window() == 118320U); // the plateau at K
        }
    }
    CHECK(cubic->window() == 120814U); // convex past W_max
}

TEST_CASE("congestion: CUBIC's Reno-friendly estimate leads where the curve is slow, and fast convergence lowers W_max",
          "[net][net-path][congestion]")
{
    // A small window with a short RTT: the estimate grows 9/17 datagram per window acknowledged and leads the curve.
    CongestionState small;
    small.window = 12000U;
    Controller cubic(policy_of(CongestionAlgorithm::Cubic), kMss, &small);
    cubic->on_lost(packet(0U, 0U), 2U * kSecond);
    CHECK(cubic->window() == 8400U);
    for (u64 i = 0U; i <= 200U; ++i)
    {
        const u64 now = 3U * kSecond + i * kMs;
        cubic->on_limited(now);
        cubic->on_sent(packet(i + 1U, now), 6000U, now);
        cubic.ack(i + 1U, now, now, 10U * kMs, 10U * kMs);
        if (i == 0U)
        {
            CHECK(cubic->window() == 8490U);
        }
        if (i == 50U)
        {
            CHECK(cubic->window() == 12369U); // the estimate passed the window before the loss: alpha 1 from there
        }
    }
    CHECK(cubic->window() == 24206U);

    // A second loss below W_max: with fast convergence W_max is 0.85 of the window, without it the window.
    for (const bool fast : {true, false})
    {
        CongestionPolicy policy = policy_of(CongestionAlgorithm::Cubic);
        policy.fast_convergence = fast;
        CongestionState seed;
        seed.window = 120000U;
        Controller c(policy, kMss, &seed);
        c->on_lost(packet(0U, 0U), 2U * kSecond);
        c->on_limited(3U * kSecond);
        c.send(1U, 1U, 3U * kSecond, 60000U);
        c.ack(1U, 3U * kSecond, 3U * kSecond, 100U * kMs);
        CHECK(c->window() == 84035U);
        c->on_lost(packet(2U, 3U * kSecond), 3500U * kMs);
        CHECK(c->window() == 58824U);
        u64 n = 3U;
        const auto step = [&](u64 now)
        {
            c->on_limited(now);
            c.send(n, n, now, 60000U);
            c.ack(n, now, now, 100U * kMs);
            ++n;
        };
        step(4U * kSecond);
        CHECK(c->window() == (fast ? 58849U : 58864U));
        for (u64 i = 1U; i <= 300U; ++i)
        {
            step(4U * kSecond + i * 10U * kMs);
        }
        CHECK(c->window() == (fast ? 71053U : 82156U));
        CHECK(c->state().reductions == 2U);
    }
}

TEST_CASE("congestion: an idle period does not count toward CUBIC's curve", "[net][net-path][congestion]")
{
    u64 windows[2] = {};
    for (u64 idle = 0U; idle < 2U; ++idle)
    {
        CongestionState seed;
        seed.window = 120000U;
        Controller cubic(policy_of(CongestionAlgorithm::Cubic), kMss, &seed);
        cubic->on_lost(packet(0U, 0U), 2U * kSecond);
        cubic->on_limited(3U * kSecond);
        cubic.send(1U, 1U, 3U * kSecond, 60000U);
        cubic.ack(1U, 3U * kSecond, 3U * kSecond, 100U * kMs); // the epoch starts
        // Idle: the next packet leaves with nothing in flight 10 s later, and the curve's time stands still for it.
        const u64 gap = (idle == 1U) ? 10U * kSecond : 0U;
        cubic->on_limited(3U * kSecond + gap);
        cubic->on_sent(packet(2U, 3U * kSecond + gap), (idle == 1U) ? kMss : 60000U, 3U * kSecond + gap);
        cubic.ack(2U, 3U * kSecond + gap, 4U * kSecond + gap, 100U * kMs);
        windows[idle] = cubic->window();
    }
    CHECK(windows[0] == windows[1]);
}

TEST_CASE("congestion: a period whose every loss proved spurious is undone, and nothing else is",
          "[net][net-path][congestion]")
{
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::NewReno, CongestionAlgorithm::Cubic})
    {
        Controller ctl(policy_of(algorithm));
        ctl.send(0U, 9U, 0U);
        for (u64 n = 0U; n <= 4U; ++n)
        {
            ctl.ack(n, 0U, 100U * kMs, 100U * kMs);
        }
        const CongestionState before = ctl->state();
        CHECK(before.window == 18000U);
        ctl->on_lost(packet(5U, 0U), 200U * kMs);
        ctl->on_lost(packet(6U, 0U), 200U * kMs);
        CHECK(ctl->window() < 18000U);
        ctl->on_spurious(packet(5U, 0U), 200U * kMs, 210U * kMs);
        CHECK(ctl->window() < 18000U); // one of the two
        ctl->on_spurious(packet(6U, 0U), 200U * kMs, 211U * kMs);
        CHECK(ctl->window() == 18000U);
        CHECK(ctl->state().slow_start_threshold == kCongestionMaxWindow);
        CHECK(ctl->state().phase == CongestionPhase::SlowStart);
        CHECK(ctl->state().recovery_start_ns == kNeverNs);
        CHECK(ctl->state().undone == 1U);
        CHECK(ctl->state().reductions == 1U);

        // A later period: a spurious loss of the earlier one changes nothing.
        ctl->on_lost(packet(7U, 0U), 300U * kMs);
        const u64 reduced = ctl->window();
        CHECK(reduced < 18000U);
        ctl->on_spurious(packet(5U, 0U), 200U * kMs, 310U * kMs);
        CHECK(ctl->window() == reduced);
        // Persistent congestion is never undone.
        ctl->on_persistent_congestion(300U * kMs);
        ctl->on_spurious(packet(7U, 0U), 300U * kMs, 320U * kMs);
        CHECK(ctl->window() == 2U * kMss);

        // An ECN mark is a congestion event no acknowledgement undoes.
        Controller marked(policy_of(algorithm));
        marked.send(0U, 9U, 0U);
        marked->on_congestion(0U, 100U * kMs);
        CHECK(marked->window() < 12000U);
        CHECK(marked->state().reductions == 1U);
        marked->on_lost(packet(1U, 0U), 110U * kMs); // inside the mark's period: no second reduction
        CHECK(marked->state().reductions == 1U);
        marked->on_spurious(packet(1U, 0U), 110U * kMs, 120U * kMs);
        CHECK(marked->window() < 12000U);
        CHECK(marked->state().undone == 0U);
    }
}

TEST_CASE("congestion: a live swap carries the window, threshold and recovery period; a reset starts over",
          "[net][net-path][congestion]")
{
    CongestionState held;
    {
        Controller cubic(policy_of(CongestionAlgorithm::Cubic));
        cubic.send(0U, 9U, 0U);
        for (u64 n = 0U; n <= 9U; ++n)
        {
            cubic.ack(n, 0U, 100U * kMs, 100U * kMs);
        }
        cubic->on_lost(packet(10U, 50U * kMs), 200U * kMs);
        held = cubic->state();
        CHECK(held.window == 16800U);
        CHECK(held.phase == CongestionPhase::Recovery);
    }
    Controller reno(policy_of(CongestionAlgorithm::NewReno), kMss, &held);
    CHECK(reno->algorithm() == CongestionAlgorithm::NewReno);
    CHECK(reno->window() == held.window);
    CHECK(reno->state().slow_start_threshold == held.slow_start_threshold);
    CHECK(reno->state().recovery_start_ns == 200U * kMs);
    CHECK(reno->state().phase == CongestionPhase::Recovery);
    CHECK(reno->state().reductions == 1U);
    // Still in the period: a packet sent before it grows nothing and its loss reduces nothing.
    reno->on_limited(210U * kMs);
    reno.ack(11U, 150U * kMs, 210U * kMs, 100U * kMs);
    reno->on_lost(packet(12U, 150U * kMs), 210U * kMs);
    CHECK(reno->window() == held.window);
    CHECK(reno->state().reductions == 1U);

    // Seeds are held to the bounds.
    CongestionState tiny;
    tiny.window = 1U;
    CHECK(Controller(CongestionPolicy{}, kMss, &tiny)->window() == 2U * kMss);
    CongestionState vast;
    vast.window = kCongestionMaxWindow * 2U;
    CHECK(Controller(CongestionPolicy{}, kMss, &vast)->window() == kCongestionMaxWindow);

    reno->reset(300U * kMs);
    CHECK(reno->window() == 12000U);
    CHECK(reno->state().phase == CongestionPhase::SlowStart);
    CHECK(reno->state().recovery_start_ns == kNeverNs);
    CHECK(reno->state().slow_start_threshold == kCongestionMaxWindow);
}

TEST_CASE("pacer: credit accrues exactly at the rate up to the burst, a packet may run it into debt, and the release "
          "time pays it",
          "[net][net-path][congestion]")
{
    constexpr u64 kRate = 1200000U; // 1200 bytes a millisecond
    constexpr u64 kBurst = 12000U;
    Pacer pacer;
    CHECK(pacer.release_ns(0U, kRate, kBurst) == 0U);
    CHECK(pacer.credit(0U, kRate, kBurst) == static_cast<crd::i64>(kBurst));
    for (u32 i = 0U; i < 10U; ++i)
    {
        CHECK(pacer.release_ns(0U, kRate, kBurst) == 0U);
        pacer.on_sent(kMss, 0U, kRate, kBurst);
    }
    CHECK(pacer.credit(0U, kRate, kBurst) == 0);
    CHECK(pacer.release_ns(0U, kRate, kBurst) == 1U); // the first nanosecond of credit
    pacer.on_sent(kMss, 1U, kRate, kBurst);
    // 1200 bytes less 1.2 milli-bytes of debt: paid 999999 ns later, positive the nanosecond after.
    CHECK(pacer.release_ns(1U, kRate, kBurst) == 1000001U);
    CHECK(pacer.release_ns(999999U, kRate, kBurst) == 1000001U);
    CHECK(pacer.release_ns(1000001U, kRate, kBurst) == 1000001U);
    CHECK(pacer.credit(1000002U, kRate, kBurst) == 0); // 0.0024 bytes
    // Debt stops at the burst, so a run of probes cannot hold data back for long.
    for (u32 i = 0U; i < 100U; ++i)
    {
        pacer.on_sent(kMss, 2000000U, kRate, kBurst);
    }
    CHECK(pacer.credit(2000000U, kRate, kBurst) == -static_cast<crd::i64>(kBurst));
    CHECK(pacer.release_ns(2000000U, kRate, kBurst) == 2000000U + 10000001U);
    // A long idle refills the burst and no more, however long.
    CHECK(pacer.credit(2000000U + 3600U * kSecond, kRate, kBurst) == static_cast<crd::i64>(kBurst));
    CHECK(pacer.credit(~u64{0} - 1U, kRate, kBurst) == static_cast<crd::i64>(kBurst));
    // Unpaced: always now, and the burst is whole when pacing starts again.
    CHECK(pacer.release_ns(2000000U, 0U, kBurst) == 2000000U);
    pacer.on_sent(kMss, 2000000U, 0U, kBurst);
    CHECK(pacer.credit(2000000U, kRate, kBurst) == static_cast<crd::i64>(kBurst));
    // A smaller burst caps credit already held.
    pacer.on_sent(kMss, 2000000U, kRate, kBurst);
    CHECK(pacer.credit(2000000U, kRate, kBurst) == static_cast<crd::i64>(kBurst - kMss));
    CHECK(pacer.credit(2000000U, kRate, 2400U) == 2400);
}
