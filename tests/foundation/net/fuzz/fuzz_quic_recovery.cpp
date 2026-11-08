// crd-fuzz-net-quic-recovery: QUIC acknowledgements and loss recovery of crd-net (4.2a;
// docs/design/network-foundation.md#quic-recovery) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: byte 0 picks the side (bit 0: server); then operations of two bytes, an operation and its parameter, on one
// connection's recovery with small storage: sends with skipped numbers, ACK frames of up to three ranges (some
// reaching numbers never sent), received packets, ACK frames written and carried, time, timeouts, key discards and
// the handshake's milestones.
//
// Oracles, against a model of every packet sent and received. Each packet is settled at most once, by one event; an
// acknowledged packet lay in the frame's ranges and was outstanding; a lost one lay at or below the largest
// acknowledged and met a threshold, and after an ACK no outstanding packet the packet threshold below the largest
// remains; lost packets come before acknowledged ones; AckOfUnsent is reported only for a range holding a number
// never sent, always for one never sent above the oldest outstanding packet, and changes nothing. A spurious loss is
// a lost packet in the frame's ranges within a probe period of its loss, counted once and with no event; the
// thresholds stay within their caps and rise only on one. Bytes in flight equal the model's.
// Received ranges stay ordered, separated, bounded and made of numbers received; a duplicate is refused exactly when
// the ranges hold it; a written ACK frame reads back as the largest ranges. A loss or probe timeout fires only when
// due, and the timer is armed while ack-eliciting packets are outstanding in a space it may serve.
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_recovery.hpp>

#include <new>

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

constexpr usize kModelPackets = 1024U;
constexpr u64   kReceivedNumbers = 4096U;

constexpr u8 kOut       = 0U;
constexpr u8 kAcked     = 1U;
constexpr u8 kLost      = 2U;
constexpr u8 kDiscarded = 3U;
constexpr u8 kLate      = 4U; // lost, then acknowledged

struct Sent
{
    u64  number;
    u64  sent_ns;
    u64  lost_ns;
    u32  bytes;
    bool eliciting;
    bool in_flight;
    u8   state;
};

struct ModelSpace
{
    Sent  sent[kModelPackets];
    usize count = 0U;
    u64   next = 0U;
    u64   largest_acked = kQuicNoPacket;
    u64   carry = kQuicNoPacket; // an ACK frame written for the next packet to carry
    bool  received[kReceivedNumbers] = {};
    bool  discarded = false;

    Sent* find(u64 number)
    {
        for (usize i = 0U; i < count; ++i)
        {
            if (sent[i].number == number)
            {
                return &sent[i];
            }
        }
        return nullptr;
    }
};

struct Log final : QuicRecoveryEvents
{
    struct Event
    {
        char      kind;
        QuicSpace space;
        u64       number;
    };
    Event events[kModelPackets * 3U];
    usize count = 0U;

    void on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept override { add('a', space, packet); }
    void on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept override { add('l', space, packet); }
    void on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept override { add('d', space, packet); }
    void add(char kind, QuicSpace space, const QuicSentPacket& packet) noexcept
    {
        CRD_FUZZ_REQUIRE(count < kModelPackets * 3U);
        events[count++] = Event{kind, space, packet.number};
    }
};

struct World
{
    QuicSentEntry initial[8];
    QuicSentEntry handshake[8];
    QuicSentEntry application[32];
    QuicRecovery  recovery;
    ModelSpace    spaces[kQuicSpaces];
    Log           log;
    QuicSide      side;
    u64           now = 0U;
    bool          confirmed = false;
    bool          blocked = false;

    explicit World(QuicSide s)
        : recovery(QuicRecoveryConfig{s}, Span<QuicSentEntry>(initial, 8U), Span<QuicSentEntry>(handshake, 8U),
                   Span<QuicSentEntry>(application, 32U)),
          side(s)
    {
    }
};

u64 model_in_flight(World& w)
{
    u64 bytes = 0U;
    for (const ModelSpace& space : w.spaces)
    {
        for (usize i = 0U; i < space.count; ++i)
        {
            if (space.sent[i].state == kOut && space.sent[i].in_flight)
            {
                bytes += space.sent[i].bytes;
            }
        }
    }
    return bytes;
}

bool ranges_hold(ConstSpan<QuicAckRange> ranges, u64 number)
{
    usize holding = 0U;
    for (const QuicAckRange& range : ranges)
    {
        holding += (number >= range.smallest && number <= range.largest) ? 1U : 0U;
    }
    return holding != 0U;
}

void check_ranges(ModelSpace& model, const QuicAckTracker& tracker)
{
    const ConstSpan<QuicAckRange> ranges = tracker.ranges();
    CRD_FUZZ_REQUIRE(ranges.size() <= kQuicAckRanges);
    for (usize i = 0U; i < ranges.size(); ++i)
    {
        CRD_FUZZ_REQUIRE(ranges[i].smallest <= ranges[i].largest);
        CRD_FUZZ_REQUIRE(i == 0U || ranges[i - 1U].smallest >= ranges[i].largest + 2U);
        for (u64 n = ranges[i].smallest; n <= ranges[i].largest; ++n)
        {
            CRD_FUZZ_REQUIRE(n < kReceivedNumbers && model.received[n]);
        }
    }
}

// Settles the events logged since `from` in the model: each one once, by the rule of its kind, with the packet
// threshold and reordering steps the detection used.
void apply_events(World& w, usize from, ConstSpan<QuicAckRange> acked_ranges, u64 threshold, u32 steps)
{
    bool seen_ack = false;
    for (usize i = from; i < w.log.count; ++i)
    {
        const Log::Event& event = w.log.events[i];
        ModelSpace&       space = w.spaces[static_cast<usize>(event.space)];
        Sent* const       sent  = space.find(event.number);
        CRD_FUZZ_REQUIRE(sent != nullptr && sent->state == kOut);
        if (event.kind == 'a')
        {
            seen_ack = true;
            CRD_FUZZ_REQUIRE(ranges_hold(acked_ranges, event.number));
            sent->state = kAcked;
        }
        else if (event.kind == 'l')
        {
            CRD_FUZZ_REQUIRE(!seen_ack); // losses first
            CRD_FUZZ_REQUIRE(space.largest_acked != kQuicNoPacket && event.number <= space.largest_acked);
            const QuicRtt& rtt    = w.recovery.rtt();
            const u64      worst  = (rtt.latest_ns > rtt.smoothed_ns) ? rtt.latest_ns : rtt.smoothed_ns;
            u64            window = static_cast<u64>(steps) * (rtt.min_ns / 4U);
            window                = (window > rtt.smoothed_ns) ? rtt.smoothed_ns : window;
            u64            delay  = worst * kQuicTimeThresholdNumerator / kQuicTimeThresholdDivisor + window;
            delay                 = (delay < kQuicGranularityNs) ? kQuicGranularityNs : delay;
            const bool by_number  = space.largest_acked - event.number >= threshold;
            const bool by_time    = w.now >= sent->sent_ns && w.now - sent->sent_ns >= delay;
            CRD_FUZZ_REQUIRE(by_number || by_time);
            sent->state   = kLost;
            sent->lost_ns = w.now;
        }
        else
        {
            CRD_FUZZ_REQUIRE(event.kind == 'd');
            sent->state = kDiscarded;
        }
    }
}

void check_state(World& w)
{
    CRD_FUZZ_REQUIRE(w.recovery.bytes_in_flight() == model_in_flight(w));
    CRD_FUZZ_REQUIRE(w.recovery.next_timer_ns() <= w.recovery.loss_timer_ns());
    CRD_FUZZ_REQUIRE(w.recovery.pto_count() <= 64U);
    const QuicRtt& rtt = w.recovery.rtt();
    CRD_FUZZ_REQUIRE(!rtt.sampled() || rtt.min_ns <= rtt.smoothed_ns);
    CRD_FUZZ_REQUIRE(w.recovery.packet_threshold() >= kQuicPacketThreshold &&
                     w.recovery.packet_threshold() <= kQuicMaxPacketThreshold);
    CRD_FUZZ_REQUIRE(w.recovery.reorder_steps() <= kQuicMaxReorderSteps);
    bool armable = false;
    for (usize s = 0U; s < kQuicSpaces; ++s)
    {
        ModelSpace& space = w.spaces[s];
        check_ranges(space, w.recovery.received(static_cast<QuicSpace>(s)));
        const bool may_serve = (s != 2U) || w.confirmed;
        for (usize i = 0U; i < space.count; ++i)
        {
            armable = armable || (may_serve && space.sent[i].state == kOut && space.sent[i].eliciting);
        }
    }
    // Armed while it can serve, until the backoff saturates the timer past any time.
    if (armable && !(w.side == QuicSide::Server && w.blocked) && w.recovery.pto_count() < 30U)
    {
        CRD_FUZZ_REQUIRE(w.recovery.loss_timer_ns() != kNeverNs);
    }
}

void op_send(World& w, QuicSpace id, u8 p)
{
    ModelSpace& space = w.spaces[static_cast<usize>(id)];
    if (space.count == kModelPackets)
    {
        return;
    }
    QuicSentPacket packet;
    packet.number        = space.next + (p & 3U);
    packet.sent_ns       = w.now;
    packet.ack_eliciting = (p & 4U) != 0U;
    packet.in_flight     = (p & 8U) != 0U;
    packet.bytes         = 20U + 64U * (p >> 4U);
    packet.carried_ack   = space.carry;
    const bool              room  = w.recovery.can_send(id);
    const QuicRecoveryError error = w.recovery.on_sent(id, packet);
    if (space.discarded)
    {
        CRD_FUZZ_REQUIRE(!room && error == QuicRecoveryError::Discarded);
        return;
    }
    CRD_FUZZ_REQUIRE(error == (room ? QuicRecoveryError::None : QuicRecoveryError::Full));
    if (error == QuicRecoveryError::None)
    {
        space.sent[space.count++] =
            Sent{packet.number, w.now, 0U, packet.bytes, packet.ack_eliciting,
                 packet.in_flight || packet.ack_eliciting, kOut};
        space.next  = packet.number + 1U;
        space.carry = kQuicNoPacket;
    }
}

void op_ack(World& w, QuicSpace id, u8 p, const u8* more, usize more_bytes)
{
    ModelSpace& space = w.spaces[static_cast<usize>(id)];
    // Up to three ranges downward from near the next number (bit 7 reaches past it), each from two parameter bytes.
    QuicAckRange ranges[3];
    usize        count = 0U;
    u64          top   = space.next + ((p & 0x80U) != 0U ? 1U : 0U);
    const usize  want  = 1U + (p & 3U) % 3U;
    for (usize r = 0U; r < want && 2U * r + 1U < more_bytes; ++r)
    {
        const u64 gap    = (r == 0U) ? 1U + (more[2U * r] & 7U) : 2U + (more[2U * r] & 7U);
        const u64 length = more[2U * r + 1U] & 7U;
        if (top < gap + length)
        {
            break;
        }
        const u64 largest = top - gap;
        ranges[count++]   = QuicAckRange{largest - length, largest};
        top               = largest - length;
    }
    if (count == 0U)
    {
        return;
    }
    u8          bytes[64];
    const usize written = write_quic_ack(ConstSpan<QuicAckRange>(ranges, count), p >> 2U, nullptr,
                                         Span<u8>(bytes, sizeof(bytes)));
    CRD_FUZZ_REQUIRE(written != 0U);
    WireReader in(ConstSpan<u8>(bytes, written));
    QuicFrame  frame;
    CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);

    // Never sent: above the last, or a gap. The recovery must see the ones at or above its oldest outstanding packet.
    bool unsent = false;
    bool unsent_above_outstanding = false;
    u64  oldest_out = space.next;
    for (usize i = 0U; i < space.count; ++i)
    {
        if (space.sent[i].state == kOut && space.sent[i].number < oldest_out)
        {
            oldest_out = space.sent[i].number;
        }
    }
    for (usize r = 0U; r < count; ++r)
    {
        for (u64 n = ranges[r].smallest; n <= ranges[r].largest; ++n)
        {
            if (space.find(n) == nullptr)
            {
                unsent = true;
                unsent_above_outstanding = unsent_above_outstanding || n >= oldest_out;
            }
        }
    }
    // Spurious losses: lost packets in the ranges within a probe period of their loss. One whose slot a send took is
    // no longer seen, so the count is at most these.
    const u64 held_for   = w.recovery.probe_period_ns(id);
    usize     candidates = 0U;
    u64       candidate_bytes = 0U;
    for (usize i = 0U; i < space.count; ++i)
    {
        Sent& sent = space.sent[i];
        if (sent.state == kLost && ranges_hold(ConstSpan<QuicAckRange>(ranges, count), sent.number) &&
            w.now - sent.lost_ns <= held_for)
        {
            ++candidates;
            candidate_bytes += sent.in_flight ? sent.bytes : 0U;
        }
    }
    const u64            threshold_before = w.recovery.packet_threshold();
    const u32            steps_before = w.recovery.reorder_steps();
    const u64            before_bytes = w.recovery.bytes_in_flight();
    const usize          from = w.log.count;
    const QuicAckOutcome out = w.recovery.on_ack(id, frame.ack, w.now, w.log);
    if (space.discarded)
    {
        CRD_FUZZ_REQUIRE(out.error == QuicRecoveryError::Discarded && w.log.count == from);
        return;
    }
    if (out.error != QuicRecoveryError::None)
    {
        CRD_FUZZ_REQUIRE(out.error == QuicRecoveryError::AckOfUnsent && unsent);
        CRD_FUZZ_REQUIRE(w.log.count == from && w.recovery.bytes_in_flight() == before_bytes);
        CRD_FUZZ_REQUIRE(w.recovery.packet_threshold() == threshold_before);
        CRD_FUZZ_REQUIRE(w.recovery.reorder_steps() == steps_before);
        return;
    }
    CRD_FUZZ_REQUIRE(out.spurious_packets <= candidates && out.spurious_bytes <= candidate_bytes);
    const u64 threshold_after = w.recovery.packet_threshold();
    const u32 steps_after     = w.recovery.reorder_steps();
    if (out.spurious_packets == 0U)
    {
        CRD_FUZZ_REQUIRE(threshold_after <= threshold_before && steps_after <= steps_before);
    }
    // A raise persists past this frame's detection, so the detection used the larger of before and after.
    const u64 threshold = (threshold_after > threshold_before) ? threshold_after : threshold_before;
    const u32 steps     = (steps_after > steps_before) ? steps_after : steps_before;
    for (usize i = 0U; i < space.count; ++i)
    {
        Sent& sent = space.sent[i];
        if (sent.state == kLost && ranges_hold(ConstSpan<QuicAckRange>(ranges, count), sent.number) &&
            w.now - sent.lost_ns <= held_for)
        {
            sent.state = kLate;
        }
    }
    CRD_FUZZ_REQUIRE(!unsent_above_outstanding);
    usize newly = 0U;
    for (usize i = 0U; i < space.count; ++i)
    {
        newly += (space.sent[i].state == kOut && ranges_hold(ConstSpan<QuicAckRange>(ranges, count),
                                                             space.sent[i].number)) ? 1U : 0U;
    }
    CRD_FUZZ_REQUIRE(out.acked_packets == newly);
    space.largest_acked = (space.largest_acked == kQuicNoPacket || ranges[0].largest > space.largest_acked)
                              ? ranges[0].largest
                              : space.largest_acked;
    apply_events(w, from, ConstSpan<QuicAckRange>(ranges, count), threshold, steps);
    usize lost = 0U;
    for (usize i = from; i < w.log.count; ++i)
    {
        CRD_FUZZ_REQUIRE(w.log.events[i].space == id);
        lost += (w.log.events[i].kind == 'l') ? 1U : 0U;
    }
    CRD_FUZZ_REQUIRE(out.lost.packets == lost);
    if (newly != 0U)
    {
        for (usize i = 0U; i < space.count; ++i)
        {
            const Sent& sent = space.sent[i];
            CRD_FUZZ_REQUIRE(!(sent.state == kOut && sent.number <= space.largest_acked &&
                               space.largest_acked - sent.number >= threshold));
        }
    }
}

void op_receive(World& w, QuicSpace id, u8 p, u8 q)
{
    ModelSpace& space  = w.spaces[static_cast<usize>(id)];
    const u64   number = (static_cast<u64>(p) << 4U | (q >> 4U)) % kReceivedNumbers;
    const bool  held   = ranges_hold(w.recovery.received(id).ranges(), number);
    const auto  ecn    = static_cast<QuicEcn>(q & 3U);
    const bool  added  = w.recovery.on_received(id, number, (q & 4U) != 0U, ecn, w.now);
    CRD_FUZZ_REQUIRE(added == (!space.discarded && !held));
    if (added)
    {
        space.received[number] = true;
    }
}

void op_write(World& w, QuicSpace id, u8 p)
{
    ModelSpace&  space = w.spaces[static_cast<usize>(id)];
    QuicAckRange before[kQuicAckRanges];
    const ConstSpan<QuicAckRange> held = w.recovery.received(id).ranges();
    for (usize i = 0U; i < held.size(); ++i)
    {
        before[i] = held[i];
    }
    const usize        held_count = held.size();
    u8                 room[kQuicAckRanges * 20U + 64U];
    const usize        size = (p & 0x7FU) + 2U;
    const QuicAckWrite out = w.recovery.write_ack(id, Span<u8>(room, size), w.now);
    if (out.bytes == 0U)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(!space.discarded && out.bytes <= size && held_count != 0U);
    CRD_FUZZ_REQUIRE(w.recovery.ack_deadline_ns(id) == kNeverNs);
    WireReader in(ConstSpan<u8>(room, out.bytes));
    QuicFrame  frame;
    CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError && in.done());
    CRD_FUZZ_REQUIRE(frame.ack.largest == out.largest && out.largest == before[0].largest);
    QuicAckRanges walk(frame.ack);
    QuicAckRange  range;
    usize         index = 0U;
    while (walk.next(range))
    {
        CRD_FUZZ_REQUIRE(index < held_count && range.smallest == before[index].smallest &&
                         range.largest == before[index].largest);
        ++index;
    }
    if ((p & 0x80U) != 0U)
    {
        space.carry = out.largest;
    }
}

void op_timeout(World& w)
{
    const u64         due  = w.recovery.loss_timer_ns();
    const u32         pto  = w.recovery.pto_count();
    const u64         threshold = w.recovery.packet_threshold();
    const u32         steps = w.recovery.reorder_steps();
    const usize       from = w.log.count;
    const QuicTimeout out  = w.recovery.on_timeout(w.now, w.log);
    if (due == kNeverNs || w.now < due)
    {
        CRD_FUZZ_REQUIRE(out.kind == QuicTimeoutKind::None && w.log.count == from);
        return;
    }
    CRD_FUZZ_REQUIRE(out.kind != QuicTimeoutKind::None);
    if (out.kind == QuicTimeoutKind::Loss)
    {
        CRD_FUZZ_REQUIRE(w.recovery.pto_count() == pto);
        apply_events(w, from, ConstSpan<QuicAckRange>(), threshold, steps);
        return;
    }
    CRD_FUZZ_REQUIRE(w.log.count == from && out.probes != 0U);
    CRD_FUZZ_REQUIRE(w.recovery.pto_count() == ((pto < 64U) ? pto + 1U : pto));
    CRD_FUZZ_REQUIRE(out.kind != QuicTimeoutKind::AntiDeadlock || w.side == QuicSide::Client);
}

void op_state(World& w, QuicSpace id, u8 p)
{
    const usize from = w.log.count;
    switch (p & 3U)
    {
    case 0U:
        w.recovery.confirm_handshake(w.now);
        w.confirmed = true;
        break;
    case 1U:
        w.recovery.set_handshake_keys(w.now);
        break;
    case 2U:
        w.blocked = !w.blocked;
        w.recovery.set_amplification_blocked(w.blocked, w.now);
        break;
    default:
        w.recovery.set_peer_parameters(static_cast<u64>(p >> 2U) * 1000000U, (p >> 2U) % 21U, w.now);
        break;
    }
    if (id != QuicSpace::Application && (p & 0x40U) != 0U)
    {
        CRD_FUZZ_REQUIRE(w.recovery.discard(id, w.now, w.log));
        apply_events(w, from, ConstSpan<QuicAckRange>(), kQuicPacketThreshold, 0U);
        ModelSpace& space = w.spaces[static_cast<usize>(id)];
        space.discarded = true;
        for (usize i = 0U; i < space.count; ++i)
        {
            CRD_FUZZ_REQUIRE(space.sent[i].state != kOut);
        }
        CRD_FUZZ_REQUIRE(!w.recovery.can_send(id) && w.recovery.received(id).ranges().empty());
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 1U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    // One world per input, in static storage: the model is too large for the stack.
    alignas(World) static unsigned char storage[sizeof(World)];
    static World*                       held = nullptr;
    if (held != nullptr)
    {
        held->~World();
    }
    held     = new (storage) World((data[0] & 1U) != 0U ? QuicSide::Server : QuicSide::Client);
    World& w = *held;
    for (usize at = 1U; at + 1U < size; at += 2U)
    {
        const u8        op = data[at];
        const u8        p  = data[at + 1U];
        const QuicSpace id = static_cast<QuicSpace>((op >> 3U) % 3U);
        switch (op & 7U)
        {
        case 0U:
            op_send(w, id, p);
            break;
        case 1U:
            op_ack(w, id, p, data + at + 2U, (size - at - 2U < 6U) ? size - at - 2U : 6U);
            break;
        case 2U:
            op_receive(w, id, p, (at + 2U < size) ? data[at + 2U] : 0U);
            break;
        case 3U:
            op_write(w, id, p);
            break;
        case 4U:
        case 5U:
            w.now += static_cast<u64>(p) * static_cast<u64>(p) * ((op & 1U) != 0U ? 1000000U : 10000U);
            break;
        case 6U:
            op_timeout(w);
            break;
        default:
            op_state(w, id, p);
            break;
        }
        check_state(w);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Operation bytes: low 3 bits the operation, bits 3 and 4 the space (0 Initial, 8 Handshake, 16 Application).
    // A client's Initial exchange: send, receive, write an ACK carried by the next packet, an acknowledgement, then a
    // probe and the Initial keys discarded.
    const u8 initial[] = {0x00U, 0x00U, 0x05U, 0x0CU, 0x00U, 0x00U, 0x02U, 0x00U, 0x05U, 0x03U, 0x83U, 0x00U,
                          0x00U, 0x04U, 0x05U, 0x0AU, 0x01U, 0x00U, 0x00U, 0x00U, 0x05U, 0xFFU, 0x06U, 0x00U,
                          0x0FU, 0x41U};
    sink.add("client-initial", initial, sizeof(initial));
    // A confirmed server's Application space: twelve sends with skips, acknowledgements of three ranges, losses by
    // both thresholds and a timeout.
    u8 application[2U + 2U * 30U] = {0x01U};
    usize at = 1U;
    const auto put = [&](u8 op, u8 p)
    {
        application[at++] = op;
        application[at++] = p;
    };
    put(0x07U, 0x00U);
    for (u8 i = 0U; i < 12U; ++i)
    {
        put(0x10U, static_cast<u8>(0x04U | (i % 3U)));
        put(0x04U, 0x20U);
    }
    put(0x11U, 0x02U);
    put(0x11U, 0x24U);
    put(0x05U, 0xFFU);
    put(0x06U, 0x00U);
    sink.add("server-application", application, at);
    // An optimistic acknowledgement past the last number sent.
    const u8 optimistic[] = {0x00U, 0x10U, 0x04U, 0x10U, 0x05U, 0x11U, 0x80U, 0x00U, 0x00U};
    sink.add("optimistic-ack", optimistic, sizeof(optimistic));
}
