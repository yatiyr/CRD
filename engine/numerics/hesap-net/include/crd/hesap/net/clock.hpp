// Clock offset and drift (HESAP-NET, capability numerics.network-estimators, owner crd-hesap-net).
//
//   on_wire_sample       RFC 5905 §8's offset and delay of one four-timestamp exchange, for clocks on any origin. A
//                        peer writes two of the four, so nothing here can overflow and an impossible exchange is
//                        refused.
//   ClockFilter          RFC 5905 §10's eight-stage minimum-delay filter, as ntpd 4.2.8p18's clock_filter(), bit for
//   bit
//                        (crd-hesap-clock-ntpd-tests), with its Allan-intercept metric and popcorn gate: the sample of
//                        least delay carries the least error.
//   ClockDriftFilter     A weighted least-squares line through the filtered samples, centred on the newest and shrunk
//                        by a prior on the frequency, over at most 64 samples. A Wald-Wolfowitz runs test on the
//                        residuals drops the oldest samples once a line no longer fits them, so a smear or a thermal
//                        change costs a shorter window, never a stale frequency.
//   ClockTracker         The three together for one peer: step detection on the raw intervals, drift compensation
//                        inside the filter, and an offset, frequency and error bound at any local time.
//   MediaClockMap        An RTP clock to NTP time line over 20 sender reports, as libwebrtc's RtpToNtpEstimator.
//   CaptureClockAligner  A capture device's clock translated to the system clock, monotonic and never in the future, as
//                        libwebrtc's TimestampAligner.
//
// The last two are bit-exact with branch-heads/8147's arithmetic (crd-hesap-estimators-webrtc-tests). Times are signed
// nanoseconds on any monotonic origin, except where a type says otherwise; ClockFilter and ClockDriftFilter hold
// doubles, so feed them offsets from a base near the first (ClockTracker does). Nothing allocates.
// Follows libwebrtc (BSD-3-Clause); notice in THIRD-PARTY-NOTICES.md.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/net/rate.hpp>

namespace crd::hesap::net
{

// a - b modulo 2^64: a peer's timestamp can sit anywhere in the range.
[[nodiscard]] constexpr crd::i64 clock_difference(crd::i64 a, crd::i64 b) noexcept
{
    return static_cast<crd::i64>(static_cast<crd::u64>(a) - static_cast<crd::u64>(b));
}

struct OnWireSample
{
    crd::i64 offset_ns = 0; // The peer's clock minus ours: ((T2 - T1) + (T3 - T4)) / 2, rounded down.
    crd::i64 delay_ns = 0;  // (T4 - T1) - (T3 - T2), at least the precision. The true offset is within delay / 2.
};

// T1 and T4 on our clock, T2 and T3 on the peer's. False, writing nothing, when our clock ran backwards, the peer's
// did, or the peer claims to have held the packet longer than the exchange took by more than precision_ns.
[[nodiscard]] constexpr bool on_wire_sample(crd::i64 t1, crd::i64 t2, crd::i64 t3, crd::i64 t4, crd::i64 precision_ns,
                                            OnWireSample& out) noexcept
{
    const crd::i64 exchange = clock_difference(t4, t1);
    const crd::i64 hold = clock_difference(t3, t2);
    if (precision_ns < 0 || exchange < 0 || hold < 0 || hold - exchange > precision_ns)
    {
        return false;
    }
    const crd::i64 out_leg = clock_difference(t2, t1);
    const crd::i64 back_leg = clock_difference(t3, t4);
    out.offset_ns = (out_leg >> 1U) + (back_leg >> 1U) + (out_leg & back_leg & crd::i64{1});
    const crd::i64 delay = exchange - hold;
    out.delay_ns = (delay > precision_ns) ? delay : precision_ns;
    return true;
}

struct ClockFilterOptions
{
    double precision_s = 1.0 / 1048576.0; // The clock's reading precision; the jitter's floor.
    double phi = 15e-6;                   // RFC 5905's frequency tolerance: dispersion grown per second.
    // ntpd's sys_maxdist. crd-lint-allow-untagged-physical: an NTP synchronization distance, a time in seconds.
    double max_distance_s = 1.5;                    // crd-lint-allow-untagged-physical
    crd::i64 allan_intercept_ns = 2048'000'000'000; // Past this age a sample's distance includes its dispersion.
};

enum class ClockFilterEvent : crd::u8
{
    Refused,  // An argument out of range, or time ran backwards: nothing changed.
    NoSample, // No stage holds a usable sample; only the dispersion was updated.
    Spike,    // A popcorn spike: the values moved, but use the previous ones.
    Stale,    // The least-delay sample is not newer than the last one used.
    Fresh,    // offset_s(), delay_s(), jitter_s() and epoch_ns() name a new sample.
};

class ClockFilter
{
public:
    static constexpr crd::u32 kStages = 8U;
    static constexpr double kMaxDispersionS = 16.0;
    static constexpr double kMaxMagnitudeS = 17179869184.0; // 2^34 s, past any difference of two i64 nanoseconds.
    static constexpr crd::i64 kMaxTimeNs = crd::i64{1} << 61U;

    struct Stage
    {
        double offset_s = 0.0;
        double delay_s = 0.0;
        double dispersion_s = kMaxDispersionS;
        crd::i64 epoch_ns = 0;
    };

    ClockFilter() noexcept = default;

    // Clears every stage; false, leaving the default options, for options out of range.
    [[nodiscard]] bool init(const ClockFilterOptions& options, crd::i64 now_ns) noexcept;

    // One exchange; poll_ns is the host's poll interval, the popcorn gate's horizon.
    [[nodiscard]] ClockFilterEvent update(crd::i64 now_ns, double offset_s, double delay_s, double dispersion_s,
                                          crd::i64 poll_ns) noexcept;

    // A poll that got no answer: RFC 5905's empty sample, which ages the filter towards NoSample.
    [[nodiscard]] ClockFilterEvent on_lost(crd::i64 now_ns, crd::i64 poll_ns) noexcept
    {
        return update(now_ns, 0.0, 0.0, kMaxDispersionS, poll_ns);
    }

    // Re-expresses the held offsets after the caller's model gained `frequency` from reference_ns on. Selection reads
    // only the delays, so the choice stays; false, changing nothing, for a frequency past 1 or out-of-range time.
    [[nodiscard]] bool slew(double frequency, crd::i64 reference_ns) noexcept;

    [[nodiscard]] double offset_s() const noexcept { return m_offset; }

    [[nodiscard]] double delay_s() const noexcept { return m_delay; }

    [[nodiscard]] double dispersion_s() const noexcept { return m_dispersion; }

    [[nodiscard]] double jitter_s() const noexcept { return m_jitter; }

    // The dispersion of the sample offset_s() came from, which RFC 5905's weighted dispersion mixes with the others'.
    [[nodiscard]] double sample_dispersion_s() const noexcept { return m_stages[m_order[0]].dispersion_s; }

    [[nodiscard]] crd::i64 epoch_ns() const noexcept { return m_epoch; }

    // Stage `index` of the register, in arrival order modulo kStages, and the index of the rank-th best.
    [[nodiscard]] const Stage& stage(crd::u32 index) const noexcept { return m_stages[index]; }

    [[nodiscard]] crd::u32 order(crd::u32 rank) const noexcept { return m_order[rank]; }

private:
    ClockFilterOptions m_options{};
    Stage m_stages[kStages] = {};
    crd::u8 m_order[kStages] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    crd::u32 m_next = 0U;
    crd::i64 m_update = 0;
    crd::i64 m_epoch = 0;
    double m_offset = 0.0;
    double m_delay = 0.0;
    double m_dispersion = kMaxDispersionS;
    double m_jitter = 1.0 / 1048576.0;
    bool m_has_epoch = false;
};

struct ClockDriftOptions
{
    // The prior standard deviation of the frequency, RFC 5905's 500 PPM tolerance: seconds per second, no unit.
    double frequency_sd = 500e-6; // crd-lint-allow-untagged-physical
};

class ClockDriftFilter
{
public:
    static constexpr crd::u32 kCapacity = 64U;
    static constexpr crd::u32 kRunsTestMinimum = 12U;
    static constexpr double kMaxMagnitudeS = 1099511627776.0; // 2^40 s.
    static constexpr double kMinUncertaintyS = 1e-12;

    ClockDriftFilter() noexcept = default;

    // Forgets every sample; false, leaving the default options, for a prior that is not positive and finite.
    [[nodiscard]] bool init(const ClockDriftOptions& options) noexcept;

    // One sample at a time after the newest, with its error half-width. False, changing nothing, when the time does not
    // advance or a value is out of range. The line is refitted before it returns.
    [[nodiscard]] bool add(crd::i64 time_ns, double offset_s, double uncertainty_s) noexcept;

    // True when the line at time_ns lies outside [offset_s - half_width_s, offset_s + half_width_s] by more than four
    // standard errors: the sample and the line cannot both be right. False before any sample.
    [[nodiscard]] bool contradicts(crd::i64 time_ns, double offset_s, double half_width_s) const noexcept;

    [[nodiscard]] crd::u32 count() const noexcept { return m_count; }

    // The newest sample's time; meaningful with count() > 0.
    [[nodiscard]] crd::i64 newest_ns() const noexcept { return m_newest_time; }

    // The fitted line: 0 before any sample.
    [[nodiscard]] double frequency() const noexcept { return m_slope; }

    [[nodiscard]] double offset_s_at(crd::i64 time_ns) const noexcept;

    // The line's standard errors, scaled by the residuals when those exceed the stated uncertainties; the options'
    // prior and kMaxMagnitudeS before any sample.
    [[nodiscard]] double frequency_sd() const noexcept;
    [[nodiscard]] double offset_sd_s_at(crd::i64 time_ns) const noexcept;

private:
    [[nodiscard]] crd::u32 slot(crd::u32 age) const noexcept { return (m_newest + kCapacity - age) % kCapacity; }

    [[nodiscard]] bool fit_passes_runs_test() noexcept;

    ClockDriftOptions m_options{};
    crd::i64 m_times[kCapacity] = {};
    double m_offsets[kCapacity] = {};
    double m_uncertainties[kCapacity] = {};
    crd::u32 m_newest = 0U;
    crd::u32 m_count = 0U;
    crd::i64 m_newest_time = 0;
    double m_mean_x = 0.0;
    double m_mean_y = 0.0;
    double m_slope = 0.0;
    double m_sum_w = 0.0;
    double m_sxx = 0.0;
    double m_scale = 1.0;
};

struct ClockTrackerOptions
{
    ClockFilterOptions filter{};
    ClockDriftOptions drift{};
    crd::i64 delay_window_ns = 600'000'000'000; // A route change forgets the old least delay after this long.
};

enum class ClockTrackerEvent : crd::u8
{
    Refused,  // An impossible exchange or time running backwards: nothing changed.
    Filtered, // Taken into the filter, with no new sample for the line yet.
    Updated,  // The line has a new sample.
    Spike,    // The exchange's interval contradicts the line, and has not been repeated: set aside.
    Step, // Three exchanges that agree contradict the line, or kStepForcedAfter in a row do: the clock moved, and the
          // estimate restarted from the last three keeping the frequency.
};

class ClockTracker
{
public:
    static constexpr crd::u32 kStepConfirmations = 3U;
    static constexpr crd::u32 kStepForcedAfter = 16U;

    ClockTracker() noexcept : m_min_delay(0U) {}

    // Restarts; false, leaving the defaults, for options out of range.
    [[nodiscard]] bool init(const ClockTrackerOptions& options) noexcept;

    // T1 and T4 on our clock, T2 and T3 on the peer's; poll_ns as for ClockFilter::update. T4 must not go backwards.
    [[nodiscard]] ClockTrackerEvent on_exchange(crd::i64 t1, crd::i64 t2, crd::i64 t3, crd::i64 t4,
                                                crd::i64 poll_ns) noexcept;

    // A poll that got no answer.
    [[nodiscard]] ClockTrackerEvent on_lost(crd::i64 now_ns, crd::i64 poll_ns) noexcept;

    // True once the line has a sample; the values below mean their "before any sample" forms until then.
    [[nodiscard]] bool synchronised() const noexcept { return m_drift.count() > 0U; }

    // The peer's clock minus ours at a local time, and the frequency of that difference (the peer's rate minus ours).
    [[nodiscard]] crd::i64 offset_ns_at(crd::i64 local_ns) const noexcept;

    [[nodiscard]] double frequency() const noexcept { return m_prior_frequency + m_drift.frequency(); }

    [[nodiscard]] double frequency_sd() const noexcept { return m_drift.frequency_sd(); }

    [[nodiscard]] double offset_sd_s_at(crd::i64 local_ns) const noexcept { return m_drift.offset_sd_s_at(local_ns); }

    // What the true offset is within, however the path's delay splits: half the least delay (an exchange cannot tell
    // its directions apart), four standard errors of the line and the frequency tolerance since its newest sample.
    // ClockFilter::kMaxDispersionS before any sample.
    [[nodiscard]] double offset_bound_s_at(crd::i64 local_ns) const noexcept;

    [[nodiscard]] crd::u32 steps() const noexcept { return m_steps; }

    [[nodiscard]] const ClockFilter& filter() const noexcept { return m_filter; }

    [[nodiscard]] const ClockDriftFilter& drift() const noexcept { return m_drift; }

private:
    struct Pending
    {
        crd::i64 time_ns = 0;
        crd::i64 offset_ns = 0;
        crd::i64 delay_ns = 0;
    };

    void anchor(crd::i64 time_ns, crd::i64 offset_ns) noexcept;
    [[nodiscard]] double frame_offset(crd::i64 time_ns, crd::i64 offset_ns) const noexcept;
    [[nodiscard]] double half_width(crd::i64 time_ns, crd::i64 delay_ns) const noexcept;
    [[nodiscard]] bool pending_agree() const noexcept;
    [[nodiscard]] ClockTrackerEvent feed(const Pending& sample, crd::i64 poll_ns) noexcept;
    [[nodiscard]] ClockTrackerEvent consume(ClockFilterEvent event) noexcept;

    ClockTrackerOptions m_options{};
    ClockFilter m_filter{};
    ClockDriftFilter m_drift{};
    WindowedFilter<double, AtMost<double>> m_min_delay;
    Pending m_pending[kStepConfirmations] = {};
    crd::u32 m_pending_count = 0U;
    crd::u32 m_contradictions = 0U;
    crd::u32 m_steps = 0U;
    crd::i64 m_precision_ns = 1;
    crd::i64 m_base_ns = 0; // The frame the two filters work in: offset - base - prior * (time - origin).
    crd::i64 m_origin_ns = 0;
    crd::i64 m_last_ns = 0;
    // Carried over a step, so the new line's prior centres on what was learned; seconds per second.
    double m_prior_frequency = 0.0; // crd-lint-allow-untagged-physical
    // Removed from the offsets inside the filter.
    double m_filter_frequency = 0.0; // crd-lint-allow-untagged-physical
    double m_path_noise = 0.0;       // A running mean of how far the used samples sat from the least delay.
    bool m_has_path_noise = false;
    bool m_started = false;
};

enum class MediaClockUpdate : crd::u8
{
    Invalid, // Out of order or a jump; after three in a row the next report restarts the map.
    Same,    // A report already held.
    New,
};

// y = slope * x + offset over the last kReports (RTP timestamp unwrapped, NTP timestamp in Q32.32) pairs.
class MediaClockMap
{
public:
    static constexpr crd::u32 kReports = 20U;
    static constexpr crd::i32 kMaxInvalidReports = 3;

    [[nodiscard]] MediaClockUpdate update(crd::u64 ntp_q32, crd::u32 rtp_timestamp) noexcept;

    // The NTP time of an RTP timestamp, or 0 without a line yet. Moves the unwrapping reference, like the reference.
    [[nodiscard]] crd::u64 estimate(crd::u32 rtp_timestamp) noexcept;

    // The RTP clock's rate, or 0 without a line yet.
    [[nodiscard]] double frequency_khz() const noexcept;

    // The line, NTP units per RTP tick and at unwrapped timestamp 0; false without a line yet.
    [[nodiscard]] bool has_line() const noexcept { return m_has_line; }

    [[nodiscard]] double slope() const noexcept { return m_slope; }

    [[nodiscard]] double offset() const noexcept { return m_offset; }

private:
    [[nodiscard]] crd::i64 unwrap(crd::u32 rtp_timestamp) noexcept;

    crd::u64 m_ntp[kReports] = {};
    crd::i64 m_rtp[kReports] = {};
    crd::u32 m_newest = 0U;
    crd::u32 m_count = 0U;
    crd::i32 m_consecutive_invalid = 0;
    crd::i64 m_last_unwrapped = 0;
    crd::u32 m_last_rtp = 0U;
    double m_slope = 0.0;
    double m_offset = 0.0;
    bool m_has_rtp = false;
    bool m_has_line = false;
};

class CaptureClockAligner
{
public:
    static constexpr crd::i64 kMaxTimeUs = crd::i64{1} << 61U;

    // The system time of a frame the capturer stamped: its own clock's rate, the system clock's position. False,
    // changing nothing, when either time is past kMaxTimeUs.
    [[nodiscard]] bool translate(crd::i64 capturer_us, crd::i64 system_us, crd::i64& out_us) noexcept;

    // Without a system reading: the offset the last translate() used.
    [[nodiscard]] crd::i64 translate(crd::i64 capturer_us) const noexcept { return capturer_us + m_previous_offset_us; }

private:
    crd::i64 m_frames_seen = 0;
    crd::i64 m_offset_us = 0;
    crd::i64 m_clip_bias_us = 0;
    crd::i64 m_previous_translated_us = -0x7FFFFFFFFFFFFFFF - 1;
    crd::i64 m_previous_offset_us = 0;
};

} // namespace crd::hesap::net
