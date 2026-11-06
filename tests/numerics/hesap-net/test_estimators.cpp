// HESAP-NET's network estimators against restated formulas: RFC 9002's RTT in real arithmetic, RFC 3550's jitter in its
// floating form, the delay trend's least squares and Kalman update in textbook form, the windowed filter against a
// brute-force window, the acknowledged rate by hand; every refusal, and the behaviour on a simulated bottleneck.
// The bit-exact comparison with libwebrtc is crd-hesap-estimators-webrtc-tests.

#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/delay_trend.hpp>
#include <crd/hesap/net/jitter.hpp>
#include <crd/hesap/net/rate.hpp>
#include <crd/hesap/net/rtt.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace hn = crd::hesap::net;
using Catch::Approx;
using crd::i32;
using crd::i64;
using crd::u32;
using crd::u64;
using hn::DelayState;

namespace
{

constexpr i64 kMs = 1'000'000;

// --------------------------------------------------------------------------------------- compile-time RTT known answers

constexpr i64 first_smoothed()
{
    hn::RttEstimator rtt;
    (void)rtt.on_sample(100 * kMs, 50 * kMs, 25 * kMs, true);
    return rtt.smoothed_ns();
}
static_assert(first_smoothed() == 100 * kMs); // §5.3: the first sample ignores the delay.

struct Trend
{
    crd::containers::Array<hn::TrendlineSample> storage;
    hn::TrendlineFilter filter;

    explicit Trend(u32 window = hn::TrendlineFilter::kDefaultWindow, hn::TrendlineOptions options = {})
    {
        storage.resize(hn::TrendlineFilter::storage_samples(window));
        REQUIRE(filter.init({storage.data(), storage.size()}, window, options));
    }
};

// Sends packets at a fixed spacing with the given delay growth per packet; the shape of libwebrtc's own unit test.
DelayState run_trend(double delay_growth_ms, u32 groups = 25U)
{
    Trend trend;
    const double send_delta_ms = 20.0;
    double arrival_ms = 10'000.0;
    for (u32 i = 0U; i < groups; ++i)
    {
        const double recv_delta_ms = send_delta_ms + delay_growth_ms;
        arrival_ms += recv_delta_ms;
        REQUIRE(trend.filter.update(recv_delta_ms, send_delta_ms, static_cast<i64>(arrival_ms)));
    }
    return trend.filter.state();
}

} // namespace

// ------------------------------------------------------------------------------------------------------------- RFC 9002

TEST_CASE("hesap-net rtt: RFC 9002 section 5 known answers", "[estimators]")
{
    hn::RttEstimator rtt;
    CHECK_FALSE(rtt.has_sample());
    CHECK(rtt.smoothed_ns() == 333 * kMs);
    CHECK(rtt.variation_ns() == 166'500'000);
    CHECK(rtt.pto_ns(0) == (333 * kMs) + (4 * 166'500'000));

    REQUIRE(rtt.on_sample(100 * kMs, 0, 25 * kMs, false));
    CHECK(rtt.has_sample());
    CHECK(rtt.min_ns() == 100 * kMs);
    CHECK(rtt.smoothed_ns() == 100 * kMs);
    CHECK(rtt.variation_ns() == 50 * kMs);

    // latest 180, delay 40 (below max_ack_delay): adjusted 140; rttvar = 3/4 * 50 + 1/4 * 40; srtt = 7/8 * 100 + 1/8 * 140
    REQUIRE(rtt.on_sample(180 * kMs, 40 * kMs, 100 * kMs, true));
    CHECK(rtt.latest_ns() == 180 * kMs);
    CHECK(rtt.min_ns() == 100 * kMs);
    CHECK(rtt.variation_ns() == 47'500'000);
    CHECK(rtt.smoothed_ns() == 105 * kMs);

    // The delay never takes a sample below min_rtt, and min_rtt never holds a delay.
    hn::RttEstimator floor;
    REQUIRE(floor.on_sample(100 * kMs, 0, 0, true));
    REQUIRE(floor.on_sample(110 * kMs, 20 * kMs, 50 * kMs, true));
    CHECK(floor.smoothed_ns() == ((7 * 100 * kMs) + (110 * kMs)) / 8);
    CHECK(floor.min_ns() == 100 * kMs);

    // Confirmed handshakes cap the delay at max_ack_delay; unconfirmed ones do not.
    hn::RttEstimator capped;
    REQUIRE(capped.on_sample(100 * kMs, 0, 0, true));
    REQUIRE(capped.on_sample(200 * kMs, 80 * kMs, 25 * kMs, true));
    CHECK(capped.smoothed_ns() == ((7 * 100 * kMs) + (175 * kMs)) / 8);
    hn::RttEstimator uncapped;
    REQUIRE(uncapped.on_sample(100 * kMs, 0, 0, true));
    REQUIRE(uncapped.on_sample(200 * kMs, 80 * kMs, 25 * kMs, false));
    CHECK(uncapped.smoothed_ns() == ((7 * 100 * kMs) + (120 * kMs)) / 8);
}

TEST_CASE("hesap-net rtt: the recovery durations", "[estimators]")
{
    hn::RttEstimator rtt;
    REQUIRE(rtt.on_sample(8 * kMs, 0, 0, true));
    CHECK(rtt.pto_ns(25 * kMs) == (8 * kMs) + (16 * kMs) + (25 * kMs));
    CHECK(rtt.persistent_congestion_ns(25 * kMs) == 3 * ((8 * kMs) + (16 * kMs) + (25 * kMs)));
    CHECK(rtt.loss_delay_ns() == 9 * kMs);

    // kGranularity floors the spread and the loss delay.
    hn::RttEstimator tiny;
    REQUIRE(tiny.on_sample(1000, 0, 0, true));
    CHECK(tiny.pto_ns(0) == 1000 + hn::RttEstimator::kGranularityNs);
    CHECK(tiny.loss_delay_ns() == hn::RttEstimator::kGranularityNs);

    // §5.2 and §9.4.
    REQUIRE(rtt.on_sample(40 * kMs, 0, 0, true));
    REQUIRE(rtt.on_sample(30 * kMs, 0, 0, true));
    CHECK(rtt.min_ns() == 8 * kMs);
    rtt.on_persistent_congestion();
    CHECK(rtt.min_ns() == 30 * kMs);
    rtt.reset();
    CHECK_FALSE(rtt.has_sample());
    CHECK(rtt.smoothed_ns() == hn::RttEstimator::kInitialRttNs);
}

TEST_CASE("hesap-net rtt: hostile inputs stay in range", "[estimators]")
{
    hn::RttEstimator rtt;
    CHECK_FALSE(rtt.on_sample(-1, 0, 0, true));
    CHECK_FALSE(rtt.has_sample());
    constexpr i64 largest = std::numeric_limits<i64>::max();
    REQUIRE(rtt.on_sample(largest, largest, largest, false));
    CHECK(rtt.smoothed_ns() == hn::RttEstimator::kMaxSampleNs);
    for (u32 i = 0U; i < 64U; ++i)
    {
        REQUIRE(rtt.on_sample((i % 2U == 0U) ? 0 : largest, largest, largest, (i % 4U) == 1U));
        CHECK(rtt.smoothed_ns() >= 0);
        CHECK(rtt.variation_ns() >= 0);
        CHECK(rtt.pto_ns(largest) > 0);
        CHECK(rtt.loss_delay_ns() > 0);
        CHECK(rtt.persistent_congestion_ns(largest) > 0);
    }
    REQUIRE(rtt.on_sample(5, std::numeric_limits<i64>::min(), std::numeric_limits<i64>::min(), true));
    CHECK(rtt.min_ns() == 0);
}

TEST_CASE("hesap-net rtt: the integers follow RFC 9002's real arithmetic", "[estimators]")
{
    SeededWords words(9002U, 1U);
    hn::RttEstimator rtt;
    long double smoothed = 0.0L;
    long double variation = 0.0L;
    long double smallest = 0.0L;
    for (u32 i = 0U; i < 20'000U; ++i)
    {
        const i64 latest = (i64{words.next_u32() % 200'000U} * 1000) + 1'000'000;
        const i64 delay = i64{words.next_u32() % 50'000U} * 1000;
        const i64 max_delay = 25 * kMs;
        const bool confirmed = (words.next_u32() & 1U) != 0U;
        REQUIRE(rtt.on_sample(latest, delay, max_delay, confirmed));
        const auto real_latest = static_cast<long double>(latest);
        if (i == 0U)
        {
            smallest = real_latest;
            smoothed = real_latest;
            variation = real_latest / 2.0L;
        }
        else
        {
            smallest = std::min(smallest, real_latest);
            long double real_delay = static_cast<long double>(delay);
            if (confirmed)
            {
                real_delay = std::min(real_delay, static_cast<long double>(max_delay));
            }
            const long double adjusted = (real_latest >= smallest + real_delay) ? real_latest - real_delay : real_latest;
            variation = (0.75L * variation) + (0.25L * std::fabs(smoothed - adjusted));
            smoothed = (0.875L * smoothed) + (0.125L * adjusted);
        }
        CHECK(static_cast<long double>(rtt.min_ns()) == smallest);
        CHECK(std::fabs(static_cast<long double>(rtt.smoothed_ns()) - smoothed) < 16.0L);
        CHECK(std::fabs(static_cast<long double>(rtt.variation_ns()) - variation) < 16.0L);
    }
}

// ------------------------------------------------------------------------------------------------------------- RFC 3550

TEST_CASE("hesap-net jitter: RFC 3550's fixed point follows its floating form", "[estimators]")
{
    SeededWords words(3550U, 1U);
    hn::InterarrivalJitter jitter;
    double floating = 0.0;
    u32 timestamp = 0xFFFF0000U; // Through the wrap.
    u32 arrival = 12345U;
    i64 previous_transit = 0;
    for (u32 i = 0U; i < 50'000U; ++i)
    {
        timestamp += 960U;
        const u32 network = words.next_u32() % 4000U;
        const u32 arrived = arrival + (960U * i) + network;
        jitter.update(arrived, timestamp);
        const i64 transit = i64{network};
        if (i > 0U)
        {
            floating += (std::fabs(static_cast<double>(transit - previous_transit)) - floating) / 16.0;
        }
        previous_transit = transit;
        CHECK(std::fabs((static_cast<double>(jitter.value_q4()) / 16.0) - floating) < 1.0);
        CHECK(jitter.value() == jitter.value_q4() >> 4U);
    }
}

TEST_CASE("hesap-net jitter: known answers, the wrap and the step bound", "[estimators]")
{
    // A steady stream has none, across the 32-bit wrap of either clock.
    hn::InterarrivalJitter steady;
    u32 timestamp = 0xFFFFFF00U;
    u32 arrival = 0xFFFFFFF0U;
    for (u32 i = 0U; i < 100U; ++i)
    {
        steady.update(arrival, timestamp);
        timestamp += 160U;
        arrival += 160U;
    }
    CHECK(steady.value_q4() == 0U);

    // One step of D = 160: J = 160 / 16 = 10, in Q4 160 - 0, then back by a step in the other direction.
    hn::InterarrivalJitter one;
    one.update(1000U, 0U);
    one.update(1160U + 160U, 160U);
    CHECK(one.value_q4() == 160U);
    CHECK(one.value() == 10U);
    one.update(1320U, 320U);
    CHECK(one.value_q4() == 160U + 160U - ((160U + 8U) >> 4U));

    // A restart of the stream does not read as jitter once bounded, but moves the reference.
    hn::InterarrivalJitter bounded(450'000U);
    bounded.update(1000U, 0U);
    bounded.update(1160U, 160U + 1'000'000U);
    CHECK(bounded.value_q4() == 0U);
    bounded.update(1320U + 16U, 320U + 1'000'000U);
    CHECK(bounded.value_q4() == 16U);
    bounded.reset();
    CHECK(bounded.value_q4() == 0U);
    bounded.update(1000U, 0U);
    CHECK(bounded.value_q4() == 0U);

    // Unbounded, the largest step of either sign is folded in without a surprise.
    hn::InterarrivalJitter wide;
    wide.update(0U, 0U);
    wide.update(0x80000000U, 0U);
    CHECK(wide.value_q4() == 0x80000000U);
}

// ------------------------------------------------------------------------------------------------- PacketGroupTracker

TEST_CASE("hesap-net delay trend: packets group by send time", "[estimators]")
{
    hn::PacketGroupTracker tracker;
    hn::GroupDeltas deltas;
    // Group one: 0, 2 and 5 ms apart at most; the 6 ms packet starts group two, the 12 ms packet group three.
    CHECK_FALSE(tracker.add(0, 50'000, 50'000, 100U, deltas));
    CHECK_FALSE(tracker.add(2'000, 52'500, 52'500, 200U, deltas));
    CHECK_FALSE(tracker.add(5'000, 55'500, 55'500, 300U, deltas));
    CHECK_FALSE(tracker.add(6'000, 61'000, 61'000, 700U, deltas));
    deltas = {};
    REQUIRE(tracker.add(12'000, 70'000, 70'000, 50U, deltas));
    CHECK(deltas.send_us == 6'000 - 5'000);
    CHECK(deltas.arrival_us == 61'000 - 55'500);
    CHECK(deltas.bytes == 700 - 600);
}

TEST_CASE("hesap-net delay trend: a burst merges and a reordered packet is dropped", "[estimators]")
{
    SECTION("the arrival burst joins the group its packets arrived in")
    {
        hn::PacketGroupTracker tracker;
        hn::GroupDeltas deltas;
        CHECK_FALSE(tracker.add(0, 10'000, 10'000, 100U, deltas));
        // 20 ms later on send, arriving 1 ms later: the propagation delta is negative, so the same group.
        CHECK_FALSE(tracker.add(20'000, 11'000, 11'000, 100U, deltas));
        CHECK_FALSE(tracker.add(40'000, 50'000, 50'000, 100U, deltas));
        deltas = {};
        REQUIRE(tracker.add(60'000, 70'000, 70'000, 100U, deltas));
        CHECK(deltas.send_us == 20'000);
        CHECK(deltas.arrival_us == 39'000);
        CHECK(deltas.bytes == 100 - 200);
    }
    SECTION("a same-send-time packet always joins")
    {
        hn::PacketGroupTracker tracker;
        hn::GroupDeltas deltas;
        CHECK_FALSE(tracker.add(0, 10'000, 10'000, 1U, deltas));
        CHECK_FALSE(tracker.add(0, 90'000, 90'000, 1U, deltas));
        CHECK_FALSE(tracker.add(10'000, 100'000, 100'000, 1U, deltas));
        deltas = {};
        REQUIRE(tracker.add(20'000, 110'000, 110'000, 1U, deltas));
        CHECK(deltas.arrival_us == 10'000);
        CHECK(deltas.bytes == 1 - 2);
    }
    SECTION("a packet older than its group is dropped")
    {
        hn::PacketGroupTracker tracker;
        hn::GroupDeltas deltas;
        CHECK_FALSE(tracker.add(100'000, 10'000, 10'000, 1U, deltas));
        CHECK_FALSE(tracker.add(90'000, 20'000, 20'000, 1000U, deltas));
        CHECK_FALSE(tracker.add(110'000, 30'000, 30'000, 2U, deltas));
        deltas = {};
        REQUIRE(tracker.add(120'000, 40'000, 40'000, 3U, deltas));
        CHECK(deltas.bytes == 2 - 1);
    }
}

TEST_CASE("hesap-net delay trend: the tracker restarts on clock jumps and repeated reordering", "[estimators]")
{
    const auto feed = [](hn::PacketGroupTracker& tracker, i64 send, i64 arrival, i64 system)
    {
        hn::GroupDeltas deltas;
        return tracker.add(send, arrival, system, 100U, deltas);
    };
    SECTION("the far clock jumping three seconds past the local one restarts")
    {
        hn::PacketGroupTracker tracker;
        CHECK_FALSE(feed(tracker, 0, 0, 0));
        CHECK_FALSE(feed(tracker, 10'000, 10'000, 10'000));
        CHECK(feed(tracker, 20'000, 3'020'000, 20'000));       // The two groups before the jump.
        CHECK_FALSE(feed(tracker, 30'000, 3'030'000, 30'000)); // The group holding the jump: refused and restarted.
        CHECK_FALSE(feed(tracker, 40'000, 3'040'000, 40'000)); // The first group after the restart.
        CHECK_FALSE(feed(tracker, 50'000, 3'050'000, 50'000));
        CHECK(feed(tracker, 60'000, 3'060'000, 60'000));
    }
    SECTION("a group that completed before the one it follows waits for a later packet of its own")
    {
        hn::PacketGroupTracker tracker;
        CHECK_FALSE(feed(tracker, 0, 100'000, 100'000));
        CHECK_FALSE(feed(tracker, 10'000, 110'000, 110'000));
        CHECK_FALSE(feed(tracker, 20'000, 50'000, 120'000));  // Arrived back in time: joins group two as a burst.
        CHECK_FALSE(feed(tracker, 30'000, 130'000, 130'000)); // Group two completed before group one: dropped.
        CHECK_FALSE(feed(tracker, 12'000, 125'000, 135'000)); // A later packet of group two completes it after.
        CHECK(feed(tracker, 40'000, 140'000, 140'000));
    }
    SECTION("three in a row restart")
    {
        hn::PacketGroupTracker tracker;
        CHECK_FALSE(feed(tracker, 0, 100'000, 100'000));
        CHECK_FALSE(feed(tracker, 10'000, 110'000, 110'000));
        CHECK_FALSE(feed(tracker, 20'000, 50'000, 120'000));
        CHECK_FALSE(feed(tracker, 30'000, 130'000, 130'000));
        CHECK_FALSE(feed(tracker, 40'000, 140'000, 140'000));
        CHECK_FALSE(feed(tracker, 50'000, 150'000, 150'000)); // The third restarts it, so two more groups must start.
        CHECK_FALSE(feed(tracker, 60'000, 160'000, 160'000));
        CHECK_FALSE(feed(tracker, 70'000, 170'000, 170'000));
        CHECK(feed(tracker, 80'000, 180'000, 180'000));
    }
    SECTION("times outside the domain change nothing")
    {
        hn::PacketGroupTracker tracker;
        constexpr i64 out_of_domain = hn::PacketGroupTracker::kMaxTimeUs + 1;
        CHECK_FALSE(feed(tracker, out_of_domain, 0, 0));
        CHECK_FALSE(feed(tracker, 0, -out_of_domain, 0));
        CHECK_FALSE(feed(tracker, 0, 0, out_of_domain));
        CHECK_FALSE(feed(tracker, 0, 0, 0));
        CHECK_FALSE(feed(tracker, 10'000, 10'000, 10'000));
        CHECK(feed(tracker, 20'000, 20'000, 20'000));
        tracker.reset();
        CHECK_FALSE(feed(tracker, 40'000, 40'000, 40'000));
    }
    SECTION("the size difference saturates")
    {
        hn::PacketGroupTracker tracker;
        hn::GroupDeltas deltas;
        CHECK_FALSE(tracker.add(0, 0, 0, 1U, deltas));
        CHECK_FALSE(tracker.add(10'000, 10'000, 10'000, 0xFFFFFFFFU, deltas));
        REQUIRE(tracker.add(20'000, 20'000, 20'000, 1U, deltas));
        CHECK(deltas.bytes == std::numeric_limits<i32>::max());
    }
}

// ---------------------------------------------------------------------------------------------- DelayOveruseDetector

TEST_CASE("hesap-net delay trend: the detector needs time, more than one signal and a rising trend", "[estimators]")
{
    hn::DelayOveruseDetector detector;
    CHECK(detector.update(1000.0, 1, 1.0, 20.0, 0) == DelayState::Normal); // Fewer than two deltas decide nothing.
    CHECK(detector.previous_signal() == 0.0);
    CHECK(detector.update(1.0, 60, 1.0, 20.0, 0) == DelayState::Normal);   // 60 > 12.5: 10 ms over so far, one signal.
    CHECK(detector.update(1.0, 60, 1.0, 20.0, 20) == DelayState::Overusing); // 30 ms over, two signals, not falling.
    CHECK(detector.update(0.0, 60, 1.0, 20.0, 40) == DelayState::Normal);
    CHECK(detector.update(-1.0, 60, 1.0, 20.0, 60) == DelayState::Underusing);

    // A falling signal does not declare overuse, however long it holds.
    hn::DelayOveruseDetector falling;
    double signal = 2.0;
    for (u32 i = 0U; i < 20U; ++i)
    {
        CHECK(falling.update(signal, 60, 1.0, 20.0, i64{i} * 20) == DelayState::Normal);
        signal -= 0.01;
    }

    // Spikes leave the threshold alone; small signals lower it towards 6; large ones raise it, never past 600.
    hn::DelayOveruseDetector adaptive;
    (void)adaptive.update(10.0, 60, 1.0, 1.0, 0);
    (void)adaptive.update(10.0, 60, 1.0, 1.0, 100);
    CHECK(adaptive.threshold() == 12.5);
    for (i64 now = 200; now < 100'000; now += 100)
    {
        (void)adaptive.update(0.0, 60, 1.0, 1.0, now);
    }
    CHECK(adaptive.threshold() == 6.0);
    double scaled = 7.0;
    for (i64 now = 100'000; now < 1'000'000; now += 100)
    {
        (void)adaptive.update(scaled / 60.0, 60, 1.0, 1.0, now);
        scaled += 1.0;
        CHECK(adaptive.threshold() >= 6.0);
        CHECK(adaptive.threshold() <= 600.0);
        if (scaled > adaptive.threshold() + 10.0)
        {
            scaled = adaptive.threshold() + 10.0;
        }
    }
    CHECK(adaptive.threshold() == 600.0);
}

// --------------------------------------------------------------------------------------------------- TrendlineFilter

TEST_CASE("hesap-net trendline: normal, overusing and underusing", "[estimators]")
{
    CHECK(run_trend(0.0) == DelayState::Normal);
    CHECK(run_trend(+4.0) == DelayState::Overusing);
    CHECK(run_trend(-4.0) == DelayState::Underusing);
}

TEST_CASE("hesap-net trendline: the slope is the least-squares fit of the smoothed delays", "[estimators]")
{
    SeededWords words(20U, 1U);
    Trend trend(12U);
    long double accumulated = 0.0L;
    long double smoothed = 0.0L;
    crd::containers::Array<long double> xs;
    crd::containers::Array<long double> ys;
    i64 arrival = 7000;
    i64 first = arrival;
    for (u32 i = 0U; i < 200U; ++i)
    {
        const double recv_delta = static_cast<double>(words.next_u32() % 40U) + 0.25;
        const double send_delta = 20.0;
        arrival += static_cast<i64>(words.next_u32() % 40U) + 1;
        REQUIRE(trend.filter.update(recv_delta, send_delta, arrival));
        accumulated += static_cast<long double>(recv_delta - send_delta);
        smoothed = (0.9L * smoothed) + (0.1L * accumulated);
        xs.push_back(static_cast<long double>(arrival - first));
        ys.push_back(smoothed);
        CHECK(trend.filter.accumulated_delay_ms() == Approx(static_cast<double>(accumulated)).margin(1e-6));
        CHECK(trend.filter.smoothed_delay_ms() == Approx(static_cast<double>(smoothed)).margin(1e-6));
        if (xs.size() >= 12U)
        {
            long double mean_x = 0.0L;
            long double mean_y = 0.0L;
            for (crd::usize k = xs.size() - 12U; k < xs.size(); ++k)
            {
                mean_x += xs[k] / 12.0L;
                mean_y += ys[k] / 12.0L;
            }
            long double top = 0.0L;
            long double bottom = 0.0L;
            for (crd::usize k = xs.size() - 12U; k < xs.size(); ++k)
            {
                top += (xs[k] - mean_x) * (ys[k] - mean_y);
                bottom += (xs[k] - mean_x) * (xs[k] - mean_x);
            }
            CHECK(trend.filter.trend() == Approx(static_cast<double>(top / bottom)).margin(1e-6));
        }
    }
}

TEST_CASE("hesap-net trendline: the sorted window and the slope cap", "[estimators]")
{
    SECTION("sorting decides which group leaves the window: the earliest arrival, not the earliest fed")
    {
        Trend sorted(10U, {.sort = true});
        Trend plain(10U);
        const i64 arrivals[] = {120, 100, 140, 160, 180, 200, 220, 240, 260, 280, 300};
        for (const i64 arrival : arrivals)
        {
            REQUIRE(sorted.filter.update(20.5, 20.0, arrival));
            REQUIRE(plain.filter.update(20.5, 20.0, arrival));
        }
        CHECK(sorted.filter.trend() != plain.filter.trend());
    }
    SECTION("the cap limits a rising slope by the lowest delays at the two ends")
    {
        Trend capped(10U, {.cap = true, .beginning_packets = 3U, .end_packets = 3U, .cap_uncertainty = 0.0});
        Trend free_slope(10U);
        // A delay that rises on the middle packets only: the fit rises, the two ends say it has not.
        const double recv[] = {20.0, 20.0, 20.0, 40.0, 40.0, 40.0, 0.0, 0.0, 0.0, 20.0};
        i64 arrival = 1000;
        for (const double delta : recv)
        {
            arrival += 20;
            REQUIRE(capped.filter.update(delta, 20.0, arrival));
            REQUIRE(free_slope.filter.update(delta, 20.0, arrival));
        }
        CHECK(free_slope.filter.trend() > 0.0);
        CHECK(capped.filter.trend() == 0.0);
    }
    SECTION("the cap passes when the window's ends are less than a millisecond apart")
    {
        Trend capped(10U, {.cap = true, .beginning_packets = 1U, .end_packets = 1U});
        for (u32 i = 0U; i < 10U; ++i)
        {
            REQUIRE(capped.filter.update(25.0, 20.0, 500));
        }
        CHECK(capped.filter.trend() == 0.0); // Every x equal: no fit, the previous trend stays.
    }
}

TEST_CASE("hesap-net trendline: refusals", "[estimators]")
{
    crd::containers::Array<hn::TrendlineSample> storage;
    storage.resize(hn::TrendlineFilter::storage_samples(hn::TrendlineFilter::kMaxWindow));
    const std::span<hn::TrendlineSample> room{storage.data(), storage.size()};
    hn::TrendlineFilter filter;
    CHECK_FALSE(filter.update(1.0, 1.0, 0)); // Before init().
    CHECK_FALSE(filter.init(room, 9U));
    CHECK_FALSE(filter.init(room, 201U));
    CHECK_FALSE(filter.init(room.first(20U), 20U));
    CHECK_FALSE(filter.update(1.0, 1.0, 0)); // A refused init() leaves it refusing.
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .beginning_packets = 0U}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .end_packets = 0U}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .beginning_packets = 21U}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .end_packets = 21U}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .beginning_packets = 11U, .end_packets = 10U}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .cap_uncertainty = 0.03}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .cap_uncertainty = -0.01}));
    CHECK_FALSE(filter.init(room, 20U, {.cap = true, .cap_uncertainty = std::numeric_limits<double>::quiet_NaN()}));
    CHECK(filter.init(room.first(21U), 20U));
    CHECK(filter.init(room, 20U, {.beginning_packets = 0U})); // Cap options only bind a capped filter.

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK_FALSE(filter.update(nan, 1.0, 0));
    CHECK_FALSE(filter.update(1.0, nan, 0));
    CHECK_FALSE(filter.update(infinity, 1.0, 0));
    CHECK_FALSE(filter.update(1.0, -infinity, 0));
    CHECK_FALSE(filter.update(2e16, 1.0, 0));
    CHECK(filter.accumulated_delay_ms() == 0.0);
    CHECK(filter.update(1e16, -1e16, std::numeric_limits<i64>::max()));
    CHECK(filter.update(1e16, -1e16, std::numeric_limits<i64>::min()));
    CHECK(std::isfinite(filter.accumulated_delay_ms()));
}

// ------------------------------------------------------------------------------------------------- KalmanDelayFilter

TEST_CASE("hesap-net kalman: it finds a bottleneck's capacity and queue growth", "[estimators]")
{
    // A 1 Mbit/s bottleneck, 125 bytes per ms, seen through group size changes: the slope converges on its inverse.
    SeededWords words(21U, 1U);
    hn::KalmanDelayFilter kalman;
    hn::DelayOveruseDetector detector;
    constexpr double bytes_per_ms = 125.0;
    i64 now = 0;
    for (u32 i = 0U; i < 4000U; ++i)
    {
        const i32 size_delta = static_cast<i32>(words.next_u32() % 2001U) - 1000;
        const double send_delta = 33.0;
        const double recv_delta = send_delta + (size_delta / bytes_per_ms);
        REQUIRE(kalman.update(recv_delta, send_delta, size_delta, detector.state()));
        now += 33;
        (void)detector.update(kalman.offset(), kalman.deltas(), 1.0, send_delta, now);
    }
    CHECK(kalman.slope() == Approx(1.0 / bytes_per_ms).margin(1e-4));
    CHECK(kalman.offset() == Approx(0.0).margin(1e-3));
    CHECK(kalman.deltas() == 1000);
    CHECK(kalman.restarts() == 0U);
    CHECK(detector.state() == DelayState::Normal);

    // A constant queue growth shows up as the offset, and the detector declares overuse.
    for (u32 i = 0U; i < 300U; ++i)
    {
        REQUIRE(kalman.update(33.0 + 4.0, 33.0, 0, detector.state()));
        now += 33;
        (void)detector.update(kalman.offset(), kalman.deltas(), 1.0, 33.0, now);
    }
    CHECK(kalman.offset() > 1.0);
    CHECK(detector.state() == DelayState::Overusing);
}

TEST_CASE("hesap-net kalman: one update follows the textbook form", "[estimators]")
{
    hn::KalmanDelayFilter kalman;
    REQUIRE(kalman.update(40.0, 30.0, 1000, DelayState::Normal));

    // Restated with P = [[100 + q0, 0], [0, 0.1 + q1]], h = (1000, 1), z = 10.
    const double p00 = 100.0 + 1e-13;
    const double p11 = 0.1 + 1e-3;
    const double residual = 10.0 - (8.0 / 512.0 * 1000.0);
    const double beta = std::pow(0.99, 30.0 * 30.0 / 1000.0); // The libm's: the engine's is within rounding of it.
    const double average = (1.0 - beta) * residual;
    const double variance = (beta * 50.0) + ((1.0 - beta) * (average - residual) * (average - residual));
    const double denominator = variance + (1000.0 * 1000.0 * p00) + p11;
    CHECK(kalman.noise_variance() == Approx(variance));
    CHECK(kalman.slope() == Approx((8.0 / 512.0) + (1000.0 * p00 / denominator * residual)));
    CHECK(kalman.offset() == Approx(p11 / denominator * residual));
    CHECK(kalman.deltas() == 1);
}

TEST_CASE("hesap-net kalman: hostile inputs", "[estimators]")
{
    hn::KalmanDelayFilter kalman;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(kalman.update(nan, 1.0, 0, DelayState::Normal));
    CHECK_FALSE(kalman.update(1.0, nan, 0, DelayState::Normal));
    CHECK_FALSE(kalman.update(2e16, 1.0, 0, DelayState::Normal));
    CHECK(kalman.deltas() == 0);
    SeededWords words(22U, 1U);
    constexpr DelayState states[] = {DelayState::Normal, DelayState::Overusing, DelayState::Underusing};
    for (u32 i = 0U; i < 20'000U; ++i)
    {
        const double scale = std::ldexp(1.0, static_cast<int>(words.next_u32() % 54U));
        const double recv = (static_cast<double>(words.next_u32()) / 4294967296.0 - 0.5) * scale;
        const double send = (static_cast<double>(words.next_u32()) / 4294967296.0 - 0.25) * scale;
        const i32 size = static_cast<i32>(words.next_u32());
        REQUIRE(kalman.update(recv, send, size, states[words.next_u32() % 3U]));
        REQUIRE(std::isfinite(kalman.slope()));
        REQUIRE(std::isfinite(kalman.offset()));
        REQUIRE(std::isfinite(kalman.noise_variance()));
        REQUIRE(kalman.noise_variance() >= 1.0);
    }
    CHECK(kalman.restarts() > 0U); // Absurd negative send deltas took the noise estimate out, and it came back.
}

// ---------------------------------------------------------------------------------------------------- WindowedFilter

TEST_CASE("hesap-net windowed filter: the extremum follows the window", "[estimators]")
{
    hn::WindowedMax<u32> maximum(100U);
    CHECK_FALSE(maximum.has_sample());
    CHECK(maximum.best() == 0U);
    maximum.update(10U, 0U);
    CHECK(maximum.has_sample());
    CHECK(maximum.best() == 10U);
    maximum.update(5U, 30U);  // Past a quarter: the second best comes from here on.
    maximum.update(7U, 60U);  // Better than the second.
    maximum.update(3U, 90U);  // Worse than the third, which is the second: kept out.
    CHECK(maximum.best() == 10U);
    CHECK(maximum.second() == 7U);
    maximum.update(2U, 101U); // The best aged out; the second takes over.
    CHECK(maximum.best() == 7U);
    maximum.update(20U, 102U); // A new best takes all three.
    CHECK(maximum.best() == 20U);
    CHECK(maximum.second() == 20U);
    CHECK(maximum.third() == 20U);
    maximum.update(1U, 500U); // The third is older than a window too: restart from this sample.
    CHECK(maximum.best() == 1U);
    maximum.update(9U, 400U); // A time before the samples reads as a full window later: restart.
    CHECK(maximum.best() == 9U);

    hn::WindowedMin<i64> minimum(10U);
    minimum.update(100, 1U);
    minimum.update(200, 2U);
    CHECK(minimum.best() == 100);
    minimum.update(50, 3U);
    CHECK(minimum.best() == 50);
    minimum.set_window(1U);
    minimum.update(70, 9U);
    CHECK(minimum.best() == 70);
    minimum.restart(5, 10U);
    CHECK(minimum.best() == 5);
    CHECK(minimum.third() == 5);
}

TEST_CASE("hesap-net windowed filter: against a brute-force window", "[estimators]")
{
    SeededWords words(23U, 1U);
    for (const u32 window : {16U, 100U, 1000U})
    {
        hn::WindowedMax<u32> maximum(window);
        hn::WindowedMin<u32> minimum(window);
        crd::containers::Array<u32> values;
        crd::containers::Array<u64> times;
        u64 time = 0U;
        for (u32 i = 0U; i < 5000U; ++i)
        {
            time += words.next_u32() % 8U;
            const u32 value = words.next_u32() % 1000U;
            values.push_back(value);
            times.push_back(time);
            maximum.update(value, time);
            minimum.update(value, time);
            bool max_in_window = false;
            bool min_in_window = false;
            u32 true_max = 0U;
            u32 true_min = 1000U;
            for (crd::usize k = values.size(); k > 0U && time - times[k - 1U] <= window; --k)
            {
                max_in_window = max_in_window || values[k - 1U] == maximum.best();
                min_in_window = min_in_window || values[k - 1U] == minimum.best();
                true_max = std::max(true_max, values[k - 1U]);
                true_min = std::min(true_min, values[k - 1U]);
            }
            REQUIRE(max_in_window); // A sample of the window, so never better than its true best.
            REQUIRE(min_in_window);
            REQUIRE(maximum.best() <= true_max);
            REQUIRE(minimum.best() >= true_min);
            REQUIRE(maximum.best() >= value); // And never worse than the newest sample.
            REQUIRE(minimum.best() <= value);
        }
    }
}

TEST_CASE("hesap-net windowed filter: exact on monotonic series", "[estimators]")
{
    hn::WindowedMax<u32> maximum(10U);
    for (u32 i = 0U; i < 100U; ++i)
    {
        maximum.update(i, u64{i});
        CHECK(maximum.best() == i); // Rising: the newest.
    }
    hn::WindowedMin<u32> minimum(10U);
    for (u32 i = 0U; i < 100U; ++i)
    {
        minimum.update(i, u64{i});
        CHECK(minimum.best() >= ((i > 10U) ? i - 10U : 0U));
        CHECK(minimum.best() <= ((i > 10U) ? i - 10U + 5U : 0U)); // The filter's bound on a rising series.
    }
}

// ------------------------------------------------------------------------------------------------------ delivery rate

TEST_CASE("hesap-net delivery rate: the longer interval decides", "[estimators]")
{
    static_assert(hn::delivery_rate(1000U, 1'000'000, 2'000'000) == 500'000.0);
    static_assert(hn::delivery_rate(1000U, 4'000'000, 2'000'000) == 250'000.0);
    static_assert(hn::delivery_rate(1000U, 0, 0) == 0.0);
    static_assert(hn::delivery_rate(1000U, -5, -1) == 0.0);
    CHECK(hn::delivery_rate(0U, 10, 10) == 0.0);
    CHECK(std::isfinite(hn::delivery_rate(std::numeric_limits<u64>::max(), 1, 1)));
}

// ------------------------------------------------------------------------------------------------- AckedRateEstimator

TEST_CASE("hesap-net acked rate: the first window, then Bayes", "[estimators]")
{
    hn::AckedRateEstimator rate;
    CHECK_FALSE(rate.has_estimate());
    rate.update(0, 1000U, false);
    rate.update(250, 1000U, false);
    CHECK_FALSE(rate.has_estimate());
    CHECK(rate.peek_kbps() == 8.0F * 2000.0F / 250.0F);
    rate.update(500, 1000U, false); // The 500 ms window closes on the 2000 bytes before this update.
    REQUIRE(rate.has_estimate());
    CHECK(rate.kbps() == 8.0F * 2000.0F / 500.0F);
    CHECK(rate.variance() == 50.0F);

    float estimate = rate.kbps();
    rate.update(650, 2000U, false); // The next window, 150 ms, holds the 1000 bytes of the last update.
    const float sample = 8.0F * 1000.0F / 150.0F;
    const float uncertainty = 10.0F * std::fabs(estimate - sample) / (estimate + 0.0F);
    const float sample_variance = uncertainty * uncertainty;
    const float predicted = 50.0F + 5.0F;
    estimate = ((sample_variance * estimate) + (predicted * sample)) / (sample_variance + predicted);
    CHECK(rate.kbps() == Approx(estimate));
    CHECK(rate.variance() == Approx(sample_variance * predicted / (sample_variance + predicted)));

    const float before = rate.variance();
    rate.expect_fast_change();
    CHECK(rate.variance() == before + 200.0F);
}

TEST_CASE("hesap-net acked rate: time moving backwards, gaps and the domain", "[estimators]")
{
    SECTION("time moving backwards restarts the window")
    {
        hn::AckedRateEstimator rate;
        rate.update(1000, 1000U, false);
        rate.update(1400, 1000U, false);
        rate.update(100, 1000U, false);
        rate.update(599, 1000U, false);
        CHECK_FALSE(rate.has_estimate());
        rate.update(600, 1000U, false);
        REQUIRE(rate.has_estimate());
        CHECK(rate.kbps() == 8.0F * 2000.0F / 500.0F);
    }
    SECTION("a gap of more than a window drops what it held")
    {
        hn::AckedRateEstimator rate;
        rate.update(0, 100'000U, false);
        rate.update(600, 1000U, false); // Nothing was seen for 600 ms: the sum is gone, 100 ms of window remain.
        CHECK_FALSE(rate.has_estimate());
        rate.update(1000, 1000U, false);
        REQUIRE(rate.has_estimate());
        CHECK(rate.kbps() == 8.0F * 1000.0F / 500.0F);
    }
    SECTION("a time past the domain is refused")
    {
        hn::AckedRateEstimator rate;
        rate.update(hn::AckedRateEstimator::kMaxTimeMs + 1, 1000U, false);
        rate.update(0, 1000U, false);
        rate.update(500, 1000U, false);
        CHECK(rate.kbps() == 8.0F * 1000.0F / 500.0F);
    }
    SECTION("a window that holds nothing starts at zero and recovers")
    {
        hn::AckedRateEstimator rate;
        rate.update(0, 0U, false);
        rate.update(500, 1000U, false);
        CHECK(rate.kbps() == 0.0F);
        rate.update(650, 1000U, false);
        CHECK(rate.kbps() == 8.0F * 1000.0F / 150.0F);
    }
    SECTION("an estimate that decays to nothing stays finite")
    {
        hn::AckedRateEstimator rate;
        rate.update(0, 1000U, false);
        i64 now = 500;
        rate.update(now, 0U, false);
        for (u32 i = 0U; i < 100'000U; ++i)
        {
            now += 150;
            rate.update(now, 0U, false);
            REQUIRE(std::isfinite(rate.kbps()));
            REQUIRE(std::isfinite(rate.variance()));
        }
        now += 150;
        rate.update(now, 0xFFFFFFFFU, false);
        now += 150;
        rate.update(now, 0U, false);
        REQUIRE(std::isfinite(rate.kbps()));
        REQUIRE(std::isfinite(rate.variance()));
    }
}

TEST_CASE("hesap-net acked rate: the options", "[estimators]")
{
    hn::AckedRateEstimator rate;
    CHECK_FALSE(rate.init({.initial_window_ms = 149}));
    CHECK_FALSE(rate.init({.initial_window_ms = 1001}));
    CHECK_FALSE(rate.init({.window_ms = 149}));
    CHECK_FALSE(rate.init({.window_ms = 1001}));
    CHECK_FALSE(rate.init({.uncertainty_scale = -1.0F}));
    CHECK_FALSE(rate.init({.application_limited_scale = std::numeric_limits<float>::infinity()}));
    CHECK_FALSE(rate.init({.small_sample_scale = std::numeric_limits<float>::quiet_NaN()}));
    CHECK_FALSE(rate.init({.small_sample_bytes = -1}));
    CHECK_FALSE(rate.init({.symmetry_cap_kbps = -1.0F}));
    CHECK_FALSE(rate.init({.floor_kbps = -1.0F}));

    SECTION("a falling sample is believed less while application limited or small")
    {
        const auto fall = [](const hn::AckedRateOptions& options, bool limited)
        {
            hn::AckedRateEstimator fallen;
            REQUIRE(fallen.init(options));
            fallen.update(0, 100'000U, false);
            fallen.update(150, 1000U, false);
            fallen.update(300, 0U, limited);
            return fallen.kbps();
        };
        const hn::AckedRateOptions base{.initial_window_ms = 150};
        const float plain = fall(base, false);
        CHECK(fall({.initial_window_ms = 150, .application_limited_scale = 40.0F}, true) > plain);
        CHECK(fall({.initial_window_ms = 150, .application_limited_scale = 40.0F}, false) == plain);
        CHECK(fall({.initial_window_ms = 150, .small_sample_scale = 40.0F, .small_sample_bytes = 2000}, false) > plain);
        CHECK(fall({.initial_window_ms = 150, .small_sample_scale = 40.0F, .small_sample_bytes = 500}, false) == plain);
    }
    SECTION("the floor holds and the symmetry cap narrows a rise's uncertainty")
    {
        hn::AckedRateEstimator floored;
        REQUIRE(floored.init({.initial_window_ms = 150, .floor_kbps = 1000.0F}));
        floored.update(0, 10'000U, false);
        floored.update(150, 10U, false);
        floored.update(300, 0U, false);
        CHECK(floored.kbps() == 1000.0F);

        const auto rise = [](float cap)
        {
            hn::AckedRateEstimator risen;
            REQUIRE(risen.init({.initial_window_ms = 150, .symmetry_cap_kbps = cap}));
            risen.update(0, 1000U, false);
            risen.update(150, 100'000U, false);
            risen.update(300, 0U, false);
            return risen.kbps();
        };
        CHECK(rise(100'000.0F) > rise(0.0F));
    }
}
