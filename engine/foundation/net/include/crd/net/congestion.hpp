#pragma once

// crd-net -- congestion control and pacing (NET-PATH; contract: docs/design/network-foundation.md#congestion).
// Sources: RFC 9002 section 7 and appendix B, RFC 9438 (CUBIC), RFC 9406 (HyStart++), RFC 8985 section 6,
// draft-ietf-ccwg-bbr-07 (BBRv3), draft-briscoe-iccrg-prague-congestion-control-04 (Prague) with RFC 9331 section 4,
// draft-ietf-ccwg-rfc8298bis-screamv2-03 (SCReAM v2) with RFC 6817's base delay, draft-ietf-rmcat-gcc-02 (GCC) as
// libwebrtc's send side runs it, RFC 6356 (LIA, coupled across a multipath connection's paths).
//
//   interface   CongestionController is what every controller is to a connection: it sees each packet counted in
//               flight leave and be acknowledged or lost, each ACK frame's end with the RTT estimate, the ECN feedback
//               of a path that carries ECN, persistent congestion, spurious losses and a Retry's reset, and answers
//               with a window in bytes and a pacing rate. The window caps bytes in flight; ACK-only packets and probes
//               are not held by it (RFC 9002 sections 6.2.4 and 7). Each controller reads the connection's
//               DeliveryRateSampler (delivery_rate.hpp), whose stamps every packet carries.
//   policy      CongestionPolicy is authored data, versioned by kCongestionInterfaceVersion, chosen per connection
//               and swappable live: make_congestion() builds a controller into storage of congestion_bytes() (a
//               CongestionSlot holds any), seeded from the one it replaces (its window and slow start threshold), so a
//               swap never restarts from the initial window.
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
//   real time   SCReAM v2 (draft-ietf-ccwg-rfc8298bis-screamv2-03), the delay-based controller NET-LIVE's calls share:
//               a reference window held near a queue delay target (60 ms, raised beside loss-based flows up to 400
//               ms), the queue delay an RTT sample less RFC 6817's ten-minute base (crd/hesap/net/queue_delay.hpp).
//               Loss, CE and a queue past half the target each reduce it at most once a smoothed RTT; it grows by a
//               datagram a round trip, multiplicatively once congestion is long past, never far past what is in
//               flight. Its packets carry ECT(1) when the policy makes it scalable (an L4S CE then reduces by alpha /
//               2), else ECT(0). It answers with the rate its source should produce (CongestionState::target_rate:
//               the window per smoothed RTT, held within the policy's bounds), paces at 1.5 times that, and lets
//               bytes in flight reach 1.5 to 3 windows so a large frame leaves at once.
//               GCC (draft-ietf-rmcat-gcc-02, the send side libwebrtc runs), the second delay-based controller: the
//               trend of one-way delay between send-time groups of packets (crd/hesap/net/delay_trend.hpp) drives an
//               AIMD rate control (Increase, Decrease, Hold; 8% a second far from the link's measured capacity, half
//               a packet per response time near it, 0.85 of the acknowledged rate on overuse), and a loss controller
//               (more than 10% lost: 1 - p / 2; under 2%: 8% over the last second's least) caps it; the source's
//               target rate is the lower of the two. A packet's peer receive time (received_ns) feeds the trend; a
//               peer that reports none leaves one sample per ACK frame, its largest packet's RTT less the ACK delay.
//               CE counts as a loss (RFC 3168 section 5), so its packets carry ECT(0). Its probing is libwebrtc's:
//               clusters of packets at 3 and 6 times the start rate, then twice each result, whose acknowledged send
//               and receive spacing sets the rate at once (the climb takes seconds, not a minute), filled with padding
//               when the source has too little (probe_bytes()); and a probe back after a large drop, or every 5 s,
//               while the source is application limited.
//   coupled     RFC 6356's Linked Increases Algorithm for the paths of one multipath connection (RFC 10069 section
//               5.3), which may share a bottleneck: NewReno's controllers of a CongestionCoupling grow in congestion
//               avoidance by min(alpha * acked * MSS / cwnd_total, acked * MSS / cwnd_i) (equations 1 and 2) and are
//               NewReno in everything else (slow start, the reduction, recovery). Taken as one divisor, cwnd_total /
//               alpha = (SUM s_j)^2 / MAX (s_j^2 / cwnd_j) with s_j = cwnd_j * rtt_max / rtt_j (section 4.2's
//               scaling), computed exactly in integers at every ACK frame; one member alone is NewReno to the byte.
//   pacing      Pacer spreads packets at the controller's rate (RFC 9002 section 7.7: 1.25 window per smoothed RTT
//               for the window controllers, one window per smoothed RTT for a flight restarted from nothing in flight
//               until its first acknowledgement, BBR's own gains of its bandwidth, Prague's window per smoothed RTT,
//               doubled in early slow start) with a burst of a millisecond of that rate for the window controllers,
//               at least the initial window (section 7.7's limit, which binds a slow or long path) and at most the
//               train cap unless that window is larger: the longest train the path's losses show it takes, from that
//               window to the whole datagrams one host train carries (an IPv4 UDP payload's, 44 of 1472 bytes), as
//               BBR's send quantum is bounded and Linux's segmentation bursts are sized (BBR: its send quantum,
//               Prague: 250 us of its rate), in exact integer credit.
//   time        every call takes the time in nanoseconds; nothing reads a clock.
//   threads     a controller and a pacer belong to their connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/delivery_rate.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_recovery.hpp>

namespace crd::net
{
// 2 added BBRv3 and the delivery rate samples; 3 added Prague and the ECN feedback's extent (on_ecn); 4 added SCReAM
// v2 and the source's target rate; 5 added GCC and the peer's receive time of a packet; 6 added GCC's probing and
// probe_bytes(); 7 added coupling (RFC 6356); 8 added the window controllers' send quantum, a held flight's burst,
// and their restart pacing. A policy of an earlier version still runs (a GCC of version 5 does not probe, a NewReno of
// version 6 is not coupled, a CUBIC of version 7 bursts its initial window and restarts at its pacing gain).
inline constexpr u32 kCongestionInterfaceVersion = 8U;
inline constexpr u64 kCongestionMaxWindow = u64{1} << 32U; // bytes: past 100 Gbit/s over 300 ms
inline constexpr u32 kCongestionMaxInitialPackets = 64U;
inline constexpr u32 kCongestionMinPacingGain = 100U; // percent of window per smoothed RTT
inline constexpr u32 kCongestionMaxPacingGain = 400U;
inline constexpr u32 kCongestionAlphaOne = u32{1} << 20U;
// SCReAM v2's queue delay targets (QDELAY_TARGET_LO's default and QDELAY_TARGET_HI) and its least pacing rate
// (RATE_PACE_MIN, 50 kbit/s, in bytes per second).
inline constexpr u64 kScreamQueueDelayTargetNs = 60000000U;
inline constexpr u64 kScreamQueueDelayTargetMaxNs = 400000000U;
inline constexpr u64 kScreamMinPacingRate = 6250U;

enum class CongestionAlgorithm : u8
{
    NewReno, // RFC 9002 section 7 and appendix B
    Cubic,   // RFC 9438
    Bbr,     // draft-ietf-ccwg-bbr-07 (BBRv3); interface version 2
    Prague,  // draft-briscoe-iccrg-prague-congestion-control-04 (L4S, RFC 9331); interface version 3
    Scream,  // draft-ietf-ccwg-rfc8298bis-screamv2-03 (SCReAM v2, real time); interface version 4
    Gcc,     // draft-ietf-rmcat-gcc-02 (Google Congestion Control, real time); interface version 5
};

// What a connection's congestion control is, as authored. BBR reads the version, the initial and minimum windows and
// pacing, which it requires; its gains are the algorithm's, so hystart, fast_convergence and pacing_gain are unused.
// Prague requires pacing too (the draft's section 2.5.1) and paces at its own gains: pacing_gain and fast_convergence
// are unused. SCReAM v2 requires pacing (its section 4.3.2) and reads the initial and minimum windows (its
// MIN_REF_WND of 3000 bytes is the floor) and the fields after pacing_gain, which nothing else reads. GCC requires
// pacing too (at its own factor) and reads the initial and minimum windows and the target rate's bounds alone.
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
    // SCReAM v2 and GCC: the source's target rate in bytes per second is held to [target_rate_min, target_rate_max]
    // (TARGET_BITRATE_MIN and _MAX over 8; a maximum of 0 is none; a SCReAM source at its maximum relaxes pacing);
    // queue_delay_target_ns is QDELAY_TARGET_LO, 1 ms to kScreamQueueDelayTargetMaxNs.
    u64 target_rate_min = 0U;
    u64 target_rate_max = 0U;
    u64 queue_delay_target_ns = kScreamQueueDelayTargetNs;
    bool scalable_ecn = false;       // IS_L4S: ECT(1), and CE answered by alpha / 2 rather than BETA_ECN's 0.8
    bool reduce_jitter = false;      // REDUCE_JITTER (section 4.2.1.4): growth and slack follow the delay's variation
    bool compete = true;             // section 4.5.4: the delay target rises beside loss-based flows
    bool tolerate_link_loss = false; // section 4.5.2: a loss reduces only past a 1% loss rate or a rising queue
    // GCC's probing (version 6), up to target_rate_max or, with none, 5 Mbit/s: `probe` sends the start's probes at 3
    // and 6 times the start rate, probes further at twice each result reaching 70% of its probe, and probes back to
    // 85% of the rate before a large drop while the source is application limited (libwebrtc's defaults);
    // `probe_alr` adds a probe at twice the estimate every 5 s while it is (periodic ALR probing, which libwebrtc's
    // screen sharing asks for); `probe_initial` repeats a probe at 3 times the estimate each second of the first 5 s
    // (repeated initial probing), its probes 100 ms long rather than 15.
    bool probe = true;
    bool probe_alr = false;
    bool probe_initial = false;
    // Version 7: NewReno coupled with the other paths of its CongestionCoupling (RFC 6356's LIA), for paths that may
    // share a bottleneck; a path that never shares one keeps its own window (false). NewReno alone: RFC 6356 extends
    // Reno's congestion avoidance, and defines no coupled CUBIC, BBR, Prague or delay-based controller.
    bool coupled = false;
};

// RFC 9002 section 7.2's initial window, min(10 * max_datagram, max(14720, 2 * max_datagram)) for 10 initial packets,
// for `policy`'s initial packets of `max_datagram` bytes, at least its minimum window. SCReAM v2 starts its reference
// window there too, at least MIN_REF_WND.
[[nodiscard]] constexpr u64 congestion_initial_window(const CongestionPolicy& policy, u64 max_datagram) noexcept
{
    const u64 packets = policy.initial_packets;
    const u64 per_packet = 1472U; // 14720 / 10
    const u64 cap = (packets * per_packet > 2U * max_datagram) ? packets * per_packet : 2U * max_datagram;
    const u64 initial = (packets * max_datagram < cap) ? packets * max_datagram : cap;
    const u64 minimum = policy.minimum_packets * max_datagram;
    return (initial > minimum) ? initial : minimum;
}

// A burst of more than the initial window bounded by a flight that ran out of data with room in the window (what the
// application wrote, the peer's flow control or the send buffer held it): at most half that flight, so two trains are
// in it and the acknowledgement of the first comes back while the second is on its way. Linux's TCP applies the same
// reasoning to its congestion window (tcp_cwnd_test sends at most half of it at once, "at least 2 GSO packets in
// flight"); this is that rule for a flight something else bounds. Without it a 64 KiB stream window left in two
// trains, the receiver read both in one turn and acknowledged them once, and each end waited for the other (the QUIC
// board, 2026-11-24). Never below the initial window (RFC 9002 section 7.7) and never above `burst`.
[[nodiscard]] constexpr u64 congestion_flight_burst(u64 burst, u64 initial, u64 flight) noexcept
{
    if (burst <= initial)
    {
        return burst;
    }
    const u64 half = flight / 2U;
    if (half >= burst)
    {
        return burst;
    }
    return (half > initial) ? half : initial;
}

// The codepoint `policy`'s packets carry on a path that carries ECN: ECT(1) for a scalable controller (RFC 9331
// section 4.1: Prague, and SCReAM v2 in L4S mode), ECT(0) for every classic one (RFC 9000 section 13.4.2).
[[nodiscard]] constexpr Ecn congestion_codepoint(const CongestionPolicy& policy) noexcept
{
    const bool scalable = policy.algorithm == CongestionAlgorithm::Prague ||
                          (policy.algorithm == CongestionAlgorithm::Scream && policy.scalable_ecn);
    return scalable ? Ecn::Ect1 : Ecn::Ect0;
}

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
    // Bytes per second the model matches its sending to (BBR.bw), GCC's delay-based estimate (A_hat); 0 for a window
    // controller.
    u64 bandwidth = 0U;
    u64 min_rtt_ns = 0U; // the model's windowed minimum round trip (BBR.min_rtt); 0 for none
    u32 alpha = 0U;      // Prague: the smoothed fraction of packets marked CE, in 2^-20 (kCongestionAlphaOne)
    u32 classic = 0U;    // Prague: how classic its answer to CE is, from 0 (scalable) to 2^16 (ABE's)
    // SCReAM v2 and GCC: the rate in bytes per second its source should produce (0 for a bulk controller); SCReAM's
    // averaged queue delay and the target it holds that delay to. SCReAM's window above is the reference window, and
    // GCC's the window its rate allows; window() is what may be in flight.
    u64 target_rate = 0U;
    u64 queue_delay_ns = 0U;
    u64 queue_delay_target_ns = 0U;
    // NewReno and CUBIC: the longest train their send quantum may be, which the path's losses teach (from the initial
    // window to one host train of whole datagrams); 0 for any other controller and before version 8.
    u64 train_cap = 0U;
    CongestionPhase phase = CongestionPhase::SlowStart;
    bool limited = false; // the window or the pacer held sending back this round (RFC 9002 section 7.8)
};

// A packet counted in bytes in flight, as it leaves or settles.
struct CongestionPacket
{
    u64 bytes = 0U;
    u64 number = 0U;
    u64 sent_ns = 0U;
    u64 train = 0U; // bytes of its train before it (Pacer::train)
    QuicSpace space = QuicSpace::Initial;
    DeliveryStamp delivery; // the sampler's stamp from when it left
    // When the peer received it, on the peer's clock (a QUIC receive timestamp); kNeverNs when it did not say, or
    // when a scheduled handover may have delayed it (handover.hpp). Only differences between two such times mean
    // anything.
    u64 received_ns = kNeverNs;
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
    // `packet`, newly acknowledged by the ACK frame being processed (with its peer receive time when reported).
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
    // The most the pacer lets leave at once with `rtt`: its credit's cap, which an idle period refills.
    [[nodiscard]] virtual u64 burst(const QuicRtt& rtt) const noexcept = 0;
    // Bytes a bandwidth probe still wants sent at the pacing rate: a sender whose application has too little fills
    // them with ack-eliciting padding, held by the window and the pacer as any packet (QuicConnection pads its 1-RTT
    // packets to the datagram). 0 for none, and always for every controller but GCC.
    [[nodiscard]] virtual u64 probe_bytes() const noexcept { return 0U; }
    [[nodiscard]] virtual CongestionState state() const noexcept = 0;

protected:
    CongestionController() = default;
};

// Room for any controller of this build, and the alignment every controller's storage takes.
inline constexpr usize kCongestionSlotBytes = 2240U; // GCC, its delay trend's window and probes, is the largest
inline constexpr usize kCongestionAlign = 16U;
struct CongestionSlot
{
    alignas(kCongestionAlign) u8 bytes[kCongestionSlotBytes];
};

// The bytes `policy`'s controller takes (its class's size, at most kCongestionSlotBytes): a CUBIC or BBR controller
// needs about a quarter of a slot. 0 for a policy congestion_policy_valid() refuses.
[[nodiscard]] usize congestion_bytes(const CongestionPolicy& policy) noexcept;

class CongestionCoupling;

// Builds `policy`'s controller in `slot` for datagrams of `max_datagram` bytes, starting from `seed` when given (a
// live swap: its window and slow start threshold carry over, clamped to this policy's bounds). It reads `delivery`,
// the connection's sampler, which outlives it, and BBR may mark it application limited (ProbeRTT); `entropy` seeds
// BBR's randomised probing. A coupled policy's controller joins `coupling` when given (a lone path without one) and
// leaves it in its destructor. Null, building nothing, for a policy congestion_policy_valid() refuses, a datagram size
// under kQuicMinInitialDatagram, `storage` under congestion_bytes() or not aligned to kCongestionAlign, or a full
// coupling. The caller destroys the controller (its destructor) before building another in the storage.
[[nodiscard]] CongestionController* make_congestion(const CongestionPolicy& policy, u64 max_datagram,
                                                    const CongestionState* seed, cont::Span<u8> storage,
                                                    DeliveryRateSampler& delivery, u64 entropy,
                                                    CongestionCoupling* coupling = nullptr) noexcept;
// The same in a slot, which holds any controller.
[[nodiscard]] inline CongestionController* make_congestion(const CongestionPolicy& policy, u64 max_datagram,
                                                           const CongestionState* seed, CongestionSlot& slot,
                                                           DeliveryRateSampler& delivery, u64 entropy,
                                                           CongestionCoupling* coupling = nullptr) noexcept
{
    return make_congestion(policy, max_datagram, seed, cont::Span<u8>(slot.bytes, kCongestionSlotBytes), delivery,
                           entropy, coupling);
}

// The paths of one connection whose coupled controllers grow together (RFC 6356). A member counts with min(cwnd_i,
// flight_size_i) (section 3: an application-limited or idle path counts what it has in flight; after a reduction the
// window is ssthresh already) once it has an RTT sample; one with neither counts nothing. The asking member counts
// with its own window and smoothed RTT. An RTT under 2^-kCouplingRttRatioBits of the largest counts as that fraction
// of it, which bounds the integers (an RTT that much shorter dominates alpha anyway). It outlives its members.
inline constexpr usize kCongestionCouplingMembers = 4U; // kQuicMaxPaths: one per path slot
inline constexpr u32 kCouplingRttRatioBits = 14U;
inline constexpr usize kCongestionNoMember = ~usize{0};

class CongestionCoupling
{
public:
    CongestionCoupling() noexcept = default;
    ~CongestionCoupling() = default;
    CongestionCoupling(const CongestionCoupling&) = delete;
    CongestionCoupling& operator=(const CongestionCoupling&) = delete;
    CongestionCoupling(CongestionCoupling&&) = delete;
    CongestionCoupling& operator=(CongestionCoupling&&) = delete;

    // A controller joins and leaves (make_congestion and its destructor): its member index, kCongestionNoMember when
    // every one is taken.
    [[nodiscard]] usize join(const CongestionController& member) noexcept;
    void leave(usize member) noexcept;
    // What `member` has in flight (at each packet sent and ACK frame's end) and its smoothed RTT (0 until sampled).
    void report(usize member, u64 flight, u64 rtt_ns) noexcept;
    [[nodiscard]] usize members() const noexcept;

    // RFC 6356's cwnd_total / alpha for `member` growing from `window` at `rtt_ns`: the window its congestion
    // avoidance divides acked * MSS by, at least `window` (equation 1's minimum).
    [[nodiscard]] u64 divisor(usize member, u64 window, u64 rtt_ns) const noexcept;

private:
    struct Member
    {
        const CongestionController* controller = nullptr;
        u64 flight = 0U;
        u64 rtt_ns = 0U;
    };
    Member m_members[kCongestionCouplingMembers];
};

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
    // Bytes of the train a packet leaving at `now_ns` joins: what left paced at that same instant before it (one
    // host turn's sends, which reach the path back to back); 0 for the first.
    [[nodiscard]] u64 train(u64 now_ns) const noexcept { return (m_started && now_ns == m_stamp_ns) ? m_train : 0U; }

private:
    [[nodiscard]] i64 credit_scaled(u64 now_ns, u64 rate, u64 burst) const noexcept;

    i64 m_credit = 0; // byte-nanoseconds per second, as of m_stamp_ns
    u64 m_stamp_ns = 0U;
    u64 m_train = 0U;       // bytes that left at m_stamp_ns
    bool m_started = false; // before the first packet the burst is whole
};

} // namespace crd::net
