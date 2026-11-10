// Classic ECN AQM detection (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net): the passive
// detection of Briscoe, "TCP Prague Fall-back on Detection of a Classic ECN AQM", TR-BB-2019-002 version 03
// (arXiv:1911.00710v3), sections 3.2 to 3.4 (version 2 of the design), in integer arithmetic. RFC 9331 section 4.3
// requires a scalable congestion control to detect a bottleneck that marks ECN as a classic AQM does and to adapt.
// Header-only: crd-net reads it without linking crd-hesap-net, as it reads the delivery rate and windowed filters.
//
//   score      classic_ecn, in 2^-16 units of CLASSIC_ECN: at or below -L_STICKY the detector is quiescent and the
//              control purely scalable, from 0 to CLASSIC_ECN it moves to classic, and up to C_STICKY it stays classic.
//              CE feedback wakes a quiescent detector by one unit.
//   rtt        fbk_srtt and fbk_mdev, moving averages of every RTT sample with gains 1 / (2 S^1.5) and half that, S the
//              slow start threshold in segments (section 3.3): smoothed over a classic sawtooth of the flow's own.
//   rounds     each round trip adds V lg(v / V0) + D lg(max(d / D0, 1)) - S s to the score: v is fbk_mdev, d the
//              likely queue depth fbk_srtt - min_rtt, s the share of the round the sender was self-limited, V = D =
//              1/2, S = 1/4, V0 = 750 us and D0 = 2 ms. A queue varying by more than about 1.5 ms is a classic one.
//   idle       each idle timeout halves a positive score.
//   input      times in nanoseconds; samples above 2^40 ns (18 minutes) count as 2^40.

#pragma once

#include <crd/core/types.hpp>
#include <crd/core/wide_multiply.hpp>

#include <bit>

namespace crd::hesap::net
{

// log2(x) in 2^-16 units, rounded down; 0 for x of 0 or 1. Each squaring of the mantissa yields a fraction bit.
[[nodiscard]] inline crd::i64 log2_q16(crd::u64 x) noexcept
{
    if (x <= 1U)
    {
        return 0;
    }
    const auto integer = static_cast<crd::u32>(63 - std::countl_zero(x));
    crd::u64 mantissa = (integer <= 62U) ? (x << (62U - integer)) : (x >> 1U); // [1, 2) with 62 fraction bits
    crd::i64 result = static_cast<crd::i64>(integer) << 16U;
    for (crd::u32 bit = 16U; bit-- > 0U;)
    {
        const WideProduct square = multiply_wide(mantissa, mantissa);
        mantissa = (square.hi << 2U) | (square.lo >> 62U);
        if (mantissa >= (crd::u64{1} << 63U))
        {
            mantissa >>= 1U;
            result |= crd::i64{1} << bit;
        }
    }
    return result;
}

class ClassicEcnDetector
{
public:
    static constexpr crd::i64 kClassic = crd::i64{1} << 16U; // CLASSIC_ECN
    static constexpr crd::i64 kSticky = 8 * kClassic;        // L_STICKY = C_STICKY = 16 V
    static constexpr crd::u64 kVariabilityNs = 750000U;      // V0
    static constexpr crd::u64 kDepthNs = 2000000U;           // D0
    static constexpr crd::u64 kMaxSampleNs = crd::u64{1} << 40U;
    static constexpr crd::u64 kMaxSegments = crd::u64{1} << 20U; // the gain's S is clamped to 1 .. this
    static constexpr crd::u64 kMaxIdleHalvings = 64U;
    static constexpr crd::u64 kInitialMdevNs = 1000U; // "fbk_mdev = 1" (us): no conservative start

    // One RTT sample, with the slow start threshold (or the window, before one is set) in `segments`.
    void on_rtt(crd::u64 rtt_ns, crd::u64 segments) noexcept
    {
        const crd::u64 sample = clamp_u64(rtt_ns, 0U, kMaxSampleNs) << 16U;
        if (!m_sampled)
        {
            m_sampled = true;
            m_srtt_q16 = sample;
            m_mdev_q16 = kInitialMdevNs << 16U;
            return;
        }
        // Section 3.4: fbk_g_srtt = 2 S^(3/2) and fbk_g_mdev = 2 fbk_g_srtt; S^3 stays below 2^61.
        const crd::u64 s = clamp_u64(segments, 1U, kMaxSegments);
        const auto gain = static_cast<crd::i64>(2U * square_root(s * s * s));
        const crd::i64 error = static_cast<crd::i64>(sample) - static_cast<crd::i64>(m_srtt_q16);
        m_srtt_q16 = static_cast<crd::u64>(static_cast<crd::i64>(m_srtt_q16) + error / gain);
        const crd::i64 deviation = (error < 0) ? -error : error;
        m_mdev_q16 = static_cast<crd::u64>(static_cast<crd::i64>(m_mdev_q16) +
                                           (deviation - static_cast<crd::i64>(m_mdev_q16)) / (2 * gain));
    }

    // CE feedback arrived.
    void on_ce() noexcept { m_score += (m_score <= -kSticky) ? 1 : 0; }

    // A round trip ended: `min_rtt_ns` is the path's minimum, and the sender was self-limited (by the application or
    // the receiver) for `self_limited_q16` 2^-16 of it.
    void on_round(crd::u64 min_rtt_ns, crd::u32 self_limited_q16) noexcept
    {
        if (m_score <= -kSticky || !m_sampled)
        {
            return; // quiescent until CE feedback
        }
        // delta = V lg(v / V0) + D lg(max(d / D0, 1)) - S s, with V = D = 1/2 and S = 1/4.
        const crd::u64 v = (mdev_ns() > 0U) ? mdev_ns() : 1U;
        const crd::u64 srtt = srtt_ns();
        const crd::u64 d = (srtt > min_rtt_ns) ? srtt - min_rtt_ns : 0U;
        const crd::i64 variability = (log2_q16(v) - log2_q16(kVariabilityNs)) / 2;
        const crd::i64 depth = (d > kDepthNs) ? (log2_q16(d) - log2_q16(kDepthNs)) / 2 : 0;
        const crd::i64 limited = static_cast<crd::i64>((self_limited_q16 < (1U << 16U)) ? self_limited_q16 : 1U << 16U);
        m_score = clamp_i64(m_score + variability + depth - limited / 4, -kSticky, kSticky);
    }

    // The sender was idle for `timeouts` idle timeouts.
    void on_idle(crd::u64 timeouts) noexcept
    {
        for (crd::u64 i = 0U; i < timeouts && i < kMaxIdleHalvings && m_score > 0; ++i)
        {
            m_score /= 2;
        }
    }

    [[nodiscard]] crd::i64 score() const noexcept { return m_score; }
    // c: how classic the response should be, from 0 (scalable) to 2^16 (classic).
    [[nodiscard]] crd::u32 classic() const noexcept { return static_cast<crd::u32>(clamp_i64(m_score, 0, kClassic)); }
    [[nodiscard]] crd::u64 srtt_ns() const noexcept { return m_srtt_q16 >> 16U; }
    [[nodiscard]] crd::u64 mdev_ns() const noexcept { return m_mdev_q16 >> 16U; }

private:
    [[nodiscard]] static constexpr crd::u64 clamp_u64(crd::u64 x, crd::u64 low, crd::u64 high) noexcept
    {
        return (x < low) ? low : ((x > high) ? high : x);
    }

    [[nodiscard]] static constexpr crd::i64 clamp_i64(crd::i64 x, crd::i64 low, crd::i64 high) noexcept
    {
        return (x < low) ? low : ((x > high) ? high : x);
    }

    // floor(sqrt(x)), bit by bit.
    [[nodiscard]] static constexpr crd::u64 square_root(crd::u64 x) noexcept
    {
        crd::u64 root = 0U;
        crd::u64 bit = crd::u64{1} << 62U;
        while (bit > x)
        {
            bit >>= 2U;
        }
        while (bit != 0U)
        {
            if (x >= root + bit)
            {
                x -= root + bit;
                root = (root >> 1U) + bit;
            }
            else
            {
                root >>= 1U;
            }
            bit >>= 2U;
        }
        return root;
    }

    crd::i64 m_score = -kSticky;
    crd::u64 m_srtt_q16 = 0U;
    crd::u64 m_mdev_q16 = 0U;
    bool m_sampled = false;
};

} // namespace crd::hesap::net
