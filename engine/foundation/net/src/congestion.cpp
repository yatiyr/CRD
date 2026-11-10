// crd-net -- congestion control and pacing (NET-PATH; contract: docs/design/network-foundation.md#congestion, #bbr
// and #prague).
#include <crd/net/congestion.hpp>

#include <crd/core/wide_multiply.hpp>
#include <crd/hesap/net/classic_ecn.hpp>
#include <crd/hesap/net/rate.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <new>

namespace crd::net
{
namespace
{
using hesap::net::ClassicEcnDetector;
using hesap::net::WindowedMax;

constexpr u64 kMaxDatagram = 65535U;
// RFC 9002 section 7.2: min(10 * max_datagram, max(14720, 2 * max_datagram)), here per packet of the initial window.
constexpr u64 kInitialBytesPerPacket = 1472U;
// RFC 9406 section 4.3.
constexpr u64 kHystartMinRttThresholdNs = 4000000U;
constexpr u64 kHystartMaxRttThresholdNs = 16000000U;
constexpr u64 kHystartMinRttDivisor = 8U;
constexpr u64 kHystartRttSamples = 8U;
constexpr u64 kHystartCssGrowthDivisor = 4U;
constexpr u32 kHystartCssRounds = 5U;
constexpr u64 kHystartUnpacedBurstPackets = 8U; // L for a sender that does not pace
// RFC 9438 section 5: C = 0.4 and beta = 0.7, kept as fractions; time in 2^-10 s steps.
constexpr u64 kCubicCNumerator = 2U;
constexpr u64 kCubicCDenominator = 5U;
constexpr u64 kCubicBetaNumerator = 7U;
constexpr u64 kCubicBetaDenominator = 10U;
constexpr u64 kCubicConvergenceNumerator = 17U; // (1 + beta) / 2
constexpr u64 kCubicConvergenceDenominator = 20U;
constexpr u64 kCubicAlphaNumerator = 9U; // 3 (1 - beta) / (1 + beta)
constexpr u64 kCubicAlphaDenominator = 17U;
constexpr u64 kCubicTimeShift = 10U;
constexpr u64 kCubicMaxOffset = (u64{1} << 21U) - 1U; // its cube stays below 2^63
constexpr u64 kPercent = 100U;

// The pacing rate's product, window * gain * 10^7, and K's cube, (W_max - window) * 2^30 * 5 / 2, fit in 64 bits.
static_assert(kCongestionMaxWindow * kCongestionMaxPacingGain <= ~u64{0} / (kNanosPerSecond / kPercent));
constexpr u64 kCubicKScale = (kCubicCDenominator << (3U * kCubicTimeShift)) / kCubicCNumerator;
static_assert(kCubicKScale * kCubicCNumerator == (kCubicCDenominator << (3U * kCubicTimeShift)));
static_assert(kCongestionMaxWindow <= ~u64{0} / kCubicKScale);

[[nodiscard]] constexpr u64 min_u64(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] constexpr u64 max_u64(u64 a, u64 b) noexcept
{
    return (a > b) ? a : b;
}

// The integer cube root, rounded down (Hacker's Delight, figure 11-5).
[[nodiscard]] constexpr u64 cube_root(u64 x) noexcept
{
    u64 y = 0U;
    u64 y2 = 0U;
    for (int s = 63; s >= 0; s -= 3)
    {
        y2 *= 4U;
        y *= 2U;
        const u64 term = 3U * (y2 + y) + 1U;
        const u64 b = term << static_cast<unsigned>(s);
        if (x >= b && (b >> static_cast<unsigned>(s)) == term)
        {
            x -= b;
            y2 += 2U * y + 1U;
            y += 1U;
        }
    }
    return y;
}

// The window skeleton the window controllers share (RFC 9002 appendix B, RFC 9406, RFC 9438 section 4.9's undo); a
// controller supplies congestion avoidance, the reduction and what an undo restores of its own.
class WindowController : public CongestionController
{
public:
    WindowController(const CongestionPolicy& policy, u64 max_datagram) noexcept : m_policy(policy)
    {
        size_windows(max_datagram);
        restart();
    }

    void seed(const CongestionState& from) noexcept
    {
        m_window = min_u64(max_u64(from.window, m_minimum), kCongestionMaxWindow);
        m_threshold = max_u64(from.slow_start_threshold, m_minimum);
        m_recovery_start_ns = from.recovery_start_ns;
        m_reductions = from.reductions;
        m_undone = from.undone;
        m_limited = from.limited;
        m_phase = (from.phase == CongestionPhase::Recovery) ? CongestionPhase::Recovery : grown_phase();
    }

    void on_sent(const CongestionPacket& packet, u64 in_flight, u64 now_ns) noexcept override
    {
        if (packet.space == QuicSpace::Application)
        {
            m_next_number = max_u64(m_next_number, packet.number + 1U);
            if (!m_rounds)
            {
                m_rounds = true;
                m_window_end = packet.number;
            }
        }
        if (in_flight + m_datagram > m_window)
        {
            m_limited = true;
        }
        sent(packet, in_flight, now_ns);
    }

    void on_limited(bool /*window*/, u64 /*now_ns*/) noexcept override
    {
        m_limited = true; // section 7.8: the window or the pacer
    }

    void on_acked(const CongestionPacket& packet, u64 /*now_ns*/) noexcept override
    {
        if (packet.space == QuicSpace::Application && m_rounds && packet.number >= m_window_end)
        {
            m_round_ended = true;
        }
        if (in_recovery(packet.sent_ns))
        {
            return;
        }
        if (m_phase == CongestionPhase::Recovery)
        {
            m_phase = grown_phase(); // a packet sent after the reduction arrived: the period ends
        }
        m_acked += packet.bytes;
    }

    void on_ack_end(const QuicRtt& rtt, bool sampled, u64 /*in_flight*/, u64 now_ns) noexcept override
    {
        if (sampled && hystart_running())
        {
            m_round_min_ns = min_u64(m_round_min_ns, rtt.latest_ns);
            ++m_samples;
        }
        if (m_acked != 0U && (m_limited || m_limited_before))
        {
            grow(min_u64(m_acked, m_window), rtt, now_ns);
        }
        m_acked = 0U;
        if (m_round_ended)
        {
            m_round_ended = false;
            m_window_end = m_next_number;
            m_last_round_min_ns = m_round_min_ns;
            m_round_min_ns = kNeverNs;
            m_samples = 0U;
            m_limited_before = m_limited;
            m_limited = false;
            if (m_phase == CongestionPhase::ConservativeSlowStart && ++m_css_rounds >= kHystartCssRounds)
            {
                m_threshold = m_window; // RFC 9406: CSS_ROUNDS rounds in CSS end slow start
                m_phase = CongestionPhase::Avoidance;
            }
            rounded(rtt, now_ns);
        }
    }

    void on_lost(const CongestionPacket& packet, u64 now_ns) noexcept override
    {
        if (in_recovery(packet.sent_ns))
        {
            m_episode_lost += packet.bytes;
            return;
        }
        reduce_window(now_ns, true);
        m_episode_lost = packet.bytes;
    }

    // RFC 9002 appendix B.7: a rise of CE is a congestion event, one per recovery period.
    void on_ecn(u64 /*marked*/, u64 ce, u64 newest_sent_ns, u64 now_ns) noexcept override
    {
        if (ce == 0U)
        {
            return;
        }
        if (!in_recovery(newest_sent_ns))
        {
            reduce_window(now_ns, false);
        }
        m_undoable = false; // a mark is no spurious loss
    }

    void on_ecn_unavailable() noexcept override {}

    void on_persistent_congestion(u64 /*now_ns*/) noexcept override
    {
        m_window = m_minimum;
        m_recovery_start_ns = kNeverNs; // RFC 9002 appendix B.8: the recovery period ends
        m_undoable = false;
        m_phase = grown_phase();
        collapse();
    }

    void on_spurious(const CongestionPacket& packet, u64 lost_ns, u64 /*now_ns*/) noexcept override
    {
        if (!m_undoable || m_recovery_start_ns == kNeverNs || lost_ns < m_recovery_start_ns)
        {
            return; // a loss of an earlier period, which a later reduction already answered
        }
        m_episode_spurious += packet.bytes;
        if (m_episode_spurious < m_episode_lost)
        {
            return;
        }
        // Every loss of the period was acknowledged after all: the reduction is undone (RFC 9438 section 4.9).
        m_window = max_u64(m_window, m_prior.window);
        m_threshold = m_prior.slow_start_threshold;
        m_recovery_start_ns = m_prior.recovery_start_ns;
        m_phase = (m_prior.phase == CongestionPhase::Recovery) ? CongestionPhase::Recovery : grown_phase();
        m_undoable = false;
        ++m_undone;
        restore();
    }

    void reset(u64 /*now_ns*/) noexcept override
    {
        restart();
        start_over();
    }

    void set_max_datagram(u64 bytes) noexcept override
    {
        const bool initial = m_window == m_initial;
        size_windows(min_u64(max_u64(bytes, kQuicMinInitialDatagram), kMaxDatagram));
        m_window = initial ? m_initial : max_u64(m_window, m_minimum);
        m_threshold = max_u64(m_threshold, m_minimum);
        resized();
    }

    [[nodiscard]] u64 window() const noexcept override { return m_window; }

    [[nodiscard]] u64 pacing_rate(const QuicRtt& rtt) const noexcept override
    {
        if (!m_policy.pacing || rtt.smoothed_ns == 0U)
        {
            return 0U;
        }
        return m_window * m_policy.pacing_gain * (kNanosPerSecond / kPercent) / rtt.smoothed_ns;
    }

    [[nodiscard]] u64 burst() const noexcept override { return m_initial; }

    [[nodiscard]] CongestionState state() const noexcept override
    {
        CongestionState out;
        out.window = m_window;
        out.slow_start_threshold = m_threshold;
        out.recovery_start_ns = m_recovery_start_ns;
        out.reductions = m_reductions;
        out.undone = m_undone;
        out.phase = m_phase;
        out.limited = m_limited || m_limited_before;
        return out;
    }

protected:
    // Congestion avoidance by `acked` bytes.
    virtual void avoid(u64 acked, const QuicRtt& rtt, u64 now_ns) noexcept = 0;
    // The reduction at the congestion event of `now_ns`: the window and threshold from m_window; what it changes of
    // its own is saved for restore().
    virtual void reduce(u64 now_ns) noexcept = 0;
    virtual void restore() noexcept = 0;
    // The window fell to its minimum.
    virtual void collapse() noexcept = 0;
    // The controller starts over (a Retry): nothing of its own is kept.
    virtual void start_over() noexcept = 0;
    // A packet counted in flight left (CUBIC times its idle periods by it).
    virtual void sent(const CongestionPacket& /*packet*/, u64 /*in_flight*/, u64 /*now_ns*/) noexcept
    {
    }
    // m_datagram changed.
    virtual void resized() noexcept {}
    // A round trip ended with the ACK frame of `now_ns`.
    virtual void rounded(const QuicRtt& /*rtt*/, u64 /*now_ns*/) noexcept {}

    void add_window(u64 bytes) noexcept { m_window = min_u64(m_window + bytes, kCongestionMaxWindow); }

    // A reduction to `window` by ECN marks alone that starts no recovery period, so growth goes on (Prague's CWR):
    // counted, and the period's losses are no longer undone.
    void reduce_to(u64 window) noexcept
    {
        m_window = min_u64(max_u64(window, m_minimum), kCongestionMaxWindow);
        m_threshold = m_window;
        m_phase = (m_phase == CongestionPhase::Recovery) ? m_phase : CongestionPhase::Avoidance;
        m_undoable = false;
        ++m_reductions;
    }

    void no_undo() noexcept { m_undoable = false; }

    CongestionPolicy m_policy;
    u64 m_datagram = 0U;
    u64 m_minimum = 0U;
    u64 m_window = 0U;
    u64 m_threshold = kCongestionMaxWindow;

private:
    [[nodiscard]] bool in_recovery(u64 sent_ns) const noexcept
    {
        return m_recovery_start_ns != kNeverNs && sent_ns <= m_recovery_start_ns;
    }

    [[nodiscard]] CongestionPhase grown_phase() const noexcept
    {
        if (m_window >= m_threshold)
        {
            return CongestionPhase::Avoidance;
        }
        return (m_phase == CongestionPhase::ConservativeSlowStart) ? m_phase : CongestionPhase::SlowStart;
    }

    // RFC 9406 section 4.3: only the first slow start, before any reduction set the threshold.
    [[nodiscard]] bool hystart_running() const noexcept
    {
        return m_policy.hystart && m_threshold == kCongestionMaxWindow &&
               (m_phase == CongestionPhase::SlowStart || m_phase == CongestionPhase::ConservativeSlowStart);
    }

    // RFC 9002 section 7.2's windows for datagrams of `max_datagram` bytes.
    void size_windows(u64 max_datagram) noexcept
    {
        const u64 packets = m_policy.initial_packets;
        m_datagram = max_datagram;
        m_initial = min_u64(packets * max_datagram, max_u64(packets * kInitialBytesPerPacket, 2U * max_datagram));
        m_minimum = m_policy.minimum_packets * max_datagram;
        m_initial = max_u64(m_initial, m_minimum);
    }

    void restart() noexcept
    {
        m_window = m_initial;
        m_threshold = kCongestionMaxWindow;
        m_recovery_start_ns = kNeverNs;
        m_phase = CongestionPhase::SlowStart;
        m_acked = 0U;
        m_round_min_ns = kNeverNs;
        m_last_round_min_ns = kNeverNs;
        m_css_baseline_ns = kNeverNs;
        m_samples = 0U;
        m_css_rounds = 0U;
        m_css_carry = 0U;
        m_undoable = false;
    }

    void reduce_window(u64 now_ns, bool undoable) noexcept
    {
        m_prior = state();
        m_recovery_start_ns = now_ns;
        reduce(now_ns);
        m_window = min_u64(max_u64(m_window, m_minimum), kCongestionMaxWindow);
        m_threshold = max_u64(m_threshold, m_minimum);
        m_phase = CongestionPhase::Recovery;
        m_undoable = undoable;
        m_episode_lost = 0U;
        m_episode_spurious = 0U;
        ++m_reductions;
    }

    void grow(u64 acked, const QuicRtt& rtt, u64 now_ns) noexcept
    {
        if (m_phase == CongestionPhase::Recovery)
        {
            return;
        }
        if (m_window >= m_threshold)
        {
            m_phase = CongestionPhase::Avoidance;
            avoid(acked, rtt, now_ns);
            return;
        }
        const u64 limit = m_policy.pacing ? acked : min_u64(acked, kHystartUnpacedBurstPackets * m_datagram);
        if (!hystart_running())
        {
            add_window(acked); // RFC 9002 section 7.3.1: every acknowledged byte
            return;
        }
        const bool measured = m_samples >= kHystartRttSamples && m_round_min_ns != kNeverNs;
        if (m_phase == CongestionPhase::SlowStart)
        {
            if (measured && m_last_round_min_ns != kNeverNs)
            {
                const u64 threshold = min_u64(max_u64(m_last_round_min_ns / kHystartMinRttDivisor,
                                                      kHystartMinRttThresholdNs),
                                              kHystartMaxRttThresholdNs);
                if (m_round_min_ns >= m_last_round_min_ns + threshold)
                {
                    m_css_baseline_ns = m_round_min_ns;
                    m_css_rounds = 0U;
                    m_css_carry = 0U;
                    m_phase = CongestionPhase::ConservativeSlowStart;
                }
            }
        }
        else if (measured && m_round_min_ns < m_css_baseline_ns)
        {
            m_css_baseline_ns = kNeverNs; // the delay rise was not lasting: slow start resumes
            m_phase = CongestionPhase::SlowStart;
        }
        if (m_phase == CongestionPhase::SlowStart)
        {
            add_window(limit);
            return;
        }
        const u64 total = limit + m_css_carry;
        m_css_carry = total % kHystartCssGrowthDivisor;
        add_window(total / kHystartCssGrowthDivisor);
    }

    u64 m_initial = 0U;
    u64 m_recovery_start_ns = kNeverNs;
    u64 m_reductions = 0U;
    u64 m_undone = 0U;
    CongestionPhase m_phase = CongestionPhase::SlowStart;
    u64 m_acked = 0U; // bytes the ACK frame being processed acknowledged that grow the window

    // Rounds: one ends when the ACK frame acknowledges m_window_end, an Application packet number.
    u64 m_next_number = 0U;
    u64 m_window_end = 0U;
    bool m_rounds = false;
    bool m_round_ended = false;
    bool m_limited = false;        // this round
    bool m_limited_before = false; // the round before

    // HyStart++.
    u64 m_round_min_ns = kNeverNs;
    u64 m_last_round_min_ns = kNeverNs;
    u64 m_css_baseline_ns = kNeverNs;
    u64 m_samples = 0U;
    u64 m_css_carry = 0U;
    u32 m_css_rounds = 0U;

    // The period's undo.
    CongestionState m_prior;
    u64 m_episode_lost = 0U;
    u64 m_episode_spurious = 0U;
    bool m_undoable = false;
};

// RFC 9002 appendix B.
class NewReno final : public WindowController
{
public:
    using WindowController::WindowController;

    [[nodiscard]] CongestionAlgorithm algorithm() const noexcept override { return CongestionAlgorithm::NewReno; }

protected:
    void avoid(u64 acked, const QuicRtt& /*rtt*/, u64 /*now_ns*/) noexcept override
    {
        // One datagram per window acknowledged, its remainder carried.
        const u64 total = acked * m_datagram + m_carry;
        add_window(total / m_window);
        m_carry = total % m_window;
    }

    void reduce(u64 /*now_ns*/) noexcept override
    {
        m_prior_carry = m_carry;
        m_threshold = m_window / 2U;
        m_window = m_threshold;
        m_carry = 0U;
    }

    void restore() noexcept override { m_carry = m_prior_carry; }
    void collapse() noexcept override { m_carry = 0U; }
    void start_over() noexcept override { m_carry = 0U; }

private:
    u64 m_carry = 0U;
    u64 m_prior_carry = 0U;
};

// RFC 9438, in bytes, with time in 2^-10 s steps.
class Cubic final : public WindowController
{
public:
    using WindowController::WindowController;

    [[nodiscard]] CongestionAlgorithm algorithm() const noexcept override { return CongestionAlgorithm::Cubic; }

protected:
    void avoid(u64 acked, const QuicRtt& rtt, u64 now_ns) noexcept override
    {
        if (m_cubic.epoch_ns == kNeverNs)
        {
            // A new epoch (section 4.2): K from W_max, or 0 above it (the window is then the curve's origin).
            m_cubic.epoch_ns = now_ns;
            m_cubic.origin = max_u64(m_cubic.w_max, m_window);
            m_cubic.k = cube_root((m_cubic.origin - m_window) * kCubicKScale / m_datagram);
            m_cubic.w_est = m_window;
            m_est_carry = 0U;
            m_grow_carry = 0U;
        }
        // Section 4.3: the Reno-friendly estimate, alpha 1 once it passed the window before the last reduction.
        const bool reno = m_cubic.w_est >= m_cubic.window_prior;
        const u64 alpha_num = reno ? 1U : kCubicAlphaNumerator;
        const u64 alpha_den = reno ? 1U : kCubicAlphaDenominator;
        const u64 est_total = acked * m_datagram * alpha_num + m_est_carry;
        const u64 est_unit = m_window * alpha_den;
        m_cubic.w_est = min_u64(m_cubic.w_est + est_total / est_unit, kCongestionMaxWindow);
        m_est_carry = est_total % est_unit;

        const u64 elapsed = (now_ns - min_u64(m_cubic.epoch_ns, now_ns)) + rtt.smoothed_ns; // t + RTT
        const u64 cubic = curve(elapsed);
        if (cubic < m_cubic.w_est)
        {
            m_window = max_u64(m_window, m_cubic.w_est); // section 4.3: the Reno-friendly region
            return;
        }
        // Sections 4.4 and 4.5: toward the target, at most half the window further, each acknowledged byte
        // closing (target - window) / window of it.
        const u64 target = min_u64(max_u64(cubic, m_window), m_window + m_window / 2U);
        const u64 total = (target - m_window) * acked + m_grow_carry;
        add_window(total / m_window);
        m_grow_carry = total % m_window;
    }

    void reduce(u64 /*now_ns*/) noexcept override
    {
        m_prior_cubic = m_cubic;
        // Section 4.6 and 4.7's fast convergence: a window that fell short of W_max releases bandwidth.
        m_cubic.window_prior = m_window;
        m_cubic.w_max = (m_policy.fast_convergence && m_window < m_cubic.w_max)
                            ? m_window * kCubicConvergenceNumerator / kCubicConvergenceDenominator
                            : m_window;
        m_threshold = m_window * kCubicBetaNumerator / kCubicBetaDenominator;
        m_window = m_threshold;
        m_cubic.epoch_ns = kNeverNs;
    }

    void restore() noexcept override { m_cubic = m_prior_cubic; }

    void collapse() noexcept override { m_cubic.epoch_ns = kNeverNs; }

    // K is in the old datagram's unit: the curve starts again from the window toward W_max (section 4.2).
    void resized() noexcept override { m_cubic.epoch_ns = kNeverNs; }

    void start_over() noexcept override
    {
        m_cubic = Curve{};
        m_est_carry = 0U;
        m_grow_carry = 0U;
        m_last_sent_ns = kNeverNs;
    }

    // Section 5.8: an idle period does not count toward the curve's time (the epoch moves forward by it).
    void sent(const CongestionPacket& packet, u64 in_flight, u64 now_ns) noexcept override
    {
        if (in_flight == packet.bytes && m_cubic.epoch_ns != kNeverNs && m_last_sent_ns != kNeverNs &&
            now_ns > m_last_sent_ns)
        {
            m_cubic.epoch_ns = min_u64(m_cubic.epoch_ns + (now_ns - m_last_sent_ns), now_ns);
        }
        m_last_sent_ns = now_ns;
    }

private:
    struct Curve
    {
        u64 w_max = 0U;        // bytes
        u64 window_prior = 0U; // the window before the last reduction
        u64 origin = 0U;       // max(W_max, window at the epoch's start)
        u64 k = 0U;            // 2^-10 s
        u64 epoch_ns = kNeverNs;
        u64 w_est = 0U;
    };

    // W_cubic at `elapsed_ns` into the epoch: origin -+ C * |t - K|^3 datagrams.
    [[nodiscard]] u64 curve(u64 elapsed_ns) const noexcept
    {
        const u64 steps = (elapsed_ns >= (u64{1} << 53U))
                              ? kCubicMaxOffset + m_cubic.k
                              : (elapsed_ns << kCubicTimeShift) / kNanosPerSecond;
        const bool before = steps < m_cubic.k;
        const u64 offset = min_u64(before ? m_cubic.k - steps : steps - m_cubic.k, kCubicMaxOffset);
        const WideProduct scaled = multiply_wide(offset * offset * offset, kCubicCNumerator * m_datagram);
        const u64 shift = 3U * kCubicTimeShift;
        u64 delta = kCongestionMaxWindow;
        if ((scaled.hi >> shift) == 0U)
        {
            delta = min_u64(((scaled.hi << (64U - shift)) | (scaled.lo >> shift)) / kCubicCDenominator,
                            kCongestionMaxWindow);
        }
        if (before)
        {
            return (delta < m_cubic.origin) ? m_cubic.origin - delta : 0U;
        }
        return min_u64(m_cubic.origin + delta, kCongestionMaxWindow);
    }

    Curve m_cubic;
    Curve m_prior_cubic;
    u64 m_est_carry = 0U;
    u64 m_grow_carry = 0U;
    u64 m_last_sent_ns = kNeverNs;
};

// ---- BBRv3 (draft-ietf-ccwg-bbr-07) ----

constexpr u64 kInfinite = ~u64{0};
// Gains in percent (section 5.6.1's table).
constexpr u64 kBbrStartupPacingGain = 277U; // 4 ln 2: the least that doubles the sending rate each round
constexpr u64 kBbrDrainPacingGain = 50U;
constexpr u64 kBbrDownPacingGain = 90U;
constexpr u64 kBbrUpPacingGain = 125U;
constexpr u64 kBbrUnitGain = 100U;
constexpr u64 kBbrDefaultCwndGain = 200U;
constexpr u64 kBbrUpCwndGain = 225U;
constexpr u64 kBbrProbeRttCwndGain = 50U;
constexpr u64 kBbrPacingMarginPercent = 1U;
constexpr u64 kBbrLossThreshPercent = 2U;
constexpr u64 kBbrBetaPercent = 70U;
constexpr u64 kBbrHeadroomPercent = 15U;
constexpr u64 kBbrMinPipePackets = 4U;
constexpr u64 kBbrMinRttFilterNs = 10U * kNanosPerSecond;
constexpr u64 kBbrProbeRttDurationNs = 200000000U;
// The filter keeps samples at most this many cycles old: the current and the previous cycle (MaxBwFilterLen 2).
constexpr u64 kBbrMaxBwFilterCycles = 1U;
constexpr u64 kBbrExtraAckedFilterRounds = 10U;
constexpr u32 kBbrStartupFullLossCount = 6U;
constexpr u32 kBbrFullBwRounds = 3U;
constexpr u64 kBbrDrainMaxRounds = 3U;
constexpr u64 kBbrProbeWaitBaseNs = 2U * kNanosPerSecond;
constexpr u64 kBbrProbeWaitRandomNs = kNanosPerSecond;
constexpr u64 kBbrRenoRoundsBound = 63U;
constexpr u32 kBbrMaxProbeUpRounds = 30U;
constexpr u64 kBbrSendQuantumMax = 65536U;
constexpr u64 kBbrSendQuantumNs = 1000000U;  // the pacing rate's 1 ms
constexpr u64 kBbrUnsampledRttNs = 1000000U; // InitPacingRate's round trip before any sample

// `x` times `gain` percent; infinite stays infinite, and the product saturates.
[[nodiscard]] u64 gained(u64 x, u64 gain) noexcept
{
    return (x == kInfinite) ? kInfinite : multiply_divide(x, gain, kPercent);
}

[[nodiscard]] constexpr u64 saturating_add(u64 a, u64 b) noexcept
{
    return (a > kInfinite - b) ? kInfinite : a + b;
}

// Section 5.5.10.2: losses past BBR.LossThresh of what was in flight.
[[nodiscard]] constexpr bool inflight_too_high(u64 lost, u64 tx_in_flight) noexcept
{
    return lost * kPercent > tx_in_flight * kBbrLossThreshPercent;
}

class Bbr final : public CongestionController
{
public:
    Bbr(const CongestionPolicy& policy, u64 max_datagram, DeliveryRateSampler& delivery, u64 entropy) noexcept
        : m_policy(policy), m_delivery(delivery), m_rng(entropy)
    {
        size(max_datagram);
        init();
    }

    void seed(const CongestionState& from) noexcept
    {
        m_cwnd = min_u64(max_u64(from.window, m_min_pipe), kCongestionMaxWindow);
        m_recovery_start_ns = from.recovery_start_ns;
        m_in_recovery = from.phase == CongestionPhase::Recovery;
        m_prior_cwnd = m_in_recovery ? m_cwnd : 0U;
        m_reductions = from.reductions;
        m_undone = from.undone;
        m_cwnd_limited = from.limited;
    }

    [[nodiscard]] CongestionAlgorithm algorithm() const noexcept override { return CongestionAlgorithm::Bbr; }

    void on_sent(const CongestionPacket& packet, u64 in_flight, u64 now_ns) noexcept override
    {
        if (in_flight <= packet.bytes && m_delivery.app_limited())
        {
            restart_from_idle(now_ns); // section 5.4
        }
        m_in_flight = in_flight;
        if (in_flight + m_datagram > m_cwnd)
        {
            m_cwnd_limited = true;
        }
    }

    void on_limited(bool window, u64 /*now_ns*/) noexcept override
    {
        m_cwnd_limited = m_cwnd_limited || window;
    }

    void on_acked(const CongestionPacket& packet, u64 /*now_ns*/) noexcept override
    {
        if (m_in_recovery && packet.sent_ns > m_recovery_start_ns)
        {
            m_in_recovery = false; // a packet sent after the loss arrived: recovery ends (section 5.6.4.4)
            m_recovery_ending = true;
        }
    }

    void on_ack_end(const QuicRtt& rtt, bool sampled, u64 in_flight, u64 now_ns) noexcept override
    {
        m_in_flight = in_flight;
        const DeliverySample& rs = m_delivery.sample();
        if (!rs.acked)
        {
            return; // nothing counted in flight was delivered: no sample (section 4.1.2.3)
        }
        if (m_pacing_rate == 0U)
        {
            m_pacing_rate = initial_pacing_rate(rtt);
        }
        update_model_and_state(rs, sampled ? rtt.latest_ns : kNeverNs, now_ns);
        set_pacing_rate(m_pacing_gain);
        set_send_quantum();
        set_cwnd(rs);
    }

    void on_lost(const CongestionPacket& packet, u64 now_ns) noexcept override
    {
        if (m_recovery_start_ns == kNeverNs || packet.sent_ns > m_recovery_start_ns)
        {
            // A loss no recovery period covers (RFC 9002 section 7.3.2): Fast Recovery begins.
            save_cwnd();
            m_recovery_start_ns = now_ns;
            m_in_recovery = true;
            save_state_upon_loss();
            m_episode_start_ns = now_ns;
            m_episode_lost = 0U;
            m_episode_spurious = 0U;
            m_episode_reductions = m_reductions;
            m_undoable = true;
        }
        m_episode_lost += packet.bytes;
        note_loss();
        if (!m_check_probe_loss)
        {
            return; // not a packet sent while probing for bandwidth
        }
        const DeliveryStamp& stamp = packet.delivery;
        const u64 lost = m_delivery.lost() - min_u64(stamp.lost, m_delivery.lost());
        if (inflight_too_high(lost, stamp.in_flight))
        {
            handle_inflight_too_high(inflight_at_loss(packet.bytes, stamp.in_flight, lost), stamp.app_limited, now_ns);
        }
    }

    // Section 3.7: CE marks are congestion. Outside probing they count as a loss of the round; while probing
    // (Startup, ProbeBW_REFILL and ProbeBW_UP) they show the inflight was too high, and Startup ends.
    void on_ecn(u64 /*marked*/, u64 ce, u64 /*newest_sent_ns*/, u64 now_ns) noexcept override
    {
        if (ce == 0U)
        {
            return;
        }
        m_undoable = false; // a mark is no spurious loss
        note_loss();
        const DeliverySample& rs = m_delivery.sample();
        if (m_mode == CongestionPhase::SlowStart && !m_full_bw_reached)
        {
            handle_inflight_too_high(rs.tx_in_flight, rs.app_limited, now_ns);
            m_full_bw_reached = true; // CheckStartupDone() enters Drain at the frame's end
            return;
        }
        if (m_check_probe_loss)
        {
            handle_inflight_too_high(rs.tx_in_flight, rs.app_limited, now_ns);
        }
    }

    void on_ecn_unavailable() noexcept override {}

    // Section 5.6.4.4's OnEnterRTO: QUIC's persistent congestion takes the window to RFC 9002's minimum.
    void on_persistent_congestion(u64 /*now_ns*/) noexcept override
    {
        save_cwnd();
        save_state_upon_loss();
        m_cwnd = m_minimum;
        m_undoable = false;
        ++m_reductions;
    }

    void on_spurious(const CongestionPacket& packet, u64 lost_ns, u64 /*now_ns*/) noexcept override
    {
        if (!m_undoable || lost_ns < m_episode_start_ns)
        {
            return; // a loss of an earlier episode
        }
        m_episode_spurious += packet.bytes;
        if (m_episode_spurious < m_episode_lost)
        {
            return;
        }
        m_undone += (m_reductions > m_episode_reductions) ? 1U : 0U;
        handle_spurious_loss();
        m_undoable = false;
    }

    void reset(u64 /*now_ns*/) noexcept override { init(); }

    void set_max_datagram(u64 bytes) noexcept override
    {
        const bool initial = m_cwnd == m_initial;
        size(min_u64(max_u64(bytes, kQuicMinInitialDatagram), kMaxDatagram));
        m_cwnd = initial ? m_initial : min_u64(max_u64(m_cwnd, m_min_pipe), kCongestionMaxWindow);
    }

    [[nodiscard]] u64 window() const noexcept override { return m_cwnd; }

    [[nodiscard]] u64 pacing_rate(const QuicRtt& rtt) const noexcept override
    {
        return (m_pacing_rate != 0U) ? m_pacing_rate : initial_pacing_rate(rtt);
    }

    [[nodiscard]] u64 burst() const noexcept override { return m_send_quantum; }

    [[nodiscard]] CongestionState state() const noexcept override
    {
        CongestionState out;
        out.window = m_cwnd;
        out.slow_start_threshold = m_full_bw_reached ? max_u64(m_cwnd, m_minimum) : kCongestionMaxWindow;
        out.recovery_start_ns = m_recovery_start_ns;
        out.reductions = m_reductions;
        out.undone = m_undone;
        out.bandwidth = m_bw;
        out.min_rtt_ns = (m_min_rtt_ns == kInfinite) ? 0U : m_min_rtt_ns;
        out.phase = m_in_recovery ? CongestionPhase::Recovery : m_mode;
        out.limited = cwnd_limited();
        return out;
    }

private:
    enum class Undo : u8
    {
        None,
        Startup,
        ProbeBwUp,
    };

    void size(u64 max_datagram) noexcept
    {
        const u64 packets = m_policy.initial_packets;
        m_datagram = max_datagram;
        m_minimum = m_policy.minimum_packets * max_datagram;
        m_min_pipe = max_u64(kBbrMinPipePackets * max_datagram, m_minimum);
        m_initial = min_u64(packets * max_datagram, max_u64(packets * kInitialBytesPerPacket, 2U * max_datagram));
        m_initial = max_u64(m_initial, m_minimum);
    }

    // Section 5.2.1's OnInit.
    void init() noexcept
    {
        m_cwnd = m_initial;
        m_pacing_rate = 0U;
        m_send_quantum = min_u64(max_u64(m_initial, 2U * m_datagram), kBbrSendQuantumMax);
        m_max_bw_filter = WindowedMax<u64>(kBbrMaxBwFilterCycles);
        m_max_bw = 0U;
        m_bw = 0U;
        m_cycle_count = 0U;
        m_min_rtt_ns = kInfinite;
        m_min_rtt_stamp_ns = kNeverNs;
        m_min_rtt_expired = false;
        m_probe_rtt_done_ns = 0U;
        m_probe_rtt_round_done = false;
        m_probe_rtt_cwnd = kInfinite;
        m_prior_cwnd = 0U;
        m_undo = Undo::None;
        m_undo_bw_shortterm = 0U;
        m_undo_inflight_shortterm = 0U;
        m_undo_inflight_longterm = 0U;
        m_idle_restart = false;
        m_extra_acked_filter = WindowedMax<u64>(1U);
        m_extra_acked = 0U;
        m_extra_acked_start_ns = 0U;
        m_extra_acked_delivered = 0U;
        m_full_bw_reached = false;
        m_inflight_longterm = kInfinite;
        m_probe_up_acked_per_inc = kInfinite;
        m_bw_probe_up_acked = 0U;
        m_bw_probe_up_rounds = 0U;
        m_rounds_since_probe_up = 0U;
        m_check_probe_loss = false;
        m_prev_probe_too_high = false;
        m_prev_probe_precautionary = false;
        m_is_bw_probe_stopping = false;
        m_cycle_stamp_ns = 0U;
        m_bw_probe_wait_ns = 0U;
        m_loss_round_delivered = 0U;
        m_loss_round_start = false;
        m_recovery_start_ns = kNeverNs;
        m_in_recovery = false;
        m_recovery_ending = false;
        m_undoable = false;
        m_cwnd_limited = false;
        m_cwnd_limited_before = false;
        reset_congestion_signals();
        reset_short_term_model();
        m_next_round_delivered = 0U; // InitRoundCounting
        m_round_start = false;
        m_round_count = 0U;
        reset_full_bw();
        enter_startup();
    }

    // ---- the per-ACK model and state (section 5.2.3) ----

    void update_model_and_state(const DeliverySample& rs, u64 rtt_ns, u64 now_ns) noexcept
    {
        update_latest_delivery_signals(rs);
        update_congestion_signals(rs);
        update_ack_aggregation(rs, now_ns);
        check_full_bw_reached(rs);
        check_startup_done(rs);
        check_drain_done(now_ns);
        update_probe_bw_cycle_phase(rs, now_ns);
        update_min_rtt(rtt_ns, now_ns);
        check_probe_rtt(rs, now_ns);
        advance_latest_delivery_signals(rs);
        m_bw = min_u64(m_max_bw, m_bw_shortterm); // BoundBWForModel
    }

    // Section 5.5.1.
    void start_round() noexcept { m_next_round_delivered = m_delivery.delivered(); }

    void update_round(const DeliverySample& rs) noexcept
    {
        m_round_start = rs.prior_delivered >= m_next_round_delivered;
        if (!m_round_start)
        {
            return;
        }
        start_round();
        ++m_round_count;
        ++m_rounds_since_probe_up;
        m_cwnd_limited_before = m_cwnd_limited;
        m_cwnd_limited = false;
    }

    [[nodiscard]] bool cwnd_limited() const noexcept { return m_cwnd_limited || m_cwnd_limited_before; }

    // Section 5.5.5: an application-limited sample counts only when it raises the estimate.
    void update_max_bw(const DeliverySample& rs) noexcept
    {
        update_round(rs);
        if (rs.rate > 0U && (rs.rate >= m_max_bw || !rs.app_limited))
        {
            m_max_bw_filter.update(rs.rate, m_cycle_count);
            m_max_bw = m_max_bw_filter.best();
        }
    }

    // Section 5.5.10.3.
    void update_latest_delivery_signals(const DeliverySample& rs) noexcept
    {
        m_loss_round_start = false;
        m_bw_latest = max_u64(m_bw_latest, rs.rate);
        m_inflight_latest = max_u64(m_inflight_latest, rs.delivered);
        if (rs.prior_delivered >= m_loss_round_delivered)
        {
            m_loss_round_delivered = m_delivery.delivered();
            m_loss_round_start = true;
        }
    }

    void advance_latest_delivery_signals(const DeliverySample& rs) noexcept
    {
        if (m_loss_round_start)
        {
            m_bw_latest = rs.rate;
            m_inflight_latest = rs.delivered;
            m_loss_events_in_round = 0U;
        }
    }

    void reset_congestion_signals() noexcept
    {
        m_is_loss_in_round = false;
        m_loss_events_in_round = 0U;
        m_bw_latest = 0U;
        m_inflight_latest = 0U;
    }

    void update_congestion_signals(const DeliverySample& rs) noexcept
    {
        update_max_bw(rs);
        if (!m_loss_round_start)
        {
            return; // once a round trip
        }
        if (!is_probing_bw() && m_is_loss_in_round)
        {
            // AdaptLowerBoundsFromCongestion: the short-term model takes the round's loss.
            m_bw_shortterm = (m_bw_shortterm == kInfinite) ? m_max_bw : m_bw_shortterm;
            m_inflight_shortterm = (m_inflight_shortterm == kInfinite) ? m_cwnd : m_inflight_shortterm;
            m_bw_shortterm = max_u64(m_bw_latest, gained(m_bw_shortterm, kBbrBetaPercent));
            m_inflight_shortterm = max_u64(m_inflight_latest, gained(m_inflight_shortterm, kBbrBetaPercent));
            ++m_reductions;
        }
        m_is_loss_in_round = false;
    }

    void reset_short_term_model() noexcept
    {
        m_bw_shortterm = kInfinite;
        m_inflight_shortterm = kInfinite;
    }

    // Section 5.5.9.
    void update_ack_aggregation(const DeliverySample& rs, u64 now_ns) noexcept
    {
        const u64 interval = now_ns - min_u64(m_extra_acked_start_ns, now_ns);
        u64 expected = multiply_divide(m_bw, interval, kNanosPerSecond);
        if (m_extra_acked_delivered <= expected)
        {
            m_extra_acked_delivered = 0U; // the ACK rate fell below the expected: a new interval
            m_extra_acked_start_ns = now_ns;
            expected = 0U;
        }
        m_extra_acked_delivered += rs.newly_acked;
        const u64 extra = min_u64(m_extra_acked_delivered - expected, m_cwnd);
        m_extra_acked_filter.set_window(m_full_bw_reached ? kBbrExtraAckedFilterRounds : 1U);
        m_extra_acked_filter.update(extra, m_round_count);
        m_extra_acked = m_extra_acked_filter.best();
    }

    // ---- Startup and Drain (sections 5.3.1 and 5.3.2) ----

    void enter_startup() noexcept
    {
        m_mode = CongestionPhase::SlowStart;
        m_pacing_gain = kBbrStartupPacingGain;
        m_cwnd_gain = kBbrDefaultCwndGain;
    }

    void reset_full_bw() noexcept
    {
        m_full_bw = 0U;
        m_full_bw_count = 0U;
        m_full_bw_now = false;
    }

    // Section 5.3.1.2: three non-application-limited rounds without 25% growth fill the pipe.
    void check_full_bw_reached(const DeliverySample& rs) noexcept
    {
        if (m_full_bw_now || !m_round_start || rs.app_limited)
        {
            return;
        }
        if (rs.rate >= saturating_add(m_full_bw, m_full_bw / 4U))
        {
            reset_full_bw();
            m_full_bw = rs.rate;
            return;
        }
        ++m_full_bw_count;
        m_full_bw_now = m_full_bw_count >= kBbrFullBwRounds;
        m_full_bw_reached = m_full_bw_reached || m_full_bw_now;
    }

    // Section 5.3.1.3.
    void check_startup_done(const DeliverySample& rs) noexcept
    {
        if (!m_full_bw_reached && m_loss_round_start && m_in_recovery &&
            m_loss_events_in_round >= kBbrStartupFullLossCount && inflight_too_high(rs.lost, rs.tx_in_flight))
        {
            m_undo = Undo::Startup;
            m_full_bw_reached = true;
            m_inflight_longterm = max_u64(bdp(), m_inflight_latest);
            ++m_reductions;
        }
        if (m_mode == CongestionPhase::SlowStart && m_full_bw_reached)
        {
            m_mode = CongestionPhase::Drain; // EnterDrain
            m_pacing_gain = kBbrDrainPacingGain;
            m_cwnd_gain = kBbrDefaultCwndGain;
            m_drain_start_round = m_round_count;
        }
    }

    void check_drain_done(u64 now_ns) noexcept
    {
        if (m_mode == CongestionPhase::Drain &&
            (m_in_flight <= inflight(m_bw, kBbrUnitGain) || m_round_count > m_drain_start_round + kBbrDrainMaxRounds))
        {
            m_cwnd_gain = kBbrDefaultCwndGain; // EnterProbeBW
            start_probe_bw_down(now_ns);
        }
    }

    // ---- ProbeBW (section 5.3.3) ----

    [[nodiscard]] bool in_probe_bw() const noexcept
    {
        return m_mode == CongestionPhase::ProbeBwDown || m_mode == CongestionPhase::ProbeBwCruise ||
               m_mode == CongestionPhase::ProbeBwRefill || m_mode == CongestionPhase::ProbeBwUp;
    }

    [[nodiscard]] bool is_probing_bw() const noexcept
    {
        return m_mode == CongestionPhase::SlowStart || m_mode == CongestionPhase::ProbeBwRefill ||
               m_mode == CongestionPhase::ProbeBwUp;
    }

    void start_probe_bw_down(u64 now_ns) noexcept
    {
        reset_congestion_signals();
        m_probe_up_acked_per_inc = kInfinite; // not growing the long-term inflight
        pick_probe_wait();
        m_cycle_stamp_ns = now_ns;
        m_is_bw_probe_stopping = true;
        start_round();
        m_mode = CongestionPhase::ProbeBwDown;
        m_pacing_gain = kBbrDownPacingGain;
        m_cwnd_gain = kBbrDefaultCwndGain;
    }

    void start_probe_bw_cruise() noexcept
    {
        m_mode = CongestionPhase::ProbeBwCruise;
        m_pacing_gain = kBbrUnitGain;
        m_cwnd_gain = kBbrDefaultCwndGain;
    }

    void start_probe_bw_refill() noexcept
    {
        reset_short_term_model();
        m_bw_probe_up_rounds = 0U;
        m_bw_probe_up_acked = 0U;
        m_prev_probe_precautionary = false;
        m_is_bw_probe_stopping = false;
        start_round();
        m_mode = CongestionPhase::ProbeBwRefill;
        m_pacing_gain = kBbrUnitGain;
        m_cwnd_gain = kBbrDefaultCwndGain;
    }

    void start_probe_bw_up(const DeliverySample& rs) noexcept
    {
        start_round();
        reset_full_bw();
        m_full_bw = rs.rate;
        m_mode = CongestionPhase::ProbeBwUp;
        m_pacing_gain = kBbrUpPacingGain;
        m_cwnd_gain = kBbrUpCwndGain;
        raise_inflight_longterm_slope();
    }

    void update_probe_bw_cycle_phase(const DeliverySample& rs, u64 now_ns) noexcept
    {
        if (!m_full_bw_reached || adapt_long_term_model(rs) || !in_probe_bw())
        {
            return;
        }
        switch (m_mode)
        {
            case CongestionPhase::ProbeBwDown:
            {
                if (!time_to_probe_bw(now_ns) && time_to_cruise())
                {
                    start_probe_bw_cruise();
                }
                break;
            }
            case CongestionPhase::ProbeBwCruise:
            {
                static_cast<void>(time_to_probe_bw(now_ns));
                break;
            }
            case CongestionPhase::ProbeBwRefill:
            {
                if (m_round_start) // one round of REFILL, then UP
                {
                    m_check_probe_loss = true;
                    start_probe_bw_up(rs);
                }
                break;
            }
            case CongestionPhase::ProbeBwUp:
            {
                if (time_to_go_down(rs))
                {
                    m_prev_probe_too_high = false; // no high loss yet
                    start_probe_bw_down(now_ns);
                }
                break;
            }
            default:
            {
                break;
            }
        }
    }

    // Section 5.3.3.8.2: the wall clock bound of 2 to 3 s and the Reno bound of the BDP in packets, at most 63
    // rounds, whichever comes first; the round bound starts 0 or 1 rounds in.
    void pick_probe_wait() noexcept
    {
        m_rounds_since_probe_up = m_rng.next_u32() & 1U;
        m_bw_probe_wait_ns = kBbrProbeWaitBaseNs + m_rng.next_below(kBbrProbeWaitRandomNs);
    }

    [[nodiscard]] bool time_to_probe_bw(u64 now_ns) noexcept
    {
        const bool elapsed = now_ns > saturating_add(m_cycle_stamp_ns, m_bw_probe_wait_ns);
        const u64 reno_rounds = min_u64(target_inflight() / m_datagram, kBbrRenoRoundsBound);
        if (!elapsed && m_rounds_since_probe_up < reno_rounds)
        {
            return false;
        }
        start_probe_bw_refill();
        return true;
    }

    [[nodiscard]] bool time_to_cruise() const noexcept
    {
        return m_in_flight <= inflight_with_headroom() && m_in_flight <= inflight(m_max_bw, kBbrUnitGain);
    }

    [[nodiscard]] bool time_to_go_down(const DeliverySample& rs) noexcept
    {
        if (m_prev_probe_too_high && m_in_flight >= m_inflight_longterm)
        {
            m_prev_probe_precautionary = true; // section 5.3.3.6's deceleration
            return true;
        }
        if (cwnd_limited() && m_cwnd >= m_inflight_longterm)
        {
            reset_full_bw(); // the long-term inflight limits the bandwidth found
            m_full_bw = rs.rate;
            return false;
        }
        return m_full_bw_now;
    }

    [[nodiscard]] u64 inflight_with_headroom() const noexcept
    {
        if (m_inflight_longterm == kInfinite)
        {
            return kInfinite;
        }
        const u64 headroom = max_u64(m_datagram, gained(m_inflight_longterm, kBbrHeadroomPercent));
        return max_u64(m_inflight_longterm - min_u64(headroom, m_inflight_longterm), m_min_pipe);
    }

    void raise_inflight_longterm_slope() noexcept
    {
        const u64 growth = u64{1} << m_bw_probe_up_rounds;
        m_bw_probe_up_rounds = (m_bw_probe_up_rounds < kBbrMaxProbeUpRounds) ? m_bw_probe_up_rounds + 1U
                                                                               : kBbrMaxProbeUpRounds;
        m_probe_up_acked_per_inc = max_u64(m_cwnd / growth, m_datagram);
    }

    void probe_inflight_longterm_upward(const DeliverySample& rs) noexcept
    {
        if (!cwnd_limited() || m_cwnd < m_inflight_longterm)
        {
            return; // not using all of it, so it does not grow
        }
        m_bw_probe_up_acked += rs.newly_acked;
        if (m_bw_probe_up_acked >= m_probe_up_acked_per_inc)
        {
            const u64 delta = m_bw_probe_up_acked / m_probe_up_acked_per_inc;
            m_bw_probe_up_acked -= delta * m_probe_up_acked_per_inc;
            m_inflight_longterm =
                min_u64(saturating_add(m_inflight_longterm, delta * m_datagram), kCongestionMaxWindow);
        }
        if (m_round_start)
        {
            raise_inflight_longterm_slope();
        }
    }

    // True when it decided a state transition.
    [[nodiscard]] bool adapt_long_term_model(const DeliverySample& rs) noexcept
    {
        if (m_is_bw_probe_stopping && m_round_start)
        {
            // The probe's samples have all arrived.
            m_check_probe_loss = false;
            m_is_bw_probe_stopping = false;
            if (in_probe_bw() && !rs.app_limited)
            {
                ++m_cycle_count; // AdvanceMaxBwFilter
            }
            if (in_probe_bw() && m_prev_probe_precautionary && !m_prev_probe_too_high)
            {
                start_probe_bw_refill(); // section 5.3.3.6's acceleration
                return true;
            }
        }
        if (inflight_too_high(rs.lost, rs.tx_in_flight) || m_inflight_longterm == kInfinite)
        {
            return false;
        }
        m_inflight_longterm = max_u64(m_inflight_longterm, rs.tx_in_flight); // the loss rate is safe
        if (m_mode == CongestionPhase::ProbeBwUp)
        {
            probe_inflight_longterm_upward(rs);
        }
        return false;
    }

    // ---- loss (section 5.5.10) ----

    void note_loss() noexcept
    {
        if (!m_is_loss_in_round)
        {
            m_loss_round_delivered = m_delivery.delivered(); // the first loss of this round trip
            save_state_upon_loss();
        }
        m_is_loss_in_round = true;
        ++m_loss_events_in_round;
    }

    void handle_inflight_too_high(u64 tx_in_flight, bool app_limited, u64 now_ns) noexcept
    {
        m_prev_probe_too_high = true;
        m_check_probe_loss = false; // once a probe
        if (!app_limited)
        {
            m_inflight_longterm =
                min_u64(max_u64(tx_in_flight, gained(target_inflight(), kBbrBetaPercent)), kCongestionMaxWindow);
        }
        ++m_reductions;
        if (m_mode == CongestionPhase::ProbeBwUp)
        {
            m_undo = Undo::ProbeBwUp;
            start_probe_bw_down(now_ns);
        }
    }

    // The bytes in flight at which this packet's losses crossed the threshold: the threshold of what was in flight
    // before it, less what was lost before it, over (1 - the threshold) of its own bytes.
    [[nodiscard]] static u64 inflight_at_loss(u64 bytes, u64 tx_in_flight, u64 lost) noexcept
    {
        const u64 before = tx_in_flight - min_u64(bytes, tx_in_flight);
        const u64 lost_before = lost - min_u64(bytes, lost);
        const u64 allowed = before * kBbrLossThreshPercent;
        const u64 taken = lost_before * kPercent;
        const u64 prefix = (allowed > taken) ? (allowed - taken) / (kPercent - kBbrLossThreshPercent) : 0U;
        return before + prefix;
    }

    // Section 5.5.11.
    void save_state_upon_loss() noexcept
    {
        save_cwnd();
        m_undo = Undo::None;
        m_undo_bw_shortterm = m_bw_shortterm;
        m_undo_inflight_shortterm = m_inflight_shortterm;
        m_undo_inflight_longterm = m_inflight_longterm;
    }

    void handle_spurious_loss() noexcept
    {
        restore_cwnd();
        m_in_recovery = false;
        m_is_loss_in_round = false;
        m_loss_events_in_round = 0U;
        reset_full_bw();
        m_bw_shortterm = max_u64(m_bw_shortterm, m_undo_bw_shortterm);
        m_inflight_shortterm = max_u64(m_inflight_shortterm, m_undo_inflight_shortterm);
        m_inflight_longterm = max_u64(m_inflight_longterm, m_undo_inflight_longterm);
        if (m_undo == Undo::Startup && m_mode != CongestionPhase::SlowStart)
        {
            m_full_bw_reached = false;
            if (m_mode != CongestionPhase::ProbeRtt)
            {
                enter_startup();
            }
        }
        else if (m_undo == Undo::ProbeBwUp && m_mode != CongestionPhase::ProbeBwUp &&
                 m_mode != CongestionPhase::ProbeRtt)
        {
            start_probe_bw_refill();
        }
        m_undo = Undo::None;
    }

    // ---- ProbeRTT and idle restarts (sections 5.3.4 and 5.4) ----

    void update_min_rtt(u64 rtt_ns, u64 now_ns) noexcept
    {
        m_min_rtt_expired =
            m_min_rtt_stamp_ns != kNeverNs && now_ns > saturating_add(m_min_rtt_stamp_ns, kBbrMinRttFilterNs);
        if (m_min_rtt_expired)
        {
            m_probe_rtt_cwnd = bdp_multiple(m_bw, kBbrProbeRttCwndGain);
        }
        if (rtt_ns != kNeverNs && (rtt_ns < m_min_rtt_ns || m_min_rtt_expired))
        {
            m_min_rtt_ns = rtt_ns;
            m_min_rtt_stamp_ns = now_ns;
        }
    }

    void check_probe_rtt(const DeliverySample& rs, u64 now_ns) noexcept
    {
        if (m_mode != CongestionPhase::ProbeRtt && m_min_rtt_expired && !m_idle_restart)
        {
            save_cwnd();
            m_mode = CongestionPhase::ProbeRtt; // EnterProbeRTT
            m_pacing_gain = kBbrUnitGain;
            m_cwnd_gain = kBbrProbeRttCwndGain;
            m_probe_rtt_done_ns = 0U;
            m_is_bw_probe_stopping = true;
            start_round();
        }
        if (m_mode == CongestionPhase::ProbeRtt)
        {
            handle_probe_rtt(now_ns);
        }
        if (rs.delivered > 0U)
        {
            m_idle_restart = false;
        }
    }

    void handle_probe_rtt(u64 now_ns) noexcept
    {
        m_delivery.on_app_limited(m_in_flight); // its low rate samples count for nothing
        if (m_probe_rtt_done_ns == 0U && m_in_flight <= probe_rtt_cwnd())
        {
            // At least ProbeRTTDuration and one round at the low inflight.
            m_probe_rtt_done_ns = max_u64(saturating_add(now_ns, kBbrProbeRttDurationNs), 1U);
            m_probe_rtt_round_done = false;
            start_round();
            return;
        }
        if (m_probe_rtt_done_ns != 0U)
        {
            m_probe_rtt_round_done = m_probe_rtt_round_done || m_round_start;
            if (m_probe_rtt_round_done)
            {
                check_probe_rtt_done(now_ns);
            }
        }
    }

    void check_probe_rtt_done(u64 now_ns) noexcept
    {
        if (m_probe_rtt_done_ns == 0U || now_ns <= m_probe_rtt_done_ns)
        {
            return;
        }
        m_min_rtt_stamp_ns = now_ns; // the next ProbeRTT is due a filter length from now
        restore_cwnd();
        reset_short_term_model(); // ExitProbeRTT
        if (m_full_bw_reached)
        {
            start_probe_bw_down(now_ns);
            start_probe_bw_cruise();
        }
        else
        {
            enter_startup();
        }
    }

    void restart_from_idle(u64 now_ns) noexcept
    {
        m_idle_restart = true;
        m_extra_acked_start_ns = now_ns;
        if (in_probe_bw())
        {
            set_pacing_rate(kBbrUnitGain);
        }
        else if (m_mode == CongestionPhase::ProbeRtt)
        {
            check_probe_rtt_done(now_ns);
        }
    }

    // ---- control parameters (section 5.6) ----

    // InitPacingRate: the initial window over the smoothed RTT (1 ms before a sample), at Startup's gain.
    [[nodiscard]] u64 initial_pacing_rate(const QuicRtt& rtt) const noexcept
    {
        const u64 round_trip = rtt.sampled() ? max_u64(rtt.smoothed_ns, 1U) : kBbrUnsampledRttNs;
        return max_u64(gained(multiply_divide(m_cwnd, kNanosPerSecond, round_trip), kBbrStartupPacingGain), 1U);
    }

    void set_pacing_rate(u64 gain) noexcept
    {
        const u64 rate = multiply_divide(m_bw, gain * (kPercent - kBbrPacingMarginPercent), kPercent * kPercent);
        if (rate != 0U && (m_full_bw_reached || rate > m_pacing_rate))
        {
            m_pacing_rate = rate;
        }
    }

    void set_send_quantum() noexcept
    {
        const u64 quantum = multiply_divide(m_pacing_rate, kBbrSendQuantumNs, kNanosPerSecond);
        m_send_quantum = max_u64(min_u64(quantum, kBbrSendQuantumMax), 2U * m_datagram);
    }

    [[nodiscard]] u64 bdp() const noexcept
    {
        return bdp_multiple(m_bw, kBbrUnitGain);
    }

    [[nodiscard]] u64 bdp_multiple(u64 bw, u64 gain) const noexcept
    {
        if (m_min_rtt_ns == kInfinite)
        {
            return m_initial; // no RTT sample yet
        }
        return min_u64(gained(multiply_divide(bw, m_min_rtt_ns, kNanosPerSecond), gain), kCongestionMaxWindow);
    }

    [[nodiscard]] u64 target_inflight() const noexcept { return min_u64(bdp(), m_cwnd); }

    // QuantizationBudget: the QUIC offload budget is the send quantum.
    [[nodiscard]] u64 quantization_budget(u64 cap) const noexcept
    {
        cap = max_u64(max_u64(cap, m_send_quantum), m_min_pipe);
        return (m_mode == CongestionPhase::ProbeBwUp) ? saturating_add(cap, 2U * m_datagram) : cap;
    }

    [[nodiscard]] u64 inflight(u64 bw, u64 gain) const noexcept { return quantization_budget(bdp_multiple(bw, gain)); }

    [[nodiscard]] u64 probe_rtt_cwnd() const noexcept
    {
        return max_u64(min_u64(bdp_multiple(m_bw, kBbrProbeRttCwndGain), m_probe_rtt_cwnd), m_min_pipe);
    }

    void save_cwnd() noexcept
    {
        m_prior_cwnd =
            (!m_in_recovery && m_mode != CongestionPhase::ProbeRtt) ? m_cwnd : max_u64(m_prior_cwnd, m_cwnd);
    }

    void restore_cwnd() noexcept { m_cwnd = max_u64(m_cwnd, m_prior_cwnd); }

    // Section 5.6.4.6, with 5.6.4.4's restore on leaving recovery and the bounds of 5.6.4.5 and 5.6.4.7.
    void set_cwnd(const DeliverySample& rs) noexcept
    {
        if (m_recovery_ending)
        {
            m_recovery_ending = false;
            restore_cwnd();
        }
        const u64 max_inflight = quantization_budget(saturating_add(bdp_multiple(m_bw, m_cwnd_gain), m_extra_acked));
        if (m_full_bw_reached)
        {
            m_cwnd = min_u64(saturating_add(m_cwnd, rs.newly_acked), max_inflight);
        }
        else if (m_cwnd < max_inflight || m_delivery.delivered() < m_initial)
        {
            m_cwnd = saturating_add(m_cwnd, rs.newly_acked);
        }
        m_cwnd = max_u64(m_cwnd, m_min_pipe);
        if (m_mode == CongestionPhase::ProbeRtt)
        {
            m_cwnd = min_u64(m_cwnd, probe_rtt_cwnd());
        }
        u64 cap = kInfinite;
        if (in_probe_bw() && m_mode != CongestionPhase::ProbeBwCruise)
        {
            cap = m_inflight_longterm;
        }
        else if (m_mode == CongestionPhase::ProbeRtt || m_mode == CongestionPhase::ProbeBwCruise)
        {
            cap = inflight_with_headroom();
        }
        cap = max_u64(min_u64(cap, m_inflight_shortterm), m_min_pipe);
        m_cwnd = min_u64(min_u64(m_cwnd, cap), kCongestionMaxWindow);
    }

    CongestionPolicy m_policy;
    DeliveryRateSampler& m_delivery;
    hesap::stats::PhiloxRng m_rng;
    u64 m_datagram = 0U;
    u64 m_minimum = 0U;  // RFC 9002's minimum window
    u64 m_min_pipe = 0U; // BBR.MinPipeCwnd, at least the minimum window
    u64 m_initial = 0U;

    // Control parameters.
    u64 m_cwnd = 0U;
    u64 m_pacing_rate = 0U; // 0 until the first ACK: InitPacingRate's from the RTT then
    u64 m_send_quantum = 0U;
    u64 m_pacing_gain = kBbrStartupPacingGain;
    u64 m_cwnd_gain = kBbrDefaultCwndGain;
    u64 m_in_flight = 0U;
    CongestionPhase m_mode = CongestionPhase::SlowStart;

    // Rounds.
    u64 m_next_round_delivered = 0U;
    u64 m_round_count = 0U;
    bool m_round_start = false;
    bool m_cwnd_limited = false;        // this round
    bool m_cwnd_limited_before = false; // the round before

    // The model.
    WindowedMax<u64> m_max_bw_filter{kBbrMaxBwFilterCycles};
    WindowedMax<u64> m_extra_acked_filter{1U};
    u64 m_max_bw = 0U;
    u64 m_bw_shortterm = kInfinite;
    u64 m_bw = 0U;
    u64 m_bw_latest = 0U;
    u64 m_inflight_latest = 0U;
    u64 m_cycle_count = 0U;
    u64 m_min_rtt_ns = kInfinite;
    u64 m_min_rtt_stamp_ns = kNeverNs;
    u64 m_inflight_longterm = kInfinite;
    u64 m_inflight_shortterm = kInfinite;
    u64 m_extra_acked = 0U;
    u64 m_extra_acked_start_ns = 0U;
    u64 m_extra_acked_delivered = 0U;
    bool m_min_rtt_expired = false;

    // Startup and Drain.
    u64 m_full_bw = 0U;
    u64 m_drain_start_round = 0U;
    u32 m_full_bw_count = 0U;
    bool m_full_bw_reached = false;
    bool m_full_bw_now = false;

    // Loss signals.
    u64 m_loss_round_delivered = 0U;
    u32 m_loss_events_in_round = 0U;
    bool m_loss_round_start = false;
    bool m_is_loss_in_round = false;

    // ProbeBW.
    u64 m_bw_probe_up_acked = 0U;
    u64 m_probe_up_acked_per_inc = kInfinite;
    u64 m_rounds_since_probe_up = 0U;
    u64 m_bw_probe_wait_ns = 0U;
    u64 m_cycle_stamp_ns = 0U;
    u32 m_bw_probe_up_rounds = 0U;
    bool m_is_bw_probe_stopping = false;
    bool m_check_probe_loss = false;
    bool m_prev_probe_too_high = false;
    bool m_prev_probe_precautionary = false;

    // ProbeRTT and idle restarts.
    u64 m_probe_rtt_done_ns = 0U;
    u64 m_probe_rtt_cwnd = kInfinite;
    u64 m_prior_cwnd = 0U;
    bool m_probe_rtt_round_done = false;
    bool m_idle_restart = false;

    // Recovery and the undo of a spurious episode.
    u64 m_recovery_start_ns = kNeverNs;
    u64 m_undo_bw_shortterm = 0U;
    u64 m_undo_inflight_shortterm = 0U;
    u64 m_undo_inflight_longterm = 0U;
    u64 m_episode_start_ns = 0U;
    u64 m_episode_lost = 0U;
    u64 m_episode_spurious = 0U;
    u64 m_episode_reductions = 0U;
    u64 m_reductions = 0U;
    u64 m_undone = 0U;
    Undo m_undo = Undo::None;
    bool m_in_recovery = false;
    bool m_recovery_ending = false;
    bool m_undoable = false;
};

// ---- Prague (draft-briscoe-iccrg-prague-congestion-control-04; RFC 9331 section 4.3) ----

constexpr u64 kPragueGainShift = 4U;             // section 2.3.2: g = 1/16
constexpr u64 kPragueRttVirtMinNs = 25000000U;   // section 2.4.4's RTT_VIRT_MIN
constexpr u64 kPragueRttTransitionRounds = 500U; // section 2.4.4's D
constexpr u64 kPragueBurstNs = 250000U;          // section 2.5.2's MAX_BURST_DELAY
constexpr u64 kPragueSlowStartPacingGain = 200U; // section 2.5.1: doubled while the window is under half the threshold
// TR-BB-2019-002 section 6.1's changeover (its alternative 2): a classic bottleneck's reduction reaches that of ABE
// (RFC 8511) at beta 0.7, an alpha of 2 (1 - 0.7).
constexpr u64 kPragueAbeAlphaNumerator = 3U;
constexpr u64 kPragueAbeAlphaDenominator = 5U;
constexpr u64 kPragueAlphaScaledOne = u64{kCongestionAlphaOne} << kPragueGainShift;

// The window skeleton with a scalable answer to ECN: slow start, recovery periods and the undo are the skeleton's; the
// moving fraction of marked packets, the reduction by it, growth and pacing are the draft's; the classic ECN AQM
// detector (TR-BB-2019-002) turns the reduction toward ABE's.
class Prague final : public WindowController
{
public:
    using WindowController::WindowController;

    [[nodiscard]] CongestionAlgorithm algorithm() const noexcept override { return CongestionAlgorithm::Prague; }

    void on_acked(const CongestionPacket& packet, u64 now_ns) noexcept override
    {
        m_round_acked = saturating_add(m_round_acked, packet.bytes);
        m_round_self_limited = saturating_add(m_round_self_limited, packet.delivery.app_limited ? packet.bytes : 0U);
        WindowController::on_acked(packet, now_ns);
    }

    void on_ack_end(const QuicRtt& rtt, bool sampled, u64 in_flight, u64 now_ns) noexcept override
    {
        m_srtt_ns = rtt.smoothed_ns;
        m_idle_ns = rtt.probe_base_ns();
        if (sampled)
        {
            m_detector.on_rtt(rtt.latest_ns, segments());
        }
        WindowController::on_ack_end(rtt, sampled, in_flight, now_ns);
        m_frame_ce_bytes = 0U;
    }

    // Sections 2.3.2 and 2.4.2: every frame's counts feed alpha; the first CE outside CWR takes the window down by
    // alpha / 2 (by more toward a classic bottleneck), and CWR lasts a round trip, a virtual one once RTT-independent.
    void on_ecn(u64 marked, u64 ce, u64 newest_sent_ns, u64 now_ns) noexcept override
    {
        m_round_marked = saturating_add(m_round_marked, marked);
        m_round_ce = saturating_add(m_round_ce, ce);
        if (ce == 0U)
        {
            return;
        }
        m_frame_ce_bytes = saturating_add(m_frame_ce_bytes, min_u64(ce, kCongestionMaxWindow) * m_datagram);
        m_saw_ce = true;
        m_detector.on_ce();
        no_undo(); // a mark is no spurious loss
        if (m_cwr_start_ns != kNeverNs &&
            (newest_sent_ns <= m_cwr_start_ns || now_ns - min_u64(m_cwr_start_ns, now_ns) < cwr_ns()))
        {
            return;
        }
        m_cwr_start_ns = now_ns;
        m_cwr_prior = m_window;
        const u64 alpha = max_u64(m_alpha_scaled >> kPragueGainShift, classic_alpha());
        reduce_to(m_window - multiply_divide(m_window, alpha, 2U * u64{kCongestionAlphaOne}));
    }

    // Section 2.3.1.2: without ECN feedback Prague is a classic (Reno) controller.
    void on_ecn_unavailable() noexcept override { m_ecn_unavailable = true; }

    [[nodiscard]] u64 pacing_rate(const QuicRtt& rtt) const noexcept override { return rate(rtt.smoothed_ns); }

    [[nodiscard]] u64 burst() const noexcept override
    {
        if (m_srtt_ns == 0U)
        {
            return WindowController::burst(); // the initial window, before a round trip is known (section 3.1.1)
        }
        return max_u64(multiply_divide(rate(m_srtt_ns), kPragueBurstNs, kNanosPerSecond), m_datagram);
    }

    [[nodiscard]] CongestionState state() const noexcept override
    {
        CongestionState out = WindowController::state();
        out.alpha = static_cast<u32>(m_alpha_scaled >> kPragueGainShift);
        out.classic = m_detector.classic();
        return out;
    }

protected:
    // Section 2.4.3: unmarked bytes alone grow the window, by (srtt / rtt_virt)^2 of Reno's per round trip once
    // RTT-independent (section 2.4.4); Reno's without ECN.
    void avoid(u64 acked, const QuicRtt& rtt, u64 /*now_ns*/) noexcept override
    {
        const u64 marked = min_u64(acked, m_frame_ce_bytes);
        m_frame_ce_bytes -= marked;
        u64 increase = (acked - marked) * m_datagram;
        if (rtt_independent() && rtt.smoothed_ns < kPragueRttVirtMinNs)
        {
            increase = multiply_divide(multiply_divide(increase, rtt.smoothed_ns, kPragueRttVirtMinNs), rtt.smoothed_ns,
                                       kPragueRttVirtMinNs);
        }
        const u64 total = increase + m_carry;
        add_window(total / m_window);
        m_carry = total % m_window;
    }

    // Section 2.4.1: Reno's half on loss; within a round trip of a mark's reduction, half the window before it, so
    // the two together reduce as Reno's one does.
    void reduce(u64 now_ns) noexcept override
    {
        m_prior_carry = m_carry;
        const bool recent =
            m_cwr_start_ns != kNeverNs && now_ns - min_u64(m_cwr_start_ns, now_ns) < max_u64(m_srtt_ns, cwr_ns());
        const u64 base = recent ? max_u64(m_window, m_cwr_prior) : m_window;
        m_threshold = min_u64(m_window, base / 2U);
        m_window = m_threshold;
        m_carry = 0U;
    }

    void restore() noexcept override { m_carry = m_prior_carry; }
    void collapse() noexcept override { m_carry = 0U; }

    void start_over() noexcept override
    {
        m_detector = ClassicEcnDetector{};
        m_alpha_scaled = kPragueAlphaScaledOne;
        m_alpha_ns = kNeverNs;
        m_cwr_start_ns = kNeverNs;
        m_cwr_prior = 0U;
        m_rounds = 0U;
        m_round_marked = 0U;
        m_round_ce = 0U;
        m_round_acked = 0U;
        m_round_self_limited = 0U;
        m_frame_ce_bytes = 0U;
        m_carry = 0U;
        m_last_sent_ns = kNeverNs;
        m_saw_ce = false;
    }

    // TR-BB-2019-002 section 3.2: each idle timeout halves a classic score.
    void sent(const CongestionPacket& packet, u64 in_flight, u64 now_ns) noexcept override
    {
        if (in_flight == packet.bytes && m_last_sent_ns != kNeverNs && m_idle_ns != 0U && now_ns > m_last_sent_ns)
        {
            m_detector.on_idle((now_ns - m_last_sent_ns) / m_idle_ns);
        }
        m_last_sent_ns = now_ns;
    }

    // Section 2.3.2: from the first CE (alpha is 1 until then), alpha moves once a (virtual) round trip by g toward
    // the fraction of marked packets acknowledged with CE since its last move; the detector takes the round's queue
    // and self-limited share.
    void rounded(const QuicRtt& rtt, u64 now_ns) noexcept override
    {
        m_rounds = saturating_add(m_rounds, 1U);
        const u64 share =
            (m_round_acked == 0U) ? 0U : multiply_divide(m_round_self_limited, u64{1} << 16U, m_round_acked);
        m_detector.on_round(rtt.min_ns, static_cast<u32>(share));
        m_round_acked = 0U;
        m_round_self_limited = 0U;
        if (!m_saw_ce)
        {
            m_round_marked = 0U;
            m_round_ce = 0U;
            return;
        }
        if (m_round_marked == 0U || (m_alpha_ns != kNeverNs && now_ns - min_u64(m_alpha_ns, now_ns) < cwr_ns()))
        {
            return;
        }
        // A peer may report CE for more packets than the frames' marked ones (RFC 9000 section 13.4.2.1): at most 1.
        const u64 fraction = multiply_divide(min_u64(m_round_ce, m_round_marked), kCongestionAlphaOne, m_round_marked);
        m_alpha_scaled = m_alpha_scaled - (m_alpha_scaled >> kPragueGainShift) + fraction;
        m_alpha_ns = now_ns;
        m_round_marked = 0U;
        m_round_ce = 0U;
    }

private:
    [[nodiscard]] bool rtt_independent() const noexcept
    {
        return !m_ecn_unavailable && m_rounds >= kPragueRttTransitionRounds;
    }

    // CWR's and alpha's period beyond the round trip itself: the virtual round trip once RTT-independent.
    [[nodiscard]] u64 cwr_ns() const noexcept
    {
        return rtt_independent() ? max_u64(m_srtt_ns, kPragueRttVirtMinNs) : 0U;
    }

    // ABE's alpha scaled by how classic the bottleneck looks, in 2^-20.
    [[nodiscard]] u64 classic_alpha() const noexcept
    {
        return (u64{m_detector.classic()} << 4U) * kPragueAbeAlphaNumerator / kPragueAbeAlphaDenominator;
    }

    // Section 2.5.1: a window per smoothed round trip, doubled while under half the slow start threshold.
    [[nodiscard]] u64 rate(u64 srtt_ns) const noexcept
    {
        if (!m_policy.pacing || srtt_ns == 0U)
        {
            return 0U;
        }
        const u64 gain = (m_window < m_threshold / 2U) ? kPragueSlowStartPacingGain : kPercent;
        return m_window * gain * (kNanosPerSecond / kPercent) / srtt_ns;
    }

    // The slow start threshold (the window, before one is set) in datagrams, for the detector's gains.
    [[nodiscard]] u64 segments() const noexcept
    {
        return ((m_threshold < kCongestionMaxWindow) ? m_threshold : m_window) / m_datagram;
    }

    ClassicEcnDetector m_detector;
    u64 m_alpha_scaled = kPragueAlphaScaledOne; // alpha / g in 2^-20 (section 2.3.2): alpha starts at 1
    u64 m_alpha_ns = kNeverNs;                  // alpha's last move
    u64 m_cwr_start_ns = kNeverNs;              // the last reduction by marks
    u64 m_cwr_prior = 0U;                       // the window before it
    u64 m_rounds = 0U;
    u64 m_round_marked = 0U;       // packets acknowledged ECN-capable since alpha's last move
    u64 m_round_ce = 0U;           // of those, CE
    u64 m_round_acked = 0U;        // bytes acknowledged this round
    u64 m_round_self_limited = 0U; // of those, sent application limited
    u64 m_frame_ce_bytes = 0U;     // the ACK frame's marked bytes, which do not grow the window
    u64 m_carry = 0U;
    u64 m_prior_carry = 0U;
    u64 m_srtt_ns = 0U;
    u64 m_idle_ns = 0U;
    u64 m_last_sent_ns = kNeverNs;
    bool m_ecn_unavailable = false;
    bool m_saw_ce = false; // alpha moves from the first CE on
};

static_assert(sizeof(NewReno) <= kCongestionSlotBytes && alignof(NewReno) <= alignof(CongestionSlot));
static_assert(sizeof(Cubic) <= kCongestionSlotBytes && alignof(Cubic) <= alignof(CongestionSlot));
static_assert(sizeof(Bbr) <= kCongestionSlotBytes && alignof(Bbr) <= alignof(CongestionSlot));
static_assert(sizeof(Prague) <= kCongestionSlotBytes && alignof(Prague) <= alignof(CongestionSlot));

[[nodiscard]] i64 scaled_bytes(u64 bytes) noexcept
{
    return static_cast<i64>(min_u64(bytes, kCongestionMaxWindow) * kNanosPerSecond);
}

} // namespace

bool congestion_policy_valid(const CongestionPolicy& policy) noexcept
{
    const bool window =
        policy.algorithm == CongestionAlgorithm::NewReno || policy.algorithm == CongestionAlgorithm::Cubic;
    // BBR is version 2's and paces by its model: unpaced, its Startup would send each window at once.
    const bool bbr = policy.algorithm == CongestionAlgorithm::Bbr && policy.version >= 2U && policy.pacing;
    // Prague is version 3's and must pace (the draft's section 2.5.1).
    const bool prague = policy.algorithm == CongestionAlgorithm::Prague && policy.version >= 3U && policy.pacing;
    return policy.version >= 1U && policy.version <= kCongestionInterfaceVersion && (window || bbr || prague) &&
           policy.minimum_packets >= 2U &&
           policy.initial_packets >= policy.minimum_packets && policy.initial_packets <= kCongestionMaxInitialPackets &&
           policy.pacing_gain >= kCongestionMinPacingGain && policy.pacing_gain <= kCongestionMaxPacingGain;
}

CongestionController* make_congestion(const CongestionPolicy& policy, u64 max_datagram, const CongestionState* seed,
                                      CongestionSlot& slot, DeliveryRateSampler& delivery, u64 entropy) noexcept
{
    if (!congestion_policy_valid(policy) || max_datagram < kQuicMinInitialDatagram || max_datagram > kMaxDatagram)
    {
        return nullptr;
    }
    if (policy.algorithm == CongestionAlgorithm::Bbr)
    {
        Bbr* const bbr = new (slot.bytes) Bbr(policy, max_datagram, delivery, entropy);
        if (seed != nullptr)
        {
            bbr->seed(*seed);
        }
        return bbr;
    }
    WindowController* made = nullptr;
    if (policy.algorithm == CongestionAlgorithm::NewReno)
    {
        made = new (slot.bytes) NewReno(policy, max_datagram);
    }
    else if (policy.algorithm == CongestionAlgorithm::Prague)
    {
        made = new (slot.bytes) Prague(policy, max_datagram);
    }
    else
    {
        made = new (slot.bytes) Cubic(policy, max_datagram);
    }
    if (seed != nullptr)
    {
        made->seed(*seed);
    }
    return made;
}

// ---- Pacer ----

i64 Pacer::credit_scaled(u64 now_ns, u64 rate, u64 burst) const noexcept
{
    const i64 full = scaled_bytes(burst);
    if (rate == 0U || !m_started)
    {
        return full;
    }
    const i64 held = (m_credit < full) ? m_credit : full;
    const u64 room = static_cast<u64>(full - held); // at most twice the burst: debt is bounded by it
    const u64 elapsed = (now_ns > m_stamp_ns) ? now_ns - m_stamp_ns : 0U;
    const WideProduct earned = multiply_wide(elapsed, rate);
    if (earned.hi != 0U || earned.lo >= room)
    {
        return full;
    }
    return held + static_cast<i64>(earned.lo);
}

i64 Pacer::credit(u64 now_ns, u64 rate, u64 burst) const noexcept
{
    return credit_scaled(now_ns, rate, burst) / static_cast<i64>(kNanosPerSecond);
}

u64 Pacer::release_ns(u64 now_ns, u64 rate, u64 burst) const noexcept
{
    const i64 held = credit_scaled(now_ns, rate, burst);
    if (rate == 0U || held > 0)
    {
        return now_ns;
    }
    // Credit turns positive once elapsed * rate passes the debt.
    const u64 wait = static_cast<u64>(-held) / rate + 1U;
    return (now_ns > kNeverNs - 1U - wait) ? kNeverNs - 1U : now_ns + wait;
}

void Pacer::on_sent(u64 bytes, u64 now_ns, u64 rate, u64 burst) noexcept
{
    if (rate == 0U)
    {
        m_started = false; // unpaced: the burst is whole once pacing starts
        return;
    }
    const i64 full = scaled_bytes(burst);
    const i64 left = credit_scaled(now_ns, rate, burst) - scaled_bytes(bytes);
    m_credit = (left < -full) ? -full : left;
    m_stamp_ns = max_u64(m_stamp_ns, now_ns);
    m_started = true;
}

} // namespace crd::net
