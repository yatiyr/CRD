#include <crd/hesap/net/clock.hpp>

#include <algorithm>
#include <cmath>

// ClockFilter keeps ntpd's order of operations, MediaClockMap and CaptureClockAligner libwebrtc's: the bit-exact
// oracles hold them to it (crd-hesap-clock-ntpd-tests, crd-hesap-estimators-webrtc-tests).
// Follows libwebrtc (BSD-3-Clause); notice in THIRD-PARTY-NOTICES.md.

namespace crd::hesap::net
{

namespace
{

constexpr double kNsPerSecond = 1e9;
constexpr double kContradictionSigmas = 4.0;
constexpr double kBoundSigmas = 4.0;

// One-sided 1% of the normal distribution: a window is cut only when too few runs make chance an unlikely reason.
constexpr double kRunsTestZ = 2.326;

// A residual below this share of the sample's own error has no sign worth counting.
constexpr double kResidualSignFloor = 1e-3;

constexpr double kPathNoiseGain = 0.125;

[[nodiscard]] double seconds(crd::i64 ns) noexcept
{
    return static_cast<double>(ns) / kNsPerSecond;
}

[[nodiscard]] bool time_in_range(crd::i64 ns) noexcept
{
    return ns >= -ClockFilter::kMaxTimeNs && ns <= ClockFilter::kMaxTimeNs;
}

[[nodiscard]] bool poll_in_range(crd::i64 ns) noexcept
{
    return ns > 0 && ns <= ClockFilter::kMaxTimeNs;
}

[[nodiscard]] bool within(double value, double low, double high) noexcept
{
    return std::isfinite(value) && value >= low && value <= high;
}

// libwebrtc's two-pass averages and sums, weighted: a weight of 1 changes no bit, so the unit-weight path is its line.
struct LineSums
{
    double sum_w = 0.0;
    double mean_x = 0.0;
    double mean_y = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
};

template <class Point> [[nodiscard]] LineSums line_sums(crd::u32 count, const Point& point) noexcept
{
    LineSums sums;
    for (crd::u32 i = 0U; i < count; ++i)
    {
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        point(i, x, y, w);
        sums.sum_w += w;
        sums.mean_x += w * x;
        sums.mean_y += w * y;
    }
    sums.mean_x /= sums.sum_w;
    sums.mean_y /= sums.sum_w;
    for (crd::u32 i = 0U; i < count; ++i)
    {
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        point(i, x, y, w);
        const double normal_x = x - sums.mean_x;
        const double normal_y = y - sums.mean_y;
        sums.sxx += w * (normal_x * normal_x);
        sums.sxy += w * (normal_x * normal_y);
    }
    return sums;
}

} // namespace

bool ClockFilter::init(const ClockFilterOptions& options, crd::i64 now_ns) noexcept
{
    const bool valid = within(options.precision_s, 1e-15, 1.0) && within(options.phi, 0.0, 1.0) &&
                       within(options.max_distance_s, 0.0, kMaxDispersionS) && options.allan_intercept_ns >= 0 &&
                       options.allan_intercept_ns <= kMaxTimeNs && time_in_range(now_ns);
    *this = ClockFilter{};
    if (valid)
    {
        m_options = options;
        m_jitter = options.precision_s;
        m_update = now_ns;
    }
    return valid;
}

ClockFilterEvent ClockFilter::update(crd::i64 now_ns, double offset_s, double delay_s, double dispersion_s,
                                     crd::i64 poll_ns) noexcept
{
    if (!time_in_range(now_ns) || now_ns < m_update || !poll_in_range(poll_ns) ||
        !within(offset_s, -kMaxMagnitudeS, kMaxMagnitudeS) || !within(delay_s, 0.0, kMaxMagnitudeS) ||
        !within(dispersion_s, 0.0, kMaxDispersionS))
    {
        return ClockFilterEvent::Refused;
    }
    m_stages[m_next] = {offset_s, delay_s, dispersion_s, now_ns};
    crd::u32 stage = (m_next + 1U) % kStages;
    m_next = stage;

    // The dispersions grow since the last update; the distance is the delay, plus the dispersion past the Allan
    // intercept, and the rank's stage runs oldest (rank 7) to newest (rank 0) before the sort.
    const double growth = m_options.phi * seconds(now_ns - m_update);
    m_update = now_ns;
    double distance[kStages];
    for (crd::u32 rank = kStages; rank-- > 0U;)
    {
        Stage& held = m_stages[stage];
        if (rank != 0U)
        {
            held.dispersion_s += growth;
        }
        if (held.dispersion_s >= kMaxDispersionS)
        {
            held.dispersion_s = kMaxDispersionS;
            distance[rank] = kMaxDispersionS;
        }
        else if (m_update - held.epoch_ns > m_options.allan_intercept_ns)
        {
            distance[rank] = held.delay_s + held.dispersion_s;
        }
        else
        {
            distance[rank] = held.delay_s;
        }
        m_order[rank] = static_cast<crd::u8>(stage);
        stage = (stage + 1U) % kStages;
    }

    // ntpd's exchange sort: not stable, so ties rank as they do there.
    for (crd::u32 i = 1U; i < kStages; ++i)
    {
        for (crd::u32 j = 0U; j < i; ++j)
        {
            if (distance[j] > distance[i])
            {
                const crd::u8 order = m_order[j];
                m_order[j] = m_order[i];
                m_order[i] = order;
                const double held_distance = distance[j];
                distance[j] = distance[i];
                distance[i] = held_distance;
            }
        }
    }

    // Past the least-delay two, a sample beyond the maximum distance leaves the jitter; none past the maximum
    // dispersion counts at all.
    crd::u32 usable = 0U;
    for (crd::u32 rank = 0U; rank < kStages; ++rank)
    {
        if (distance[rank] < kMaxDispersionS && (usable < 2U || distance[rank] < m_options.max_distance_s))
        {
            ++usable;
        }
    }

    m_dispersion = 0.0;
    m_jitter = 0.0;
    const crd::u32 best = m_order[0];
    for (crd::u32 rank = kStages; rank-- > 0U;)
    {
        const Stage& held = m_stages[m_order[rank]];
        m_dispersion = 0.5 * (m_dispersion + held.dispersion_s);
        if (rank < usable)
        {
            const double difference = held.offset_s - m_stages[best].offset_s;
            m_jitter += difference * difference;
        }
    }
    if (usable == 0U)
    {
        return ClockFilterEvent::NoSample;
    }

    const Stage& chosen = m_stages[best];
    const double moved = std::fabs(m_offset - chosen.offset_s);
    m_offset = chosen.offset_s;
    m_delay = chosen.delay_s;
    if (usable > 1U)
    {
        m_jitter /= static_cast<double>(usable - 1U);
    }
    const double jitter = std::sqrt(m_jitter);
    m_jitter = (jitter > m_options.precision_s) ? jitter : m_options.precision_s;

    if (m_has_epoch && chosen.epoch_ns >= m_epoch && m_dispersion < m_options.max_distance_s &&
        chosen.dispersion_s < m_options.max_distance_s && moved > 3.0 * m_jitter &&
        chosen.epoch_ns - m_epoch < poll_ns * 2)
    {
        return ClockFilterEvent::Spike;
    }
    if (m_has_epoch && chosen.epoch_ns <= m_epoch)
    {
        return ClockFilterEvent::Stale;
    }
    m_epoch = chosen.epoch_ns;
    m_has_epoch = true;
    return ClockFilterEvent::Fresh;
}

bool ClockFilter::slew(double frequency, crd::i64 reference_ns) noexcept
{
    if (!within(frequency, -1.0, 1.0) || !time_in_range(reference_ns))
    {
        return false;
    }
    for (Stage& held : m_stages)
    {
        held.offset_s -= frequency * seconds(held.epoch_ns - reference_ns);
    }
    if (m_has_epoch)
    {
        m_offset -= frequency * seconds(m_epoch - reference_ns);
    }
    return true;
}

bool ClockDriftFilter::init(const ClockDriftOptions& options) noexcept
{
    const bool valid = within(options.frequency_sd, 1e-12, 1.0);
    *this = ClockDriftFilter{};
    if (valid)
    {
        m_options = options;
    }
    return valid;
}

bool ClockDriftFilter::fit_passes_runs_test() noexcept
{
    const double newest_offset = m_offsets[m_newest];
    const auto point = [this, newest_offset](crd::u32 age, double& x, double& y, double& w) noexcept
    {
        const crd::u32 index = slot(age);
        x = seconds(m_times[index] - m_newest_time);
        y = m_offsets[index] - newest_offset;
        w = 1.0 / (m_uncertainties[index] * m_uncertainties[index]);
    };
    const LineSums sums = line_sums(m_count, point);
    const double ridge = 1.0 / (m_options.frequency_sd * m_options.frequency_sd);
    m_mean_x = sums.mean_x;
    m_mean_y = sums.mean_y;
    m_sum_w = sums.sum_w;
    m_sxx = sums.sxx + ridge;
    m_slope = sums.sxy / m_sxx;

    double chi_square = 0.0;
    double above = 0.0;
    double below = 0.0;
    double runs = 0.0;
    double previous_sign = 0.0;
    for (crd::u32 age = 0U; age < m_count; ++age)
    {
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        point(age, x, y, w);
        const double residual = (y - m_mean_y) - (m_slope * (x - m_mean_x));
        chi_square += w * (residual * residual);
        if (std::fabs(residual) > kResidualSignFloor * m_uncertainties[slot(age)])
        {
            const double sign = (residual > 0.0) ? 1.0 : -1.0;
            (sign > 0.0 ? above : below) += 1.0;
            runs += (sign != previous_sign) ? 1.0 : 0.0;
            previous_sign = sign;
        }
    }
    const double degrees = static_cast<double>(m_count) - 2.0;
    m_scale = (degrees > 0.0 && chi_square > degrees) ? chi_square / degrees : 1.0;

    const double signed_count = above + below;
    if (signed_count < static_cast<double>(kRunsTestMinimum))
    {
        return true;
    }
    if (above == 0.0 || below == 0.0)
    {
        return false;
    }
    const double mean_runs = (2.0 * above * below / signed_count) + 1.0;
    const double variance = (mean_runs - 1.0) * (mean_runs - 2.0) / (signed_count - 1.0);
    return variance <= 0.0 || (runs + 0.5 - mean_runs) >= -kRunsTestZ * std::sqrt(variance);
}

bool ClockDriftFilter::add(crd::i64 time_ns, double offset_s, double uncertainty_s) noexcept
{
    if (!time_in_range(time_ns) || !within(offset_s, -kMaxMagnitudeS, kMaxMagnitudeS) ||
        !within(uncertainty_s, kMinUncertaintyS, kMaxMagnitudeS) || (m_count > 0U && time_ns <= m_newest_time))
    {
        return false;
    }
    m_newest = (m_newest + 1U) % kCapacity;
    m_times[m_newest] = time_ns;
    m_offsets[m_newest] = offset_s;
    m_uncertainties[m_newest] = uncertainty_s;
    m_newest_time = time_ns;
    m_count = (m_count < kCapacity) ? m_count + 1U : kCapacity;

    // The oldest go first, an eighth at a time, until a line fits what is left.
    while (!fit_passes_runs_test())
    {
        const crd::u32 cut = m_count / 8U;
        m_count -= (cut > 0U) ? cut : 1U;
    }
    return true;
}

double ClockDriftFilter::offset_s_at(crd::i64 time_ns) const noexcept
{
    if (m_count == 0U)
    {
        return 0.0;
    }
    const double x = seconds(clock_difference(time_ns, m_newest_time));
    return m_offsets[m_newest] + m_mean_y + (m_slope * (x - m_mean_x));
}

double ClockDriftFilter::frequency_sd() const noexcept
{
    if (m_count == 0U)
    {
        return m_options.frequency_sd;
    }
    return std::sqrt(m_scale / m_sxx);
}

double ClockDriftFilter::offset_sd_s_at(crd::i64 time_ns) const noexcept
{
    if (m_count == 0U)
    {
        return kMaxMagnitudeS;
    }
    const double lever = seconds(clock_difference(time_ns, m_newest_time)) - m_mean_x;
    return std::sqrt(m_scale * ((1.0 / m_sum_w) + (lever * lever / m_sxx)));
}

bool ClockDriftFilter::contradicts(crd::i64 time_ns, double offset_s, double half_width_s) const noexcept
{
    if (m_count == 0U)
    {
        return false;
    }
    const double gate = half_width_s + (kContradictionSigmas * offset_sd_s_at(time_ns));
    return std::fabs(offset_s - offset_s_at(time_ns)) > gate;
}

bool ClockTracker::init(const ClockTrackerOptions& options) noexcept
{
    ClockFilter filter;
    ClockDriftFilter drift;
    const bool valid = filter.init(options.filter, 0) && drift.init(options.drift) && options.delay_window_ns > 0;
    *this = ClockTracker{};
    (void)m_filter.init(valid ? options.filter : ClockFilterOptions{}, 0);
    if (valid)
    {
        m_options = options;
        (void)m_drift.init(options.drift);
        m_min_delay.set_window(static_cast<crd::u64>(options.delay_window_ns));
        const double precision_ns = std::ceil(options.filter.precision_s * kNsPerSecond);
        m_precision_ns = static_cast<crd::i64>(precision_ns);
    }
    return valid;
}

void ClockTracker::anchor(crd::i64 time_ns, crd::i64 offset_ns) noexcept
{
    m_base_ns = offset_ns;
    m_origin_ns = time_ns;
    m_filter_frequency = 0.0;
    (void)m_filter.init(m_options.filter, time_ns);
    (void)m_drift.init(m_options.drift);
    m_min_delay = WindowedFilter<double, AtMost<double>>(static_cast<crd::u64>(m_options.delay_window_ns));
    m_pending_count = 0U;
    m_contradictions = 0U;
    m_has_path_noise = false;
    m_started = true;
}

double ClockTracker::frame_offset(crd::i64 time_ns, crd::i64 offset_ns) const noexcept
{
    return seconds(clock_difference(offset_ns, m_base_ns)) - (m_prior_frequency * seconds(time_ns - m_origin_ns));
}

double ClockTracker::half_width(crd::i64 time_ns, crd::i64 delay_ns) const noexcept
{
    const crd::i64 age = (m_drift.count() > 0U) ? time_ns - m_drift.newest_ns() : 0;
    return (seconds(delay_ns) * 0.5) + (2.0 * m_options.filter.precision_s) + (m_options.filter.phi * seconds(age));
}

bool ClockTracker::pending_agree() const noexcept
{
    // The line's frequency taken out and the prior's spread allowed for, three intervals that share a point describe
    // one new position.
    const crd::i64 newest = m_pending[kStepConfirmations - 1U].time_ns;
    double low = -ClockDriftFilter::kMaxMagnitudeS * 2.0;
    double high = ClockDriftFilter::kMaxMagnitudeS * 2.0;
    for (const Pending& sample : m_pending)
    {
        const double offset = frame_offset(sample.time_ns, sample.offset_ns) -
                              (m_drift.frequency() * seconds(sample.time_ns - m_origin_ns));
        const double width = half_width(sample.time_ns, sample.delay_ns) +
                             (m_options.drift.frequency_sd * seconds(newest - sample.time_ns));
        low = (offset - width > low) ? offset - width : low;
        high = (offset + width < high) ? offset + width : high;
    }
    return low <= high;
}

ClockTrackerEvent ClockTracker::consume(ClockFilterEvent event) noexcept
{
    if (event == ClockFilterEvent::Refused)
    {
        return ClockTrackerEvent::Refused;
    }
    if (event != ClockFilterEvent::Fresh)
    {
        return ClockTrackerEvent::Filtered;
    }
    const crd::i64 epoch = m_filter.epoch_ns();
    const double offset = m_filter.offset_s() + (m_filter_frequency * seconds(epoch - m_origin_ns));

    // A sample's error is half its delay above the path's least, on top of the path's usual noise. Until the least
    // delay has been seen for a while nothing is known but the exchange's own bound, which is where the mean starts.
    const double above = (m_filter.delay_s() - m_min_delay.best()) * 0.5;
    const double excess = (above > 0.0) ? above : 0.0;
    m_path_noise =
        m_has_path_noise ? m_path_noise + ((excess - m_path_noise) * kPathNoiseGain) : m_filter.delay_s() * 0.5;
    m_has_path_noise = true;
    const double uncertainty = m_filter.sample_dispersion_s() + excess + m_path_noise;
    if (!m_drift.add(epoch, offset, uncertainty))
    {
        return ClockTrackerEvent::Filtered;
    }
    const double change = m_drift.frequency() - m_filter_frequency;
    if (m_filter.slew(change, m_origin_ns))
    {
        m_filter_frequency += change;
    }
    return ClockTrackerEvent::Updated;
}

ClockTrackerEvent ClockTracker::feed(const Pending& sample, crd::i64 poll_ns) noexcept
{
    const double delay = seconds(sample.delay_ns);
    const double offset =
        frame_offset(sample.time_ns, sample.offset_ns) - (m_filter_frequency * seconds(sample.time_ns - m_origin_ns));
    const double dispersion = (2.0 * m_options.filter.precision_s) + (m_options.filter.phi * delay);
    const ClockFilterEvent event = m_filter.update(sample.time_ns, offset, delay, dispersion, poll_ns);
    if (event != ClockFilterEvent::Refused)
    {
        m_min_delay.update(delay, static_cast<crd::u64>(sample.time_ns - m_origin_ns));
    }
    return consume(event);
}

ClockTrackerEvent ClockTracker::on_exchange(crd::i64 t1, crd::i64 t2, crd::i64 t3, crd::i64 t4,
                                            crd::i64 poll_ns) noexcept
{
    OnWireSample wire;
    if (!on_wire_sample(t1, t2, t3, t4, m_precision_ns, wire) || !time_in_range(t4) || !poll_in_range(poll_ns) ||
        (m_started && t4 < m_last_ns))
    {
        return ClockTrackerEvent::Refused;
    }
    if (!m_started)
    {
        anchor(t4, wire.offset_ns);
    }
    const Pending sample{t4, wire.offset_ns, wire.delay_ns};
    m_last_ns = t4;
    if (!m_drift.contradicts(t4, frame_offset(t4, wire.offset_ns), half_width(t4, wire.delay_ns)))
    {
        m_pending_count = 0U;
        m_contradictions = 0U;
        return feed(sample, poll_ns);
    }
    ++m_contradictions;
    if (m_pending_count == kStepConfirmations)
    {
        m_pending[0] = m_pending[1];
        m_pending[1] = m_pending[2];
        m_pending_count = kStepConfirmations - 1U;
    }
    m_pending[m_pending_count] = sample;
    ++m_pending_count;
    if (m_pending_count < kStepConfirmations || (!pending_agree() && m_contradictions < kStepForcedAfter))
    {
        return ClockTrackerEvent::Spike;
    }

    // The clock moved, its rate did not: the new frame keeps the frequency, and the exchanges that proved it restart
    // it.
    const Pending confirmed[kStepConfirmations] = {m_pending[0], m_pending[1], m_pending[2]};
    m_prior_frequency += m_drift.frequency();
    anchor(confirmed[0].time_ns, confirmed[0].offset_ns);
    for (const Pending& replayed : confirmed)
    {
        (void)feed(replayed, poll_ns);
    }
    ++m_steps;
    return ClockTrackerEvent::Step;
}

ClockTrackerEvent ClockTracker::on_lost(crd::i64 now_ns, crd::i64 poll_ns) noexcept
{
    if (!m_started || !time_in_range(now_ns) || now_ns < m_last_ns || !poll_in_range(poll_ns))
    {
        return ClockTrackerEvent::Refused;
    }
    m_last_ns = now_ns;
    return consume(m_filter.on_lost(now_ns, poll_ns));
}

crd::i64 ClockTracker::offset_ns_at(crd::i64 local_ns) const noexcept
{
    const double frame =
        (m_prior_frequency * seconds(clock_difference(local_ns, m_origin_ns))) + m_drift.offset_s_at(local_ns);
    const double limit = static_cast<double>(ClockFilter::kMaxTimeNs);
    const double ns = std::clamp(std::round(frame * kNsPerSecond), -limit, limit);
    return static_cast<crd::i64>(static_cast<crd::u64>(m_base_ns) + static_cast<crd::u64>(static_cast<crd::i64>(ns)));
}

double ClockTracker::offset_bound_s_at(crd::i64 local_ns) const noexcept
{
    if (m_drift.count() == 0U)
    {
        return ClockFilter::kMaxDispersionS;
    }
    const crd::i64 age = clock_difference(local_ns, m_drift.newest_ns());
    const double elapsed = seconds((age < 0) ? clock_difference(0, age) : age);
    return (m_min_delay.best() * 0.5) + (2.0 * m_options.filter.precision_s) +
           (kBoundSigmas * m_drift.offset_sd_s_at(local_ns)) + (m_options.filter.phi * elapsed);
}

crd::i64 MediaClockMap::unwrap(crd::u32 rtp_timestamp) noexcept
{
    if (!m_has_rtp)
    {
        m_last_unwrapped = rtp_timestamp;
        m_has_rtp = true;
    }
    else
    {
        // Half the range apart, the larger value is ahead.
        const crd::u32 forward = rtp_timestamp - m_last_rtp;
        const bool ahead = (forward == 0x80000000U) ? m_last_rtp < rtp_timestamp : forward < 0x80000000U;
        m_last_unwrapped += static_cast<crd::i64>(forward) - (ahead ? crd::i64{0} : crd::i64{1} << 32U);
    }
    m_last_rtp = rtp_timestamp;
    return m_last_unwrapped;
}

MediaClockUpdate MediaClockMap::update(crd::u64 ntp_q32, crd::u32 rtp_timestamp) noexcept
{
    constexpr crd::u64 max_report_interval = crd::u64{3600} << 32U;
    constexpr crd::i64 max_rtp_jump = crd::i64{1} << 25U;
    const crd::i64 unwrapped = unwrap(rtp_timestamp);
    for (crd::u32 age = 0U; age < m_count; ++age)
    {
        const crd::u32 index = (m_newest + kReports - age) % kReports;
        if (m_ntp[index] == ntp_q32 || m_rtp[index] == unwrapped)
        {
            return MediaClockUpdate::Same;
        }
    }
    if (ntp_q32 == 0U)
    {
        return MediaClockUpdate::Invalid;
    }
    if (m_count > 0U)
    {
        const crd::u64 old_ntp = m_ntp[m_newest];
        const crd::i64 old_rtp = m_rtp[m_newest];
        if (ntp_q32 <= old_ntp || ntp_q32 > old_ntp + max_report_interval || unwrapped <= old_rtp ||
            unwrapped - old_rtp > max_rtp_jump)
        {
            ++m_consecutive_invalid;
            if (m_consecutive_invalid < kMaxInvalidReports)
            {
                return MediaClockUpdate::Invalid;
            }
            m_count = 0U;
            m_has_line = false;
        }
    }
    m_consecutive_invalid = 0;

    m_newest = (m_newest + 1U) % kReports;
    m_ntp[m_newest] = ntp_q32;
    m_rtp[m_newest] = unwrapped;
    m_count = (m_count < kReports) ? m_count + 1U : kReports;
    if (m_count < 2U)
    {
        return MediaClockUpdate::New;
    }

    // Newest to oldest, the order of libwebrtc's list: the sums round in that order.
    const LineSums sums = line_sums(m_count,
                                    [this](crd::u32 age, double& x, double& y, double& w) noexcept
                                    {
                                        const crd::u32 index = (m_newest + kReports - age) % kReports;
                                        x = static_cast<double>(m_rtp[index]);
                                        y = static_cast<double>(m_ntp[index]);
                                        w = 1.0;
                                    });
    if (std::fabs(sums.sxx) >= 1e-8)
    {
        m_slope = sums.sxy / sums.sxx;
        m_offset = sums.mean_y - (m_slope * sums.mean_x);
        m_has_line = true;
    }
    return MediaClockUpdate::New;
}

crd::u64 MediaClockMap::estimate(crd::u32 rtp_timestamp) noexcept
{
    if (!m_has_line)
    {
        return 0U;
    }
    const double estimated = (static_cast<double>(unwrap(rtp_timestamp)) * m_slope) + m_offset + 0.5;
    if (!(estimated > 0.0))
    {
        return 0U;
    }
    return (estimated >= 18446744073709551616.0) ? ~crd::u64{0} : static_cast<crd::u64>(estimated);
}

double MediaClockMap::frequency_khz() const noexcept
{
    constexpr double ntp_units_per_ms = 4.294967296E6;
    return m_has_line ? ntp_units_per_ms / m_slope : 0.0;
}

bool CaptureClockAligner::translate(crd::i64 capturer_us, crd::i64 system_us, crd::i64& out_us) noexcept
{
    constexpr crd::i64 reset_threshold_us = 300'000;
    constexpr crd::i64 window = 100;
    constexpr crd::i64 min_frame_interval_us = 1'000;
    if (capturer_us < -kMaxTimeUs || capturer_us > kMaxTimeUs || system_us < -kMaxTimeUs || system_us > kMaxTimeUs)
    {
        return false;
    }

    // Plain averaging over the first frames, exponential after; a jump past the threshold starts again.
    const crd::i64 difference = system_us - capturer_us;
    const crd::i64 error = difference - m_offset_us;
    if (error > reset_threshold_us || error < -reset_threshold_us)
    {
        m_frames_seen = 0;
        m_clip_bias_us = 0;
    }
    m_frames_seen = (m_frames_seen < window) ? m_frames_seen + 1 : m_frames_seen;
    m_offset_us += error / m_frames_seen;

    // Never in the future; monotonic with a least interval unless the system clock leaves no room.
    crd::i64 time_us = capturer_us + m_offset_us - m_clip_bias_us;
    if (time_us > system_us)
    {
        m_clip_bias_us += time_us - system_us;
        time_us = system_us;
    }
    else if (time_us < m_previous_translated_us + min_frame_interval_us)
    {
        time_us = m_previous_translated_us + min_frame_interval_us;
        time_us = (time_us > system_us) ? system_us : time_us;
    }
    m_previous_translated_us = time_us;
    m_previous_offset_us = time_us - capturer_us;
    out_us = time_us;
    return true;
}

} // namespace crd::hesap::net
