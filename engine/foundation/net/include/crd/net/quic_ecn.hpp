#pragma once

// crd-net -- QUIC's ECN validation (NET-PATH; contract: docs/design/network-foundation.md#ecn). Sources: RFC 9000
// section 13.4 and appendix A.4, RFC 9002 section 7.1 and appendix B.7.
//
//   marking     a path's datagrams carry the policy's codepoint (ECT(0), or ECT(1) for L4S) while the path is tested
//               and once it proved to carry ECN. Testing marks the first kQuicEcnTestingPackets packets or for three
//               probe timeouts without backoff, whichever ends first, then nothing until an acknowledgement validates
//               the marks (A.4's "unknown").
//   validation  each ACK frame that raises its space's largest acknowledged checks the peer's counts (13.4.2.1): they
//               are present once marked packets are acknowledged, never fall, never exceed the packets sent with
//               their codepoint, and rise by at least the marked packets newly acknowledged. A frame that does not
//               raise the largest is not checked: it may be an older acknowledgement arriving late. A failure stops
//               marking for the path's life; so does losing every packet marked before validation (a path that drops
//               marked packets).
//   congestion  a CE count that rises on a validated path is a congestion signal (RFC 9002 section 7.1), which the
//               controller's on_congestion takes with the newest acknowledged packet's send time (B.7). One reduction
//               per recovery period bounds what a peer inflating CE can do.
//   paths       one validator per path: a new path starts it again.

#include <crd/core/types.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_recovery.hpp>

namespace crd::net
{
inline constexpr u64 kQuicEcnTestingPackets = 10U; // RFC 9000 section 13.4.2

enum class QuicEcnState : u8
{
    Off,     // the path is never marked: the policy marks nothing, or the socket cannot
    Testing, // marking, not yet validated
    Unknown, // the testing period ended unvalidated: unmarked until an acknowledgement validates it
    Capable, // validated: marked, and CE is a congestion signal
    Failed,  // unmarked for the path's life
};

enum class QuicEcnFailure : u8
{
    None,
    Missing,   // marked packets were acknowledged without ECN counts: bleached, or a peer that reports none
    Decreased, // a count fell
    Exceeded,  // a count above the packets sent with its codepoint, a codepoint never sent among them
    Short,     // the counts rose by less than the marked packets newly acknowledged: re-marked or bleached
    Lost,      // every packet marked before validation was lost or discarded
};

class QuicEcnValidator
{
public:
    // Starts on a new path marking with `codepoint`: Ect0 or Ect1. Anything else never marks (Off).
    void start(Ecn codepoint) noexcept;

    // The codepoint of the datagram written at `now_ns`. The testing period ends once `testing_ns` (three probe
    // timeouts without backoff, RFC 9002 section 7.6.1's period) has passed since the first marked packet.
    [[nodiscard]] Ecn marking(u64 now_ns, u64 testing_ns) noexcept;
    // A packet of `space` left marked `ecn`, as marking() said.
    void on_sent(QuicSpace space, Ecn ecn, u64 now_ns) noexcept;
    // A packet marked `ecn` was declared lost, or its space's keys were discarded: it validates nothing.
    void on_gone(Ecn ecn) noexcept;
    // An ACK frame of `space` and what QuicRecovery::on_ack made of it. True for a congestion signal.
    [[nodiscard]] bool on_ack(QuicSpace space, const QuicAck& ack, const QuicAckOutcome& outcome) noexcept;

    [[nodiscard]] QuicEcnState state() const noexcept { return m_state; }
    [[nodiscard]] QuicEcnFailure failure() const noexcept { return m_failure; }
    // The highest counts the peer reported in `space`.
    [[nodiscard]] const EcnCounts& reported(QuicSpace space) const noexcept;

private:
    struct Space
    {
        EcnCounts sent;     // packets sent marked (ce unused)
        EcnCounts reported; // the peer's highest counts
    };

    void fail(QuicEcnFailure failure) noexcept;
    void end_testing() noexcept;

    Space m_spaces[kQuicSpaces];
    Ecn m_codepoint = Ecn::NotEct;
    QuicEcnState m_state = QuicEcnState::Off;
    QuicEcnFailure m_failure = QuicEcnFailure::None;
    u64 m_testing_from_ns = kNeverNs; // the first marked packet
    u64 m_unvalidated = 0U;           // packets marked before validation
    u64 m_unvalidated_gone = 0U;      // of those, lost or discarded
};

} // namespace crd::net
