// crd-fuzz-hesap-link: hesap-net's link models (HESAP-NET; crd/hesap/net/link_models.hpp) under the bounded fuzz
// harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the next bytes its settings, and the rest is the words the model draws (zeros
// when they run out).
//   0  random loss: any chance and correlation;
//   1  Gilbert-Elliott: any four chances;
//   2  four states: any five chances;
//   3  a simple Gilbert chain by what it means: any two doubles, either form;
//   4  a table delay: any mean, deviation and table;
//   5  a DelayProcess: any options, table and seed, every shape;
//   6  a slot gate: any options, then a clock and packets chosen by the words;
//   7  medium grants: any options, then a clock and packets chosen by the words.
//
// Oracles, none of them the model's own verdict:
//   * the three loss machines and the correlated word, restated as data (a band table per four-state state, and the
//     correlated word as a move from the last answer toward the new word), decide every packet and every word
//     consumed, and a refused four-state setting changes nothing; the Gilbert-Elliott machine and the Gilbert
//     parameters follow the published models, but which side of a threshold a word falls on, the 2^32 fixed point and
//     the correlation's 32.32 weights are netem's rules, which the models match and these oracles pin down;
//   * the table delay's arithmetic, restated in magnitude and sign, decides every table delay; its word-to-entry
//     choice and its scale of 8192 are netem's;
//   * the refusal conditions are restated for the chains, the process, the gate and the grants; a refused call writes
//     nothing;
//   * every delay lies between the floor and the ceiling, and the same seed and stream repeat a process exactly;
//   * a slot's clock never moves unless a word was drawn, the gate is open or the slot's budget ran out as restated;
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
using crd::i16;
using crd::i32;
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

// A second reader of the same words, for the restated machine.
struct Replay
{
    Input words;

    u32 next() { return words.next_u32(); }
};

// The correlated word restated: the new word moved toward the last answer by (rho + 1) / 2^32 of the distance between
// them, rounded down. Moving up is the floor of the product; moving down takes its ceiling off the word. Both products
// fit in 64 bits.
u32 pulled_toward(u32 word, u32 last, u32 rho)
{
    const u64 weight = u64{rho} + 1U;
    if (last >= word)
    {
        return static_cast<u32>(word + ((weight * (last - word)) >> 32U));
    }
    const u64 product = weight * (word - last);
    return static_cast<u32>(word - ((product + 0xFFFFFFFFU) >> 32U));
}

void hold_random(Input& input)
{
    const u32 chance = input.read<u32>();
    const u32 rho = input.read<u32>();
    Replay replay{input};
    LossModel model;
    model.set_random(chance, rho, input);
    u32 last = replay.next();
    for (u32 packet = 0U; packet < 256U && input.size != 0U; ++packet)
    {
        bool expected = false;
        if (chance != 0U)
        {
            u32 word = replay.next();
            if (rho != 0U)
            {
                word = pulled_toward(word, last, rho);
                last = word;
            }
            expected = word <= chance;
        }
        CRD_FUZZ_REQUIRE(model.next(input) == expected);
        CRD_FUZZ_REQUIRE(input.used_words == replay.words.used_words);
    }
}

void hold_gilbert_elliott(Input& input)
{
    const GilbertElliottChances chances = input.read<GilbertElliottChances>();
    Replay replay{input};
    LossModel model;
    model.set_gilbert_elliott(chances);
    bool good = true;
    for (u32 packet = 0U; packet < 256U && input.size != 0U; ++packet)
    {
        const u32 move = replay.next();
        const u32 fate = replay.next();
        const bool expected = good ? fate < chances.lost_good : fate > chances.survive_bad;
        const bool moves = good ? move < chances.p : move < chances.r;
        CRD_FUZZ_REQUIRE(model.next(input) == expected);
        good = moves ? !good : good;
        CRD_FUZZ_REQUIRE(input.used_words == replay.words.used_words);
    }
}

// Where the four-state chain sends a word, as data: in state `at`, a word from `from` up to but not including `until`
// moves to state `to` and loses the packet or not. A word in no band of its state stays and loses nothing. The states
// are numbered from 0: received in a gap, received in a burst, lost in a burst, lost in a gap.
struct Band
{
    usize at;
    u64 from;
    u64 until;
    usize to;
    bool lost;
};

void hold_four_state(Input& input)
{
    const FourStateChances chances = input.read<FourStateChances>();
    const u64 top = u64{1} << 32U;
    const u64 gap_sum = u64{chances.p13} + chances.p14;
    const u64 burst_sum = u64{chances.p31} + chances.p32;
    const bool valid = gap_sum <= top && burst_sum <= top;
    // A word equal to a window's lower edge falls in no band, so the next band opens one above it.
    const Band bands[] = {{0U, 0U, chances.p14, 3U, true},
                          {0U, u64{chances.p14} + 1U, gap_sum, 2U, true},
                          {1U, 0U, chances.p23, 2U, true},
                          {2U, 0U, chances.p32, 1U, false},
                          {2U, u64{chances.p32} + 1U, burst_sum, 0U, false},
                          {2U, burst_sum + 1U, top, 2U, true},
                          {3U, 0U, top, 0U, false}};
    Replay replay{input};
    LossModel model;
    CRD_FUZZ_REQUIRE(model.set_four_state(chances) == valid);
    usize at = 0U;
    for (u32 packet = 0U; packet < 256U && input.size != 0U; ++packet)
    {
        bool expected = false;
        if (valid)
        {
            const u64 word = replay.next();
            for (const Band& band : bands)
            {
                if (band.at == at && band.from <= word && word < band.until)
                {
                    at = band.to;
                    expected = band.lost;
                    break;
                }
            }
        }
        CRD_FUZZ_REQUIRE(model.next(input) == expected);
        CRD_FUZZ_REQUIRE(input.used_words == replay.words.used_words);
    }
}

void hold_gilbert_meaning(Input& input)
{
    const f64 loss = input.read<f64>();
    const f64 second = input.read<f64>();
    const GilbertElliottChances sentinel{1U, 2U, 3U, 4U};

    GilbertElliottChances burst = sentinel;
    const f64 r = 1.0 / second;
    const f64 p = loss * r / (1.0 - loss);
    const bool burst_valid = loss >= 0.0 && loss < 1.0 && second >= 1.0 && p <= 1.0;
    CRD_FUZZ_REQUIRE(gilbert_from_rate_and_burst(loss, second, burst) == burst_valid);
    CRD_FUZZ_REQUIRE(burst_valid ? (burst.survive_bad == 0U && burst.lost_good == 0U) : burst.p == 1U);

    GilbertElliottChances correlated = sentinel;
    const bool correlated_valid = loss >= 0.0 && loss < 1.0 && second >= 0.0 && second < 1.0;
    CRD_FUZZ_REQUIRE(gilbert_from_rate_and_correlation(loss, second, correlated) == correlated_valid);
    CRD_FUZZ_REQUIRE(correlated_valid ? correlated.survive_bad == 0U : correlated.p == 1U);

    const u32 chance = link_chance(loss);
    if (loss >= 0.0 && loss <= 1.0)
    {
        CRD_FUZZ_REQUIRE(chance == static_cast<u32>(std::rint(loss * 4294967295.0)));
    }
    else
    {
        CRD_FUZZ_REQUIRE(chance == (loss > 1.0 ? 0xFFFFFFFFU : 0U));
    }
}

void hold_table_delay(Input& input)
{
    const i64 mean = input.read<i64>();
    const i32 deviation = input.read<i32>();
    const u32 rho = input.read<u32>();
    i16 table[32];
    const usize size = input.read<u8>() % 33U;
    for (usize i = 0U; i < size; ++i)
    {
        table[i] = input.read<i16>();
    }
    const crd::containers::ConstSpan<i16> entries{table, size};
    Replay replay{input};
    CorrelatedDraw draw;
    draw.init(rho, input);
    u32 last = replay.next();
    // Every sum below fits: the mean is held within 2^62 and the rest is within 2^34.
    const i64 lower = mean < -kMaxLinkDelayNs ? -kMaxLinkDelayNs : mean;
    const i64 held = lower > kMaxLinkDelayNs ? kMaxLinkDelayNs : lower;
    for (u32 packet = 0U; packet < 64U && input.size != 0U; ++packet)
    {
        const i64 answer = netem_delay(mean, deviation, draw, input, entries);
        i64 expected = held;
        if (deviation > 0)
        {
            u32 word = replay.next();
            if (rho != 0U)
            {
                word = pulled_toward(word, last, rho);
                last = word;
            }
            if (size == 0U)
            {
                const i64 low_end = held - deviation;
                expected = low_end + static_cast<i64>(word % (u64{2} * static_cast<u64>(deviation)));
            }
            else
            {
                // The deviation in whole scales times the entry, plus the remainder's product rounded to the nearest
                // multiple of the scale in magnitude, halves away from zero, with its sign put back.
                const i64 entry = table[word % size];
                const i64 whole = (deviation / kNetemTableScale) * entry;
                const i64 product = (deviation % kNetemTableScale) * entry;
                const i64 magnitude = product < 0 ? -product : product;
                const i64 nearest = (magnitude + (kNetemTableScale / 2)) / kNetemTableScale;
                expected = held + whole + (product < 0 ? -nearest : nearest);
            }
        }
        CRD_FUZZ_REQUIRE(answer == expected);
        CRD_FUZZ_REQUIRE(input.used_words == replay.words.used_words);
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

void hold_slot(Input& input)
{
    SlotOptions options;
    options.min_delay_ns = input.read<i64>();
    options.max_delay_ns = input.read<i64>();
    options.max_packets = input.read<i32>();
    options.max_bytes = input.read<i32>();
    options.dist_mean_ns = input.read<i64>();
    options.dist_deviation_ns = input.read<i32>();
    i16 table[8];
    const usize size = input.read<u8>() % 9U;
    for (usize i = 0U; i < size; ++i)
    {
        table[i] = input.read<i16>();
    }
    options.table = crd::containers::ConstSpan<i16>{table, size};
    i64 now = input.read<i32>();

    const bool valid = options.min_delay_ns >= 0 && options.max_delay_ns >= options.min_delay_ns &&
                       options.max_packets >= 0 && options.max_bytes >= 0 && options.dist_deviation_ns >= 0;
    SlotGate gate;
    CRD_FUZZ_REQUIRE(gate.init(options, now) == valid);
    if (!valid)
    {
        CRD_FUZZ_REQUIRE(!gate.active());
        return;
    }
    const bool active = options.min_delay_ns != 0 || options.max_delay_ns != 0 || options.dist_deviation_ns != 0;
    CRD_FUZZ_REQUIRE(gate.active() == active);
    for (u32 packet = 0U; packet < 64U && input.size != 0U; ++packet)
    {
        now += input.read<u8>();
        const i64 due = now + static_cast<i64>(input.read<u8>()) - 128;
        const u32 bytes = input.read<u32>();
        const i64 before = gate.next_open_ns();
        const usize words_before = input.used_words;
        const bool left = gate.admit(now, due, bytes, input);
        // A slot over by its due time was replaced, or the gate was open, or the head left within it.
        CRD_FUZZ_REQUIRE(!left || due <= now);
        if (!active)
        {
            CRD_FUZZ_REQUIRE(left == (due <= now));
            CRD_FUZZ_REQUIRE(input.used_words == words_before);
        }
        else if (options.table.empty())
        {
            // A new slot draws one word; a table with no deviation has its mean and draws none, so only here is the
            // word the sign of a new slot.
            if (input.used_words != words_before)
            {
                const i64 gap = gate.next_open_ns() - now;
                CRD_FUZZ_REQUIRE(gap >= options.min_delay_ns && gap <= options.max_delay_ns);
            }
            else
            {
                CRD_FUZZ_REQUIRE(gate.next_open_ns() == before);
            }
        }
    }
}

// Builds a seed in the order the modes read it, then ends it with words.
class Seed
{
public:
    explicit Seed(u8 mode) { m_bytes[0] = mode; }

    Seed& u8s(std::initializer_list<u8> values) { return put(values); }
    Seed& i16s(std::initializer_list<i16> values) { return put(values); }
    Seed& i32s(std::initializer_list<i32> values) { return put(values); }
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
    switch (data[0] % 8U)
    {
        case 0U:
            hold_random(input);
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
            hold_table_delay(input);
            break;
        case 5U:
            hold_process(input);
            break;
        case 6U:
            hold_slot(input);
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
    Seed(0U).u32s({42949673U, 0U, 7U}).words(sink, "random");
    Seed(0U).u32s({0xFFFFFFFFU, 0xFFFFFFFFU, 7U}).words(sink, "random-full");
    Seed(0U).u32s({429496729U, 1073741824U, 7U}).words(sink, "random-correlated");
    Seed(1U).u32s({214748365U, 1288490189U, 0U, 0U}).words(sink, "gilbert");
    Seed(1U).u32s({100U, 200U, 300U, 400U}).near(sink, "gilbert-thresholds", {99U, 100U, 200U, 300U, 301U, 399U, 400U});
    Seed(2U).u32s({85899346U, 1717986918U, 1288490189U, 858993459U, 21474836U}).words(sink, "four-state");
    Seed(2U)
        .u32s({100U, 100U, 300U, 50U, 100U})
        .near(sink, "four-state-thresholds", {49U, 50U, 99U, 100U, 200U, 300U, 400U});
    Seed(2U).u32s({0xFFFFFFFFU, 0U, 0U, 0U, 2U}).end(sink, "four-state-past-one");
    Seed(2U).u32s({2147483648U, 2147483648U, 2147483648U, 0U, 2147483648U}).words(sink, "four-state-one");
    Seed(3U).f64s({0.03, 4.0}).end(sink, "meaning-burst");
    Seed(3U).f64s({0.05, 0.25}).end(sink, "meaning-correlation");
    Seed(3U).f64s({std::numeric_limits<f64>::quiet_NaN(), 2.0}).end(sink, "meaning-nan");
    Seed(3U).f64s({0.9, 1.0}).end(sink, "meaning-entry-past-one");
    Seed(4U).i64s({40000000}).i32s({8000000}).u32s({0U}).u8s({0U}).words(sink, "delay-uniform");
    Seed(4U)
        .i64s({40000000})
        .i32s({10000})
        .u32s({1073741824U})
        .u8s({4U})
        .i16s({-16384, -1, 8192, 32767})
        .words(sink, "delay-table");
    Seed(4U).i64s({-far}).i32s({std::numeric_limits<i32>::max()}).u32s({0U}).u8s({0U}).words(sink, "delay-extremes");
    const auto process = [far](u8 shape, i64 mean, i64 deviation, f64 correlation, f64 tail) -> Seed
    {
        Seed seed(5U);
        seed.u8s({shape}).i64s({mean, deviation}).f64s({correlation, tail, 0.75}).i64s({0, far});
        return seed;
    };
    process(1U, 40000000, 8000000, 0.5, 3.0).u8s({0U}).i64s({1, 2}).end(sink, "process-normal");
    process(2U, 40000000, 8000000, 0.0, 3.0).u8s({0U}).i64s({3, 4}).end(sink, "process-log-normal");
    process(3U, 40000000, kMaxLinkDelayNs, 0.99, 2.0001).u8s({0U}).i64s({5, 6}).end(sink, "process-heavy");
    process(4U, 40000000, 8000000, 0.9, 3.0).u8s({0U}).i64s({7, 8}).end(sink, "process-mix");
    process(5U, 0, 0, 0.5, 3.0).u8s({4U}).i64s({10, 20, 20, 90, 9, 10}).end(sink, "process-table");
    process(5U, 0, 0, 0.0, 3.0).u8s({2U}).i64s({20, 10, 9, 10}).end(sink, "process-table-descending");
    process(0U, 0, 1, 1.0, 3.0).u8s({0U}).i64s({0, 0}).end(sink, "process-refused");
    Seed(6U).i64s({1000, 3000}).i32s({2, 0}).i64s({0}).i32s({0}).u8s({0U}).i32s({100}).words(sink, "slot-packets");
    Seed(6U).i64s({10, 10}).i32s({0, 3000}).i64s({0}).i32s({0}).u8s({0U}).i32s({0}).words(sink, "slot-bytes");
    Seed(6U)
        .i64s({0, 0})
        .i32s({1, 0})
        .i64s({500})
        .i32s({100})
        .u8s({2U})
        .i16s({8192, 16384})
        .i32s({0})
        .words(sink, "slot-table");
    Seed(6U).i64s({0, far}).i32s({1, 0}).i64s({0}).i32s({0}).u8s({0U}).i32s({2147483647}).words(sink, "slot-far");
    Seed(6U).i64s({-1, 0}).i32s({0, 0}).i64s({0}).i32s({0}).u8s({0U}).i32s({0}).end(sink, "slot-refused");
    const auto gap = [](i64 mean, i64 deviation) -> Seed
    {
        Seed seed(7U);
        seed.u8s({0U}).i64s({mean, deviation}).f64s({0.0, 3.0, 0.75}).i64s({0, kMaxLinkDelayNs}).u8s({0U});
        return seed;
    };
    gap(1000, 300).i64s({0}).u32s({2U}).u64s({0U}).i64s({0}).u64s({1U}).words(sink, "grants-packets");
    gap(10, 0).i64s({0}).u32s({0U}).u64s({3000U}).i64s({0}).u64s({2U}).words(sink, "grants-bytes");
    gap(500, 100).i64s({2000}).u32s({0U}).u64s({0U}).i64s({0}).u64s({3U}).words(sink, "grants-hold");
    gap(kMaxLinkDelayNs, 0).i64s({0}).u32s({1U}).u64s({0U}).i64s({far - 10}).u64s({4U}).words(sink, "grants-far");
    gap(10, 0).i64s({-1}).u32s({0U}).u64s({0U}).i64s({0}).u64s({5U}).end(sink, "grants-refused");
}
