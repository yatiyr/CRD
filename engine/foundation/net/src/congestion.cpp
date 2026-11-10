// crd-net -- congestion control and pacing (NET-PATH; contract: docs/design/network-foundation.md#congestion).
#include <crd/net/congestion.hpp>

#include <crd/core/wide_multiply.hpp>

#include <new>

namespace crd::net
{
namespace
{
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

// The window skeleton both controllers share (RFC 9002 appendix B, RFC 9406, RFC 9438 section 4.9's undo); a
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

    void on_limited(u64 /*now_ns*/) noexcept override
    {
        m_limited = true;
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

    void on_ack_end(const QuicRtt& rtt, bool sampled, u64 now_ns) noexcept override
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

    void on_congestion(u64 newest_sent_ns, u64 now_ns) noexcept override
    {
        if (!in_recovery(newest_sent_ns))
        {
            reduce_window(now_ns, false);
        }
        m_undoable = false; // a mark is no spurious loss
    }

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
    // The reduction at a congestion event: the window and threshold from m_window; what it changes of its own is
    // saved for restore().
    virtual void reduce() noexcept = 0;
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

    void add_window(u64 bytes) noexcept { m_window = min_u64(m_window + bytes, kCongestionMaxWindow); }

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
        reduce();
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

    void reduce() noexcept override
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

    void reduce() noexcept override
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

static_assert(sizeof(NewReno) <= kCongestionSlotBytes && alignof(NewReno) <= alignof(CongestionSlot));
static_assert(sizeof(Cubic) <= kCongestionSlotBytes && alignof(Cubic) <= alignof(CongestionSlot));

[[nodiscard]] i64 scaled_bytes(u64 bytes) noexcept
{
    return static_cast<i64>(min_u64(bytes, kCongestionMaxWindow) * kNanosPerSecond);
}

} // namespace

bool congestion_policy_valid(const CongestionPolicy& policy) noexcept
{
    const bool algorithm =
        policy.algorithm == CongestionAlgorithm::NewReno || policy.algorithm == CongestionAlgorithm::Cubic;
    return policy.version == kCongestionInterfaceVersion && algorithm && policy.minimum_packets >= 2U &&
           policy.initial_packets >= policy.minimum_packets && policy.initial_packets <= kCongestionMaxInitialPackets &&
           policy.pacing_gain >= kCongestionMinPacingGain && policy.pacing_gain <= kCongestionMaxPacingGain;
}

CongestionController* make_congestion(const CongestionPolicy& policy, u64 max_datagram, const CongestionState* seed,
                                      CongestionSlot& slot) noexcept
{
    if (!congestion_policy_valid(policy) || max_datagram < kQuicMinInitialDatagram || max_datagram > kMaxDatagram)
    {
        return nullptr;
    }
    WindowController* made = nullptr;
    if (policy.algorithm == CongestionAlgorithm::NewReno)
    {
        made = new (slot.bytes) NewReno(policy, max_datagram);
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
