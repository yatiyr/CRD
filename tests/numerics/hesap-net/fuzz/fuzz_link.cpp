// crd-fuzz-hesap-link: hesap-net's link models (HESAP-NET; crd/hesap/net/link_models.hpp) under the bounded fuzz
// harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the next bytes its settings, and the rest is the words the model draws (zeros
// when they run out).
//   0  independent loss: any chance;
//   1  Gilbert-Elliott: any four chances;
//   2  four states: any five chances;
//   3  a simple Gilbert chain by what it means: any two doubles, either form;
//   4  a DelayProcess: any options, table and seed, every shape;
//   5  medium grants: any options, then a clock and packets chosen by the words.
//
// Oracles, none of them the model's own verdict:
//   * the chains' definitions, restated as a transition matrix of the model's published chances, decide every packet,
//     and every packet takes exactly two words; a refused four-state setting changes nothing;
//   * the refusal conditions are restated for the chains, the process and the grants; a refused call writes nothing;
//   * every delay lies between the floor and the ceiling, and the same seed and stream repeat a process exactly;
//   * a departure is never before its packet is ready or before the last departure, no grant exceeds its budgets, and
//     a grant moves only for a spent budget or a lapsed hold.

#include <crd/fuzz/harness.hpp>
#include <crd/hesap/net/link_models.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace
{

using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

// The input's bytes as settings and words, each read once, in order.
struct Input
{
    const u8* data;
    usize size;
    usize used_words = 0U;

    template <typename T> T read()
    {
        T value{};
        const usize take = sizeof(T) < size ? sizeof(T) : size;
        std::memcpy(&value, data, take);
        data += take;
        size -= take;
        return value;
    }

    u32 next_u32()
    {
        ++used_words;
        return read<u32>();
    }
};

// A second reader of the same words, for the restated chain.
struct Replay
{
    Input words;

    u64 next() { return words.next_u32(); }
};

// One state of a restated chain: the words below each cumulative edge take it, the rest stay.
struct Edges
{
    u64 first;
    u64 second;
    u32 first_to;
    u32 second_to;
    u64 lost;
};

void hold_chain(Input& input, LossChain& chain, const Edges* states)
{
    Replay replay{input};
    u32 at = 0U;
    for (u32 packet = 0U; packet < 256U && input.size != 0U; ++packet)
    {
        const u64 move = replay.next();
        const u64 fate = replay.next();
        const Edges& edges = states[at];
        if (move < edges.first)
        {
            at = edges.first_to;
        }
        else if (move < edges.second)
        {
            at = edges.second_to;
        }
        const usize words_before = input.used_words;
        CRD_FUZZ_REQUIRE(chain.next(input) == (fate < states[at].lost));
        CRD_FUZZ_REQUIRE(input.used_words == words_before + 2U);
    }
}

void hold_independent(Input& input)
{
    const u64 lost = input.read<u64>();
    LossChain chain;
    chain.set_independent(lost);
    const Edges states[] = {{0U, 0U, 0U, 0U, lost}};
    hold_chain(input, chain, states);
}

void hold_gilbert_elliott(Input& input)
{
    const GilbertElliottChances chances = input.read<GilbertElliottChances>();
    LossChain chain;
    chain.set_gilbert_elliott(chances);
    const Edges states[] = {{chances.to_bad, 0U, 1U, 0U, chances.lost_in_good},
                            {chances.to_good, 0U, 0U, 0U, chances.lost_in_bad}};
    hold_chain(input, chain, states);
}

void hold_four_state(Input& input)
{
    const FourStateChances chances = input.read<FourStateChances>();
    const bool each_within = chances.p13 <= kLinkCertain && chances.p14 <= kLinkCertain &&
                             chances.p23 <= kLinkCertain && chances.p31 <= kLinkCertain && chances.p32 <= kLinkCertain;
    const bool valid =
        each_within && chances.p13 + chances.p14 <= kLinkCertain && chances.p31 + chances.p32 <= kLinkCertain;
    LossChain chain;
    chain.set_independent(kLinkCertain);
    CRD_FUZZ_REQUIRE(chain.set_four_state(chances) == valid);
    if (!valid)
    {
        // Still the setting before the refused call: every packet lost.
        for (u32 packet = 0U; packet < 8U; ++packet)
        {
            CRD_FUZZ_REQUIRE(chain.next(input));
        }
        return;
    }
    // Paper states 1, 2, 3 and 4 in that order; 3 and 4 are the lost ones and 4 always returns to 1.
    const Edges states[] = {{chances.p13, chances.p13 + chances.p14, 2U, 3U, 0U},
                            {chances.p23, 0U, 2U, 0U, 0U},
                            {chances.p31, chances.p31 + chances.p32, 0U, 1U, kLinkCertain},
                            {kLinkCertain, 0U, 0U, 0U, kLinkCertain}};
    hold_chain(input, chain, states);
}

void hold_gilbert_meaning(Input& input)
{
    const f64 loss = input.read<f64>();
    const f64 second = input.read<f64>();
    const GilbertElliottChances untouched{1U, 2U, 3U, 4U};

    GilbertElliottChances burst = untouched;
    const bool burst_valid = loss >= 0.0 && loss < 1.0 && second >= 1.0 && loss * (1.0 / second) / (1.0 - loss) <= 1.0;
    CRD_FUZZ_REQUIRE(gilbert_from_rate_and_burst(loss, second, burst) == burst_valid);
    if (burst_valid)
    {
        CRD_FUZZ_REQUIRE(burst.lost_in_good == 0U && burst.lost_in_bad == kLinkCertain);
        CRD_FUZZ_REQUIRE(burst.to_good == link_chance(1.0 / second));
        CRD_FUZZ_REQUIRE(burst.to_bad <= kLinkCertain);
    }
    else
    {
        CRD_FUZZ_REQUIRE(burst.to_bad == 1U && burst.to_good == 2U && burst.lost_in_good == 3U &&
                         burst.lost_in_bad == 4U);
    }

    GilbertElliottChances correlated = untouched;
    const bool correlated_valid = loss >= 0.0 && loss < 1.0 && second >= 0.0 && second < 1.0;
    CRD_FUZZ_REQUIRE(gilbert_from_rate_and_correlation(loss, second, correlated) == correlated_valid);
    if (correlated_valid)
    {
        CRD_FUZZ_REQUIRE(correlated.to_bad == link_chance(loss * (1.0 - second)));
        CRD_FUZZ_REQUIRE(correlated.to_good == link_chance((1.0 - loss) * (1.0 - second)));
        CRD_FUZZ_REQUIRE(correlated.lost_in_good == 0U && correlated.lost_in_bad == kLinkCertain);
    }
    else
    {
        CRD_FUZZ_REQUIRE(correlated.to_bad == 1U && correlated.to_good == 2U && correlated.lost_in_good == 3U &&
                         correlated.lost_in_bad == 4U);
    }
}

[[nodiscard]] DelayOptions read_delay_options(Input& input, i64* table, usize table_capacity)
{
    DelayOptions options;
    options.shape = static_cast<DelayShape>(input.read<u8>() % 6U);
    options.mean_ns = input.read<i64>();
    options.deviation_ns = input.read<i64>();
    options.correlation = input.read<f64>();
    options.pareto_tail = input.read<f64>();
    options.pareto_share = input.read<f64>();
    options.floor_ns = input.read<i64>();
    options.ceiling_ns = input.read<i64>();
    const usize entries = input.read<u8>() % (table_capacity + 1U);
    for (usize i = 0U; i < entries; ++i)
    {
        table[i] = input.read<i64>();
    }
    options.table = crd::containers::ConstSpan<i64>{table, entries};
    return options;
}

[[nodiscard]] bool delay_options_valid(const DelayOptions& options)
{
    const bool pareto_shape = options.shape == DelayShape::Pareto || options.shape == DelayShape::ParetoNormal;
    bool table_valid = !options.table.empty();
    for (usize i = 0U; i < options.table.size(); ++i)
    {
        table_valid = table_valid && options.table[i] >= -kMaxLinkDelayNs && options.table[i] <= kMaxLinkDelayNs &&
                      (i == 0U || options.table[i - 1U] <= options.table[i]);
    }
    return options.mean_ns >= -kMaxLinkDelayNs && options.mean_ns <= kMaxLinkDelayNs && options.deviation_ns >= 0 &&
           options.deviation_ns <= kMaxLinkDelayNs && options.correlation >= 0.0 && options.correlation < 1.0 &&
           options.floor_ns <= options.ceiling_ns &&
           (options.shape != DelayShape::LogNormal || options.deviation_ns == 0 || options.mean_ns > 0) &&
           (!pareto_shape || (options.pareto_tail > 2.0 && options.pareto_tail <= 1e6)) &&
           (options.shape != DelayShape::ParetoNormal ||
            (options.pareto_share >= 0.0 && options.pareto_share <= 1.0)) &&
           (options.shape != DelayShape::Table || table_valid);
}

void hold_process(Input& input)
{
    i64 table[16] = {};
    const DelayOptions options = read_delay_options(input, table, 16U);
    const u64 seed = input.read<u64>();
    const u64 stream = input.read<u64>();
    const bool valid = delay_options_valid(options);

    crd::hesap::stats::PhiloxRng first_rng(seed, stream);
    crd::hesap::stats::PhiloxRng second_rng(seed, stream);
    DelayProcess first(first_rng);
    DelayProcess second(second_rng);
    CRD_FUZZ_REQUIRE(first.init(options) == valid);
    CRD_FUZZ_REQUIRE(second.init(options) == valid);
    if (!valid)
    {
        return;
    }
    for (u32 packet = 0U; packet < 32U; ++packet)
    {
        const i64 delay = first.next();
        CRD_FUZZ_REQUIRE(delay == second.next());
        CRD_FUZZ_REQUIRE(delay >= options.floor_ns && delay <= options.ceiling_ns);
        if (options.shape == DelayShape::Table && options.floor_ns <= options.table[0] &&
            options.ceiling_ns >= options.table[options.table.size() - 1U])
        {
            CRD_FUZZ_REQUIRE(delay >= options.table[0] && delay <= options.table[options.table.size() - 1U]);
        }
    }
}

void hold_grants(Input& input)
{
    i64 table[8] = {};
    MediumGrantOptions options;
    options.gap = read_delay_options(input, table, 8U);
    options.hold_ns = input.read<i64>();
    options.packets = input.read<u32>();
    options.bytes = input.read<u64>();
    i64 now = input.read<i64>();
    const u64 seed = input.read<u64>();

    const bool valid = options.hold_ns >= 0 && options.gap.floor_ns >= 0 && delay_options_valid(options.gap);
    crd::hesap::stats::PhiloxRng rng(seed, 1U);
    MediumGrants grants(rng);
    CRD_FUZZ_REQUIRE(grants.init(options, now) == valid);
    if (!valid)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(grants.grant_ns() == now);

    constexpr i64 far = std::numeric_limits<i64>::max();
    const auto later = [](i64 from, i64 gap)
    {
        return from > far - gap ? far : from + gap;
    };
    i64 previous = std::numeric_limits<i64>::min();
    u64 packets_spent = 0U;
    u64 bytes_spent = 0U;
    for (u32 packet = 0U; packet < 256U && input.size != 0U; ++packet)
    {
        now = later(now, input.next_u32());
        const u32 bytes = input.next_u32() >> 16U;
        const i64 grant = grants.grant_ns();
        const i64 ready_in_grant = now > grant ? now : grant;
        const bool lapses = options.hold_ns != 0 && static_cast<u64>(ready_in_grant) - static_cast<u64>(grant) >=
                                                        static_cast<u64>(options.hold_ns);

        const i64 leaves = grants.depart(now, bytes);
        CRD_FUZZ_REQUIRE(leaves >= previous);
        previous = leaves;
        if (lapses)
        {
            CRD_FUZZ_REQUIRE(leaves >= later(now, options.gap.floor_ns));
            CRD_FUZZ_REQUIRE(leaves <= later(now, options.gap.ceiling_ns));
            packets_spent = 0U;
            bytes_spent = 0U;
        }
        else
        {
            CRD_FUZZ_REQUIRE(leaves == ready_in_grant);
        }
        ++packets_spent;
        bytes_spent += bytes;
        const bool closes = (options.packets != 0U && packets_spent >= options.packets) ||
                            (options.bytes != 0U && bytes_spent >= options.bytes);
        if (closes)
        {
            CRD_FUZZ_REQUIRE(grants.grant_ns() >= later(leaves, options.gap.floor_ns));
            CRD_FUZZ_REQUIRE(grants.grant_ns() <= later(leaves, options.gap.ceiling_ns));
            packets_spent = 0U;
            bytes_spent = 0U;
        }
        else
        {
            CRD_FUZZ_REQUIRE(grants.grant_ns() == (lapses ? leaves : grant));
        }
    }
}

class Seed
{
public:
    explicit Seed(u8 mode) { m_bytes[0] = mode; }

    Seed& u8s(std::initializer_list<u8> values) { return put(values); }
    Seed& u32s(std::initializer_list<u32> values) { return put(values); }
    Seed& u64s(std::initializer_list<u64> values) { return put(values); }
    Seed& i64s(std::initializer_list<i64> values) { return put(values); }
    Seed& f64s(std::initializer_list<f64> values) { return put(values); }

    void end(crd::fuzz::SeedSink& sink, const char* name) const { sink.add(name, m_bytes, m_size); }

    // Spread words, so the chains visit their states.
    void words(crd::fuzz::SeedSink& sink, const char* name)
    {
        u32 word = 0x9E3779B9U;
        for (u32 i = 0U; i < 200U; ++i)
        {
            word = (word * 2654435761U) + 12345U;
            (void)u32s({word});
        }
        end(sink, name);
    }

    // Words cycling over the given values, which sit on and beside a model's thresholds.
    void near(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u32> values)
    {
        for (u32 round = 0U; round < 13U; ++round)
        {
            for (const u32 value : values)
            {
                (void)u32s({value + (round % 3U)});
            }
        }
        end(sink, name);
    }

private:
    template <typename T> Seed& put(std::initializer_list<T> values)
    {
        for (const T value : values)
        {
            std::memcpy(m_bytes + m_size, &value, sizeof(T));
            m_size += sizeof(T);
        }
        return *this;
    }

    u8 m_bytes[2048]{};
    usize m_size = 1U;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Input input{data + 1U, size - 1U};
    switch (data[0] % 6U)
    {
        case 0U:
            hold_independent(input);
            break;
        case 1U:
            hold_gilbert_elliott(input);
            break;
        case 2U:
            hold_four_state(input);
            break;
        case 3U:
            hold_gilbert_meaning(input);
            break;
        case 4U:
            hold_process(input);
            break;
        default:
            hold_grants(input);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    const i64 far = std::numeric_limits<i64>::max();
    const u64 half = u64{1} << 31U;
    Seed(0U).u64s({42949673U}).words(sink, "independent");
    Seed(0U).u64s({kLinkCertain}).words(sink, "independent-certain");
    Seed(1U).u64s({214748365U, 1288490189U, 0U, kLinkCertain}).words(sink, "gilbert");
    Seed(1U).u64s({100U, 200U, 300U, 400U}).near(sink, "gilbert-thresholds", {99U, 100U, 200U, 300U, 301U, 399U, 400U});
    Seed(2U).u64s({85899346U, 21474836U, 858993459U, 1717986918U, 1288490189U}).words(sink, "four-state");
    Seed(2U)
        .u64s({100U, 50U, 70U, 200U, 30U})
        .near(sink, "four-state-thresholds",
              {29U, 30U, 49U, 50U, 69U, 70U, 99U, 100U, 149U, 150U, 199U, 200U, 229U, 230U});
    Seed(2U).u64s({kLinkCertain, 1U, 0U, 0U, 0U}).words(sink, "four-state-past-one");
    Seed(2U).u64s({half, half, half, half, half}).words(sink, "four-state-one");
    Seed(2U).u64s({~u64{0}, 2U, 0U, 0U, 0U}).words(sink, "four-state-wrap");
    Seed(3U).f64s({0.03, 4.0}).end(sink, "meaning-burst");
    Seed(3U).f64s({0.05, 0.25}).end(sink, "meaning-correlation");
    Seed(3U).f64s({std::numeric_limits<f64>::quiet_NaN(), 2.0}).end(sink, "meaning-nan");
    Seed(3U).f64s({0.9, 1.0}).end(sink, "meaning-entry-past-one");
    const auto process = [far](u8 shape, i64 mean, i64 deviation, f64 correlation, f64 tail, f64 share) -> Seed
    {
        Seed seed(4U);
        seed.u8s({shape}).i64s({mean, deviation}).f64s({correlation, tail, share}).i64s({0, far});
        return seed;
    };
    process(1U, 40000000, 8000000, 0.5, 3.0, 0.75).u8s({0U}).i64s({1, 2}).end(sink, "process-normal");
    process(2U, 40000000, 8000000, 0.0, 3.0, 0.75).u8s({0U}).i64s({3, 4}).end(sink, "process-log-normal");
    process(3U, 40000000, kMaxLinkDelayNs, 0.99, 2.0001, 0.75).u8s({0U}).i64s({5, 6}).end(sink, "process-heavy");
    process(4U, 40000000, 8000000, 0.9, 3.0, 0.25).u8s({0U}).i64s({7, 8}).end(sink, "process-mix");
    process(5U, 0, 0, 0.5, 3.0, 0.0).u8s({4U}).i64s({10, 20, 20, 90, 9, 10}).end(sink, "process-table");
    process(5U, 0, 0, 0.0, 3.0, 0.0).u8s({2U}).i64s({20, 10, 9, 10}).end(sink, "process-table-descending");
    process(0U, 0, 1, 1.0, 3.0, 0.0).u8s({0U}).i64s({0, 0}).end(sink, "process-refused");
    const auto gap = [](u8 mode, i64 mean, i64 deviation) -> Seed
    {
        Seed seed(mode);
        seed.u8s({0U}).i64s({mean, deviation}).f64s({0.0, 3.0, 0.75}).i64s({0, kMaxLinkDelayNs}).u8s({0U});
        return seed;
    };
    gap(5U, 1000, 300).i64s({0}).u32s({2U}).u64s({0U}).i64s({0}).u64s({1U}).words(sink, "grants-packets");
    gap(5U, 10, 0).i64s({0}).u32s({0U}).u64s({3000U}).i64s({0}).u64s({2U}).words(sink, "grants-bytes");
    gap(5U, 500, 100).i64s({2000}).u32s({0U}).u64s({0U}).i64s({0}).u64s({3U}).words(sink, "grants-hold");
    gap(5U, kMaxLinkDelayNs, 0).i64s({0}).u32s({1U}).u64s({0U}).i64s({far - 10}).u64s({4U}).words(sink, "grants-far");
    gap(5U, 10, 0).i64s({-1}).u32s({0U}).u64s({0U}).i64s({0}).u64s({5U}).end(sink, "grants-refused");
}
