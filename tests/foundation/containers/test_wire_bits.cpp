// crd-containers -- bit-packed fields over byte spans (wire_bits.hpp): every width at every bit offset against a model
// of the bits, refused takes that consume nothing, padding, viewing byte ranges, and a writer that never writes past
// its span and keeps measuring.

#include <crd/containers/wire_bits.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::BitReader;
using crd::containers::BitWriter;
using crd::containers::ConstSpan;
using crd::containers::Span;

namespace
{
// A SplitMix64 step: the tests' own bits.
u64 next_bits(u64& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    u64 z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

u64 masked(u64 value, u32 bits)
{
    return (bits == 64U) ? value : (value & ((u64{1} << bits) - 1U));
}

// The bit at `at` of `bytes`, the most significant first.
u32 model_bit(const u8* bytes, u64 at)
{
    return (bytes[at / 8U] >> (7U - (at % 8U))) & 1U;
}

u64 model_read(const u8* bytes, u64 at, u32 bits)
{
    u64 value = 0U;
    for (u32 i = 0U; i < bits; ++i)
    {
        value = (value << 1U) | model_bit(bytes, at + i);
    }
    return value;
}
} // namespace

TEST_CASE("BitWriter and BitReader agree with a model at every width and bit offset", "[containers][wire-bits]")
{
    u64 state = 1U;
    for (u32 offset = 0U; offset < 16U; ++offset)
    {
        for (u32 bits = 1U; bits <= 64U; ++bits)
        {
            u8 out[32] = {};
            BitWriter writer{Span<u8>(out)};
            const u64 lead = masked(next_bits(state), offset);
            if (offset != 0U)
            {
                writer.put(lead, offset);
            }
            const u64 value = masked(next_bits(state), bits);
            writer.put(value, bits);
            CHECK(writer.bit_position() == offset + bits);
            const usize size = writer.finish();
            CHECK(size == (offset + bits + 7U) / 8U);
            CHECK_FALSE(writer.overflowed());
            CHECK(model_read(out, offset, bits) == value);
            for (u64 at = offset + bits; at < size * 8U; ++at)
            {
                CHECK(model_bit(out, at) == 0U);
            }

            // Tight to the last byte of the message, and with room behind it: the two read paths.
            for (const usize span : {size, usize{32}})
            {
                BitReader reader{ConstSpan<u8>(out, span)};
                u64 taken = 0U;
                if (offset != 0U)
                {
                    REQUIRE(reader.take(offset, taken));
                    CHECK(taken == lead);
                }
                REQUIRE(reader.take(bits, taken));
                CHECK(taken == value);
                CHECK(reader.bit_position() == offset + bits);
                CHECK(reader.bits_left() == (span * 8U) - offset - bits);
            }
        }
    }
}

TEST_CASE("A long run of mixed fields reads back at any span", "[containers][wire-bits]")
{
    u8 out[2048] = {};
    BitWriter writer{Span<u8>(out)};
    u64 state = 7U;
    u32 widths[1024] = {};
    u64 values[1024] = {};
    for (usize i = 0U; i < 1024U; ++i)
    {
        widths[i] = static_cast<u32>(next_bits(state) % 64U) + 1U;
        values[i] = masked(next_bits(state), widths[i]);
        if (writer.bit_position() + widths[i] > 2048U * 8U)
        {
            widths[i] = 0U;
            continue;
        }
        writer.put(values[i], widths[i]);
    }
    const u64   position = writer.bit_position();
    const usize size     = writer.finish();
    REQUIRE_FALSE(writer.overflowed());
    CHECK(size == (position + 7U) / 8U);
    BitReader reader{ConstSpan<u8>(out, size)};
    u64       at = 0U;
    for (usize i = 0U; i < 1024U; ++i)
    {
        if (widths[i] == 0U)
        {
            continue;
        }
        u64 taken = 0U;
        REQUIRE(reader.take(widths[i], taken));
        CHECK(taken == values[i]);
        CHECK(model_read(out, at, widths[i]) == values[i]);
        at += widths[i];
    }
    CHECK(reader.bits_left() == (size * 8U) - position);
}

TEST_CASE("A take past the end reports it and consumes nothing", "[containers][wire-bits]")
{
    const u8  bytes[2] = {0xABU, 0xCDU};
    BitReader reader{ConstSpan<u8>(bytes)};
    u64       value = 77U;
    CHECK_FALSE(reader.take(17U, value));
    CHECK(value == 77U);
    CHECK(reader.bit_position() == 0U);
    REQUIRE(reader.take(12U, value));
    CHECK(value == 0xABCU);
    CHECK_FALSE(reader.take(5U, value));
    CHECK(reader.bit_position() == 12U);
    REQUIRE(reader.take(4U, value));
    CHECK(value == 0xDU);
    CHECK_FALSE(reader.take(1U, value));
    CHECK(reader.bits_left() == 0U);

    BitReader empty{ConstSpan<u8>()};
    CHECK_FALSE(empty.take(1U, value));
    CHECK(empty.align());
}

TEST_CASE("Fields read as an RFC's diagram does", "[containers][wire-bits]")
{
    u8        out[4] = {};
    BitWriter writer{Span<u8>(out)};
    writer.put(0b101U, 3U);
    writer.put(0x1FU, 5U);
    writer.put(0x1234U, 16U);
    writer.put(1U, 1U);
    CHECK(writer.finish() == 4U);
    CHECK(out[0] == 0b10111111U);
    CHECK(out[1] == 0x12U);
    CHECK(out[2] == 0x34U);
    CHECK(out[3] == 0x80U);
}

TEST_CASE("Alignment pads with zeros and a set padding bit is refused", "[containers][wire-bits]")
{
    u8        out[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
    BitWriter writer{Span<u8>(out)};
    writer.align();
    CHECK(writer.bit_position() == 0U);
    writer.put(1U, 1U);
    writer.align();
    CHECK(writer.bit_position() == 8U);
    writer.align();
    CHECK(writer.bit_position() == 8U);
    CHECK(writer.finish() == 1U);
    CHECK(out[0] == 0x80U);

    const u8  clean[1] = {0x80U};
    BitReader good{ConstSpan<u8>(clean)};
    u64       value = 0U;
    REQUIRE(good.take(1U, value));
    CHECK(good.align());
    CHECK(good.bit_position() == 8U);
    CHECK(good.align());

    const u8  dirty[1] = {0x81U};
    BitReader bad{ConstSpan<u8>(dirty)};
    REQUIRE(bad.take(1U, value));
    CHECK_FALSE(bad.align());
    CHECK(bad.bit_position() == 1U);
}

TEST_CASE("Byte fields view the input and write through", "[containers][wire-bits]")
{
    const u8  payload[5] = {1U, 2U, 3U, 4U, 5U};
    u8        out[16]    = {};
    BitWriter writer{Span<u8>(out)};
    writer.put(0b11U, 2U);
    writer.align();
    writer.put_bytes(ConstSpan<u8>(payload));
    writer.put(0xFFU, 8U);
    writer.put_bytes(ConstSpan<u8>());
    writer.put_bytes(ConstSpan<u8>(payload, 2U));
    const usize size = writer.finish();
    REQUIRE(size == 9U);

    BitReader reader{ConstSpan<u8>(out, size)};
    u64       value = 0U;
    REQUIRE(reader.take(2U, value));
    REQUIRE(reader.align());
    ConstSpan<u8> bytes;
    REQUIRE(reader.take_bytes(5U, bytes));
    CHECK(bytes.data() == out + 1U);
    CHECK(std::memcmp(bytes.data(), payload, 5U) == 0);
    REQUIRE(reader.take(8U, value));
    CHECK(value == 0xFFU);
    CHECK_FALSE(reader.take_bytes(3U, bytes));
    CHECK(reader.bit_position() == 56U);
    BitReader inner;
    REQUIRE(reader.sub_bytes(2U, inner));
    CHECK(inner.bits_left() == 16U);
    REQUIRE(inner.take(8U, value));
    CHECK(value == 1U);
    CHECK(reader.bits_left() == 0U);
    CHECK_FALSE(reader.sub_bytes(1U, inner));
}

TEST_CASE("skip moves within the input only", "[containers][wire-bits]")
{
    const u8  bytes[2] = {0x00U, 0x01U};
    BitReader reader{ConstSpan<u8>(bytes)};
    CHECK_FALSE(reader.skip(17U));
    CHECK(reader.bit_position() == 0U);
    REQUIRE(reader.skip(15U));
    u64 value = 0U;
    REQUIRE(reader.take(1U, value));
    CHECK(value == 1U);
}

TEST_CASE("A writer never writes past its span, reports it and keeps measuring", "[containers][wire-bits]")
{
    for (usize room = 0U; room <= 12U; ++room)
    {
        u8 out[32];
        std::memset(out, 0xEEU, sizeof(out));
        BitWriter writer{Span<u8>(out, room)};
        writer.put(0xFFFFFFFFFFFFFFFFULL, 64U);
        writer.put(0x3U, 2U);
        writer.align();
        const u8 tail[3] = {9U, 9U, 9U};
        writer.put_bytes(ConstSpan<u8>(tail));
        const usize size = writer.finish();
        CHECK(size == 12U);
        CHECK(writer.bit_position() == 96U);
        CHECK(writer.overflowed() == (room < 12U));
        for (usize i = room; i < sizeof(out); ++i)
        {
            CHECK(out[i] == 0xEEU);
        }
    }
    u8        one[1] = {};
    BitWriter fits{Span<u8>(one)};
    fits.put(1U, 8U);
    CHECK(fits.finish() == 1U);
    CHECK_FALSE(fits.overflowed());
    CHECK(one[0] == 1U);
}
