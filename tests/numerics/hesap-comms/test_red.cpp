// HESAP-NET's redundant audio data (RFC 2198): §7's example packet byte by byte, the field bounds of §3's header,
// timestamps across the wrap, several levels of redundancy, a primary alone, random round trips, and every refusal
// in its order with nothing written.

#include <crd/containers/array.hpp>
#include <crd/hesap/comms/red.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <initializer_list>
#include <span>

namespace hc = crd::hesap::comms;
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using hc::RedBlock;
using hc::RedStatus;
using Bytes = crd::containers::Array<u8>;

namespace
{

constexpr u8 kSentinel = 0xA5U;

Bytes bytes_of(std::initializer_list<u8> list)
{
    Bytes v;
    for (const u8 b : list)
    {
        v.push_back(b);
    }
    return v;
}

Bytes counting(usize n, u8 first)
{
    Bytes v;
    for (usize i = 0U; i < n; ++i)
    {
        v.push_back(static_cast<u8>(first + i));
    }
    return v;
}

bool same(std::span<const u8> a, std::span<const u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

RedBlock redundant(u8 type, u16 offset, const Bytes& data)
{
    RedBlock b;
    b.payload_type = type;
    b.offset = offset;
    b.data = std::span<const u8>(data.data(), data.size());
    return b;
}

bool untouched(const RedBlock& b)
{
    return b.payload_type == 0x55U && b.offset == 0x5555U && b.timestamp == 0x55555555U && b.data.empty();
}

RedBlock marked()
{
    RedBlock b;
    b.payload_type = 0x55U;
    b.offset = 0x5555U;
    b.timestamp = 0x55555555U;
    return b;
}

RedStatus parse(const Bytes& payload, u32 timestamp, std::span<RedBlock> blocks, u32& count)
{
    return hc::red_parse(std::span<const u8>(payload.data(), payload.size()), timestamp, blocks, count);
}

} // namespace

TEST_CASE("RED: section 7's DVI4 packet with one LPC redundancy is written and read byte for byte", "[hesap][red]")
{
    // A DVI4 (PT 5) primary of 84 bytes and an LPC (PT 7) block of 14 bytes, both 20 ms at 8 kHz: the redundancy is
    // the previous packet's interval, 160 samples back. Header: 1|7, then offset 160 and length 14 in 24 bits
    // (160·2^10 + 14 = 0x02800E), then 0|5.
    const Bytes lpc = counting(14U, 0x10U);
    const Bytes dvi = counting(84U, 0x40U);
    const RedBlock blocks[] = {redundant(7U, 160U, lpc)};
    Bytes out;
    out.resize(5U + 14U + 84U + 8U, kSentinel);
    usize written = 0U;
    REQUIRE(hc::red_encode(blocks, 5U, std::span<const u8>(dvi.data(), dvi.size()),
                           std::span<u8>(out.data(), out.size()), written) == RedStatus::Ok);
    REQUIRE(written == 5U + 14U + 84U);
    CHECK(hc::red_bytes(blocks, dvi.size()) == written);
    const u8 header[] = {0x87U, 0x02U, 0x80U, 0x0EU, 0x05U};
    CHECK(std::memcmp(out.data(), header, sizeof(header)) == 0);
    CHECK(std::memcmp(out.data() + 5U, lpc.data(), lpc.size()) == 0);
    CHECK(std::memcmp(out.data() + 19U, dvi.data(), dvi.size()) == 0);
    CHECK(out[written] == kSentinel);

    RedBlock read[4];
    u32 count = 0U;
    REQUIRE(hc::red_parse(std::span<const u8>(out.data(), written), 48000U, read, count) == RedStatus::Ok);
    REQUIRE(count == 2U);
    CHECK((read[0].payload_type == 7U && read[0].offset == 160U && read[0].timestamp == 48000U - 160U));
    CHECK(same(read[0].data, std::span<const u8>(lpc.data(), lpc.size())));
    CHECK(read[0].data.data() == out.data() + 5U); // the blocks view the payload
    CHECK((read[1].payload_type == 5U && read[1].offset == 0U && read[1].timestamp == 48000U));
    CHECK(same(read[1].data, std::span<const u8>(dvi.data(), dvi.size())));
}

TEST_CASE("RED: a primary alone is one header byte, and may be empty", "[hesap][red]")
{
    const Bytes media = counting(3U, 1U);
    u8 out[4] = {};
    usize written = 0U;
    REQUIRE(hc::red_encode({}, 111U, std::span<const u8>(media.data(), media.size()), out, written) == RedStatus::Ok);
    CHECK(written == 4U);
    CHECK((out[0] == 111U && out[1] == 1U && out[3] == 3U));

    RedBlock read[1];
    u32 count = 0U;
    REQUIRE(parse(bytes_of({0x6FU}), 7U, read, count) == RedStatus::Ok);
    CHECK((count == 1U && read[0].payload_type == 0x6FU && read[0].timestamp == 7U && read[0].data.empty()));
    REQUIRE(hc::red_encode({}, 0U, {}, std::span<u8>(out, 1U), written) == RedStatus::Ok);
    CHECK((written == 1U && out[0] == 0U));
}

TEST_CASE("RED: the header's fields reach their bounds and timestamps wrap", "[hesap][red]")
{
    const Bytes longest = counting(RedBlock::kMaxLength, 0U);
    const Bytes empty;
    const Bytes media = counting(9U, 0xC0U);
    const RedBlock blocks[] = {redundant(127U, RedBlock::kMaxOffset, longest), redundant(0U, 1U, empty)};
    Bytes out;
    out.resize(hc::red_bytes(blocks, media.size()), kSentinel);
    usize written = 0U;
    REQUIRE(hc::red_encode(blocks, 127U, std::span<const u8>(media.data(), media.size()),
                           std::span<u8>(out.data(), out.size()), written) == RedStatus::Ok);
    REQUIRE(written == 4U + 4U + 1U + 1023U + 9U);
    const u8 header[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x80U, 0x00U, 0x04U, 0x00U, 0x7FU};
    CHECK(std::memcmp(out.data(), header, sizeof(header)) == 0);

    RedBlock read[3];
    u32 count = 0U;
    REQUIRE(parse(out, 100U, read, count) == RedStatus::Ok);
    REQUIRE(count == 3U);
    CHECK((read[0].payload_type == 127U && read[0].offset == RedBlock::kMaxOffset));
    CHECK(read[0].timestamp == 100U - RedBlock::kMaxOffset); // modulo 2^32: 0xFFFFC065
    CHECK(read[0].timestamp == 0xFFFFC065U);
    CHECK(same(read[0].data, std::span<const u8>(longest.data(), longest.size())));
    CHECK((read[1].offset == 1U && read[1].timestamp == 99U && read[1].data.empty()));
    CHECK(read[1].data.data() == out.data() + 9U + 1023U);
    CHECK(same(read[2].data, std::span<const u8>(media.data(), media.size())));
}

TEST_CASE("RED: several levels of redundancy keep their order", "[hesap][red]")
{
    const Bytes a = counting(5U, 0x20U);
    const Bytes b = counting(3U, 0x30U);
    const Bytes c = counting(1U, 0x40U);
    const Bytes media = counting(30U, 0x50U);
    const RedBlock blocks[] = {redundant(9U, 960U, a), redundant(10U, 640U, b), redundant(11U, 320U, c)};
    u8 out[64];
    usize written = 0U;
    REQUIRE(hc::red_encode(blocks, 12U, std::span<const u8>(media.data(), media.size()), out, written) ==
            RedStatus::Ok);
    RedBlock read[4];
    u32 count = 0U;
    REQUIRE(hc::red_parse(std::span<const u8>(out, written), 1000U, read, count) == RedStatus::Ok);
    REQUIRE(count == 4U);
    const Bytes* const data[] = {&a, &b, &c, &media};
    const u32 timestamps[] = {40U, 360U, 680U, 1000U};
    for (u32 i = 0U; i < 4U; ++i)
    {
        CHECK(read[i].payload_type == 9U + i);
        CHECK(read[i].timestamp == timestamps[i]);
        CHECK(same(read[i].data, std::span<const u8>(data[i]->data(), data[i]->size())));
    }
}

TEST_CASE("RED: random packets round-trip through the writer and the reader", "[hesap][red]")
{
    crd::hesap::stats::PhiloxRng rng(2198, 1);
    for (u32 round = 0U; round < 500U; ++round)
    {
        const u32 levels = rng.next_u32() % 6U;
        Bytes data[5];
        RedBlock blocks[5];
        for (u32 i = 0U; i < levels; ++i)
        {
            data[i] = counting(rng.next_u32() % (RedBlock::kMaxLength + 1U), static_cast<u8>(rng.next_u32()));
            const u16 offset = static_cast<u16>(1U + (i * 3277U) + (rng.next_u32() % 3277U)); // distinct, <= 16383
            blocks[i] = redundant(static_cast<u8>(rng.next_u32() & 0x7FU), offset, data[i]);
        }
        const Bytes media = counting(rng.next_u32() % 1500U, static_cast<u8>(rng.next_u32()));
        const u8 type = static_cast<u8>(rng.next_u32() & 0x7FU);
        const std::span<const RedBlock> list(blocks, levels);
        Bytes out;
        out.resize(hc::red_bytes(list, media.size()));
        usize written = 0U;
        REQUIRE(hc::red_encode(list, type, std::span<const u8>(media.data(), media.size()),
                               std::span<u8>(out.data(), out.size()), written) == RedStatus::Ok);
        REQUIRE(written == out.size());
        RedBlock read[6];
        u32 count = 0U;
        const u32 timestamp = rng.next_u32();
        REQUIRE(parse(out, timestamp, read, count) == RedStatus::Ok);
        REQUIRE(count == levels + 1U);
        for (u32 i = 0U; i < levels; ++i)
        {
            CHECK((read[i].payload_type == blocks[i].payload_type && read[i].offset == blocks[i].offset));
            CHECK(read[i].timestamp == timestamp - blocks[i].offset);
            CHECK(same(read[i].data, blocks[i].data));
        }
        CHECK((read[levels].payload_type == type && read[levels].timestamp == timestamp));
        CHECK(same(read[levels].data, std::span<const u8>(media.data(), media.size())));
    }
}

TEST_CASE("RED: a malformed payload is refused in order and leaves the blocks as they were", "[hesap][red]")
{
    struct Case
    {
        const char* name;
        Bytes payload;
        usize room;
        RedStatus want;
    };
    const Case cases[] = {
        {"empty", Bytes{}, 4U, RedStatus::Truncated},
        {"a redundant header of one byte", bytes_of({0x87U}), 4U, RedStatus::Truncated},
        {"a redundant header of three bytes", bytes_of({0x87U, 0x00U, 0x40U}), 4U, RedStatus::Truncated},
        {"a chain that never ends", bytes_of({0x87U, 0x00U, 0x40U, 0x00U, 0x88U, 0x00U, 0x80U, 0x00U}), 4U,
         RedStatus::Truncated},
        {"a cut header before the room runs out", bytes_of({0x87U, 0x00U, 0x40U, 0x00U, 0x87U, 0x00U}), 1U,
         RedStatus::Truncated},
        {"more blocks than room", bytes_of({0x87U, 0x00U, 0x40U, 0x00U, 0x05U}), 1U, RedStatus::NoSpace},
        {"no room at all", bytes_of({0x05U}), 0U, RedStatus::NoSpace},
        {"lengths past the payload", bytes_of({0x87U, 0x00U, 0x40U, 0x02U, 0x05U, 0xAAU}), 4U, RedStatus::Truncated},
        {"room before lengths", bytes_of({0x87U, 0x00U, 0x40U, 0x02U, 0x05U, 0xAAU}), 1U, RedStatus::NoSpace},
    };
    for (const Case& c : cases)
    {
        INFO(c.name);
        RedBlock blocks[4] = {marked(), marked(), marked(), marked()};
        u32 count = 77U;
        CHECK(parse(c.payload, 1U, std::span<RedBlock>(blocks, c.room), count) == c.want);
        CHECK(count == 77U);
        for (const RedBlock& b : blocks)
        {
            CHECK(untouched(b));
        }
    }
    // The longest length a header can name, exactly met: accepted, with an empty primary.
    Bytes exact = bytes_of({0x87U, 0x00U, 0x43U, 0xFFU, 0x05U});
    for (u32 i = 0U; i < RedBlock::kMaxLength; ++i)
    {
        exact.push_back(static_cast<u8>(i));
    }
    RedBlock blocks[2];
    u32 count = 0U;
    REQUIRE(parse(exact, 0U, blocks, count) == RedStatus::Ok);
    CHECK((count == 2U && blocks[0].data.size() == RedBlock::kMaxLength && blocks[1].data.empty()));
    exact.pop_back();
    CHECK(parse(exact, 0U, blocks, count) == RedStatus::Truncated);
}

TEST_CASE("RED: the writer refuses fields out of range, repeated intervals and short output, writing nothing",
          "[hesap][red]")
{
    const Bytes small = counting(4U, 1U);
    const Bytes too_long = counting(RedBlock::kMaxLength + 1U, 0U);
    const Bytes media = counting(6U, 9U);
    struct Case
    {
        const char* name;
        RedBlock blocks[2];
        usize count;
        u8 primary_type;
        RedStatus want;
    };
    const Case cases[] = {
        {"a payload type above 127", {redundant(128U, 160U, small), {}}, 1U, 5U, RedStatus::OutOfRange},
        {"an offset above 16383", {redundant(7U, 16384U, small), {}}, 1U, 5U, RedStatus::OutOfRange},
        {"a block of 1024 bytes", {redundant(7U, 160U, too_long), {}}, 1U, 5U, RedStatus::OutOfRange},
        {"a primary type above 127", {redundant(7U, 160U, small), {}}, 1U, 128U, RedStatus::OutOfRange},
        {"the primary's interval", {redundant(7U, 0U, small), {}}, 1U, 5U, RedStatus::Repeated},
        {"one interval twice", {redundant(7U, 160U, small), redundant(8U, 160U, small)}, 2U, 5U, RedStatus::Repeated},
        {"the first block's fault first",
         {redundant(7U, 0U, small), redundant(200U, 160U, small)},
         2U,
         5U,
         RedStatus::Repeated},
        {"each block's fields before its interval",
         {redundant(7U, 320U, small), redundant(7U, 320U, too_long)},
         2U,
         5U,
         RedStatus::OutOfRange},
        {"a block's fault before the primary's", {redundant(7U, 0U, small), {}}, 1U, 200U, RedStatus::Repeated},
    };
    for (const Case& c : cases)
    {
        INFO(c.name);
        u8 out[1100];
        std::memset(out, kSentinel, sizeof(out));
        usize written = 99U;
        CHECK(hc::red_encode(std::span<const RedBlock>(c.blocks, c.count), c.primary_type,
                             std::span<const u8>(media.data(), media.size()), out, written) == c.want);
        CHECK(written == 99U);
        for (const u8 b : out)
        {
            CHECK(b == kSentinel);
        }
    }
    const RedBlock blocks[] = {redundant(7U, 160U, small)};
    const usize need = hc::red_bytes(blocks, media.size());
    REQUIRE(need == 4U + 1U + 4U + 6U);
    u8 out[32];
    std::memset(out, kSentinel, sizeof(out));
    usize written = 0U;
    CHECK(hc::red_encode(blocks, 5U, std::span<const u8>(media.data(), media.size()), std::span<u8>(out, need - 1U),
                         written) == RedStatus::NoSpace);
    CHECK(written == 0U);
    for (const u8 b : out)
    {
        CHECK(b == kSentinel);
    }
    CHECK(hc::red_encode(blocks, 5U, std::span<const u8>(media.data(), media.size()), std::span<u8>(out, need),
                         written) == RedStatus::Ok);
    CHECK((written == need && out[need] == kSentinel));
}
