// crd-net -- congestion control and pacing (NET-PATH; contract: docs/design/network-foundation.md#congestion, #bbr,
// #prague, #scream and #gcc).
#include <crd/net/congestion.hpp>

#include <crd/core/wide_multiply.hpp>
#include <crd/hesap/net/classic_ecn.hpp>
#include <crd/hesap/net/delay_trend.hpp>
#include <crd/hesap/net/queue_delay.hpp>
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
        m_datagram = max_datagram;
        m_initial = congestion_initial_window(m_policy, max_datagram);
        m_minimum = m_policy.minimum_packets * max_datagram;
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
        m_datagram = max_datagram;
        m_minimum = m_policy.minimum_packets * max_datagram;
        m_min_pipe = max_u64(kBbrMinPipePackets * max_datagram, m_minimum);
        m_initial = congestion_initial_window(m_policy, max_datagram);
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

// ---- SCReAM v2 (draft-ietf-ccwg-rfc8298bis-screamv2-03) ----

using hesap::net::BaseDelayHistory;
using hesap::net::QueueDelayHistory;

// Fractions are kept in 2^-16 (kQ16 is 1) unless named otherwise; the draft's constants follow it, rounded.
constexpr u64 kQ16 = u64{1} << 16U;
constexpr u64 kScreamMinRefWnd = 3000U;             // MIN_REF_WND
constexpr u64 kScreamVirtualRttNs = 25000000U;      // VIRTUAL_RTT
constexpr u64 kScreamBetaLoss = 45875U;             // BETA_LOSS, 0.7
constexpr u64 kScreamBetaEcn = 52429U;              // BETA_ECN, 0.8
constexpr u64 kScreamMulIncrease = 1311U;           // MUL_INCREASE_FACTOR, 0.02
constexpr u64 kScreamPostCongestionRtts = 100U;     // POST_CONGESTION_DELAY_RTTS
constexpr u64 kScreamHoldRtts = 50U;                // REF_WND_I_UPDATE_MAX_HOLD_RTTS
constexpr u64 kScreamRestoreRtts = 100U;            // section 4.2.2.1: uncongested this long, ref_wnd meets the flight
constexpr u64 kScreamHeadroomPercent = 150U;        // BYTES_IN_FLIGHT_HEAD_ROOM
constexpr u64 kScreamInflectionFloor = 13107U;      // 0.2: scl_t's floor near ref_wnd_i
constexpr u64 kScreamAlphaShift = 4U;               // L4S_AVG_G, 1/16
constexpr u64 kScreamAlphaPeriodNs = 10000000U;     // l4s_alpha moves at least every 10 ms
constexpr u64 kScreamQueueAvgShift = 2U;            // QDELAY_AVG_G, 1/4
constexpr i64 kScreamShortAvgDivisor = 5;           // QDELAY_SHORT_AVG_G, 1/5
constexpr i64 kScreamLongAvgDivisor = 50;           // QDELAY_LONG_AVG_G, 1/50
constexpr i64 kScreamLatencyMarginNs = 1000000;     // LATENCY_DIFF_MARGIN_1
constexpr u64 kScreamLatencyLowNs = 10000000U;      // LATENCY_DIFF_MARGIN_2
constexpr i64 kScreamLatencyUpGain = 20;            // LATENCY_DIFF_UP_GAIN
constexpr i64 kScreamLatencyDownGain = 2;           // LATENCY_DIFF_DOWN_GAIN
constexpr u64 kScreamOverheadMin = 98304U;          // REF_WND_OVERHEAD_MIN, 1.5
constexpr u64 kScreamOverheadMax = 196608U;         // REF_WND_OVERHEAD_MAX, 3.0
constexpr u64 kScreamPacingHeadroomPercent = 150U;  // PACKET_PACING_HEADROOM
constexpr u64 kScreamRelaxedPacingFloor = 16384U;   // 1 / MAX_RELAXED_PACING_FACTOR
constexpr u64 kScreamRelaxedPacingLow = 52429U;     // RELAXED_PACING_LIMIT_LOW, 0.8
constexpr u64 kScreamPacketOverhead = 20U;          // PACKET_OVERHEAD
constexpr u64 kScreamRateMarginPercent = 120U;      // section 4.4's downscaling by 1.2
constexpr u64 kScreamAckedMarginPercent = 80U;      // ACKED_BITRATE_MARGIN
constexpr u64 kScreamLowTargetBackoffShift = 2U;    // BACKOFF_SCALE_LOW_TARGET_RATE, 1/4
constexpr u64 kScreamLossRateThreshold = 42949673U; // LOSS_RATE_THRESHOLD, 0.01 in 2^-32
constexpr u64 kScreamLossEventThreshold = 131U;     // section 4.5.4: 0.002 of round trips with a loss
constexpr u64 kScreamLossEventShift = 4U;           // loss_event_rate's gain per smoothed RTT, 1/16
constexpr u64 kScreamDriftShift = 8U;               // QDELAY_MIN_AVG_ALPHA, 1/256
constexpr u64 kScreamDriftHoldRtts = 5U;            // section 4.5.1: the target rate halved for max(5 s_rtt, 0.2 s)
constexpr u64 kScreamDriftHoldNs = 200000000U;
constexpr u64 kScreamNormShift = 8U;        // qdelay_norm in 2^-8 of QDELAY_TARGET_LO
constexpr u64 kScreamSafeVariance = 13107U; // section 4.5.4: a variance of qdelay_norm under 0.2, in 2^-16
constexpr u64 kScreamBurstNs = 1000000U;    // a millisecond of the pacing rate leaves at once

class Scream final : public CongestionController
{
public:
    Scream(const CongestionPolicy& policy, u64 max_datagram, DeliveryRateSampler& delivery) noexcept
        : m_policy(policy), m_delivery(delivery)
    {
        size(max_datagram);
        restart();
    }

    void seed(const CongestionState& from) noexcept
    {
        m_ref_wnd = min_u64(max_u64(from.window, m_floor), kCongestionMaxWindow);
        m_reaction_ns = from.recovery_start_ns;
        m_reductions = from.reductions;
        m_undone = from.undone;
        update_target(0U);
    }

    [[nodiscard]] CongestionAlgorithm algorithm() const noexcept override { return CongestionAlgorithm::Scream; }

    void on_sent(const CongestionPacket& /*packet*/, u64 in_flight, u64 /*now_ns*/) noexcept override
    {
        m_max_in_flight = max_u64(m_max_in_flight, in_flight);
    }

    void on_limited(bool /*window*/, u64 /*now_ns*/) noexcept override {}

    // Section 4.1.2's bytes_newly_acked, and section 4.5.2's loss rate decaying with every delivered packet.
    void on_acked(const CongestionPacket& packet, u64 /*now_ns*/) noexcept override
    {
        m_acked = saturating_add(m_acked, packet.bytes);
        ++m_frame_packets;
        m_loss_rate -= multiply_divide(m_loss_rate, loss_gain(), kQ16);
    }

    void on_ack_end(const QuicRtt& rtt, bool sampled, u64 in_flight, u64 now_ns) noexcept override
    {
        m_srtt_ns = max_u64(rtt.smoothed_ns, 1U);
        m_sampled = m_sampled || sampled;
        if (m_delivery.sample().acked && m_delivery.sample().rate != 0U)
        {
            m_acked_rate = m_delivery.sample().rate;
        }
        if (sampled)
        {
            queue_sample(rtt.adjusted_ns, now_ns);
        }
        tick(in_flight, now_ns);
        update_alpha(now_ns);
        const u64 inflection = inflection_scale();
        react(inflection, in_flight, now_ns);
        grow(inflection, now_ns);
        update_target(now_ns);
        m_acked = 0U;
        m_acked_ce = 0U;
        m_frame_packets = 0U;
    }

    // Sections 4.2.1.1 and 4.5.2: a loss of a packet sent since the last reaction is a congestion signal.
    void on_lost(const CongestionPacket& packet, u64 /*now_ns*/) noexcept override
    {
        const u64 gain = loss_gain();
        m_loss_rate = m_loss_rate - multiply_divide(m_loss_rate, gain, kQ16) + (gain << 16U);
        m_round_lost = true;
        if (m_reaction_ns == kNeverNs || packet.sent_ns > m_reaction_ns)
        {
            m_lost = true;
        }
    }

    // Sections 4.2.1.2 and 4.2.1.3: marks feed l4s_alpha; CE on a packet sent since the last reaction is a signal,
    // and the frame's CE bytes, at its mean packet size, grow nothing.
    void on_ecn(u64 marked, u64 ce, u64 newest_sent_ns, u64 /*now_ns*/) noexcept override
    {
        m_units_delivered = saturating_add(m_units_delivered, marked);
        m_units_marked = saturating_add(m_units_marked, ce);
        if (ce == 0U)
        {
            return;
        }
        const u64 unit = (m_frame_packets != 0U) ? m_acked / m_frame_packets : m_datagram;
        const WideProduct bytes = multiply_wide(ce, unit);
        m_acked_ce = min_u64(m_acked, saturating_add(m_acked_ce, (bytes.hi != 0U) ? kInfinite : bytes.lo));
        if (m_reaction_ns == kNeverNs || newest_sent_ns > m_reaction_ns)
        {
            m_marked = true;
        }
    }

    void on_ecn_unavailable() noexcept override {}

    void on_persistent_congestion(u64 now_ns) noexcept override
    {
        m_ref_wnd = m_floor;
        m_carry = 0U;
        m_reaction_ns = now_ns;
        m_detected_ns = now_ns;
        ++m_reductions;
        update_target(now_ns);
    }

    // The draft keeps no undo: a reduction a spurious loss caused stands.
    void on_spurious(const CongestionPacket& /*packet*/, u64 /*lost_ns*/, u64 /*now_ns*/) noexcept override {}

    void reset(u64 /*now_ns*/) noexcept override { restart(); }

    void set_max_datagram(u64 bytes) noexcept override
    {
        const bool initial = m_ref_wnd == m_initial;
        size(min_u64(max_u64(bytes, kQuicMinInitialDatagram), kMaxDatagram));
        m_ref_wnd = initial ? m_initial : max_u64(m_ref_wnd, m_floor);
        update_target(0U);
    }

    // Section 4.3.1: bytes in flight may reach ref_wnd times the overhead, at least two datagrams.
    [[nodiscard]] u64 window() const noexcept override
    {
        const u64 overhead =
            kScreamOverheadMin + multiply_divide(kScreamOverheadMax - kScreamOverheadMin, steadiness(), kQ16);
        return min_u64(max_u64(multiply_divide(m_ref_wnd, overhead, kQ16), 2U * m_datagram), kCongestionMaxWindow);
    }

    // Section 4.3.2: 1.5 times the target rate, at least RATE_PACE_MIN, relaxed up to four times as the target nears
    // the policy's maximum.
    [[nodiscard]] u64 pacing_rate(const QuicRtt& /*rtt*/) const noexcept override
    {
        const u64 pace =
            multiply_divide(max_u64(m_target, kScreamMinPacingRate), kScreamPacingHeadroomPercent, kPercent);
        const u64 most = m_policy.target_rate_max;
        const u64 nominal = (most != 0U) ? multiply_divide(m_target, kQ16, most) : 0U;
        if (nominal <= kScreamRelaxedPacingLow)
        {
            return pace;
        }
        const u64 relaxed =
            min_u64(multiply_divide(nominal - kScreamRelaxedPacingLow, kQ16, kQ16 - kScreamRelaxedPacingLow), kQ16);
        return multiply_divide(pace, kQ16, max_u64(kQ16 - relaxed, kScreamRelaxedPacingFloor));
    }

    [[nodiscard]] u64 burst() const noexcept override
    {
        if (!m_sampled)
        {
            return m_initial;
        }
        return max_u64(multiply_divide(pacing_rate(QuicRtt{}), kScreamBurstNs, kNanosPerSecond), 2U * m_datagram);
    }

    [[nodiscard]] CongestionState state() const noexcept override
    {
        CongestionState out;
        out.window = m_ref_wnd;
        out.slow_start_threshold = m_ref_wnd; // no slow start: a controller swapped in avoids congestion from here
        out.recovery_start_ns = m_reaction_ns;
        out.reductions = m_reductions;
        out.undone = m_undone;
        out.alpha = static_cast<u32>(m_alpha);
        out.target_rate = m_target;
        out.queue_delay_ns = m_queue_avg_ns;
        out.queue_delay_target_ns = m_target_delay_ns;
        out.phase = CongestionPhase::Avoidance;
        return out;
    }

private:
    void size(u64 max_datagram) noexcept
    {
        m_datagram = max_datagram;
        m_floor = max_u64(kScreamMinRefWnd, m_policy.minimum_packets * max_datagram);
        m_initial = max_u64(congestion_initial_window(m_policy, max_datagram), m_floor);
    }

    void restart() noexcept
    {
        m_ref_wnd = m_initial;
        m_ref_wnd_i = 1U;
        m_ref_wnd_lowest = 0U;
        m_inflection_ns = kNeverNs;
        m_inflection_allowed = true;
        m_carry = 0U;
        m_acked = 0U;
        m_acked_ce = 0U;
        m_frame_packets = 0U;
        m_acked_rate = 0U;
        m_max_in_flight = 0U;
        m_max_in_flight_prev = 0U;
        m_in_flight_prev = 0U;
        m_tick_ns = kNeverNs;
        m_units_delivered = 0U;
        m_units_marked = 0U;
        m_alpha = 0U;
        m_alpha_ns = kNeverNs;
        m_base.reset();
        m_history.reset();
        m_queue_ns = 0U;
        m_queue_avg_ns = 0U;
        m_queue_avg_prev_ns = 0U;
        m_queue_update_ns = kNeverNs;
        m_queue_min_ns = kNeverNs;
        m_queue_min_avg_ns = 0U;
        m_short_avg_ns = 0;
        m_long_avg_ns = 0;
        m_latency_diff = 0U;
        m_target_delay_ns = m_policy.queue_delay_target_ns;
        m_loss_rate = 0U;
        m_loss_events = 0U;
        m_round_lost = false;
        m_lost = false;
        m_marked = false;
        m_congested = false;
        m_detected_ns = kNeverNs;
        m_reaction_ns = kNeverNs;
        m_hold_until_ns = 0U;
        m_srtt_ns = kQuicInitialRttNs;
        m_sampled = false;
        update_target(0U);
    }

    // 1 - latency_diff_avg: 1 while the queue delay is steady, toward 0 as it varies (REDUCE_JITTER).
    [[nodiscard]] u64 steadiness() const noexcept { return kQ16 - m_latency_diff; }

    // Section 4.5.2's EWMA gain: min(LOSS_RATE_THRESHOLD / 4, MSS / ref_wnd / 2), in 2^-16.
    [[nodiscard]] u64 loss_gain() const noexcept
    {
        return min_u64((kScreamLossRateThreshold >> 16U) / 4U, multiply_divide(m_datagram, kQ16, 2U * m_ref_wnd));
    }

    // Section 4.2.1.4: the queue delay is the sample less RFC 6817's base, averaged with a slow attack and fast decay
    // once each min(VIRTUAL_RTT, s_rtt).
    void queue_sample(u64 delay_ns, u64 now_ns) noexcept
    {
        m_base.update(delay_ns, now_ns);
        m_queue_ns = delay_ns - min_u64(delay_ns, m_base.base());
        m_queue_min_ns = min_u64(m_queue_min_ns, m_queue_ns);
        if (m_queue_update_ns != kNeverNs &&
            now_ns - min_u64(m_queue_update_ns, now_ns) < min_u64(kScreamVirtualRttNs, m_srtt_ns))
        {
            return;
        }
        m_queue_update_ns = now_ns;
        m_queue_avg_ns = (m_queue_ns < m_queue_avg_ns)
                             ? m_queue_ns
                             : m_queue_avg_ns + ((m_queue_ns - m_queue_avg_ns) >> kScreamQueueAvgShift);
        if (m_policy.reduce_jitter)
        {
            update_latency_diff();
        }
        if (m_policy.compete)
        {
            adjust_target_delay();
        }
    }

    // Section 4.2.1.4's calculate_latency_diff_avg().
    void update_latency_diff() noexcept
    {
        const auto queue = static_cast<i64>(min_u64(m_queue_ns, kCongestionMaxWindow));
        m_short_avg_ns += (queue - m_short_avg_ns) / kScreamShortAvgDivisor;
        m_long_avg_ns += (queue - m_long_avg_ns) / kScreamLongAvgDivisor;
        if (!m_congested)
        {
            m_long_avg_ns = m_short_avg_ns;
            return;
        }
        i64 diff = m_short_avg_ns - m_long_avg_ns - kScreamLatencyMarginNs;
        diff = (diff < -kScreamLatencyMarginNs) ? -kScreamLatencyMarginNs : diff;
        diff = (diff > kScreamLatencyMarginNs) ? kScreamLatencyMarginNs : diff;
        auto latency = static_cast<i64>(m_latency_diff);
        if (diff > 0)
        {
            const u64 low = min_u64(multiply_divide(m_queue_avg_ns, kQ16, kScreamLatencyLowNs), kQ16);
            latency += kScreamLatencyUpGain * diff * static_cast<i64>(low) / static_cast<i64>(kNanosPerSecond);
        }
        else
        {
            latency += kScreamLatencyDownGain * diff * static_cast<i64>(kQ16) / static_cast<i64>(kNanosPerSecond);
        }
        latency = (latency < 0) ? 0 : latency;
        m_latency_diff = min_u64(static_cast<u64>(latency), kQ16);
    }

    // Section 4.5.4's adjust_qdelay_target(): the target rises toward the queue a competing loss-based flow holds.
    void adjust_target_delay() noexcept
    {
        const u64 low = m_policy.queue_delay_target_ns;
        const u64 norm = min_u64(multiply_divide(m_queue_ns, u64{1} << kScreamNormShift, low), 0xFFFFU);
        m_history.push(static_cast<u16>(norm));
        const u64 variance = m_history.variance();
        const u64 scaled = m_history.mean() + hesap::net::square_root(variance);
        const u64 candidate = multiply_divide(scaled, low, u64{1} << kScreamNormShift);
        u64 target = m_target_delay_ns;
        if (m_loss_events > kScreamLossEventThreshold)
        {
            target = candidate + candidate / 2U;
        }
        else if (variance < kScreamSafeVariance)
        {
            target = candidate;
        }
        else if (candidate < low)
        {
            target = max_u64(target / 2U, candidate);
        }
        else
        {
            target = target * 9U / 10U;
        }
        m_target_delay_ns = min_u64(max_u64(target, low), max_u64(kScreamQueueDelayTargetMaxNs, low));
    }

    // Once a smoothed RTT: the round's largest flight and its losses move to the previous round's, and section
    // 4.5.1's long average of the round's least queue delay, past a quarter of the target, takes a new base delay
    // with the target rate halved meanwhile.
    void tick(u64 in_flight, u64 now_ns) noexcept
    {
        if (m_tick_ns != kNeverNs && now_ns - min_u64(m_tick_ns, now_ns) < m_srtt_ns)
        {
            return;
        }
        const bool first = m_tick_ns == kNeverNs;
        m_tick_ns = now_ns;
        m_max_in_flight_prev = m_max_in_flight;
        m_max_in_flight = in_flight;
        m_in_flight_prev = in_flight;
        m_loss_events = m_loss_events - (m_loss_events >> kScreamLossEventShift) +
                        (m_round_lost ? (kQ16 >> kScreamLossEventShift) : 0U);
        m_round_lost = false;
        if (first || m_queue_min_ns == kNeverNs)
        {
            return;
        }
        m_queue_min_avg_ns =
            m_queue_min_avg_ns - (m_queue_min_avg_ns >> kScreamDriftShift) + (m_queue_min_ns >> kScreamDriftShift);
        m_queue_min_ns = kNeverNs;
        if (m_queue_min_avg_ns > m_target_delay_ns / 4U)
        {
            m_queue_min_avg_ns = 0U;
            m_base.reset();
            m_hold_until_ns = now_ns + max_u64(kScreamDriftHoldRtts * m_srtt_ns, kScreamDriftHoldNs);
        }
    }

    // Section 4.2.1.3: l4s_alpha moves by 1/16 toward the fraction of marked packets at least each min(10 ms, s_rtt).
    void update_alpha(u64 now_ns) noexcept
    {
        if (m_units_delivered == 0U ||
            (m_alpha_ns != kNeverNs && now_ns - min_u64(m_alpha_ns, now_ns) < min_u64(kScreamAlphaPeriodNs, m_srtt_ns)))
        {
            return;
        }
        const u64 fraction =
            multiply_divide(min_u64(m_units_marked, m_units_delivered), kCongestionAlphaOne, m_units_delivered);
        m_alpha = m_alpha - (m_alpha >> kScreamAlphaShift) + (fraction >> kScreamAlphaShift);
        m_alpha_ns = now_ns;
        m_units_delivered = 0U;
        m_units_marked = 0U;
    }

    // Section 4.2.2.1's scl_t: growth and the L4S backoff shrink near the last known maximum, ref_wnd_i.
    [[nodiscard]] u64 inflection_scale() const noexcept
    {
        const u64 gap = (m_ref_wnd > m_ref_wnd_i) ? m_ref_wnd - m_ref_wnd_i : m_ref_wnd_i - m_ref_wnd;
        const u64 x = min_u64(multiply_divide(gap, 8U * kQ16, m_ref_wnd_i), kQ16);
        u64 scale = max_u64(x * x / kQ16, kScreamInflectionFloor);
        if (m_ref_wnd > m_ref_wnd_i && scale < kQ16)
        {
            scale = max_u64(multiply_divide(scale, steadiness(), kQ16), kScreamInflectionFloor);
        }
        return scale;
    }

    // Section 4.5.3: a target already under the acknowledged rate needs no further cut while the flight shrinks.
    [[nodiscard]] u64 undershoot(u64 backoff, u64 in_flight) const noexcept
    {
        if (m_acked_rate == 0U || m_target >= multiply_divide(m_acked_rate, kScreamAckedMarginPercent, kPercent))
        {
            return backoff;
        }
        return (in_flight < m_in_flight_prev) ? 0U : backoff >> kScreamLowTargetBackoffShift;
    }

    // Section 4.2.2.1: at most once a smoothed RTT, a loss, CE or a queue past half the target reduces ref_wnd.
    void react(u64 inflection, u64 in_flight, u64 now_ns) noexcept
    {
        if (m_lost || m_marked)
        {
            m_detected_ns = now_ns;
        }
        bool loss = false;
        bool ce = false;
        u64 virtual_alpha = 0U;
        const u64 half = m_target_delay_ns / 2U;
        if (m_reaction_ns == kNeverNs || now_ns - min_u64(m_reaction_ns, now_ns) >= m_srtt_ns)
        {
            if (m_lost)
            {
                // Section 4.5.2: tolerating link losses, a loss reduces past a 1% rate or with a queue building.
                loss = !m_policy.tolerate_link_loss || m_loss_rate > kScreamLossRateThreshold ||
                       m_queue_avg_ns > m_target_delay_ns / 4U;
            }
            else if (m_marked)
            {
                ce = true;
            }
            else if (m_queue_avg_ns > half && m_queue_avg_ns >= m_queue_avg_prev_ns)
            {
                virtual_alpha = min_u64(multiply_divide(m_queue_avg_ns - half, kQ16, half), kQ16);
                m_detected_ns = now_ns;
            }
            m_lost = false;
            m_marked = false;
        }
        m_queue_avg_prev_ns = m_queue_avg_ns;
        if (!loss && !ce && virtual_alpha == 0U)
        {
            return;
        }
        m_congested = true;
        if (m_ref_wnd > max_u64(m_ref_wnd_lowest + m_datagram, m_ref_wnd_lowest + m_ref_wnd_lowest / 20U) ||
            m_inflection_ns == kNeverNs || now_ns - min_u64(m_inflection_ns, now_ns) > kScreamHoldRtts * m_srtt_ns)
        {
            m_inflection_allowed = true;
        }
        if (m_inflection_allowed)
        {
            m_ref_wnd_i = m_ref_wnd;
            m_inflection_allowed = false;
            m_inflection_ns = now_ns;
        }
        u64 window = m_ref_wnd;
        if (loss)
        {
            window = multiply_divide(window, kScreamBetaLoss, kQ16);
        }
        if (ce && !m_policy.scalable_ecn)
        {
            window = multiply_divide(window, kScreamBetaEcn, kQ16);
        }
        else if (ce)
        {
            u64 backoff = (m_alpha >> 4U) / 2U; // l4s_alpha / 2, in 2^-16
            if (m_queue_avg_ns < m_target_delay_ns / 4U)
            {
                backoff = multiply_divide(backoff, inflection, kQ16);
            }
            const bool long_quiet =
                m_reaction_ns == kNeverNs || now_ns - min_u64(m_reaction_ns, now_ns) > kScreamRestoreRtts * m_srtt_ns;
            if (long_quiet && m_max_in_flight_prev != 0U)
            {
                window = min_u64(window, m_max_in_flight_prev);
            }
            window -= multiply_divide(window, undershoot(backoff, in_flight), kQ16);
        }
        if (virtual_alpha != 0U)
        {
            window -= multiply_divide(window, undershoot(virtual_alpha / 2U, in_flight), kQ16);
        }
        m_ref_wnd = max_u64(window, m_floor);
        m_ref_wnd_lowest = m_ref_wnd;
        m_reaction_ns = now_ns;
        m_carry = 0U;
        ++m_reductions;
    }

    // Section 4.2.2.2: unmarked acknowledged bytes grow ref_wnd by about a datagram a round trip, scaled down near
    // ref_wnd_i and under VIRTUAL_RTT, multiplicatively once congestion is POST_CONGESTION_DELAY_RTTS past, and
    // never beyond 1.5 times the largest recent flight or once the target rate reached its maximum.
    void grow(u64 inflection, u64 now_ns) noexcept
    {
        const u64 plain = m_acked - min_u64(m_acked_ce, m_acked);
        if (plain == 0U)
        {
            return;
        }
        const u64 steady = steadiness();
        u64 post = kQ16;
        if (m_detected_ns != kNeverNs)
        {
            const u64 span = kScreamPostCongestionRtts * max_u64(kScreamVirtualRttNs, m_srtt_ns);
            post = min_u64(multiply_divide(now_ns - min_u64(m_detected_ns, now_ns), kQ16, span), kQ16);
        }
        post = multiply_divide(post, steady, kQ16);
        const u64 factor = kQ16 + multiply_divide(kScreamMulIncrease, m_ref_wnd, m_datagram);
        u64 increment = multiply_divide(plain << 16U, min_u64(m_datagram, m_ref_wnd), m_ref_wnd);
        increment = multiply_divide(increment, kQ16, 4U * kQ16 - 3U * steady);
        increment = multiply_divide(increment, min_u64(m_srtt_ns, kScreamVirtualRttNs), kScreamVirtualRttNs);
        increment = multiply_divide(increment, inflection, kQ16);
        increment = multiply_divide(increment, kQ16 + multiply_divide(factor - kQ16, post, kQ16), kQ16);
        const u64 total = saturating_add(increment, m_carry);
        const u64 grown = saturating_add(m_ref_wnd, total >> 16U);
        const u64 allowed = m_datagram + multiply_divide(max_u64(m_max_in_flight, m_max_in_flight_prev),
                                                         kScreamHeadroomPercent, kPercent);
        const bool at_most = m_policy.target_rate_max != 0U && m_target >= m_policy.target_rate_max;
        if (grown <= allowed && !at_most)
        {
            m_ref_wnd = min_u64(grown, kCongestionMaxWindow);
            m_carry = total & (kQ16 - 1U);
        }
    }

    // Section 4.4: the window per smoothed RTT, less for a window of few datagrams and the packet overhead, over 1.2,
    // halved while section 4.5.1's remedy holds, and within the policy's bounds. The media queue's own terms
    // (rate_adjust_factor, frame_size_dev) are the source's.
    void update_target(u64 now_ns) noexcept
    {
        const u64 ratio = min_u64(multiply_divide(m_datagram, kQ16, m_ref_wnd), kQ16);
        const u64 small = (ratio > kQ16 / 10U) ? min_u64(ratio - kQ16 / 10U, kQ16 / 5U) : 0U;
        u64 rate = multiply_divide(m_ref_wnd, kNanosPerSecond, m_srtt_ns);
        rate = multiply_divide(rate, kQ16 - small, kQ16);
        rate = multiply_divide(rate, m_datagram * kPercent,
                               (m_datagram + kScreamPacketOverhead) * kScreamRateMarginPercent);
        if (now_ns < m_hold_until_ns)
        {
            rate /= 2U;
        }
        const u64 most = (m_policy.target_rate_max != 0U) ? m_policy.target_rate_max : ~u64{0};
        m_target = min_u64(max_u64(rate, m_policy.target_rate_min), most);
    }

    CongestionPolicy m_policy;
    DeliveryRateSampler& m_delivery;
    u64 m_datagram = 0U;
    u64 m_floor = 0U;
    u64 m_initial = 0U;
    u64 m_reductions = 0U;
    u64 m_undone = 0U;

    // The reference window (section 4.2.2) and its inflection point.
    u64 m_ref_wnd = 0U;
    u64 m_ref_wnd_i = 1U;
    u64 m_ref_wnd_lowest = 0U;
    u64 m_inflection_ns = kNeverNs;
    u64 m_carry = 0U; // growth below a byte, in 2^-16 bytes
    bool m_inflection_allowed = true;

    // Feedback (section 4.1).
    u64 m_acked = 0U;    // bytes the frame acknowledged
    u64 m_acked_ce = 0U; // of those, CE
    u64 m_frame_packets = 0U;
    u64 m_acked_rate = 0U; // bytes per second, the delivery rate's last sample
    u64 m_max_in_flight = 0U;
    u64 m_max_in_flight_prev = 0U;
    u64 m_in_flight_prev = 0U;
    u64 m_tick_ns = kNeverNs; // the smoothed RTT's last turn

    // L4S (section 4.2.1.3): alpha in 2^-20.
    u64 m_units_delivered = 0U;
    u64 m_units_marked = 0U;
    u64 m_alpha = 0U;
    u64 m_alpha_ns = kNeverNs;

    // Queue delay (sections 4.2.1.4, 4.5.1 and 4.5.4).
    BaseDelayHistory m_base;
    QueueDelayHistory m_history;
    u64 m_queue_ns = 0U;
    u64 m_queue_avg_ns = 0U;
    u64 m_queue_avg_prev_ns = 0U;
    u64 m_queue_update_ns = kNeverNs;
    u64 m_queue_min_ns = kNeverNs;
    u64 m_queue_min_avg_ns = 0U;
    i64 m_short_avg_ns = 0;
    i64 m_long_avg_ns = 0;
    u64 m_latency_diff = 0U; // latency_diff_avg, in 2^-16
    u64 m_target_delay_ns = 0U;
    u64 m_hold_until_ns = 0U; // section 4.5.1's halved target rate lasts until then

    // Congestion (sections 4.2.1, 4.5.2 and 4.5.4).
    u64 m_loss_rate = 0U;   // in 2^-32
    u64 m_loss_events = 0U; // the fraction of smoothed RTTs with a loss, in 2^-16
    u64 m_detected_ns = kNeverNs;
    u64 m_reaction_ns = kNeverNs;
    bool m_round_lost = false;
    bool m_lost = false;
    bool m_marked = false;
    bool m_congested = false; // is_congestion_detected

    u64 m_target = 0U; // bytes per second
    u64 m_srtt_ns = kQuicInitialRttNs;
    bool m_sampled = false;
};

// ---- GCC (draft-ietf-rmcat-gcc-02, as libwebrtc's send side runs it) ----

using hesap::net::AckedRateEstimator;
using hesap::net::DelayState;
using hesap::net::GroupDeltas;
using hesap::net::PacketGroupTracker;
using hesap::net::TrendlineFilter;
using hesap::net::TrendlineSample;
using hesap::net::WindowedMin;

// Rates are bytes per second; libwebrtc's constants in bits are divided by 8.
constexpr u64 kGccMinRate = 625U;                // 5 kbit/s, libwebrtc's least congestion controller rate
constexpr u64 kGccMaxRate = 12500000000U;        // 100 Gbit/s, the ceiling when the policy sets none
constexpr u64 kGccDefaultRttNs = 200000000U;     // the rate control's RTT before a sample (kDefaultRtt)
constexpr u64 kGccBetaPercent = 85U;             // section 5.5's beta
constexpr u64 kGccBackoffTerm = 625U;            // 5 kbit/s taken off a decrease above it
constexpr u64 kGccIncreaseFloor = 125U;          // 1 kbit/s, the least multiplicative step
constexpr u64 kGccLimitTerm = 1250U;             // 10 kbit/s over 1.5 times the throughput
constexpr u64 kGccLn108Q32 = 330545155U;         // ln(1.08) in 2^-32
constexpr u64 kGccResponseNs = 100000000U;       // the detector's reaction time added to the RTT
constexpr u64 kGccFramesPerSecond = 30U;         // the frame rate the additive step assumes
constexpr u64 kGccPacketBytes = 1200U;           // and the packet size
constexpr u64 kGccMinAdditiveRate = 500U;        // 4 kbit/s a second
constexpr u64 kGccReduceMinNs = 10000000U;       // a further reduction waits clamp(rtt, 10 ms, 200 ms)
constexpr u64 kGccReduceMaxNs = 200000000U;
constexpr u64 kGccDeviationMin = u64{50} << 16U; // the capacity's normalised variance, 0.4 to 2.5 kbit/s, in 2^-16
constexpr u64 kGccDeviationMax = u64{625} << 15U;
constexpr u64 kGccCapacityNorm = 125U;           // its least normaliser, 1 kbit/s
constexpr u64 kGccCapacityGain = 20U;            // its gain on overuse, 1/20
constexpr u64 kGccStreamTimeoutNs = 2000000000U; // feedback silent this long restarts the delay trend
constexpr u64 kGccLossPackets = 20U;             // a loss fraction needs this many packets (kLimitNumPackets)
constexpr u64 kGccLowLoss = 5U;                  // in 2^-8: at most 2% lost increases
constexpr u64 kGccHighLoss = 25U;                // in 2^-8: more than 10% lost decreases
constexpr u64 kGccLossIncreasePercent = 108U;
constexpr u64 kGccLossDecreaseNs = 300000000U;   // loss decreases stand at least this and an RTT apart
constexpr u64 kGccStartPhaseNs = 2000000000U;    // with no loss, the loss controller follows the delay's
constexpr u64 kGccHistoryNs = 1000000000U;       // the loss controller grows from this window's least rate
constexpr u64 kGccHistorySlackNs = 1000000U;     // and lets an entry go this much early
constexpr u64 kGccHistorySlotNs = 64000000U;     // its entries' time slots
constexpr usize kGccHistory = 17U;
constexpr u64 kGccQueueNs = 350000000U;          // the window lets the rate queue this long past the RTT
constexpr u64 kGccMinWindow = 3000U;
constexpr u64 kGccRttFrames = 32U;               // the window's RTT: the least of the last frames' largest
constexpr u64 kGccPacePercent = 110U;            // libwebrtc's send-side pacing factor
constexpr u64 kGccFirstPacePercent = 250U;       // and its factor before any feedback
constexpr u64 kGccBurstNs = 1000000U;            // a millisecond of the pacing rate leaves at once
constexpr u64 kQ32 = u64{1} << 32U;
constexpr u32 kGccTrendWindow = TrendlineFilter::kDefaultWindow;

// A window of the history meets at most this many slots.
static_assert(kGccHistory * kGccHistorySlotNs >= kGccHistoryNs + kGccHistorySlotNs);

// e^y - 1 for y in 2^-32 up to ln(1.08): four terms of the series, within 2^-31 of it.
[[nodiscard]] constexpr u64 exp_minus_one_q32(u64 y) noexcept
{
    const u64 y2 = (y * y) >> 32U;
    const u64 y3 = (y2 * y) >> 32U;
    const u64 y4 = (y3 * y) >> 32U;
    return y + y2 / 2U + y3 / 6U + y4 / 24U;
}

class Gcc final : public CongestionController
{
public:
    Gcc(const CongestionPolicy& policy, u64 max_datagram) noexcept : m_policy(policy)
    {
        m_max_rate = (policy.target_rate_max != 0U) ? policy.target_rate_max : kGccMaxRate;
        m_min_rate = min_u64(max_u64(policy.target_rate_min, kGccMinRate), m_max_rate); // a lower maximum wins
        size(max_datagram);
        restart();
    }

    // A live swap: the rate starts from the model's bandwidth or the source's target, else the window per the
    // initial RTT; the window and the last reduction carry over.
    void seed(const CongestionState& from) noexcept
    {
        u64 rate = from.bandwidth;
        if (rate == 0U)
        {
            rate = (from.target_rate != 0U) ? from.target_rate
                                            : multiply_divide(from.window, kNanosPerSecond, kQuicInitialRttNs);
        }
        start(rate);
        m_window = min_u64(max_u64(from.window, m_min_window), kCongestionMaxWindow);
        m_decrease_ns = from.recovery_start_ns;
        m_reductions = from.reductions;
        m_undone = from.undone;
    }

    [[nodiscard]] CongestionAlgorithm algorithm() const noexcept override { return CongestionAlgorithm::Gcc; }

    void on_sent(const CongestionPacket& /*packet*/, u64 /*in_flight*/, u64 /*now_ns*/) noexcept override {}

    void on_limited(bool /*window*/, u64 /*now_ns*/) noexcept override {}

    // The acknowledged rate counts the packet when its acknowledgement arrived (one clock); the delay trend takes its
    // peer receive time, and the frame's largest packet is kept for the frame's own sample when the peer gives none.
    void on_acked(const CongestionPacket& packet, u64 now_ns) noexcept override
    {
        ++m_frame_packets;
        m_frame_oldest_ns = min_u64(m_frame_oldest_ns, packet.sent_ns);
        m_acked.update(static_cast<i64>(min_u64(now_ns / 1000000U, AckedRateEstimator::kMaxTimeMs)),
                       static_cast<u32>(min_u64(packet.bytes, ~u32{0})), packet.delivery.app_limited);
        if (packet.received_ns != kNeverNs)
        {
            if (!m_timestamps)
            {
                m_timestamps = true; // the peer's clock from here: the trend of the frames' samples starts over
                restart_delay();
            }
            add_delay(packet.sent_ns, packet.received_ns, packet.bytes, now_ns);
        }
        if (!m_frame_acked || packet.number > m_largest_number)
        {
            m_largest_number = packet.number;
            m_largest_sent_ns = packet.sent_ns;
            m_largest_bytes = packet.bytes;
        }
        m_frame_acked = true;
    }

    // The feedback of one ACK frame: the delay trend's state through the rate control, the loss controller once the
    // packets make a fraction, the window from the rate; the frame's counts start over.
    void on_ack_end(const QuicRtt& rtt, bool sampled, u64 /*in_flight*/, u64 now_ns) noexcept override
    {
        m_rtt_ns = (rtt.smoothed_ns != 0U) ? rtt.smoothed_ns : kGccDefaultRttNs;
        m_sampled = m_sampled || sampled;
        if (!m_timestamps && sampled && m_frame_acked)
        {
            add_delay(m_largest_sent_ns, saturating_add(m_largest_sent_ns, rtt.adjusted_ns), m_largest_bytes, now_ns);
        }
        if (m_frame_acked)
        {
            update_delay_based(now_ns);
            m_feedback = true;
            if (m_frame_oldest_ns <= now_ns)
            {
                ++m_frames;
                m_rtt_min.update(now_ns - m_frame_oldest_ns, m_frames);
            }
        }
        update_packets_lost(now_ns);
        if (m_rtt_min.has_sample())
        {
            update_window();
        }
        m_frame_acked = false;
        m_frame_packets = 0U;
        m_frame_lost = 0U;
        m_frame_ce = 0U;
        m_frame_oldest_ns = kNeverNs;
    }

    void on_lost(const CongestionPacket& /*packet*/, u64 /*now_ns*/) noexcept override
    {
        m_frame_lost = saturating_add(m_frame_lost, 1U);
    }

    // RFC 3168 section 5: CE answers as a loss would; the marked packets were counted as acknowledged.
    void on_ecn(u64 /*marked*/, u64 ce, u64 /*newest_sent_ns*/, u64 /*now_ns*/) noexcept override
    {
        m_frame_ce = saturating_add(m_frame_ce, ce);
    }

    void on_ecn_unavailable() noexcept override {}

    // The rates and the window halve, as an overuse with no acknowledged rate does, and the link's capacity is
    // forgotten.
    void on_persistent_congestion(u64 now_ns) noexcept override
    {
        m_has_capacity = false;
        set_delay_rate(m_delay_rate / 2U, now_ns);
        m_delay_limit = m_delay_rate;
        set_target(m_target / 2U);
        m_window = max_u64(m_window / 2U, m_min_window);
    }

    // Neither controller keeps an undo: a loss counted stands.
    void on_spurious(const CongestionPacket& /*packet*/, u64 /*lost_ns*/, u64 /*now_ns*/) noexcept override {}

    void reset(u64 /*now_ns*/) noexcept override { restart(); }

    void set_max_datagram(u64 bytes) noexcept override
    {
        const bool initial = m_window == m_initial;
        size(min_u64(max_u64(bytes, kQuicMinInitialDatagram), kMaxDatagram));
        m_window = initial ? m_initial : max_u64(m_window, m_min_window);
    }

    [[nodiscard]] u64 window() const noexcept override { return m_window; }

    // 1.1 times the target once feedback arrived, 2.5 times before (libwebrtc's pacing factors).
    [[nodiscard]] u64 pacing_rate(const QuicRtt& /*rtt*/) const noexcept override
    {
        return multiply_divide(m_target, m_feedback ? kGccPacePercent : kGccFirstPacePercent, kPercent);
    }

    [[nodiscard]] u64 burst() const noexcept override
    {
        if (!m_sampled)
        {
            return m_initial;
        }
        return max_u64(multiply_divide(pacing_rate(QuicRtt{}), kGccBurstNs, kNanosPerSecond), 2U * m_datagram);
    }

    [[nodiscard]] CongestionState state() const noexcept override
    {
        CongestionState out;
        out.window = m_window;
        out.slow_start_threshold = m_window; // no slow start: a controller swapped in avoids congestion from here
        out.recovery_start_ns = m_decrease_ns;
        out.reductions = m_reductions;
        out.undone = m_undone;
        out.bandwidth = m_delay_rate;
        out.target_rate = m_target;
        const bool holding = m_decrease_ns != kNeverNs && m_rate_state != RateState::Increase;
        out.phase = holding ? CongestionPhase::Recovery : CongestionPhase::Avoidance;
        return out;
    }

private:
    enum class RateState : u8
    {
        Hold,
        Increase,
        Decrease,
    };

    struct Least
    {
        u64 at_ns = 0U;
        u64 rate = 0U;
    };

    void size(u64 max_datagram) noexcept
    {
        m_datagram = max_datagram;
        m_min_window = max_u64(kGccMinWindow, m_policy.minimum_packets * max_datagram);
        m_initial = max_u64(congestion_initial_window(m_policy, max_datagram), m_min_window);
    }

    // The start rate is the initial window over the initial RTT (12000 bytes: 288 kbit/s, near libwebrtc's 300).
    void restart() noexcept
    {
        m_window = m_initial;
        m_timestamps = false;
        m_acked = AckedRateEstimator{};
        restart_delay();
        start(multiply_divide(m_initial, kNanosPerSecond, kQuicInitialRttNs));
        m_rtt_ns = kGccDefaultRttNs;
        m_rtt_min = WindowedMin<u64>(kGccRttFrames);
        m_frames = 0U;
        m_feedback = false;
        m_feedback_window = false;
        m_sampled = false;
        m_frame_acked = false;
        m_frame_packets = 0U;
        m_frame_lost = 0U;
        m_frame_ce = 0U;
        m_frame_oldest_ns = kNeverNs;
    }

    void restart_delay() noexcept
    {
        m_groups.reset();
        const bool ready = m_trend.init(m_samples, kGccTrendWindow);
        static_cast<void>(ready); // the window and its storage are this class's constants
        m_last_seen_ns = kNeverNs;
    }

    // Both controllers start at `rate` (SetStartBitrate and SetSendBitrate), the link's capacity unknown.
    void start(u64 rate) noexcept
    {
        m_delay_rate = min_u64(max_u64(rate, m_min_rate), m_max_rate);
        m_throughput = m_delay_rate;
        m_delay_limit = m_delay_rate;
        m_target = m_delay_rate;
        m_rate_state = RateState::Hold;
        m_change_ns = kNeverNs;
        m_decrease_ns = kNeverNs;
        m_has_capacity = false;
        m_capacity = 0U;
        m_deviation = kGccDeviationMin;
        m_expected = 0U;
        m_lost = 0U;
        m_fraction = 0U;
        m_decreased = false;
        m_loss_decrease_ns = kNeverNs;
        m_first_report_ns = kNeverNs;
        m_least_count = 0U;
    }

    // Sections 5.2 and 5.3: one packet's send and receive times, in microseconds, into the groups and the trend.
    void add_delay(u64 sent_ns, u64 received_ns, u64 bytes, u64 now_ns) noexcept
    {
        if (m_last_seen_ns != kNeverNs && now_ns - m_last_seen_ns > kGccStreamTimeoutNs)
        {
            restart_delay();
        }
        m_last_seen_ns = now_ns;
        const auto us = [](u64 ns) { return static_cast<i64>(ns / 1000U); };
        GroupDeltas deltas;
        if (m_groups.add(us(sent_ns), us(received_ns), us(now_ns), static_cast<u32>(min_u64(bytes, ~u32{0})), deltas))
        {
            const bool taken = m_trend.update(deltas, us(received_ns));
            static_cast<void>(taken); // a delta outside the trend's domain changes nothing
        }
    }

    [[nodiscard]] bool acked_rate(u64& rate) const noexcept
    {
        if (!m_acked.has_estimate())
        {
            return false;
        }
        rate = static_cast<u64>(m_acked.kbps() * 125.0F); // kbit/s to bytes per second, rounded down
        return true;
    }

    // DelayBasedBwe::MaybeUpdateEstimate: an overuse reduces at most once a clamped RTT, or at once when the
    // throughput fell below half the rate; with no acknowledged rate yet it halves the rate.
    void update_delay_based(u64 now_ns) noexcept
    {
        u64 acked = 0U;
        const bool has_acked = acked_rate(acked);
        if (m_trend.state() != DelayState::Overusing)
        {
            change_rate(m_trend.state(), has_acked, acked, now_ns);
        }
        else if (!has_acked)
        {
            set_delay_rate(m_delay_rate / 2U, now_ns); // the throughput it tests, rate / 2 - 1 bit/s, is always low
        }
        else if (reduce_further(now_ns, acked))
        {
            change_rate(DelayState::Overusing, has_acked, acked, now_ns);
        }
        m_delay_limit = m_delay_rate; // UpdateDelayBasedEstimate: the loss controller's ceiling
        set_target(m_target);
    }

    [[nodiscard]] bool reduce_further(u64 now_ns, u64 throughput) const noexcept
    {
        const u64 interval = min_u64(max_u64(m_rtt_ns, kGccReduceMinNs), kGccReduceMaxNs);
        if (m_change_ns == kNeverNs || now_ns - m_change_ns >= interval)
        {
            return true;
        }
        return throughput < m_delay_rate / 2U;
    }

    // AimdRateControl::SetEstimate.
    void set_delay_rate(u64 rate, u64 now_ns) noexcept
    {
        const u64 previous = m_delay_rate;
        m_delay_rate = min_u64(max_u64(rate, m_min_rate), kGccMaxRate);
        m_change_ns = now_ns;
        if (m_delay_rate < previous)
        {
            m_decrease_ns = now_ns;
            ++m_reductions;
        }
    }

    // Section 5.5's rate control (AimdRateControl::ChangeBitrate) on one detector state.
    void change_rate(DelayState signal, bool has_acked, u64 acked, u64 now_ns) noexcept
    {
        const u64 throughput = has_acked ? acked : m_throughput;
        m_throughput = throughput;
        if (signal == DelayState::Overusing)
        {
            m_rate_state = RateState::Decrease;
        }
        else if (signal == DelayState::Underusing)
        {
            m_rate_state = RateState::Hold;
        }
        else if (m_rate_state == RateState::Hold)
        {
            m_change_ns = now_ns;
            m_rate_state = RateState::Increase;
        }
        if (m_rate_state == RateState::Increase)
        {
            increase(throughput, now_ns);
        }
        else if (m_rate_state == RateState::Decrease)
        {
            decrease(throughput, now_ns);
        }
    }

    // Additive near the link's capacity, multiplicative far from it, never past 1.5 times the throughput and 10
    // kbit/s; a throughput past the capacity's spread forgets it.
    void increase(u64 throughput, u64 now_ns) noexcept
    {
        if (m_has_capacity && throughput > capacity_upper())
        {
            m_has_capacity = false;
        }
        const u64 limit = saturating_add(saturating_add(throughput, throughput / 2U), kGccLimitTerm);
        if (m_delay_rate < limit)
        {
            const u64 step = m_has_capacity ? additive_step(now_ns) : multiplicative_step(now_ns);
            m_delay_rate = min_u64(min_u64(saturating_add(m_delay_rate, step), limit), kGccMaxRate);
        }
        m_change_ns = now_ns;
    }

    // To 0.85 of the throughput less 5 kbit/s (0.85 of the capacity's estimate when that is not below the rate),
    // never up; the capacity learns the throughput, and the rate holds until the queues drain.
    void decrease(u64 throughput, u64 now_ns) noexcept
    {
        u64 decreased = multiply_divide(throughput, kGccBetaPercent, kPercent);
        if (decreased > kGccBackoffTerm)
        {
            decreased -= kGccBackoffTerm;
        }
        if (decreased > m_delay_rate && m_has_capacity)
        {
            decreased = multiply_divide(m_capacity, kGccBetaPercent, kPercent);
        }
        if (decreased < m_delay_rate)
        {
            m_delay_rate = max_u64(decreased, m_min_rate);
            ++m_reductions;
        }
        if (m_has_capacity && throughput < capacity_lower())
        {
            m_has_capacity = false;
        }
        update_capacity(throughput);
        m_rate_state = RateState::Hold;
        m_change_ns = now_ns;
        m_decrease_ns = now_ns;
    }

    // 1.08 a second, for at most a second, and at least 1 kbit/s.
    [[nodiscard]] u64 multiplicative_step(u64 now_ns) const noexcept
    {
        const u64 elapsed = (m_change_ns == kNeverNs || now_ns < m_change_ns)
                                ? kNanosPerSecond
                                : min_u64(now_ns - m_change_ns, kNanosPerSecond);
        const u64 grown = exp_minus_one_q32(multiply_divide(elapsed, kGccLn108Q32, kNanosPerSecond));
        return max_u64(multiply_divide(m_delay_rate, grown, kQ32), kGccIncreaseFloor);
    }

    // Half a packet per response time (the RTT and 100 ms), the packet a 30 frames a second source's of 1200 bytes
    // at most, and at least 4 kbit/s a second.
    [[nodiscard]] u64 additive_step(u64 now_ns) const noexcept
    {
        const u64 frame = max_u64(m_delay_rate / kGccFramesPerSecond, 1U);
        const u64 packet = frame / ((frame + kGccPacketBytes - 1U) / kGccPacketBytes);
        const u64 response = 2U * saturating_add(m_rtt_ns, kGccResponseNs);
        const u64 per_second = max_u64(multiply_divide(packet, kNanosPerSecond, response), kGccMinAdditiveRate);
        const u64 elapsed = (m_change_ns == kNeverNs || now_ns < m_change_ns) ? 0U : now_ns - m_change_ns;
        return multiply_divide(per_second, elapsed, kNanosPerSecond);
    }

    // LinkCapacityEstimator: the throughput at overuse averaged with gain 1/20, and its variance normalised by it.
    void update_capacity(u64 sample) noexcept
    {
        m_capacity = m_has_capacity ? m_capacity - m_capacity / kGccCapacityGain + sample / kGccCapacityGain : sample;
        m_has_capacity = true;
        const u64 error = (m_capacity > sample) ? m_capacity - sample : sample - m_capacity;
        const u64 most = kGccDeviationMax * kGccCapacityGain;
        const u64 term = (error < (u64{1} << 24U))
                             ? min_u64(multiply_divide(error * error, u64{1} << 16U, max_u64(m_capacity,
                                                                                                 kGccCapacityNorm)),
                                       most)
                             : most;
        const u64 deviation = m_deviation - m_deviation / kGccCapacityGain + term / kGccCapacityGain;
        m_deviation = min_u64(max_u64(deviation, kGccDeviationMin), kGccDeviationMax);
    }

    // Three standard deviations, 3 sqrt(deviation * capacity), around the capacity.
    [[nodiscard]] u64 capacity_spread() const noexcept
    {
        return 3U * hesap::net::square_root(multiply_divide(m_deviation, m_capacity, u64{1} << 16U));
    }

    [[nodiscard]] u64 capacity_upper() const noexcept { return saturating_add(m_capacity, capacity_spread()); }

    [[nodiscard]] u64 capacity_lower() const noexcept
    {
        const u64 spread = capacity_spread();
        return (m_capacity > spread) ? m_capacity - spread : 0U;
    }

    // Section 6 (SendSideBandwidthEstimation::UpdatePacketsLost): the frame's lost and acknowledged packets join
    // the count until it holds 20, which gives the loss fraction in 2^-8; CE-marked packets count as lost.
    void update_packets_lost(u64 now_ns) noexcept
    {
        const u64 packets = saturating_add(m_frame_packets, m_frame_lost);
        if (packets == 0U)
        {
            return;
        }
        if (m_first_report_ns == kNeverNs)
        {
            m_first_report_ns = now_ns;
        }
        m_expected = saturating_add(m_expected, packets);
        m_lost = saturating_add(m_lost, min_u64(saturating_add(m_frame_lost, m_frame_ce), packets));
        if (m_expected < kGccLossPackets)
        {
            return;
        }
        m_decreased = false;
        m_fraction = min_u64(multiply_divide(m_lost, 256U, m_expected), 255U);
        m_expected = 0U;
        m_lost = 0U;
        update_loss_based(now_ns);
    }

    // SendSideBandwidthEstimation::UpdateEstimate's classic controller: in its first 2 s with no loss it follows the
    // delay-based rate up; at most 2% lost, it grows to 1.08 times the last second's least rate and 1 kbit/s; past
    // 10%, it takes 1 - p / 2 at most once 300 ms and an RTT; between, it holds.
    void update_loss_based(u64 now_ns) noexcept
    {
        if (m_fraction == 0U && now_ns - m_first_report_ns < kGccStartPhaseNs && m_delay_limit > m_target)
        {
            m_least_count = 0U;
            remember_least(now_ns);
            set_target(m_delay_limit);
            return;
        }
        remember_least(now_ns);
        if (m_fraction <= kGccLowLoss)
        {
            set_target(saturating_add(multiply_divide(m_least[0].rate, kGccLossIncreasePercent, kPercent),
                                      kGccIncreaseFloor));
            return;
        }
        const bool spaced = m_loss_decrease_ns == kNeverNs || now_ns < m_loss_decrease_ns ||
                            now_ns - m_loss_decrease_ns >= saturating_add(kGccLossDecreaseNs, m_rtt_ns);
        if (m_fraction > kGccHighLoss && !m_decreased && spaced)
        {
            m_loss_decrease_ns = now_ns;
            m_decreased = true;
            set_target(multiply_divide(m_target, 512U - m_fraction, 512U));
            ++m_reductions;
            return;
        }
        set_target(m_target);
    }

    // The least target rate of the last second: a monotonic queue with an entry per time slot at most. A rate in the
    // newest entry's slot merges into it, keeping the lower rate and the later time: the least reads at most 64 ms
    // late and never high.
    void remember_least(u64 now_ns) noexcept
    {
        usize expired = 0U;
        while (expired < m_least_count &&
               (now_ns < m_least[expired].at_ns || now_ns - m_least[expired].at_ns + kGccHistorySlackNs > kGccHistoryNs))
        {
            ++expired;
        }
        for (usize i = expired; i < m_least_count; ++i)
        {
            m_least[i - expired] = m_least[i];
        }
        m_least_count -= expired;
        while (m_least_count != 0U && m_target <= m_least[m_least_count - 1U].rate)
        {
            --m_least_count;
        }
        if (m_least_count != 0U && m_least[m_least_count - 1U].at_ns / kGccHistorySlotNs == now_ns / kGccHistorySlotNs)
        {
            m_least[m_least_count - 1U].at_ns = now_ns;
            return;
        }
        m_least[m_least_count] = Least{now_ns, m_target};
        ++m_least_count;
    }

    // UpdateTargetBitrate: under the delay-based rate and the policy's maximum, at least its minimum.
    void set_target(u64 rate) noexcept
    {
        m_target = max_u64(min_u64(min_u64(rate, m_delay_limit), m_max_rate), m_min_rate);
    }

    // The target over the least of the recent frames' largest RTTs and 350 ms, averaged with the window before.
    void update_window() noexcept
    {
        const u64 span = saturating_add(m_rtt_min.best(), kGccQueueNs);
        const u64 window = min_u64(multiply_divide(m_target, span, kNanosPerSecond), kCongestionMaxWindow);
        m_window = max_u64(m_feedback_window ? window / 2U + m_window / 2U : window, m_min_window);
        m_feedback_window = true;
    }

    CongestionPolicy m_policy;
    u64 m_datagram = 0U;
    u64 m_min_window = 0U;
    u64 m_initial = 0U;
    u64 m_window = 0U;
    u64 m_min_rate = 0U;
    u64 m_max_rate = 0U;
    u64 m_reductions = 0U;
    u64 m_undone = 0U;

    // The delay trend (sections 5.2 to 5.4) and the acknowledged rate.
    PacketGroupTracker m_groups;
    TrendlineFilter m_trend;
    TrendlineSample m_samples[TrendlineFilter::storage_samples(kGccTrendWindow)];
    AckedRateEstimator m_acked;
    u64 m_last_seen_ns = kNeverNs;
    bool m_timestamps = false; // the peer reports receive times

    // The frame in progress.
    u64 m_largest_number = 0U;
    u64 m_largest_sent_ns = 0U;
    u64 m_largest_bytes = 0U;
    u64 m_frame_packets = 0U;
    u64 m_frame_lost = 0U;
    u64 m_frame_ce = 0U;
    u64 m_frame_oldest_ns = kNeverNs;
    bool m_frame_acked = false;

    // The rate control (section 5.5) and the link's capacity.
    u64 m_delay_rate = 0U;
    u64 m_throughput = 0U;
    u64 m_change_ns = kNeverNs;
    u64 m_decrease_ns = kNeverNs;
    u64 m_capacity = 0U;
    u64 m_deviation = kGccDeviationMin;
    u64 m_rtt_ns = kGccDefaultRttNs;
    RateState m_rate_state = RateState::Hold;
    bool m_has_capacity = false;

    // The loss controller (section 6).
    u64 m_delay_limit = 0U;
    u64 m_target = 0U;
    u64 m_expected = 0U;
    u64 m_lost = 0U;
    u64 m_fraction = 0U; // in 2^-8
    u64 m_loss_decrease_ns = kNeverNs;
    u64 m_first_report_ns = kNeverNs;
    Least m_least[kGccHistory] = {};
    usize m_least_count = 0U;
    bool m_decreased = false;

    // The window.
    WindowedMin<u64> m_rtt_min{kGccRttFrames};
    u64 m_frames = 0U;
    bool m_feedback = false;
    bool m_feedback_window = false;
    bool m_sampled = false;
};

static_assert(sizeof(NewReno) <= kCongestionSlotBytes && alignof(NewReno) <= alignof(CongestionSlot));
static_assert(sizeof(Cubic) <= kCongestionSlotBytes && alignof(Cubic) <= alignof(CongestionSlot));
static_assert(sizeof(Bbr) <= kCongestionSlotBytes && alignof(Bbr) <= alignof(CongestionSlot));
static_assert(sizeof(Prague) <= kCongestionSlotBytes && alignof(Prague) <= alignof(CongestionSlot));
static_assert(sizeof(Scream) <= kCongestionSlotBytes && alignof(Scream) <= alignof(CongestionSlot));
static_assert(sizeof(Gcc) <= kCongestionSlotBytes && alignof(Gcc) <= alignof(CongestionSlot));

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
    // SCReAM v2 is version 4's and must pace (its section 4.3.2), its delay target in [1 ms, QDELAY_TARGET_HI] and its
    // target rate's bounds in order.
    const bool scream = policy.algorithm == CongestionAlgorithm::Scream && policy.version >= 4U && policy.pacing &&
                        policy.queue_delay_target_ns >= 1000000U &&
                        policy.queue_delay_target_ns <= kScreamQueueDelayTargetMaxNs &&
                        (policy.target_rate_max == 0U || policy.target_rate_min <= policy.target_rate_max);
    // GCC is version 5's and must pace (its rate is a pacing rate), its target rate's bounds in order.
    const bool gcc = policy.algorithm == CongestionAlgorithm::Gcc && policy.version >= 5U && policy.pacing &&
                     (policy.target_rate_max == 0U || policy.target_rate_min <= policy.target_rate_max);
    const bool algorithm = window || bbr || prague || scream || gcc;
    return policy.version >= 1U && policy.version <= kCongestionInterfaceVersion && algorithm &&
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
    if (policy.algorithm == CongestionAlgorithm::Scream)
    {
        Scream* const scream = new (slot.bytes) Scream(policy, max_datagram, delivery);
        if (seed != nullptr)
        {
            scream->seed(*seed);
        }
        return scream;
    }
    if (policy.algorithm == CongestionAlgorithm::Gcc)
    {
        Gcc* const gcc = new (slot.bytes) Gcc(policy, max_datagram);
        if (seed != nullptr)
        {
            gcc->seed(*seed);
        }
        return gcc;
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
