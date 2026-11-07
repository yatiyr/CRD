// crd-hesap-link-board: nanoseconds per packet of hesap-net's link models, on the Philox stream production draws from.
// No peer: netem runs in the kernel behind a qdisc and has no comparable loop of its own, so crd-hesap-link-netem-tests
// compares its laws and this says what each model costs a simulated packet.
//
// Usage: crd-hesap-link-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round over few packets (the smoke CTest)

#include <crd/hesap/net/link_models.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace hn = crd::hesap::net;
using crd::i64;
using crd::u32;

namespace
{

using Clock = std::chrono::steady_clock;
using Rng = crd::hesap::stats::PhiloxRng;

template <typename Work> [[nodiscard]] double timed_ns(u32 packets, double best, const Work& work)
{
    const Clock::time_point start = Clock::now();
    work();
    const double each = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / packets;
    return each < best ? each : best;
}

[[nodiscard]] double chain_round(hn::LossChain& chain, u32 packets, double best)
{
    Rng rng(601U, 1U);
    u32 lost = 0U;
    const double time = timed_ns(packets, best,
                                 [&]
                                 {
                                     for (u32 i = 0U; i < packets; ++i)
                                     {
                                         lost += chain.next(rng) ? 1U : 0U;
                                     }
                                 });
    // A chain whose answers are thrown away is timing nothing.
    if (lost > packets)
    {
        std::exit(1);
    }
    return time;
}

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 25U;
    u32 packets = 1U << 20U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            packets = 1U << 12U;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-hesap-link-board [--rounds N] [--quick]\n");
            return 2;
        }
    }

    constexpr double far = std::numeric_limits<double>::max();
    double independent_best = far;
    double gilbert_best = far;
    double four_best = far;
    double grants_best = far;
    hn::GilbertElliottChances gilbert;
    if (!hn::gilbert_from_rate_and_burst(0.03, 4.0, gilbert))
    {
        return 1;
    }
    const hn::FourStateChances four_chances{hn::link_chance(0.02), hn::link_chance(0.005), hn::link_chance(0.2),
                                            hn::link_chance(0.4), hn::link_chance(0.3)};
    hn::MediumGrantOptions grant_options;
    grant_options.gap.mean_ns = 10'000;
    grant_options.packets = 4U;
    grant_options.hold_ns = 5000;

    for (u32 round = 0U; round < rounds; ++round)
    {
        hn::LossChain chain;
        chain.set_independent(hn::link_chance(0.01));
        independent_best = chain_round(chain, packets, independent_best);
        chain.set_gilbert_elliott(gilbert);
        gilbert_best = chain_round(chain, packets, gilbert_best);
        if (!chain.set_four_state(four_chances))
        {
            return 1;
        }
        four_best = chain_round(chain, packets, four_best);

        Rng rng(601U, 2U);
        hn::MediumGrants grants(rng);
        if (!grants.init(grant_options, 0))
        {
            return 1;
        }
        i64 sum = 0;
        grants_best = timed_ns(packets, grants_best,
                               [&]
                               {
                                   for (u32 i = 0U; i < packets; ++i)
                                   {
                                       sum ^= grants.depart(i64{i} * 1000, 1200U);
                                   }
                               });
        if (sum == 0x7FFFFFFFFFFFFFFFLL)
        {
            return 1;
        }
    }

    std::printf("| Model | ns per packet |\n|---|---|\n");
    std::printf("| independent loss | %.2f |\n", independent_best);
    std::printf("| Gilbert-Elliott | %.2f |\n", gilbert_best);
    std::printf("| four states | %.2f |\n", four_best);
    std::printf("| medium grants | %.2f |\n", grants_best);

    struct ShapeRow
    {
        const char* name;
        hn::DelayShape shape;
    };
    const i64 measured[] = {10'000, 20'000, 25'000, 40'000, 90'000};
    std::printf("\n| Exact law | ns, correlation 0 | ns, correlation 0.5 |\n|---|---|---|\n");
    const ShapeRow shapes[] = {{"uniform", hn::DelayShape::Uniform},
                               {"normal", hn::DelayShape::Normal},
                               {"log-normal", hn::DelayShape::LogNormal},
                               {"Pareto", hn::DelayShape::Pareto},
                               {"Pareto-normal", hn::DelayShape::ParetoNormal},
                               {"table", hn::DelayShape::Table}};
    for (const ShapeRow& shape : shapes)
    {
        double best[2] = {far, far};
        for (u32 round = 0U; round < rounds; ++round)
        {
            for (int arm = 0; arm < 2; ++arm)
            {
                hn::DelayOptions options;
                options.shape = shape.shape;
                options.mean_ns = 40'000'000;
                options.deviation_ns = 8'000'000;
                options.correlation = arm == 0 ? 0.0 : 0.5;
                options.table = crd::containers::ConstSpan<i64>{measured, 5U};
                Rng rng(602U, 2U);
                hn::DelayProcess process(rng);
                if (!process.init(options))
                {
                    return 1;
                }
                i64 sum = 0;
                best[arm] = timed_ns(packets, best[arm],
                                     [&]
                                     {
                                         for (u32 i = 0U; i < packets; ++i)
                                         {
                                             sum += process.next();
                                         }
                                     });
                if (sum < 0)
                {
                    return 1;
                }
            }
        }
        std::printf("| %s | %.1f | %.1f |\n", shape.name, best[0], best[1]);
    }
    return 0;
}
