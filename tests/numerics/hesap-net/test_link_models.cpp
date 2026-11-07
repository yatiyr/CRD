// HESAP-NET's link models against their published laws: the chance arithmetic by hand, every transition of the chains
// on scripted words with a word on each threshold, the stationary laws and burst lengths in closed form, the delay
// shapes by a Kolmogorov-Smirnov distance from their own CDFs and their correlation by Sheppard's formula, the medium
// grants' budgets, gaps and lapses, and one digest per model that every lane must reproduce. crd-hesap-link-netem-tests
// sets the same laws on the running kernel's netem and compares the two statistically.

#include "link_laws.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/link_models.hpp>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace hn = crd::hesap::net;
using Catch::Approx;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::hesap::stats::PhiloxRng;
using hn::DelayOptions;
using hn::DelayProcess;
using hn::DelayShape;
using link_laws::mix_deviation;
using link_laws::pareto_standard;

namespace
{

constexpr f64 kPi = 3.14159265358979323846;
constexpr i64 kMean = 40'000'000;
constexpr i64 kDeviation = 8'000'000;
constexpr f64 kKsOneInAThousand = 1.95;

struct Script
{
    const u32* words;
    usize count;
    usize used = 0U;

    u32 next_u32()
    {
        REQUIRE(used < count);
        return words[used++];
    }
};

[[nodiscard]] f64 phi(f64 z)
{
    return 0.5 * std::erfc(-z * std::sqrt(0.5));
}

// The rank of `y` under the law whose value at standard normal z is the increasing `quantile`.
template <typename Quantile> [[nodiscard]] f64 rank_of(f64 y, const Quantile& quantile)
{
    f64 low = -12.0;
    f64 high = 12.0;
    for (int i = 0; i < 100; ++i)
    {
        const f64 middle = 0.5 * (low + high);
        (quantile(middle) < y ? low : high) = middle;
    }
    return phi(0.5 * (low + high));
}

// The standard CDF of a shape, at a delay. Pareto's shapes invert by bisection, so `tail` and `mix` are computed once.
[[nodiscard]] f64 shape_cdf(DelayShape shape, f64 tail, f64 mix, f64 delay)
{
    const f64 x = (delay - static_cast<f64>(kMean)) / static_cast<f64>(kDeviation);
    switch (shape)
    {
        case DelayShape::Uniform:
            return std::clamp((x + std::sqrt(3.0)) / (2.0 * std::sqrt(3.0)), 0.0, 1.0);
        case DelayShape::Normal:
            return phi(x);
        case DelayShape::LogNormal:
        {
            const f64 ratio = static_cast<f64>(kDeviation) / static_cast<f64>(kMean);
            const f64 s = std::sqrt(std::log1p(ratio * ratio));
            return phi((std::log(delay / static_cast<f64>(kMean)) + (0.5 * s * s)) / s);
        }
        case DelayShape::Pareto:
            return rank_of(x, [tail](f64 z) { return pareto_standard(z, tail); });
        case DelayShape::ParetoNormal:
            return rank_of(x, [tail, mix](f64 z) { return ((0.25 * z) + (0.75 * pareto_standard(z, tail))) / mix; });
        case DelayShape::Table:
            break;
    }
    return 0.0;
}

struct Shape
{
    DelayShape shape;
    const char* name;
};

constexpr Shape kShapes[] = {{DelayShape::Uniform, "uniform"},
                             {DelayShape::Normal, "normal"},
                             {DelayShape::LogNormal, "log_normal"},
                             {DelayShape::Pareto, "pareto"},
                             {DelayShape::ParetoNormal, "pareto_normal"}};

[[nodiscard]] DelayOptions wide_options(DelayShape shape, f64 correlation, f64 tail)
{
    DelayOptions options;
    options.shape = shape;
    options.mean_ns = kMean;
    options.deviation_ns = kDeviation;
    options.correlation = correlation;
    options.pareto_tail = tail;
    options.floor_ns = std::numeric_limits<i64>::min();
    options.ceiling_ns = std::numeric_limits<i64>::max();
    return options;
}

template <typename Words> [[nodiscard]] u32 lost_of(hn::LossChain& model, Words& words, u32 packets)
{
    u32 lost = 0U;
    for (u32 i = 0U; i < packets; ++i)
    {
        lost += model.next(words) ? 1U : 0U;
    }
    return lost;
}

[[nodiscard]] u64 digest_step(u64 digest, u64 value)
{
    return (digest ^ value) * 0x100000001B3ULL;
}

} // namespace

TEST_CASE("link models: a chance is a count of words below it", "[link-models]")
{
    CHECK(hn::link_chance(0.0) == 0U);
    CHECK(hn::link_chance(-1.0) == 0U);
    CHECK(hn::link_chance(std::numeric_limits<f64>::quiet_NaN()) == 0U);
    CHECK(hn::link_chance(1.0) == hn::kLinkCertain);
    CHECK(hn::link_chance(2.0) == hn::kLinkCertain);
    CHECK(hn::link_chance(0.5) == u64{1} << 31U);
    CHECK(hn::link_chance(0.25) == u64{1} << 30U);
    CHECK(hn::link_chance(std::ldexp(1.0, -32)) == 1U);
    CHECK(hn::link_chance(std::ldexp(1.0, -33)) == 0U); // A tie goes to the even count.
    CHECK(hn::link_chance(std::ldexp(3.0, -33)) == 2U);
}

TEST_CASE("link models: independent loss", "[link-models]")
{
    SECTION("the fate word decides, the move word does nothing, and a word on the chance is not below it")
    {
        constexpr u32 words[] = {0U, 99U, 0xFFFFFFFFU, 100U, 7U, 101U};
        Script script{words, 6U};
        hn::LossChain chain;
        chain.set_independent(100U);
        CHECK(chain.next(script));
        CHECK_FALSE(chain.next(script));
        CHECK_FALSE(chain.next(script));
        CHECK(script.used == 6U);
    }
    SECTION("never and certain hold at the ends of the word range")
    {
        constexpr u32 words[] = {0U, 0U, 0U, 0xFFFFFFFFU};
        Script script{words, 4U};
        hn::LossChain chain;
        chain.set_independent(0U);
        CHECK_FALSE(chain.next(script));
        chain.set_independent(hn::kLinkCertain);
        CHECK(chain.next(script));
    }
    SECTION("default-constructed loses nothing, on two words a packet")
    {
        constexpr u32 words[] = {0U, 0U};
        Script script{words, 2U};
        hn::LossChain chain;
        CHECK_FALSE(chain.next(script));
        CHECK(script.used == 2U);
    }
    SECTION("the rate")
    {
        PhiloxRng rng(11U, 1U);
        hn::LossChain chain;
        chain.set_independent(hn::link_chance(0.02));
        constexpr u32 packets = 2'000'000U;
        CHECK(static_cast<f64>(lost_of(chain, rng, packets)) / packets == Approx(0.02).margin(5e-4));
    }
}

TEST_CASE("link models: Gilbert-Elliott moves, then the packet takes the state it is in", "[link-models]")
{
    const hn::GilbertElliottChances chances{.to_bad = 10U, .to_good = 20U, .lost_in_good = 30U, .lost_in_bad = 40U};
    SECTION("every transition and fate, a word on each threshold")
    {
        constexpr u32 words[] = {
            10U, 29U, // Good stays on a word at to_bad, and a fate word under lost_in_good loses.
            10U, 30U, // A fate word at it survives.
            9U,  39U, // Below to_bad moves to bad, and the packet takes bad's chance.
            20U, 40U, // Bad stays on a word at to_good; a fate word at lost_in_bad survives.
            19U, 29U, // Below to_good moves to good; good's chance applies.
        };
        Script script{words, 10U};
        hn::LossChain chain;
        chain.set_gilbert_elliott(chances);
        CHECK(chain.next(script));
        CHECK_FALSE(chain.next(script));
        CHECK(chain.next(script));
        CHECK_FALSE(chain.next(script));
        CHECK(chain.next(script));
    }
    SECTION("a new setting starts in the good state")
    {
        constexpr u32 words[] = {0U, 0xFFFFFFFFU, 0xFFFFFFFFU, 35U};
        Script script{words, 4U};
        hn::LossChain chain;
        chain.set_gilbert_elliott(chances);
        CHECK_FALSE(chain.next(script)); // Now bad.
        chain.set_gilbert_elliott(chances);
        CHECK_FALSE(chain.next(script)); // 35 is below bad's 40 and not below good's 30.
    }
    SECTION("the stationary loss of the full four-parameter chain")
    {
        // pi_bad = p / (p + r), so the loss is pi_good (1 - k) + pi_bad (1 - h) (Elliott 1963).
        PhiloxRng rng(21U, 2U);
        hn::LossChain chain;
        chain.set_gilbert_elliott(
            {hn::link_chance(0.05), hn::link_chance(0.3), hn::link_chance(0.01), hn::link_chance(0.7)});
        constexpr u32 packets = 4'000'000U;
        const f64 pi_bad = 0.05 / 0.35;
        const f64 expected = ((1.0 - pi_bad) * 0.01) + (pi_bad * 0.7);
        CHECK(static_cast<f64>(lost_of(chain, rng, packets)) / packets == Approx(expected).margin(1e-3));
    }
}

TEST_CASE("link models: a simple Gilbert chain by what it means", "[link-models]")
{
    SECTION("refusals write nothing")
    {
        hn::GilbertElliottChances out{1U, 2U, 3U, 4U};
        const f64 nan = std::numeric_limits<f64>::quiet_NaN();
        CHECK_FALSE(hn::gilbert_from_rate_and_burst(-0.1, 2.0, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_burst(1.0, 2.0, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_burst(nan, 2.0, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_burst(0.1, 0.5, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_burst(0.1, nan, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_burst(0.9, 1.0, out)); // An entry chance of 9 is no chance.
        CHECK_FALSE(hn::gilbert_from_rate_and_correlation(-0.1, 0.5, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_correlation(1.0, 0.5, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_correlation(nan, 0.5, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_correlation(0.1, -0.1, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_correlation(0.1, 1.0, out));
        CHECK_FALSE(hn::gilbert_from_rate_and_correlation(0.1, nan, out));
        CHECK((out.to_bad == 1U && out.to_good == 2U && out.lost_in_good == 3U && out.lost_in_bad == 4U));
    }
    SECTION("the asked rate and burst")
    {
        hn::GilbertElliottChances chances;
        REQUIRE(hn::gilbert_from_rate_and_burst(0.03, 4.0, chances));
        CHECK(chances.to_good == hn::link_chance(0.25));
        CHECK(chances.to_bad == hn::link_chance(0.03 * 0.25 / 0.97));
        hn::LossChain chain;
        chain.set_gilbert_elliott(chances);
        PhiloxRng rng(31U, 1U);
        constexpr u32 packets = 4'000'000U;
        u32 lost = 0U;
        u32 bursts = 0U;
        u32 singles = 0U;
        u32 run = 0U;
        for (u32 i = 0U; i < packets; ++i)
        {
            if (chain.next(rng))
            {
                ++lost;
                ++run;
            }
            else if (run != 0U)
            {
                ++bursts;
                singles += run == 1U ? 1U : 0U;
                run = 0U;
            }
        }
        CHECK(static_cast<f64>(lost) / packets == Approx(0.03).margin(1e-3));
        CHECK(static_cast<f64>(lost) / bursts == Approx(4.0).margin(0.1));
        CHECK(static_cast<f64>(singles) / bursts == Approx(0.25).margin(0.01)); // A burst of one is the exit chance.
    }
    SECTION("the asked rate and lag-one correlation")
    {
        for (const f64 correlation : {0.0, 0.25, 0.6, 0.9})
        {
            hn::GilbertElliottChances chances;
            REQUIRE(hn::gilbert_from_rate_and_correlation(0.05, correlation, chances));
            hn::LossChain chain;
            chain.set_gilbert_elliott(chances);
            PhiloxRng rng(41U, 2U);
            constexpr u32 packets = 4'000'000U;
            u64 lost = 0U;
            u64 both = 0U;
            bool last = false;
            for (u32 i = 0U; i < packets; ++i)
            {
                const bool now = chain.next(rng);
                lost += now ? 1U : 0U;
                both += (now && last) ? 1U : 0U;
                last = now;
            }
            const f64 rate = static_cast<f64>(lost) / packets;
            const f64 lag_one = ((static_cast<f64>(both) / packets) - (rate * rate)) / (rate * (1.0 - rate));
            CHECK(rate == Approx(0.05).margin(2e-3));
            CHECK(lag_one == Approx(correlation).margin(0.01));
        }
    }
}

TEST_CASE("link models: the four-state chain", "[link-models]")
{
    SECTION("refusals change nothing")
    {
        hn::LossChain chain;
        chain.set_independent(hn::kLinkCertain);
        hn::FourStateChances chances;
        chances.p13 = hn::kLinkCertain;
        chances.p14 = 1U;
        CHECK_FALSE(chain.set_four_state(chances));
        chances = {};
        chances.p31 = hn::kLinkCertain;
        chances.p32 = 1U;
        CHECK_FALSE(chain.set_four_state(chances));
        chances = {};
        chances.p23 = hn::kLinkCertain + 1U;
        CHECK_FALSE(chain.set_four_state(chances));
        chances = {};
        chances.p13 = ~u64{0};
        chances.p14 = 2U; // A sum that would wrap is refused on the chance itself.
        CHECK_FALSE(chain.set_four_state(chances));
        constexpr u32 words[] = {0U, 0U};
        Script script{words, 2U};
        CHECK(chain.next(script)); // Still the independent setting.
    }
    SECTION("outgoing sums of exactly one are allowed")
    {
        hn::LossChain chain;
        const u64 half = u64{1} << 31U;
        CHECK(chain.set_four_state({.p13 = half, .p14 = half, .p31 = half, .p32 = half}));
    }
    SECTION("every transition, a word on each threshold")
    {
        const hn::FourStateChances chances{.p13 = 100U, .p14 = 50U, .p23 = 70U, .p31 = 200U, .p32 = 30U};
        // Each pair is a move word and a fate word. The states lose or keep with certainty, so the fate word is free.
        constexpr u32 words[] = {
            150U,        0xFFFFFFFFU, // 1 stays at p13 + p14.
            99U,         0xFFFFFFFFU, // 1 to 3 below p13: lost.
            230U,        0xFFFFFFFFU, // 3 stays at p31 + p32: lost.
            229U,        0xFFFFFFFFU, // 3 to 2 below p31 + p32: received.
            70U,         0U,          // 2 stays at p23: received, and fate word 0 changes nothing.
            69U,         0xFFFFFFFFU, // 2 to 3 below p23: lost.
            199U,        0U,          // 3 to 1 below p31: received.
            100U,        0xFFFFFFFFU, // 1 to 4 from p13 up: lost alone.
            0xFFFFFFFFU, 0U,          // 4 back to 1 whatever the word: received.
        };
        Script script{words, 18U};
        hn::LossChain chain;
        REQUIRE(chain.set_four_state(chances));
        const bool expected[] = {false, true, true, false, false, true, false, true, false};
        for (const bool lost : expected)
        {
            CHECK(chain.next(script) == lost);
        }
        CHECK(script.used == 18U);
    }
    SECTION("the stationary law")
    {
        // From the balance equations of the chain: pi_4 = p14 pi_1, pi_1 p13 = p31 pi_3, pi_2 p23 = p32 pi_3.
        const f64 p13 = 0.02;
        const f64 p14 = 0.005;
        const f64 p23 = 0.2;
        const f64 p31 = 0.4;
        const f64 p32 = 0.3;
        const f64 pi_1_per_3 = p31 / p13;
        const f64 pi_2_per_3 = p32 / p23;
        const f64 pi_4_per_3 = p14 * pi_1_per_3;
        const f64 pi_3 = 1.0 / (1.0 + pi_1_per_3 + pi_2_per_3 + pi_4_per_3);
        const f64 expected = pi_3 * (1.0 + pi_4_per_3);
        hn::LossChain chain;
        REQUIRE(chain.set_four_state({hn::link_chance(p13), hn::link_chance(p14), hn::link_chance(p23),
                                      hn::link_chance(p31), hn::link_chance(p32)}));
        PhiloxRng rng(51U, 3U);
        constexpr u32 packets = 4'000'000U;
        CHECK(static_cast<f64>(lost_of(chain, rng, packets)) / packets == Approx(expected).margin(2e-3));
    }
}

TEST_CASE("link models: DelayProcess refuses what has no law", "[link-models]")
{
    PhiloxRng rng(1U);
    DelayProcess process(rng);
    DelayOptions good;
    good.mean_ns = 100;
    good.deviation_ns = 10;
    REQUIRE(process.init(good));

    const auto refused = [&process](const DelayOptions& options)
    {
        return !process.init(options);
    };
    DelayOptions options = good;
    options.mean_ns = hn::kMaxLinkDelayNs + 1;
    CHECK(refused(options));
    options = good;
    options.mean_ns = -hn::kMaxLinkDelayNs - 1;
    CHECK(refused(options));
    options = good;
    options.deviation_ns = -1;
    CHECK(refused(options));
    options = good;
    options.deviation_ns = hn::kMaxLinkDelayNs + 1;
    CHECK(refused(options));
    options = good;
    options.correlation = 1.0;
    CHECK(refused(options));
    options = good;
    options.correlation = -0.01;
    CHECK(refused(options));
    options = good;
    options.correlation = std::numeric_limits<f64>::quiet_NaN();
    CHECK(refused(options));
    options = good;
    options.floor_ns = 11;
    options.ceiling_ns = 10;
    CHECK(refused(options));
    options = good;
    options.shape = DelayShape::LogNormal;
    options.mean_ns = 0;
    CHECK(refused(options));
    options.deviation_ns = 0;
    CHECK(process.init(options)); // No deviation is the mean, a log-normal or not.
    for (const DelayShape shape : {DelayShape::Pareto, DelayShape::ParetoNormal})
    {
        options = good;
        options.shape = shape;
        options.pareto_tail = 2.0;
        CHECK(refused(options));
        options.pareto_tail = std::numeric_limits<f64>::quiet_NaN();
        CHECK(refused(options));
        options.pareto_tail = 1e7;
        CHECK(refused(options));
        options.pareto_tail = 1e6;
        CHECK(process.init(options));
    }
    options = good;
    options.pareto_tail = -1.0; // Not read outside the Pareto shapes.
    CHECK(process.init(options));
    options = good;
    options.shape = DelayShape::ParetoNormal;
    options.pareto_share = 1.01;
    CHECK(refused(options));
    options.pareto_share = -0.01;
    CHECK(refused(options));
    options.pareto_share = std::numeric_limits<f64>::quiet_NaN();
    CHECK(refused(options));
    options.pareto_share = 1.0;
    CHECK(process.init(options));
    options = good;
    options.shape = DelayShape::Table;
    CHECK(refused(options)); // No table.
    const i64 descending[] = {2, 1};
    options.table = crd::containers::ConstSpan<i64>{descending, 2U};
    CHECK(refused(options));
    const i64 too_far[] = {0, hn::kMaxLinkDelayNs + 1};
    options.table = crd::containers::ConstSpan<i64>{too_far, 2U};
    CHECK(refused(options));
    const i64 flat[] = {5, 5};
    options.table = crd::containers::ConstSpan<i64>{flat, 2U};
    CHECK(process.init(options));
}

TEST_CASE("link models: every shape's law, with and without correlation", "[link-models]")
{
    constexpr usize samples_wanted = usize{1} << 17U;
    constexpr usize thin = 16U; // At correlation 0.5 samples this far apart are independent enough for the KS bound.
    for (const Shape& shape : kShapes)
    {
        for (const f64 correlation : {0.0, 0.5})
        {
            for (const f64 tail : {3.0, 5.0})
            {
                const bool pareto_family = shape.shape == DelayShape::Pareto || shape.shape == DelayShape::ParetoNormal;
                if (tail != 3.0 && !pareto_family)
                {
                    continue;
                }
                PhiloxRng rng(61U, 5U);
                DelayProcess process(rng);
                REQUIRE(process.init(wide_options(shape.shape, correlation, tail)));

                const usize stride = correlation == 0.0 ? 1U : thin;
                crd::containers::Array<i64> samples(samples_wanted);
                samples.resize(samples_wanted);
                f64 sum = 0.0;
                f64 squares = 0.0;
                for (usize i = 0U; i < samples_wanted; ++i)
                {
                    for (usize skip = 1U; skip < stride; ++skip)
                    {
                        (void)process.next();
                    }
                    samples[i] = process.next();
                    const f64 x =
                        (static_cast<f64>(samples[i]) - static_cast<f64>(kMean)) / static_cast<f64>(kDeviation);
                    sum += x;
                    squares += x * x;
                }
                INFO(shape.name << " correlation " << correlation << " tail " << tail);
                const f64 root_count = std::sqrt(static_cast<f64>(samples_wanted));
                CHECK(std::abs(sum / static_cast<f64>(samples_wanted)) < 6.0 / root_count);
                // A tail of 3 has no fourth moment, so its sample deviation says little; 5 has one.
                if (!pareto_family || tail == 5.0)
                {
                    CHECK(std::sqrt(squares / static_cast<f64>(samples_wanted)) ==
                          Approx(1.0).margin(pareto_family ? 0.1 : 0.02));
                }

                const f64 mix = mix_deviation(tail);
                std::sort(samples.begin(), samples.end());
                // The bisecting CDFs see every 64th order statistic, enough to find a wrong law at this distance.
                const usize step = pareto_family ? 64U : 1U;
                f64 worst = 0.0;
                for (usize i = 0U; i < samples_wanted; i += step)
                {
                    const f64 value = shape_cdf(shape.shape, tail, mix, static_cast<f64>(samples[i]));
                    worst = std::max(worst, std::abs(value - (static_cast<f64>(i) / static_cast<f64>(samples_wanted))));
                    worst = std::max(worst,
                                     std::abs(value - (static_cast<f64>(i + 1U) / static_cast<f64>(samples_wanted))));
                }
                CHECK(worst < kKsOneInAThousand / root_count);
            }
        }
    }
}

TEST_CASE("link models: a delay's correlation is the driving Gaussian's", "[link-models]")
{
    constexpr usize samples_wanted = usize{1} << 19U;
    for (const Shape& shape : kShapes)
    {
        for (const f64 correlation : {0.0, 0.3, 0.8})
        {
            PhiloxRng rng(71U, 6U);
            DelayProcess process(rng);
            REQUIRE(process.init(wide_options(shape.shape, correlation, 3.0)));

            crd::containers::Array<i64> series(samples_wanted);
            series.resize(samples_wanted);
            for (usize i = 0U; i < samples_wanted; ++i)
            {
                series[i] = process.next();
            }
            crd::containers::Array<i64> sorted(series);
            std::nth_element(sorted.begin(), sorted.begin() + (samples_wanted / 2U), sorted.end());
            const i64 median = sorted[samples_wanted / 2U];
            usize same_side = 0U;
            for (usize i = 1U; i < samples_wanted; ++i)
            {
                same_side += (series[i] > median) == (series[i - 1U] > median) ? 1U : 0U;
            }
            // Sheppard: P(consecutive values on one side of the median) = 1/2 + asin(rho) / pi, whatever increasing map
            // the shape applies.
            INFO(shape.name << " correlation " << correlation);
            CHECK(static_cast<f64>(same_side) / static_cast<f64>(samples_wanted - 1U) ==
                  Approx(0.5 + (std::asin(correlation) / kPi)).margin(4e-3));
        }
    }
}

TEST_CASE("link models: every answer is held between the floor and the ceiling", "[link-models]")
{
    for (const Shape& shape : kShapes)
    {
        DelayOptions options;
        options.shape = shape.shape;
        options.mean_ns = 1000;
        options.deviation_ns = hn::kMaxLinkDelayNs;
        options.pareto_tail = 2.0001;
        options.correlation = 0.99;
        options.floor_ns = 50;
        options.ceiling_ns = 5000;
        PhiloxRng rng(81U, 7U);
        DelayProcess process(rng);
        REQUIRE(process.init(options));
        for (u32 i = 0U; i < 20000U; ++i)
        {
            const i64 delay = process.next();
            CHECK((delay >= 50 && delay <= 5000));
        }
    }
    DelayOptions fixed;
    fixed.mean_ns = 77;
    PhiloxRng rng(1U);
    DelayProcess process(rng);
    REQUIRE(process.init(fixed));
    CHECK(process.next() == 77);
    fixed.floor_ns = 100;
    fixed.ceiling_ns = 100;
    REQUIRE(process.init(fixed));
    CHECK(process.next() == 100);
}

TEST_CASE("link models: init restarts the correlation", "[link-models]")
{
    DelayOptions options;
    options.mean_ns = kMean;
    options.deviation_ns = kDeviation;
    options.correlation = 0.99;
    PhiloxRng rng(91U, 8U);
    DelayProcess process(rng);
    REQUIRE(process.init(options));
    (void)process.next();
    (void)process.next();
    (void)process.next();
    REQUIRE(process.init(options));
    // The first answer after init is the next Gaussian itself, with no weight of the last.
    PhiloxRng alone(91U, 8U);
    crd::hesap::stats::NormalSampler normal(alone);
    (void)normal.next();
    (void)normal.next();
    (void)normal.next();
    const f64 fresh = static_cast<f64>(kMean) + (static_cast<f64>(kDeviation) * normal.next());
    CHECK(process.next() == static_cast<i64>(std::rint(fresh)));
}

TEST_CASE("link models: a table delay reads its measured law at the driving rank", "[link-models]")
{
    const i64 measured[] = {10'000, 20'000, 20'000, 80'000, 90'000};
    const crd::containers::ConstSpan<i64> table{measured, 5U};
    DelayOptions options;
    options.shape = DelayShape::Table;
    options.table = table;
    SECTION("the quantile function between the steps")
    {
        PhiloxRng rng(95U, 1U);
        DelayProcess process(rng);
        REQUIRE(process.init(options));
        constexpr u32 samples = 400000U;
        u32 at_or_below_20k = 0U;
        u32 at_or_below_50k = 0U;
        for (u32 i = 0U; i < samples; ++i)
        {
            const i64 delay = process.next();
            CHECK((delay >= 10'000 && delay <= 90'000));
            at_or_below_20k += delay <= 20'000 ? 1U : 0U;
            at_or_below_50k += delay <= 50'000 ? 1U : 0U;
        }
        // Ranks 0.25 and 0.5 hold 20 us, and 50 us lies halfway from rank 0.5 to rank 0.75.
        CHECK(static_cast<f64>(at_or_below_20k) / samples == Approx(0.5).margin(4e-3));
        CHECK(static_cast<f64>(at_or_below_50k) / samples == Approx(0.625).margin(4e-3));
    }
    SECTION("one entry is that delay")
    {
        options.table = crd::containers::ConstSpan<i64>{measured, 1U};
        PhiloxRng rng(95U, 2U);
        DelayProcess process(rng);
        REQUIRE(process.init(options));
        CHECK(process.next() == 10'000);
    }
    SECTION("the floor and the ceiling still hold")
    {
        options.floor_ns = 15'000;
        options.ceiling_ns = 60'000;
        PhiloxRng rng(95U, 3U);
        DelayProcess process(rng);
        REQUIRE(process.init(options));
        for (u32 i = 0U; i < 10000U; ++i)
        {
            const i64 delay = process.next();
            CHECK((delay >= 15'000 && delay <= 60'000));
        }
    }
    SECTION("correlation is the driving Gaussian's, as in every shape")
    {
        options.correlation = 0.8;
        PhiloxRng rng(95U, 4U);
        DelayProcess process(rng);
        REQUIRE(process.init(options));
        constexpr u32 samples = 400000U;
        u32 same_side = 0U;
        bool last = process.next() > 20'000;
        for (u32 i = 1U; i < samples; ++i)
        {
            const bool now = process.next() > 20'000;
            same_side += now == last ? 1U : 0U;
            last = now;
        }
        CHECK(static_cast<f64>(same_side) / (samples - 1U) == Approx(0.5 + (std::asin(0.8) / kPi)).margin(4e-3));
    }
}

[[nodiscard]] DelayOptions fixed_gap(i64 gap_ns)
{
    DelayOptions options;
    options.mean_ns = gap_ns;
    return options;
}

TEST_CASE("link models: medium grants", "[link-models]")
{
    PhiloxRng rng(101U, 1U);
    hn::MediumGrants grants(rng);
    SECTION("refusals change nothing")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(100);
        options.packets = 1U;
        REQUIRE(grants.init(options, 1000));
        hn::MediumGrantOptions bad = options;
        bad.hold_ns = -1;
        CHECK_FALSE(grants.init(bad, 5));
        bad = options;
        bad.gap.floor_ns = -1;
        CHECK_FALSE(grants.init(bad, 5));
        bad = options;
        bad.gap.correlation = 1.0;
        CHECK_FALSE(grants.init(bad, 5));
        CHECK(grants.grant_ns() == 1000);
        CHECK(grants.depart(0, 1U) == 1000);
        CHECK(grants.grant_ns() == 1100);
    }
    SECTION("no budget and no hold leave the medium open and draw nothing")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(100);
        REQUIRE(grants.init(options, 0));
        PhiloxRng untouched(101U, 1U);
        const u64 first = untouched.next_u64();
        CHECK(grants.depart(7, 1000U) == 7);
        CHECK(grants.depart(7, 1000U) == 7);
        CHECK(grants.depart(9000, 1000U) == 9000);
        CHECK(rng.next_u64() == first);
    }
    SECTION("a packet budget closes the grant on its last packet")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(100);
        options.packets = 2U;
        REQUIRE(grants.init(options, 0));
        CHECK(grants.depart(10, 1U) == 10);
        CHECK(grants.grant_ns() == 0);
        CHECK(grants.depart(20, 1U) == 20);
        CHECK(grants.grant_ns() == 120); // A gap after the packet that spent it.
        CHECK(grants.depart(20, 1U) == 120);
        CHECK(grants.depart(20, 1U) == 120);
        CHECK(grants.grant_ns() == 220);
    }
    SECTION("a byte budget lets the overrunning packet leave and ends the grant")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(50);
        options.bytes = 1000U;
        REQUIRE(grants.init(options, 0));
        CHECK(grants.depart(0, 999U) == 0);
        CHECK(grants.grant_ns() == 0);
        CHECK(grants.depart(1, 5000U) == 1);
        CHECK(grants.grant_ns() == 51);
        CHECK(grants.depart(1, 1000U) == 51);
        CHECK(grants.grant_ns() == 101); // Exactly the budget spends it.
    }
    SECTION("a grant unused for its hold lapses and the packet contends again")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(100);
        options.hold_ns = 30;
        REQUIRE(grants.init(options, 0));
        CHECK(grants.depart(29, 1U) == 29);
        CHECK(grants.grant_ns() == 0);
        CHECK(grants.depart(30, 1U) == 130);
        CHECK(grants.grant_ns() == 130);
        CHECK(grants.depart(159, 1U) == 159);
    }
    SECTION("a lapse draws its gap from the packet, and one spent grant may lapse into the next")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(100);
        options.hold_ns = 30;
        options.packets = 1U;
        REQUIRE(grants.init(options, 0));
        CHECK(grants.depart(0, 1U) == 0);
        CHECK(grants.grant_ns() == 100);
        CHECK(grants.depart(500, 1U) == 600); // 500 is past 100 + 30.
        CHECK(grants.grant_ns() == 700);
    }
    SECTION("the clock saturates at the far end")
    {
        hn::MediumGrantOptions options;
        options.gap = fixed_gap(hn::kMaxLinkDelayNs);
        options.packets = 1U;
        const i64 far = std::numeric_limits<i64>::max();
        REQUIRE(grants.init(options, far - 10));
        CHECK(grants.depart(far - 10, 1U) == far - 10);
        CHECK(grants.grant_ns() == far);
        CHECK(grants.depart(0, 1U) == far);
    }
    SECTION("drawn gaps follow their law: uniform between the two ends, from a seeded stream")
    {
        hn::MediumGrantOptions options;
        options.gap.shape = DelayShape::Uniform;
        options.gap.mean_ns = 3000;
        options.gap.deviation_ns = 577;
        options.packets = 1U;
        REQUIRE(grants.init(options, 0));
        i64 last = grants.grant_ns();
        f64 sum = 0.0;
        constexpr u32 draws = 200000U;
        for (u32 i = 0U; i < draws; ++i)
        {
            (void)grants.depart(last, 1U);
            const i64 gap = grants.grant_ns() - last;
            CHECK((gap >= 2000 && gap <= 4000));
            sum += static_cast<f64>(gap);
            last = grants.grant_ns();
        }
        CHECK(sum / draws == Approx(3000.0).margin(5.0));
    }
    SECTION("departures never run backward and never precede readiness")
    {
        hn::MediumGrantOptions options;
        options.gap.shape = DelayShape::LogNormal;
        options.gap.mean_ns = 500;
        options.gap.deviation_ns = 300;
        options.gap.floor_ns = 1;
        options.hold_ns = 200;
        options.packets = 3U;
        options.bytes = 3000U;
        REQUIRE(grants.init(options, 0));
        PhiloxRng arrivals(101U, 2U);
        i64 ready = 0;
        i64 previous = 0;
        for (u32 i = 0U; i < 100000U; ++i)
        {
            ready += static_cast<i64>(arrivals.next_u32() % 200U);
            const i64 leaves = grants.depart(ready, arrivals.next_u32() % 2000U);
            CHECK(leaves >= ready);
            CHECK(leaves >= previous);
            previous = leaves;
        }
    }
}

TEST_CASE("link models: a run is a pure function of seed and stream, on every lane", "[link-models]")
{
    // These digests pin every word and every rounded delay across compilers, kernel levels and platforms.
    constexpr u32 steps = 200000U;
    const auto loss_digest = [](hn::LossChain& chain, PhiloxRng& rng)
    {
        u64 digest = 0xCBF29CE484222325ULL;
        for (u32 i = 0U; i < steps; ++i)
        {
            digest = digest_step(digest, chain.next(rng) ? 1U : 0U);
        }
        return digest;
    };

    PhiloxRng independent_rng(1234U, 1U);
    hn::LossChain independent;
    independent.set_independent(hn::link_chance(0.1));
    CHECK(loss_digest(independent, independent_rng) == 0xa91baa204ae5f92fULL);

    PhiloxRng chain_rng(1234U, 2U);
    hn::LossChain chain;
    chain.set_gilbert_elliott(
        {hn::link_chance(0.05), hn::link_chance(0.3), hn::link_chance(0.01), hn::link_chance(0.9)});
    CHECK(loss_digest(chain, chain_rng) == 0xfb011f0f791eb1c5ULL);

    PhiloxRng four_rng(1234U, 3U);
    hn::LossChain four;
    REQUIRE(four.set_four_state({hn::link_chance(0.02), hn::link_chance(0.005), hn::link_chance(0.2),
                                 hn::link_chance(0.4), hn::link_chance(0.3)}));
    CHECK(loss_digest(four, four_rng) == 0xda61dc79c272f5deULL);

    const u64 expected[] = {0xa3507cf3cfb9e2efULL, 0xddb571acff457c98ULL, 0x7581afafbed2335fULL, 0xb7835fd681ca003aULL,
                            0x8fd890ab47a6ca8bULL};
    usize index = 0U;
    for (const Shape& shape : kShapes)
    {
        DelayOptions options;
        options.shape = shape.shape;
        options.mean_ns = kMean;
        options.deviation_ns = kDeviation;
        options.correlation = 0.4;
        PhiloxRng rng(1234U, 10U + index);
        DelayProcess process(rng);
        REQUIRE(process.init(options));
        u64 digest = 0xCBF29CE484222325ULL;
        for (u32 i = 0U; i < steps; ++i)
        {
            digest = digest_step(digest, static_cast<u64>(process.next()));
        }
        INFO(shape.name);
        CHECK(digest == expected[index]);
        ++index;
    }

    const i64 measured[] = {10'000, 20'000, 20'000, 80'000, 90'000};
    DelayOptions table_options;
    table_options.shape = DelayShape::Table;
    table_options.table = crd::containers::ConstSpan<i64>{measured, 5U};
    PhiloxRng table_rng(1234U, 20U);
    DelayProcess table(table_rng);
    REQUIRE(table.init(table_options));
    u64 table_digest = 0xCBF29CE484222325ULL;
    for (u32 i = 0U; i < steps; ++i)
    {
        table_digest = digest_step(table_digest, static_cast<u64>(table.next()));
    }
    CHECK(table_digest == 0xccbf8fec8e9fa576ULL);

    hn::MediumGrantOptions grant_options;
    grant_options.gap.shape = DelayShape::Uniform;
    grant_options.gap.mean_ns = 3000;
    grant_options.gap.deviation_ns = 577;
    grant_options.packets = 3U;
    grant_options.bytes = 4000U;
    grant_options.hold_ns = 1000;
    PhiloxRng grant_rng(1234U, 21U);
    hn::MediumGrants grants(grant_rng);
    REQUIRE(grants.init(grant_options, 0));
    u64 grant_digest = 0xCBF29CE484222325ULL;
    for (u32 i = 0U; i < steps; ++i)
    {
        grant_digest = digest_step(grant_digest, static_cast<u64>(grants.depart(i64{i} * 300, 1200U)));
    }
    CHECK(grant_digest == 0xb1287fec5ac66fefULL);

    // Another stream of the same seed is another run.
    PhiloxRng one(1234U, 1U);
    PhiloxRng other(1234U, 2U);
    CHECK(one.next_u64() != other.next_u64());
}
