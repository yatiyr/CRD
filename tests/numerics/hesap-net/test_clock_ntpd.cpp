// HESAP-NET's ClockFilter held bit for bit to ntpd 4.2.8p18's own clock_filter() (crd-ntpd-peer, the function cut
// verbatim from the pinned ntp_proto.c): every stage, the order, the offset, delay, dispersion and jitter after every
// update, and the outcome, on streams that reach every branch of the function. ntpd counts time in whole seconds and
// its poll in log 2, so the streams do too.

#include "ntpd_peer.h"
#include "seeded_words.hpp"

#include <crd/hesap/net/clock.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace hn = crd::hesap::net;
using crd::i64;
using crd::u32;
using crd::u64;

namespace
{

constexpr i64 kNsPerSecond = 1'000'000'000;
constexpr u32 kUpdatesPerStream = 200'000U;
constexpr u64 kStart = 1'000'000U; // ntpd's epoch starts at 0: far past any two-poll horizon, as a running daemon's is.

struct Stream
{
    const char* name;
    signed char precision_log2;
    double phi;
    double max_distance;
    unsigned char allan_log2;
    u32 max_gap_s;
    unsigned char min_poll;
    unsigned char max_poll;
    double jitter_s; // Of the offset.
    double delay_low_s;
    double delay_span_s;
    u32 delay_steps; // Delays on a grid, so ties reach the sort; 0 for a continuous spread.
    u32 lost_per_1024;
    u32 spike_per_1024;
    double spike_s;
    double dispersion_s;
};

constexpr Stream kStreams[] = {
    {"daemon", -20, 15e-6, 1.5, 11, 64U, 6U, 10U, 1e-3, 0.01, 0.02, 0U, 0U, 0U, 0.0, 1e-6},
    {"ties", -20, 15e-6, 1.5, 11, 16U, 3U, 6U, 1e-3, 0.01, 0.02, 4U, 10U, 0U, 0.0, 1e-6},
    {"burst-same-second", -18, 15e-6, 1.5, 11, 1U, 3U, 3U, 1e-4, 1e-3, 1e-3, 0U, 0U, 20U, 0.2, 0.0},
    {"popcorn", -20, 15e-6, 1.5, 11, 16U, 3U, 5U, 1e-4, 0.01, 1e-3, 0U, 0U, 150U, 2.0, 1e-6},
    {"losses", -20, 15e-6, 1.5, 11, 128U, 6U, 8U, 1e-3, 0.02, 0.05, 0U, 500U, 20U, 1.0, 1e-5},
    {"past-allan", -20, 15e-6, 1.5, 3, 300U, 3U, 9U, 1e-3, 0.01, 0.1, 0U, 50U, 0U, 0.0, 1e-4},
    {"past-max-distance", -6, 15e-6, 0.03, 11, 32U, 3U, 6U, 5e-3, 0.0, 0.2, 0U, 30U, 50U, 0.5, 2e-3},
    {"dispersion-saturates", -20, 15e-3, 1.5, 11, 512U, 6U, 10U, 1e-3, 0.01, 0.02, 0U, 100U, 0U, 0.0, 4.0},
    {"no-aging", -24, 0.0, 16.0, 11, 8U, 3U, 3U, 1e-5, 0.0, 1e-4, 8U, 0U, 0U, 0.0, 0.0},
};

[[nodiscard]] bool same_bits(double a, double b) noexcept
{
    return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
}

[[nodiscard]] double uniform(SeededWords& words) noexcept
{
    return static_cast<double>(words.next_u32()) / 4294967296.0;
}

[[nodiscard]] int outcome_of(hn::ClockFilterEvent event) noexcept
{
    switch (event)
    {
        case hn::ClockFilterEvent::NoSample:
            return CRD_NTPD_NO_SAMPLE;
        case hn::ClockFilterEvent::Spike:
            return CRD_NTPD_SPIKE;
        case hn::ClockFilterEvent::Stale:
            return CRD_NTPD_STALE;
        case hn::ClockFilterEvent::Fresh:
            return CRD_NTPD_FRESH;
        case hn::ClockFilterEvent::Refused:
            break;
    }
    return -1;
}

} // namespace

TEST_CASE("clock filter: bit for bit with ntpd's clock_filter on every stream", "[clock][ntpd]")
{
    u64 outcomes[4] = {};
    for (const Stream& stream : kStreams)
    {
        INFO(stream.name);
        SeededWords words(0x4E5450U, static_cast<u64>(stream.max_gap_s) << 8U);
        const double precision = std::ldexp(1.0, stream.precision_log2);

        crd_ntpd_peer peer;
        crd_ntpd_clear(&peer, kStart, stream.precision_log2);
        hn::ClockFilterOptions options;
        options.precision_s = precision;
        options.phi = stream.phi;
        options.max_distance_s = stream.max_distance;
        options.allan_intercept_ns = (i64{1} << stream.allan_log2) * kNsPerSecond;
        hn::ClockFilter filter;
        REQUIRE(filter.init(options, static_cast<i64>(kStart) * kNsPerSecond));

        u64 now = kStart;
        double base = 0.0;
        u64 mismatches = 0U;
        for (u32 update = 0U; update < kUpdatesPerStream; ++update)
        {
            now += words.next_u32() % (stream.max_gap_s + 1U);
            peer.hpoll = static_cast<unsigned char>(
                stream.min_poll + (words.next_u32() % (static_cast<u32>(stream.max_poll - stream.min_poll) + 1U)));
            const i64 poll_ns = (i64{1} << peer.hpoll) * kNsPerSecond;

            base += (uniform(words) - 0.5) * 1e-5; // Wander, so the offset is never the same twice.
            double offset = base + ((uniform(words) - 0.5) * stream.jitter_s);
            if ((words.next_u32() % 1024U) < stream.spike_per_1024)
            {
                offset += (uniform(words) - 0.5) * stream.spike_s;
            }
            double delay = stream.delay_low_s;
            if (stream.delay_steps != 0U)
            {
                delay += stream.delay_span_s * static_cast<double>(words.next_u32() % stream.delay_steps) /
                         static_cast<double>(stream.delay_steps);
            }
            else
            {
                delay += uniform(words) * stream.delay_span_s;
            }
            const double dispersion = uniform(words) * stream.dispersion_s;
            const bool lost = (words.next_u32() % 1024U) < stream.lost_per_1024;

            const hn::ClockFilterEvent event =
                lost ? filter.on_lost(static_cast<i64>(now) * kNsPerSecond, poll_ns)
                     : filter.update(static_cast<i64>(now) * kNsPerSecond, offset, delay, dispersion, poll_ns);
            const crd_ntpd_outcome outcome =
                lost ? crd_ntpd_filter(&peer, now, 0.0, 0.0, 16.0, stream.phi, stream.max_distance, stream.allan_log2,
                                       stream.precision_log2)
                     : crd_ntpd_filter(&peer, now, offset, delay, dispersion, stream.phi, stream.max_distance,
                                       stream.allan_log2, stream.precision_log2);
            ++outcomes[outcome];

            bool same = outcome_of(event) == outcome && same_bits(filter.offset_s(), peer.offset) &&
                        same_bits(filter.delay_s(), peer.delay) && same_bits(filter.dispersion_s(), peer.disp) &&
                        same_bits(filter.jitter_s(), peer.jitter) &&
                        static_cast<u64>(filter.epoch_ns()) == peer.epoch * static_cast<u64>(kNsPerSecond);
            for (u32 i = 0U; i < CRD_NTPD_STAGES; ++i)
            {
                const hn::ClockFilter::Stage& stage = filter.stage(i);
                same = same && same_bits(stage.offset_s, peer.filter_offset[i]) &&
                       same_bits(stage.delay_s, peer.filter_delay[i]) &&
                       same_bits(stage.dispersion_s, peer.filter_disp[i]) && filter.order(i) == peer.filter_order[i];
                if (peer.filter_epoch[i] != 0U)
                {
                    same = same && static_cast<u64>(stage.epoch_ns) == peer.filter_epoch[i] * u64{1'000'000'000};
                }
            }
            if (!same)
            {
                ++mismatches;
                if (mismatches == 1U)
                {
                    UNSCOPED_INFO("first difference at update " << update << ": outcome " << outcome << " event "
                                                                << static_cast<int>(event) << " offset "
                                                                << filter.offset_s() << " vs " << peer.offset);
                }
            }
        }
        CHECK(mismatches == 0U);
    }
    // Every return of the function was reached.
    CHECK(outcomes[CRD_NTPD_NO_SAMPLE] > 0U);
    CHECK(outcomes[CRD_NTPD_SPIKE] > 0U);
    CHECK(outcomes[CRD_NTPD_STALE] > 0U);
    CHECK(outcomes[CRD_NTPD_FRESH] > 0U);
}
