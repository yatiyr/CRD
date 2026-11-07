// crd-fuzz-hesap-clock: hesap-net's clock filters (HESAP-NET; crd/hesap/net/clock.hpp) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md). Every value is one a peer's timestamps or a clock can carry, chosen to
// reach the arithmetic's edges.
//
// Input. Op bytes, each followed by its arguments; a short input reads zeros.
//
// Oracles, none of them the filters' own verdict:
//   * the exchange: restated modulo 2^64, with its refusal conditions, and no write on a refusal;
//   * the filter: the order a permutation sorted by the restated distance, the chosen stage's values reported, the
//     dispersion within its limit, the event restated from the usable count, a refused update changing no bit;
//   * the drift filter: every value finite, the standard errors not negative, a refused sample changing nothing;
//   * the tracker: every value finite, the bound positive, steps only counted on Step, a refusal changing nothing;
//   * the media map: the estimate the line's own arithmetic gives, the frequency its reciprocal;
//   * the capture aligner: never past the system clock, monotonic while the system clock is, a refusal writing nothing.
#include <crd/fuzz/harness.hpp>
#include <crd/hesap/net/clock.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace
{
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

    [[nodiscard]] u32 next_u32() noexcept { return static_cast<u32>(next_u64() >> 32U); }

    // Small values, around 2^n, and the extremes.
    [[nodiscard]] i64 next_time() noexcept
    {
        const u8 shape = next();
        const u64 bits = next_u64();
        switch (shape % 4U)
        {
            case 0U:
                return static_cast<i64>(bits % 200'000'000'000U);
            case 1U:
                return static_cast<i64>(bits >> (shape >> 2U));
            case 2U:
                return -static_cast<i64>((bits >> 1U) >> (shape >> 2U));
            default:
                return (shape & 4U) != 0U ? std::numeric_limits<i64>::max() : std::numeric_limits<i64>::min();
        }
    }

    // Mostly a plausible value in seconds, sometimes any 64 bits.
    [[nodiscard]] double next_seconds() noexcept
    {
        const u8 shape = next();
        const u64 bits = next_u64();
        if ((shape & 7U) == 7U)
        {
            return std::bit_cast<double>(bits);
        }
        const double value = static_cast<double>(static_cast<i64>(bits >> 40U)) * 1e-9;
        return ((shape & 8U) != 0U) ? -value : value;
    }
};

[[nodiscard]] bool same_bits(double a, double b) noexcept
{
    return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
}

[[nodiscard]] bool same_filter(const ClockFilter& a, const ClockFilter& b) noexcept
{
    bool same = same_bits(a.offset_s(), b.offset_s()) && same_bits(a.delay_s(), b.delay_s()) &&
                same_bits(a.dispersion_s(), b.dispersion_s()) && same_bits(a.jitter_s(), b.jitter_s()) &&
                a.epoch_ns() == b.epoch_ns();
    for (u32 i = 0U; i < ClockFilter::kStages; ++i)
    {
        same = same && same_bits(a.stage(i).offset_s, b.stage(i).offset_s) &&
               same_bits(a.stage(i).delay_s, b.stage(i).delay_s) &&
               same_bits(a.stage(i).dispersion_s, b.stage(i).dispersion_s) &&
               a.stage(i).epoch_ns == b.stage(i).epoch_ns && a.order(i) == b.order(i);
    }
    return same;
}

class Run
{
public:
    Run() { CRD_FUZZ_REQUIRE(m_filter.init(m_filter_options, 0)); }

    void exchange(Reader& in)
    {
        const i64 t1 = in.next_time();
        const i64 t2 = in.next_time();
        const i64 t3 = in.next_time();
        const i64 t4 = in.next_time();
        const i64 precision = (in.next() & 1U) != 0U ? in.next_time() : 1;
        OnWireSample out{-7, -9};
        const bool accepted = on_wire_sample(t1, t2, t3, t4, precision, out);
        const i64 span = clock_difference(t4, t1);
        const i64 hold = clock_difference(t3, t2);
        const bool possible = precision >= 0 && span >= 0 && hold >= 0 && hold - span <= precision;
        CRD_FUZZ_REQUIRE(accepted == possible);
        if (!accepted)
        {
            CRD_FUZZ_REQUIRE(out.offset_ns == -7 && out.delay_ns == -9);
            return;
        }
        CRD_FUZZ_REQUIRE(out.delay_ns >= precision && out.delay_ns >= span - hold);
        // Twice the offset is the legs' sum modulo 2^64, less the bit the rounding dropped.
        const u64 sum = static_cast<u64>(clock_difference(t2, t1)) + static_cast<u64>(clock_difference(t3, t4));
        CRD_FUZZ_REQUIRE((static_cast<u64>(out.offset_ns) * 2U) + (sum & 1U) == sum);
    }

    void filter(Reader& in)
    {
        const ClockFilter before = m_filter;
        const u8 kind = in.next();
        const i64 advance = static_cast<i64>(in.next_u32() >> ((kind >> 2U) & 31U));
        const i64 now = ((kind & 0x80U) != 0U) ? in.next_time() : m_filter_now + advance;
        const i64 poll = 1 + static_cast<i64>(in.next_u32());
        ClockFilterEvent event = ClockFilterEvent::Refused;
        if ((kind & 3U) == 3U)
        {
            event = m_filter.on_lost(now, poll);
        }
        else if ((kind & 3U) == 2U)
        {
            const bool slewed = m_filter.slew(in.next_seconds(), in.next_time());
            if (!slewed)
            {
                CRD_FUZZ_REQUIRE(same_filter(m_filter, before));
            }
            return;
        }
        else
        {
            const double offset = in.next_seconds();
            const double delay = in.next_seconds();
            const double dispersion = in.next_seconds();
            event = m_filter.update(now, offset, delay, dispersion, poll);
            if (event != ClockFilterEvent::Refused)
            {
                // The newest stage holds what was written, its dispersion clamped.
                bool found = false;
                for (u32 i = 0U; i < ClockFilter::kStages; ++i)
                {
                    const ClockFilter::Stage& stage = m_filter.stage(i);
                    found = found || (stage.epoch_ns == now && same_bits(stage.offset_s, offset) &&
                                      same_bits(stage.delay_s, delay));
                }
                CRD_FUZZ_REQUIRE(found);
            }
        }
        if (event == ClockFilterEvent::Refused)
        {
            CRD_FUZZ_REQUIRE(same_filter(m_filter, before));
            return;
        }
        m_filter_now = now;

        // The order is a permutation, sorted by the restated distance.
        u32 seen = 0U;
        double previous = -1.0;
        u32 usable = 0U;
        for (u32 rank = 0U; rank < ClockFilter::kStages; ++rank)
        {
            const u32 index = m_filter.order(rank);
            CRD_FUZZ_REQUIRE(index < ClockFilter::kStages);
            seen |= u32{1} << index;
            const ClockFilter::Stage& stage = m_filter.stage(index);
            CRD_FUZZ_REQUIRE(stage.dispersion_s >= 0.0 && stage.dispersion_s <= ClockFilter::kMaxDispersionS);
            double distance = stage.delay_s;
            if (stage.dispersion_s >= ClockFilter::kMaxDispersionS)
            {
                distance = ClockFilter::kMaxDispersionS;
            }
            else if (now - stage.epoch_ns > m_filter_options.allan_intercept_ns)
            {
                distance += stage.dispersion_s;
            }
            CRD_FUZZ_REQUIRE(distance >= previous);
            previous = distance;
            if (distance < ClockFilter::kMaxDispersionS && (usable < 2U || distance < m_filter_options.max_distance_s))
            {
                ++usable;
            }
        }
        CRD_FUZZ_REQUIRE(seen == 0xFFU);
        CRD_FUZZ_REQUIRE((event == ClockFilterEvent::NoSample) == (usable == 0U));
        CRD_FUZZ_REQUIRE(m_filter.dispersion_s() >= 0.0 && m_filter.dispersion_s() <= ClockFilter::kMaxDispersionS);
        CRD_FUZZ_REQUIRE(std::isfinite(m_filter.jitter_s()));
        if (usable != 0U)
        {
            const ClockFilter::Stage& best = m_filter.stage(m_filter.order(0U));
            CRD_FUZZ_REQUIRE(same_bits(m_filter.offset_s(), best.offset_s));
            CRD_FUZZ_REQUIRE(same_bits(m_filter.delay_s(), best.delay_s));
            CRD_FUZZ_REQUIRE(m_filter.jitter_s() >= m_filter_options.precision_s);
        }
        if (event == ClockFilterEvent::Fresh)
        {
            CRD_FUZZ_REQUIRE(m_filter.epoch_ns() == m_filter.stage(m_filter.order(0U)).epoch_ns);
            CRD_FUZZ_REQUIRE(!m_fresh || m_filter.epoch_ns() > m_fresh_epoch);
            m_fresh = true;
            m_fresh_epoch = m_filter.epoch_ns();
        }
        else
        {
            CRD_FUZZ_REQUIRE(m_filter.epoch_ns() == before.epoch_ns());
        }
    }

    void drift(Reader& in)
    {
        const u32 count = m_drift.count();
        const i64 at = in.next_time();
        const i64 time = ((at & 1) != 0) ? at : m_drift_now + 1 + (at & 0xFFFFFFFF);
        const double offset = in.next_seconds();
        const double uncertainty = in.next_seconds();
        const double before = m_drift.offset_s_at(m_drift_now);
        if (m_drift.add(time, offset, uncertainty))
        {
            CRD_FUZZ_REQUIRE(m_drift.count() >= 1U && m_drift.count() <= ClockDriftFilter::kCapacity);
            CRD_FUZZ_REQUIRE(m_drift.newest_ns() == time);
            m_drift_now = time;
        }
        else
        {
            CRD_FUZZ_REQUIRE(m_drift.count() == count);
            CRD_FUZZ_REQUIRE(same_bits(before, m_drift.offset_s_at(m_drift_now)));
        }
        CRD_FUZZ_REQUIRE(std::isfinite(m_drift.frequency()));
        CRD_FUZZ_REQUIRE(std::isfinite(m_drift.frequency_sd()) && m_drift.frequency_sd() >= 0.0);
        CRD_FUZZ_REQUIRE(std::isfinite(m_drift.offset_s_at(m_drift_now)));
        CRD_FUZZ_REQUIRE(std::isfinite(m_drift.offset_sd_s_at(m_drift_now)) &&
                         m_drift.offset_sd_s_at(m_drift_now) >= 0.0);
        (void)m_drift.contradicts(m_drift_now, offset, uncertainty);
    }

    void tracker(Reader& in)
    {
        const u8 kind = in.next();
        const i64 advance = static_cast<i64>(in.next_u32() >> ((kind >> 2U) & 31U));
        const i64 t4 = ((kind & 0x80U) != 0U) ? in.next_time() : m_tracker_now + advance;
        const i64 poll = 1 + static_cast<i64>(in.next_u32());
        const u32 steps = m_tracker.steps();
        const i64 asked = m_tracker_now;
        const i64 before = m_tracker.offset_ns_at(asked);
        const u32 count = m_tracker.drift().count();
        ClockTrackerEvent event = ClockTrackerEvent::Refused;
        if ((kind & 3U) == 3U)
        {
            event = m_tracker.on_lost(t4, poll);
        }
        else
        {
            // A plausible exchange on a moving peer clock, or any four timestamps.
            const i64 rtt = static_cast<i64>(in.next_u32() >> 6U);
            const i64 peer_offset = ((kind & 3U) == 2U) ? in.next_time() : static_cast<i64>(in.next_u32());
            const i64 t2 = clock_difference(clock_difference(t4, rtt / 2), clock_difference(0, peer_offset));
            const i64 t3 = ((kind & 0x40U) != 0U) ? in.next_time() : t2;
            event = m_tracker.on_exchange(clock_difference(t4, rtt), t2, t3, t4, poll);
        }
        if (event == ClockTrackerEvent::Refused)
        {
            CRD_FUZZ_REQUIRE(m_tracker.drift().count() == count);
            CRD_FUZZ_REQUIRE(m_tracker.steps() == steps);
            CRD_FUZZ_REQUIRE(m_tracker.offset_ns_at(asked) == before);
        }
        else
        {
            m_tracker_now = t4;
        }
        CRD_FUZZ_REQUIRE(m_tracker.steps() == steps + ((event == ClockTrackerEvent::Step) ? 1U : 0U));
        CRD_FUZZ_REQUIRE(std::isfinite(m_tracker.frequency()));
        CRD_FUZZ_REQUIRE(std::isfinite(m_tracker.frequency_sd()) && m_tracker.frequency_sd() >= 0.0);
        const double bound = m_tracker.offset_bound_s_at(m_tracker_now);
        CRD_FUZZ_REQUIRE(std::isfinite(bound) && bound > 0.0);
        CRD_FUZZ_REQUIRE(std::isfinite(m_tracker.offset_sd_s_at(m_tracker_now)));
        (void)m_tracker.offset_ns_at(in.next_time());
    }

    void media(Reader& in)
    {
        const u64 ntp = ((in.next() & 1U) != 0U) ? in.next_u64() : m_ntp + (u64{in.next_u32() >> 8U} << 16U);
        const u32 rtp = ((in.next() & 1U) != 0U) ? in.next_u32() : m_rtp + (in.next_u32() >> 12U);
        const MediaClockUpdate result = m_media.update(ntp, rtp);
        if (result == MediaClockUpdate::New)
        {
            m_ntp = ntp;
            m_rtp = rtp;
        }
        if (!m_media.has_line())
        {
            CRD_FUZZ_REQUIRE(m_media.frequency_khz() == 0.0);
            CRD_FUZZ_REQUIRE(m_media.estimate(rtp) == 0U);
            return;
        }
        CRD_FUZZ_REQUIRE(same_bits(m_media.frequency_khz(), 4.294967296E6 / m_media.slope()));
        const u64 estimate = m_media.estimate(rtp + in.next_u32());
        if (std::isnan(m_media.slope()) || std::isnan(m_media.offset()))
        {
            CRD_FUZZ_REQUIRE(estimate == 0U);
        }
    }

    void capture(Reader& in)
    {
        const u8 kind = in.next();
        const i64 capturer = ((kind & 1U) != 0U) ? in.next_time() : m_capturer + static_cast<i64>(in.next_u32() >> 12U);
        const i64 system = ((kind & 2U) != 0U) ? in.next_time() : m_system + static_cast<i64>(in.next_u32() >> 12U);
        i64 out = -11;
        if (!m_capture.translate(capturer, system, out))
        {
            CRD_FUZZ_REQUIRE(out == -11);
            return;
        }
        CRD_FUZZ_REQUIRE(out <= system);
        if (m_has_translation && system >= m_system)
        {
            CRD_FUZZ_REQUIRE(out >= m_translated || out == system);
        }
        CRD_FUZZ_REQUIRE(m_capture.translate(capturer) == out);
        m_capturer = capturer;
        m_system = system;
        m_translated = out;
        m_has_translation = true;
    }

private:
    ClockFilterOptions m_filter_options{};
    ClockFilter m_filter{};
    i64 m_filter_now = 0;
    i64 m_fresh_epoch = 0;
    bool m_fresh = false;
    ClockDriftFilter m_drift{};
    i64 m_drift_now = 0;
    ClockTracker m_tracker{};
    i64 m_tracker_now = 0;
    MediaClockMap m_media{};
    u64 m_ntp = u64{1} << 40U;
    u32 m_rtp = 0U;
    CaptureClockAligner m_capture{};
    i64 m_capturer = 0;
    i64 m_system = 0;
    i64 m_translated = 0;
    bool m_has_translation = false;
};

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    sink.add(name, bytes.begin(), bytes.size());
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
                run.exchange(in);
                break;
            case 1U:
                run.filter(in);
                break;
            case 2U:
                run.drift(in);
                break;
            case 3U:
                run.tracker(in);
                break;
            case 4U:
                run.media(in);
                break;
            default:
                run.capture(in);
                break;
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // One op of each kind at small values and at the extremes, and sequences that fill the registers.
    add(sink, "exchange-small", {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 100U});
    add(sink, "exchange-extremes", {0U, 3U, 3U, 7U, 3U, 3U});
    add(sink, "filter-sample", {1U, 0U, 0U, 0U, 0U, 16U, 0U, 0U, 0U, 0U, 16U, 0U, 0U, 0U, 1U});
    add(sink, "filter-lost", {1U, 3U, 0U, 0U, 0U, 16U, 1U, 3U, 0U, 0U, 0U, 16U, 1U, 3U, 0U, 0U, 0U, 16U});
    add(sink, "filter-nan", {1U, 0U, 0U, 0U, 0U, 16U, 0U, 0U, 0U, 0U, 16U, 0U, 0U, 0U, 1U, 7U, 0x7FU, 0xF8U});
    add(sink, "filter-slew", {1U, 2U, 0U, 0U, 0U, 16U, 7U, 0x3FU, 0xF0U, 0U, 0U, 0U, 0U, 0U, 0U});
    add(sink, "drift-line", {2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 3U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 7U, 0x3FU, 0xF0U});
    add(sink, "tracker-exchange", {3U, 0U, 0U, 0U, 16U, 0U, 0U, 16U, 0U, 3U, 0U, 16U, 0U, 0U, 0U, 16U, 0U});
    add(sink, "tracker-any", {3U, 0x42U, 0U, 16U, 0U, 16U, 0U, 3U, 3U, 3U, 0x42U, 0U, 16U, 0U, 16U, 0U, 3U, 3U, 3U});
    add(sink, "tracker-lost", {3U, 3U, 0U, 16U, 0U, 16U, 0U, 16U, 3U, 3U, 0U, 16U, 0U, 16U, 0U, 16U});
    add(sink, "media-reports",
        {4U, 0U, 0U, 16U, 0U, 16U, 0U, 16U, 0U, 16U, 0U, 16U, 4U, 0U, 0U, 16U, 0U, 16U, 0U, 16U});
    add(sink, "media-any", {4U, 1U, 7U, 7U, 7U, 7U, 7U, 7U, 7U, 7U, 1U, 0U, 0U, 16U, 4U, 1U, 7U, 7U, 7U, 7U, 7U, 7U});
    add(sink, "capture-frames", {5U, 0U, 0U, 16U, 0U, 16U, 0U, 0U, 16U, 0U, 16U, 5U, 0U, 0U, 16U, 0U, 16U});
    add(sink, "capture-extremes", {5U, 3U, 3U, 3U, 5U, 3U, 7U, 7U, 7U});
}
