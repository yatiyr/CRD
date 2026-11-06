// HESAP-NET's delay trend and acknowledged rate held bit for bit to libwebrtc's own estimators (crd-webrtc-bwe-peer,
// the pinned branch's files with only its plumbing and its one pow shimmed): every packet of every simulated path through the group
// tracker, every trendline option, the Kalman filter with its detector and the Bayesian rate, each double and float
// compared by its bits after every update. The peer's fields are read by compiling its headers with `private` open.

#include "estimator_paths.hpp"

#include <crd/hesap/net/delay_trend.hpp>
#include <crd/hesap/net/rate.hpp>

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>

#define private public
#include "modules/congestion_controller/goog_cc/bitrate_estimator.h"
#include "modules/congestion_controller/goog_cc/inter_arrival_delta.h"
#include "modules/congestion_controller/goog_cc/trendline_estimator.h"
#include "modules/remote_bitrate_estimator/overuse_detector.h"
#include "modules/remote_bitrate_estimator/overuse_estimator.h"
#undef private

namespace hn = crd::hesap::net;
using crd::i64;
using crd::u32;
using crd::u64;
using estimator_paths::kProfiles;
using estimator_paths::Packet;
using estimator_paths::Path;
using estimator_paths::Profile;
using webrtc::BandwidthUsage;
using webrtc::DataSize;
using webrtc::TimeDelta;
using webrtc::Timestamp;

namespace
{

constexpr u32 kPacketsPerProfile = 200'000U;

class Trials final : public webrtc::FieldTrialsView
{
public:
    Trials(const char* key, const char* value) : m_key(key), m_value(value) {}

    std::string Lookup(std::string_view key) const override // crd-lint-allow-std-container: its interface
    {
        return key == m_key ? m_value : "";
    }

private:
    const char* m_key;
    const char* m_value;
};

[[nodiscard]] hn::DelayState ours(BandwidthUsage usage)
{
    switch (usage)
    {
    case BandwidthUsage::kBwOverusing:
        return hn::DelayState::Overusing;
    case BandwidthUsage::kBwUnderusing:
        return hn::DelayState::Underusing;
    default:
        return hn::DelayState::Normal;
    }
}

[[nodiscard]] bool same(double a, double b)
{
    return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
}

[[nodiscard]] bool same(float a, float b)
{
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

struct Coverage
{
    u64 deltas = 0U;
    u64 refused = 0U;
    u64 states[3] = {0U, 0U, 0U};
};

struct TrendlineArm
{
    const char* trial;
    u32 window;
    hn::TrendlineOptions options;
};

constexpr TrendlineArm kTrendlineArms[] = {
    {"", 20U, {}},
    {"window_size:10", 10U, {}},
    {"window_size:200", 200U, {}},
    {"sort:true", 20U, {.sort = true}},
    {"cap:true", 20U, {.cap = true}},
    {"cap:true,beginning_packets:3,end_packets:5,cap_uncertainty:0.0125,window_size:30", 30U,
     {.cap = true, .beginning_packets = 3U, .end_packets = 5U, .cap_uncertainty = 0.0125}},
    {"sort:true,cap:true,beginning_packets:7,end_packets:7,cap_uncertainty:0.025,window_size:50", 50U,
     {.sort = true, .cap = true, .beginning_packets = 7U, .end_packets = 7U, .cap_uncertainty = 0.025}},
};

} // namespace

TEST_CASE("hesap-net estimators: the group tracker and every trendline option against libwebrtc", "[estimators][webrtc]")
{
    Coverage total;
    for (const TrendlineArm& arm : kTrendlineArms)
    {
        CAPTURE(arm.trial);
        for (const Profile& profile : kProfiles)
        {
            CAPTURE(profile.name);
            const Trials trials(webrtc::TrendlineEstimatorSettings::kKey, arm.trial);
            webrtc::TrendlineEstimator peer(trials, nullptr);
            webrtc::InterArrivalDelta peer_groups(TimeDelta::Millis(5));
            hn::TrendlineSample storage[hn::TrendlineFilter::storage_samples(hn::TrendlineFilter::kMaxWindow)];
            hn::TrendlineFilter filter;
            REQUIRE(filter.init(storage, arm.window, arm.options));
            REQUIRE(peer.settings_.window_size == arm.window);
            hn::PacketGroupTracker groups;

            Path path(profile, 7U);
            for (u32 i = 0U; i < kPacketsPerProfile; ++i)
            {
                const Packet packet = path.next();
                TimeDelta peer_send = TimeDelta::Zero();
                TimeDelta peer_recv = TimeDelta::Zero();
                int peer_size = 0;
                const bool peer_calculated = peer_groups.ComputeDeltas(
                    Timestamp::Micros(packet.send_us), Timestamp::Micros(packet.arrival_us),
                    Timestamp::Micros(packet.system_us), packet.bytes, &peer_send, &peer_recv, &peer_size);
                hn::GroupDeltas deltas;
                const bool calculated =
                    groups.add(packet.send_us, packet.arrival_us, packet.system_us, packet.bytes, deltas);
                if (peer_calculated != calculated)
                {
                    FAIL("the group decision differs at packet " << i);
                }
                peer.Update(peer_recv.ms<double>(), peer_send.ms<double>(), packet.send_us / 1000,
                            Timestamp::Micros(packet.arrival_us).ms(), packet.bytes, peer_calculated);
                if (!calculated)
                {
                    ++total.refused;
                    continue;
                }
                if (deltas.send_us != peer_send.us() || deltas.arrival_us != peer_recv.us() ||
                    deltas.bytes != peer_size)
                {
                    FAIL("the group deltas differ at packet " << i);
                }
                REQUIRE(filter.update(deltas, packet.arrival_us));
                ++total.deltas;
                const bool identical = filter.state() == ours(peer.State()) && same(filter.trend(), peer.prev_trend_) &&
                                       same(filter.accumulated_delay_ms(), peer.accumulated_delay_) &&
                                       same(filter.smoothed_delay_ms(), peer.smoothed_delay_) &&
                                       same(filter.detector().threshold(), peer.threshold_);
                if (!identical)
                {
                    FAIL("the trendline differs at packet " << i << ": trend " << filter.trend() << " vs "
                                                            << peer.prev_trend_ << ", threshold "
                                                            << filter.detector().threshold() << " vs " << peer.threshold_);
                }
                ++total.states[static_cast<u32>(filter.state())];
            }
        }
    }
    INFO("deltas " << total.deltas << " not completed " << total.refused);
    CHECK(total.deltas > 2'000'000U);
    CHECK(total.refused > 500'000U);
    CHECK(total.states[0] > 1000U); // Normal, Overusing and Underusing were all reached and compared.
    CHECK(total.states[1] > 1000U);
    CHECK(total.states[2] > 1000U);
}

TEST_CASE("hesap-net estimators: the Kalman filter and its detector against libwebrtc", "[estimators][webrtc]")
{
    Coverage total;
    for (const Profile& profile : kProfiles)
    {
        CAPTURE(profile.name);
        webrtc::OveruseEstimator peer;
        webrtc::OveruseDetector peer_detector;
        webrtc::InterArrivalDelta peer_groups(TimeDelta::Millis(5));
        hn::KalmanDelayFilter kalman;
        hn::DelayOveruseDetector detector;
        hn::PacketGroupTracker groups;

        Path path(profile, 11U);
        for (u32 i = 0U; i < kPacketsPerProfile; ++i)
        {
            const Packet packet = path.next();
            TimeDelta peer_send = TimeDelta::Zero();
            TimeDelta peer_recv = TimeDelta::Zero();
            int peer_size = 0;
            const bool peer_calculated = peer_groups.ComputeDeltas(
                Timestamp::Micros(packet.send_us), Timestamp::Micros(packet.arrival_us),
                Timestamp::Micros(packet.system_us), packet.bytes, &peer_send, &peer_recv, &peer_size);
            hn::GroupDeltas deltas;
            REQUIRE(peer_calculated ==
                    groups.add(packet.send_us, packet.arrival_us, packet.system_us, packet.bytes, deltas));
            if (!peer_calculated)
            {
                continue;
            }
            ++total.deltas;
            // Its receive-side path takes the arrival delta in whole milliseconds.
            const i64 recv_ms = peer_recv.ms();
            const double send_ms = peer_send.ms<double>();
            const i64 now_ms = packet.arrival_us / 1000;
            peer.Update(recv_ms, send_ms, peer_size, peer_detector.State(), now_ms);
            REQUIRE(kalman.update(static_cast<double>(recv_ms), send_ms, deltas.bytes, detector.state()));
            (void)peer_detector.Detect(peer.offset(), send_ms, peer.num_of_deltas(), now_ms);
            (void)detector.update(kalman.offset(), kalman.deltas(), 1.0, send_ms, now_ms);
            const bool identical = same(kalman.offset(), peer.offset()) && same(kalman.slope(), peer.slope_) &&
                                   same(kalman.noise_variance(), peer.var_noise()) &&
                                   kalman.deltas() == peer.num_of_deltas() &&
                                   detector.state() == ours(peer_detector.State()) &&
                                   same(detector.threshold(), peer_detector.threshold_) &&
                                   kalman.restarts() == 0U;
            if (!identical)
            {
                FAIL("the Kalman filter differs at packet " << i << ": offset " << kalman.offset() << " vs "
                                                            << peer.offset() << ", restarts "
                                                            << kalman.restarts());
            }
            ++total.states[static_cast<u32>(detector.state())];
        }
    }
    INFO("deltas " << total.deltas);
    CHECK(total.deltas > 400'000U);
    CHECK(total.states[0] > 1000U);
    CHECK(total.states[1] > 100U);
    CHECK(total.states[2] > 100U);
}

TEST_CASE("hesap-net estimators: the acknowledged rate against libwebrtc", "[estimators][webrtc]")
{
    const auto kilobits = [](int value) { return webrtc::DataRate::KilobitsPerSec(value).kbps<float>(); };
    struct Arm
    {
        const char* trial;
        hn::AckedRateOptions options;
    };
    const Arm arms[] = {
        {"", {}},
        {"initial_window_ms:350,window_ms:250", {.initial_window_ms = 350, .window_ms = 250}},
        {"scale:5,scale_alr:20,scale_small:30,small_thresh:500,symmetry_cap:128,floor:10",
         {.uncertainty_scale = 5.0F,
          .application_limited_scale = 20.0F,
          .small_sample_scale = 30.0F,
          .small_sample_bytes = 500,
          .symmetry_cap_kbps = kilobits(128),
          .floor_kbps = kilobits(10)}},
        {"initial_window_ms:150,window_ms:1000,scale:0.5,symmetry_cap:100000",
         {.initial_window_ms = 150,
          .window_ms = 1000,
          .uncertainty_scale = 0.5F,
          .symmetry_cap_kbps = kilobits(100000)}},
    };
    u64 estimates = 0U;
    for (const Arm& arm : arms)
    {
        CAPTURE(arm.trial);
        for (const Profile& profile : kProfiles)
        {
            CAPTURE(profile.name);
            const Trials trials("WebRTC-BweThroughputWindowConfig", arm.trial);
            webrtc::BitrateEstimator peer(&trials);
            hn::AckedRateEstimator rate;
            REQUIRE(rate.init(arm.options));
            SeededWords words(13U, 2U);
            Path path(profile, 13U);
            for (u32 i = 0U; i < kPacketsPerProfile; ++i)
            {
                const Packet packet = path.next();
                const i64 now_ms = packet.arrival_us / 1000;
                const bool limited = (words.next_u32() & 7U) == 0U;
                peer.Update(Timestamp::Millis(now_ms), DataSize::Bytes(packet.bytes), limited);
                rate.update(now_ms, packet.bytes, limited);
                if ((words.next_u32() & 1023U) == 0U)
                {
                    peer.ExpectFastRateChange();
                    rate.expect_fast_change();
                }
                if (!same(rate.kbps(), peer.bitrate_estimate_kbps_) || !same(rate.variance(), peer.bitrate_estimate_var_))
                {
                    FAIL("the rate differs at packet " << i << ": " << rate.kbps() << " vs "
                                                       << peer.bitrate_estimate_kbps_ << ", variance " << rate.variance()
                                                       << " vs " << peer.bitrate_estimate_var_);
                }
                estimates += rate.has_estimate() ? 1U : 0U;
            }
        }
    }
    CHECK(estimates > 5'000'000U);
}
