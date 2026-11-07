#include <crd/hesap/net/rate.hpp>

#include <algorithm>
#include <cmath>

// The expressions below keep libwebrtc's order of operations and its float arithmetic: the bit-exact oracle holds them
// to it (crd-hesap-estimators-webrtc-tests).
// Follows libwebrtc (BSD-3-Clause); notice in THIRD-PARTY-NOTICES.md.

namespace crd::hesap::net
{

namespace
{

[[nodiscard]] bool usable(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

} // namespace

bool AckedRateEstimator::init(const AckedRateOptions& options) noexcept
{
    const bool valid = options.initial_window_ms >= kMinWindowMs && options.initial_window_ms <= kMaxWindowMs &&
                       options.window_ms >= kMinWindowMs && options.window_ms <= kMaxWindowMs &&
                       options.small_sample_bytes >= 0 && usable(options.uncertainty_scale) &&
                       usable(options.application_limited_scale) && usable(options.small_sample_scale) &&
                       usable(options.symmetry_cap_kbps) && usable(options.floor_kbps);
    *this = AckedRateEstimator{};
    if (valid)
    {
        m_options = options;
    }
    return valid;
}

float AckedRateEstimator::update_window(crd::i64 now_ms, crd::u32 bytes, crd::i32 window_ms, bool& small) noexcept
{
    if (now_ms < m_previous_ms)
    {
        m_previous_ms = -1;
        m_sum_bytes = 0;
        m_window_elapsed_ms = 0;
    }
    if (m_previous_ms >= 0)
    {
        m_window_elapsed_ms += now_ms - m_previous_ms;
        if (now_ms - m_previous_ms > window_ms)
        {
            m_sum_bytes = 0;
            m_window_elapsed_ms %= window_ms;
        }
    }
    m_previous_ms = now_ms;
    float sample = -1.0F;
    if (m_window_elapsed_ms >= window_ms)
    {
        small = m_sum_bytes < m_options.small_sample_bytes;
        sample = 8.0F * static_cast<float>(m_sum_bytes) / static_cast<float>(window_ms);
        m_window_elapsed_ms -= window_ms;
        m_sum_bytes = 0;
    }
    m_sum_bytes += bytes;
    return sample;
}

void AckedRateEstimator::update(crd::i64 now_ms, crd::u32 bytes, bool application_limited) noexcept
{
    if (now_ms < -kMaxTimeMs || now_ms > kMaxTimeMs)
    {
        return;
    }
    const crd::i32 window_ms = m_estimate_kbps < 0.0F ? m_options.initial_window_ms : m_options.window_ms;
    bool small = false;
    const float sample = update_window(now_ms, bytes, window_ms, small);
    if (sample < 0.0F)
    {
        return;
    }
    if (m_estimate_kbps <= 0.0F)
    {
        m_estimate_kbps = sample; // The first sample, or an estimate of zero, which no rule below can move.
        return;
    }
    float scale = m_options.uncertainty_scale;
    if (small && sample < m_estimate_kbps)
    {
        scale = m_options.small_sample_scale;
    }
    else if (application_limited && sample < m_estimate_kbps)
    {
        scale = m_options.application_limited_scale;
    }
    // The further a sample is from the estimate the less it is believed; a low cap believes falls more than rises.
    const float uncertainty = scale * std::abs(m_estimate_kbps - sample) /
                              (m_estimate_kbps + std::min(sample, m_options.symmetry_cap_kbps));
    const float sample_variance = uncertainty * uncertainty;
    const float predicted_variance = m_variance + 5.0F;
    if (!std::isfinite(sample_variance))
    {
        m_variance = predicted_variance; // The formulas' limit: an unbelievable sample moves nothing.
        return;
    }
    m_estimate_kbps = (sample_variance * m_estimate_kbps + predicted_variance * sample) /
                      (sample_variance + predicted_variance);
    m_estimate_kbps = std::max(m_estimate_kbps, m_options.floor_kbps);
    m_variance = sample_variance * predicted_variance / (sample_variance + predicted_variance);
}

float AckedRateEstimator::peek_kbps() const noexcept
{
    return m_window_elapsed_ms > 0
               ? 8.0F * static_cast<float>(m_sum_bytes) / static_cast<float>(m_window_elapsed_ms)
               : 0.0F;
}

void AckedRateEstimator::expect_fast_change() noexcept
{
    m_variance += 200;
}

} // namespace crd::hesap::net
