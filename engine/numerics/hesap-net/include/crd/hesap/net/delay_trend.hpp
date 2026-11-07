// One-way delay trend (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net): Google Congestion
// Control's signals, draft-ietf-rmcat-gcc-02 §5 as libwebrtc runs them, without allocation and bit-exact with its
// arithmetic around one shared pow (crd-hesap-estimators-webrtc-tests).
//
//   PacketGroupTracker    packets into send-time groups (bursts merged, reordering dropped), deltas between groups;
//   TrendlineFilter       the least-squares slope of the smoothed accumulated delay over a window of groups;
//   KalmanDelayFilter     the two-state Kalman filter of the delay offset and the inverse capacity;
//   DelayOveruseDetector  the adaptive threshold that turns either signal into Normal, Overusing or Underusing.
//
// The feedback that carries arrival times comes from the peer, so every entry point refuses what would leave the
// domain (non-finite numbers, times past kMaxTimeUs) with its state unchanged, a covariance that rounding pushed off
// positive semi-definite restarts where libwebrtc only logs, and so does a noise estimate that left the finite numbers
// where libwebrtc carries the infinity on. Within the domain every state stays finite and the threshold stays in
// [6, 600] whatever the input. A send delta can be negative: a burst's later send time joins an earlier group.
//
// Rate control and the loss-based controller are NET-PATH's and NET-LIVE's; these are their inputs.
// Follows libwebrtc (BSD-3-Clause); notice in THIRD-PARTY-NOTICES.md.

#pragma once

#include <crd/core/types.hpp>

#include <span>

namespace crd::hesap::net
{

enum class DelayState : crd::u8
{
    Normal,
    Overusing,
    Underusing,
};

// The deltas between two complete send-time groups.
struct GroupDeltas
{
    crd::i64 send_us = 0;
    crd::i64 arrival_us = 0;
    crd::i32 bytes = 0; // Saturated to the i32 range.
};

class PacketGroupTracker
{
public:
    static constexpr crd::i64 kDefaultGroupUs = 5'000;
    static constexpr crd::i64 kBurstDeltaUs = 5'000;
    static constexpr crd::i64 kMaxBurstUs = 100'000;
    static constexpr crd::i64 kArrivalJumpUs = 3'000'000;
    static constexpr crd::u32 kReorderedReset = 3U;
    static constexpr crd::i64 kMaxTimeUs = crd::i64{1} << 60U;

    constexpr explicit PacketGroupTracker(crd::i64 group_us = kDefaultGroupUs) noexcept : m_group_us(group_us) {}

    // One packet: when it was sent, when it arrived on the far clock and when the feedback saying so arrived on the
    // local clock (a jump between the two restarts the tracker). True when a group completed against an earlier one,
    // with `out` set; `out` is untouched otherwise.
    [[nodiscard]] bool add(crd::i64 send_us, crd::i64 arrival_us, crd::i64 system_us, crd::u32 bytes,
                           GroupDeltas& out) noexcept;

    void reset() noexcept;

private:
    struct Group
    {
        crd::i64 first_send_us = 0;
        crd::i64 send_us = 0;
        crd::i64 first_arrival_us = 0;
        crd::i64 complete_us = 0;
        crd::i64 system_us = 0;
        crd::u64 bytes = 0U;
        bool started = false;
    };

    [[nodiscard]] bool belongs_to_burst(crd::i64 send_us, crd::i64 arrival_us) const noexcept;

    Group m_current;
    Group m_previous;
    crd::i64 m_group_us;
    crd::u32 m_reordered = 0U;
};

class DelayOveruseDetector
{
public:
    // Feeds one filtered signal. Fewer than two deltas decide nothing; otherwise min(deltas, 60) * signal * gain is
    // compared with the adaptive threshold, and overuse is declared once it has held longer than 10 ms of send time
    // over more than one signal while the signal is not falling.
    DelayState update(double signal, crd::i32 deltas, double gain, double send_delta_ms, crd::i64 now_ms) noexcept;

    [[nodiscard]] DelayState state() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] double threshold() const noexcept
    {
        return m_threshold;
    }

    // The signal of the last decision, 0 before any.
    [[nodiscard]] double previous_signal() const noexcept
    {
        return m_previous;
    }

private:
    void adapt(double scaled, crd::i64 now_ms) noexcept;

    double m_threshold = 12.5;
    double m_previous = 0.0;
    double m_over_ms = -1.0;
    crd::i64 m_last_update_ms = -1;
    crd::i32 m_over_count = 0;
    DelayState m_state = DelayState::Normal;
};

struct TrendlineSample
{
    double arrival_ms = 0.0;
    double smoothed_delay_ms = 0.0;
    double raw_delay_ms = 0.0;
};

struct TrendlineOptions
{
    bool sort = false; // Keep the window in arrival order.
    bool cap = false;  // Cap a rising slope by the delays' lowest points at the two ends of the window.
    crd::u32 beginning_packets = 7U;
    crd::u32 end_packets = 7U;
    double cap_uncertainty = 0.0;
};

class TrendlineFilter
{
public:
    static constexpr crd::u32 kMinWindow = 10U;
    static constexpr crd::u32 kMaxWindow = 200U;
    static constexpr crd::u32 kDefaultWindow = 20U;
    static constexpr double kMaxDeltaMs = 1e16; // Past any delta of two kMaxTimeUs times, in milliseconds.

    // The samples a window needs of the storage init() takes.
    [[nodiscard]] static constexpr crd::usize storage_samples(crd::u32 window) noexcept
    {
        return crd::usize{window} + 1U;
    }

    // False, leaving the filter refusing every update, for a window outside [10, 200], storage shorter than
    // storage_samples() or cap options out of range (each end 1 or more, together at most the window, the uncertainty
    // in [0, 0.025]).
    [[nodiscard]] bool init(std::span<TrendlineSample> storage, crd::u32 window = kDefaultWindow,
                            TrendlineOptions options = {}) noexcept;

    // One pair of group deltas, in milliseconds, and the group's arrival time in whole milliseconds. False, changing
    // nothing, before init() or for a delta outside the domain.
    [[nodiscard]] bool update(double recv_delta_ms, double send_delta_ms, crd::i64 arrival_ms) noexcept;

    // The tracker's deltas and the arrival time of the packet that completed them, in microseconds.
    [[nodiscard]] bool update(const GroupDeltas& deltas, crd::i64 arrival_us) noexcept;

    [[nodiscard]] DelayState state() const noexcept
    {
        return m_detector.state();
    }

    // The latest slope, (send rate - capacity) / capacity while queues build.
    [[nodiscard]] double trend() const noexcept
    {
        return m_detector.previous_signal();
    }

    [[nodiscard]] const DelayOveruseDetector& detector() const noexcept
    {
        return m_detector;
    }

    [[nodiscard]] double accumulated_delay_ms() const noexcept
    {
        return m_accumulated;
    }

    [[nodiscard]] double smoothed_delay_ms() const noexcept
    {
        return m_smoothed;
    }

private:
    [[nodiscard]] crd::u32 slot(crd::u32 index) const noexcept;
    [[nodiscard]] double fitted_slope(double fallback) const noexcept;
    [[nodiscard]] bool slope_cap(double& cap) const noexcept;

    std::span<TrendlineSample> m_storage;
    DelayOveruseDetector m_detector;
    TrendlineOptions m_options;
    double m_accumulated = 0.0;
    double m_smoothed = 0.0;
    crd::i64 m_first_arrival_ms = -1;
    crd::u32 m_window = 0U;
    crd::u32 m_head = 0U;
    crd::u32 m_count = 0U;
    crd::i32 m_deltas = 0;
};

class KalmanDelayFilter
{
public:
    static constexpr crd::u32 kFramePeriodHistory = 60U;

    // One pair of group deltas, the size difference between the groups and the detector's state before this update.
    // False, changing nothing, outside the domain.
    [[nodiscard]] bool update(double recv_delta_ms, double send_delta_ms, crd::i32 size_delta,
                              DelayState hypothesis) noexcept;

    // The tracker's deltas.
    [[nodiscard]] bool update(const GroupDeltas& deltas, DelayState hypothesis) noexcept;

    // The estimated delay offset between groups, the signal for DelayOveruseDetector (gain 1), in milliseconds.
    [[nodiscard]] double offset() const noexcept
    {
        return m_offset;
    }

    // The estimated inverse capacity, in milliseconds per byte.
    [[nodiscard]] double slope() const noexcept
    {
        return m_slope;
    }

    [[nodiscard]] double noise_variance() const noexcept
    {
        return m_noise_variance;
    }

    [[nodiscard]] crd::i32 deltas() const noexcept
    {
        return m_deltas;
    }

    // How many times the covariance or the noise estimate left its domain and restarted.
    [[nodiscard]] crd::u32 restarts() const noexcept
    {
        return m_restarts;
    }

private:
    [[nodiscard]] double min_frame_period(double send_delta_ms) noexcept;
    void update_noise(double residual, double period_ms, bool stable) noexcept;

    double m_history[kFramePeriodHistory] = {};
    double m_covariance[2][2] = {{100.0, 0.0}, {0.0, 0.1}};
    double m_slope = 8.0 / 512.0;
    double m_offset = 0.0;
    double m_previous_offset = 0.0;
    double m_average_noise = 0.0;
    double m_noise_variance = 50.0;
    crd::u32 m_history_head = 0U;
    crd::u32 m_history_count = 0U;
    crd::u32 m_restarts = 0U;
    crd::i32 m_deltas = 0;
};

} // namespace crd::hesap::net
