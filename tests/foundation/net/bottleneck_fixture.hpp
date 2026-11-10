#pragma once

// A simulated bottleneck that the congestion controller tests share (NET-PATH; docs/design/network-foundation.md#bbr
// and #prague): flows of one controller each, with its delivery rate sampler and pacer, driven as a connection drives
// them, over a link of a rate, a drop-tail buffer and a step-marking queue that one flow owns or several share, each
// flow with its own propagation delay and random loss.

#include <crd/net/congestion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <memory>

namespace crd::net::testing
{
inline constexpr u64 kBottleneckSecond = kNanosPerSecond;
inline constexpr u64 kBottleneckMss = 1200U;
inline constexpr u64 kInfiniteData = ~u64{0};

// A FIFO bottleneck of `rate` bytes per second with a `buffer` byte drop-tail queue. With `ce_queue` set, a
// step-marking AQM: CE on every packet that finds more than that many bytes queued (L4S's step, RFC 9332's 1 ms; a
// classic one's deeper), and the packets of every flow through it are ECN-capable, their feedback reaching on_ecn().
struct Link
{
    u64 rate = 0U;
    u64 buffer = 0U;
    u64 ce_queue = 0U;
    u64 ce_ramp = 0U; // 0: a step; else CE with a probability rising from 0 at ce_queue to 1 this many bytes above it
    u64 free_ns = 0U; // when the last queued packet has left

    [[nodiscard]] u64 queue_bytes(u64 now_ns) const
    {
        return (free_ns > now_ns) ? (free_ns - now_ns) * rate / kBottleneckSecond : 0U;
    }
};

// A sender with one controller, its sampler and pacer, over a link it owns or shares, `one_way` propagation each way,
// with every packet acknowledged at once by its own ACK frame. A dropped packet is declared lost when the ACK of the
// next packet arrives (or a probe timeout after the last), as RFC 9002's packet threshold would within a few packets.
class Flow
{
public:
    explicit Flow(const CongestionPolicy& policy, u64 rate, u64 one_way_ns, u64 buffer, u64 entropy = 7U)
        : m_own{rate, buffer}, m_link(&m_own), m_one_way_ns(one_way_ns)
    {
        ctl = make_congestion(policy, kBottleneckMss, nullptr, m_slot, delivery, entropy);
        REQUIRE(ctl != nullptr);
    }
    // A flow through `link`, which outlives it and which other flows may share (run_flows).
    Flow(const CongestionPolicy& policy, Link& link, u64 one_way_ns, u64 entropy = 7U)
        : m_link(&link), m_one_way_ns(one_way_ns)
    {
        ctl = make_congestion(policy, kBottleneckMss, nullptr, m_slot, delivery, entropy);
        REQUIRE(ctl != nullptr);
    }
    ~Flow() { ctl->~CongestionController(); }
    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;

    // Replaces the controller live, as QuicConnection::set_congestion does.
    void swap(const CongestionPolicy& policy, u64 entropy = 9U)
    {
        const CongestionState held = ctl->state();
        ctl->~CongestionController();
        ctl = make_congestion(policy, kBottleneckMss, &held, m_slot, delivery, entropy);
        REQUIRE(ctl != nullptr);
    }

    void set_rate(u64 rate) { m_link->rate = rate; }
    void set_loss(u32 permille) { m_loss = permille; }
    // The link's step-marking threshold (Link::ce_queue); 0 marks nothing and turns ECN off.
    void set_ce_queue(u64 bytes) { m_link->ce_queue = bytes; }
    // Bytes the application has written so far; kInfiniteData for a bulk transfer.
    void set_data(u64 bytes) { m_data = bytes; }
    void add_data(u64 bytes) { m_data += bytes; }

    // Runs until `until_ns`, calling `watch` after each ACK frame.
    template <typename Watch> void run(u64 until_ns, Watch watch)
    {
        while (now < until_ns)
        {
            const u64 next = next_event_ns();
            if (next >= until_ns)
            {
                now = until_ns;
                return;
            }
            if (step(next))
            {
                watch();
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

    // The bottleneck's queue in bytes now.
    [[nodiscard]] u64 queue_bytes() const { return m_link->queue_bytes(now); }

    // When this flow's next event comes, an ACK frame or a send; kNeverNs for none. It reads the controller now.
    [[nodiscard]] u64 next_event_ns()
    {
        m_ack_ns = (m_head != m_tail) ? event_ns(m_flight[m_head % kFlight]) : kNeverNs;
        m_send_ns = next_send_ns();
        return (m_ack_ns < m_send_ns) ? m_ack_ns : m_send_ns;
    }

    // Moves to `at_ns`, the time the last next_event_ns() returned, and handles that event: true for an ACK frame.
    bool step(u64 at_ns)
    {
        now = (at_ns > now) ? at_ns : now;
        if (m_ack_ns <= m_send_ns)
        {
            settle();
            return true;
        }
        transmit();
        return false;
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
        const bool data = sent_bytes + kBottleneckMss <= m_data || m_data == kInfiniteData;
        if (!data)
        {
            delivery.on_app_limited(in_flight);
            return kNeverNs;
        }
        if (in_flight + kBottleneckMss > ctl->window())
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
        f.packet.bytes = kBottleneckMss;
        f.packet.number = m_number++;
        f.packet.sent_ns = now;
        f.packet.space = QuicSpace::Application;
        in_flight += kBottleneckMss;
        sent_bytes += kBottleneckMss;
        f.packet.delivery = delivery.on_sent(kBottleneckMss, in_flight, now);
        ctl->on_sent(f.packet, in_flight, now);
        m_pacer.on_sent(kBottleneckMss, now, ctl->pacing_rate(rtt), ctl->burst());
        const u64 queue = queue_bytes();
        max_queue = (queue > max_queue) ? queue : max_queue;
        m_seed = m_seed * 6364136223846793005U + 1442695040888963407U;
        const bool random_loss = m_loss != 0U && (m_seed >> 33U) % 1000U < m_loss;
        const u64 start = (m_link->free_ns > now) ? m_link->free_ns : now;
        if (queue + kBottleneckMss > m_link->buffer || random_loss)
        {
            f.dropped = true;
            f.ack_ns = start + 2U * m_one_way_ns;
            ++dropped;
        }
        else
        {
            m_link->free_ns = start + kBottleneckMss * kBottleneckSecond / m_link->rate;
            f.ack_ns = m_link->free_ns + 2U * m_one_way_ns;
            const u64 over = (m_link->ce_queue != 0U && queue > m_link->ce_queue) ? queue - m_link->ce_queue : 0U;
            f.ce = over != 0U && (m_link->ce_ramp == 0U || (m_seed >> 11U) % m_link->ce_ramp < over);
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
        if (m_link->ce_queue != 0U)
        {
            marked += f.ce ? 1U : 0U;
            ctl->on_ecn(1U, f.ce ? 1U : 0U, f.packet.sent_ns, now);
        }
        ctl->on_ack_end(rtt, true, in_flight, now);
    }

    Link m_own;
    Link* m_link;
    u64 m_one_way_ns;
    u64 m_data = kInfiniteData;
    u32 m_loss = 0U;
    u64 m_seed = 0x853C49E6748FEA9BU;
    u64 m_ack_ns = kNeverNs;
    u64 m_send_ns = kNeverNs;
    u64 m_number = 0U;
    u64 m_head = 0U;
    u64 m_tail = 0U;
    std::unique_ptr<Flight[]> m_flight = std::make_unique<Flight[]>(kFlight);
    CongestionSlot m_slot;
    Pacer m_pacer;
};

// Runs flows that share a link until `until_ns`, every event in time order (a tie to the earlier flow), calling
// `watch` after each ACK frame. A flow's next event reads only its own state, so it holds until that flow steps.
template <typename Watch> void run_flows(std::initializer_list<Flow*> flows, u64 until_ns, Watch watch)
{
    for (;;)
    {
        Flow* first = nullptr;
        u64 next = kNeverNs;
        for (Flow* flow : flows)
        {
            const u64 at = flow->next_event_ns();
            if (at < next)
            {
                next = at;
                first = flow;
            }
        }
        if (next >= until_ns)
        {
            for (Flow* flow : flows)
            {
                flow->now = until_ns;
            }
            return;
        }
        if (first->step(next))
        {
            watch();
        }
    }
}

} // namespace crd::net::testing
