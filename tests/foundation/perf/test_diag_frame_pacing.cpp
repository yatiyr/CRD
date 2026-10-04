// DIAG.6c(i): frame pacing / input-to-present analysis, proven SEPARATE from throughput and shader/work time.
//
// The acceptance point of clause (i) is the word "separately": pacing, throughput and work/shader time must be
// distinct measurements, not one derived from another. These cases pin that with synthetic timestamps (no GPU
// needed): equal throughput but different pacing; equal pacing but different work; and input-to-present that is not
// the frame duration. crd:: containers only; integer nanoseconds throughout.

#include <catch2/catch_test_macros.hpp>

#include <crd/perf/frame_pacing.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace
{
constexpr crd::i64 MS = 1000000; // 1 ms in ns
}

TEST_CASE("pacing is not throughput: equal mean interval, different tail", "[perf][diag][pacing]")
{
    crd::perf::FramePacingAnalyzer steady;
    crd::perf::FramePacingAnalyzer alt;

    // Identical throughput (mean interval 16 ms) -- steady 16/16/16..., alternating 8/24/8/24...
    for (int i = 0; i < 100; ++i)
    {
        steady.add_interval(16 * MS);
    }
    for (int i = 0; i < 100; ++i)
    {
        alt.add_interval((i % 2 == 0) ? 8 * MS : 24 * MS);
    }

    REQUIRE(steady.mean_interval_ns() == alt.mean_interval_ns()); // same throughput...
    REQUIRE(steady.mean_interval_ns() == 16 * MS);

    // ...but the pacing tail is sharply different. If p99 were the mean in disguise this REQUIRE would fail --
    // this case IS the tooth for "pacing != throughput".
    REQUIRE(alt.percentile_interval_ns(99) >= steady.percentile_interval_ns(99) + 5 * MS);
    REQUIRE(steady.max_interval_ns() == 16 * MS);
    REQUIRE(alt.max_interval_ns() == 24 * MS);

    // Stutter (intervals >= 20 ms) is a pacing property, never derivable from the equal throughput.
    REQUIRE(steady.stutter_count(20 * MS) == 0u);
    REQUIRE(alt.stutter_count(20 * MS) == 50u);
}

TEST_CASE("pacing is not shader/work time: equal pacing, different frame duration", "[perf][diag][pacing]")
{
    crd::perf::FramePacingAnalyzer light;
    crd::perf::FramePacingAnalyzer heavy;

    // Identical begin cadence (16 ms) -> identical pacing; different per-frame work (4 ms vs 12 ms).
    for (int i = 0; i < 50; ++i)
    {
        light.add_frame(static_cast<crd::i64>(i) * 16 * MS, static_cast<crd::i64>(i) * 16 * MS + 4 * MS);
    }
    for (int i = 0; i < 50; ++i)
    {
        heavy.add_frame(static_cast<crd::i64>(i) * 16 * MS, static_cast<crd::i64>(i) * 16 * MS + 12 * MS);
    }

    // Pacing is identical -- proving it is computed from frame boundaries, not from the work/shader time.
    REQUIRE(light.mean_interval_ns() == heavy.mean_interval_ns());
    REQUIRE(light.max_interval_ns() == heavy.max_interval_ns());
    REQUIRE(light.percentile_interval_ns(99) == heavy.percentile_interval_ns(99));

    // Work differs and is tracked on its own axis.
    REQUIRE(light.mean_duration_ns() == 4 * MS);
    REQUIRE(heavy.mean_duration_ns() == 12 * MS);
    REQUIRE(light.mean_duration_ns() != heavy.mean_duration_ns());
}

TEST_CASE("input-to-present is not the frame duration", "[perf][diag][pacing]")
{
    crd::perf::FramePacingAnalyzer a;

    // 16 ms frames...
    for (int i = 0; i < 10; ++i)
    {
        a.add_frame(static_cast<crd::i64>(i) * 16 * MS, static_cast<crd::i64>(i) * 16 * MS + 16 * MS);
    }
    // ...but a 3-frame-deep pipeline means present lands 48 ms after the input that produced it.
    for (int i = 0; i < 10; ++i)
    {
        a.add_latency(static_cast<crd::i64>(i) * 16 * MS, static_cast<crd::i64>(i) * 16 * MS + 48 * MS);
    }

    REQUIRE(a.mean_duration_ns() == 16 * MS);
    REQUIRE(a.mean_latency_ns() == 48 * MS);
    REQUIRE(a.mean_latency_ns() != a.mean_duration_ns()); // latency is a separate axis, not the frame's own duration
    REQUIRE(a.max_latency_ns() == 48 * MS);
    REQUIRE(a.percentile_latency_ns(99) >= 48 * MS);
}

TEST_CASE("feed_frames derives pacing + work from a FrameRecord run", "[perf][diag][pacing]")
{
    crd::perf::FrameRecord recs[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        recs[i].frame_begin_ns = static_cast<crd::i64>(i) * 16 * MS;
        recs[i].frame_end_ns   = static_cast<crd::i64>(i) * 16 * MS + 5 * MS;
    }

    crd::perf::FramePacingAnalyzer a;
    crd::perf::feed_frames(a, crd::containers::ConstSpan<crd::perf::FrameRecord>(recs, 4));

    REQUIRE(a.frame_count() == 4u);       // four durations
    REQUIRE(a.interval_count() == 3u);    // three intervals (first frame has no predecessor)
    REQUIRE(a.mean_interval_ns() == 16 * MS);
    REQUIRE(a.mean_duration_ns() == 5 * MS);
}
