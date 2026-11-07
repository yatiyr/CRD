// Bandwidth estimation (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net).
//
//   WindowedFilter       Kathleen Nichols' three-sample windowed extremum, the running maximum of BBR's bandwidth and
//                        minimum of its round trip; constant memory, any ordered value, time in any monotonic unit.
//   delivery_rate        one sample of the rate packets were delivered at, bounded by the rate they were sent at
//                        (draft-ietf-ccwg-bbr's delivery rate).
//   AckedRateEstimator   the Bayesian estimate of the acknowledged rate Google Congestion Control feeds its rate
//                        control, bit-exact with libwebrtc's arithmetic (crd-hesap-estimators-webrtc-tests), which
//                        turns to NaN on an estimate of zero and on a sample variance past float; here the first
//                        restarts from the sample and the second keeps the estimate.
//
// Nothing allocates.
// Follows libwebrtc (BSD-3-Clause); notice in THIRD-PARTY-NOTICES.md.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::net
{

template <class T>
struct AtLeast
{
    [[nodiscard]] constexpr bool operator()(const T& candidate, const T& best) const noexcept
    {
        return candidate >= best;
    }
};

template <class T>
struct AtMost
{
    [[nodiscard]] constexpr bool operator()(const T& candidate, const T& best) const noexcept
    {
        return candidate <= best;
    }
};

// The best sample of the last `window` time units, and the second and third best of its later parts that replace it
// when it ages out. best() is a sample of the window and never better than the window's true best; a sample worse
// than the three held can be lost while it is the true best of a later part of the window, the filter's known bound.
// The refreshes test the held samples' times, as Linux's lib/win_minmax.c does; quiche tests their values, which differs
// only when different samples tie.
template <class T, class AsGood>
class WindowedFilter
{
public:
    constexpr explicit WindowedFilter(crd::u64 window) noexcept : m_window(window) {}

    // A time before the third sample's reads as a full window later: the filter restarts from the sample.
    constexpr void update(T sample, crd::u64 time) noexcept
    {
        const AsGood as_good{};
        if (!m_started || as_good(sample, m_samples[0].value) || time - m_samples[2].time > m_window)
        {
            restart(sample, time);
            return;
        }
        if (as_good(sample, m_samples[1].value))
        {
            m_samples[1] = {sample, time};
            m_samples[2] = m_samples[1];
        }
        else if (as_good(sample, m_samples[2].value))
        {
            m_samples[2] = {sample, time};
        }
        if (time - m_samples[0].time > m_window)
        {
            m_samples[0] = m_samples[1];
            m_samples[1] = m_samples[2];
            m_samples[2] = {sample, time};
            if (time - m_samples[0].time > m_window)
            {
                m_samples[0] = m_samples[1];
                m_samples[1] = m_samples[2];
            }
            return;
        }
        if (m_samples[1].time == m_samples[0].time && time - m_samples[1].time > m_window / 4U)
        {
            m_samples[1] = {sample, time}; // A quarter of the window passed: take the second best from what follows.
            m_samples[2] = m_samples[1];
            return;
        }
        if (m_samples[2].time == m_samples[1].time && time - m_samples[2].time > m_window / 2U)
        {
            m_samples[2] = {sample, time}; // Half of the window passed: take the third best from what follows.
        }
    }

    constexpr void restart(T sample, crd::u64 time) noexcept
    {
        m_samples[0] = {sample, time};
        m_samples[1] = m_samples[0];
        m_samples[2] = m_samples[0];
        m_started = true;
    }

    constexpr void set_window(crd::u64 window) noexcept
    {
        m_window = window;
    }

    [[nodiscard]] constexpr bool has_sample() const noexcept
    {
        return m_started;
    }

    // A default T before any sample.
    [[nodiscard]] constexpr T best() const noexcept
    {
        return m_samples[0].value;
    }

    [[nodiscard]] constexpr T second() const noexcept
    {
        return m_samples[1].value;
    }

    [[nodiscard]] constexpr T third() const noexcept
    {
        return m_samples[2].value;
    }

private:
    struct Timed
    {
        T value{};
        crd::u64 time = 0U;
    };

    Timed m_samples[3] = {};
    crd::u64 m_window;
    bool m_started = false;
};

template <class T>
using WindowedMax = WindowedFilter<T, AtLeast<T>>;

template <class T>
using WindowedMin = WindowedFilter<T, AtMost<T>>;

// Bytes per second over the longer of the send and acknowledgement intervals of the delivered bytes, so an ACK burst
// cannot read as more than the rate the data left at. 0 when neither interval is positive.
[[nodiscard]] constexpr double delivery_rate(crd::u64 delivered_bytes, crd::i64 send_elapsed_ns,
                                             crd::i64 ack_elapsed_ns) noexcept
{
    const crd::i64 interval = (send_elapsed_ns > ack_elapsed_ns) ? send_elapsed_ns : ack_elapsed_ns;
    return (interval > 0) ? static_cast<double>(delivered_bytes) * 1e9 / static_cast<double>(interval) : 0.0;
}

struct AckedRateOptions
{
    crd::i32 initial_window_ms = 500; // The first sample's window, in [150, 1000].
    crd::i32 window_ms = 150;         // Every later window, in [150, 1000].
    float uncertainty_scale = 10.0F;
    float application_limited_scale = 10.0F; // For a falling sample while the sender is application limited.
    float small_sample_scale = 10.0F;        // For a falling sample of fewer than small_sample_bytes.
    crd::i64 small_sample_bytes = 0;
    float symmetry_cap_kbps = 0.0F; // Higher values make the uncertainty of rises and falls more alike.
    float floor_kbps = 0.0F;
};

class AckedRateEstimator
{
public:
    static constexpr crd::i32 kMinWindowMs = 150;
    static constexpr crd::i32 kMaxWindowMs = 1000;
    static constexpr crd::i64 kMaxTimeMs = crd::i64{1} << 60U;

    // Default options; init() takes others.
    AckedRateEstimator() noexcept = default;

    // False, keeping the defaults, for a window outside [150, 1000], a negative byte bound or a scale, cap or floor
    // that is negative or not finite.
    [[nodiscard]] bool init(const AckedRateOptions& options) noexcept;

    // Bytes acknowledged at now_ms. Time moving backwards restarts the window, and a gap of more than a window drops
    // what it held. A time past kMaxTimeMs is refused.
    void update(crd::i64 now_ms, crd::u32 bytes, bool application_limited) noexcept;

    [[nodiscard]] bool has_estimate() const noexcept
    {
        return m_estimate_kbps >= 0.0F;
    }

    // The estimate in kilobits per second, negative before the first sample.
    [[nodiscard]] float kbps() const noexcept
    {
        return m_estimate_kbps;
    }

    [[nodiscard]] float variance() const noexcept
    {
        return m_variance;
    }

    // The rate of the window in progress, in kilobits per second; 0 when it has not started.
    [[nodiscard]] float peek_kbps() const noexcept;

    // Lets the estimate move fast for the next few samples, after a known change such as a probe or a new path.
    void expect_fast_change() noexcept;

private:
    [[nodiscard]] float update_window(crd::i64 now_ms, crd::u32 bytes, crd::i32 window_ms, bool& small) noexcept;

    AckedRateOptions m_options;
    crd::i64 m_sum_bytes = 0;
    crd::i64 m_window_elapsed_ms = 0;
    crd::i64 m_previous_ms = -1;
    float m_estimate_kbps = -1.0F;
    float m_variance = 50.0F;
};

} // namespace crd::hesap::net
