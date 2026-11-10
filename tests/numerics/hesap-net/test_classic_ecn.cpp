// HESAP-NET's classic ECN AQM detector (TR-BB-2019-002 version 03, sections 3.2 to 3.4) against restated formulas:
// the fixed-point log against the real one, the RTT averages' gains by hand, a quiescent detector woken by CE, a
// classic sawtooth queue driving the score to its classic end, an L4S queue's jitter keeping it scalable, a deep
// queue that varies little left scalable (depth only adds where variability allows), a self-limited sender pulled
// toward scalable, and idle timeouts halving a classic score.

#include <crd/hesap/net/classic_ecn.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace hn = crd::hesap::net;
using crd::i64;
using crd::u32;
using crd::u64;
using hn::ClassicEcnDetector;

namespace
{
constexpr u64 kMs = 1000000U;
constexpr u64 kUs = 1000U;

// A round of `samples` RTT samples of the base round trip plus a queue rising linearly from `low` to `high`.
void round(ClassicEcnDetector& detector, u64 base_ns, u64 low_ns, u64 high_ns, u64 samples, u64 segments,
           bool ce = true, u32 self_limited = 0U)
{
    for (u64 i = 0U; i < samples; ++i)
    {
        detector.on_rtt(base_ns + low_ns + (high_ns - low_ns) * i / samples, segments);
    }
    if (ce)
    {
        detector.on_ce();
    }
    detector.on_round(base_ns, self_limited);
}
} // namespace

TEST_CASE("classic ECN: the fixed-point log is the real one rounded down", "[hesap][net][classic-ecn]")
{
    CHECK(hn::log2_q16(0U) == 0);
    CHECK(hn::log2_q16(1U) == 0);
    for (u32 k = 1U; k < 64U; ++k)
    {
        CHECK(hn::log2_q16(u64{1} << k) == i64{k} << 16U);
    }
    u64 x = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 2000; ++i)
    {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        const u64 v = x >> (static_cast<u32>(i) % 63U);
        if (v < 2U)
        {
            continue;
        }
        const double real = std::log2(static_cast<double>(v)) * 65536.0;
        const i64 got = hn::log2_q16(v);
        // Doubles hold the log to about 2^-40 of a unit here: off by one only where the real value sits on a step.
        CHECK(static_cast<double>(got) <= real + 1e-6);
        CHECK(static_cast<double>(got) > real - 1.0 - 1e-6);
    }
    CHECK(hn::log2_q16(750000U) == 1279035); // floor(lg(750000) * 2^16): V0 in nanoseconds
}

TEST_CASE("classic ECN: the averages move by 1 / (2 S^1.5) and the deviation by half that", "[hesap][net][classic-ecn]")
{
    ClassicEcnDetector detector;
    detector.on_rtt(10U * kMs, 1U);
    CHECK(detector.srtt_ns() == 10U * kMs);
    CHECK(detector.mdev_ns() == 1U * kUs);
    // S = 1: gain 2; the deviation's gain 4.
    detector.on_rtt(20U * kMs, 1U);
    CHECK(detector.srtt_ns() == 15U * kMs);
    CHECK(detector.mdev_ns() == kUs + (10U * kMs - kUs) / 4U);
    // S = 100: gain 2 * 1000 = 2000.
    ClassicEcnDetector slow;
    slow.on_rtt(10U * kMs, 100U);
    slow.on_rtt(30U * kMs, 100U);
    CHECK(slow.srtt_ns() == 10U * kMs + 20U * kMs / 2000U);
    // Samples past 2^40 ns count as 2^40.
    ClassicEcnDetector huge;
    huge.on_rtt(~u64{0}, 1U);
    CHECK(huge.srtt_ns() == ClassicEcnDetector::kMaxSampleNs);
}

TEST_CASE("classic ECN: quiescent until CE, then a classic sawtooth queue turns it classic",
          "[hesap][net][classic-ecn]")
{
    ClassicEcnDetector detector;
    CHECK(detector.score() == -ClassicEcnDetector::kSticky);
    CHECK(detector.classic() == 0U);
    // Rounds without CE change nothing, whatever the queue.
    for (int i = 0; i < 50; ++i)
    {
        round(detector, 20U * kMs, 0U, 20U * kMs, 40U, 20U, false);
    }
    CHECK(detector.score() == -ClassicEcnDetector::kSticky);
    // A classic AQM's queue (a 0 to 20 ms sawtooth, about 5 ms of mean deviation) with marks: the score climbs to
    // C_STICKY and stays.
    int rounds = 0;
    while (detector.classic() < u32{1} << 16U && rounds < 400)
    {
        round(detector, 20U * kMs, 0U, 20U * kMs, 40U, 20U);
        ++rounds;
    }
    INFO("rounds " << rounds << " mdev " << detector.mdev_ns() << " srtt " << detector.srtt_ns());
    CHECK(rounds < 400);
    for (int i = 0; i < 50; ++i)
    {
        round(detector, 20U * kMs, 0U, 20U * kMs, 40U, 20U);
    }
    CHECK(detector.score() == ClassicEcnDetector::kSticky);
    CHECK(detector.classic() == u32{1} << 16U);
    // The queue turns into an L4S one (under 0.3 ms of jitter): back to scalable, and quiescent.
    for (int i = 0; i < 400; ++i)
    {
        round(detector, 20U * kMs, 0U, 300U * kUs, 40U, 20U);
    }
    CHECK(detector.classic() == 0U);
    CHECK(detector.score() == -ClassicEcnDetector::kSticky);
}

TEST_CASE("classic ECN: an L4S queue, a deep steady queue and a self-limited sender stay scalable",
          "[hesap][net][classic-ecn]")
{
    // An L4S AQM's queue under its 1 ms step, with marks every round.
    ClassicEcnDetector l4s;
    for (int i = 0; i < 400; ++i)
    {
        round(l4s, 20U * kMs, 0U, 900U * kUs, 40U, 20U);
        CHECK(l4s.classic() == 0U);
    }
    // A 10 ms standing queue varying by 100 us: depth adds 0.5 lg(5) a round, variability takes 0.5 lg(7.5).
    ClassicEcnDetector deep;
    for (int i = 0; i < 400; ++i)
    {
        round(deep, 20U * kMs, 9950U * kUs, 10050U * kUs, 40U, 20U);
    }
    CHECK(deep.classic() == 0U);
    // A queue varying by about 1 ms with marks: variability alone moves the score up by 0.5 lg(1 / 0.75) a round, but
    // a sender self-limited all the time takes 0.25 a round away; half the time, it still turns classic.
    ClassicEcnDetector limited;
    ClassicEcnDetector half;
    for (int i = 0; i < 600; ++i)
    {
        round(limited, 20U * kMs, 0U, 4U * kMs, 40U, 20U, true, u32{1} << 16U);
        round(half, 20U * kMs, 0U, 4U * kMs, 40U, 20U, true, u32{1} << 15U);
    }
    INFO("mdev " << limited.mdev_ns());
    CHECK(limited.classic() == 0U);
    CHECK(half.classic() == u32{1} << 16U);
}

TEST_CASE("classic ECN: each idle timeout halves a classic score, a scalable one is kept", "[hesap][net][classic-ecn]")
{
    ClassicEcnDetector detector;
    for (int i = 0; i < 400; ++i)
    {
        round(detector, 20U * kMs, 0U, 20U * kMs, 40U, 20U);
    }
    REQUIRE(detector.score() == ClassicEcnDetector::kSticky);
    detector.on_idle(1U);
    CHECK(detector.score() == ClassicEcnDetector::kSticky / 2);
    detector.on_idle(3U);
    CHECK(detector.score() == ClassicEcnDetector::kSticky / 16);
    detector.on_idle(~u64{0}); // bounded: at most 64 halvings
    CHECK(detector.score() == 0);
    ClassicEcnDetector scalable;
    scalable.on_ce();
    const i64 woken = scalable.score();
    CHECK(woken == -ClassicEcnDetector::kSticky + 1);
    scalable.on_idle(5U);
    CHECK(scalable.score() == woken);
}
