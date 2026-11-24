// crd-fuzz-net-quic-recovery: QUIC acknowledgements and loss recovery of crd-net (4.2a;
// docs/design/network-foundation.md#quic-recovery) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: byte 0 picks the side (bit 0: server) and receive timestamps (bit 1: reported, bits 2 to 4 how many an ACK
// frame carries less one, bits 5 to 7 their exponent); then operations of two bytes, an operation and its parameter, on
// one connection's recovery with small storage: sends with skipped numbers, ACK frames of up to three ranges (some
// reaching numbers never sent), received packets, ACK frames written and carried, time, timeouts, key discards and
// the handshake's milestones. An Application operation names a path slot (RFC 10069 section 5) in bits 5 and 6 of its
// operation byte, and bit 7 has an ACK frame arrive through the next slot; an Application state operation with bit 6
// of its parameter opens its slot, or closes it, or with bit 7 too stops it (its path ID abandoned). A state operation
// with bit 7 of its parameter (alone for the Application space) then moves its space's entries to the space's other
// storage of the same size, a discarded space's to none.
//
// Oracles, against a model of every packet sent and received. Each packet is settled at most once, by one event; an
// acknowledged packet lay in the frame's ranges and was outstanding; a lost one lay at or below the largest
// acknowledged and met a threshold, and after an ACK no outstanding packet the packet threshold below the largest
// remains; lost packets come before acknowledged ones; AckOfUnsent is reported only for a range holding a number never
// sent, always for one never sent above the oldest outstanding packet, and changes nothing. A spurious loss is a lost
// packet in the frame's ranges within a probe period of its loss, counted once and with no event; the thresholds stay
// within their caps and rise only on one. Bytes in flight equal the model's. Each packet carries an ECN codepoint of
// its number: a frame's outcome counts its newly acknowledged ECT(0) and ECT(1) packets (spurious losses among them),
// says whether it raised the largest, and names a send time no later than now. Received ranges stay ordered, separated,
// bounded and made of numbers received; a copy of a number reaches recovery only while the ranges hold it (the packet
// protection's replay window refuses any other) and is then refused, so a number is recorded at most once; a written
// ACK frame reads back as the largest ranges. A loss or probe timeout fires only when due, and the timer is armed while
// ack-eliciting packets are outstanding in a space it may serve. A probe or anti-deadlock timeout declares nothing lost
// but, in a space whose storage could not record a packet, its oldest outstanding packets, in order; after it the space
// can record its probes. A send records whenever the space says it can, discarding at most the oldest outstanding
// ACK-only packet with no skipped number before it. Receive timestamps (draft-ietf-quic-receive-ts-03): a written
// Application ACK frame reports at most the asked-for count of packets it acknowledges, each received, with its arrival
// time from the first arrival less a nanosecond in the asked-for unit, the times never rising; the sender's reading of
// a report times only packets outstanding or lost, and only when their one-way delays spread no wider than the time
// since the oldest of them was sent. Paths: every slot keeps its own numbers, estimate, thresholds, probe count and
// bytes in flight, and an operation on one leaves every other slot's exactly as it was; a slot not open refuses every
// operation (NoPath) and reads empty; closing a slot declares each of its outstanding packets lost, outside every
// estimate, and the slot then starts from nothing when it opens again; slot 0 closes only after the handshake's spaces,
// and its ACK frames then report no receive timestamps. Stopping a slot other than 0 declares each of its outstanding
// packets lost in the same way at once; it then refuses sends and ACK frames (NoPath), is never a timeout's, holds no
// bytes in flight or probe count, and still records and acknowledges what it receives until it closes, which reports
// nothing more. Each open slot drives its own QuicPathControl (quic_path_control.hpp) as a connection does: packets
// stamped and counted on their slot's, the events of packets on their path, each ACK frame's end and each timeout's
// detection reach that slot's alone, so the isolation above holds for every slot's window, delivery, losses, ECN state,
// path MTU and pacer too; the window stays above its floor and the datagram size is the search's after every operation.
#include <crd/core/platform.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_path_control.hpp>
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

// The codepoint a packet was sent with, from its number.
[[nodiscard]] Ecn ecn_of(u64 number)
{
    return static_cast<Ecn>(number % 3U); // Not-ECT, ECT(1), ECT(0)
}

struct ModelSpace
{
    Sent  sent[kModelPackets];
    usize count = 0U;
    u64   next = 0U;
    u64   largest_acked = kQuicNoPacket;
    u64   carry = kQuicNoPacket; // an ACK frame written for the next packet to carry
    bool  received[kReceivedNumbers] = {};
    u64 received_ns[kReceivedNumbers] = {};
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

// The model's spaces: Initial, Handshake, then the Application space of each slot.
constexpr usize kModelSpaces = 2U + kQuicMaxPaths;

[[nodiscard]] usize model_index(QuicSpace id, usize path)
{
    return (id == QuicSpace::Application) ? 2U + path : static_cast<usize>(id);
}

// The slot whose estimate a space follows.
[[nodiscard]] usize owner_of(QuicSpace id, usize path)
{
    return (id == QuicSpace::Application) ? path : 0U;
}

struct Log final : QuicRecoveryEvents
{
    struct Event
    {
        char      kind;
        QuicSpace space;
        usize     path;
        u64       number;
        bool      on_path; // the packet was on its path's estimate when the event came
    };
    const QuicRecovery* recovery = nullptr;
    QuicPathControl* controls = nullptr; // each slot's, which the events of its packets on their path reach
    const u64* now = nullptr;
    Event events[kModelPackets * 3U];
    usize count = 0U;

    void on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept override
    {
        if (add('a', space, packet))
        {
            controls[packet.path].on_acked(space, packet, kNeverNs, *now);
        }
    }
    void on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept override
    {
        if (add('l', space, packet))
        {
            static_cast<void>(controls[packet.path].on_lost(space, packet, *now));
        }
    }
    void on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept override
    {
        if (add('d', space, packet))
        {
            controls[packet.path].on_discarded(packet);
        }
    }
    void on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns) noexcept override
    {
        if (recovery->on_path(space, packet))
        {
            controls[packet.path].on_spurious(space, packet, lost_ns, *now);
        }
    }
    // Whether the packet was on its path's estimate.
    bool add(char kind, QuicSpace space, const QuicSentPacket& packet) noexcept
    {
        CRD_FUZZ_REQUIRE(count < kModelPackets * 3U);
        const bool on_path = recovery->on_path(space, packet);
        events[count++] = Event{kind, space, packet.path, packet.number, on_path};
        return on_path;
    }
};

// A move changes no answer: it succeeds exactly for a space the recovery holds, hands back the storage it held and
// leaves the storage's size, and every oracle above holds across it.
// (MSVC C4324: the path controls are 16-byte aligned.)
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct World
{
    QuicSentEntry initial[8];
    QuicSentEntry handshake[8];
    QuicSentEntry application[kQuicMaxPaths][32];
    // Each space's other storage of the same size, which a move takes in turn (moved: the space holds it now).
    QuicSentEntry other_initial[8];
    QuicSentEntry other_handshake[8];
    QuicSentEntry other_application[kQuicMaxPaths][32];
    bool          moved[kModelSpaces] = {};
    QuicPathRecovery states[kQuicMaxPaths]; // each slot's, slot 0's once it reopens
    QuicPathControl controls[kQuicMaxPaths]; // each open slot's, started when it opens
    QuicRecovery  recovery;
    ModelSpace    spaces[kModelSpaces];
    bool          open[kQuicMaxPaths] = {true};
    bool          stopped[kQuicMaxPaths] = {};
    bool          log_ended = false; // slot 0 closed: its ACK frames report no receive timestamps
    // RFC 10069 section 5.4: the slot each path's last RTT sample came through, whether its minimum may have
    // restarted since (min_rtt is then above smoothed_rtt at times) and whether the next sample restarts it.
    usize         last_via[kQuicMaxPaths] = {};
    bool          restarted[kQuicMaxPaths] = {};
    bool          restart_due[kQuicMaxPaths] = {};
    Log           log;
    QuicSide      side;
    u64           now = 0U;
    u64 per_ack = 0U; // receive timestamps an ACK frame reports (0: none)
    u64 exponent = 0U;
    u64 basis_ns = kNeverNs;
    bool          confirmed = false;
    bool          blocked = false;

    explicit World(QuicSide s)
        : recovery(QuicRecoveryConfig{s}, Span<QuicSentEntry>(initial, 8U), Span<QuicSentEntry>(handshake, 8U),
                   Span<QuicSentEntry>(application[0], 32U)),
          side(s)
    {
        log.recovery = &recovery;
        log.controls = controls;
        log.now = &now;
        CRD_FUZZ_REQUIRE(controls[0].start(QuicPathControlConfig{}, QuicPath{}, 1U));
        for (usize& via : last_via)
        {
            via = kQuicNoPath;
        }
    }
    [[nodiscard]] bool usable(QuicSpace id, usize path) const
    {
        return (id == QuicSpace::Application) ? open[path] : path == 0U;
    }
    // Usable, and not stopped: what sends and ACK frames take.
    [[nodiscard]] bool sending(QuicSpace id, usize path) const
    {
        return usable(id, path) && (id != QuicSpace::Application || !stopped[path]);
    }
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

// What an operation on another slot must leave exactly as it was.
struct PathState
{
    u64 latest_ns;
    u64 min_ns;
    u64 smoothed_ns;
    u64 variance_ns;
    u64 bytes;
    u64 threshold;
    u64 largest_acked;
    u32 steps;
    u32 pto;
    // Its control's.
    u64 window;
    u64 delivered;
    u64 lost;
    u64 pace_timer_ns;
    u32 datagram_bytes;
    QuicEcnState ecn;
};

void snapshot(const QuicRecovery& recovery, const QuicPathControl (&controls)[kQuicMaxPaths],
              PathState (&out)[kQuicMaxPaths])
{
    for (usize path = 0U; path < kQuicMaxPaths; ++path)
    {
        const QuicRtt& rtt = recovery.rtt(path);
        const QuicPathControl& control = controls[path];
        const bool started = control.started();
        out[path] = PathState{rtt.latest_ns,
                              rtt.min_ns,
                              rtt.smoothed_ns,
                              rtt.variance_ns,
                              recovery.bytes_in_flight(path),
                              recovery.packet_threshold(path),
                              recovery.largest_acked(QuicSpace::Application, path),
                              recovery.reorder_steps(path),
                              recovery.pto_count(path),
                              started ? control.congestion().window() : 0U,
                              control.delivery().delivered(),
                              control.delivery().lost(),
                              control.pace_timer_ns(),
                              control.datagram_bytes(),
                              control.ecn().state()};
    }
}

[[nodiscard]] bool same(const PathState& a, const PathState& b)
{
    return a.latest_ns == b.latest_ns && a.min_ns == b.min_ns && a.smoothed_ns == b.smoothed_ns &&
           a.variance_ns == b.variance_ns && a.bytes == b.bytes && a.threshold == b.threshold &&
           a.largest_acked == b.largest_acked && a.steps == b.steps && a.pto == b.pto && a.window == b.window &&
           a.delivered == b.delivered && a.lost == b.lost && a.pace_timer_ns == b.pace_timer_ns &&
           a.datagram_bytes == b.datagram_bytes && a.ecn == b.ecn;
}

u64 model_in_flight(World& w, usize path)
{
    u64 bytes = 0U;
    for (usize s = 0U; s < kModelSpaces; ++s)
    {
        const ModelSpace& space = w.spaces[s];
        if (((s < 2U) ? 0U : s - 2U) != path)
        {
            continue;
        }
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
        ModelSpace&       space = w.spaces[model_index(event.space, event.path)];
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
            const QuicRtt& rtt    = w.recovery.rtt(owner_of(event.space, event.path));
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
    CRD_FUZZ_REQUIRE(w.recovery.next_timer_ns() <= w.recovery.loss_timer_ns());
    for (usize path = 0U; path < kQuicMaxPaths; ++path)
    {
        CRD_FUZZ_REQUIRE(w.recovery.path_open(path) == w.open[path]);
        CRD_FUZZ_REQUIRE(w.recovery.path_sending(path) == (w.open[path] && !w.stopped[path]));
        CRD_FUZZ_REQUIRE(!w.stopped[path] || (w.recovery.pto_count(path) == 0U && model_in_flight(w, path) == 0U));
        const QuicPathControl& control = w.controls[path];
        CRD_FUZZ_REQUIRE(!w.open[path] || control.started());
        CRD_FUZZ_REQUIRE(!control.started() || (control.congestion().window() >= 2U * kQuicPmtuFloor &&
                                                control.datagram_bytes() == control.path_mtu().size()));
        CRD_FUZZ_REQUIRE(w.recovery.bytes_in_flight(path) == model_in_flight(w, path));
        CRD_FUZZ_REQUIRE(w.recovery.pto_count(path) <= 64U);
        const QuicRtt& rtt = w.recovery.rtt(path);
        CRD_FUZZ_REQUIRE(!rtt.sampled() || (rtt.min_ns <= rtt.latest_ns &&
                                            (w.restarted[path] || rtt.min_ns <= rtt.smoothed_ns)));
        CRD_FUZZ_REQUIRE(w.recovery.packet_threshold(path) >= kQuicPacketThreshold &&
                         w.recovery.packet_threshold(path) <= kQuicMaxPacketThreshold);
        CRD_FUZZ_REQUIRE(w.recovery.reorder_steps(path) <= kQuicMaxReorderSteps);
        CRD_FUZZ_REQUIRE(w.open[path] || (!rtt.sampled() && w.recovery.pto_count(path) == 0U &&
                                          w.recovery.largest_acked(QuicSpace::Application, path) == kQuicNoPacket));
    }
    bool armable = false;
    for (usize s = 0U; s < kModelSpaces; ++s)
    {
        ModelSpace& space = w.spaces[s];
        const QuicSpace id = (s < 2U) ? static_cast<QuicSpace>(s) : QuicSpace::Application;
        const usize path = (s < 2U) ? 0U : s - 2U;
        check_ranges(space, w.recovery.received(id, path));
        // Until the backoff saturates its timer past any time.
        const bool may_serve = ((s < 2U) || w.confirmed) && w.recovery.pto_count(path) < 30U;
        for (usize i = 0U; i < space.count; ++i)
        {
            armable = armable || (may_serve && space.sent[i].state == kOut && space.sent[i].eliciting);
        }
    }
    // Armed while it can serve.
    if (armable && !(w.side == QuicSide::Server && w.blocked))
    {
        CRD_FUZZ_REQUIRE(w.recovery.loss_timer_ns() != kNeverNs);
    }
}

// The oldest outstanding ACK-only packet with no skipped number before it: the one whose entry a full storage gives
// to the next packet (QuicRecovery::on_sent).
[[nodiscard]] u64 oldest_spare(const ModelSpace& space)
{
    for (usize i = 0U; i < space.count; ++i)
    {
        const Sent& sent = space.sent[i];
        const u64 before = (i == 0U) ? 0U : space.sent[i - 1U].number + 1U;
        if (sent.state == kOut && !sent.eliciting && !sent.in_flight && sent.number == before)
        {
            return sent.number;
        }
    }
    return kQuicNoPacket;
}

void op_send(World& w, QuicSpace id, usize path, u8 p)
{
    ModelSpace& space = w.spaces[model_index(id, path)];
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
    packet.ecn           = ecn_of(packet.number);
    packet.path          = static_cast<u8>(path);
    const bool              room  = w.recovery.can_send(id, path);
    QuicPathControl& control = w.controls[path];
    const bool in_flight = packet.in_flight || packet.ack_eliciting; // as recovery records it
    if (room && in_flight)
    {
        packet.delivery = control.stamp(packet.bytes, w.recovery.bytes_in_flight(path) + packet.bytes, w.now);
    }
    const u64 spare = oldest_spare(space);
    const usize from = w.log.count;
    const QuicRecoveryError error = w.recovery.on_sent(id, packet, w.log);
    // At most one event: the oldest spare ACK-only packet discarded for the entry, and only for a recorded packet.
    CRD_FUZZ_REQUIRE(w.log.count == from || (error == QuicRecoveryError::None && w.log.count == from + 1U));
    if (w.log.count != from)
    {
        const Log::Event& event = w.log.events[from];
        Sent* const gone = space.find(event.number);
        CRD_FUZZ_REQUIRE(event.kind == 'd' && event.space == id && event.path == path && event.number == spare &&
                         gone != nullptr);
        gone->state = kDiscarded;
    }
    if (!w.sending(id, path))
    {
        CRD_FUZZ_REQUIRE(!room && error == QuicRecoveryError::NoPath);
        return;
    }
    if (space.discarded)
    {
        CRD_FUZZ_REQUIRE(!room && error == QuicRecoveryError::Discarded);
        return;
    }
    CRD_FUZZ_REQUIRE(error == (room ? QuicRecoveryError::None : QuicRecoveryError::Full));
    if (error == QuicRecoveryError::None)
    {
        control.on_sent(id, packet, in_flight, w.recovery.bytes_in_flight(path), w.recovery.rtt(path), w.now);
        space.sent[space.count++] =
            Sent{packet.number, w.now, 0U, packet.bytes, packet.ack_eliciting,
                 packet.in_flight || packet.ack_eliciting, kOut};
        space.next  = packet.number + 1U;
        space.carry = kQuicNoPacket;
    }
}

// The sender's reading of a report: only packets outstanding or lost get a time, the received one, and their one-way
// delays spread no wider than the time since the oldest of them was sent.
void check_peer_times(World& w, ModelSpace& space, const QuicAck& ack)
{
    static QuicPeerReceiveTimes times;
    const usize given = times.read(ack, w.exponent, w.recovery, w.now);
    CRD_FUZZ_REQUIRE(given <= kQuicMaxReceiveTimestamps);
    const u64 unit = 1000U << w.exponent;
    usize found = 0U;
    crd::i64 least = 0;
    crd::i64 most = 0;
    u64 oldest = w.now;
    QuicAckTimestamps walk(ack);
    QuicReceiveTimestamp stamp;
    while (walk.next(stamp))
    {
        const u64 received = times.find(stamp.number);
        if (received == kNeverNs)
        {
            continue;
        }
        const Sent* const sent = space.find(stamp.number);
        CRD_FUZZ_REQUIRE(sent != nullptr && (sent->state == kOut || sent->state == kLost));
        CRD_FUZZ_REQUIRE(received == stamp.time * unit);
        const crd::i64 one_way = static_cast<crd::i64>(received) - static_cast<crd::i64>(sent->sent_ns);
        least = (found == 0U || one_way < least) ? one_way : least;
        most = (found == 0U || one_way > most) ? one_way : most;
        oldest = (sent->sent_ns < oldest) ? sent->sent_ns : oldest;
        ++found;
    }
    CRD_FUZZ_REQUIRE(found == given);
    CRD_FUZZ_REQUIRE(found == 0U || static_cast<u64>(most - least) <= w.now - oldest);
    times.clear();
}

void op_ack(World& w, QuicSpace id, usize path, usize via, u8 p, const u8* more, usize more_bytes)
{
    ModelSpace& space = w.spaces[model_index(id, path)];
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
    // With timestamps asked for, the Application space's frames time each range's largest packet, a millisecond
    // apart less the parameter's low bits in microseconds.
    QuicReceiveTimestamp stamps[3];
    const usize timed = (id == QuicSpace::Application && path == 0U && w.open[0] && w.per_ack != 0U) ? count : 0U;
    for (usize r = 0U; r < timed; ++r)
    {
        stamps[r] = QuicReceiveTimestamp{ranges[r].largest, (3U - r) * (1000U - (p & 0x3FU))};
    }
    u8 bytes[96];
    const usize written =
        write_quic_ack(ConstSpan<QuicAckRange>(ranges, count), p >> 2U, nullptr, Span<u8>(bytes, sizeof(bytes)),
                       ConstSpan<QuicReceiveTimestamp>(stamps, timed));
    CRD_FUZZ_REQUIRE(written != 0U);
    WireReader in(ConstSpan<u8>(bytes, written));
    QuicFrame  frame;
    CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CRD_FUZZ_REQUIRE(frame.ack.timestamps == (timed != 0U));
    if (timed != 0U)
    {
        check_peer_times(w, space, frame.ack);
    }

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
    const u64 held_for   = w.recovery.probe_period_ns(id, path);
    usize     candidates = 0U;
    u64       candidate_bytes = 0U;
    u64       candidate_marks[2] = {}; // ECT(0), ECT(1)
    for (usize i = 0U; i < space.count; ++i)
    {
        Sent& sent = space.sent[i];
        if (sent.state == kLost && ranges_hold(ConstSpan<QuicAckRange>(ranges, count), sent.number) &&
            w.now - sent.lost_ns <= held_for)
        {
            ++candidates;
            candidate_bytes += sent.in_flight ? sent.bytes : 0U;
            candidate_marks[0] += (ecn_of(sent.number) == Ecn::Ect0) ? 1U : 0U;
            candidate_marks[1] += (ecn_of(sent.number) == Ecn::Ect1) ? 1U : 0U;
        }
    }
    const u64 largest_before = space.largest_acked;
    const usize          owner = owner_of(id, path);
    const u64            threshold_before = w.recovery.packet_threshold(owner);
    const u32            steps_before = w.recovery.reorder_steps(owner);
    const u64            before_bytes = w.recovery.bytes_in_flight(owner);
    const usize          from = w.log.count;
    QuicPathControl& control = w.controls[owner];
    control.on_ack_begin();
    const QuicAckOutcome out = w.recovery.on_ack(id, frame.ack, w.now, w.log, path, via);
    if (out.error == QuicRecoveryError::None)
    {
        static_cast<void>(
            control.on_ack_end(id, frame.ack, out, w.recovery.rtt(owner), w.recovery.bytes_in_flight(owner), w.now));
    }
    if (!w.sending(id, path) || (!space.discarded && !w.open[via]))
    {
        CRD_FUZZ_REQUIRE(out.error == QuicRecoveryError::NoPath && w.log.count == from);
        return;
    }
    if (space.discarded)
    {
        CRD_FUZZ_REQUIRE(out.error == QuicRecoveryError::Discarded && w.log.count == from);
        return;
    }
    if (out.error != QuicRecoveryError::None)
    {
        CRD_FUZZ_REQUIRE(out.error == QuicRecoveryError::AckOfUnsent && unsent);
        CRD_FUZZ_REQUIRE(w.log.count == from && w.recovery.bytes_in_flight(owner) == before_bytes);
        CRD_FUZZ_REQUIRE(w.recovery.packet_threshold(owner) == threshold_before);
        CRD_FUZZ_REQUIRE(w.recovery.reorder_steps(owner) == steps_before);
        return;
    }
    CRD_FUZZ_REQUIRE(out.spurious_packets <= candidates && out.spurious_bytes <= candidate_bytes);
    if (out.rtt_sampled)
    {
        const bool restart = w.restart_due[owner] || (w.last_via[owner] != kQuicNoPath && w.last_via[owner] != via);
        CRD_FUZZ_REQUIRE(!restart || w.recovery.rtt(owner).min_ns == w.recovery.rtt(owner).latest_ns);
        w.restarted[owner] = w.restarted[owner] || restart;
        w.restart_due[owner] = false;
        w.last_via[owner] = via;
    }
    const u64 threshold_after = w.recovery.packet_threshold(owner);
    const u32 steps_after     = w.recovery.reorder_steps(owner);
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
    u64   newly_marks[2] = {};
    for (usize i = 0U; i < space.count; ++i)
    {
        const bool acked = space.sent[i].state == kOut &&
                           ranges_hold(ConstSpan<QuicAckRange>(ranges, count), space.sent[i].number);
        newly += acked ? 1U : 0U;
        newly_marks[0] += (acked && ecn_of(space.sent[i].number) == Ecn::Ect0) ? 1U : 0U;
        newly_marks[1] += (acked && ecn_of(space.sent[i].number) == Ecn::Ect1) ? 1U : 0U;
    }
    CRD_FUZZ_REQUIRE(out.acked_packets == newly);
    CRD_FUZZ_REQUIRE(out.advanced == (largest_before == kQuicNoPacket || ranges[0].largest > largest_before));
    CRD_FUZZ_REQUIRE(out.ect0_packets >= newly_marks[0] && out.ect0_packets <= newly_marks[0] + candidate_marks[0]);
    CRD_FUZZ_REQUIRE(out.ect1_packets >= newly_marks[1] && out.ect1_packets <= newly_marks[1] + candidate_marks[1]);
    CRD_FUZZ_REQUIRE(out.ect0_packets + out.ect1_packets <= out.acked_packets + out.spurious_packets);
    CRD_FUZZ_REQUIRE(out.newest_sent_ns <= w.now);
    space.largest_acked = (space.largest_acked == kQuicNoPacket || ranges[0].largest > space.largest_acked)
                              ? ranges[0].largest
                              : space.largest_acked;
    apply_events(w, from, ConstSpan<QuicAckRange>(ranges, count), threshold, steps);
    usize lost = 0U;
    for (usize i = from; i < w.log.count; ++i)
    {
        CRD_FUZZ_REQUIRE(w.log.events[i].space == id && w.log.events[i].path == path);
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

void op_receive(World& w, QuicSpace id, usize path, u8 p, u8 q)
{
    ModelSpace& space  = w.spaces[model_index(id, path)];
    const u64   number = (static_cast<u64>(p) << 4U | (q >> 4U)) % kReceivedNumbers;
    const bool  held   = ranges_hold(w.recovery.received(id, path).ranges(), number);
    if (space.received[number] && !held)
    {
        return; // its range forgotten: QuicPacketProtection's replay window refuses the copy (RFC 9000 section 12.3)
    }
    const auto  ecn    = static_cast<Ecn>(q & 3U);
    const bool  added  = w.recovery.on_received(id, number, (q & 4U) != 0U, ecn, w.now, path);
    CRD_FUZZ_REQUIRE(added == (w.usable(id, path) && !space.discarded && !held));
    CRD_FUZZ_REQUIRE(!added || !space.received[number]);
    if (added)
    {
        space.received[number] = true;
        space.received_ns[number] = w.now;
        if (id == QuicSpace::Application && path == 0U && !w.log_ended && w.per_ack != 0U &&
            w.basis_ns == kNeverNs)
        {
            w.basis_ns = (w.now == 0U) ? 0U : w.now - 1U;
        }
    }
}

void op_write(World& w, QuicSpace id, usize path, u8 p)
{
    ModelSpace&  space = w.spaces[model_index(id, path)];
    QuicAckRange before[kQuicAckRanges];
    const ConstSpan<QuicAckRange> held = w.recovery.received(id, path).ranges();
    for (usize i = 0U; i < held.size(); ++i)
    {
        before[i] = held[i];
    }
    const usize        held_count = held.size();
    u8                 room[kQuicAckRanges * 20U + 64U];
    const usize        size = (p & 0x7FU) + 2U;
    const QuicAckWrite out = w.recovery.write_ack(id, Span<u8>(room, size), w.now, path);
    if (out.bytes == 0U)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(w.usable(id, path) && !space.discarded && out.bytes <= size && held_count != 0U);
    CRD_FUZZ_REQUIRE(w.recovery.ack_deadline_ns(id, path) == kNeverNs);
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
    CRD_FUZZ_REQUIRE(!frame.ack.timestamps ||
                     (id == QuicSpace::Application && path == 0U && !w.log_ended && w.per_ack != 0U));
    CRD_FUZZ_REQUIRE(frame.ack.timestamp_count <= w.per_ack);
    QuicAckTimestamps times(frame.ack);
    QuicReceiveTimestamp stamp;
    u64 previous = kNeverNs;
    while (times.next(stamp))
    {
        CRD_FUZZ_REQUIRE(stamp.number < kReceivedNumbers && space.received[stamp.number]);
        CRD_FUZZ_REQUIRE(ranges_hold(ConstSpan<QuicAckRange>(before, index), stamp.number));
        const u64 arrived = space.received_ns[stamp.number];
        CRD_FUZZ_REQUIRE(arrived > w.basis_ns && stamp.time == ((arrived - w.basis_ns) / 1000U) >> w.exponent);
        CRD_FUZZ_REQUIRE(stamp.time <= previous);
        previous = stamp.time;
    }
    if ((p & 0x80U) != 0U)
    {
        space.carry = out.largest;
    }
}

// Returns the slot whose timer fired, kQuicMaxPaths for none.
usize op_timeout(World& w)
{
    const u64         due  = w.recovery.loss_timer_ns();
    u32               pto[kQuicMaxPaths] = {};
    u64               threshold[kQuicMaxPaths] = {};
    u32               steps[kQuicMaxPaths] = {};
    for (usize path = 0U; path < kQuicMaxPaths; ++path)
    {
        pto[path] = w.recovery.pto_count(path);
        threshold[path] = w.recovery.packet_threshold(path);
        steps[path] = w.recovery.reorder_steps(path);
    }
    const usize       from = w.log.count;
    bool              could[kModelSpaces] = {};
    for (usize s = 0U; s < kModelSpaces; ++s)
    {
        could[s] = (s < 2U) ? w.recovery.can_send(static_cast<QuicSpace>(s))
                            : w.recovery.can_send(QuicSpace::Application, s - 2U);
    }
    const QuicTimeout out  = w.recovery.on_timeout(w.now, w.log);
    if (due == kNeverNs || w.now < due)
    {
        CRD_FUZZ_REQUIRE(out.kind == QuicTimeoutKind::None && w.log.count == from);
        return kQuicMaxPaths;
    }
    CRD_FUZZ_REQUIRE(out.kind != QuicTimeoutKind::None);
    CRD_FUZZ_REQUIRE(out.path < kQuicMaxPaths && w.sending(out.space, out.path));
    w.controls[out.path].end_detection(out.lost, w.now);
    const usize owner = owner_of(out.space, out.path);
    if (out.kind == QuicTimeoutKind::Loss)
    {
        CRD_FUZZ_REQUIRE(w.recovery.pto_count(owner) == pto[owner]);
        for (usize i = from; i < w.log.count; ++i)
        {
            CRD_FUZZ_REQUIRE(w.log.events[i].space == out.space && w.log.events[i].path == out.path);
        }
        apply_events(w, from, ConstSpan<QuicAckRange>(), threshold[owner], steps[owner]);
        return out.path;
    }
    CRD_FUZZ_REQUIRE(out.probes != 0U);
    // A probe must leave (RFC 9002 section 6.2.4): a storage that could not record it gives up its oldest packets.
    const usize index = model_index(out.space, out.path);
    ModelSpace& space = w.spaces[index];
    CRD_FUZZ_REQUIRE(could[index] || w.log.count > from || space.discarded);
    for (usize i = from; i < w.log.count; ++i)
    {
        const Log::Event& event = w.log.events[i];
        CRD_FUZZ_REQUIRE(event.kind == 'l' && event.space == out.space && event.path == out.path);
        Sent* const sent = space.find(event.number);
        CRD_FUZZ_REQUIRE(sent != nullptr && sent->state == kOut);
        for (usize j = 0U; j < space.count; ++j)
        {
            CRD_FUZZ_REQUIRE(space.sent[j].state != kOut || space.sent[j].number >= event.number);
        }
        sent->state   = kLost;
        sent->lost_ns = w.now;
    }
    CRD_FUZZ_REQUIRE(w.recovery.can_send(out.space, out.path) || space.discarded);
    CRD_FUZZ_REQUIRE(w.recovery.pto_count(owner) == ((pto[owner] < 64U) ? pto[owner] + 1U : pto[owner]));
    CRD_FUZZ_REQUIRE(out.kind != QuicTimeoutKind::AntiDeadlock || w.side == QuicSide::Client);
    return out.path;
}

// Each outstanding packet of slot `path`'s, reported lost from `from` on, in rising order and off every estimate, is
// lost in the model too.
void take_closing_losses(World& w, usize path, usize from)
{
    ModelSpace& space = w.spaces[2U + path];
    usize outstanding = 0U;
    for (usize i = 0U; i < space.count; ++i)
    {
        outstanding += (space.sent[i].state == kOut) ? 1U : 0U;
    }
    CRD_FUZZ_REQUIRE(w.log.count - from == outstanding);
    u64 previous = 0U;
    for (usize i = from; i < w.log.count; ++i)
    {
        const Log::Event& event = w.log.events[i];
        CRD_FUZZ_REQUIRE(event.kind == 'l' && event.space == QuicSpace::Application && event.path == path);
        CRD_FUZZ_REQUIRE(!event.on_path && (i == from || event.number > previous));
        Sent* const sent = space.find(event.number);
        CRD_FUZZ_REQUIRE(sent != nullptr && sent->state == kOut);
        sent->state = kLost;
        sent->lost_ns = w.now;
        previous = event.number;
    }
}

// Stops slot `path` (bit 7 of `p`), opens it, or closes it: a stopped or closed slot's outstanding packets are all
// lost, each off every estimate when reported; a closed one starts from nothing.
void op_toggle(World& w, usize path, u8 p)
{
    const usize from = w.log.count;
    const bool handshake_done = w.confirmed && w.spaces[0].discarded && w.spaces[1].discarded;
    if ((p & 0x80U) != 0U)
    {
        usize sending = 0U;
        for (usize each = 0U; each < kQuicMaxPaths; ++each)
        {
            sending += (w.open[each] && !w.stopped[each]) ? 1U : 0U;
        }
        // Never the last slot sending; slot 0 only once the handshake is gone, as it closes.
        const bool stops = w.open[path] && !w.stopped[path] && sending > 1U && (path != 0U || handshake_done);
        CRD_FUZZ_REQUIRE(w.recovery.stop_path(path, w.now, w.log) == stops);
        if (!stops)
        {
            CRD_FUZZ_REQUIRE(w.log.count == from);
            return;
        }
        take_closing_losses(w, path, from);
        w.stopped[path] = true;
        CRD_FUZZ_REQUIRE(!w.recovery.can_send(QuicSpace::Application, path));
        return;
    }
    if (!w.open[path])
    {
        CRD_FUZZ_REQUIRE(w.recovery.open_path(path, w.states[path], Span<QuicSentEntry>(w.application[path], 32U)));
        w.moved[2U + path] = false;
        CRD_FUZZ_REQUIRE(w.controls[path].start(QuicPathControlConfig{}, QuicPath{}, path + 1U));
        w.spaces[2U + path] = ModelSpace{};
        w.open[path] = true;
        w.last_via[path] = kQuicNoPath;
        w.restarted[path] = false;
        w.restart_due[path] = false;
        return;
    }
    usize open = 0U;
    for (const bool each : w.open)
    {
        open += each ? 1U : 0U;
    }
    const bool closes = open > 1U && (path != 0U || handshake_done);
    CRD_FUZZ_REQUIRE(w.recovery.close_path(path, w.now, w.log) == closes);
    if (!closes)
    {
        CRD_FUZZ_REQUIRE(w.log.count == from);
        return;
    }
    take_closing_losses(w, path, from);
    w.spaces[2U + path] = ModelSpace{};
    w.open[path] = false;
    w.stopped[path] = false;
    for (usize other = 0U; other < kQuicMaxPaths; ++other)
    {
        w.restart_due[other] = w.restart_due[other] || w.last_via[other] == path;
    }
    w.log_ended = w.log_ended || path == 0U;
}

void op_move(World& w, QuicSpace id, usize path)
{
    const bool app = id == QuicSpace::Application;
    const usize index = app ? 2U + path : static_cast<usize>(id);
    const usize size = app ? 32U : 8U;
    QuicSentEntry* own = w.application[path];
    QuicSentEntry* other = w.other_application[path];
    if (id == QuicSpace::Initial)
    {
        own = w.initial;
        other = w.other_initial;
    }
    else if (id == QuicSpace::Handshake)
    {
        own = w.handshake;
        other = w.other_handshake;
    }
    const bool gone = !app && w.spaces[index].discarded;
    Span<QuicSentEntry> storage;
    if (!gone)
    {
        storage = Span<QuicSentEntry>(w.moved[index] ? own : other, size);
    }
    const bool held = w.usable(id, path);
    const usize stored = w.recovery.stored(id, path);
    const bool before = w.recovery.storage_size(id, path) != 0U;
    CRD_FUZZ_REQUIRE(w.recovery.swap_storage(id, storage, path) == held);
    if (!held)
    {
        CRD_FUZZ_REQUIRE(w.recovery.storage_size(id, path) == 0U && w.recovery.stored(id, path) == 0U);
        return;
    }
    CRD_FUZZ_REQUIRE(w.recovery.stored(id, path) == stored);
    CRD_FUZZ_REQUIRE(w.recovery.storage_size(id, path) == (gone ? 0U : size));
    if (before)
    {
        CRD_FUZZ_REQUIRE(storage.size() == size && storage.data() == (w.moved[index] ? other : own));
    }
    w.moved[index] = !gone && !w.moved[index];
}

void op_state(World& w, QuicSpace id, usize path, u8 p)
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
    if (id == QuicSpace::Application && (p & 0x40U) != 0U)
    {
        op_toggle(w, path, p);
    }
    else if ((p & 0x80U) != 0U)
    {
        op_move(w, id, path);
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
    if ((data[0] & 2U) != 0U)
    {
        w.per_ack = 1U + ((data[0] >> 2U) & 7U);
        w.exponent = (data[0] >> 5U) & 7U;
        w.recovery.set_receive_timestamps(w.per_ack, w.exponent);
    }
    for (usize at = 1U; at + 1U < size; at += 2U)
    {
        const u8        op = data[at];
        const u8        p  = data[at + 1U];
        const QuicSpace id = static_cast<QuicSpace>((op >> 3U) % 3U);
        const usize path = (id == QuicSpace::Application) ? (op >> 5U) & 3U : 0U;
        const usize via = (id == QuicSpace::Application && (op & 0x80U) != 0U) ? (path + 1U) % kQuicMaxPaths : path;
        PathState before[kQuicMaxPaths];
        snapshot(w.recovery, w.controls, before);
        usize touched = owner_of(id, path); // the slot this operation may change
        switch (op & 7U)
        {
        case 0U:
            op_send(w, id, path, p);
            break;
        case 1U:
            op_ack(w, id, path, via, p, data + at + 2U, (size - at - 2U < 6U) ? size - at - 2U : 6U);
            break;
        case 2U:
            op_receive(w, id, path, p, (at + 2U < size) ? data[at + 2U] : 0U);
            break;
        case 3U:
            op_write(w, id, path, p);
            break;
        case 4U:
        case 5U:
            w.now += static_cast<u64>(p) * static_cast<u64>(p) * ((op & 1U) != 0U ? 1000000U : 10000U);
            break;
        case 6U:
            touched = op_timeout(w);
            break;
        default:
            op_state(w, id, path, p);
            break;
        }
        PathState after[kQuicMaxPaths];
        snapshot(w.recovery, w.controls, after);
        for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
        {
            CRD_FUZZ_REQUIRE(slot == touched || same(before[slot], after[slot]));
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
    // With receive timestamps (four an ACK frame, units of 2 us): a server receives eight Application packets, two
    // out of order, writes an ACK frame reporting the four latest, sends four packets and reads a timed report.
    u8 timed[2U + 2U * 40U] = {0x2FU};
    at = 1U;
    const auto put_timed = [&](u8 op, u8 p)
    {
        timed[at++] = op;
        timed[at++] = p;
    };
    put_timed(0x07U, 0x00U);
    const u8 arrivals[] = {0U, 1U, 2U, 3U, 4U, 6U, 5U, 7U};
    for (const u8 i : arrivals)
    {
        // Packet 16 * i (the next operation byte, 0x04, ends the number and makes it ack-eliciting), then 90 us.
        put_timed(0x12U, i);
        put_timed(0x04U, 0x03U);
    }
    put_timed(0x13U, 0x7FU);
    for (u8 i = 0U; i < 4U; ++i)
    {
        put_timed(0x10U, 0x04U);
        put_timed(0x04U, 0x20U);
    }
    put_timed(0x11U, 0x01U);
    put_timed(0x01U, 0x01U);
    sink.add("server-application-timestamps", timed, at);
    // A confirmed server's Application storage filled with packets nothing acknowledges, then probe timeouts: each
    // gives up the oldest for its probes.
    u8 full[2U + 2U * 40U] = {0x01U};
    at = 1U;
    const auto add = [&](u8 op, u8 p)
    {
        full[at++] = op;
        full[at++] = p;
    };
    add(0x07U, 0x00U);
    for (u8 i = 0U; i < 33U; ++i)
    {
        add(0x10U, 0x04U);
    }
    for (u8 i = 0U; i < 3U; ++i)
    {
        add(0x05U, 0xFFU);
        add(0x06U, 0x00U);
    }
    sink.add("full-storage-probe", full, at);
    // Multipath (bits 5 and 6 of an Application operation name the slot, bit 7 an ACK frame's arrival through the
    // next): a confirmed server opens slot 1, sends on both slots, acknowledges slot 1's packets through it and slot
    // 0's through slot 1 (the minimum restarts), lets probes fire, closes slot 1 with packets outstanding and opens it
    // again.
    u8 paths[2U + 2U * 40U] = {0x01U};
    at = 1U;
    const auto put_path = [&](u8 op, u8 p)
    {
        paths[at++] = op;
        paths[at++] = p;
    };
    put_path(0x07U, 0x00U);
    put_path(0x2FU, 0x40U);
    for (u8 i = 0U; i < 4U; ++i)
    {
        put_path(0x28U, 0x04U);
        put_path(0x10U, 0x04U);
        put_path(0x04U, 0x20U);
    }
    put_path(0x29U, 0x00U);
    put_path(0x11U, 0x00U);
    put_path(0x89U, 0x00U);
    put_path(0x05U, 0xFFU);
    put_path(0x06U, 0x00U);
    put_path(0x28U, 0x04U);
    put_path(0x28U, 0x04U);
    put_path(0x2FU, 0x40U);
    put_path(0x2FU, 0x40U);
    put_path(0x28U, 0x04U);
    put_path(0x29U, 0x00U);
    sink.add("multipath-two-slots", paths, at);
    // A confirmed server whose Initial and Handshake keys are gone opens slot 2, closes slot 0 (its receive
    // timestamps end) and carries on over slot 2 alone, which it cannot close.
    u8 handover[2U + 2U * 20U] = {0x2FU};
    at = 1U;
    const auto put_handover = [&](u8 op, u8 p)
    {
        handover[at++] = op;
        handover[at++] = p;
    };
    put_handover(0x07U, 0x40U);
    put_handover(0x0FU, 0x40U);
    put_handover(0x47U, 0x40U);
    put_handover(0x10U, 0x04U);
    put_handover(0x12U, 0x01U);
    put_handover(0x04U, 0x04U);
    put_handover(0x17U, 0x40U);
    put_handover(0x13U, 0x7FU);
    put_handover(0x40U, 0x04U);
    put_handover(0x42U, 0x01U);
    put_handover(0x04U, 0x04U);
    put_handover(0x43U, 0x7FU);
    put_handover(0x41U, 0x00U);
    put_handover(0x47U, 0x40U);
    sink.add("multipath-slot-zero-closed", handover, at);
    // Timestamps (two an ACK frame, 1 us): a server receives 4081, writes an ACK frame for the next packet, receives
    // 80, sends that packet and has it acknowledged, so 80's range is retired; a copy of 80 then never reaches
    // recovery (the replay window refuses it), and the next ACK frame reports 80 once, at its one arrival.
    const u8 retired[] = {0x07U, 0x12U, 0xFFU, 0x13U, 0xA6U, 0x04U, 0x03U, 0x12U, 0x05U, 0x04U, 0x03U, 0x10U,
                          0x03U, 0x11U, 0x13U, 0x20U, 0x01U, 0x12U, 0x05U, 0x04U, 0x03U, 0x13U, 0x20U};
    sink.add("copy-after-retired-range", retired, sizeof(retired));
    // An optimistic acknowledgement past the last number sent.
    const u8 optimistic[] = {0x00U, 0x10U, 0x04U, 0x10U, 0x05U, 0x11U, 0x80U, 0x00U, 0x00U};
    sink.add("optimistic-ack", optimistic, sizeof(optimistic));
}
