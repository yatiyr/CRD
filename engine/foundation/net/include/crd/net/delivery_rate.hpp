#pragma once

// crd-net -- delivery rate sampling (NET-PATH; contract: docs/design/network-foundation.md#bbr).
// Source: draft-ietf-ccwg-bbr-07 section 4.1 (after Cheng, Cardwell, Yeganeh, Jacobson, Delivery Rate Estimation).
//
//   stamps     every packet counted in flight carries a DeliveryStamp of the connection's delivery when it left: what
//              was delivered and when, when the newest packet acknowledged by then left, what was lost, the bytes in
//              flight with it, and whether sending was application limited.
//   samples    each ACK frame yields one DeliverySample from its most recently sent acknowledged packet: the bytes
//              delivered over the longer of that packet's send and acknowledgement intervals (an ACK burst cannot read
//              faster than the data left), refused as a rate when that interval is under the minimum RTT.
//   owner      the connection holds one sampler beside its recovery, so every controller (BBRv3, Prague, NET-LIVE's
//              delay-based ones) and a live swap between them read the same stamps; the rate itself is
//              crd-hesap-net's delivery_rate(), in exact integers.
//   threads    a sampler belongs to its connection's thread; nothing allocates.

#include <crd/core/types.hpp>

namespace crd::net
{
// The connection's delivery when a packet counted in flight left (section 4.1.2.2's P state).
struct DeliveryStamp
{
    u64 delivered = 0U;     // bytes delivered when it left (P.delivered)
    u64 delivered_ns = 0U;  // when the last of those were delivered (P.delivered_time)
    u64 first_sent_ns = 0U; // when the newest packet acknowledged by then left (P.first_send_time)
    u64 lost = 0U;          // bytes declared lost when it left (P.lost)
    u64 in_flight = 0U;     // bytes in flight with it (P.tx_in_flight)
    bool app_limited = false;
};

// What one ACK frame tells of delivery (section 4.1.2.3's RS), from its most recently sent acknowledged packet.
struct DeliverySample
{
    u64 rate = 0U;            // bytes per second; 0 when nothing was delivered or the interval was under the min RTT
    u64 delivered = 0U;       // bytes delivered over the interval (RS.delivered)
    u64 interval_ns = 0U;     // the longer of the send and acknowledgement intervals
    u64 prior_delivered = 0U; // the connection's delivered bytes when that packet left: rounds end on it
    u64 lost = 0U;            // bytes declared lost while it was in flight (RS.lost)
    u64 tx_in_flight = 0U;    // bytes in flight when it left (RS.tx_in_flight)
    u64 newly_acked = 0U;     // bytes the frame acknowledged (RS.newly_acked)
    bool app_limited = false; // it left while sending was application limited
    bool acked = false;       // the frame acknowledged a packet counted in flight (RS.has_data)
};

class DeliveryRateSampler
{
public:
    // A packet of `bytes` leaves at `now_ns` with `in_flight` bytes in flight, itself included: its stamp.
    [[nodiscard]] DeliveryStamp on_sent(u64 bytes, u64 in_flight, u64 now_ns) noexcept;
    // An ACK frame begins: its sample starts empty.
    void on_ack_begin() noexcept;
    // A packet of `bytes` sent at `sent_ns` with `stamp`, acknowledged at `now_ns` by the frame being processed;
    // `order` breaks ties between packets of one send time (a later packet of a coalesced datagram is greater).
    // A packet declared lost and then acknowledged counts too: its data was delivered.
    void on_acked(const DeliveryStamp& stamp, u64 bytes, u64 sent_ns, u64 order, u64 now_ns) noexcept;
    void on_lost(u64 bytes) noexcept;
    // The frame ends: its sample is made, a rate only over an interval of at least `min_rtt_ns` (0: none yet).
    void on_ack_end(u64 min_rtt_ns) noexcept;
    // Sending had room and nothing to send with `in_flight` bytes out (section 4.1.2.4): every sample until those
    // bytes are delivered is application limited.
    void on_app_limited(u64 in_flight) noexcept;
    // A client accepted a Retry: delivery starts over.
    void reset() noexcept;

    [[nodiscard]] const DeliverySample& sample() const noexcept { return m_sample; }
    [[nodiscard]] u64 delivered() const noexcept { return m_delivered; }
    [[nodiscard]] u64 lost() const noexcept { return m_lost; }
    [[nodiscard]] bool app_limited() const noexcept { return m_app_limited != 0U; }

private:
    DeliverySample m_sample;
    u64 m_delivered = 0U;
    u64 m_delivered_ns = 0U;
    u64 m_first_sent_ns = 0U;
    u64 m_lost = 0U;
    u64 m_app_limited = 0U; // the delivered bytes that end the application-limited phase; 0 for none
    // The frame's newest acknowledged packet.
    u64 m_newest_order = 0U;
    u64 m_send_elapsed_ns = 0U;
    u64 m_ack_elapsed_ns = 0U;
};

} // namespace crd::net
