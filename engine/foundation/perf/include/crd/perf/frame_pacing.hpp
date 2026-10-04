#pragma once

// DIAG.6c(i): frame pacing / input-to-present analysis, kept SEPARATE from offline throughput and shader
// execution time (design/runtime-diagnostics.md: "Profile frame pacing/input-to-present separately from offline
// throughput and shader execution time").
//
// Three axes, deliberately distinct, all derived from timestamps that ALREADY exist (FrameRecord::frame_begin_ns
// / frame_end_ns) -- so this adds NO on-disk field and forces NO CPROF version bump (the on-disk layout is pinned
// by 6c(b)):
//   * PACING     -- the distribution of frame-to-frame INTERVALS (begin[i]-begin[i-1]); its tail (p99, max, stutter
//                   count) is what a player feels. NOT the same as throughput: two runs with the identical mean
//                   interval (identical throughput) can have wildly different pacing.
//   * WORK       -- each frame's own DURATION (end-begin). A frame can take 5 ms of work yet be paced at 16.7 ms
//                   (vsync idle), so duration is a separate accumulator from the interval, never mixed in.
//   * LATENCY    -- input-to-present (present_ns - input_ns). With a multi-frame pipeline this is several frame
//                   intervals long, so it is emphatically NOT the frame duration.
//
// This is a pure, bounded analyzer: fixed-bucket histograms (no sort, no allocation), integer nanoseconds
// throughout (locale-independent), and no dependency on the profiler being compiled in. GPU/shader execution time
// stays where it already lives (GPU scopes); this analyzer never reads scope durations, which is exactly how the
// "separately from ... shader execution time" clause is honoured.

#include <crd/core/types.hpp>
#include <crd/containers/span.hpp>   // ConstSpan (feeding from a CaptureView's frame history)
#include <crd/perf/frame_record.hpp> // FrameRecord (cheap POD: types + config + sample); no heavy profiler headers

namespace crd::perf
{

// A bounded frame-timing analyzer. Percentiles come from a linear fixed-bucket histogram over [0, kBuckets*width);
// intervals at or beyond the range land in a single overflow bucket and are reported at the observed max. Tail stats
// (percentile / stutter) assume >= 2 samples; with a single sample a percentile returns that one bucket's upper edge.
class FramePacingAnalyzer
{
public:
    static constexpr crd::u32 kBuckets = 256;

    // bucket_width_ns sizes the histogram resolution; the default 0.1 ms covers [0, 25.6 ms) which spans the usual
    // 60/144 Hz cadence with headroom. Values >= the range are still counted (overflow) and bound min/max/mean.
    explicit FramePacingAnalyzer(crd::i64 bucket_width_ns = 100000) noexcept
        : m_bucket_width_ns(bucket_width_ns > 0 ? bucket_width_ns : 1)
    {
        reset();
    }

    void reset() noexcept
    {
        m_prev_begin_ns = 0;
        m_have_prev     = false;

        m_interval_count = 0;
        m_interval_sum   = 0;
        m_interval_min   = 0;
        m_interval_max   = 0;
        for (crd::u32 i = 0; i <= kBuckets; ++i)
        {
            m_interval_hist[i] = 0;
        }

        m_dur_count = 0;
        m_dur_sum   = 0;

        m_lat_count = 0;
        m_lat_sum   = 0;
        m_lat_min   = 0;
        m_lat_max   = 0;
        for (crd::u32 i = 0; i <= kBuckets; ++i)
        {
            m_lat_hist[i] = 0;
        }
    }

    // Feed one frame boundary. The interval (pacing) is begin_ns - previous begin_ns; the duration (work) is
    // end_ns - begin_ns. The first frame contributes a duration but no interval (there is no predecessor).
    void add_frame(crd::i64 begin_ns, crd::i64 end_ns) noexcept
    {
        if (m_have_prev)
        {
            add_interval(begin_ns - m_prev_begin_ns);
        }
        m_prev_begin_ns = begin_ns;
        m_have_prev     = true;

        crd::i64 dur = end_ns - begin_ns; // a truncated capture can hand us end < begin -> clamp like interval/latency
        if (dur < 0)
        {
            dur = 0;
        }
        m_dur_sum += dur;
        ++m_dur_count;
    }

    // Feed a frame-to-frame interval directly (pacing only; use when the duration is unknown).
    void add_interval(crd::i64 interval_ns) noexcept
    {
        if (interval_ns < 0)
        {
            interval_ns = 0;
        }
        if (m_interval_count == 0 || interval_ns < m_interval_min)
        {
            m_interval_min = interval_ns;
        }
        if (m_interval_count == 0 || interval_ns > m_interval_max)
        {
            m_interval_max = interval_ns;
        }
        m_interval_sum += interval_ns;
        ++m_interval_count;
        ++m_interval_hist[bucket_of(interval_ns)];
    }

    // Feed one input-to-present measurement. Latency is present_ns - input_ns and is tracked independently of any
    // frame interval or duration.
    void add_latency(crd::i64 input_ns, crd::i64 present_ns) noexcept
    {
        crd::i64 latency_ns = present_ns - input_ns;
        if (latency_ns < 0)
        {
            latency_ns = 0;
        }
        if (m_lat_count == 0 || latency_ns < m_lat_min)
        {
            m_lat_min = latency_ns;
        }
        if (m_lat_count == 0 || latency_ns > m_lat_max)
        {
            m_lat_max = latency_ns;
        }
        m_lat_sum += latency_ns;
        ++m_lat_count;
        ++m_lat_hist[bucket_of(latency_ns)];
    }

    // --- pacing axis (frame delivery timing) -------------------------------------------------------------------
    [[nodiscard]] crd::u64 interval_count() const noexcept { return m_interval_count; }
    // Mean interval == the reciprocal of throughput; kept here so a caller can show throughput WITHOUT it standing
    // in for pacing. It is the average, not the tail.
    [[nodiscard]] crd::i64 mean_interval_ns() const noexcept
    {
        return m_interval_count ? m_interval_sum / static_cast<crd::i64>(m_interval_count) : 0;
    }
    [[nodiscard]] crd::i64 min_interval_ns() const noexcept { return m_interval_count ? m_interval_min : 0; }
    [[nodiscard]] crd::i64 max_interval_ns() const noexcept { return m_interval_count ? m_interval_max : 0; }
    // The pacing tail: the pct-th percentile interval. This is what differs between smooth and stuttery runs of
    // equal throughput.
    [[nodiscard]] crd::i64 percentile_interval_ns(crd::u32 pct) const noexcept
    {
        return percentile(m_interval_hist, m_interval_count, m_interval_max, pct);
    }
    // Frames whose interval is at/over threshold_ns (bucket-quantized). Never derived from throughput or work.
    [[nodiscard]] crd::u64 stutter_count(crd::i64 threshold_ns) const noexcept
    {
        if (threshold_ns <= 0)
        {
            return m_interval_count;
        }
        crd::u64 first = static_cast<crd::u64>(threshold_ns / m_bucket_width_ns);
        crd::u64 c     = m_interval_hist[kBuckets]; // overflow is always over-threshold
        for (crd::u32 b = static_cast<crd::u32>(first < kBuckets ? first : kBuckets); b < kBuckets; ++b)
        {
            c += m_interval_hist[b];
        }
        return c;
    }

    // --- work axis (per-frame CPU duration) -- separate from pacing --------------------------------------------
    [[nodiscard]] crd::u64 frame_count() const noexcept { return m_dur_count; }
    [[nodiscard]] crd::i64 mean_duration_ns() const noexcept
    {
        return m_dur_count ? m_dur_sum / static_cast<crd::i64>(m_dur_count) : 0;
    }

    // --- input-to-present latency axis -- separate from frame duration -----------------------------------------
    [[nodiscard]] crd::u64 latency_count() const noexcept { return m_lat_count; }
    [[nodiscard]] crd::i64 mean_latency_ns() const noexcept
    {
        return m_lat_count ? m_lat_sum / static_cast<crd::i64>(m_lat_count) : 0;
    }
    [[nodiscard]] crd::i64 max_latency_ns() const noexcept { return m_lat_count ? m_lat_max : 0; }
    [[nodiscard]] crd::i64 percentile_latency_ns(crd::u32 pct) const noexcept
    {
        return percentile(m_lat_hist, m_lat_count, m_lat_max, pct);
    }

private:
    [[nodiscard]] crd::u32 bucket_of(crd::i64 value_ns) const noexcept
    {
        crd::i64 idx = value_ns / m_bucket_width_ns;
        if (idx < 0)
        {
            idx = 0;
        }
        if (idx >= static_cast<crd::i64>(kBuckets)) // overflow bucket
        {
            return kBuckets;
        }
        return static_cast<crd::u32>(idx);
    }

    [[nodiscard]] crd::i64 percentile(const crd::u32 (&hist)[kBuckets + 1], crd::u64 count, crd::i64 observed_max,
                                      crd::u32 pct) const noexcept
    {
        if (count == 0)
        {
            return 0;
        }
        if (pct > 100)
        {
            pct = 100;
        }
        crd::u64 rank = (static_cast<crd::u64>(pct) * count + 99) / 100; // ceil(pct% of count)
        if (rank == 0)
        {
            rank = 1;
        }
        crd::u64 cum = 0;
        for (crd::u32 b = 0; b < kBuckets; ++b)
        {
            cum += hist[b];
            if (cum >= rank) // conservative upper edge
            {
                return static_cast<crd::i64>(b + 1) * m_bucket_width_ns;
            }
        }
        return observed_max; // rank fell in the overflow bucket -> report the observed maximum
    }

    crd::i64 m_bucket_width_ns;

    crd::i64 m_prev_begin_ns;
    bool     m_have_prev;

    // pacing (interval) accumulators
    crd::u64 m_interval_count;
    crd::i64 m_interval_sum;
    crd::i64 m_interval_min;
    crd::i64 m_interval_max;
    crd::u32 m_interval_hist[kBuckets + 1];

    // work (duration) accumulators -- mean only; the point is that it is SEPARATE from pacing
    crd::u64 m_dur_count;
    crd::i64 m_dur_sum;

    // latency (input-to-present) accumulators
    crd::u64 m_lat_count;
    crd::i64 m_lat_sum;
    crd::i64 m_lat_min;
    crd::i64 m_lat_max;
    crd::u32 m_lat_hist[kBuckets + 1];
};

// Feed a run of consecutive per-frame records (e.g. CaptureView::frame_records()) into the analyzer: pacing from
// consecutive frame_begin_ns, work from end-begin. Input-to-present is NOT stored on-disk (see the header note), so
// a caller holding input timestamps supplies it separately via add_latency.
inline void feed_frames(FramePacingAnalyzer& a, crd::containers::ConstSpan<FrameRecord> frames) noexcept
{
    for (crd::usize i = 0; i < frames.size(); ++i)
    {
        a.add_frame(frames[i].frame_begin_ns, frames[i].frame_end_ns);
    }
}

} // namespace crd::perf
