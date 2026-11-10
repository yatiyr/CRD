#pragma once

// A simulated bottleneck that the congestion controller tests share (NET-PATH; docs/design/network-foundation.md#bbr
// and #prague): one controller with its delivery rate sampler and pacer, driven as a connection drives them, over a
// rate, a drop-tail buffer, a propagation delay, random loss and a step-marking queue.

#include <crd/net/congestion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace crd::net::testing
{
inline constexpr u64 kBottleneckSecond = kNanosPerSecond;
inline constexpr u64 kBottleneckMss = 1200U;
inline constexpr u64 kInfiniteData = ~u64{0};

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
        ctl = make_congestion(policy, kBottleneckMss, nullptr, m_slot, delivery, entropy);
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
        ctl = make_congestion(policy, kBottleneckMss, &held, m_slot, delivery, entropy);
        REQUIRE(ctl != nullptr);
    }

    void set_rate(u64 rate) { m_rate = rate; }
    void set_loss(u32 permille) { m_loss = permille; }
    // CE on every packet that finds more than `bytes` queued: a step-marking AQM (L4S's, RFC 9332's step at 1 ms; a
    // classic one's deeper). With a threshold, packets are ECN-capable and feedback reaches on_ecn().
    void set_ce_queue(u64 bytes)
    {
        m_ce_queue = bytes;
        m_ecn = bytes != 0U;
    }
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
        return (m_link_free_ns > now) ? (m_link_free_ns - now) * m_rate / kBottleneckSecond : 0U;
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
        const u64 start = (m_link_free_ns > now) ? m_link_free_ns : now;
        if (queue + kBottleneckMss > m_buffer || random_loss)
        {
            f.dropped = true;
            f.ack_ns = start + 2U * m_one_way_ns;
            ++dropped;
        }
        else
        {
            m_link_free_ns = start + kBottleneckMss * kBottleneckSecond / m_rate;
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
        if (m_ecn)
        {
            marked += f.ce ? 1U : 0U;
            ctl->on_ecn(1U, f.ce ? 1U : 0U, f.packet.sent_ns, now);
        }
        ctl->on_ack_end(rtt, true, in_flight, now);
    }

    u64 m_rate;
    u64 m_one_way_ns;
    u64 m_buffer;
    u64 m_data = kInfiniteData;
    u32 m_loss = 0U;
    u64 m_ce_queue = 0U;
    bool m_ecn = false;
    u64 m_seed = 0x853C49E6748FEA9BU;
    u64 m_link_free_ns = 0U;
    u64 m_number = 0U;
    u64 m_head = 0U;
    u64 m_tail = 0U;
    std::unique_ptr<Flight[]> m_flight = std::make_unique<Flight[]>(kFlight);
    CongestionSlot m_slot;
    Pacer m_pacer;
};

} // namespace crd::net::testing
