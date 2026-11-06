// crd-fuzz-hesap-estimators: hesap-net's network estimators (HESAP-NET; crd/hesap/net/rtt.hpp, jitter.hpp,
// delay_trend.hpp, rate.hpp) under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md). Every value
// here is one a peer's feedback or a clock can carry, chosen to reach the arithmetic's edges.
//
// Input. Op bytes, each followed by its arguments; a short input reads zeros.
//
// Oracles, none of them the estimators' own verdict:
//   * the RTT: min_rtt is the smallest clamped sample, smoothed_rtt stays between the smallest and largest, a refused
//     sample changes nothing, the durations stay positive;
//   * the jitter: RFC 3550's update restated in 64 bits, the value never past the largest step seen;
//   * the group tracker: `out` is written only on a true return, within the bounds its domain allows;
//   * the trendline, the Kalman filter and the detector: every state finite, the threshold in [6, 600], the noise
//     variance at least 1, a refused update changes nothing;
//   * the windowed filter: best() against a brute-force history of the window (a sample of it, not worse than the
//     newest);
//   * the acknowledged rate: the estimate finite, never below the floor once set, the variance not negative.
#include <crd/fuzz/harness.hpp>
#include <crd/hesap/net/delay_trend.hpp>
#include <crd/hesap/net/jitter.hpp>
#include <crd/hesap/net/rate.hpp>
#include <crd/hesap/net/rtt.hpp>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace
{
using crd::i32;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

struct Reader
{
    const u8* data;
    usize size;

    [[nodiscard]] u8 next() noexcept
    {
        if (size == 0U)
        {
            return 0U;
        }
        --size;
        return *data++;
    }

    [[nodiscard]] u64 next_u64() noexcept
    {
        u64 value = 0U;
        for (u32 i = 0U; i < 8U; ++i)
        {
            value = (value << 8U) | next();
        }
        return value;
    }

    [[nodiscard]] u32 next_u32() noexcept
    {
        u32 value = 0U;
        for (u32 i = 0U; i < 4U; ++i)
        {
            value = (value << 8U) | next();
        }
        return value;
    }

    // Small values, around 2^n, and the extremes.
    [[nodiscard]] i64 next_time() noexcept
    {
        const u8 shape = next();
        const u64 bits = next_u64();
        switch (shape % 4U)
        {
        case 0U:
            return static_cast<i64>(bits % 200'000U);
        case 1U:
            return static_cast<i64>(bits >> (shape >> 2U));
        case 2U:
            return -static_cast<i64>((bits >> 1U) >> (shape >> 2U));
        default:
            return (shape & 4U) != 0U ? std::numeric_limits<i64>::max() : std::numeric_limits<i64>::min();
        }
    }

    [[nodiscard]] double next_double() noexcept
    {
        u64 bits = next_u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Mostly a plausible delta in milliseconds, sometimes any 64 bits.
    [[nodiscard]] double next_delta() noexcept
    {
        const u8 shape = next();
        if ((shape & 7U) == 7U)
        {
            return next_double();
        }
        return static_cast<double>(static_cast<crd::i32>(next_u32() % 100'000U)) / 16.0 *
               (((shape & 8U) != 0U) ? -1.0 : 1.0);
    }
};

constexpr u32 kWindowHistory = 64U;
constexpr u64 kFilterWindow = 32U;

struct Sample
{
    u32 value = 0U;
    u64 time = 0U;
};

class Run
{
public:
    Run()
    {
        const bool accepted = m_trendline.init(m_storage);
        CRD_FUZZ_REQUIRE(accepted);
    }

    void rtt(Reader& in)
    {
        const i64 latest = in.next_time();
        const i64 delay = in.next_time();
        const i64 max_delay = in.next_time();
        const bool confirmed = (in.next() & 1U) != 0U;
        const RttEstimator before = m_rtt;
        if (!m_rtt.on_sample(latest, delay, max_delay, confirmed))
        {
            CRD_FUZZ_REQUIRE(latest < 0);
            CRD_FUZZ_REQUIRE(m_rtt.smoothed_ns() == before.smoothed_ns() && m_rtt.min_ns() == before.min_ns());
        }
        else
        {
            const i64 clamped = latest < RttEstimator::kMaxSampleNs ? latest : RttEstimator::kMaxSampleNs;
            m_rtt_low = (!m_rtt_seen || clamped < m_rtt_low) ? clamped : m_rtt_low;
            m_rtt_high = (!m_rtt_seen || clamped > m_rtt_high) ? clamped : m_rtt_high;
            m_rtt_seen = true;
            if (!m_rtt_congested)
            {
                CRD_FUZZ_REQUIRE(m_rtt.min_ns() == m_rtt_low);
            }
            CRD_FUZZ_REQUIRE(m_rtt.latest_ns() == clamped);
            // An adjusted sample is never below min_rtt, and the integer mean rounds down by at most a nanosecond.
            CRD_FUZZ_REQUIRE(m_rtt.smoothed_ns() >= 0 && m_rtt.smoothed_ns() <= m_rtt_high);
            CRD_FUZZ_REQUIRE(m_rtt.variation_ns() >= 0 && m_rtt.variation_ns() <= m_rtt_high);
        }
        CRD_FUZZ_REQUIRE(m_rtt.pto_ns(max_delay) >= RttEstimator::kGranularityNs);
        CRD_FUZZ_REQUIRE(m_rtt.loss_delay_ns() >= RttEstimator::kGranularityNs);
        CRD_FUZZ_REQUIRE(m_rtt.persistent_congestion_ns(max_delay) > 0);
        if ((in.next() & 15U) == 0U)
        {
            m_rtt.on_persistent_congestion();
            m_rtt_congested = true;
        }
    }

    void jitter(Reader& in)
    {
        const u32 arrival = in.next_u32();
        const u32 timestamp = in.next_u32();
        const u32 transit = arrival - timestamp;
        if (m_jitter_started)
        {
            const auto difference = static_cast<crd::i32>(transit - m_transit);
            const u64 step = static_cast<u64>(std::abs(static_cast<i64>(difference)));
            m_expected_q4 = static_cast<u32>(u64{m_expected_q4} + step - ((u64{m_expected_q4} + 8U) >> 4U));
            m_largest_step = step > m_largest_step ? step : m_largest_step;
        }
        m_jitter_started = true;
        m_transit = transit;
        m_jitter.update(arrival, timestamp);
        CRD_FUZZ_REQUIRE(m_jitter.value_q4() == m_expected_q4);
        CRD_FUZZ_REQUIRE(u64{m_jitter.value()} <= m_largest_step);
    }

    void delay(Reader& in)
    {
        const i64 send = in.next_time();
        const i64 arrival = in.next_time();
        const i64 system = (in.next() & 1U) != 0U ? arrival : in.next_time();
        const u32 bytes = in.next_u32();
        GroupDeltas out{123, 456, 789};
        const GroupDeltas untouched = out;
        if (m_tracker.add(send, arrival, system, bytes, out))
        {
            const i64 bound = i64{1} << 61U;
            CRD_FUZZ_REQUIRE(out.send_us >= -bound && out.send_us <= bound);
            CRD_FUZZ_REQUIRE(out.arrival_us >= 0 && out.arrival_us <= bound);
            signal(static_cast<double>(out.arrival_us) / 1000.0, static_cast<double>(out.send_us) / 1000.0, arrival / 1000,
                   out.bytes);
        }
        else
        {
            CRD_FUZZ_REQUIRE(out.send_us == untouched.send_us && out.arrival_us == untouched.arrival_us &&
                             out.bytes == untouched.bytes);
        }
    }

    void signal(double recv, double send, i64 arrival_ms, i32 size)
    {
        const bool in_domain = std::abs(recv) <= TrendlineFilter::kMaxDeltaMs &&
                               std::abs(send) <= TrendlineFilter::kMaxDeltaMs;
        const double accumulated = m_trendline.accumulated_delay_ms();
        CRD_FUZZ_REQUIRE(m_trendline.update(recv, send, arrival_ms) == in_domain);
        if (!in_domain)
        {
            CRD_FUZZ_REQUIRE(m_trendline.accumulated_delay_ms() == accumulated);
        }
        CRD_FUZZ_REQUIRE(std::isfinite(m_trendline.trend()));
        CRD_FUZZ_REQUIRE(std::isfinite(m_trendline.smoothed_delay_ms()));
        CRD_FUZZ_REQUIRE(m_trendline.detector().threshold() >= 6.0 && m_trendline.detector().threshold() <= 600.0);

        const crd::i32 deltas = m_kalman.deltas();
        CRD_FUZZ_REQUIRE(m_kalman.update(recv, send, size, m_detector.state()) == in_domain);
        if (!in_domain)
        {
            CRD_FUZZ_REQUIRE(m_kalman.deltas() == deltas);
            return;
        }
        (void)m_detector.update(m_kalman.offset(), m_kalman.deltas(), 1.0, send, arrival_ms);
        CRD_FUZZ_REQUIRE(std::isfinite(m_kalman.offset()) && std::isfinite(m_kalman.slope()));
        CRD_FUZZ_REQUIRE(std::isfinite(m_kalman.noise_variance()) && m_kalman.noise_variance() >= 1.0);
        CRD_FUZZ_REQUIRE(m_detector.threshold() >= 6.0 && m_detector.threshold() <= 600.0);
    }

    void windowed(Reader& in)
    {
        m_filter_time += 1U + (in.next() % 16U); // Time advances, so kWindowHistory holds a whole window.
        const u32 value = in.next_u32() >> (in.next() % 32U);
        m_history[m_history_count % kWindowHistory] = {value, m_filter_time};
        ++m_history_count;
        m_filter.update(value, m_filter_time);
        CRD_FUZZ_REQUIRE(m_filter.best() >= value);
        bool found = false;
        const u32 held = m_history_count < kWindowHistory ? m_history_count : kWindowHistory;
        for (u32 i = 0U; i < held; ++i)
        {
            const Sample& sample = m_history[i];
            found = found || (sample.value == m_filter.best() && m_filter_time - sample.time <= kFilterWindow);
        }
        CRD_FUZZ_REQUIRE(found);
    }

    void rate(Reader& in)
    {
        const i64 now = in.next_time();
        const u32 bytes = in.next_u32();
        m_rate.update(now, bytes, (in.next() & 1U) != 0U);
        CRD_FUZZ_REQUIRE(std::isfinite(m_rate.kbps()));
        CRD_FUZZ_REQUIRE(std::isfinite(m_rate.variance()) && m_rate.variance() >= 0.0F);
        CRD_FUZZ_REQUIRE(m_rate.kbps() >= -1.0F);
        CRD_FUZZ_REQUIRE(std::isfinite(m_rate.peek_kbps()));
    }

private:
    TrendlineSample m_storage[TrendlineFilter::storage_samples(TrendlineFilter::kDefaultWindow)];
    TrendlineFilter m_trendline;
    PacketGroupTracker m_tracker;
    KalmanDelayFilter m_kalman;
    DelayOveruseDetector m_detector;
    RttEstimator m_rtt;
    InterarrivalJitter m_jitter;
    AckedRateEstimator m_rate;
    WindowedMax<u32> m_filter{kFilterWindow};
    Sample m_history[kWindowHistory] = {};
    u64 m_filter_time = 0U;
    u64 m_largest_step = 0U;
    i64 m_rtt_low = 0;
    i64 m_rtt_high = 0;
    u32 m_history_count = 0U;
    u32 m_expected_q4 = 0U;
    u32 m_transit = 0U;
    bool m_jitter_started = false;
    bool m_rtt_seen = false;
    bool m_rtt_congested = false;
};

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    u8 buffer[128];
    usize size = 0U;
    for (const u8 b : bytes)
    {
        buffer[size++] = b;
    }
    sink.add(name, buffer, size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Reader in{data, size};
    Run run;
    while (in.size != 0U)
    {
        switch (in.next() % 6U)
        {
        case 0U:
            run.rtt(in);
            break;
        case 1U:
            run.jitter(in);
            break;
        case 2U:
            run.delay(in);
            break;
        case 3U:
            run.signal(in.next_delta(), in.next_delta(), in.next_time(), static_cast<i32>(in.next_u32()));
            break;
        case 4U:
            run.windowed(in);
            break;
        default:
            run.rate(in);
            break;
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // One op of each kind at small values and at the extremes, and the sequences that complete a group and a window.
    add(sink, "rtt-small", {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 100U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 10U});
    add(sink, "rtt-extremes", {0U, 7U, 0U, 3U, 3U, 0U, 3U, 7U, 0U, 1U, 1U});
    add(sink, "rtt-negative", {0U, 3U, 0U, 3U, 0U, 3U, 0U, 0U, 0U});
    add(sink, "jitter-wrap",
        {1U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U, 1U, 0U, 0U, 0U, 0x10U, 0U, 0U, 0U, 0x20U});
    add(sink, "jitter-half-range", {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0x80U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    add(sink, "delay-out-of-domain", {2U, 3U, 3U, 3U, 0U, 0U, 0U, 0U, 1U});
    add(sink, "signal-nan", {3U, 7U, 0x7FU, 0xF8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    add(sink, "signal-infinite", {3U, 7U, 0xFFU, 0xF0U, 0U, 0U, 0U, 0U, 0U, 0U, 7U, 0x7FU, 0xF0U, 0U, 0U, 0U, 0U, 0U, 0U});
    add(sink, "windowed-rising", {4U, 3U, 0U, 0U, 0U, 1U, 0U, 4U, 3U, 0U, 0U, 0U, 2U, 0U, 4U, 3U, 0U, 0U, 0U, 3U, 0U});
    add(sink, "windowed-aging", {4U, 15U, 0U, 0U, 0U, 9U, 0U, 4U, 15U, 0U, 0U, 0U, 5U, 0U, 4U, 15U, 0U, 0U, 0U, 3U, 0U});
    add(sink, "rate-window", {5U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 16U, 0U, 0U});
    add(sink, "rate-extremes", {5U, 3U, 0U, 0U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 1U, 5U, 7U, 0U, 0U, 0U, 0U, 1U});
}
