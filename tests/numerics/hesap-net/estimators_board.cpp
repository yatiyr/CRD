// crd-hesap-estimators-board: nanoseconds per update, bytes per connection and allocations of hesap-net's delay trend
// and acknowledged rate against libwebrtc's (crd-webrtc-bwe-peer), on the same recorded packets of a simulated path.
// Every arm consumes the same inputs and the two sides' final states are compared, so a faster wrong answer fails.
//
// Usage: crd-hesap-estimators-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 15)
//   quick   one round over few packets (the smoke CTest)

#include "estimator_paths.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/delay_trend.hpp>
#include <crd/hesap/net/rate.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#define private public
#include "modules/congestion_controller/goog_cc/bitrate_estimator.h"
#include "modules/congestion_controller/goog_cc/inter_arrival_delta.h"
#include "modules/congestion_controller/goog_cc/trendline_estimator.h"
#include "modules/remote_bitrate_estimator/overuse_detector.h"
#include "modules/remote_bitrate_estimator/overuse_estimator.h"
#undef private

namespace
{
crd::u64 g_allocations = 0U;
crd::u64 g_allocated_bytes = 0U;
} // namespace

// Counts what each arm's connection state allocates.
void* operator new(std::size_t size)
{
    ++g_allocations;
    g_allocated_bytes += size;
    void* memory = std::malloc(size != 0U ? size : 1U);
    if (memory == nullptr)
    {
        std::abort();
    }
    return memory;
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace hn = crd::hesap::net;
using crd::i32;
using crd::i64;
using crd::u32;
using crd::u64;
using estimator_paths::Packet;
using estimator_paths::Path;
using webrtc::DataSize;
using webrtc::TimeDelta;
using webrtc::Timestamp;

namespace
{

class NoTrials final : public webrtc::FieldTrialsView
{
public:
    std::string Lookup(std::string_view) const override // crd-lint-allow-std-container: its interface
    {
        return "";
    }
};

struct Completed
{
    hn::GroupDeltas deltas;
    i64 arrival_us = 0;
};

struct Series
{
    crd::containers::Array<Packet> packets;
    crd::containers::Array<Completed> completed;
};

struct Run
{
    double seconds = 0.0;
    double signature = 0.0; // The final state, compared across the two sides.
    u64 allocations = 0U;
    u64 allocated_bytes = 0U;
    crd::usize state_bytes = 0U;
};

template <class Body>
[[nodiscard]] Run timed(crd::usize state_bytes, Body&& body)
{
    const u64 allocations = g_allocations;
    const u64 allocated_bytes = g_allocated_bytes;
    const auto begin = std::chrono::steady_clock::now();
    const double signature = body();
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration<double>(end - begin).count(), signature, g_allocations - allocations,
            g_allocated_bytes - allocated_bytes, state_bytes};
}

[[nodiscard]] Run ours_groups(const Series& series)
{
    return timed(sizeof(hn::PacketGroupTracker),
                 [&]
                 {
                     hn::PacketGroupTracker groups;
                     hn::GroupDeltas deltas;
                     double sum = 0.0;
                     for (const Packet& packet : series.packets)
                     {
                         if (groups.add(packet.send_us, packet.arrival_us, packet.system_us, packet.bytes, deltas))
                         {
                             sum += static_cast<double>(deltas.arrival_us - deltas.send_us);
                         }
                     }
                     return sum;
                 });
}

[[nodiscard]] Run their_groups(const Series& series)
{
    return timed(sizeof(webrtc::InterArrivalDelta),
                 [&]
                 {
                     webrtc::InterArrivalDelta groups(TimeDelta::Millis(5));
                     TimeDelta send = TimeDelta::Zero();
                     TimeDelta recv = TimeDelta::Zero();
                     int size = 0;
                     double sum = 0.0;
                     for (const Packet& packet : series.packets)
                     {
                         if (groups.ComputeDeltas(Timestamp::Micros(packet.send_us),
                                                  Timestamp::Micros(packet.arrival_us),
                                                  Timestamp::Micros(packet.system_us), packet.bytes, &send, &recv, &size))
                         {
                             sum += static_cast<double>(recv.us() - send.us());
                         }
                     }
                     return sum;
                 });
}

[[nodiscard]] Run ours_trendline(const Series& series)
{
    constexpr crd::usize kSamples = hn::TrendlineFilter::storage_samples(hn::TrendlineFilter::kDefaultWindow);
    return timed(sizeof(hn::TrendlineFilter) + (kSamples * sizeof(hn::TrendlineSample)),
                 [&]
                 {
                     hn::TrendlineSample storage[kSamples];
                     hn::TrendlineFilter filter;
                     if (!filter.init(storage))
                     {
                         std::abort();
                     }
                     for (const Completed& completed : series.completed)
                     {
                         if (!filter.update(completed.deltas, completed.arrival_us))
                         {
                             std::abort();
                         }
                     }
                     return filter.trend();
                 });
}

[[nodiscard]] Run their_trendline(const Series& series)
{
    return timed(sizeof(webrtc::TrendlineEstimator),
                 [&]
                 {
                     const NoTrials trials;
                     webrtc::TrendlineEstimator filter(trials, nullptr);
                     for (const Completed& completed : series.completed)
                     {
                         filter.Update(TimeDelta::Micros(completed.deltas.arrival_us).ms<double>(),
                                       TimeDelta::Micros(completed.deltas.send_us).ms<double>(), 0,
                                       Timestamp::Micros(completed.arrival_us).ms(), 0U, true);
                     }
                     return filter.prev_trend_;
                 });
}

[[nodiscard]] Run ours_kalman(const Series& series)
{
    return timed(sizeof(hn::KalmanDelayFilter) + sizeof(hn::DelayOveruseDetector),
                 [&]
                 {
                     hn::KalmanDelayFilter kalman;
                     hn::DelayOveruseDetector detector;
                     for (const Completed& completed : series.completed)
                     {
                         const double send_ms = TimeDelta::Micros(completed.deltas.send_us).ms<double>();
                         const i64 recv_ms = TimeDelta::Micros(completed.deltas.arrival_us).ms();
                         const i64 now_ms = completed.arrival_us / 1000;
                         if (!kalman.update(static_cast<double>(recv_ms), send_ms, completed.deltas.bytes,
                                            detector.state()))
                         {
                             std::abort();
                         }
                         (void)detector.update(kalman.offset(), kalman.deltas(), 1.0, send_ms, now_ms);
                     }
                     return kalman.offset();
                 });
}

[[nodiscard]] Run their_kalman(const Series& series)
{
    return timed(sizeof(webrtc::OveruseEstimator) + sizeof(webrtc::OveruseDetector),
                 [&]
                 {
                     webrtc::OveruseEstimator kalman;
                     webrtc::OveruseDetector detector;
                     for (const Completed& completed : series.completed)
                     {
                         const double send_ms = TimeDelta::Micros(completed.deltas.send_us).ms<double>();
                         const i64 recv_ms = TimeDelta::Micros(completed.deltas.arrival_us).ms();
                         const i64 now_ms = completed.arrival_us / 1000;
                         kalman.Update(recv_ms, send_ms, completed.deltas.bytes, detector.State(), now_ms);
                         (void)detector.Detect(kalman.offset(), send_ms, kalman.num_of_deltas(), now_ms);
                     }
                     return kalman.offset();
                 });
}

[[nodiscard]] Run ours_rate(const Series& series)
{
    return timed(sizeof(hn::AckedRateEstimator),
                 [&]
                 {
                     hn::AckedRateEstimator rate;
                     for (const Packet& packet : series.packets)
                     {
                         rate.update(packet.arrival_us / 1000, packet.bytes, false);
                     }
                     return static_cast<double>(rate.kbps());
                 });
}

[[nodiscard]] Run their_rate(const Series& series)
{
    return timed(sizeof(webrtc::BitrateEstimator),
                 [&]
                 {
                     const NoTrials trials;
                     webrtc::BitrateEstimator rate(&trials);
                     for (const Packet& packet : series.packets)
                     {
                         rate.Update(Timestamp::Millis(packet.arrival_us / 1000), DataSize::Bytes(packet.bytes), false);
                     }
                     return static_cast<double>(rate.bitrate_estimate_kbps_);
                 });
}

struct Arm
{
    const char* name;
    Run (*ours)(const Series&);
    Run (*theirs)(const Series&);
    bool per_packet;
};

constexpr Arm kArms[] = {
    {"packet groups", ours_groups, their_groups, true},
    {"trendline, window 20", ours_trendline, their_trendline, false},
    {"Kalman and detector", ours_kalman, their_kalman, false},
    {"acknowledged rate", ours_rate, their_rate, true},
};

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 15U;
    u32 packets = 400'000U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            packets = 20'000U;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-hesap-estimators-board [--rounds N] [--quick]\n");
            return 2;
        }
    }
    if (rounds == 0U)
    {
        std::fprintf(stderr, "crd-hesap-estimators-board: --rounds is at least 1\n");
        return 2;
    }

    Series series;
    Path path(estimator_paths::kProfiles[2], 42U); // capacity-steps: every state of the detector.
    hn::PacketGroupTracker groups;
    for (u32 i = 0U; i < packets; ++i)
    {
        const Packet packet = path.next();
        series.packets.push_back(packet);
        Completed completed;
        if (groups.add(packet.send_us, packet.arrival_us, packet.system_us, packet.bytes, completed.deltas))
        {
            completed.arrival_us = packet.arrival_us;
            series.completed.push_back(completed);
        }
    }

    std::printf("| Estimator | hesap ns/update | libwebrtc ns/update | libwebrtc / hesap | hesap bytes, heap | "
                "libwebrtc bytes, heap |\n|---|---|---|---|---|---|\n");
    for (const Arm& arm : kArms)
    {
        const double updates =
            static_cast<double>(arm.per_packet ? series.packets.size() : series.completed.size());
        double best_ours = 1e300;
        double best_theirs = 1e300;
        Run ours;
        Run theirs;
        for (u32 round = 0U; round < rounds; ++round)
        {
            ours = arm.ours(series);
            theirs = arm.theirs(series);
            best_ours = std::min(best_ours, ours.seconds);
            best_theirs = std::min(best_theirs, theirs.seconds);
            if (std::bit_cast<u64>(ours.signature) != std::bit_cast<u64>(theirs.signature))
            {
                std::fprintf(stderr, "crd-hesap-estimators-board: %s disagrees: %.17g vs %.17g\n", arm.name,
                             ours.signature, theirs.signature);
                return 1;
            }
        }
        std::printf("| %s | %.1f | %.1f | %.2f | %zu, %llu in %llu blocks | %zu, %llu in %llu blocks |\n", arm.name,
                    best_ours * 1e9 / updates, best_theirs * 1e9 / updates, best_theirs / best_ours, ours.state_bytes,
                    static_cast<unsigned long long>(ours.allocated_bytes),
                    static_cast<unsigned long long>(ours.allocations), theirs.state_bytes,
                    static_cast<unsigned long long>(theirs.allocated_bytes),
                    static_cast<unsigned long long>(theirs.allocations));
    }
    return 0;
}
