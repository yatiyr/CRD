// crd-compression's entropy coder (NET.3): known answers from a restatement in Python, what every refusal refuses, and
// a coded message's one encoding.

#include <crd/compression/entropy.hpp>
#include <crd/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <generated/entropy_vectors.inc>
#include <random>

using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::compression::EntropyModel;
using crd::compression::EntropyTrainer;
using crd::compression::kEntropyContexts;
using crd::compression::kEntropyModelBytes;
using crd::compression::kEntropyScale;
using crd::compression::kEntropyStateBytes;
using crd::compression::kEntropySymbols;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;

namespace
{

// A local model object is 20 KiB, past what these tests' frames should hold.
const EntropyModel& known_model()
{
    static EntropyModel model;
    static const bool kLoaded = model.load(kModelSource);
    REQUIRE(kLoaded);
    return model;
}

[[nodiscard]] u32 frequency_at(ConstSpan<u8> source, usize context, usize symbol)
{
    const usize at = ((context * kEntropySymbols) + symbol) * 2U;
    return u32{source[at]} | (u32{source[at + 1U]} << 8U);
}

[[nodiscard]] usize context_of(u8 byte)
{
    if (byte == 0U)
    {
        return 0U;
    }
    if (byte < 16U)
    {
        return 1U;
    }
    return ((byte & 15U) == 0U) ? 2U : 3U;
}

// What the model says these bytes are worth, in bits.
[[nodiscard]] double model_cost(ConstSpan<u8> source, ConstSpan<u8> data)
{
    double bits = 0.0;
    usize context = 0U;
    for (const u8 byte : data)
    {
        bits -= std::log2(static_cast<double>(frequency_at(source, context, byte)) / kEntropyScale);
        context = context_of(byte);
    }
    return bits;
}

} // namespace

TEST_CASE("Entropy coder: training gives the restatement's model", "[compression][entropy]")
{
    EntropyTrainer trainer;
    trainer.add(kTrainingSample0);
    trainer.add(kTrainingSample1);
    u8 source[kEntropyModelBytes] = {};
    trainer.write_model(source);
    CHECK(std::memcmp(source, kModelSource, kEntropyModelBytes) == 0);
    static EntropyModel model;
    CHECK(model.load(source));
}

TEST_CASE("Entropy coder: known answers, both ways, at exactly their room", "[compression][entropy]")
{
    const EntropyModel& model = known_model();
    for (const EntropyVector& vector : kVectors)
    {
        Array<u8> coded;
        coded.resize(vector.coded.size());
        REQUIRE(model.encode(vector.input, Span<u8>(coded.data(), coded.size())) == vector.coded.size());
        CHECK(std::memcmp(coded.data(), vector.coded.data(), coded.size()) == 0);

        // One byte less is no room at all.
        CHECK(model.encode(vector.input, Span<u8>(coded.data(), coded.size() - 1U)) == 0U);

        // More room than it needs writes at the start and says so.
        Array<u8> wide;
        wide.resize(vector.coded.size() + 17U);
        REQUIRE(model.encode(vector.input, Span<u8>(wide.data(), wide.size())) == vector.coded.size());
        CHECK(std::memcmp(wide.data(), vector.coded.data(), vector.coded.size()) == 0);

        Array<u8> plain;
        plain.resize(vector.input.size());
        REQUIRE(model.decode(vector.coded, Span<u8>(plain.data(), plain.size())));
        if (!plain.empty())
        {
            CHECK(std::memcmp(plain.data(), vector.input.data(), plain.size()) == 0);
        }
    }
}

TEST_CASE("Entropy coder: a coded message has one size to decode to and no spare byte", "[compression][entropy]")
{
    const EntropyModel& model = known_model();
    for (const EntropyVector& vector : kVectors)
    {
        Array<u8> plain;
        plain.resize(vector.input.size() + 1U);
        // Asked for one byte more or one fewer, it does not end on its starting state.
        CHECK_FALSE(model.decode(vector.coded, Span<u8>(plain.data(), vector.input.size() + 1U)));
        if (!vector.input.empty())
        {
            CHECK_FALSE(model.decode(vector.coded, Span<u8>(plain.data(), vector.input.size() - 1U)));
        }

        // Truncated, or with anything after it.
        CHECK_FALSE(
            model.decode(vector.coded.first(vector.coded.size() - 1U), Span<u8>(plain.data(), vector.input.size())));
        Array<u8> longer;
        longer.resize(vector.coded.size() + 1U);
        std::memcpy(longer.data(), vector.coded.data(), vector.coded.size());
        longer[vector.coded.size()] = 0U;
        CHECK_FALSE(
            model.decode(ConstSpan<u8>(longer.data(), longer.size()), Span<u8>(plain.data(), vector.input.size())));
    }

    // A state below the range no coder leaves.
    const u8 low[] = {0U, 0xFFU, 0xFFU};
    u8 none[1] = {};
    CHECK_FALSE(model.decode(low, Span<u8>(none, 0U)));
    const u8 start[] = {1U, 0U, 0U};
    CHECK(model.decode(start, Span<u8>(none, 0U)));
    const u8 past[] = {1U, 0U, 1U};
    CHECK_FALSE(model.decode(past, Span<u8>(none, 0U)));
    CHECK_FALSE(model.decode(ConstSpan<u8>(start, 2U), Span<u8>(none, 0U)));
}

TEST_CASE("Entropy coder: whatever decodes re-encodes to itself", "[compression][entropy]")
{
    const EntropyModel& model = known_model();

    // Every three bytes there are as the coding of one byte: exactly the bytes whose coding takes three bytes.
    usize expected = 0U;
    for (u32 symbol = 0U; symbol < kEntropySymbols; ++symbol)
    {
        const u8 in[1] = {static_cast<u8>(symbol)};
        u8 out[8] = {};
        if (model.encode(in, out) == kEntropyStateBytes)
        {
            ++expected;
        }
    }
    usize accepted = 0U;
    for (u32 state = 0U; state < (1U << 24U); ++state)
    {
        const u8 coded[kEntropyStateBytes] = {static_cast<u8>(state >> 16U), static_cast<u8>((state >> 8U) & 0xFFU),
                                              static_cast<u8>(state & 0xFFU)};
        u8 plain[1] = {};
        if (!model.decode(coded, plain))
        {
            continue;
        }
        ++accepted;
        u8 again[8] = {};
        REQUIRE(model.encode(plain, again) == kEntropyStateBytes);
        REQUIRE(std::memcmp(again, coded, kEntropyStateBytes) == 0);
    }
    CHECK(expected > 0U);
    CHECK(accepted == expected);

    // A state below the range decodes to a state a following byte could lift back onto the starting one: a second
    // encoding of a message, had it been let in.
    for (u32 state = 0U; state < (1U << 16U); ++state)
    {
        const u8 coded[kEntropyStateBytes + 1U] = {0U, static_cast<u8>(state >> 8U), static_cast<u8>(state & 0xFFU),
                                                   0U};
        u8 plain[1] = {};
        REQUIRE_FALSE(model.decode(coded, plain));
    }
}

TEST_CASE("Entropy coder: any message round-trips within the model's cost", "[compression][entropy]")
{
    const EntropyModel& model = known_model();
    std::minstd_rand rng(0xA11);
    Array<u8> plain;
    Array<u8> coded;
    Array<u8> back;
    for (u32 round = 0U; round < 400U; ++round)
    {
        const usize size = 1U + (rng() % 3000U);
        const u32 zero_share = static_cast<u32>(rng() % 100U);
        plain.resize(size);
        coded.resize((size * 2U) + kEntropyStateBytes);
        back.resize(size);
        for (u8& byte : plain)
        {
            byte = ((rng() % 100U) < zero_share) ? u8{0U} : static_cast<u8>(rng());
        }
        const usize written = model.encode(ConstSpan<u8>(plain.data(), size), Span<u8>(coded.data(), coded.size()));
        REQUIRE(written != 0U);
        REQUIRE(model.decode(ConstSpan<u8>(coded.data(), written), Span<u8>(back.data(), size)));
        REQUIRE(std::memcmp(back.data(), plain.data(), size) == 0);

        const double bytes = model_cost(kModelSource, ConstSpan<u8>(plain.data(), size)) / 8.0;
        CHECK(static_cast<double>(written) <= (bytes * 1.01) + static_cast<double>(kEntropyStateBytes) + 1.0);
        CHECK(static_cast<double>(written) >= (bytes * 0.99) - 2.0);
    }
}

TEST_CASE("Entropy coder: a model is a table of every symbol summing to the scale", "[compression][entropy]")
{
    static EntropyModel model;
    u8 source[kEntropyModelBytes] = {};
    std::memcpy(source, kModelSource, sizeof(source));
    u8 out[16] = {};
    const u8 in[4] = {1U, 2U, 3U, 4U};

    SECTION("the wrong size")
    {
        CHECK_FALSE(model.load(ConstSpan<u8>(source, sizeof(source) - 1U)));
        CHECK_FALSE(model.load(ConstSpan<u8>(source, 0U)));
    }
    SECTION("a frequency of zero, at the last context's last symbol or anywhere")
    {
        const usize last = sizeof(source) - 2U;
        const u32 moved = frequency_at(source, kEntropyContexts - 1U, kEntropySymbols - 1U);
        source[last] = 0U;
        source[last + 1U] = 0U;
        const u32 before = frequency_at(source, kEntropyContexts - 1U, kEntropySymbols - 2U) + moved;
        source[last - 2U] = static_cast<u8>(before & 0xFFU);
        source[last - 1U] = static_cast<u8>(before >> 8U);
        CHECK_FALSE(model.load(source));
    }
    SECTION("a context short of the scale")
    {
        CHECK(source[0] + (u32{source[1]} << 8U) > 1U);
        --source[0];
        CHECK_FALSE(model.load(source));
    }
    SECTION("a context past the scale")
    {
        ++source[0];
        CHECK_FALSE(model.load(source));
    }
    SECTION("the same table with its two contexts swapped stays a model")
    {
        u8 swapped[kEntropyModelBytes] = {};
        std::memcpy(swapped, source + (2U * kEntropySymbols), 2U * kEntropySymbols);
        std::memcpy(swapped + (2U * kEntropySymbols), source, 2U * kEntropySymbols);
        std::memcpy(swapped + (4U * kEntropySymbols), source + (4U * kEntropySymbols), 4U * kEntropySymbols);
        CHECK(model.load(swapped));
        return;
    }
    // A refused load leaves nothing to code with.
    CHECK(model.encode(in, out) == 0U);
    u8 plain[4] = {};
    const u8 start[] = {1U, 0U, 0U};
    CHECK_FALSE(model.decode(start, Span<u8>(plain, 0U)));
}

TEST_CASE("Entropy coder: training keeps every symbol, whatever the samples hold", "[compression][entropy]")
{
    const auto source_of = [](const EntropyTrainer& trainer, Span<u8> out)
    {
        trainer.write_model(out);
    };
    static EntropyModel model;
    u8 source[kEntropyModelBytes] = {};

    SECTION("nothing at all is uniform")
    {
        const EntropyTrainer trainer;
        source_of(trainer, source);
        for (usize context = 0U; context < kEntropyContexts; ++context)
        {
            for (usize symbol = 0U; symbol < kEntropySymbols; ++symbol)
            {
                CHECK(frequency_at(source, context, symbol) == kEntropyScale / kEntropySymbols);
            }
        }
    }
    SECTION("one symbol takes all but what the others must keep")
    {
        EntropyTrainer trainer;
        const u8 zeros[4096] = {};
        trainer.add(zeros);
        source_of(trainer, source);
        CHECK(frequency_at(source, 0U, 0U) == kEntropyScale - (kEntropySymbols - 1U));
        CHECK(frequency_at(source, 0U, 1U) == 1U);
    }
    SECTION("every symbol once rounds to the same table however it is reached")
    {
        EntropyTrainer trainer;
        u8 all[256] = {};
        for (usize i = 0U; i < sizeof(all); ++i)
        {
            all[i] = static_cast<u8>(i);
        }
        trainer.add(all);
        source_of(trainer, source);
    }
    SECTION("more symbols at the floor than the scale can spare comes back from the largest")
    {
        EntropyTrainer trainer;
        Array<u8> skew;
        skew.resize(200000U);
        for (usize i = 0U; i < skew.size(); ++i)
        {
            // Half the symbols a little above nothing, so their floor passes their share.
            skew[i] = (i % 1000U < 200U) ? static_cast<u8>(16U * (1U + (i % 15U))) : u8{16U};
        }
        trainer.add(ConstSpan<u8>(skew.data(), skew.size()));
        source_of(trainer, source);
    }
    CHECK(model.load(source));
    for (usize context = 0U; context < kEntropyContexts; ++context)
    {
        u32 sum = 0U;
        for (usize symbol = 0U; symbol < kEntropySymbols; ++symbol)
        {
            const u32 frequency = frequency_at(source, context, symbol);
            CHECK(frequency >= 1U);
            sum += frequency;
        }
        CHECK(sum == kEntropyScale);
    }
}
