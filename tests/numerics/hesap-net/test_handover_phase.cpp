// crd-hesap-net -- the handover phase learner (HESAP-NET; design S76): its bounds, a LEO link's timetable learned on
// any clock and tracked through a skewed one, the background a busy or congested path adds refused, an aliased or
// one-off burst refused, the lock released once the handovers stop, any time sequence, and the binomial tail.

#include <crd/hesap/net/handover_phase.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::u32;
using crd::u64;
namespace hn = crd::hesap::net;
using Learner = hn::HandoverPhaseLearner;

constexpr u64 kMs = 1000000U;
constexpr u64 kSecond = 1000U * kMs;
constexpr u64 kPeriod = 15U * kSecond;

// A deterministic stream of 64-bit words (SplitMix64).
struct Words
{
    u64 state = 0x9E3779B97F4A7C15U;

    u64 next()
    {
        u64 z = (state += 0x9E3779B97F4A7C15U);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9U;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBU;
        return z ^ (z >> 31U);
    }
};

// A path seen by a sender: an RTT sample every `step` from `origin` on its clock, 40 ms with up to 1 ms of jitter;
// handovers every `period` from `first` (0: none) losing what is sent in the first `outage` and adding `spike` for
// `spike_for`; `background` random losses a second.
struct LeoPath
{
    u64 origin = 0U;
    u64 first = 0U;
    u64 period = kPeriod;
    u64 outage = 60U * kMs;
    u64 spike = 30U * kMs;
    u64 spike_for = 150U * kMs;
    u64 step = 10U * kMs;
    u64 background = 0U;
    u64 handovers_until = ~u64{0}; // no handover from then
    Words words;

    // The time since the latest handover at `t`, or ~0 before the first.
    [[nodiscard]] u64 since(u64 t) const
    {
        if (first == 0U || t < first || t >= handovers_until)
        {
            return ~u64{0};
        }
        return (t - first) % period;
    }

    // Feeds the sends of [from, to) to `learner`.
    void run(Learner& learner, u64 from, u64 to)
    {
        for (u64 t = from; t < to; t += step)
        {
            const u64 s = since(t);
            const bool random_loss = background != 0U && words.next() % (kSecond / step) < background;
            if (s < outage || random_loss)
            {
                learner.on_loss(t);
                continue;
            }
            const u64 jitter = words.next() % kMs;
            learner.on_rtt(t, 40U * kMs + jitter + ((s < spike_for) ? spike : 0U));
        }
    }
};

// How far `phase` is from `truth` around a period.
u64 distance(u64 phase, u64 truth, u64 period)
{
    const u64 d = (phase + period - truth % period) % period;
    return (d < period - d) ? d : period - d;
}

double tail_reference(u32 n, u32 h, double q)
{
    double sum = 0.0;
    for (u32 x = h; x <= n; ++x)
    {
        double choose = 1.0;
        for (u32 k = 1U; k <= x; ++k)
        {
            choose = choose * static_cast<double>(n - k + 1U) / static_cast<double>(k);
        }
        sum += choose * std::pow(q, static_cast<double>(x)) * std::pow(1.0 - q, static_cast<double>(n - x));
    }
    return sum;
}

} // namespace

TEST_CASE("HandoverPhaseLearner: a period of 1 s to an hour", "[hesap-net][handover]")
{
    Learner learner;
    CHECK_FALSE(learner.learning());
    learner.on_loss(5U * kSecond); // off: nothing happens
    CHECK(learner.phase() == hn::HandoverPhase{});
    const u32 revision = learner.revision();
    CHECK_FALSE(learner.start(Learner::kMinPeriodNs - 1U));
    CHECK_FALSE(learner.start(Learner::kMaxPeriodNs + 1U));
    CHECK_FALSE(learner.learning());
    CHECK(learner.revision() == revision);
    REQUIRE(learner.start(Learner::kMinPeriodNs));
    REQUIRE(learner.start(Learner::kMaxPeriodNs));
    REQUIRE(learner.start(kPeriod));
    CHECK(learner.period_ns() == kPeriod);
    CHECK(learner.revision() == revision + 3U);
    learner.stop();
    CHECK_FALSE(learner.learning());
    CHECK(learner.revision() == revision + 4U);
}

TEST_CASE("HandoverPhaseLearner: Starlink's timetable is learned on any clock within four periods, to a few ms",
          "[hesap-net][handover]")
{
    for (const u64 origin : {u64{0}, u64{123456789012345}, ~u64{0} - 600U * kSecond})
    {
        for (const u64 offset : {u64{0}, 7300U * kMs, kPeriod - 30U * kMs})
        {
            INFO("origin " << origin << ", offset " << offset);
            LeoPath path;
            path.origin = origin;
            path.first = origin + kSecond + offset;
            Learner learner;
            REQUIRE(learner.start(kPeriod));
            // Three handovers cannot lock; the period after the fourth's can.
            path.run(learner, origin, path.first + 3U * kPeriod - kSecond);
            CHECK_FALSE(learner.phase().locked);
            path.run(learner, path.first + 3U * kPeriod - kSecond, path.first + 5U * kPeriod + kSecond);
            REQUIRE(learner.phase().locked);
            CHECK(learner.phase().support >= Learner::kMinLock);
            CHECK(learner.phase().phase_ns < kPeriod);
            // Within 12 ms of the event: the first packet the outage takes, a bin's 256th either side.
            CHECK(distance(learner.phase().phase_ns, path.first, kPeriod) <= 12U * kMs);
        }
    }
}

TEST_CASE("HandoverPhaseLearner: a delay step alone, a sparse sender, and a busy path's background still lock",
          "[hesap-net][handover]")
{
    SECTION("the delay step alone")
    {
        LeoPath path;
        path.first = 4321U * kMs;
        path.outage = 0U;
        path.spike = 25U * kMs;
        path.spike_for = 100U * kMs;
        Learner learner;
        REQUIRE(learner.start(kPeriod));
        path.run(learner, 0U, 12U * kPeriod);
        REQUIRE(learner.phase().locked);
        CHECK(distance(learner.phase().phase_ns, path.first, kPeriod) <= 15U * kMs);
    }
    SECTION("a sample every 50 ms")
    {
        LeoPath path;
        path.first = 9876U * kMs;
        path.step = 50U * kMs;
        Learner learner;
        REQUIRE(learner.start(kPeriod));
        path.run(learner, 0U, 12U * kPeriod);
        REQUIRE(learner.phase().locked);
        CHECK(distance(learner.phase().phase_ns, path.first, kPeriod) <= 55U * kMs);
    }
    SECTION("a random loss a second")
    {
        LeoPath path;
        path.first = 2500U * kMs;
        path.background = 1U;
        Learner learner;
        REQUIRE(learner.start(kPeriod));
        path.run(learner, 0U, 16U * kPeriod);
        REQUIRE(learner.phase().locked);
        CHECK(distance(learner.phase().phase_ns, path.first, kPeriod) <= 12U * kMs);
    }
}

TEST_CASE("HandoverPhaseLearner: random losses, however often, and a one-off burst never lock", "[hesap-net][handover]")
{
    for (const u64 background : {u64{1}, u64{3}, u64{8}})
    {
        INFO("losses a second " << background);
        LeoPath path;
        path.background = background;
        Learner learner;
        REQUIRE(learner.start(kPeriod));
        // The recurrence share alone (half the periods) would lock on these: the binomial test refuses them.
        for (u32 period = 0U; period < 64U; ++period)
        {
            path.run(learner, period * kPeriod, (period + 1U) * kPeriod);
            REQUIRE_FALSE(learner.phase().locked);
        }
    }
    // One burst of 100 losses, then a quiet path: one period's event, never a timetable.
    Learner learner;
    REQUIRE(learner.start(kPeriod));
    LeoPath quiet;
    quiet.run(learner, 0U, 3U * kSecond);
    for (u64 i = 0U; i < 100U; ++i)
    {
        learner.on_loss(3U * kSecond + i * kMs);
    }
    quiet.run(learner, 3100U * kMs, 20U * kPeriod);
    CHECK_FALSE(learner.phase().locked);
}

TEST_CASE("HandoverPhaseLearner: a clock 1000 ppm slow is followed period by period", "[hesap-net][handover]")
{
    // The link's period is 15.015 s on this clock: the event moves 15 ms a period, 600 ms over the run.
    LeoPath path;
    path.first = 5000U * kMs;
    path.period = kPeriod + 15U * kMs;
    Learner learner;
    REQUIRE(learner.start(kPeriod));
    u64 worst = 0U;
    u32 locked = 0U;
    for (u32 period = 0U; period < 48U; ++period)
    {
        path.run(learner, period * kPeriod, (period + 1U) * kPeriod);
        if (period < 6U)
        {
            continue;
        }
        REQUIRE(learner.phase().locked);
        ++locked;
        // The estimate is for the period being sent, the newest one: its event.
        const u64 d = distance(learner.phase().phase_ns, path.first + period * path.period, kPeriod);
        worst = (d > worst) ? d : worst;
    }
    INFO("worst " << worst / kMs << " ms");
    CHECK(locked == 42U);
    CHECK(worst <= 20U * kMs); // the drift followed, not lagged
}

TEST_CASE("HandoverPhaseLearner: the lock is released once the handovers stop, and restart() forgets it",
          "[hesap-net][handover]")
{
    LeoPath path;
    path.first = 1000U * kMs;
    path.handovers_until = 20U * kPeriod;
    Learner learner;
    REQUIRE(learner.start(kPeriod));
    path.run(learner, 0U, 20U * kPeriod);
    REQUIRE(learner.phase().locked);
    const u32 locked_revision = learner.revision();
    u32 quiet = 0U;
    while (learner.phase().locked && quiet < 20U)
    {
        path.run(learner, (20U + quiet) * kPeriod, (21U + quiet) * kPeriod);
        // Period 20 + quiet's first send weighed the `quiet` quiet periods completed before it.
        quiet += learner.phase().locked ? 1U : 0U;
    }
    // A third of the fifteen periods completed keeps it: the eleventh quiet one releases it.
    CHECK(quiet == 11U);
    CHECK(learner.phase() == hn::HandoverPhase{});
    CHECK(learner.revision() != locked_revision);
    // Learned again, then forgotten by restart() with the period kept.
    LeoPath again;
    again.first = 40U * kPeriod + 3U * kSecond;
    again.run(learner, 40U * kPeriod, 50U * kPeriod);
    REQUIRE(learner.phase().locked);
    learner.restart();
    CHECK(learner.learning());
    CHECK(learner.period_ns() == kPeriod);
    CHECK(learner.phase() == hn::HandoverPhase{});
}

TEST_CASE("HandoverPhaseLearner: a busy path's own disturbances neither hold a lock once the handovers stop nor drop "
          "it while they go on",
          "[hesap-net][handover]")
{
    // A random loss a second disturbs about 38% of the periods in any two-bin window of a 15 s period: more than the
    // third the keep rule asks, so without the binomial test the background alone held the lock for good.
    for (const u64 until : {20U * kPeriod, ~u64{0}})
    {
        INFO("handovers until " << until / kSecond << " s");
        LeoPath path;
        path.first = 2500U * kMs;
        path.background = 1U;
        path.handovers_until = until;
        Learner learner;
        REQUIRE(learner.start(kPeriod));
        path.run(learner, 0U, 16U * kPeriod);
        REQUIRE(learner.phase().locked);
        u32 held = 0U;
        for (u32 period = 16U; period < 64U && learner.phase().locked; ++period)
        {
            path.run(learner, period * kPeriod, (period + 1U) * kPeriod);
            held += learner.phase().locked ? 1U : 0U;
        }
        if (until == ~u64{0})
        {
            CHECK(held == 48U);
            CHECK(distance(learner.phase().phase_ns, path.first, kPeriod) <= 12U * kMs);
        }
        else
        {
            // Four periods held by the handovers before they stop, then fewer than the eleven a quiet path takes.
            CHECK(held >= 4U);
            CHECK(held < 4U + 11U);
            CHECK(learner.phase() == hn::HandoverPhase{});
        }
    }
}

TEST_CASE("HandoverPhaseLearner: time running backwards, jumping and at the clock's end leaves a valid estimate",
          "[hesap-net][handover]")
{
    Learner learner;
    REQUIRE(learner.start(Learner::kMinPeriodNs));
    Words words;
    for (u32 i = 0U; i < 200000U; ++i)
    {
        const u64 w = words.next();
        const u64 t = ((w & 3U) == 0U) ? w : (~u64{0} - (w >> 40U));
        if ((w & 4U) != 0U)
        {
            learner.on_loss(t);
        }
        else
        {
            learner.on_rtt(t, w >> 20U);
        }
        const hn::HandoverPhase& phase = learner.phase();
        REQUIRE(phase.phase_ns < Learner::kMinPeriodNs);
        REQUIRE(phase.support <= phase.periods);
        REQUIRE(phase.periods <= Learner::kHistory);
        REQUIRE((phase.locked || phase == hn::HandoverPhase{}));
    }
}

TEST_CASE("HandoverPhaseLearner: the binomial tail is never below the true tail and within 1e-11 of it",
          "[hesap-net][handover]")
{
    constexpr double one = 4611686018427387904.0; // 2^62
    CHECK(Learner::binomial_tail_q62(16U, 0U, 0U) == Learner::kOneQ62);
    CHECK(Learner::binomial_tail_q62(4U, 5U, Learner::kOneQ62) == 0U);
    CHECK(Learner::binomial_tail_q62(16U, 16U, Learner::kOneQ62) == Learner::kOneQ62);
    CHECK(Learner::binomial_tail_q62(16U, 1U, 0U) == 0U);
    for (u32 n = 1U; n <= Learner::kHistory; ++n)
    {
        for (u32 h = 1U; h <= n; ++h)
        {
            for (const double q : {0.001, 0.05, 0.3, 0.5, 0.9})
            {
                const u64 tail = Learner::binomial_tail_q62(n, h, static_cast<u64>(q * one));
                const double reference = tail_reference(n, h, static_cast<double>(static_cast<u64>(q * one)) / one);
                INFO(n << " " << h << " " << q);
                CHECK(static_cast<double>(tail) / one >= reference * (1.0 - 1e-12));
                CHECK(static_cast<double>(tail) / one <= reference * (1.0 + 1e-12) + 1e-11);
                CHECK(tail <= Learner::binomial_tail_q62(n, h - 1U, static_cast<u64>(q * one)));
            }
        }
    }
}
