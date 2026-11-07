// HESAP-NET's budget board for the clock filters: the cost per update against ntpd's clock_filter() and libwebrtc's
// RtpToNtpEstimator and TimestampAligner on the same inputs (each pair is bit-exact: its oracle tests hold that, and
// the signatures here check it again), and the error on the simulated paths of the raw exchange, of ntpd's filter on
// its own and of the tracker. ntpd disciplines the host clock behind its filter; on free-running clocks the filter
// alone lags the frequency, and that lag is what the tracker's line and drift compensation remove.

#include "clock_paths.hpp"
#include "ntpd_peer.h"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/clock.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <new>
#include <optional>

#define private public // NOLINT(readability-identifier-naming)
#include "rtc_base/rtp_to_ntp_estimator.h"
#undef private
#include "rtc_base/timestamp_aligner.h"

namespace
{
crd::u64 g_allocations = 0U;
crd::u64 g_allocated_bytes = 0U;
} // namespace

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
using crd::i64;
using crd::u32;
using crd::u64;

namespace
{

constexpr i64 kNsPerSecond = 1'000'000'000;

struct FilterInput
{
    u64 now = 0U;
    double offset = 0.0;
    double delay = 0.0;
    double dispersion = 0.0;
};

struct Report
{
    u64 ntp = 0U;
    u32 rtp = 0U;
    i64 capturer = 0;
    i64 system = 0;
};

struct Series
{
    crd::containers::Array<FilterInput> filter;
    crd::containers::Array<Report> reports;
    crd::containers::Array<clock_paths::Exchange> exchanges;
    i64 poll_ns = 0;
};

struct Run
{
    double seconds = 0.0;
    u64 signature = 0U;
    u64 allocations = 0U;
    u64 allocated_bytes = 0U;
    crd::usize state_bytes = 0U;
};

template <class Body> [[nodiscard]] Run timed(crd::usize state_bytes, const Body& body)
{
    Run run;
    run.state_bytes = state_bytes;
    const u64 allocations = g_allocations;
    const u64 bytes = g_allocated_bytes;
    const auto start = std::chrono::steady_clock::now();
    run.signature = body();
    const auto end = std::chrono::steady_clock::now();
    run.seconds = std::chrono::duration<double>(end - start).count();
    run.allocations = g_allocations - allocations;
    run.allocated_bytes = g_allocated_bytes - bytes;
    return run;
}

[[nodiscard]] Run ours_filter(const Series& series)
{
    return timed(sizeof(hn::ClockFilter),
                 [&]
                 {
                     hn::ClockFilter filter;
                     if (!filter.init({}, static_cast<i64>(series.filter[0].now) * kNsPerSecond))
                     {
                         std::abort();
                     }
                     for (const FilterInput& input : series.filter)
                     {
                         (void)filter.update(static_cast<i64>(input.now) * kNsPerSecond, input.offset, input.delay,
                                             input.dispersion, 64 * kNsPerSecond);
                     }
                     return std::bit_cast<u64>(filter.jitter_s()) ^ std::bit_cast<u64>(filter.dispersion_s());
                 });
}

[[nodiscard]] Run their_filter(const Series& series)
{
    return timed(sizeof(crd_ntpd_peer),
                 [&]
                 {
                     crd_ntpd_peer peer;
                     crd_ntpd_clear(&peer, series.filter[0].now, -20);
                     peer.hpoll = 6U;
                     for (const FilterInput& input : series.filter)
                     {
                         (void)crd_ntpd_filter(&peer, input.now, input.offset, input.delay, input.dispersion, 15e-6,
                                               1.5, 11U, -20);
                     }
                     return std::bit_cast<u64>(peer.jitter) ^ std::bit_cast<u64>(peer.disp);
                 });
}

[[nodiscard]] Run ours_media(const Series& series)
{
    return timed(sizeof(hn::MediaClockMap),
                 [&]
                 {
                     hn::MediaClockMap map;
                     for (const Report& report : series.reports)
                     {
                         (void)map.update(report.ntp, report.rtp);
                     }
                     return std::bit_cast<u64>(map.slope()) ^ std::bit_cast<u64>(map.offset());
                 });
}

[[nodiscard]] Run their_media(const Series& series)
{
    return timed(sizeof(webrtc::RtpToNtpEstimator),
                 [&]
                 {
                     webrtc::RtpToNtpEstimator estimator;
                     for (const Report& report : series.reports)
                     {
                         (void)estimator.UpdateMeasurements(webrtc::NtpTime(report.ntp), report.rtp);
                     }
                     if (!estimator.params_.has_value())
                     {
                         std::abort();
                     }
                     return std::bit_cast<u64>(estimator.params_->slope) ^
                            std::bit_cast<u64>(estimator.params_->offset);
                 });
}

[[nodiscard]] Run ours_capture(const Series& series)
{
    return timed(sizeof(hn::CaptureClockAligner),
                 [&]
                 {
                     hn::CaptureClockAligner aligner;
                     u64 sum = 0U;
                     for (const Report& report : series.reports)
                     {
                         i64 out = 0;
                         (void)aligner.translate(report.capturer, report.system, out);
                         sum += static_cast<u64>(out);
                     }
                     return sum;
                 });
}

[[nodiscard]] Run their_capture(const Series& series)
{
    return timed(sizeof(webrtc::TimestampAligner),
                 [&]
                 {
                     webrtc::TimestampAligner aligner;
                     u64 sum = 0U;
                     for (const Report& report : series.reports)
                     {
                         sum += static_cast<u64>(aligner.TranslateTimestamp(report.capturer, report.system));
                     }
                     return sum;
                 });
}

[[nodiscard]] Run ours_tracker(const Series& series)
{
    return timed(sizeof(hn::ClockTracker),
                 [&]
                 {
                     hn::ClockTracker tracker;
                     if (!tracker.init({}))
                     {
                         std::abort();
                     }
                     for (const clock_paths::Exchange& exchange : series.exchanges)
                     {
                         (void)tracker.on_exchange(exchange.t1, exchange.t2, exchange.t3, exchange.t4, series.poll_ns);
                     }
                     return static_cast<u64>(tracker.offset_ns_at(series.exchanges[0].t4));
                 });
}

[[nodiscard]] double seconds_of(i64 ns) noexcept
{
    return static_cast<double>(ns) * 1e-9;
}

struct Quantiles
{
    double median = 0.0;
    double p99 = 0.0;
};

[[nodiscard]] Quantiles quantiles(crd::containers::Array<double>& values)
{
    std::sort(values.begin(), values.end());
    return {values[values.size() / 2U], values[(values.size() * 99U) / 100U]};
}

// The error at T4 of each estimate past the first 200 polls and an event's 120 polls of recovery.
void accuracy(const clock_paths::Profile& profile, u32 polls)
{
    clock_paths::Path path(profile, 7U, 987'654'321);
    hn::ClockTracker tracker;
    if (!tracker.init({}))
    {
        std::abort();
    }
    crd_ntpd_peer peer;
    crd_ntpd_clear(&peer, 1'000'000U, -20);
    peer.hpoll = 6U;
    u64 filter_now = 1'000'000U;
    double filter_offset = 0.0;
    crd::containers::Array<double> raw;
    crd::containers::Array<double> filtered;
    crd::containers::Array<double> tracked;
    u32 violations = 0U;
    double frequency_error = 0.0;
    double final_bound = 0.0;
    for (u32 poll = 1U; poll <= polls; ++poll)
    {
        const clock_paths::Exchange exchange = path.next();
        filter_now += 1U + (static_cast<u64>(path.poll_ns()) / static_cast<u64>(kNsPerSecond));
        hn::OnWireSample wire{};
        const bool usable =
            !exchange.lost && hn::on_wire_sample(exchange.t1, exchange.t2, exchange.t3, exchange.t4, 1, wire);
        if (usable)
        {
            (void)tracker.on_exchange(exchange.t1, exchange.t2, exchange.t3, exchange.t4, path.poll_ns());
            const crd_ntpd_outcome outcome = crd_ntpd_filter(&peer, filter_now, seconds_of(wire.offset_ns),
                                                             seconds_of(wire.delay_ns), 2e-6, 15e-6, 1.5, 11U, -20);
            if (outcome == CRD_NTPD_FRESH)
            {
                filter_offset = peer.offset;
            }
        }
        else
        {
            (void)tracker.on_lost(exchange.t4, path.poll_ns());
            (void)crd_ntpd_filter(&peer, filter_now, 0.0, 0.0, 16.0, 15e-6, 1.5, 11U, -20);
        }
        const bool recovering =
            profile.event_poll != 0U && poll >= profile.event_poll && poll < profile.event_poll + 120U;
        if (poll < 200U || recovering)
        {
            continue;
        }
        const double error = seconds_of(tracker.offset_ns_at(exchange.t4) - exchange.true_offset_ns);
        tracked.push_back(std::fabs(error));
        violations += (std::fabs(error) > tracker.offset_bound_s_at(exchange.t4)) ? 1U : 0U;
        filtered.push_back(std::fabs(filter_offset - seconds_of(exchange.true_offset_ns)));
        if (usable)
        {
            raw.push_back(std::fabs(seconds_of(wire.offset_ns - exchange.true_offset_ns)));
        }
        frequency_error = tracker.frequency() - exchange.true_frequency;
        final_bound = tracker.offset_bound_s_at(exchange.t4);
    }
    const Quantiles raw_q = quantiles(raw);
    const Quantiles filtered_q = quantiles(filtered);
    const Quantiles tracked_q = quantiles(tracked);
    std::printf("| %s | %.1f, %.1f | %.1f, %.1f | %.1f, %.1f | %u | %.2f | %.1f |\n", profile.name, raw_q.median * 1e6,
                raw_q.p99 * 1e6, filtered_q.median * 1e6, filtered_q.p99 * 1e6, tracked_q.median * 1e6,
                tracked_q.p99 * 1e6, violations, std::fabs(frequency_error) * 1e6, final_bound * 1e6);
}

struct Arm
{
    const char* name;
    Run (*ours)(const Series&);
    Run (*theirs)(const Series&);
};

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 15U;
    u32 updates = 400'000U;
    u32 polls = 4000U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            updates = 20'000U;
            polls = 2000U;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-hesap-clock-board [--rounds N] [--quick]\n");
            return 2;
        }
    }
    if (rounds == 0U)
    {
        std::fprintf(stderr, "crd-hesap-clock-board: --rounds is at least 1\n");
        return 2;
    }

    Series series;
    SeededWords words(0xB0A2DU, 1U);
    const auto uniform = [&words]
    {
        return static_cast<double>(words.next_u32()) / 4294967296.0;
    };
    u64 now = 1'000'000U;
    double sender_ms = 1e9;
    u32 rtp = 7U;
    i64 capturer = 5'000'000;
    i64 system = 1'000'000'000;
    for (u32 i = 0U; i < updates; ++i)
    {
        now += 1U + (words.next_u32() % 64U);
        series.filter.push_back({now, (uniform() - 0.5) * 1e-3, 0.01 + (uniform() * 0.02), uniform() * 1e-6});
        const double step_ms = 500.0 + (uniform() * 1000.0);
        sender_ms += step_ms;
        rtp += static_cast<u32>(step_ms * 90.0) + (words.next_u32() % 40U);
        capturer += 33'333 + (words.next_u32() % 200U);
        system += 33'333 + (words.next_u32() % 3000U) - 1500;
        series.reports.push_back({static_cast<u64>(sender_ms * 4294967.296), rtp, capturer, system});
    }
    clock_paths::Path path(clock_paths::kProfiles[1], 42U);
    series.poll_ns = path.poll_ns();
    for (u32 i = 0U; i < updates; ++i)
    {
        series.exchanges.push_back(path.next());
    }

    std::printf("| Filter | hesap ns/update | peer ns/update | peer / hesap | hesap bytes, heap | peer bytes, heap |\n"
                "|---|---|---|---|---|---|\n");
    const Arm arms[] = {
        {"RFC 5905 clock filter / ntpd", ours_filter, their_filter},
        {"RTP to NTP line / RtpToNtpEstimator", ours_media, their_media},
        {"capture aligner / TimestampAligner", ours_capture, their_capture},
    };
    for (const Arm& arm : arms)
    {
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
            if (ours.signature != theirs.signature)
            {
                std::fprintf(stderr, "crd-hesap-clock-board: %s disagrees\n", arm.name);
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
    double best_tracker = 1e300;
    Run tracker;
    for (u32 round = 0U; round < rounds; ++round)
    {
        tracker = ours_tracker(series);
        best_tracker = std::min(best_tracker, tracker.seconds);
    }
    std::printf("| ClockTracker, whole exchange | %.1f | no peer | | %zu, %llu in %llu blocks | |\n\n",
                best_tracker * 1e9 / updates, tracker.state_bytes,
                static_cast<unsigned long long>(tracker.allocated_bytes),
                static_cast<unsigned long long>(tracker.allocations));

    std::printf(
        "Errors in microseconds (median, p99), with the bound's violations, the final frequency error in PPM and "
        "the final bound.\n\n"
        "| Path | raw exchange | ntpd filter alone | tracker | bound violations | frequency error | bound |\n"
        "|---|---|---|---|---|---|---|\n");
    for (const clock_paths::Profile& profile : clock_paths::kProfiles)
    {
        accuracy(profile, polls);
    }
    return 0;
}
