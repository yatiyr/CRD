// hesap-net's link models against Linux's netem, the pinned net/sched/sch_netem.c's own functions on the same words:
// every loss decision, every delay and every slot, bit for bit, with the words consumed counted too, on the Philox
// stream and on scripted words that sit on each threshold. The analytic delay shapes are then held to iproute2's own
// distribution tables, built here from the pinned release's generators, and the correlated word's lost rate is
// measured.

#include "link_laws.hpp"
#include "netem_peer.h"

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/link_models.hpp>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace hn = crd::hesap::net;
using Catch::Approx;
using crd::f64;
using crd::i16;
using crd::i32;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::hesap::stats::PhiloxRng;

namespace
{

constexpr u32 kOne = 0xFFFFFFFFU;

using Words = crd::containers::Array<u32>;

// The same array the peer reads, in the same cycle.
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

[[nodiscard]] Words philox_words(u64 seed, u64 stream, usize count)
{
    Words words(count);
    words.resize(count);
    PhiloxRng rng(seed, stream);
    for (usize i = 0U; i < count; ++i)
    {
        words[i] = rng.next_u32();
    }
    return words;
}

// Words that land on each threshold, one either side of it, and on the ends of the range.
[[nodiscard]] Words threshold_words(std::initializer_list<u32> thresholds, usize count, u64 seed)
{
    Words candidates;
    for (const u32 threshold : thresholds)
    {
        candidates.push_back(threshold);
        candidates.push_back(threshold + 1U);
        candidates.push_back(threshold - 1U);
    }
    candidates.push_back(0U);
    candidates.push_back(kOne);
    Words words(count);
    words.resize(count);
    PhiloxRng rng(seed, 77U);
    for (usize i = 0U; i < count; ++i)
    {
        words[i] = candidates[rng.next_below(candidates.size())];
    }
    return words;
}

void hold_loss_to_netem(hn::LossModel& model, CycleWords& ours, u32 packets)
{
    u32 lost = 0U;
    for (u32 i = 0U; i < packets; ++i)
    {
        const bool ours_lost = model.next(ours);
        const bool netem_lost = crd_netem_loss_event();
        lost += netem_lost ? 1U : 0U;
        if (ours_lost != netem_lost || ours.used != crd_netem_words_used())
        {
            FAIL_CHECK("packet " << i << ": ours " << ours_lost << " netem " << netem_lost << ", words " << ours.used
                                 << " and " << crd_netem_words_used());
            return;
        }
    }
    SUCCEED("lost " << lost << " of " << packets);
}

// A distribution table as get_distribution() reads a .dist file.
[[nodiscard]] crd::containers::Array<i16> read_table(const char* name)
{
    crd::containers::Array<i16> table;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%s.dist", CRD_NETEM_TABLE_DIR, name);
    std::FILE* file = std::fopen(path, "r");
    REQUIRE(file != nullptr);
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        if (line[0] == '#')
        {
            continue;
        }
        char* at = line;
        char* after = nullptr;
        for (long value = std::strtol(at, &after, 10); after != at; value = std::strtol(at, &after, 10))
        {
            table.push_back(static_cast<i16>(value));
            at = after;
        }
    }
    std::fclose(file);
    return table;
}

constexpr u32 kPackets = 400000U;

} // namespace

TEST_CASE("netem: every loss decision and word, bit for bit", "[link-netem]")
{
    struct Random
    {
        f64 chance;
        f64 correlation;
    };
    SECTION("random, with and without correlation")
    {
        for (const Random setting : {Random{0.0, 0.0}, Random{0.01, 0.0}, Random{0.1, 0.0}, Random{1.0, 0.0},
                                     Random{0.01, 0.25}, Random{0.3, 0.75}, Random{0.5, 1.0}})
        {
            const Words words = philox_words(101U, 1U, kPackets);
            crd_netem_set_words(words.data(), words.size());
            crd_netem_loss_random(hn::link_chance(setting.chance), hn::link_chance(setting.correlation));
            CycleWords ours{words.data(), words.size()};
            hn::LossModel model;
            model.set_random(hn::link_chance(setting.chance), hn::link_chance(setting.correlation), ours);
            hold_loss_to_netem(model, ours, kPackets);
        }
    }
    SECTION("Gilbert-Elliott")
    {
        const hn::GilbertElliottChances settings[] = {
            {hn::link_chance(0.05), hn::link_chance(0.3), 0U, 0U},
            {hn::link_chance(0.05), hn::link_chance(0.3), hn::link_chance(0.2), hn::link_chance(0.01)},
            {hn::link_chance(1.0), hn::link_chance(1.0), hn::link_chance(0.5), hn::link_chance(0.5)},
            {0U, 0U, kOne, 0U},
        };
        for (const hn::GilbertElliottChances& chances : settings)
        {
            const Words words = philox_words(102U, 2U, kPackets);
            crd_netem_set_words(words.data(), words.size());
            crd_netem_loss_gilbert_elliott(chances.p, chances.r, chances.survive_bad, chances.lost_good);
            CycleWords ours{words.data(), words.size()};
            hn::LossModel model;
            model.set_gilbert_elliott(chances);
            hold_loss_to_netem(model, ours, kPackets);
        }
    }
    SECTION("Gilbert-Elliott, words on the thresholds")
    {
        const hn::GilbertElliottChances chances{1000U, 2000U, 3000U, 4000U};
        const Words words = threshold_words({1000U, 2000U, 3000U, 4000U}, kPackets, 103U);
        crd_netem_set_words(words.data(), words.size());
        crd_netem_loss_gilbert_elliott(chances.p, chances.r, chances.survive_bad, chances.lost_good);
        CycleWords ours{words.data(), words.size()};
        hn::LossModel model;
        model.set_gilbert_elliott(chances);
        hold_loss_to_netem(model, ours, kPackets);
    }
    SECTION("four states")
    {
        const hn::FourStateChances settings[] = {
            {hn::link_chance(0.02), hn::link_chance(0.4), hn::link_chance(0.3), hn::link_chance(0.2),
             hn::link_chance(0.005)},
            {hn::link_chance(0.1), hn::link_chance(0.9), 0U, 0U, 0U},
            {hn::link_chance(0.5), hn::link_chance(0.25), hn::link_chance(0.75), hn::link_chance(1.0),
             hn::link_chance(0.25)},
        };
        for (const hn::FourStateChances& chances : settings)
        {
            const Words words = philox_words(104U, 3U, kPackets);
            crd_netem_set_words(words.data(), words.size());
            crd_netem_loss_four_state(chances.p13, chances.p31, chances.p32, chances.p23, chances.p14);
            CycleWords ours{words.data(), words.size()};
            hn::LossModel model;
            REQUIRE(model.set_four_state(chances));
            hold_loss_to_netem(model, ours, kPackets);
        }
    }
    SECTION("four states, words on the thresholds and their sums")
    {
        const hn::FourStateChances chances{1000U, 2000U, 3000U, 500U, 700U};
        const Words words = threshold_words({1000U, 2000U, 3000U, 500U, 700U, 1700U, 5000U}, kPackets, 105U);
        crd_netem_set_words(words.data(), words.size());
        crd_netem_loss_four_state(chances.p13, chances.p31, chances.p32, chances.p23, chances.p14);
        CycleWords ours{words.data(), words.size()};
        hn::LossModel model;
        REQUIRE(model.set_four_state(chances));
        hold_loss_to_netem(model, ours, kPackets);
    }
}

TEST_CASE("netem: every delay, bit for bit, on each of iproute2's tables", "[link-netem]")
{
    const crd::containers::Array<i16> normal = read_table("normal");
    const crd::containers::Array<i16> pareto = read_table("pareto");
    const crd::containers::Array<i16> mix = read_table("paretonormal");
    REQUIRE(normal.size() == 4096U);
    REQUIRE(pareto.size() == 4096U);
    REQUIRE(mix.size() == 4096U);

    struct Table
    {
        const i16* entries;
        u32 size;
    };
    const Table tables[] = {{nullptr, 0U}, {normal.data(), 4096U}, {pareto.data(), 4096U}, {mix.data(), 4096U}};
    const i64 means[] = {0, 1, 40'000'000, -5000, i64{1} << 40U};
    const i32 deviations[] = {0, 1, 8191, 8192, 8193, 123456, 8'000'000, std::numeric_limits<i32>::max()};

    for (const Table& table : tables)
    {
        for (const f64 correlation : {0.0, 0.25, 0.9})
        {
            for (const i64 mean : means)
            {
                for (const i32 deviation : deviations)
                {
                    const Words words = philox_words(201U, 4U, 4096U);
                    crd_netem_set_words(words.data(), words.size());
                    crd_netem_delay_table(table.entries, table.size);
                    crd_netem_delay_correlation(hn::link_chance(correlation));
                    CycleWords ours{words.data(), words.size()};
                    hn::CorrelatedDraw draw;
                    draw.init(hn::link_chance(correlation), ours);
                    const crd::containers::ConstSpan<i16> entries{table.entries, table.size};
                    bool same = true;
                    for (u32 i = 0U; i < 4000U; ++i)
                    {
                        same =
                            same &&
                            hn::netem_delay(mean, deviation, draw, ours, entries) == crd_netem_delay(mean, deviation) &&
                            ours.used == crd_netem_words_used();
                    }
                    CHECK(same);
                }
            }
        }
    }
    crd_netem_delay_table(nullptr, 0U);
}

TEST_CASE("netem: every slot, bit for bit", "[link-netem]")
{
    const crd::containers::Array<i16> normal = read_table("normal");
    struct Setting
    {
        i64 min_delay;
        i64 max_delay;
        i32 max_packets;
        i32 max_bytes;
        i64 dist_delay;
        i32 dist_jitter;
        bool table;
    };
    const Setting settings[] = {
        {0, 0, 0, 0, 0, 0, false},
        {1000, 1000, 1, 0, 0, 0, false},
        {800'000, 7'000'000, 8, 0, 0, 0, false},
        {800'000, 7'000'000, 0, 6000, 0, 0, false},
        {1, 1'000'000'000, 3, 3000, 0, 0, false},
        {0, 0, 4, 9000, 5'000'000, 1'000'000, true},
    };
    for (const Setting& setting : settings)
    {
        const Words words = philox_words(301U, 5U, 65536U);
        crd_netem_set_words(words.data(), words.size());
        crd_netem_delay_table(setting.table ? normal.data() : nullptr, setting.table ? 4096U : 0U);
        constexpr i64 start = 1'000'000;
        crd_netem_slot_init(setting.min_delay, setting.max_delay, setting.max_packets, setting.max_bytes,
                            setting.dist_delay, setting.dist_jitter, start);
        hn::SlotOptions options;
        options.min_delay_ns = setting.min_delay;
        options.max_delay_ns = setting.max_delay;
        options.max_packets = setting.max_packets;
        options.max_bytes = setting.max_bytes;
        options.dist_mean_ns = setting.dist_delay;
        options.dist_deviation_ns = setting.dist_jitter;
        if (setting.table)
        {
            options.table = crd::containers::ConstSpan<i16>{normal.data(), 4096U};
        }
        hn::SlotGate gate;
        REQUIRE(gate.init(options, start));

        CycleWords ours{words.data(), words.size()};
        PhiloxRng schedule(302U, 6U);
        i64 now = start;
        i64 due = start;
        bool same = true;
        for (u32 i = 0U; i < 200000U && same; ++i)
        {
            now += static_cast<i64>(schedule.next_below(400'000U));
            const u32 bytes = 40U + static_cast<u32>(schedule.next_below(1460U));
            const bool ours_left = gate.admit(now, due, bytes, ours);
            const bool netem_left = crd_netem_slot_admit(static_cast<u64>(now), static_cast<u64>(due), bytes);
            same = ours_left == netem_left && ours.used == crd_netem_words_used() &&
                   (!gate.active() || static_cast<u64>(gate.next_open_ns()) == crd_netem_slot_next());
            if (ours_left)
            {
                due = now + static_cast<i64>(schedule.next_below(800'000U));
            }
        }
        CHECK(same);
    }
    crd_netem_delay_table(nullptr, 0U);
}

TEST_CASE("netem: the analytic shapes have the laws of iproute2's tables", "[link-netem]")
{
    constexpr usize samples_wanted = usize{1} << 20U;
    constexpr i64 mean_ns = 400'000'000;
    constexpr i64 deviation_ns = 8'000'000;

    struct Case
    {
        const char* table;
        hn::DelayShape shape;
        f64 unit;
    };
    const Case cases[] = {{"normal", hn::DelayShape::Normal, 1.0},
                          {"pareto", hn::DelayShape::Pareto, 2.0 / std::sqrt(3.0)},
                          {"paretonormal", hn::DelayShape::ParetoNormal, link_laws::mix_deviation(3.0)}};
    for (const Case& shape_case : cases)
    {
        const crd::containers::Array<i16> table = read_table(shape_case.table);
        crd::containers::Array<i16> sorted(table);
        std::sort(sorted.begin(), sorted.end());

        hn::DelayOptions options;
        options.shape = shape_case.shape;
        options.mean_ns = mean_ns;
        options.deviation_ns = deviation_ns;
        options.floor_ns = 0;
        PhiloxRng rng(401U, 7U);
        hn::DelayProcess process(rng);
        REQUIRE(process.init(options));
        crd::containers::Array<f64> samples(samples_wanted);
        samples.resize(samples_wanted);
        for (usize i = 0U; i < samples_wanted; ++i)
        {
            samples[i] =
                (static_cast<f64>(process.next()) - static_cast<f64>(mean_ns)) / static_cast<f64>(deviation_ns);
        }
        std::sort(samples.begin(), samples.end());

        // netem's tables are the laws at their own scale: the normal's is one, Pareto's 2 / sqrt(3), the mix's its own
        // deviation. Pareto's part of a table stops at 4 of its units, a rank of 0.989 and past it, so ranks stop
        // short.
        const f64 count = static_cast<f64>(sorted.size());
        INFO(shape_case.table << " unit " << shape_case.unit);

        f64 worst = 0.0;
        for (usize i = 0U; i < (sorted.size() * 985U) / 1000U; ++i)
        {
            const f64 x = static_cast<f64>(sorted[i]) / hn::kNetemTableScale;
            const f64* found = std::lower_bound(samples.begin(), samples.end(), x / shape_case.unit);
            const f64 ours = static_cast<f64>(found - samples.begin()) / static_cast<f64>(samples_wanted);
            worst = std::max(worst, std::abs(ours - (static_cast<f64>(i) / count)));
        }
        // One table step and the samples' KS at one in a thousand.
        CHECK(worst < (2.0 / count) + (1.95 / std::sqrt(static_cast<f64>(samples_wanted))));
    }
}

TEST_CASE("netem: its correlated word loses the rate it was asked for, and the chain keeps it", "[link-netem]")
{
    constexpr u32 many_packets = 4'000'000U;
    constexpr f64 asked = 0.01;
    constexpr f64 correlation = 0.25;

    PhiloxRng netem_rng(501U, 8U);
    hn::LossModel netem_style;
    netem_style.set_random(hn::link_chance(asked), hn::link_chance(correlation), netem_rng);
    u32 netem_lost = 0U;
    for (u32 i = 0U; i < many_packets; ++i)
    {
        netem_lost += netem_style.next(netem_rng) ? 1U : 0U;
    }

    PhiloxRng chain_rng(501U, 9U);
    hn::GilbertElliottChances chances;
    REQUIRE(hn::gilbert_from_rate_and_correlation(asked, correlation, chances));
    hn::LossModel chain;
    chain.set_gilbert_elliott(chances);
    u32 chain_lost = 0U;
    for (u32 i = 0U; i < many_packets; ++i)
    {
        chain_lost += chain.next(chain_rng) ? 1U : 0U;
    }

    const f64 netem_rate = static_cast<f64>(netem_lost) / many_packets;
    const f64 chain_rate = static_cast<f64>(chain_lost) / many_packets;
    INFO("asked " << asked << ", netem's word " << netem_rate << ", the chain " << chain_rate);
    CHECK(netem_rate < 0.7 * asked);
    CHECK(chain_rate == Approx(asked).margin(3e-4));
}
