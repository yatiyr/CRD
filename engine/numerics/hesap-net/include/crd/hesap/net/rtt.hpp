// Round-trip time estimation (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net): RFC 9002 §5's
// three values and the durations recovery derives from them, in integer nanoseconds. With no acknowledgement delay
// the updates and pto_ns() are RFC 6298's SRTT, RTTVAR and RTO before its one-second floor.
//
// A sample is the caller's to qualify (§5.1: the largest acknowledged packet newly acknowledged, and it was
// ack-eliciting). The peer writes ack_delay, so samples and delays are clamped to kMaxSampleNs, nine years, which
// keeps every sum and product below in range whatever the peer sends; a negative sample is refused.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::net
{

class RttEstimator
{
public:
    static constexpr crd::i64 kInitialRttNs = 333'000'000; // §6.2.2's kInitialRtt, until a sample arrives.
    static constexpr crd::i64 kGranularityNs = 1'000'000;  // §6.1.2's kGranularity.
    static constexpr crd::i64 kMaxSampleNs = crd::i64{1} << 58U;

    // Feeds one qualified sample; false, changing nothing, for a negative latest_rtt_ns. The first sample ignores
    // ack_delay_ns (§5.3: it would only lower the estimate); later ones subtract it, capped at max_ack_delay_ns once
    // the handshake is confirmed, when the sample stays at or above min_rtt without it.
    [[nodiscard]] constexpr bool on_sample(crd::i64 latest_rtt_ns, crd::i64 ack_delay_ns, crd::i64 max_ack_delay_ns,
                                           bool handshake_confirmed) noexcept
    {
        if (latest_rtt_ns < 0)
        {
            return false;
        }
        const crd::i64 latest = clamped(latest_rtt_ns);
        m_latest = latest;
        if (!m_sampled)
        {
            m_sampled = true;
            m_min = latest;
            m_smoothed = latest;
            m_variation = latest / 2;
            return true;
        }
        m_min = (latest < m_min) ? latest : m_min;
        crd::i64 delay = clamped(ack_delay_ns);
        if (handshake_confirmed && delay > clamped(max_ack_delay_ns))
        {
            delay = clamped(max_ack_delay_ns);
        }
        const crd::i64 adjusted = (latest >= m_min + delay) ? latest - delay : latest;
        const crd::i64 deviation = (m_smoothed >= adjusted) ? m_smoothed - adjusted : adjusted - m_smoothed;
        m_variation = ((3 * m_variation) + deviation) / 4;
        m_smoothed = ((7 * m_smoothed) + adjusted) / 8;
        return true;
    }

    // §5.2: after persistent congestion the path may have changed, so min_rtt restarts from the newest sample.
    constexpr void on_persistent_congestion() noexcept
    {
        m_min = m_latest;
    }

    // §9.4: a new path restarts the estimate.
    constexpr void reset() noexcept
    {
        *this = RttEstimator{};
    }

    [[nodiscard]] constexpr bool has_sample() const noexcept
    {
        return m_sampled;
    }

    [[nodiscard]] constexpr crd::i64 latest_ns() const noexcept
    {
        return m_latest;
    }

    // The smallest sample since the last restart; kInitialRttNs before any.
    [[nodiscard]] constexpr crd::i64 min_ns() const noexcept
    {
        return m_min;
    }

    [[nodiscard]] constexpr crd::i64 smoothed_ns() const noexcept
    {
        return m_smoothed;
    }

    [[nodiscard]] constexpr crd::i64 variation_ns() const noexcept
    {
        return m_variation;
    }

    // §6.2.1: smoothed_rtt + max(4 * rttvar, kGranularity) + max_ack_delay; pass 0 in the Initial and Handshake spaces.
    [[nodiscard]] constexpr crd::i64 pto_ns(crd::i64 max_ack_delay_ns) const noexcept
    {
        const crd::i64 spread = 4 * m_variation;
        return m_smoothed + ((spread > kGranularityNs) ? spread : kGranularityNs) + clamped(max_ack_delay_ns);
    }

    // §6.1.2: max(9/8 * max(smoothed_rtt, latest_rtt), kGranularity), how long a packet may stay reordered.
    [[nodiscard]] constexpr crd::i64 loss_delay_ns() const noexcept
    {
        const crd::i64 base = (m_smoothed > m_latest) ? m_smoothed : m_latest;
        const crd::i64 delay = base + (base / 8);
        return (delay > kGranularityNs) ? delay : kGranularityNs;
    }

    // §7.6.1: (smoothed_rtt + max(4 * rttvar, kGranularity) + max_ack_delay) * kPersistentCongestionThreshold.
    [[nodiscard]] constexpr crd::i64 persistent_congestion_ns(crd::i64 max_ack_delay_ns) const noexcept
    {
        return 3 * pto_ns(max_ack_delay_ns);
    }

private:
    [[nodiscard]] static constexpr crd::i64 clamped(crd::i64 value) noexcept
    {
        if (value < 0)
        {
            return 0;
        }
        return (value < kMaxSampleNs) ? value : kMaxSampleNs;
    }

    crd::i64 m_latest = 0;
    crd::i64 m_min = kInitialRttNs;
    crd::i64 m_smoothed = kInitialRttNs;
    crd::i64 m_variation = kInitialRttNs / 2;
    bool m_sampled = false;
};

} // namespace crd::hesap::net
