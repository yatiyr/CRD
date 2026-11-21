// Handover phase learning (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net): where in a known
// period a link's scheduled handovers fall, learned from the disturbances they leave on a connection, for a sender
// with no clock synchronised to the link's (NET-PATH's handover-aware congestion response, design S76).
//
//   folding     epoch folding at a known period (Leahy et al., ApJ 266:160, 1983): each period is cut into kBins
//               bins, and per period a bin is observed (a packet sent in it was acknowledged or lost) and disturbed
//               (one of those was lost, or its RTT rose above the previous sample's by at least
//               max(kMinStepUs, kStepJitters J), J the RFC 3550 jitter of successive RTTs: for a busy sender's few
//               hundred samples a second, past the tail of their noise). Counting periods, not packets, makes a
//               burst of losses one event and a busy sender no louder than a sparse one.
//   test        a window of kWindow bins (an event's, and the next for one that straddles them) wins when the most
//               periods disturbed it. It locks when at
//               least kMinLock periods did, at least half of those that observed it, and an exact binomial test
//               refuses the background rate (the other bins' disturbed share, with Laplace's rule of succession) at
//               kLockAlpha after a Bonferroni correction for the kBins windows tried. A locked phase keeps while
//               kMinKeep periods and a third of the observed ones disturb a window overlapping it, so a clock skew
//               moves it period by period, and unlocks when they stop.
//   phase       each disturbing period's first disturbance from the window's onset bin (its earliest bin disturbed
//               in a third of those periods, so background events before the onset do not pull it early), as a line
//               through the periods by the Theil-Sen estimator (Sen, JASA 63:1379, 1968), read at the period being
//               sent: a skewed clock's drift is followed without lag. Times are send times: the phase is where packets
//               sent cross the event, the link's distance from this sender included.
//   bounds      kHistory periods, integer arithmetic only (Q62 fixed point for the test, rounded towards refusing),
//               so every platform learns the same phase; nothing allocates; any time sequence, backwards or near
//               the wrap, leaves a valid state.
//
// Cerid's own design (S76): the published LEO controllers read UTC (StarQUIC) or detect each reconfiguration as it
// happens (LeoCC); none learns the timetable's phase.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/net/jitter.hpp>

namespace crd::hesap::net
{

struct HandoverPhase
{
    bool locked = false;
    crd::u64 phase_ns = 0U; // below the period
    crd::u32 support = 0U;  // the periods that disturbed the window
    crd::u32 periods = 0U;  // the periods that observed it

    [[nodiscard]] constexpr bool operator==(const HandoverPhase&) const noexcept = default;
};

class HandoverPhaseLearner
{
public:
    static constexpr crd::u32 kBins = 64U;
    static constexpr crd::u32 kHistory = 16U;
    static constexpr crd::u32 kMinLock = 4U;
    static constexpr crd::u32 kMinKeep = 3U;
    static constexpr crd::u32 kWindow = 2U;
    static constexpr crd::u64 kMinStepUs = 2000U;
    static constexpr crd::u64 kStepJitters = 8U;
    static constexpr crd::u64 kOneQ62 = crd::u64{1} << 62U;
    static constexpr crd::u64 kLockAlphaQ62 = kOneQ62 / (crd::u64{1000} * kBins); // 10^-3 over the windows
    static constexpr crd::u64 kMinPeriodNs = crd::u64{1000000000};
    static constexpr crd::u64 kMaxPeriodNs = crd::u64{3600000000000};

    // Learns at `period_ns` (kMinPeriodNs to kMaxPeriodNs), forgetting everything; false, changing nothing, outside.
    [[nodiscard]] bool start(crd::u64 period_ns) noexcept;
    void stop() noexcept;
    // Forgets what was learned, keeping the period: a new link.
    void restart() noexcept;

    // A packet sent at `send_ns` acknowledged with an RTT sample of `rtt_ns`, or lost.
    void on_rtt(crd::u64 send_ns, crd::u64 rtt_ns) noexcept;
    void on_loss(crd::u64 send_ns) noexcept;

    [[nodiscard]] bool learning() const noexcept { return m_period_ns != 0U; }
    [[nodiscard]] crd::u64 period_ns() const noexcept { return m_period_ns; }
    [[nodiscard]] const HandoverPhase& phase() const noexcept { return m_phase; }
    // Changes with phase(): a reader compares it to follow the estimate.
    [[nodiscard]] crd::u32 revision() const noexcept { return m_revision; }

    // The exact binomial tail P(X >= hits) of `periods` trials at rate `q_q62`, rounded up, in Q62 (at most one).
    [[nodiscard]] static crd::u64 binomial_tail_q62(crd::u32 periods, crd::u32 hits, crd::u64 q_q62) noexcept;

private:
    struct Period
    {
        crd::u64 index = ~crd::u64{0}; // the period's number on the clock; ~0 is none
        crd::u64 observed = 0U;        // a bit a bin
        crd::u64 disturbed = 0U;
        crd::u8 first[kBins] = {}; // each disturbed bin's first disturbance, in 256ths of the bin
    };

    struct Window
    {
        crd::u32 start = 0U;
        crd::u32 hits = 0U;
        crd::u32 periods = 0U;
    };

    void record(crd::u64 send_ns, bool disturbed) noexcept;
    void evaluate() noexcept;
    [[nodiscard]] bool completed(const Period& p) const noexcept { return p.index < m_newest; }
    [[nodiscard]] crd::u64 window_mask(crd::u32 start, crd::u32 bins) const noexcept;
    [[nodiscard]] Window window(crd::u32 start) const noexcept;
    [[nodiscard]] crd::u64 window_q62(crd::u32 start) const noexcept;
    [[nodiscard]] crd::u64 onset_ns(const Window& w) const noexcept;

    Period m_history[kHistory];
    crd::u64 m_period_ns = 0U;
    crd::u64 m_newest = ~crd::u64{0}; // the newest period's number; ~0 before any
    crd::u64 m_previous_rtt_us = 0U;
    InterarrivalJitter m_jitter;
    HandoverPhase m_phase;
    crd::u32 m_lock_start = 0U; // the locked window's first bin
    crd::u32 m_revision = 0U;
    bool m_has_rtt = false;
};

} // namespace crd::hesap::net
