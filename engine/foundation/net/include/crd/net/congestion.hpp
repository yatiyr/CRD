#pragma once

// crd-net -- congestion control and pacing (NET-PATH; contract: docs/design/network-foundation.md#congestion).
// Sources: RFC 9002 section 7 and appendix B, RFC 9438 (CUBIC), RFC 9406 (HyStart++), RFC 8985 section 6,
// draft-ietf-ccwg-bbr-07 (BBRv3), draft-briscoe-iccrg-prague-congestion-control-04 (Prague) with RFC 9331 section 4.
//
//   interface   CongestionController is what every controller is to a connection: it sees each packet counted in
//               flight leave and be acknowledged or lost, each ACK frame's end with the RTT estimate, the ECN feedback
//               of a path that carries ECN, persistent congestion, spurious losses and a Retry's reset, and answers
//               with a window in bytes and a pacing rate. The window caps bytes in flight; ACK-only packets and probes
//               are not held by it (RFC 9002 sections 6.2.4 and 7). Each controller reads the connection's
//               DeliveryRateSampler (delivery_rate.hpp), whose stamps every packet carries. NET-LIVE's delay-based
//               controllers plug in here.
//   policy      CongestionPolicy is authored data, versioned by kCongestionInterfaceVersion, chosen per connection
//               and swappable live: make_congestion() builds a controller into a CongestionSlot, seeded from the one
//               it replaces (its window and slow start threshold), so a swap never restarts from the initial window.
//   window      both controllers of this version share RFC 9002's skeleton: slow start under HyStart++ (RFC 9406)
//               with its conservative phase, one reduction per recovery period, persistent congestion to the minimum
//               window, no growth while the window was not what held sending back (section 7.8), and an undo when
//               every loss of the period proves spurious (RFC 9438 section 4.9). NewReno avoids congestion as
//               appendix B does; CUBIC (RFC 9438) by its cubic function, Reno-friendly region and fast convergence,
//               in integer arithmetic so every platform computes the same window (NET-SIM's replays).
//   model       BBRv3 (draft-ietf-ccwg-bbr-07) builds a model of the path instead: the bandwidth as the windowed
//               maximum of delivery rate samples and the round trip as the windowed minimum, sets the pacing rate and
//               window from them through Startup, Drain, ProbeBW's four phases and ProbeRTT, bounds them by loss and
//               ECN signals at two time scales, and undoes a spurious loss episode; integer arithmetic too.
//   scalable    Prague (L4S, RFC 9331) is the window skeleton with a scalable answer to CE: the smoothed fraction of
//               marked packets (alpha, gain 1/16, updated once a virtual round trip) takes the window down by alpha / 2
//               once a virtual round trip, growth counts unmarked bytes alone and, after 500 rounds, as if the round
//               trip were at least 25 ms (reduced RTT dependence). Loss halves the window from where it stood before
//               a mark's reduction. Its packets carry ECT(1) (congestion_codepoint()); the classic ECN AQM detector
//               (crd/hesap/net/classic_ecn.hpp) moves the reduction toward ABE's 0.7 where a bottleneck marks as a
//               classic AQM does, and a path without ECN makes it Reno.
//   pacing      Pacer spreads packets at the controller's rate (RFC 9002 section 7.7: 1.25 window per smoothed RTT
//               for the window controllers, BBR's own gains of its bandwidth, Prague's window per smoothed RTT, doubled
//               in early slow start) with a burst of the initial window (BBR:
//               its send quantum, Prague: 250 us of its rate), in exact integer credit.
//   time        every call takes the time in nanoseconds; nothing reads a clock.
//   threads     a controller and a pacer belong to their connection's thread.

#include <crd/core/types.hpp>
#include <crd/net/delivery_rate.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_recovery.hpp>

namespace crd::net
{
// 2 added BBRv3 and the delivery rate samples; 3 added Prague and the ECN feedback's extent (on_ecn). A policy of
// an earlier version still runs.
inline constexpr u32 kCongestionInterfaceVersion = 3U;
inline constexpr u64 kCongestionMaxWindow = u64{1} << 32U; // bytes: past 100 Gbit/s over 300 ms
inline constexpr u32 kCongestionMaxInitialPackets = 64U;
inline constexpr u32 kCongestionMinPacingGain = 100U; // percent of window per smoothed RTT
inline constexpr u32 kCongestionMaxPacingGain = 400U;
inline constexpr u32 kCongestionAlphaOne = u32{1} << 20U;

enum class CongestionAlgorithm : u8
{
    NewReno, // RFC 9002 section 7 and appendix B
    Cubic,   // RFC 9438
    Bbr,     // draft-ietf-ccwg-bbr-07 (BBRv3); interface version 2
    Prague,  // draft-briscoe-iccrg-prague-congestion-control-04 (L4S, RFC 9331); interface version 3
};

// The codepoint `algorithm`'s packets carry on a path that carries ECN: ECT(1) for a scalable controller (RFC 9331
// section 4.1), ECT(0) for every classic one (RFC 9000 section 13.4.2).
[[nodiscard]] constexpr Ecn congestion_codepoint(CongestionAlgorithm algorithm) noexcept
{
    return (algorithm == CongestionAlgorithm::Prague) ? Ecn::Ect1 : Ecn::Ect0;
}

// What a connection's congestion control is, as authored. BBR reads the version, the initial and minimum windows and
// pacing, which it requires; its gains are the algorithm's, so hystart, fast_convergence and pacing_gain are unused.
// Prague requires pacing too (the draft's section 2.5.1) and paces at its own gains: pacing_gain and fast_convergence
// are unused.
struct CongestionPolicy
{
    u32 version = kCongestionInterfaceVersion;
    CongestionAlgorithm algorithm = CongestionAlgorithm::Cubic;
    // RFC 9002 section 7.2: min(10 * max_datagram, max(14720, 2 * max_datagram)) for 10; at most the cap above.
    u32 initial_packets = 10U;
    u32 minimum_packets = 2U;    // the window's floor in datagrams (section 7.2's kMinimumWindow), at least 2
    bool hystart = true;         // RFC 9406's slow start; false is plain slow start to the first congestion event
    bool fast_convergence = true; // RFC 9438 section 4.7 (CUBIC alone)
    bool pacing = true;          // RFC 9002 section 7.7; false sends a window at once
    u32 pacing_gain = 125U;      // percent; kCongestionMinPacingGain to kCongestionMaxPacingGain
};

// Whether a policy is one this build runs: its version, algorithm and bounds.
[[nodiscard]] bool congestion_policy_valid(const CongestionPolicy& policy) noexcept;

enum class CongestionPhase : u8
{
    SlowStart,
    ConservativeSlowStart, // RFC 9406's CSS
    Recovery,              // a reduction was made; packets sent before it do not grow the window
    Avoidance,
    // BBRv3's states (draft-ietf-ccwg-bbr-07 section 5.1); its Startup reports as SlowStart, and a BBR in loss
    // recovery as Recovery.
    Drain,
    ProbeBwDown,
    ProbeBwCruise,
    ProbeBwRefill,
    ProbeBwUp,
    ProbeRtt,
};

// What a controller holds that another may start from, and what NET-DEBUG shows.
struct CongestionState
{
    u64 window = 0U;
    u64 slow_start_threshold = kCongestionMaxWindow;
    u64 recovery_start_ns = kNeverNs; // when the recovery period began; kNeverNs for none
    u64 reductions = 0U;        // congestion events that reduced the window
    u64 undone = 0U;            // reductions undone as spurious
    u64 bandwidth = 0U;  // bytes per second the model matches its sending to (BBR.bw); 0 for a window controller
    u64 min_rtt_ns = 0U; // the model's windowed minimum round trip (BBR.min_rtt); 0 for none
    u32 alpha = 0U;      // Prague: the smoothed fraction of packets marked CE, in 2^-20 (kCongestionAlphaOne)
    u32 classic = 0U;    // Prague: how classic its answer to CE is, from 0 (scalable) to 2^16 (ABE's)
    CongestionPhase phase = CongestionPhase::SlowStart;
    bool limited = false; // the window or the pacer held sending back this round (RFC 9002 section 7.8)
};

// A packet counted in bytes in flight, as it leaves or settles.
struct CongestionPacket
{
    u64 bytes = 0U;
    u64 number = 0U;
    u64 sent_ns = 0U;
    QuicSpace space = QuicSpace::Initial;
    DeliveryStamp delivery; // the sampler's stamp from when it left
};

class CongestionController
{
public:
    virtual ~CongestionController() = default;
    CongestionController(const CongestionController&) = delete;
    CongestionController& operator=(const CongestionController&) = delete;
    CongestionController(CongestionController&&) = delete;
    CongestionController& operator=(CongestionController&&) = delete;

    [[nodiscard]] virtual CongestionAlgorithm algorithm() const noexcept = 0;
    // `packet` left at `now_ns`; `in_flight` holds it.
    virtual void on_sent(const CongestionPacket& packet, u64 in_flight, u64 now_ns) noexcept = 0;
    // Sending had more to send and the window (`window`) or the pacer held it back.
    virtual void on_limited(bool window, u64 now_ns) noexcept = 0;
    // `packet`, newly acknowledged by the ACK frame being processed.
    virtual void on_acked(const CongestionPacket& packet, u64 now_ns) noexcept = 0;
    // The ACK frame's end, after the sampler's: `rtt` holds its sample when `sampled`, and `in_flight` bytes are in
    // flight. The window grows here by what it acknowledged.
    virtual void on_ack_end(const QuicRtt& rtt, bool sampled, u64 in_flight, u64 now_ns) noexcept = 0;
    // `packet` was declared lost at `now_ns`: a congestion event unless a recovery period already covers it.
    virtual void on_lost(const CongestionPacket& packet, u64 now_ns) noexcept = 0;
    // An ACK frame on a path that proved to carry ECN acknowledged `marked` packets sent ECN-capable, the newest sent
    // at `newest_sent_ns`, and the peer's CE counts rose by `ce` (RFC 9000 section 13.4.2: counts are packets, and a
    // rise may cover packets earlier frames acknowledged). Called before on_ack_end; `ce` 0 is no congestion signal.
    virtual void on_ecn(u64 marked, u64 ce, u64 newest_sent_ns, u64 now_ns) noexcept = 0;
    // The path does not carry ECN: validation failed, or nothing is marked (quic_ecn.hpp). A controller that answers
    // marks behaves as a classic one from then on.
    virtual void on_ecn_unavailable() noexcept = 0;
    // RFC 9002 section 7.6: the window falls to its minimum and nothing of the period is undone.
    virtual void on_persistent_congestion(u64 now_ns) noexcept = 0;
    // `packet`, declared lost at `lost_ns`, was acknowledged after all.
    virtual void on_spurious(const CongestionPacket& packet, u64 lost_ns, u64 now_ns) noexcept = 0;
    // A client accepted a Retry (RFC 9002 section 6.3): the controller starts again.
    virtual void reset(u64 now_ns) noexcept = 0;
    // The path's datagrams now take `bytes` (DPLPMTUD, quic_pmtu.hpp; 1200 to 65535). The minimum and initial
    // windows follow it, a window still at its initial size takes the new one (RFC 9002 section 7.2), and CUBIC
    // starts a new epoch in the new unit.
    virtual void set_max_datagram(u64 bytes) noexcept = 0;

    [[nodiscard]] virtual u64 window() const noexcept = 0;
    // Bytes per second to pace at with `rtt`; 0 sends unpaced.
    [[nodiscard]] virtual u64 pacing_rate(const QuicRtt& rtt) const noexcept = 0;
    // The burst the pacer allows after an idle period.
    [[nodiscard]] virtual u64 burst() const noexcept = 0;
    [[nodiscard]] virtual CongestionState state() const noexcept = 0;

protected:
    CongestionController() = default;
};

// Room for any controller of this build.
inline constexpr usize kCongestionSlotBytes = 768U;
struct CongestionSlot
{
    alignas(16) u8 bytes[kCongestionSlotBytes];
};

// Builds `policy`'s controller in `slot` for datagrams of `max_datagram` bytes, starting from `seed` when given (a
// live swap: its window and slow start threshold carry over, clamped to this policy's bounds). It reads `delivery`,
// the connection's sampler, which outlives it, and BBR may mark it application limited (ProbeRTT); `entropy` seeds
// BBR's randomised probing. Null, building nothing, for a policy congestion_policy_valid() refuses or a datagram size
// under kQuicMinInitialDatagram. The caller destroys the controller (its destructor) before building another in the
// slot.
[[nodiscard]] CongestionController* make_congestion(const CongestionPolicy& policy, u64 max_datagram,
                                                    const CongestionState* seed, CongestionSlot& slot,
                                                    DeliveryRateSampler& delivery, u64 entropy) noexcept;

// Packets spread at a rate: credit accrues at the rate up to a burst, a packet goes while any credit is left and
// may take it below zero, and the next goes once the debt is paid. Credit is kept in byte-nanoseconds per second,
// so no rounding loses rate.
class Pacer
{
public:
    // When a packet may go: `now_ns` while credit is left, else when the debt is paid at `rate` bytes per second.
    // `rate` 0 is unpaced: always `now_ns`.
    [[nodiscard]] u64 release_ns(u64 now_ns, u64 rate, u64 burst) const noexcept;
    // `bytes` left at `now_ns`.
    void on_sent(u64 bytes, u64 now_ns, u64 rate, u64 burst) noexcept;
    // Credit in bytes (negative: debt).
    [[nodiscard]] i64 credit(u64 now_ns, u64 rate, u64 burst) const noexcept;

private:
    [[nodiscard]] i64 credit_scaled(u64 now_ns, u64 rate, u64 burst) const noexcept;

    i64 m_credit = 0; // byte-nanoseconds per second, as of m_stamp_ns
    u64 m_stamp_ns = 0U;
    bool m_started = false; // before the first packet the burst is whole
};

} // namespace crd::net
