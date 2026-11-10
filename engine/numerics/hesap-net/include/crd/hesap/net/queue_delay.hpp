// Queue delay estimation (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net), the inputs of the
// delay-based real-time controllers (NET-PATH's SCReAM v2, design S71). Header-only and integer: crd-net reads it
// without linking crd-hesap-net, and every platform computes the same delays.
//
//   BaseDelayHistory   RFC 6817 section 3.4.2 and appendix A's base delay: the minimum delay of each of the last
//                      BASE_HISTORY (10) intervals of a minute, the base the least of them, so a path whose propagation
//                      delay rises takes a new base within ten minutes and a single low outlier ages out. A queue
//                      delay is a delay sample less the base.
//   QueueDelayHistory  draft-ietf-ccwg-rfc8298bis-screamv2-03 section 4.5.4's queue delay history: the last 200
//                      normalised samples, the mean of the newest 50 and the variance of all 200, kept as exact running
//                      sums so each sample costs a constant.
//
// Nothing allocates.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::net
{

class BaseDelayHistory
{
public:
    static constexpr crd::u32 kIntervals = 10U;                         // BASE_HISTORY
    static constexpr crd::u64 kIntervalNs = crd::u64{60} * 1000000000U; // a minute

    // A delay sample of `now_ns`. A sample in a later minute than the newest interval's start opens the next
    // interval, dropping the oldest of the ten (appendix A's update_base_delay); time going backwards stays in the
    // newest.
    constexpr void update(crd::u64 delay_ns, crd::u64 now_ns) noexcept
    {
        if (m_count == 0U || now_ns - min(now_ns, m_start_ns) >= kIntervalNs)
        {
            m_newest = (m_count == 0U) ? 0U : (m_newest + 1U) % kIntervals;
            m_count = (m_count < kIntervals) ? m_count + 1U : kIntervals;
            m_minimum[m_newest] = delay_ns;
            m_start_ns = now_ns;
            return;
        }
        m_minimum[m_newest] = min(m_minimum[m_newest], delay_ns);
    }

    // The least of the intervals' minima; ~0 before a sample.
    [[nodiscard]] constexpr crd::u64 base() const noexcept
    {
        crd::u64 least = ~crd::u64{0};
        for (crd::u32 i = 0U; i < m_count; ++i)
        {
            least = min(least, m_minimum[i]);
        }
        return least;
    }

    [[nodiscard]] constexpr bool has_sample() const noexcept { return m_count != 0U; }

    // Forgets every interval: the next sample is the base (a path change, or SCReAM v2's drift remedy).
    constexpr void reset() noexcept { m_count = 0U; }

private:
    [[nodiscard]] static constexpr crd::u64 min(crd::u64 a, crd::u64 b) noexcept { return (a < b) ? a : b; }

    crd::u64 m_minimum[kIntervals] = {};
    crd::u64 m_start_ns = 0U; // when the newest interval opened
    crd::u32 m_newest = 0U;
    crd::u32 m_count = 0U;
};

class QueueDelayHistory
{
public:
    static constexpr crd::u32 kLong = 200U; // the variance's samples
    static constexpr crd::u32 kShort = 50U; // the mean's

    constexpr void push(crd::u16 sample) noexcept
    {
        const crd::u32 slot = m_next;
        if (m_count == kLong)
        {
            const crd::u64 oldest = m_samples[slot];
            m_long_sum -= oldest;
            m_long_squares -= oldest * oldest;
        }
        if (m_count >= kShort)
        {
            m_short_sum -= m_samples[(slot + kLong - kShort) % kLong]; // leaves the newest 50
        }
        m_samples[slot] = sample;
        m_long_sum += sample;
        m_long_squares += crd::u64{sample} * sample;
        m_short_sum += sample;
        m_next = (slot + 1U) % kLong;
        m_count = (m_count < kLong) ? m_count + 1U : kLong;
    }

    [[nodiscard]] constexpr crd::u32 count() const noexcept { return m_count; }

    // The mean of the newest min(count, 50) samples, rounded down; 0 before a sample.
    [[nodiscard]] constexpr crd::u64 mean() const noexcept
    {
        const crd::u32 n = (m_count < kShort) ? m_count : kShort;
        return (n == 0U) ? 0U : m_short_sum / n;
    }

    // The population variance of every held sample, in the samples' unit squared, rounded down; 0 before two.
    [[nodiscard]] constexpr crd::u64 variance() const noexcept
    {
        if (m_count < 2U)
        {
            return 0U;
        }
        // n * sum(x^2) - (sum x)^2 over n^2: at most 200 * 200 * 2^32, exact in 64 bits.
        return (m_count * m_long_squares - m_long_sum * m_long_sum) / (crd::u64{m_count} * m_count);
    }

    constexpr void reset() noexcept
    {
        m_count = 0U;
        m_next = 0U;
        m_long_sum = 0U;
        m_long_squares = 0U;
        m_short_sum = 0U;
    }

private:
    crd::u16 m_samples[kLong] = {};
    crd::u64 m_long_sum = 0U;
    crd::u64 m_long_squares = 0U;
    crd::u64 m_short_sum = 0U;
    crd::u32 m_next = 0U;
    crd::u32 m_count = 0U;
};

} // namespace crd::hesap::net
