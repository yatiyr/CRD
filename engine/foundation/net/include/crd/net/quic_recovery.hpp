#pragma once

// crd-net -- QUIC acknowledgements and loss recovery (4.2a; contract: docs/design/network-foundation.md#quic-recovery).
// Sources: RFC 9000 sections 13.1, 13.2 and 21.4, RFC 9002 sections 5, 6 and appendix A.
//
//   receiving   QuicAckTracker holds one space's received packet numbers as at most kQuicAckRanges merged ranges and
//               owes an ACK (section 13.2.1 and 13.2.2): at once for an ack-eliciting Initial or Handshake packet, a
//               packet out of order or after a gap and a CE-marked packet, after two ack-eliciting packets, otherwise
//               within max_ack_delay. It writes the ACK frame (the smallest ranges left out when the room is short,
//               the delay measured from the largest's arrival) and forgets what an acknowledged ACK covered (13.2.4).
//               Dropping ranges is legal because QuicPacketProtection's window of the space (each path's own, with
//               multipath) refuses a number already opened and every number below its span, so a forgotten packet
//               is never accepted twice (13.2.3).
//   sending     each space's sent packets sit in number order in the caller's QuicSentEntry storage, which bounds
//               the packets a connection holds in flight: a full space refuses the next packet (can_send), and data
//               stops short of the last entries, which ACK-only packets and probes keep for themselves. An
//               acknowledgement of a number this side never sent (above the largest sent, or a gap it left in its
//               numbering, which is how a sender skips numbers against optimistic acknowledgement, RFC 9000 section
//               21.4) is AckOfUnsent, PROTOCOL_VIOLATION (section 13.1), and changes nothing.
//   recovery    QuicRecovery is appendix A in code: RTT samples (section 5), loss by the packet and time thresholds
//               (6.1), the probe timeout with its backoff, the Application space held until confirmation, a client's
//               anti-deadlock probe and a server blocked by the amplification limit (6.2), key discards (6.4) and
//               persistent congestion's establishment (7.6.2), which neither PMTU probes nor packets sent in a
//               scheduled handover (handover.hpp) start or end.
//   reordering  a packet declared lost stays held until a probe period passes (or its slot is needed), so a late
//               acknowledgement shows the loss was spurious; the thresholds then rise (6.1) by RFC 8985's mechanism:
//               the packet threshold to the reordering seen, the time threshold by a quarter of min_rtt per round
//               trip, capped at smoothed_rtt, both back to RFC 9002's after 16 loss detections with no spurious loss.
//               Both are capped, so a peer timing its ACKs to fake reordering can at worst leave a loss to the time
//               threshold (T09).
//   timestamps  draft-ietf-quic-receive-ts-03 (NET-PATH, design S73): QuicReceiveTimestampLog keeps the Application
//               space's receive times until an ACK frame reports them (ACK_RECEIVE_TIMESTAMPS, only packets the frame
//               acknowledges, the latest first); QuicPeerReceiveTimes reads a peer's report for the packets a frame
//               acknowledges, for the congestion controller's receive times.
//   paths       RFC 10069 section 5 (NET-PATH, design S78): every open path ID has its own Application space, RTT
//               estimate, probe timeout and its backoff, loss detection, reordering thresholds and bytes in flight.
//               Path IDs live in slots 0 to kQuicMaxPaths - 1, which the connection maps to the IDs (an ID is never
//               reused; a slot is, through close_path and open_path). Slot 0 is the handshake's path, open from
//               construction, and its estimate serves the Initial and Handshake spaces too, so a connection without
//               multipath is slot 0 alone, every call's default. One loss detection timer serves every path, the
//               earliest of theirs. A path's packets may be acknowledged through any open path: the minimum RTT
//               restarts when the path such frames arrive through changes (section 5.4).
//   congestion  NET-PATH's controllers (congestion.hpp) take the per-packet events (QuicRecoveryEvents, spurious
//               losses among them) and the outcomes' RTT samples and persistent congestion; the connection applies
//               their window and pacing, so nothing here limits sending beyond the storage.
//   time        every call takes the time in nanoseconds (TransportClock::now_ns); nothing reads a clock.
//   threads     a QuicRecovery belongs to its connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/delivery_rate.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_path.hpp>
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
// The receive timestamps one ACK frame carries at most, either way: what a connection asks for and reports at most.
inline constexpr usize kQuicMaxReceiveTimestamps = 32U;
inline constexpr u64   kQuicNoPacket               = ~u64{0};
// QuicAckTracker::write's path ID for an ACK frame (path ID 0's space without naming it, RFC 10069 section 2.3).
inline constexpr u64 kQuicAckFrame = ~u64{0};

// What an ACK frame the tracker wrote acknowledges: its bytes (0 for nothing written) and its largest packet number,
// which the packet carrying it records (QuicSentPacket::carried_ack).
struct QuicAckWrite
{
    usize bytes   = 0U;
    u64   largest = kQuicNoPacket;
};

// The receive times of the Application space's packets that no ACK frame reported yet, the latest
// kQuicMaxReceiveTimestamps processed kept. The basis is the first recorded time less a nanosecond, constant for the
// connection; a later time at or below it (a packet held for its keys) is never reported.
class QuicReceiveTimestampLog
{
public:
    // Reports at most `per_ack` timestamps an ACK frame (the peer's max_receive_timestamps_per_ack, kept to
    // kQuicMaxReceiveTimestamps; 0 records nothing) in units of 2^`exponent` microseconds (the peer's
    // receive_timestamps_exponent, at most kQuicMaxReceiveTimestampsExponent).
    void configure(u64 per_ack, u64 exponent) noexcept;
    [[nodiscard]] bool enabled() const noexcept { return m_per_ack != 0U; }
    // Records packet `number`, received at `received_ns`, once.
    void record(u64 number, u64 received_ns) noexcept;
    // Fills `out` (kQuicMaxReceiveTimestamps entries) with the recorded packets that `ranges` acknowledge, the latest
    // received first and at most per_ack of them, as write_quic_ack takes them, and forgets every record (draft
    // section 4.2: reporting is best effort). Returns how many it filled.
    [[nodiscard]] usize take(cont::ConstSpan<QuicAckRange> ranges, cont::Span<QuicReceiveTimestamp> out) noexcept;

private:
    struct Entry
    {
        u64 number = 0U;
        u64 received_ns = 0U;
    };

    Entry m_entries[kQuicMaxReceiveTimestamps] = {};
    usize m_next = 0U; // where the next record goes
    usize m_count = 0U;
    usize m_per_ack = 0U;
    u64 m_exponent = 0U;
    u64 m_basis_ns = kNeverNs;
};

// One space's received packet numbers and the ACK they owe.
class QuicAckTracker
{
public:
    // Forgets everything; `space` picks the rule for when an ACK is owed.
    void reset(QuicSpace space) noexcept;

    // Records packet `number`, which QuicPacketProtection opened and whose frames were processed, received at
    // `received_ns` (its datagram's arrival, so a packet buffered for its keys keeps its time). False for a number
    // already recorded, which changes nothing. `max_ack_delay_ns` is this side's advertised max_ack_delay. A number
    // comes here once: duplicate suppression is the protection's replay window (RFC 9000 section 12.3), and the
    // ranges forget what an acknowledged ACK frame or full storage retires.
    bool on_received(u64 number, bool ack_eliciting, Ecn ecn, u64 received_ns, u64 max_ack_delay_ns) noexcept;

    // When the owed ACK must be sent: kNeverNs when none is owed. An ACK may still ride along in any packet while
    // ranges() is not empty; only ack-eliciting packets make one owed (section 13.2.1).
    [[nodiscard]] u64 ack_deadline_ns() const noexcept { return m_deadline_ns; }

    // Writes the ACK frame for the recorded ranges at the start of `out`, as many of the largest ranges as fit, with
    // the delay since the largest arrived scaled down by `exponent` and the ECN counts once a marked packet arrived.
    // With an enabled `log` holding times of packets those ranges acknowledge, it is an ACK_RECEIVE_TIMESTAMPS frame
    // reporting as many of them, the latest first, as the room left by the ranges holds. With a `path_id` (at most
    // kQuicMaxPathId) it is a PATH_ACK frame of that path ID's space (RFC 10069 section 4.1), which reports no
    // timestamps. Writing settles the owed ACK. Nothing is written when no range is recorded or not even one fits.
    [[nodiscard]] QuicAckWrite write(cont::Span<u8> out, u64 now_ns, u64 exponent,
                                     QuicReceiveTimestampLog* log = nullptr, u64 path_id = kQuicAckFrame) noexcept;

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
    // Sent on a path other than the active one (a path validation, RFC 9000 section 9.4): outside the estimate,
    // bytes in flight and congestion control, as a packet before path_floor() is.
    bool off_path = false;
    // Sent where a scheduled handover may take it (handover.hpp): its loss starts no persistent congestion, and the
    // connection keeps its loss and delay from congestion control.
    bool handover = false;
    Ecn  ecn = Ecn::NotEct; // the codepoint its datagram was marked with (quic_ecn.hpp)
    // The slot of the path ID it was sent on (QuicRecovery's paths): 0 for every Initial and Handshake packet and for
    // a connection without multipath.
    u8 path = 0U;
    // Never 0 for a datagram of one 1-RTT packet: what an ICMP message quoting it must repeat (quic_pmtu.hpp's hints);
    // 0 for any other packet.
    u32 tag = 0U;
    DeliveryStamp delivery; // the connection's delivery when a packet counted in flight left (delivery_rate.hpp)
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
    NoPath,      // a path slot not open or stopped, or a slot other than 0 for the Initial or Handshake space
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
    bool              rtt_handover = false; // the sample's packet was sent in a scheduled handover (handover.hpp)
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
    usize           path = 0U; // the slot whose timer fired: 0 for the Initial and Handshake spaces
    u8              probes = 0U;
    QuicLoss        lost;
};

// The RTT estimate (RFC 9002 section 5).
struct QuicRtt
{
    u64 latest_ns = 0U;
    u64 adjusted_ns = 0U; // latest_ns less the ACK delay taken off it (section 5.3's adjusted_rtt)
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

// One path slot's recovery: its path ID's Application space and the estimate, which slot 0's Initial and Handshake
// spaces share. A connection allocates one, with the slot's sent-packet storage, when a path validates (RFC 10069
// section 7.1) and lends both to QuicRecovery::open_path until close_path returns; slot 0's first is the recovery's
// own, so a connection without multipath allocates none. Only QuicRecovery reads or writes it.
class QuicPathRecovery
{
    friend class QuicRecovery;

    // One packet number space's sent packets and received numbers.
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

    struct State
    {
        Space   application;
        QuicRtt rtt;
        u64     bytes_in_flight = 0U;
        u64     floor = 0U; // QuicRecovery::path_floor()
        u64     packet_threshold = kQuicPacketThreshold;
        u32     reorder_steps = 0U;
        u32     reorder_persist = 0U; // loss detections before the thresholds return to RFC 9002's
        u32     pto_count = 0U;
        usize   min_via = 0U; // the slot whose frames gave rtt.min_ns; kQuicNoPath once it closed
        bool    sending = true; // false once stop_path() left it receiving alone
    };

    State m_state;
};

// A connection's acknowledgements and loss recovery across its Initial and Handshake spaces and the Application space
// of every open path (the paths note above). Calls name a path by its slot, 0 unless they say otherwise; a slot that
// is not open reads as a path with nothing sent or received and RFC 9002's initial estimate.
class QuicRecovery
{
public:
    // `storage` holds each space's sent packets (Initial, Handshake, and the Application space of slot 0), each span
    // at least one entry; it stays the caller's and must outlive the recovery.
    QuicRecovery(const QuicRecoveryConfig& config, cont::Span<QuicSentEntry> initial,
                 cont::Span<QuicSentEntry> handshake, cont::Span<QuicSentEntry> application) noexcept;
    // Slot 0 lives inside: a copy would point at the original's.
    QuicRecovery(const QuicRecovery&) = delete;
    QuicRecovery& operator=(const QuicRecovery&) = delete;
    QuicRecovery(QuicRecovery&&) = delete;
    QuicRecovery& operator=(QuicRecovery&&) = delete;
    ~QuicRecovery() = default;

    // ---- Paths (RFC 10069) ----

    // Opens slot `path` for a new path ID, its recovery in `state` and its Application packets in `storage` (at least
    // one entry), both the caller's until the slot closes: numbers from 0, nothing received, the initial RTT estimate.
    // False, changing nothing, for a slot out of range or open, empty storage, or a `state` an open slot holds.
    [[nodiscard]] bool open_path(usize path, QuicPathRecovery& state, cont::Span<QuicSentEntry> storage) noexcept;
    // The path ID in slot `path` is forgotten (RFC 10069 sections 3.4 and 3.4.3: abandoned, and its packets'
    // acknowledgements waited for). The slot closes first, so on_path() is false for its packets; then each one still
    // outstanding is declared lost (on_lost), for its frames to go on another path, which is no congestion signal. A
    // path whose minimum RTT came from frames received through this slot restarts it (section 5.4). Closing slot 0
    // ends the receive timestamps (only ACK frames, path ID 0's, report them). False, changing nothing, for a slot not
    // open, the last one open, and slot 0 before the handshake is confirmed and the Initial and Handshake spaces
    // discarded. The slot's state and storage are the caller's again when it returns.
    [[nodiscard]] bool close_path(usize path, u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // Slot `path` sends no more (RFC 10069 section 3.4: its path ID abandoned) but still records what it receives,
    // for its PATH_ACK frames to go on another path until close_path (section 3.4.3). Each packet still outstanding
    // is declared lost at once, for its frames to go on another path, which no controller hears as congestion; from
    // then on can_send() is false there, no timer is armed for it, the largest probe period leaves it out and its
    // own probe period loses its backoff. False, changing nothing, for a slot not open or stopped already, the last
    // one sending, and slot 0 before the handshake is confirmed and both handshake spaces discarded (close_path's).
    [[nodiscard]] bool stop_path(usize path, u64 now_ns, QuicRecoveryEvents& events) noexcept;
    [[nodiscard]] bool path_open(usize path) const noexcept
    {
        return path < kQuicMaxPaths && m_paths[path] != nullptr;
    }
    [[nodiscard]] bool path_sending(usize path) const noexcept { return path_open(path) && m_paths[path]->sending; }

    // ---- What the connection learns ----

    // The peer's authenticated max_ack_delay and ack_delay_exponent (transport parameters).
    void set_peer_parameters(u64 max_ack_delay_ns, u64 ack_delay_exponent, u64 now_ns) noexcept;
    // Slot 0's ACK frames report receive timestamps (QuicReceiveTimestampLog::configure): at most `per_ack`, in units
    // of 2^`exponent` microseconds; 0 reports none.
    void set_receive_timestamps(u64 per_ack, u64 exponent) noexcept { m_receive_log.configure(per_ack, exponent); }
    // The handshake is confirmed (RFC 9001 section 4.1.2): max_ack_delay caps samples and the Application spaces'
    // probe timeouts arm.
    void confirm_handshake(u64 now_ns) noexcept;
    // A client installed Handshake keys: its anti-deadlock probe is then a Handshake packet.
    void set_handshake_keys(u64 now_ns) noexcept;
    // A server can send nothing more before the client's address is validated (RFC 9000 section 8.1): its timer
    // waits for a datagram. Clearing it re-arms the timer, which may then be due already (appendix A.6).
    void set_amplification_blocked(bool blocked, u64 now_ns) noexcept;
    // Initial or Handshake keys were discarded (section 6.4): the space's packets leave bytes in flight as
    // on_discarded, its received ranges are forgotten and slot 0's probe count resets. False for the Application
    // space.
    [[nodiscard]] bool discard(QuicSpace space, u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // A client accepted a Retry (section 6.3, RFC 9000 section 17.2.5.3): every outstanding packet leaves bytes in
    // flight as on_discarded, the timers and the probe count reset and packet numbers continue. The time from the
    // first Initial to the Retry replaces the initial RTT estimate when no probe was sent (section 6.2.2); it is no
    // sample.
    void retry(u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // A client's server rejected 0-RTT (section 6.4: the recovery state of every 0-RTT packet in flight is
    // discarded): every outstanding Application packet of slot 0, all of them 0-RTT before the client's first 1-RTT
    // packet, leaves bytes in flight as on_discarded; packet numbers continue.
    void reject_early(u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // The datagrams of slot `path`'s path ID go on a new 4-tuple (RFC 9000 section 9.4, RFC 9002 appendix A.3,
    // RFC 10069 section 5.1): its RTT estimate, probe count and reordering thresholds start again, and every
    // Application packet it sent so far leaves bytes in flight. Such a packet is still acknowledged and declared lost,
    // for its frames, but gives no RTT sample and counts in no acknowledged, lost or spurious bytes and no ECN marks:
    // nothing sent on the old 4-tuple reaches the new one's estimate or its congestion controller.
    void restart_path(u64 now_ns, usize path = 0U) noexcept;
    // The first Application packet of slot `path`'s current 4-tuple (0 until a restart).
    [[nodiscard]] u64 path_floor(usize path = 0U) const noexcept { return view(path).floor; }
    // Whether `packet` of `space` was sent where its path's estimate follows: on an open slot, at or past its
    // path_floor() and not off_path.
    [[nodiscard]] bool on_path(QuicSpace space, const QuicSentPacket& packet) const noexcept;

    // ---- Sending ----

    // Whether `space` of slot `path` can record another packet; a connection writes none when it cannot.
    [[nodiscard]] bool can_send(QuicSpace space, usize path = 0U) const noexcept;
    // Whether it can record one that is neither ACK-only nor a probe. Two ends full of each other's unacknowledged
    // data must still be able to acknowledge it, or neither ever frees a slot.
    [[nodiscard]] bool can_send_data(QuicSpace space, usize path = 0U) const noexcept;
    // Records `packet`, sent now (its sent_ns) on its slot (its path): numbers rise within a space and may leave gaps
    // (skipped numbers).
    [[nodiscard]] QuicRecoveryError on_sent(QuicSpace space, const QuicSentPacket& packet) noexcept;
    // The ACK frame `space` of slot `path` owes or may carry, written into `out` (QuicAckTracker::write) with the
    // exponent that space uses: this side's own for Application, the default 3 for Initial and Handshake, whose ACK
    // frames are sent before the parameters are authenticated. Only slot 0's report receive timestamps, and only in
    // an ACK frame; with a `path_id` it is the slot's PATH_ACK frame.
    [[nodiscard]] QuicAckWrite write_ack(QuicSpace space, cont::Span<u8> out, u64 now_ns, usize path = 0U,
                                         u64 path_id = kQuicAckFrame) noexcept;
    // A packet that carried slot `path`'s Application acknowledgement up to `largest` was acknowledged
    // (QuicAckTracker::on_ack_acknowledged). A packet's carried_ack does this for its own space and slot; this is for
    // an acknowledgement of one path that went on another (a PATH_ACK). Nothing for a slot not open.
    void on_ack_acknowledged(usize path, u64 largest) noexcept;

    // ---- Receiving ----

    // A packet of `space` and slot `path` opened and processed (QuicAckTracker::on_received, and the receive
    // timestamp log for slot 0's Application space), each number once (QuicAckTracker::on_received); false for a
    // number still recorded, a discarded space or a slot not open.
    bool on_received(QuicSpace space, u64 number, bool ack_eliciting, Ecn ecn, u64 received_ns,
                     usize path = 0U) noexcept;
    // An ACK frame of `space` (appendix A.7) acknowledging slot `path`'s packets (an ACK frame's are slot 0's, a
    // PATH_ACK's its path ID's), received in a packet of slot `via`. NoPath for either slot not open or `path`
    // stopped (stop_path: its packets are lost already); an error changes nothing.
    [[nodiscard]] QuicAckOutcome on_ack(QuicSpace space, const QuicAck& ack, u64 now_ns, QuicRecoveryEvents& events,
                                        usize path = 0U, usize via = 0U) noexcept;

    // ---- Timers ----

    // The loss detection timer (appendix A.8), the earliest of every path's: kNeverNs when disarmed.
    [[nodiscard]] u64 loss_timer_ns() const noexcept { return m_timer_ns; }
    // The earliest of the loss detection timer and every space's owed ACK.
    [[nodiscard]] u64 next_timer_ns() const noexcept;
    // Runs the loss detection timer if it is due at `now_ns` (appendix A.9) for the path whose timer it is; owed ACKs
    // are the connection's to send (ack_deadline_ns).
    [[nodiscard]] QuicTimeout on_timeout(u64 now_ns, QuicRecoveryEvents& events) noexcept;

    // ---- State, of slot `path` ----

    [[nodiscard]] const QuicRtt& rtt(usize path = 0U) const noexcept { return view(path).rtt; }
    [[nodiscard]] u64 bytes_in_flight(usize path = 0U) const noexcept { return view(path).bytes_in_flight; }
    [[nodiscard]] u32 pto_count(usize path = 0U) const noexcept { return view(path).pto_count; }
    [[nodiscard]] u64 largest_acked(QuicSpace space, usize path = 0U) const noexcept;
    [[nodiscard]] u64 ack_deadline_ns(QuicSpace space, usize path = 0U) const noexcept;
    [[nodiscard]] const QuicAckTracker& received(QuicSpace space, usize path = 0U) const noexcept;
    // The probe timeout's period now: probe_base + max_ack_delay (Application only) shifted by the backoff.
    [[nodiscard]] u64 probe_period_ns(QuicSpace space, usize path = 0U) const noexcept;
    // The largest Application probe period among the open paths still sending: three of them are what a key update
    // and closing wait (RFC 10069 sections 2.5 and 2.6).
    [[nodiscard]] u64 largest_probe_period_ns() const noexcept;
    // RFC 9002 section 7.6.1: (probe_base + max_ack_delay) * kQuicPersistentCongestionThreshold.
    [[nodiscard]] u64 persistent_congestion_ns(usize path = 0U) const noexcept;
    // The packet threshold now: kQuicPacketThreshold, raised by spurious losses up to kQuicMaxPacketThreshold.
    [[nodiscard]] u64 packet_threshold(usize path = 0U) const noexcept { return view(path).packet_threshold; }
    // Quarters of min_rtt the time threshold's window holds (RFC 8985 section 6.2's reo_wnd_mult less one).
    [[nodiscard]] u32 reorder_steps(usize path = 0U) const noexcept { return view(path).reorder_steps; }
    // The time threshold now: max(9/8 max(smoothed, latest) + min(steps * min_rtt / 4, smoothed), 1 ms).
    [[nodiscard]] u64 loss_delay_ns(usize path = 0U) const noexcept;
    // The newest packet of `space` sent with `tag` that no ACK frame acknowledged (lost ones included); null for none
    // and for a zero tag.
    [[nodiscard]] const QuicSentPacket* unacknowledged(QuicSpace space, u32 tag, usize path = 0U) const noexcept;
    // When packet `number` of `space` was sent, while no ACK frame acknowledged it (lost ones included); kNeverNs for
    // a packet acknowledged, never sent or no longer held.
    [[nodiscard]] u64 unacknowledged_sent_ns(QuicSpace space, u64 number, usize path = 0U) const noexcept;

private:
    using Space = QuicPathRecovery::Space;
    using Path = QuicPathRecovery::State;

    // The slot the state accessors read: an empty path for one not open.
    [[nodiscard]] const Path& view(usize path) const noexcept;
    // Whether `space` of slot `path` is one: an open slot for Application, slot 0 for the others.
    [[nodiscard]] bool usable(QuicSpace space, usize path) const noexcept;
    // An Application space of an open slot stop_path() stopped: it records and acknowledges, and sends nothing.
    [[nodiscard]] bool stopped(QuicSpace space, usize path) const noexcept;
    // `space` of slot `path`, and the path whose estimate it follows; both for a usable() pair only.
    [[nodiscard]] Space& space_of(QuicSpace space, usize path) noexcept;
    [[nodiscard]] const Space& space_of(QuicSpace space, usize path) const noexcept;
    [[nodiscard]] Path& owner(QuicSpace space, usize path) noexcept;
    [[nodiscard]] const Path& owner(QuicSpace space, usize path) const noexcept;
    // Starts `path`'s estimate, probe count and thresholds again, as a new path's.
    void reset_estimate(Path& path) const noexcept;
    [[nodiscard]] static QuicSentEntry& entry(Space& space, usize index) noexcept;
    [[nodiscard]] static const QuicSentEntry& entry(const Space& space, usize index) noexcept;
    [[nodiscard]] static usize lower_bound(const Space& space, u64 number) noexcept;
    [[nodiscard]] static bool acknowledges_unsent(const Space& space, const QuicAck& ack) noexcept;
    void settle(QuicSpace id, Space& space, QuicSentEntry& slot) noexcept;
    // Every outstanding packet of `id` in slot `path` leaves bytes in flight as on_discarded, and the space holds
    // none.
    void drop_outstanding(QuicSpace id, usize path, QuicRecoveryEvents& events) noexcept;
    // Drops settled entries from the head: acknowledged ones, and lost ones past their probe period or, when `room`
    // asks for a slot, until one is free.
    void compact(QuicSpace id, usize path, u64 now_ns, bool room) noexcept;
    [[nodiscard]] bool held_lost(QuicSpace id, usize path, const QuicSentEntry& slot, u64 now_ns) const noexcept;
    [[nodiscard]] QuicLoss detect_lost(QuicSpace id, usize path, u64 now_ns, QuicRecoveryEvents& events) noexcept;
    // The entries `space` can still record: free ones and the settled run at the head that compact() gives up.
    [[nodiscard]] static usize room(const Space& space) noexcept;
    // Declares the oldest outstanding packets of `id` lost until it can record `entries` more (section 6.2.4).
    [[nodiscard]] QuicLoss make_room(QuicSpace id, usize path, usize entries, u64 now_ns,
                                     QuicRecoveryEvents& events) noexcept;
    [[nodiscard]] bool peer_validated() const noexcept;
    // Confirmed, the Initial and Handshake spaces discarded: slot 0 may stop or close.
    [[nodiscard]] bool handshake_gone() const noexcept;
    [[nodiscard]] bool any_eliciting_in_flight() const noexcept;
    [[nodiscard]] u64 decoded_ack_delay_ns(QuicSpace id, u64 raw) const noexcept;
    [[nodiscard]] bool earliest_loss(u64& time_ns, QuicSpace& id, usize& path) const noexcept;
    [[nodiscard]] u64 pto_time(u64 now_ns, QuicSpace& id, usize& path) const noexcept;
    void arm(u64 now_ns) noexcept;

    Space            m_spaces[2]; // Initial and Handshake
    QuicPathRecovery m_first; // slot 0's from construction
    Path*            m_paths[kQuicMaxPaths] = {}; // each open slot's; null for a slot not open
    QuicReceiveTimestampLog m_receive_log;
    crypto::QuicSide m_side;
    u64              m_local_max_ack_delay_ns;
    u64              m_local_ack_delay_exponent;
    u64              m_peer_max_ack_delay_ns = kQuicDefaultMaxAckDelay * 1000000U;
    u64              m_peer_ack_delay_exponent = kQuicDefaultAckDelayExponent;
    u64              m_initial_rtt_ns;
    u64              m_timer_ns = kNeverNs;
    usize            m_reserved_entries = 0U;
    bool             m_confirmed = false;
    bool             m_handshake_keys = false;
    bool             m_handshake_acked = false; // a client received an ACK in a Handshake packet
    bool             m_amplification_blocked = false;
};

// The peer's receive times for the packets one ACK_RECEIVE_TIMESTAMPS frame newly acknowledges, in nanoseconds on its
// clock from its basis (only differences mean anything): CongestionPacket::received_ns for each.
class QuicPeerReceiveTimes
{
public:
    // Reads `ack`'s timestamps in units of 2^`exponent` microseconds (this side's receive_timestamps_exponent) for
    // the packets of the Application space `recovery` holds unacknowledged, at most kQuicMaxReceiveTimestamps (a
    // frame reporting more was refused), each packet's first report. The draft's plausibility check (section 9): no
    // one-way delay is longer than the time from its packet's sending to this frame's arrival at `now_ns`, so a frame
    // whose one-way delays (receive less send time, on two clocks) spread wider than the time since the oldest of
    // them was sent, or whose time does not fit in nanoseconds, gives none. Returns how many packets it gave a time.
    usize read(const QuicAck& ack, u64 exponent, const QuicRecovery& recovery, u64 now_ns) noexcept;
    // Packet `number`'s receive time from the frame read last; kNeverNs when it gave none.
    [[nodiscard]] u64 find(u64 number) const noexcept;
    void clear() noexcept { m_count = 0U; }

private:
    struct Entry
    {
        u64 number = 0U;
        u64 received_ns = 0U;
    };

    Entry m_entries[kQuicMaxReceiveTimestamps] = {}; // by packet number
    usize m_count = 0U;
};

} // namespace crd::net
