#pragma once
// crd-net -- what one QUIC path controls of its sending (NET-PATH; contract:
// docs/design/network-foundation.md#multipath).
//
// QuicPathControl is the sending state RFC 10069 section 5 keeps per path, one for each path slot (S78):
//   congestion  the controller (congestion.hpp) with its delivery rate sampler and pacer: the window, the pacing
//               rate and when the next datagram may go (RFC 9002 section 7; S66).
//   ECN         validation and the codepoint (quic_ecn.hpp; S67): a path that fails it is no longer marked, and its
//               controller hears once that the path carries no ECN.
//   path MTU    the search (quic_pmtu.hpp; S68): the controller's datagram size follows what it finds.
//   handover    the link's schedule and its learner (handover.hpp; S76): a covered packet's loss is no congestion
//               signal, no PMTU probe leaves while one is covered, and a learner keeps seeing every loss and sample.
// The recovery's events reach the control of the slot their packet was sent on (QuicSentPacket::path), so one path's
// losses, marks, black holes and handovers never reach another path's controller, sampler or search.
//
//   paths       built for a validated path (its 4-tuple, path()); a move to another address starts it over and the
//               schedule, which was the old link's, ends, while a new port alone keeps it (RFC 9000 section 9.4).
//   ownership   the controller is built in the control's own storage and reads its sampler, so a control is never
//               copied or moved; slot 0's lives in the connection, the others are allocated with their slot.
//   allocation  none.
//   threads     a connection's, on its loop's thread.
//   time        the caller's clock, in nanoseconds.

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/net/congestion.hpp>
#include <crd/net/delivery_rate.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/handover.hpp>
#include <crd/net/quic_ecn.hpp>
#include <crd/net/quic_path.hpp>
#include <crd/net/quic_pmtu.hpp>
#include <crd/net/quic_recovery.hpp>

namespace crd::net
{
// What a path starts from: the connection's configuration (QuicConnectionConfig names each field), which the caller
// checked (congestion_policy_valid, the sizes, at most one of the schedule and the learning).
struct QuicPathControlConfig
{
    CongestionPolicy congestion;
    u32 base_datagram_bytes = kQuicPmtuFloor;
    u32 max_datagram_bytes = kQuicDefaultMaxDatagram;
    bool path_mtu_discovery = true;
    bool ecn = true;
    HandoverSchedule handover;
    HandoverLearning handover_learning;
};

// What an event did that the connection counts (QuicConnectionCounters).
struct QuicPathSignals
{
    bool handover_loss = false; // a loss a scheduled handover may have caused, kept from congestion control
    bool handover_lock = false; // the learner put a schedule in force where none was
    bool ecn_signal = false;    // a rise of the peer's CE counts taken as congestion
};

// (MSVC C4324: the controller's storage is 16-byte aligned, so the members before it are padded.)
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
static_assert(kCongestionCouplingMembers >= kQuicMaxPaths, "a coupling holds one controller of every path slot");

class QuicPathControl
{
public:
    QuicPathControl() noexcept;
    ~QuicPathControl() noexcept;
    QuicPathControl(const QuicPathControl&) = delete;
    QuicPathControl& operator=(const QuicPathControl&) = delete;
    QuicPathControl(QuicPathControl&&) = delete;
    QuicPathControl& operator=(QuicPathControl&&) = delete;

    // Builds the path's state for `path` from `config`, discarding any it held: false, building nothing, for a
    // policy or a size make_congestion() refuses. `entropy` seeds the controller's randomised probing. A coupled
    // policy's controllers join `coupling` when given: the connection's, which outlives the control and which at most
    // kQuicMaxPaths controls share, so a join never finds it full.
    [[nodiscard]] bool start(const QuicPathControlConfig& config, const QuicPath& path, u64 entropy,
                             CongestionCoupling* coupling = nullptr) noexcept;
    [[nodiscard]] bool started() const noexcept { return m_controller != nullptr; }

    // The 4-tuple the state was built on. bind() names another and keeps the state: a peer port not yet known, or a
    // NAT rebinding (only the port changed, RFC 9000 section 9.4).
    [[nodiscard]] const QuicPath& path() const noexcept { return m_path; }
    void bind(const QuicPath& path) noexcept { m_path = path; }
    // A validated move to another address: every part starts over (the search from `base_datagram_bytes`, the peer's
    // ceiling kept), learning starts again and the schedule, which was the old link's, ends.
    void restart(const QuicPath& path, u32 base_datagram_bytes, u64 entropy) noexcept;
    // Recovery started over (a Retry, RFC 9002 section 6.3): congestion control, the sampler and the pacer too.
    void reset(u64 now_ns) noexcept;

    [[nodiscard]] const CongestionController& congestion() const noexcept { return *m_controller; }
    [[nodiscard]] const DeliveryRateSampler& delivery() const noexcept { return m_delivery; }
    [[nodiscard]] const QuicEcnValidator& ecn() const noexcept { return m_ecn; }
    [[nodiscard]] const QuicPathMtu& path_mtu() const noexcept { return m_pmtu; }
    // What datagrams on the path take now, which the controller was told.
    [[nodiscard]] u32 datagram_bytes() const noexcept { return m_datagram_bytes; }

    // Replaces the policy live: the new controller starts from the current window, slow start threshold and recovery
    // period, and ECN validation takes its codepoint. False, changing nothing, for one congestion_policy_valid()
    // refuses.
    [[nodiscard]] bool set_congestion(const CongestionPolicy& policy, u64 entropy) noexcept;
    // The peer's max_udp_payload_size, capped: the search's ceiling, kept across a move.
    void limit(u32 ceiling) noexcept;

    // The schedule in force, authored or learned; set_handover() ends learning and set_handover_learning() clears
    // the schedule. False, changing nothing, for one handover_schedule_valid() or handover_learning_valid() refuses.
    [[nodiscard]] bool set_handover(const HandoverSchedule& schedule) noexcept;
    [[nodiscard]] bool set_handover_learning(const HandoverLearning& learning) noexcept;
    [[nodiscard]] const HandoverSchedule& handover() const noexcept { return m_handover; }
    [[nodiscard]] const HandoverLearning& handover_learning() const noexcept { return m_learning; }
    [[nodiscard]] bool covered(u64 now_ns) const noexcept { return handover_covers(m_handover, now_ns); }

    // Sending. The codepoint the next datagram is marked with (RFC 9000 section 13.4.2), `testing_ns` the recovery's
    // persistent congestion period.
    [[nodiscard]] Ecn marking(u64 now_ns, u64 testing_ns) noexcept;
    // When the pacer lets a packet go at the controller's rate for `rtt`.
    [[nodiscard]] u64 release_ns(u64 now_ns, const QuicRtt& rtt) const noexcept;
    // The pacer's timer: the earliest release a held datagram waits for (kNeverNs for none).
    [[nodiscard]] u64 pace_timer_ns() const noexcept { return m_pace_timer_ns; }
    void arm_pace(u64 release_ns) noexcept;
    void disarm_pace() noexcept { m_pace_timer_ns = kNeverNs; }
    // Data waits: the window (`by_window`) or the pacer holds it (RFC 9002 section 7.8).
    void on_limited(bool by_window, u64 now_ns) noexcept { m_controller->on_limited(by_window, now_ns); }
    // Nothing waits while the window and the pacer would let it go: rate samples show the application's rate.
    void on_app_limited(u64 in_flight) noexcept { m_delivery.on_app_limited(in_flight); }
    // A packet of `bytes` in flight leaving with `in_flight` bytes in flight after it: its delivery stamp.
    [[nodiscard]] DeliveryStamp stamp(u64 bytes, u64 in_flight, u64 now_ns) noexcept;
    // `packet` left (recovery recorded it): `controlled` when it is in flight on this path, which the controller and
    // the pacer then count, with `in_flight` bytes in flight and `rtt` the path's estimate.
    void on_sent(QuicSpace space, const QuicSentPacket& packet, bool controlled, u64 in_flight, const QuicRtt& rtt,
                 u64 now_ns) noexcept;
    // The PMTU probe due now (0 for none; `busy`: data waits) and the one that left.
    [[nodiscard]] u32 probe(u64 now_ns, bool busy) noexcept { return m_pmtu.probe(now_ns, busy); }
    void on_probe_sent(u32 bytes) noexcept { m_pmtu.on_probe_sent(bytes); }
    // An ICMP Packet Too Big quoting a packet of `quoted` bytes sent on this path (RFC 8899 section 4.6).
    void on_too_big(u32 quoted, u32 limit, u64 now_ns) noexcept;

    // The recovery's events of this path's packets (QuicRecoveryEvents, after QuicRecovery::on_path()); `received_ns`
    // is when the peer reports it received the packet (kNeverNs for unknown).
    void on_acked(QuicSpace space, const QuicSentPacket& packet, u64 received_ns, u64 now_ns) noexcept;
    [[nodiscard]] QuicPathSignals on_lost(QuicSpace space, const QuicSentPacket& packet, u64 now_ns) noexcept;
    void on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns, u64 now_ns) noexcept;
    void on_discarded(const QuicSentPacket& packet) noexcept { m_ecn.on_gone(packet.ecn); }
    // An ACK frame of this path: on_ack_begin() before the recovery reads it, on_ack_end() after, with its outcome,
    // the path's estimate and bytes in flight, which ends with end_detection().
    void on_ack_begin() noexcept { m_delivery.on_ack_begin(); }
    [[nodiscard]] QuicPathSignals on_ack_end(QuicSpace space, const QuicAck& ack, const QuicAckOutcome& outcome,
                                             const QuicRtt& rtt, u64 in_flight, u64 now_ns) noexcept;
    // A loss detection of this path (an ACK frame's or a timeout's) declared `lost`: persistent congestion reaches the
    // controller, then black hole detection runs and the size found reaches the controller.
    void end_detection(const QuicLoss& lost, u64 now_ns) noexcept;

private:
    // crd-hesap-net's handover phase learner (handover_phase.hpp), which no public header of crd-net includes.
    struct LearnerSlot
    {
        alignas(8) u8 bytes[1488U];
    };

    [[nodiscard]] Ecn codepoint() const noexcept;
    void build(u64 entropy) noexcept;
    void follow_ecn() noexcept;
    // Puts the learner's estimate in force when it changed: true when it locked where no schedule was.
    [[nodiscard]] bool follow_learner() noexcept;
    void apply_size() noexcept;

    QuicPathControlConfig m_config;
    QuicPath m_path;
    CongestionController* m_controller = nullptr; // built in m_controller_slot
    CongestionSlot m_controller_slot;
    CongestionCoupling* m_coupling = nullptr;
    DeliveryRateSampler m_delivery; // every controller's delivery rate samples, stamped on each packet in flight
    Pacer m_pacer;
    u64 m_pace_timer_ns = kNeverNs;
    QuicEcnValidator m_ecn;
    QuicPathMtu m_pmtu;
    u32 m_ceiling = 0U; // the peer's, 0 until known
    u32 m_datagram_bytes = kQuicPmtuFloor;
    bool m_ecn_unavailable = false; // the controller heard that the path carries no ECN
    HandoverSchedule m_handover;
    HandoverLearning m_learning;
    LearnerSlot m_learner;
    u32 m_learner_revision = 0U; // the learner's estimate in force
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
} // namespace crd::net
