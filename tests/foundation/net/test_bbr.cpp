// crd-net -- BBRv3 (NET-PATH; docs/design/network-foundation.md#bbr, draft-ietf-ccwg-bbr-07): the controller and the
// delivery rate sampler driven as a connection drives them, over a simulated bottleneck (a rate, a drop-tail buffer,
// a propagation delay, random loss and a CE-marking queue): Startup finding the bottleneck rate and the round trip,
// Drain emptying the queue it built, ProbeBW cycling through its four phases with a short queue and a full link,
// ProbeRTT every ten seconds at half the BDP for at least 200 ms, a rate that halves and doubles followed, random loss
// under the threshold kept at full rate where CUBIC falls, a shallow buffer's losses bounding the inflight, an
// application-limited sender that lowers nothing, ECN marks treated as congestion, a spurious loss episode undone,
// persistent congestion, live swaps both ways, the policy rules and a deterministic run.

#include <crd/net/congestion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kSecond = 1000U * kMs;
constexpr u64 kMss = 1200U;
constexpr u64 kInfiniteData = ~u64{0};

CongestionPolicy bbr_policy()
{
    CongestionPolicy policy;
    policy.algorithm = CongestionAlgorithm::Bbr;
    return policy;
}

// A sender with one controller, its sampler and pacer, over a FIFO bottleneck of `rate` bytes per second and a
// `buffer` byte queue, `one_way` propagation each way, with every packet acknowledged at once by its own ACK frame.
// A dropped packet is declared lost when the ACK of the next packet arrives (or a probe timeout after the last), as
// RFC 9002's packet threshold would within a few packets.
class Path
{
public:
    explicit Path(const CongestionPolicy& policy, u64 rate, u64 one_way_ns, u64 buffer, u64 entropy = 7U)
        : m_rate(rate), m_one_way_ns(one_way_ns), m_buffer(buffer)
    {
        ctl = make_congestion(policy, kMss, nullptr, m_slot, delivery, entropy);
        REQUIRE(ctl != nullptr);
    }
    ~Path() { ctl->~CongestionController(); }
    Path(const Path&) = delete;
    Path& operator=(const Path&) = delete;

    // Replaces the controller live, as QuicConnection::set_congestion does.
    void swap(const CongestionPolicy& policy, u64 entropy = 9U)
    {
        const CongestionState held = ctl->state();
        ctl->~CongestionController();
        ctl = make_congestion(policy, kMss, &held, m_slot, delivery, entropy);
        REQUIRE(ctl != nullptr);
    }

    void set_rate(u64 rate) { m_rate = rate; }
    void set_loss(u32 permille) { m_loss = permille; }
    void set_ce_queue(u64 bytes) { m_ce_queue = bytes; }
    // Bytes the application has written so far; kInfiniteData for a bulk transfer.
    void set_data(u64 bytes) { m_data = bytes; }
    void add_data(u64 bytes) { m_data += bytes; }

    // Runs until `until_ns`, calling `watch` after each ACK frame.
    template <typename Watch> void run(u64 until_ns, Watch watch)
    {
        while (now < until_ns)
        {
            const u64 ack = (m_head != m_tail) ? event_ns(m_flight[m_head % kFlight]) : kNeverNs;
            const u64 send = next_send_ns();
            const u64 next = (ack < send) ? ack : send;
            if (next >= until_ns || next == kNeverNs)
            {
                now = until_ns;
                return;
            }
            now = (next > now) ? next : now;
            if (ack <= send)
            {
                settle();
                watch();
            }
            else
            {
                transmit();
            }
        }
    }

    void run(u64 until_ns)
    {
        run(until_ns, [] {});
    }

    CongestionController* ctl = nullptr;
    DeliveryRateSampler delivery;
    QuicRtt rtt;
    u64 now = 0U;
    u64 in_flight = 0U;
    u64 sent_bytes = 0U;
    u64 acked_bytes = 0U;
    u64 lost_bytes = 0U;
    u64 dropped = 0U;
    u64 marked = 0U;
    u64 max_queue = 0U;

    // The bottleneck's queue in bytes at `now_ns`.
    [[nodiscard]] u64 queue_bytes() const
    {
        return (m_link_free_ns > now) ? (m_link_free_ns - now) * m_rate / kSecond : 0U;
    }

private:
    static constexpr usize kFlight = 8192U;

    struct Flight
    {
        CongestionPacket packet;
        u64 ack_ns = 0U;
        bool dropped = false;
        bool ce = false;
    };

    // A delivered packet's ACK arrives at ack_ns; a dropped one is found lost at the next ACK, or after a probe
    // timeout when it is the last.
    [[nodiscard]] u64 event_ns(const Flight& f) const
    {
        if (!f.dropped)
        {
            return f.ack_ns;
        }
        for (u64 i = m_head + 1U; i != m_tail; ++i)
        {
            const Flight& later = m_flight[i % kFlight];
            if (!later.dropped)
            {
                return (later.ack_ns > f.ack_ns) ? later.ack_ns : f.ack_ns;
            }
        }
        return f.ack_ns + 3U * rtt.smoothed_ns / 2U;
    }

    [[nodiscard]] u64 next_send_ns()
    {
        const bool data = sent_bytes + kMss <= m_data || m_data == kInfiniteData;
        if (!data)
        {
            delivery.on_app_limited(in_flight);
            return kNeverNs;
        }
        if (in_flight + kMss > ctl->window())
        {
            ctl->on_limited(true, now);
            return kNeverNs;
        }
        if (m_tail - m_head == kFlight)
        {
            return kNeverNs;
        }
        const u64 release = m_pacer.release_ns(now, ctl->pacing_rate(rtt), ctl->burst());
        if (release > now)
        {
            ctl->on_limited(false, now);
        }
        return release;
    }

    void transmit()
    {
        Flight& f = m_flight[m_tail % kFlight];
        f = Flight{};
        f.packet.bytes = kMss;
        f.packet.number = m_number++;
        f.packet.sent_ns = now;
        f.packet.space = QuicSpace::Application;
        in_flight += kMss;
        sent_bytes += kMss;
        f.packet.delivery = delivery.on_sent(kMss, in_flight, now);
        ctl->on_sent(f.packet, in_flight, now);
        m_pacer.on_sent(kMss, now, ctl->pacing_rate(rtt), ctl->burst());
        const u64 queue = queue_bytes();
        max_queue = (queue > max_queue) ? queue : max_queue;
        m_seed = m_seed * 6364136223846793005U + 1442695040888963407U;
        const bool random_loss = m_loss != 0U && (m_seed >> 33U) % 1000U < m_loss;
        const u64 start = (m_link_free_ns > now) ? m_link_free_ns : now;
        if (queue + kMss > m_buffer || random_loss)
        {
            f.dropped = true;
            f.ack_ns = start + 2U * m_one_way_ns;
            ++dropped;
        }
        else
        {
            m_link_free_ns = start + kMss * kSecond / m_rate;
            f.ack_ns = m_link_free_ns + 2U * m_one_way_ns;
            f.ce = m_ce_queue != 0U && queue > m_ce_queue;
        }
        ++m_tail;
    }

    void settle()
    {
        Flight f = m_flight[m_head % kFlight];
        ++m_head;
        in_flight -= f.packet.bytes;
        if (f.dropped)
        {
            lost_bytes += f.packet.bytes;
            delivery.on_lost(f.packet.bytes);
            ctl->on_lost(f.packet, now);
            return;
        }
        acked_bytes += f.packet.bytes;
        delivery.on_ack_begin();
        delivery.on_acked(f.packet.delivery, f.packet.bytes, f.packet.sent_ns, f.packet.number, now);
        ctl->on_acked(f.packet, now);
        rtt.sample(now - f.packet.sent_ns, 0U, true, 0U, now);
        delivery.on_ack_end(rtt.min_ns);
        if (f.ce)
        {
            ++marked;
            ctl->on_congestion(f.packet.sent_ns, now);
        }
        ctl->on_ack_end(rtt, true, in_flight, now);
    }

    u64 m_rate;
    u64 m_one_way_ns;
    u64 m_buffer;
    u64 m_data = kInfiniteData;
    u32 m_loss = 0U;
    u64 m_ce_queue = 0U;
    u64 m_seed = 0x853C49E6748FEA9BU;
    u64 m_link_free_ns = 0U;
    u64 m_number = 0U;
    u64 m_head = 0U;
    u64 m_tail = 0U;
    std::unique_ptr<Flight[]> m_flight = std::make_unique<Flight[]>(kFlight);
    CongestionSlot m_slot;
    Pacer m_pacer;
};

// Bit `phase` set for every phase seen.
u32 bit(CongestionPhase phase)
{
    return u32{1} << static_cast<u32>(phase);
}

// 10 MB/s (80 Mbit/s), 40 ms round trip: a 400 KB BDP (333 packets), a buffer of one BDP.
constexpr u64 kRate = 10000000U;
constexpr u64 kOneWay = 20U * kMs;
constexpr u64 kBdp = kRate / 1000U * 40U;

} // namespace

TEST_CASE("BBR: the policy rules, the initial window and pacing before any sample", "[net][net-path][bbr]")
{
    CHECK(congestion_policy_valid(bbr_policy()));
    CongestionPolicy unpaced = bbr_policy();
    unpaced.pacing = false;
    CHECK_FALSE(congestion_policy_valid(unpaced));
    CongestionPolicy old = bbr_policy();
    old.version = 1U;
    CHECK_FALSE(congestion_policy_valid(old));
    CongestionPolicy cubic_v1;
    cubic_v1.version = 1U;
    CHECK(congestion_policy_valid(cubic_v1)); // a version 1 policy still runs
    CongestionPolicy none = bbr_policy();
    none.version = 0U;
    CHECK_FALSE(congestion_policy_valid(none));

    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    CHECK(path.ctl->algorithm() == CongestionAlgorithm::Bbr);
    CHECK(path.ctl->window() == 10U * kMss);
    const CongestionState state = path.ctl->state();
    CHECK(state.phase == CongestionPhase::SlowStart);
    CHECK(state.bandwidth == 0U);
    CHECK(state.min_rtt_ns == 0U);
    // InitPacingRate: 2.77 initial windows a round trip; 1 ms before a sample, the smoothed RTT after.
    QuicRtt rtt;
    CHECK(path.ctl->pacing_rate(rtt) == 10U * kMss * 1000U * 277U / 100U);
    rtt.sample(100U * kMs, 0U, true, 0U, 0U);
    CHECK(path.ctl->pacing_rate(rtt) == 10U * kMss * 10U * 277U / 100U);
    CHECK(path.ctl->burst() == 10U * kMss);
}

TEST_CASE("BBR: Startup finds the bottleneck and the round trip, Drain empties its queue, ProbeBW cycles",
          "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    u32 seen = 0U;
    u64 drain_left_ns = kNeverNs;
    path.run(2U * kSecond,
             [&]
             {
                 const CongestionState s = path.ctl->state();
                 seen |= bit(s.phase);
                 if (drain_left_ns == kNeverNs && (seen & bit(CongestionPhase::Drain)) != 0U &&
                     s.phase != CongestionPhase::Drain)
                 {
                     drain_left_ns = path.now;
                 }
             });
    const CongestionState s = path.ctl->state();
    CHECK((seen & bit(CongestionPhase::Drain)) != 0U);
    CHECK(drain_left_ns < kSecond); // Startup and Drain take a handful of round trips
    // The model: the bottleneck rate within 5%, the propagation round trip within a millisecond.
    CHECK(s.bandwidth >= kRate * 95U / 100U);
    CHECK(s.bandwidth <= kRate * 105U / 100U);
    CHECK(s.min_rtt_ns >= 40U * kMs);
    CHECK(s.min_rtt_ns <= 41U * kMs);
    // Startup's loss: a buffer of one BDP holds its overshoot, so nothing or little is dropped.
    CHECK(path.lost_bytes * 50U <= path.sent_bytes);

    // Steady state: the link stays full and the queue short.
    const u64 acked_before = path.acked_bytes;
    const u64 start = path.now;
    u64 queue_sum = 0U;
    u64 samples = 0U;
    path.run(start + 5U * kSecond,
             [&]
             {
                 queue_sum += path.queue_bytes();
                 ++samples;
                 seen |= bit(path.ctl->state().phase);
             });
    // ProbeBW's four phases, each probe 2 to 3 s apart (DOWN may last one ACK when Drain left no queue).
    CHECK((seen & bit(CongestionPhase::ProbeBwDown)) != 0U);
    CHECK((seen & bit(CongestionPhase::ProbeBwCruise)) != 0U);
    CHECK((seen & bit(CongestionPhase::ProbeBwRefill)) != 0U);
    CHECK((seen & bit(CongestionPhase::ProbeBwUp)) != 0U);
    const u64 goodput = (path.acked_bytes - acked_before) * kSecond / (path.now - start);
    CHECK(goodput >= kRate * 93U / 100U);
    CHECK(queue_sum / samples <= kBdp / 4U); // under a quarter of a BDP queued on average
    CHECK(path.lost_bytes * 100U <= path.sent_bytes);
}

TEST_CASE("BBR: ProbeRTT every ten seconds holds half the BDP for at least 200 ms, then the model resumes",
          "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    u64 entered_ns = kNeverNs;
    u64 left_ns = kNeverNs;
    u64 peak_window = 0U;
    u64 peak_in_flight = 0U;
    path.run(13U * kSecond,
             [&]
             {
                 const CongestionState s = path.ctl->state();
                 if (s.phase == CongestionPhase::ProbeRtt)
                 {
                     entered_ns = (entered_ns == kNeverNs) ? path.now : entered_ns;
                     peak_window = (s.window > peak_window) ? s.window : peak_window;
                 }
                 else if (entered_ns != kNeverNs && left_ns == kNeverNs)
                 {
                     left_ns = path.now;
                 }
                 if (entered_ns != kNeverNs && left_ns == kNeverNs && path.now > entered_ns + 50U * kMs)
                 {
                     peak_in_flight = (path.in_flight > peak_in_flight) ? path.in_flight : peak_in_flight;
                 }
             });
    REQUIRE(entered_ns != kNeverNs);
    CHECK(entered_ns >= 10U * kSecond);
    CHECK(entered_ns <= 11U * kSecond);
    REQUIRE(left_ns != kNeverNs);
    CHECK(left_ns - entered_ns >= 200U * kMs);
    CHECK(left_ns - entered_ns <= 400U * kMs);
    CHECK(peak_window <= kBdp / 2U + 4U * kMss);
    CHECK(peak_in_flight <= kBdp / 2U + 4U * kMss);
    const CongestionState s = path.ctl->state();
    CHECK(s.phase != CongestionPhase::ProbeRtt);
    CHECK(s.bandwidth >= kRate * 9U / 10U);
}

TEST_CASE("BBR: the model follows a bottleneck that halves and then doubles", "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    path.run(3U * kSecond);
    path.set_rate(kRate / 2U);
    path.run(12U * kSecond); // the max filter forgets the old rate within two ProbeBW cycles
    CHECK(path.ctl->state().bandwidth <= kRate / 2U * 105U / 100U);
    CHECK(path.ctl->state().bandwidth >= kRate / 2U * 90U / 100U);
    path.set_rate(kRate);
    const u64 before = path.acked_bytes;
    path.run(18U * kSecond); // ProbeBW_UP finds the new rate
    CHECK(path.ctl->state().bandwidth >= kRate * 90U / 100U);
    CHECK((path.acked_bytes - before) / 6U >= kRate * 80U / 100U);
}

TEST_CASE("BBR: random loss under the threshold keeps the rate, where CUBIC falls", "[net][net-path][bbr]")
{
    const auto goodput = [](const CongestionPolicy& policy)
    {
        Path path(policy, kRate, kOneWay, kBdp);
        path.set_loss(10U); // 1%
        path.run(2U * kSecond);
        const u64 before = path.acked_bytes;
        path.run(12U * kSecond);
        return (path.acked_bytes - before) / 10U;
    };
    const u64 bbr = goodput(bbr_policy());
    const u64 cubic = goodput(CongestionPolicy{});
    // BBRv3 answers each lossy round by its short-term bounds, so 1% loss costs it about a fifth of the link (82% in
    // this run); CUBIC's window halves at each loss.
    CHECK(bbr >= kRate * 75U / 100U);
    CHECK(bbr > 3U * cubic);
}

TEST_CASE("BBR: a shallow buffer's losses bound the inflight and keep the loss rate low", "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, kBdp / 10U);
    path.run(2U * kSecond);
    const u64 sent = path.sent_bytes;
    const u64 lost = path.lost_bytes;
    const u64 acked = path.acked_bytes;
    path.run(12U * kSecond);
    // After Startup's overshoot: losses stay near the 2% the probes allow, and the link stays well used.
    CHECK((path.lost_bytes - lost) * 100U <= 3U * (path.sent_bytes - sent));
    CHECK((path.acked_bytes - acked) / 10U >= kRate * 85U / 100U);
    CHECK(path.ctl->state().reductions > 0U);
}

TEST_CASE("BBR: an application-limited sender lowers nothing, and a restart from idle paces at the model",
          "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    path.run(3U * kSecond);
    const u64 bandwidth = path.ctl->state().bandwidth;
    REQUIRE(bandwidth >= kRate * 9U / 10U);
    // A tenth of the rate for four seconds: app-limited samples cannot lower the estimate.
    path.set_data(path.sent_bytes);
    for (u64 t = 0U; t < 40U; ++t)
    {
        path.add_data(kRate / 100U);
        path.run(3U * kSecond + (t + 1U) * 100U * kMs);
    }
    const CongestionState s = path.ctl->state();
    CHECK(s.bandwidth >= bandwidth * 9U / 10U);
    // Bulk again: the link fills at once.
    path.set_data(kInfiniteData);
    const u64 before = path.acked_bytes;
    path.run(8U * kSecond);
    CHECK((path.acked_bytes - before) >= kRate * 85U / 100U);
}

TEST_CASE("BBR: ECN marks are congestion: Startup ends and the model's bounds fall", "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, 4U * kBdp);
    path.set_ce_queue(kBdp / 8U); // a queue past an eighth of a BDP marks
    u64 left_startup_ns = kNeverNs;
    path.run(4U * kSecond,
             [&]
             {
                 if (left_startup_ns == kNeverNs && path.ctl->state().phase != CongestionPhase::SlowStart &&
                     path.ctl->state().phase != CongestionPhase::Recovery)
                 {
                     left_startup_ns = path.now;
                 }
             });
    CHECK(path.marked > 0U);
    CHECK(path.dropped == 0U);
    REQUIRE(left_startup_ns != kNeverNs);
    CHECK(left_startup_ns < kSecond);
    CHECK(path.ctl->state().reductions > 0U);
    // The buffer is four BDPs deep, yet the marks keep the queue near their threshold.
    CHECK(path.max_queue <= 2U * kBdp);
    const u64 before = path.acked_bytes;
    u64 queue_sum = 0U;
    u64 samples = 0U;
    path.run(10U * kSecond,
             [&]
             {
                 queue_sum += path.queue_bytes();
                 ++samples;
             });
    CHECK(queue_sum / samples <= kBdp / 2U);
    CHECK((path.acked_bytes - before) / 6U >= kRate * 85U / 100U);
}

TEST_CASE("BBR: a spurious loss episode is undone, persistent congestion takes the minimum window",
          "[net][net-path][bbr]")
{
    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    path.run(3U * kSecond);
    const CongestionState before = path.ctl->state();
    REQUIRE(before.phase != CongestionPhase::Recovery);
    // A packet declared lost and then acknowledged: the episode's response is undone and recovery ends.
    CongestionPacket packet;
    packet.bytes = kMss;
    packet.number = 1U << 30U;
    packet.sent_ns = path.now;
    packet.space = QuicSpace::Application;
    packet.delivery.in_flight = path.in_flight;
    packet.delivery.lost = path.delivery.lost();
    path.ctl->on_lost(packet, path.now + 1U);
    CHECK(path.ctl->state().phase == CongestionPhase::Recovery);
    CHECK(path.ctl->state().recovery_start_ns == path.now + 1U);
    path.ctl->on_spurious(packet, path.now + 1U, path.now + 2U);
    const CongestionState undone = path.ctl->state();
    CHECK(undone.phase != CongestionPhase::Recovery);
    CHECK(undone.window >= before.window);
    CHECK(undone.undone == before.undone + ((undone.reductions > before.reductions) ? 1U : 0U));
    // A loss of a packet sent before the recovery began starts no new episode.
    const u64 reductions = undone.reductions;
    packet.sent_ns = path.now;
    path.ctl->on_lost(packet, path.now + 3U);
    CHECK(path.ctl->state().recovery_start_ns == path.now + 1U);
    CHECK(path.ctl->state().reductions == reductions);

    path.ctl->on_persistent_congestion(path.now + 4U);
    CHECK(path.ctl->window() == 2U * kMss);
    CHECK(path.ctl->state().reductions == reductions + 1U);
    // The next ACK frame lifts it to BBR's floor of four datagrams, and the run recovers its rate.
    path.run(path.now + 3U * kSecond);
    CHECK(path.ctl->window() >= 4U * kMss);
    CHECK(path.ctl->state().bandwidth >= kRate * 9U / 10U);
}

TEST_CASE("BBR: live swaps keep the window both ways, and the model starts from the delivery the sampler holds",
          "[net][net-path][bbr]")
{
    Path path(CongestionPolicy{}, kRate, kOneWay, kBdp);
    path.run(2U * kSecond);
    const CongestionState cubic = path.ctl->state();
    path.swap(bbr_policy());
    CHECK(path.ctl->algorithm() == CongestionAlgorithm::Bbr);
    CHECK(path.ctl->window() == ((cubic.window > 4U * kMss) ? cubic.window : 4U * kMss));
    CHECK(path.ctl->state().reductions == cubic.reductions);
    CHECK(path.ctl->state().recovery_start_ns == cubic.recovery_start_ns);
    path.run(5U * kSecond);
    CHECK(path.ctl->state().bandwidth >= kRate * 9U / 10U);
    const CongestionState bbr = path.ctl->state();
    path.swap(CongestionPolicy{});
    CHECK(path.ctl->algorithm() == CongestionAlgorithm::Cubic);
    CHECK(path.ctl->window() == bbr.window);
    CHECK(path.ctl->state().slow_start_threshold == bbr.slow_start_threshold); // BBR left Startup: avoidance
    CHECK(path.ctl->state().phase != CongestionPhase::SlowStart);
    path.run(7U * kSecond);
    CHECK(path.acked_bytes > 0U);
}

TEST_CASE("BBR: one entropy gives one run, another moves the probe times; a Retry starts over",
          "[net][net-path][bbr]")
{
    const auto trace = [](u64 entropy)
    {
        Path path(bbr_policy(), kRate, kOneWay, kBdp, entropy);
        u64 hash = 0xCBF29CE484222325U;
        path.run(6U * kSecond,
                 [&]
                 {
                     const CongestionState s = path.ctl->state();
                     hash = (hash ^ (s.window + static_cast<u64>(s.phase) * 7919U + s.bandwidth)) * 0x100000001B3U;
                 });
        return hash;
    };
    CHECK(trace(1U) == trace(1U));
    CHECK(trace(1U) != trace(2U));

    Path path(bbr_policy(), kRate, kOneWay, kBdp);
    path.run(2U * kSecond);
    path.ctl->reset(path.now);
    const CongestionState s = path.ctl->state();
    CHECK(s.window == 10U * kMss);
    CHECK(s.phase == CongestionPhase::SlowStart);
    CHECK(s.bandwidth == 0U);
    CHECK(s.min_rtt_ns == 0U);
    CHECK(s.recovery_start_ns == kNeverNs);
}

TEST_CASE("BBR: a larger datagram moves the initial window and the floor with it", "[net][net-path][bbr]")
{
    CongestionSlot slot;
    DeliveryRateSampler delivery;
    CongestionController* ctl = make_congestion(bbr_policy(), kMss, nullptr, slot, delivery, 0U);
    REQUIRE(ctl != nullptr);
    ctl->set_max_datagram(1452U);
    CHECK(ctl->window() == 10U * 1452U);
    ctl->on_persistent_congestion(0U);
    CHECK(ctl->window() == 2U * 1452U);
    ctl->~CongestionController();
}
