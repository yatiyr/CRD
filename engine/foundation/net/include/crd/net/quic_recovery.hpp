#pragma once

// crd-net -- QUIC acknowledgements and loss recovery (4.2a; contract: docs/design/network-foundation.md#quic-recovery).
// Sources: RFC 9000 sections 13.1, 13.2 and 21.4, RFC 9002 sections 5, 6 and appendix A.
//
//   receiving   QuicAckTracker holds one space's received packet numbers as at most kQuicAckRanges merged ranges and
//               owes an ACK (section 13.2.1 and 13.2.2): at once for an ack-eliciting Initial or Handshake packet, a
//               packet out of order or after a gap and a CE-marked packet, after two ack-eliciting packets, otherwise
//               within max_ack_delay. It writes the ACK frame (the smallest ranges left out when the room is short,
//               the delay measured from the largest's arrival) and forgets what an acknowledged ACK covered (13.2.4).
//               Dropping ranges is legal because QuicPacketProtection's window refuses a number already opened and
//               every number below its span, so a forgotten packet is never accepted twice (13.2.3).
//   sending     each space's sent packets sit in number order in the caller's QuicSentEntry storage, which bounds
//               the packets a connection holds in flight: a full space refuses the next packet (can_send), and data
//               stops short of the last entries, which ACK-only packets and probes keep for themselves. An
//               acknowledgement of a number this side never sent (above the largest sent, or a gap it left in its
//               numbering, which is how a sender skips numbers against optimistic acknowledgement, RFC 9000 section
//               21.4) is AckOfUnsent, PROTOCOL_VIOLATION (section 13.1), and changes nothing.
//   recovery    QuicRecovery is appendix A in code: RTT samples (section 5), loss by the packet and time thresholds
//               (6.1), the probe timeout with its backoff, the Application space held until confirmation, a client's
//               anti-deadlock probe and a server blocked by the amplification limit (6.2), key discards (6.4) and
//               persistent congestion's establishment (7.6.2).
//   reordering  a packet declared lost stays held until a probe period passes (or its slot is needed), so a late
//               acknowledgement shows the loss was spurious; the thresholds then rise (6.1) by RFC 8985's mechanism:
//               the packet threshold to the reordering seen, the time threshold by a quarter of min_rtt per round
//               trip, capped at smoothed_rtt, both back to RFC 9002's after 16 loss detections with no spurious loss.
//               Both are capped, so a peer timing its ACKs to fake reordering can at worst leave a loss to the time
//               threshold (T09).
//   congestion  NET-PATH's controllers (congestion.hpp) take the per-packet events (QuicRecoveryEvents, spurious
//               losses among them) and the outcomes' RTT samples and persistent congestion; the connection applies
//               their window and pacing, so nothing here limits sending beyond the storage.
//   time        every call takes the time in nanoseconds (TransportClock::now_ns); nothing reads a clock.
//   threads     a QuicRecovery belongs to its connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_transport_parameters.hpp>
#include <crd/net/quota.hpp>

namespace crd::net
{
inline constexpr u64   kQuicPacketThreshold        = 3U;          // RFC 9002 section 6.1.1
inline constexpr u64   kQuicMaxPacketThreshold     = 32U;         // past this, a loss is the time threshold's
inline constexpr u32   kQuicMaxReorderSteps        = 255U;        // quarters of min_rtt; the window caps at srtt
inline constexpr u32   kQuicReorderPersist         = 16U;         // RFC 8985 section 6.2, step 4
inline constexpr u64   kQuicTimeThresholdNumerator = 9U;          // 9/8 of an RTT, section 6.1.2
inline constexpr u64   kQuicTimeThresholdDivisor   = 8U;
inline constexpr u64   kQuicGranularityNs          = 1000000U;    // 1 ms, section 6.1.2
inline constexpr u64   kQuicInitialRttNs           = 333000000U;  // 333 ms, section 6.2.2
inline constexpr u64   kQuicPersistentCongestionThreshold = 3U;   // section 7.6.1
inline constexpr u32   kQuicAckElicitingThreshold  = 2U;          // RFC 9000 section 13.2.2
inline constexpr usize kQuicAckRanges              = 32U;         // the ranges one space remembers
inline constexpr u64   kQuicNoPacket               = ~u64{0};

// What an ACK frame the tracker wrote acknowledges: its bytes (0 for nothing written) and its largest packet number,
// which the packet carrying it records (QuicSentPacket::carried_ack).
struct QuicAckWrite
{
    usize bytes   = 0U;
    u64   largest = kQuicNoPacket;
};

// One space's received packet numbers and the ACK they owe.
class QuicAckTracker
{
public:
    // Forgets everything; `space` picks the rule for when an ACK is owed.
    void reset(QuicSpace space) noexcept;

    // Records packet `number`, which QuicPacketProtection opened and whose frames were processed, received at
    // `received_ns` (its datagram's arrival, so a packet buffered for its keys keeps its time). False for a number
    // already recorded, which changes nothing. `max_ack_delay_ns` is this side's advertised max_ack_delay.
    bool on_received(u64 number, bool ack_eliciting, Ecn ecn, u64 received_ns, u64 max_ack_delay_ns) noexcept;

    // When the owed ACK must be sent: kNeverNs when none is owed. An ACK may still ride along in any packet while
    // ranges() is not empty; only ack-eliciting packets make one owed (section 13.2.1).
    [[nodiscard]] u64 ack_deadline_ns() const noexcept { return m_deadline_ns; }

    // Writes the ACK frame for the recorded ranges at the start of `out`, as many of the largest ranges as fit, with
    // the delay since the largest arrived scaled down by `exponent` and the ECN counts once a marked packet arrived.
    // Writing settles the owed ACK. Nothing is written when no range is recorded or not even one range fits.
    [[nodiscard]] QuicAckWrite write(cont::Span<u8> out, u64 now_ns, u64 exponent) noexcept;

    // The peer acknowledged a packet that carried an ACK frame up to `largest`: the ranges at or below it are no
    // longer sent, except that the largest received number always stays (section 13.2.4).
    void on_ack_acknowledged(u64 largest) noexcept;

    // The ranges, largest first, each at least two below the one before.
    [[nodiscard]] cont::ConstSpan<QuicAckRange> ranges() const noexcept
    {
        return cont::ConstSpan<QuicAckRange>(m_ranges, m_count);
    }
    [[nodiscard]] const EcnCounts& ecn() const noexcept { return m_ecn; }

private:
    void insert(u64 number) noexcept;

    QuicAckRange  m_ranges[kQuicAckRanges] = {};
    usize         m_count = 0U;
    QuicSpace     m_space = QuicSpace::Initial;
    u64           m_largest_ns = 0U;                  // when the largest recorded number arrived
    u64           m_largest_eliciting = kQuicNoPacket; // the largest ack-eliciting number received
    u32           m_unacked_eliciting = 0U;           // ack-eliciting packets since the last ACK written
    u64           m_deadline_ns = kNeverNs;
    EcnCounts m_ecn;
    bool          m_ecn_seen = false;
};

// One sent packet as the sender records it. `token` is the caller's handle on what the packet carried (its frames to
// resend or settle); `carried_ack` the largest number of an ACK frame in it, kQuicNoPacket for none.
struct QuicSentPacket
{
    u64  number = 0U;
    u64  sent_ns = 0U;
    u64  carried_ack = kQuicNoPacket;
    u32  token = 0U;
    u32  bytes = 0U;
    bool ack_eliciting = false;
    bool in_flight = false; // counted in bytes in flight: a packet carrying PADDING, and every ack-eliciting one
    bool path_probe = false; // a PMTU probe (quic_pmtu.hpp): its loss starts no persistent congestion
    Ecn  ecn = Ecn::NotEct; // the codepoint its datagram was marked with (quic_ecn.hpp)
};

// Storage for one sent packet: QuicRecovery's to fill.
struct QuicSentEntry
{
    QuicSentPacket packet;
    u64            lost_ns = 0U; // when it was declared lost
    u8             state = 0U;
};

// What the recovery tells the connection and its congestion controller about each packet it settles. Lost packets
// of one detection come before the acknowledged packets of the same ACK frame (appendix A.7).
class QuicRecoveryEvents
{
public:
    virtual ~QuicRecoveryEvents() = default;
    virtual void on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept = 0;
    virtual void on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept = 0;
    // A packet of a space whose keys were discarded: neither acknowledged nor lost (section 6.4).
    virtual void on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept = 0;
    // A packet declared lost at `lost_ns` that an ACK frame acknowledged within its probe period (section 6.1): its
    // loss was reported already, and a congestion controller may undo its response (NET-PATH).
    virtual void on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns) noexcept;

protected:
    QuicRecoveryEvents() = default;
    QuicRecoveryEvents(const QuicRecoveryEvents&) = default;
    QuicRecoveryEvents(QuicRecoveryEvents&&) noexcept = default;
    QuicRecoveryEvents& operator=(const QuicRecoveryEvents&) = default;
    QuicRecoveryEvents& operator=(QuicRecoveryEvents&&) noexcept = default;
};

enum class QuicRecoveryError : u8
{
    None,
    AckOfUnsent, // PROTOCOL_VIOLATION: an acknowledged number this side never sent
    Full,        // the space's storage holds no room for another packet
    BadNumber,   // a packet number not above the space's last
    Discarded,   // the space's keys were discarded
};

// The packets one detection declared lost.
struct QuicLoss
{
    usize packets = 0U;
    u64   bytes = 0U;          // of those in flight
    u64   newest_sent_ns = 0U; // when the newest of them was sent: a controller's congestion event
    bool  persistent_congestion = false;
};

struct QuicAckOutcome
{
    QuicRecoveryError error = QuicRecoveryError::None;
    usize             acked_packets = 0U;
    u64               acked_bytes = 0U; // of those in flight
    // Packets declared lost that this frame acknowledges (RFC 9002 section 6.1), each reported by on_spurious.
    usize             spurious_packets = 0U;
    u64               spurious_bytes = 0U; // of those that were in flight
    // For ECN validation (quic_ecn.hpp): whether the frame raised the space's largest acknowledged, the packets it
    // newly acknowledges (spurious losses among them) that were sent marked ECT(0) and ECT(1), and when the largest
    // of them was sent.
    bool              advanced = false;
    u64               ect0_packets = 0U;
    u64               ect1_packets = 0U;
    u64               newest_sent_ns = 0U;
    bool              rtt_sampled = false;
    QuicLoss          lost;
};

enum class QuicTimeoutKind : u8
{
    None,         // nothing was due
    Loss,         // the time threshold declared packets lost
    Probe,        // send `probes` ack-eliciting packets in `space` (section 6.2.4; other spaces' data may coalesce)
    AntiDeadlock, // a client sends one ack-eliciting packet in `space`: Handshake with its keys, else a padded Initial
};

struct QuicTimeout
{
    QuicTimeoutKind kind = QuicTimeoutKind::None;
    QuicSpace       space = QuicSpace::Initial;
    u8              probes = 0U;
    QuicLoss        lost;
};

// The RTT estimate (RFC 9002 section 5).
struct QuicRtt
{
    u64 latest_ns = 0U;
    u64 min_ns = 0U;
    u64 smoothed_ns = kQuicInitialRttNs;
    u64 variance_ns = kQuicInitialRttNs / 2U;
    u64 first_sample_ns = kNeverNs; // when the first sample was taken

    [[nodiscard]] bool sampled() const noexcept { return first_sample_ns != kNeverNs; }
    // smoothed + max(4 * rttvar, kGranularity): the probe timeout before max_ack_delay and the backoff.
    [[nodiscard]] u64 probe_base_ns() const noexcept;
    // Takes `latest_ns`, the time from a packet's sending to its acknowledgement, at `now_ns`, with the peer's
    // `ack_delay_ns`, capped at `max_ack_delay_ns` once `confirmed`.
    void sample(u64 latest_ns, u64 ack_delay_ns, bool confirmed, u64 max_ack_delay_ns, u64 now_ns) noexcept;
};

struct QuicRecoveryConfig
{
    crypto::QuicSide side = crypto::QuicSide::Client;
    u64              initial_rtt_ns = kQuicInitialRttNs;
    u64              local_max_ack_delay_ns = kQuicDefaultMaxAckDelay * 1000000U; // what this side advertises
    u64              local_ack_delay_exponent = kQuicDefaultAckDelayExponent;
    // The last entries of each space, kept from data: only ACK-only packets and probes take them (can_send_data).
    usize            reserved_entries = 0U;
};

// A connection's acknowledgements and loss recovery across its three packet number spaces.
class QuicRecovery
{
public:
    // `storage` holds each space's sent packets (Initial, Handshake, Application), each span at least one entry; it
    // stays the caller's and must outlive the recovery.
    QuicRecovery(const QuicRecoveryConfig& config, cont::Span<QuicSentEntry> initial,
                 cont::Span<QuicSentEntry> handshake, cont::Span<QuicSentEntry> application) noexcept;

    // ---- What the connection learns ----

    // The peer's authenticated max_ack_delay and ack_delay_exponent (transport parameters).
    void set_peer_parameters(u64 max_ack_delay_ns, u64 ack_delay_exponent, u64 now_ns) noexcept;
    // The handshake is confirmed (RFC 9001 section 4.1.2): max_ack_delay caps samples and the Application space's
    // probe timeout arms.
    void confirm_handshake(u64 now_ns) noexcept;
    // A client installed Handshake keys: its anti-deadlock probe is then a Handshake packet.
    void set_handshake_keys(u64 now_ns) noexcept;
    // A server can send nothing more before the client's address is validated (RFC 9000 section 8.1): its timer
    // waits for a datagram. Clearing it re-arms the timer, which may then be due already (appendix A.6).
    void set_amplification_blocked(bool blocked, u64 now_ns) noexcept;
    // Initial or Handshake keys were discarded (section 6.4): the space's packets leave bytes in flight as
    // on_discarded, its received ranges are forgotten and the probe count resets. False for the Application space.
    [[nodiscard]] bool discard(QuicSpace space, u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // A client accepted a Retry (section 6.3, RFC 9000 section 17.2.5.3): every outstanding packet leaves bytes in
    // flight as on_discarded, the timers and the probe count reset and packet numbers continue. The time from the
    // first Initial to the Retry replaces the initial RTT estimate when no probe was sent (section 6.2.2); it is no
    // sample.
    void retry(u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // A client's server rejected 0-RTT (section 6.4: the recovery state of every 0-RTT packet in flight is
    // discarded): every outstanding Application packet, all of them 0-RTT before the client's first 1-RTT packet,
    // leaves bytes in flight as on_discarded; packet numbers continue.
    void reject_early(u64 now_ns, QuicRecoveryEvents& events) noexcept;

    // ---- Sending ----

    // Whether `space` can record another packet; a connection writes none when it cannot.
    [[nodiscard]] bool can_send(QuicSpace space) const noexcept;
    // Whether it can record one that is neither ACK-only nor a probe. Two ends full of each other's unacknowledged
    // data must still be able to acknowledge it, or neither ever frees a slot.
    [[nodiscard]] bool can_send_data(QuicSpace space) const noexcept;
    // Records `packet`, sent now (its sent_ns): numbers rise within a space and may leave gaps (skipped numbers).
    [[nodiscard]] QuicRecoveryError on_sent(QuicSpace space, const QuicSentPacket& packet) noexcept;
    // The ACK frame `space` owes or may carry, written into `out` (QuicAckTracker::write) with the exponent that
    // space uses: this side's own for Application, the default 3 for Initial and Handshake, whose ACK frames are
    // sent before the parameters are authenticated.
    [[nodiscard]] QuicAckWrite write_ack(QuicSpace space, cont::Span<u8> out, u64 now_ns) noexcept;

    // ---- Receiving ----

    // A packet of `space` opened and processed (QuicAckTracker::on_received); false for a duplicate or a discarded
    // space.
    bool on_received(QuicSpace space, u64 number, bool ack_eliciting, Ecn ecn, u64 received_ns) noexcept;
    // An ACK frame received in a packet of `space` (appendix A.7). An error changes nothing.
    [[nodiscard]] QuicAckOutcome on_ack(QuicSpace space, const QuicAck& ack, u64 now_ns,
                                        QuicRecoveryEvents& events) noexcept;

    // ---- Timers ----

    // The loss detection timer (appendix A.8): kNeverNs when disarmed.
    [[nodiscard]] u64 loss_timer_ns() const noexcept { return m_timer_ns; }
    // The earliest of the loss detection timer and every space's owed ACK.
    [[nodiscard]] u64 next_timer_ns() const noexcept;
    // Runs the loss detection timer if it is due at `now_ns` (appendix A.9); owed ACKs are the connection's to send
    // (ack_deadline_ns).
    [[nodiscard]] QuicTimeout on_timeout(u64 now_ns, QuicRecoveryEvents& events) noexcept;

    // ---- State ----

    [[nodiscard]] const QuicRtt& rtt() const noexcept { return m_rtt; }
    [[nodiscard]] u64 bytes_in_flight() const noexcept { return m_bytes_in_flight; }
    [[nodiscard]] u32 pto_count() const noexcept { return m_pto_count; }
    [[nodiscard]] u64 largest_acked(QuicSpace space) const noexcept;
    [[nodiscard]] u64 ack_deadline_ns(QuicSpace space) const noexcept;
    [[nodiscard]] const QuicAckTracker& received(QuicSpace space) const noexcept;
    // The probe timeout's period now: probe_base + max_ack_delay (Application only) shifted by the backoff.
    [[nodiscard]] u64 probe_period_ns(QuicSpace space) const noexcept;
    // RFC 9002 section 7.6.1: (probe_base + max_ack_delay) * kQuicPersistentCongestionThreshold.
    [[nodiscard]] u64 persistent_congestion_ns() const noexcept;
    // The packet threshold now: kQuicPacketThreshold, raised by spurious losses up to kQuicMaxPacketThreshold.
    [[nodiscard]] u64 packet_threshold() const noexcept { return m_packet_threshold; }
    // Quarters of min_rtt the time threshold's window holds (RFC 8985 section 6.2's reo_wnd_mult less one).
    [[nodiscard]] u32 reorder_steps() const noexcept { return m_reorder_steps; }
    // The time threshold now: max(9/8 max(smoothed, latest) + min(steps * min_rtt / 4, smoothed), 1 ms).
    [[nodiscard]] u64 loss_delay_ns() const noexcept;

private:
    struct Space
    {
        cont::Span<QuicSentEntry> entries;
        usize                     head = 0U;  // the oldest entry
        usize                     count = 0U; // entries held, settled ones included until they reach the head
        u64                       next_number = 0U;   // the last sent number plus one
        u64                       largest_acked = kQuicNoPacket;
        u64                       loss_time_ns = kNeverNs;
        u64                       last_eliciting_ns = 0U;
        u64                       reorder_round = 0U; // next_number when the window last grew: one step a round
        usize                     eliciting_in_flight = 0U;
        bool                      discarded = false;
        QuicAckTracker            received;
    };

    [[nodiscard]] QuicSentEntry& entry(Space& space, usize index) noexcept;
    [[nodiscard]] usize lower_bound(Space& space, u64 number) noexcept;
    [[nodiscard]] bool acknowledges_unsent(Space& space, const QuicAck& ack) noexcept;
    void settle(Space& space, QuicSentEntry& slot) noexcept;
    // Every outstanding packet of `id` leaves bytes in flight as on_discarded, and the space holds none.
    void drop_outstanding(QuicSpace id, QuicRecoveryEvents& events) noexcept;
    // Drops settled entries from the head: acknowledged ones, and lost ones past their probe period or, when `room`
    // asks for a slot, until one is free.
    void compact(QuicSpace id, u64 now_ns, bool room) noexcept;
    [[nodiscard]] bool held_lost(QuicSpace id, const QuicSentEntry& slot, u64 now_ns) const noexcept;
    [[nodiscard]] QuicLoss detect_lost(QuicSpace id, u64 now_ns, QuicRecoveryEvents& events) noexcept;
    [[nodiscard]] bool peer_validated() const noexcept;
    [[nodiscard]] bool any_eliciting_in_flight() const noexcept;
    [[nodiscard]] u64 decoded_ack_delay_ns(QuicSpace id, u64 raw) const noexcept;
    [[nodiscard]] bool earliest_loss(u64& time_ns, QuicSpace& id) const noexcept;
    [[nodiscard]] u64 pto_time(u64 now_ns, QuicSpace& id) const noexcept;
    void arm(u64 now_ns) noexcept;

    Space            m_spaces[kQuicSpaces];
    QuicRtt          m_rtt;
    crypto::QuicSide m_side;
    u64              m_local_max_ack_delay_ns;
    u64              m_local_ack_delay_exponent;
    u64              m_peer_max_ack_delay_ns = kQuicDefaultMaxAckDelay * 1000000U;
    u64              m_peer_ack_delay_exponent = kQuicDefaultAckDelayExponent;
    u64              m_bytes_in_flight = 0U;
    u64              m_timer_ns = kNeverNs;
    u64              m_packet_threshold = kQuicPacketThreshold;
    usize            m_reserved_entries = 0U;
    u32              m_reorder_steps = 0U;
    u32              m_reorder_persist = 0U; // loss detections before the thresholds return to RFC 9002's
    u32              m_pto_count = 0U;
    bool             m_confirmed = false;
    bool             m_handshake_keys = false;
    bool             m_handshake_acked = false; // a client received an ACK in a Handshake packet
    bool             m_amplification_blocked = false;
};

} // namespace crd::net
