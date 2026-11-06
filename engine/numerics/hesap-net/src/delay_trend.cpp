#include <crd/hesap/net/delay_trend.hpp>

#include <crd/math/power.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// The expressions below keep libwebrtc's order of operations: reassociating them would change the rounding the
// bit-exact oracle holds them to (crd-hesap-estimators-webrtc-tests). The one libm call, pow, is crd::math's on both
// sides of that oracle (the Cerid Math Mandate), which also makes the estimate the same on every platform.

namespace crd::hesap::net
{

namespace
{

constexpr crd::i32 kDeltaCounterMax = 1000;
constexpr crd::i32 kMaxScaledDeltas = 60;
constexpr double kMaxAdaptOffsetMs = 15.0;
constexpr double kOverusingTimeThresholdMs = 10.0;
constexpr double kThresholdUp = 0.0087;
constexpr double kThresholdDown = 0.039;
constexpr crd::i64 kMaxAdaptTimeMs = 100;
constexpr double kMinThreshold = 6.0;
constexpr double kMaxThreshold = 600.0;
constexpr double kSmoothing = 0.9;
constexpr double kTrendlineGain = 4.0;
constexpr double kProcessNoiseSlope = 1e-13;
constexpr double kProcessNoiseOffset = 1e-3;
constexpr double kInitialNoiseVariance = 50.0;

[[nodiscard]] bool in_time_domain(crd::i64 time_us) noexcept
{
    return time_us >= -PacketGroupTracker::kMaxTimeUs && time_us <= PacketGroupTracker::kMaxTimeUs;
}

[[nodiscard]] bool in_delta_domain(double delta_ms) noexcept
{
    return std::abs(delta_ms) <= TrendlineFilter::kMaxDeltaMs; // False for NaN too.
}

// Differences of caller-supplied clocks wrap instead of overflowing.
[[nodiscard]] crd::i64 wrapping_difference(crd::i64 a, crd::i64 b) noexcept
{
    return static_cast<crd::i64>(static_cast<crd::u64>(a) - static_cast<crd::u64>(b));
}

constexpr double kNotOver = -1.0;

[[nodiscard]] bool is_not_over(double over_ms) noexcept
{
    return !(over_ms < kNotOver) && !(over_ms > kNotOver);
}

// Microseconds to milliseconds, the way libwebrtc's units convert them.
[[nodiscard]] double fractional_ms(crd::i64 time_us) noexcept
{
    return static_cast<double>(time_us) * (1 / 1000.0);
}

[[nodiscard]] crd::i64 nearest_ms(crd::i64 time_us) noexcept
{
    const crd::i64 quotient = time_us / 1000;
    const crd::i64 remainder = time_us % 1000;
    if (time_us < 0)
    {
        return (-remainder > 500) ? quotient - 1 : quotient;
    }
    return (remainder > 499) ? quotient + 1 : quotient;
}

} // namespace

// ------------------------------------------------------------------------------------------------ PacketGroupTracker

bool PacketGroupTracker::belongs_to_burst(crd::i64 send_us, crd::i64 arrival_us) const noexcept
{
    const crd::i64 arrival_delta = arrival_us - m_current.complete_us;
    const crd::i64 send_delta = send_us - m_current.send_us;
    if (send_delta == 0)
    {
        return true;
    }
    const crd::i64 propagation_delta = arrival_delta - send_delta;
    return propagation_delta < 0 && arrival_delta <= kBurstDeltaUs &&
           arrival_us - m_current.first_arrival_us < kMaxBurstUs;
}

bool PacketGroupTracker::add(crd::i64 send_us, crd::i64 arrival_us, crd::i64 system_us, crd::u32 bytes,
                             GroupDeltas& out) noexcept
{
    if (!in_time_domain(send_us) || !in_time_domain(arrival_us) || !in_time_domain(system_us))
    {
        return false;
    }
    bool calculated = false;
    if (!m_current.started)
    {
        m_current.send_us = send_us;
        m_current.first_send_us = send_us;
        m_current.first_arrival_us = arrival_us;
    }
    else if (m_current.first_send_us > send_us)
    {
        return false; // A reordered packet.
    }
    else if (!belongs_to_burst(send_us, arrival_us) && send_us - m_current.first_send_us > m_group_us)
    {
        if (m_previous.started)
        {
            const crd::i64 send_delta = m_current.send_us - m_previous.send_us;
            const crd::i64 arrival_delta = m_current.complete_us - m_previous.complete_us;
            const crd::i64 system_delta = m_current.system_us - m_previous.system_us;
            if (arrival_delta - system_delta >= kArrivalJumpUs)
            {
                reset(); // The far clock's offset changed.
                return false;
            }
            if (arrival_delta < 0)
            {
                ++m_reordered;
                if (m_reordered >= kReorderedReset)
                {
                    reset();
                }
                return false;
            }
            m_reordered = 0U;
            const auto size_delta = static_cast<crd::i64>(m_current.bytes) - static_cast<crd::i64>(m_previous.bytes);
            out.send_us = send_delta;
            out.arrival_us = arrival_delta;
            out.bytes = static_cast<crd::i32>(std::clamp<crd::i64>(size_delta, std::numeric_limits<crd::i32>::min(),
                                                                   std::numeric_limits<crd::i32>::max()));
            calculated = true;
        }
        m_previous = m_current;
        m_current.first_send_us = send_us;
        m_current.send_us = send_us;
        m_current.first_arrival_us = arrival_us;
        m_current.bytes = 0U;
    }
    else
    {
        m_current.send_us = std::max(m_current.send_us, send_us);
    }
    m_current.bytes += bytes;
    m_current.complete_us = arrival_us;
    m_current.system_us = system_us;
    m_current.started = true;
    return calculated;
}

void PacketGroupTracker::reset() noexcept
{
    m_reordered = 0U;
    m_current = Group{};
    m_previous = Group{};
}

// ---------------------------------------------------------------------------------------------- DelayOveruseDetector

DelayState DelayOveruseDetector::update(double signal, crd::i32 deltas, double gain, double send_delta_ms,
                                        crd::i64 now_ms) noexcept
{
    if (deltas < 2)
    {
        return m_state;
    }
    const double scaled = std::min(deltas, kMaxScaledDeltas) * signal * gain;
    if (scaled > m_threshold)
    {
        // The first signal over assumes half of the time since the one before it was over too. -1 is "not over", also
        // when a burst's negative send delta lands on it, as in libwebrtc.
        m_over_ms = is_not_over(m_over_ms) ? send_delta_ms / 2 : m_over_ms + send_delta_ms;
        ++m_over_count;
        if (m_over_ms > kOverusingTimeThresholdMs && m_over_count > 1 && signal >= m_previous)
        {
            m_over_ms = 0.0;
            m_over_count = 0;
            m_state = DelayState::Overusing;
        }
    }
    else
    {
        m_over_ms = kNotOver;
        m_over_count = 0;
        m_state = scaled < -m_threshold ? DelayState::Underusing : DelayState::Normal;
    }
    m_previous = signal;
    adapt(scaled, now_ms);
    return m_state;
}

void DelayOveruseDetector::adapt(double scaled, crd::i64 now_ms) noexcept
{
    if (m_last_update_ms == -1)
    {
        m_last_update_ms = now_ms;
    }
    if (std::abs(scaled) > m_threshold + kMaxAdaptOffsetMs)
    {
        m_last_update_ms = now_ms; // A spike, say a sudden capacity drop, must not move the threshold.
        return;
    }
    const double k = std::abs(scaled) < m_threshold ? kThresholdDown : kThresholdUp;
    const crd::i64 elapsed_ms = std::min(wrapping_difference(now_ms, m_last_update_ms), kMaxAdaptTimeMs);
    m_threshold += k * (std::abs(scaled) - m_threshold) * static_cast<double>(elapsed_ms);
    m_threshold = std::clamp(m_threshold, kMinThreshold, kMaxThreshold);
    m_last_update_ms = now_ms;
}

// --------------------------------------------------------------------------------------------------- TrendlineFilter

bool TrendlineFilter::init(std::span<TrendlineSample> storage, crd::u32 window, TrendlineOptions options) noexcept
{
    *this = TrendlineFilter{};
    if (window < kMinWindow || window > kMaxWindow || storage.size() < storage_samples(window))
    {
        return false;
    }
    if (options.cap &&
        (options.beginning_packets < 1U || options.end_packets < 1U || options.beginning_packets > window ||
         options.end_packets > window || options.beginning_packets + options.end_packets > window ||
         !(options.cap_uncertainty >= 0.0 && options.cap_uncertainty <= 0.025)))
    {
        return false;
    }
    m_storage = storage.first(storage_samples(window));
    m_window = window;
    m_options = options;
    return true;
}

crd::u32 TrendlineFilter::slot(crd::u32 index) const noexcept
{
    const crd::u32 at = m_head + index;
    return (at > m_window) ? at - (m_window + 1U) : at;
}

double TrendlineFilter::fitted_slope(double fallback) const noexcept
{
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (crd::u32 i = 0U; i < m_count; ++i)
    {
        const TrendlineSample& sample = m_storage[slot(i)];
        sum_x += sample.arrival_ms;
        sum_y += sample.smoothed_delay_ms;
    }
    const double x_average = sum_x / m_count;
    const double y_average = sum_y / m_count;
    double numerator = 0.0;
    double denominator = 0.0;
    for (crd::u32 i = 0U; i < m_count; ++i)
    {
        const TrendlineSample& sample = m_storage[slot(i)];
        const double x = sample.arrival_ms;
        const double y = sample.smoothed_delay_ms;
        numerator += (x - x_average) * (y - y_average);
        denominator += (x - x_average) * (x - x_average);
    }
    return std::fpclassify(denominator) == FP_ZERO ? fallback : numerator / denominator;
}

bool TrendlineFilter::slope_cap(double& cap) const noexcept
{
    const TrendlineSample* early = &m_storage[slot(0U)];
    for (crd::u32 i = 1U; i < m_options.beginning_packets; ++i)
    {
        const TrendlineSample& sample = m_storage[slot(i)];
        if (sample.raw_delay_ms < early->raw_delay_ms)
        {
            early = &sample;
        }
    }
    const crd::u32 late_start = m_count - m_options.end_packets;
    const TrendlineSample* late = &m_storage[slot(late_start)];
    for (crd::u32 i = late_start + 1U; i < m_count; ++i)
    {
        const TrendlineSample& sample = m_storage[slot(i)];
        if (sample.raw_delay_ms < late->raw_delay_ms)
        {
            late = &sample;
        }
    }
    if (late->arrival_ms - early->arrival_ms < 1)
    {
        return false;
    }
    cap = (late->raw_delay_ms - early->raw_delay_ms) / (late->arrival_ms - early->arrival_ms) +
          m_options.cap_uncertainty;
    return true;
}

bool TrendlineFilter::update(double recv_delta_ms, double send_delta_ms, crd::i64 arrival_ms) noexcept
{
    if (m_window == 0U || !in_delta_domain(recv_delta_ms) || !in_delta_domain(send_delta_ms))
    {
        return false;
    }
    const double delta_ms = recv_delta_ms - send_delta_ms;
    m_deltas = std::min(m_deltas + 1, kDeltaCounterMax);
    if (m_first_arrival_ms == -1)
    {
        m_first_arrival_ms = arrival_ms;
    }
    m_accumulated += delta_ms;
    m_smoothed = kSmoothing * m_smoothed + (1 - kSmoothing) * m_accumulated;

    m_storage[slot(m_count)] = {static_cast<double>(wrapping_difference(arrival_ms, m_first_arrival_ms)), m_smoothed,
                                m_accumulated};
    ++m_count;
    if (m_options.sort)
    {
        for (crd::u32 i = m_count - 1U;
             i > 0U && m_storage[slot(i)].arrival_ms < m_storage[slot(i - 1U)].arrival_ms; --i)
        {
            std::swap(m_storage[slot(i)], m_storage[slot(i - 1U)]);
        }
    }
    if (m_count > m_window)
    {
        m_head = slot(1U);
        --m_count;
    }

    double trend = m_detector.previous_signal();
    if (m_count == m_window)
    {
        trend = fitted_slope(trend);
        double cap = 0.0;
        if (m_options.cap && trend >= 0 && slope_cap(cap) && trend > cap)
        {
            trend = cap;
        }
    }
    m_detector.update(trend, m_deltas, kTrendlineGain, send_delta_ms, arrival_ms);
    return true;
}

bool TrendlineFilter::update(const GroupDeltas& deltas, crd::i64 arrival_us) noexcept
{
    return update(fractional_ms(deltas.arrival_us), fractional_ms(deltas.send_us), nearest_ms(arrival_us));
}

// ------------------------------------------------------------------------------------------------- KalmanDelayFilter

double KalmanDelayFilter::min_frame_period(double send_delta_ms) noexcept
{
    // A full history drops its oldest before the scan, so that entry takes no part in this minimum.
    const bool full = m_history_count == kFramePeriodHistory;
    double period = send_delta_ms;
    for (crd::u32 i = 0U; i < m_history_count; ++i)
    {
        if (!full || i != m_history_head)
        {
            period = std::min(m_history[i], period);
        }
    }
    if (full)
    {
        m_history[m_history_head] = send_delta_ms;
        m_history_head = (m_history_head + 1U) % kFramePeriodHistory;
    }
    else
    {
        m_history[m_history_count] = send_delta_ms;
        ++m_history_count;
    }
    return period;
}

void KalmanDelayFilter::update_noise(double residual, double period_ms, bool stable) noexcept
{
    if (!stable)
    {
        return;
    }
    // Faster during startup, to reach the network's jitter level; tuned at 30 updates a second, scaled by the period.
    const double alpha = m_deltas > 10 * 30 ? 0.002 : 0.01;
    const double beta = crd::math::pow(1 - alpha, period_ms * 30.0 / 1000.0);
    m_average_noise = beta * m_average_noise + (1 - beta) * residual;
    m_noise_variance =
        beta * m_noise_variance + (1 - beta) * (m_average_noise - residual) * (m_average_noise - residual);
    if (!std::isfinite(m_average_noise) || !std::isfinite(m_noise_variance))
    {
        // A negative period, from a burst's send times, grows beta past 1: absurd ones reach infinity here.
        m_average_noise = 0.0;
        m_noise_variance = kInitialNoiseVariance;
        ++m_restarts;
    }
    if (m_noise_variance < 1)
    {
        m_noise_variance = 1;
    }
}

bool KalmanDelayFilter::update(double recv_delta_ms, double send_delta_ms, crd::i32 size_delta,
                               DelayState hypothesis) noexcept
{
    if (!in_delta_domain(recv_delta_ms) || !in_delta_domain(send_delta_ms))
    {
        return false;
    }
    const double period_ms = min_frame_period(send_delta_ms);
    const double delay_delta = recv_delta_ms - send_delta_ms;
    const double size = size_delta;

    m_deltas = std::min(m_deltas + 1, kDeltaCounterMax);

    m_covariance[0][0] += kProcessNoiseSlope;
    m_covariance[1][1] += kProcessNoiseOffset;
    if ((hypothesis == DelayState::Overusing && m_offset < m_previous_offset) ||
        (hypothesis == DelayState::Underusing && m_offset > m_previous_offset))
    {
        m_covariance[1][1] += 10 * kProcessNoiseOffset;
    }

    const double h[2] = {size, 1.0};
    const double eh[2] = {m_covariance[0][0] * h[0] + m_covariance[0][1] * h[1],
                          m_covariance[1][0] * h[0] + m_covariance[1][1] * h[1]};
    const double residual = delay_delta - m_slope * h[0] - m_offset;

    // A late group, say a periodic key frame, does not fit the Gaussian model: it counts as at most three deviations.
    const bool stable = hypothesis == DelayState::Normal;
    const double max_residual = 3.0 * std::sqrt(m_noise_variance);
    if (std::abs(residual) < max_residual)
    {
        update_noise(residual, period_ms, stable);
    }
    else
    {
        update_noise(residual < 0 ? -max_residual : max_residual, period_ms, stable);
    }

    const double denominator = m_noise_variance + h[0] * eh[0] + h[1] * eh[1];
    const double k[2] = {eh[0] / denominator, eh[1] / denominator};
    const double ikh[2][2] = {{1.0 - k[0] * h[0], -k[0] * h[1]}, {-k[1] * h[0], 1.0 - k[1] * h[1]}};
    const double e00 = m_covariance[0][0];
    const double e01 = m_covariance[0][1];

    m_covariance[0][0] = e00 * ikh[0][0] + m_covariance[1][0] * ikh[0][1];
    m_covariance[0][1] = e01 * ikh[0][0] + m_covariance[1][1] * ikh[0][1];
    m_covariance[1][0] = e00 * ikh[1][0] + m_covariance[1][0] * ikh[1][1];
    m_covariance[1][1] = e01 * ikh[1][0] + m_covariance[1][1] * ikh[1][1];

    const bool positive_semi_definite =
        m_covariance[0][0] + m_covariance[1][1] >= 0 &&
        m_covariance[0][0] * m_covariance[1][1] - m_covariance[0][1] * m_covariance[1][0] >= 0 &&
        m_covariance[0][0] >= 0;
    if (!positive_semi_definite)
    {
        m_covariance[0][0] = 100.0;
        m_covariance[0][1] = 0.0;
        m_covariance[1][0] = 0.0;
        m_covariance[1][1] = 0.1;
        ++m_restarts;
    }

    m_slope = m_slope + k[0] * residual;
    m_previous_offset = m_offset;
    m_offset = m_offset + k[1] * residual;
    return true;
}

bool KalmanDelayFilter::update(const GroupDeltas& deltas, DelayState hypothesis) noexcept
{
    return update(fractional_ms(deltas.arrival_us), fractional_ms(deltas.send_us), deltas.bytes, hypothesis);
}

} // namespace crd::hesap::net
