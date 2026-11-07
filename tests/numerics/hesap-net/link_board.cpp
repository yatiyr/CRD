// crd-hesap-link-board: nanoseconds per packet of hesap-net's link models against Linux's netem, the pinned
// net/sched/sch_netem.c's own functions, on the same words. Each arm's decisions are summed and compared with the
// peer's, so a faster wrong answer fails. The analytic delay shapes have no peer: netem draws from a table, so their
// rows say what the exact marginal law costs beside it.
//
// Usage: crd-hesap-link-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round over few packets (the smoke CTest)

#include "netem_peer.h"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/link_models.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace hn = crd::hesap::net;
using crd::f64;
using crd::i16;
using crd::i64;
using crd::u32;
using crd::usize;

namespace
{

using Clock = std::chrono::steady_clock;

struct CycleWords
{
    const u32* words;
    usize count;
    usize used = 0U;

    u32 next_u32()
    {
        const u32 word = words[used % count];
        ++used;
        return word;
    }
};

template <typename Work> [[nodiscard]] double timed_ns(u32 packets, double best, const Work& work)
{
    const Clock::time_point start = Clock::now();
    work();
    const double each = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / packets;
    return each < best ? each : best;
}

void require_same(const char* arm, i64 ours, i64 netem)
{
    if (ours != netem)
    {
        std::fprintf(stderr, "%s: ours %lld, netem %lld\n", arm, static_cast<long long>(ours),
                     static_cast<long long>(netem));
        std::exit(1);
    }
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

    const usize word_count = usize{4U} * packets;
    crd::containers::Array<u32> words(word_count);
    words.resize(word_count);
    crd::hesap::stats::PhiloxRng source(601U, 1U);
    for (usize i = 0U; i < word_count; ++i)
    {
        words[i] = source.next_u32();
    }
    crd::containers::Array<i16> table(16U);
    for (int i = -8; i < 8; ++i)
    {
        table.push_back(static_cast<i16>(i * 1024));
    }

    struct Best
    {
        double ours = std::numeric_limits<double>::max();
        double netem = std::numeric_limits<double>::max();
    };
    Best random;
    Best chain;
    Best four;
    Best delay;
    const u32 chance = hn::link_chance(0.01);
    const hn::GilbertElliottChances chain_chances{hn::link_chance(0.05), hn::link_chance(0.3), hn::link_chance(0.1),
                                                  hn::link_chance(0.01)};
    const hn::FourStateChances four_chances{hn::link_chance(0.02), hn::link_chance(0.4), hn::link_chance(0.3),
                                            hn::link_chance(0.2), hn::link_chance(0.005)};
    const auto ours_run = [&packets](hn::LossModel& model, CycleWords& ours)
    {
        i64 lost = 0;
        for (u32 i = 0U; i < packets; ++i)
        {
            lost += model.next(ours) ? 1 : 0;
        }
        return lost;
    };

    for (u32 round = 0U; round < rounds; ++round)
    {
        i64 ours_sum = 0;
        i64 netem_sum = 0;

        CycleWords ours{words.data(), word_count};
        hn::LossModel model;
        model.set_random(chance, 0U, ours);
        random.ours = timed_ns(packets, random.ours, [&] { ours_sum = ours_run(model, ours); });
        crd_netem_set_words(words.data(), word_count);
        crd_netem_loss_random(chance, 0U);
        random.netem = timed_ns(packets, random.netem, [&] { netem_sum = crd_netem_loss_run(packets); });
        require_same("random", ours_sum, netem_sum);

        ours = CycleWords{words.data(), word_count};
        model = hn::LossModel{};
        model.set_gilbert_elliott(chain_chances);
        chain.ours = timed_ns(packets, chain.ours, [&] { ours_sum = ours_run(model, ours); });
        crd_netem_set_words(words.data(), word_count);
        crd_netem_loss_gilbert_elliott(chain_chances.p, chain_chances.r, chain_chances.survive_bad,
                                       chain_chances.lost_good);
        chain.netem = timed_ns(packets, chain.netem, [&] { netem_sum = crd_netem_loss_run(packets); });
        require_same("gilbert-elliott", ours_sum, netem_sum);

        ours = CycleWords{words.data(), word_count};
        model = hn::LossModel{};
        if (!model.set_four_state(four_chances))
        {
            return 1;
        }
        four.ours = timed_ns(packets, four.ours, [&] { ours_sum = ours_run(model, ours); });
        crd_netem_set_words(words.data(), word_count);
        crd_netem_loss_four_state(four_chances.p13, four_chances.p31, four_chances.p32, four_chances.p23,
                                  four_chances.p14);
        four.netem = timed_ns(packets, four.netem, [&] { netem_sum = crd_netem_loss_run(packets); });
        require_same("four-state", ours_sum, netem_sum);

        ours = CycleWords{words.data(), word_count};
        hn::CorrelatedDraw draw;
        draw.init(0U, ours);
        const crd::containers::ConstSpan<i16> entries{table.data(), table.size()};
        delay.ours = timed_ns(packets, delay.ours,
                              [&]
                              {
                                  ours_sum = 0;
                                  for (u32 i = 0U; i < packets; ++i)
                                  {
                                      ours_sum += hn::netem_delay(40'000'000, 8'000'000, draw, ours, entries);
                                  }
                              });
        crd_netem_set_words(words.data(), word_count);
        crd_netem_delay_table(table.data(), static_cast<u32>(table.size()));
        crd_netem_delay_correlation(0U);
        delay.netem =
            timed_ns(packets, delay.netem, [&] { netem_sum = crd_netem_delay_run(40'000'000, 8'000'000, packets); });
        require_same("table delay", ours_sum, netem_sum);
    }

    std::printf("| Model | netem ns | hesap ns | ratio |\n|---|---|---|---|\n");
    const auto row = [](const char* name, const Best& best)
    {
        std::printf("| %s | %.2f | %.2f | %.2fx |\n", name, best.netem, best.ours, best.netem / best.ours);
    };
    row("loss random 1%", random);
    row("Gilbert-Elliott", chain);
    row("four states", four);
    row("table delay", delay);

    struct ShapeRow
    {
        const char* name;
        hn::DelayShape shape;
    };
    std::printf("\n| Exact law | ns, correlation 0 | ns, correlation 0.5 |\n|---|---|---|\n");
    const ShapeRow shapes[] = {{"uniform", hn::DelayShape::Uniform},
                               {"normal", hn::DelayShape::Normal},
                               {"log-normal", hn::DelayShape::LogNormal},
                               {"Pareto", hn::DelayShape::Pareto},
                               {"Pareto-normal", hn::DelayShape::ParetoNormal}};
    for (const ShapeRow& shape : shapes)
    {
        double best[2] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
        for (u32 round = 0U; round < rounds; ++round)
        {
            for (int arm = 0; arm < 2; ++arm)
            {
                hn::DelayOptions options;
                options.shape = shape.shape;
                options.mean_ns = 40'000'000;
                options.deviation_ns = 8'000'000;
                options.correlation = arm == 0 ? 0.0 : 0.5;
                crd::hesap::stats::PhiloxRng rng(602U, 2U);
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
