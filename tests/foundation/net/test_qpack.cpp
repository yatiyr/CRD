// crd-net-tests: QPACK (4.2a; docs/design/network-foundation.md#qpack): RFC 9204 appendix B byte for byte and its table
// sizes, every field line form and instruction with every refusal, the Required Insert Count's wrap, blocked streams,
// eviction with a name and a duplicate taken from the entry being evicted, a section larger than its storage, and the
// encoder: what it inserts and references, what it never indexes, the blocked streams it stays within, decoder stream
// refusals, and random sections through a real decoder over a delayed encoder stream.

#include "ingress_fixture.hpp"
#include "text_fixture.hpp"

#include <crd/net/qpack.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <random>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::CountingAllocator;
using crd::net::testing::erase_front;
using crd::net::testing::number_text;
using crd::net::testing::slice;
using crd::net::testing::Text;
using crd::net::testing::TextPair;
using namespace crd::net::testing;

Text hex(const char* text)
{
    Text out;
    u32 held = 0U;
    bool odd = false;
    for (const char* at = text; *at != '\0'; ++at)
    {
        if (*at == ' ')
        {
            continue;
        }
        const u32 digit = (*at >= 'a') ? static_cast<u32>(*at - 'a') + 10U : static_cast<u32>(*at - '0');
        if (odd)
        {
            out.push_back(static_cast<char>((held << 4U) | digit));
        }
        held = digit;
        odd = !odd;
    }
    return out;
}

ConstSpan<u8> view(std::string_view bytes)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
}

Text text_of(ConstSpan<u8> bytes)
{
    return Text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A section's fields in one string, "name=value" joined by newlines; '!' before a never-indexed one.
Text joined(ConstSpan<HttpField> fields)
{
    Text out;
    for (const HttpField& field : fields)
    {
        out += field.never_index ? "!" : "";
        out += text_of(field.name) + "=" + text_of(field.value) + "\n";
    }
    return out;
}

struct Decoder
{
    explicit Decoder(u32 capacity = 220U, u32 blocked = 8U, u32 instructions = 256U)
    {
        QpackDecoderConfig config;
        config.table_capacity = capacity;
        config.blocked_streams = blocked;
        config.instruction_bytes = instructions;
        REQUIRE(codec.init(allocator, config));
    }

    bool feed(std::string_view bytes) { return codec.on_encoder_stream(view(bytes)); }

    QpackStatus decode(u64 stream, std::string_view section)
    {
        HttpField fields[32];
        u8 text[2048];
        usize count = 0U;
        const QpackStatus status = codec.decode(stream, view(section), Span<HttpField>(fields), Span<u8>(text), count);
        out = joined(ConstSpan<HttpField>(fields, count));
        return status;
    }

    Text instructions()
    {
        u8 room[256];
        Text all;
        for (usize got = codec.take_instructions(Span<u8>(room)); got != 0U;
             got = codec.take_instructions(Span<u8>(room)))
        {
            all.append(reinterpret_cast<const char*>(room), got);
        }
        return all;
    }

    CountingAllocator allocator;
    QpackDecoder codec;
    Text out;
};

struct Encoder
{
    explicit Encoder(u32 capacity = 4096U, u32 sections = 64U, u32 instructions = 8192U)
    {
        QpackEncoderConfig config;
        config.table_capacity = capacity;
        config.sections = sections;
        config.instruction_bytes = instructions;
        REQUIRE(codec.init(allocator, config));
    }

    Text encode(u64 stream, const cont::Array<TextPair>& fields, bool never = false)
    {
        cont::Array<HttpField> list;
        for (const auto& field : fields)
        {
            list.push_back(HttpField{view(field.first), view(field.second), never});
        }
        u8 room[4096];
        const usize bytes = codec.encode(stream, ConstSpan<HttpField>(list.data(), list.size()), Span<u8>(room));
        return Text(reinterpret_cast<const char*>(room), bytes);
    }

    Text instructions()
    {
        u8 room[9000];
        const usize got = codec.take_instructions(Span<u8>(room));
        return Text(reinterpret_cast<const char*>(room), got);
    }

    CountingAllocator allocator;
    QpackEncoder codec;
};

Text field_line(std::string_view name, std::string_view value)
{
    Text line(name);
    line += "=";
    line += value;
    line += "\n";
    return line;
}
} // namespace

TEST_CASE("qpack: RFC 9204 appendix B, every byte and the table's size", "[net][qpack]")
{
    Decoder decoder;

    // B.1: a literal with a static name reference.
    CHECK(decoder.decode(0U, hex("0000 510b 2f69 6e64 6578 2e68 746d 6c")) == QpackStatus::Complete);
    CHECK(decoder.out == ":path=/index.html\n");
    CHECK(decoder.instructions().empty());

    // B.2: a section ahead of its entries blocks; they arrive; the post-base lines read them.
    const Text stream_4 = hex("0381 10 11");
    CHECK(decoder.decode(4U, stream_4) == QpackStatus::Blocked);
    CHECK(decoder.feed(hex("3fbd01")));
    CHECK(decoder.feed(hex("c00f 7777 772e 6578 616d 706c 652e 636f 6d c10c 2f73 616d 706c 652f 7061 7468")));
    CHECK(decoder.codec.table().size() == 106U);
    u64 unblocked = 0U;
    REQUIRE(decoder.codec.next_unblocked(unblocked));
    CHECK(unblocked == 4U);
    CHECK_FALSE(decoder.codec.next_unblocked(unblocked));
    CHECK(decoder.decode(4U, stream_4) == QpackStatus::Complete);
    CHECK(decoder.out == ":authority=www.example.com\n:path=/sample/path\n");
    CHECK(decoder.instructions() == hex("84"));

    // B.3: a speculative insert and its acknowledgement.
    CHECK(decoder.feed(hex("4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565")));
    CHECK(decoder.codec.table().size() == 160U);
    CHECK(decoder.instructions() == hex("01"));

    // B.4: a duplicate, a section over it, the stream cancelled.
    CHECK(decoder.feed(hex("02")));
    CHECK(decoder.codec.table().size() == 217U);
    CHECK(decoder.decode(8U, hex("0500 80 c1 81")) == QpackStatus::Complete);
    CHECK(decoder.out == ":authority=www.example.com\n:path=/\ncustom-key=custom-value\n");
    CHECK(decoder.instructions() == hex("88"));
    decoder.codec.cancel(8U);
    CHECK(decoder.instructions() == hex("48"));

    // B.5: an insert naming a dynamic entry evicts the oldest.
    CHECK(decoder.feed(hex("810d 6375 7374 6f6d 2d76 616c 7565 32")));
    CHECK(decoder.codec.table().size() == 215U);
    CHECK(decoder.codec.table().dropped() == 1U);
    CHECK(decoder.codec.table().inserted() == 5U);
    CHECK(decoder.codec.error() == 0U);
}

TEST_CASE("qpack: an encoder stream split anywhere", "[net][qpack]")
{
    const Text stream = hex("3fbd01 c00f 7777 772e 6578 616d 706c 652e 636f 6d") +
                               hex("4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565");
    for (usize split = 1U; split < 5U; ++split)
    {
        Decoder decoder;
        for (usize at = 0U; at < stream.size(); at += split)
        {
            REQUIRE(decoder.feed(slice(stream, at, split)));
        }
        CHECK(decoder.codec.table().inserted() == 2U);
        CHECK(decoder.decode(0U, hex("0300 81 80")) == QpackStatus::Complete);
        CHECK(decoder.out == ":authority=www.example.com\ncustom-key=custom-value\n");
    }
}

TEST_CASE("qpack: every field line form", "[net][qpack]")
{
    Decoder decoder;
    REQUIRE(decoder.feed(hex("3fbd01")));
    REQUIRE(decoder.feed(hex("4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565"))); // abs 0
    REQUIRE(decoder.feed(hex("c10c 2f73 616d 706c 652f 7061 7468")));                         // abs 1: :path=/sample/path

    // Base 1, so abs 0 sits below it and abs 1 after it. RIC 2 is encoded as 3.
    CHECK(decoder.decode(0U, hex("03 00")) == QpackStatus::Complete); // Base = RIC + 0 = 2
    // S=1, delta 0: Base = RIC - 0 - 1 = 1.
    const Text prefix = hex("03 80");
    CHECK(decoder.decode(0U, prefix + hex("80 10")) == QpackStatus::Complete); // indexed pre-base, post-base
    CHECK(decoder.out == "custom-key=custom-value\n:path=/sample/path\n");
    CHECK(decoder.decode(0U, prefix + hex("c1")) == QpackStatus::Complete); // indexed static
    CHECK(decoder.out == ":path=/\n");
    // Literal with a dynamic name, a never-indexed value in plain text.
    CHECK(decoder.decode(0U, prefix + hex("60 01 78")) == QpackStatus::Complete);
    CHECK(decoder.out == "!custom-key=x\n");
    // Literal with a post-base name.
    CHECK(decoder.decode(0U, prefix + hex("00 01 79")) == QpackStatus::Complete);
    CHECK(decoder.out == ":path=y\n");
    CHECK(decoder.decode(0U, prefix + hex("08 01 79")) == QpackStatus::Complete);
    CHECK(decoder.out == "!:path=y\n");
    // Literal with a literal name, never indexed, Huffman-coded value ('302').
    CHECK(decoder.decode(0U, hex("0000 32 62 6f 82 6402")) == QpackStatus::Complete);
    CHECK(decoder.out == "!bo=302\n");
    // Literal with a Huffman-coded name ('custom-key' is RFC 7541's).
    CHECK(decoder.decode(0U, hex("0000 2f 01 25a849e95ba97d7f") + hex("01 61")) == QpackStatus::Complete);
    CHECK(decoder.out == "custom-key=a\n");
}

TEST_CASE("qpack: Huffman literals in a section and in instructions", "[net][qpack]")
{
    Decoder decoder;
    REQUIRE(decoder.feed(hex("3fbd01")));
    // Insert with a Huffman name and value.
    REQUIRE(decoder.feed(hex("68 25a849e95ba97d7f 89 25a849e95bb8e8b4bf")));
    CHECK(decoder.decode(0U, hex("02 00 80")) == QpackStatus::Complete);
    CHECK(decoder.out == "custom-key=custom-value\n");
    // Literal name with Huffman in a section: 001N H len(3+).
    CHECK(decoder.decode(0U, hex("0000 2f 01 25a849e95ba97d7f 89 25a849e95bb8e8b4bf")) == QpackStatus::Complete);
    CHECK(decoder.out == "custom-key=custom-value\n");
    // A plain name with a Huffman value, the value's room being the whole scratch.
    {
        Decoder other(64U);
        REQUIRE(other.feed(hex("3f21 50 6e6e6e6e 6e6e6e6e 6e6e6e6e 6e6e6e6e 8c f1e3c2e5f23a6ba0ab90f4ff")));
        CHECK(other.decode(0U, hex("0200 80")) == QpackStatus::Complete);
        CHECK(other.out == "nnnnnnnnnnnnnnnn=www.example.com\n");
    }
    // The same with no table at all: refused, and nothing is written.
    {
        Decoder none(0U, 0U);
        CHECK_FALSE(none.feed(hex("41 62 8c f1e3c2e5f23a6ba0ab90f4ff")));
        CHECK(none.codec.error() == kQpackEncoderStreamError);
    }
    // An insert with a static name and Huffman value.
    REQUIRE(decoder.feed(hex("c1 86 6402 6402 ffff")) == false);
}

TEST_CASE("qpack: Required Insert Count wraps and refuses", "[net][qpack]")
{
    // Capacity 64: MaxEntries 2, FullRange 4. A section's encoded count has at most four values, reading near the
    // table's count.
    Decoder decoder(64U, 8U);
    REQUIRE(decoder.feed(hex("3f21"))); // Set Dynamic Table Capacity = 64 (31 + 33)
    for (u32 round = 0U; round < 9U; ++round)
    {
        REQUIRE(decoder.feed(hex("4000"))); // an entry with an empty name and value
        const u64 inserted = decoder.codec.table().inserted();
        const Text one = Text(1U, static_cast<char>((inserted % 4U) + 1U)) + Text(1U, '\0') + hex("c1");
        CHECK(decoder.decode(1U, one) == QpackStatus::Complete);
    }
    CHECK(decoder.instructions().size() != 0U);

    // An Encoded Required Insert Count beyond FullRange.
    {
        Decoder other(64U, 8U);
        CHECK(other.decode(0U, hex("05 00")) == QpackStatus::Failed);
        CHECK(other.codec.error() == kQpackDecompressionFailed);
    }
    // One that would have wrapped below zero.
    {
        Decoder other(64U, 8U);
        REQUIRE(other.feed(hex("3f21")));
        CHECK(other.decode(0U, hex("04 00")) == QpackStatus::Failed); // RIC 3 with no inserts and MaxEntries 2
    }
    // A count that means zero inserts yet says some were needed.
    {
        Decoder other;
        CHECK(other.decode(0U, hex("01 00")) == QpackStatus::Failed);
        CHECK(other.codec.error() == kQpackDecompressionFailed);
    }
    // With MaxEntries 0 every non-zero count is beyond the range.
    {
        Decoder other(0U, 0U);
        CHECK(other.decode(0U, hex("01 00")) == QpackStatus::Failed);
    }
    // A negative Base.
    {
        Decoder other;
        REQUIRE(other.feed(hex("3fbd01 4000")));
        CHECK(other.decode(0U, hex("02 81 c1")) == QpackStatus::Failed); // RIC 1, S=1 delta 1
    }
    {
        Decoder other;
        CHECK(other.decode(0U, hex("00 80")) == QpackStatus::Failed); // RIC 0, S=1
    }
}

TEST_CASE("qpack: references the decoder refuses", "[net][qpack]")
{
    const auto table = []()
    {
        auto decoder = std::make_unique<Decoder>();
        REQUIRE(decoder->feed(hex("3fbd01 4000 4000")));
        return decoder;
    };
    SECTION("static index past the table")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("0000 ff24")) == QpackStatus::Failed); // 63 + 36 = 99
    }
    SECTION("the last static entry")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("0000 ff23")) == QpackStatus::Complete);
        CHECK(decoder->out == "x-frame-options=sameorigin\n");
    }
    SECTION("a dynamic index at or past the Base")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("0300 82")) == QpackStatus::Failed); // Base 2: relative 2 is before it
    }
    SECTION("a reference at or past the Required Insert Count")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("0201 80")) == QpackStatus::Failed); // RIC 1, Base 2: relative 0 is abs 1
    }
    SECTION("a post-base reference at or past the Required Insert Count")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("0280 10")) == QpackStatus::Complete); // RIC 1, Base 0, post-base 0
        CHECK(decoder->decode(0U, hex("0280 11")) == QpackStatus::Failed);
    }
    SECTION("an evicted entry")
    {
        auto decoder = table();
        REQUIRE(decoder->feed(hex("3f01"))); // capacity 32: evicts all but the newest
        CHECK(decoder->decode(0U, hex("0300 81")) == QpackStatus::Failed);
    }
    SECTION("a section cut short")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("00")) == QpackStatus::Failed);
        auto other = table();
        CHECK(other->decode(0U, hex("0000 51 0b 2f")) == QpackStatus::Failed);
    }
    SECTION("a failed decoder stays failed")
    {
        auto decoder = table();
        CHECK(decoder->decode(0U, hex("00")) == QpackStatus::Failed);
        CHECK(decoder->decode(0U, hex("0000")) == QpackStatus::Failed);
        CHECK_FALSE(decoder->feed(hex("4000")));
        CHECK(decoder->codec.error() == kQpackDecompressionFailed);
    }
}

TEST_CASE("qpack: encoder stream refusals", "[net][qpack]")
{
    const auto refused = [](u32 capacity, std::string_view bytes)
    {
        Decoder decoder(capacity);
        CHECK_FALSE(decoder.feed(bytes));
        CHECK(decoder.codec.error() == kQpackEncoderStreamError);
    };
    refused(220U, hex("3fbe01"));          // a capacity beyond the setting
    refused(0U, hex("4000"));              // an insert with no capacity
    refused(220U, hex("3fbd01 ff24 00"));  // a static name past the table
    refused(220U, hex("3fbd01 80 00"));    // a dynamic name in an empty table
    refused(220U, hex("3fbd01 00"));       // a duplicate in an empty table
    refused(32U, hex("3f01 4001 62"));     // Set capacity 32, a one-byte name with no room for the overhead
    refused(220U, hex("3fbd01 c0 81 ff")); // a Huffman value that does not decode
    refused(64U, hex("3f21 c1 28") + Text(40U, 'v'));                // a static name, an entry too long
    refused(64U, hex("3f21 41 61 00 80 28") + Text(40U, 'v'));       // a dynamic name, an entry too long
    refused(220U, hex("3fbd01 4000 4000 3f01 01"));                         // a duplicate of an evicted entry
}

TEST_CASE("qpack: an entry longer than the table is refused on its length", "[net][qpack]")
{
    Decoder decoder(64U);
    REQUIRE(decoder.feed(hex("3f21")));
    // A 300-byte value, far beyond any instruction the table could hold.
    CHECK_FALSE(decoder.feed(hex("c0 7f ad 02")));
    CHECK(decoder.codec.error() == kQpackEncoderStreamError);
}

TEST_CASE("qpack: blocked streams", "[net][qpack]")
{
    Decoder decoder(220U, 2U);
    const Text section = hex("02 00 80"); // RIC 1, Base 1, relative 0
    CHECK(decoder.decode(4U, section) == QpackStatus::Blocked);
    CHECK(decoder.decode(4U, section) == QpackStatus::Blocked); // the same stream counts once
    CHECK(decoder.decode(8U, section) == QpackStatus::Blocked);
    CHECK(decoder.codec.blocked() == 2U);
    decoder.codec.cancel(8U);
    CHECK(decoder.codec.blocked() == 1U);
    CHECK(decoder.decode(8U, section) == QpackStatus::Blocked);
    CHECK(decoder.decode(12U, section) == QpackStatus::Failed); // one more than promised
    CHECK(decoder.codec.error() == kQpackDecompressionFailed);
    CHECK(decoder.instructions() == hex("48"));

    Decoder ordered(220U, 4U);
    const Text later = hex("03 00 80");
    CHECK(ordered.decode(4U, later) == QpackStatus::Blocked);
    CHECK(ordered.decode(8U, section) == QpackStatus::Blocked);
    REQUIRE(ordered.feed(hex("3fbd01 4000")));
    u64 stream = 0U;
    REQUIRE(ordered.codec.next_unblocked(stream));
    CHECK(stream == 8U); // the section that one entry satisfies
    CHECK_FALSE(ordered.codec.next_unblocked(stream));
    REQUIRE(ordered.feed(hex("4000")));
    REQUIRE(ordered.codec.next_unblocked(stream));
    CHECK(stream == 4U);

    // A section decoded without being named stops counting as blocked.
    Decoder eager(220U, 4U);
    CHECK(eager.decode(4U, section) == QpackStatus::Blocked);
    REQUIRE(eager.feed(hex("3fbd01 4000")));
    CHECK(eager.decode(4U, section) == QpackStatus::Complete);
    CHECK(eager.codec.blocked() == 0U);
    CHECK_FALSE(eager.codec.next_unblocked(stream));

    // A zero-limit decoder fails at the first block.
    Decoder none(220U, 0U);
    CHECK(none.decode(4U, section) == QpackStatus::Failed);
    CHECK(none.codec.error() == kQpackDecompressionFailed);
}

TEST_CASE("qpack: an insert copies the name of the entry it evicts, across the ring's end", "[net][qpack]")
{
    // The ring holds 96 bytes and each entry here is 32 + 28 = 60, so one fits at a time, and every new entry's name
    // comes from the entry being evicted, writing over the bytes it reads.
    Decoder decoder(96U);
    REQUIRE(decoder.feed(hex("3f41"))); // capacity 96
    const Text name(16U, 'n');
    const Text first(12U, 'a');
    Text insert = hex("50");
    insert += name + Text(1U, static_cast<char>(first.size())) + first;
    REQUIRE(decoder.feed(insert));
    for (u32 round = 0U; round < 20U; ++round)
    {
        const Text value(12U, static_cast<char>('b' + round));
        REQUIRE(decoder.feed(hex("80") + Text(1U, static_cast<char>(value.size())) + value));
        CHECK(decoder.codec.table().inserted() == u64{round} + 2U);
        CHECK(decoder.codec.table().dropped() == u64{round} + 1U);
        const u64 required = decoder.codec.table().inserted();
        const Text section = Text(1U, static_cast<char>((required % 6U) + 1U)) + hex("00 80");
        REQUIRE(decoder.decode(0U, section) == QpackStatus::Complete);
        REQUIRE(decoder.out == field_line(name, value));
    }

    // And a duplicate of the entry it evicts: 16 + 12 + 32 twice does not fit in 96.
    const Text last(12U, static_cast<char>('b' + 19U));
    for (u32 round = 0U; round < 20U; ++round)
    {
        REQUIRE(decoder.feed(hex("00")));
        const u64 required = decoder.codec.table().inserted();
        const Text section = Text(1U, static_cast<char>((required % 6U) + 1U)) + hex("00 80");
        REQUIRE(decoder.decode(0U, section) == QpackStatus::Complete);
        REQUIRE(decoder.out == field_line(name, last));
    }
}

TEST_CASE("qpack: a section larger than its storage is still read and acknowledged", "[net][qpack]")
{
    Decoder decoder;
    REQUIRE(decoder.feed(hex("3fbd01 4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565")));
    const Text section = hex("0200 80 c1 80");
    HttpField fields[2];
    u8 text[64];
    usize count = 7U;
    // Too many fields for the array.
    CHECK(decoder.codec.decode(0U, view(section), Span<HttpField>(fields), Span<u8>(text), count) ==
          QpackStatus::TooLarge);
    CHECK(count == 0U);
    CHECK(decoder.instructions() == hex("80"));
    // Too much text for the bytes; the Huffman value and a dynamic entry are measured alike.
    u8 small[8];
    CHECK(decoder.codec.decode(4U, view(section), Span<HttpField>(fields), Span<u8>(small), count) ==
          QpackStatus::TooLarge);
    CHECK(decoder.instructions() == hex("84"));
    // A literal past the storage still has to be valid.
    CHECK(decoder.codec.decode(8U, view(hex("0000 2f 01 25a849e95ba97d7f 81 ff")), Span<HttpField>(fields),
                               Span<u8>(small), count) == QpackStatus::Failed);
    CHECK(decoder.codec.error() == kQpackDecompressionFailed);
}

TEST_CASE("qpack: a decoder stream the peer will not read fails the connection", "[net][qpack]")
{
    Decoder decoder(220U, 8U, 64U);
    REQUIRE(decoder.feed(hex("3fbd01 4000")));
    const Text section = hex("0200 80");
    QpackStatus status = QpackStatus::Complete;
    for (u32 stream = 0U; stream < 64U && status == QpackStatus::Complete; ++stream)
    {
        status = decoder.decode(u64{stream} * 4U, section);
    }
    CHECK(status == QpackStatus::Failed);
    CHECK(decoder.codec.error() == kHttp3ExcessiveLoad);
}

TEST_CASE("qpack: a table too small for any entry is legal and holds nothing", "[net][qpack]")
{
    Decoder decoder(16U, 2U);
    REQUIRE(decoder.feed(hex("30"))); // Set Dynamic Table Capacity = 16
    CHECK_FALSE(decoder.feed(hex("4000")));
    CHECK(decoder.codec.error() == kQpackEncoderStreamError);

    Decoder reader(16U, 2U);
    CHECK(reader.decode(0U, hex("0000 c1")) == QpackStatus::Complete);
    CHECK(reader.out == ":path=/\n");
    CHECK(reader.decode(0U, hex("0100")) == QpackStatus::Failed); // no count can be encoded for it

    Encoder encoder(16U);
    encoder.codec.peer_settings(16U, 2U);
    CHECK(encoder.instructions() == hex("30"));
    for (u32 round = 0U; round < 3U; ++round)
    {
        const Text section = encoder.encode(round, {{"x-a", "1"}});
        Decoder other(16U, 2U);
        REQUIRE(other.decode(round, section) == QpackStatus::Complete);
        CHECK(other.out == "x-a=1\n");
    }
    CHECK(encoder.codec.table().inserted() == 0U);
}

TEST_CASE("qpack: decoder configurations and allocation", "[net][qpack]")
{
    QpackDecoderConfig config;
    CountingAllocator allocator;
    {
        QpackDecoder codec;
        QpackDecoderConfig bad = config;
        bad.table_capacity = kQpackMaxTableCapacity + 1U;
        CHECK_FALSE(codec.init(allocator, bad));
        bad = config;
        bad.blocked_streams = kQpackMaxBlockedStreams + 1U;
        CHECK_FALSE(codec.init(allocator, bad));
        bad = config;
        bad.instruction_bytes = kQpackMinInstructionBytes - 1U;
        CHECK_FALSE(codec.init(allocator, bad));
        bad = config;
        bad.instruction_bytes = kQpackMaxInstructionBytes + 1U;
        CHECK_FALSE(codec.init(allocator, bad));
        CHECK(codec.init(allocator, config));
        CHECK_FALSE(codec.init(allocator, config)); // once
    }
    // A refusal at each allocation leaves nothing.
    for (usize refuse = 0U; refuse < 6U; ++refuse)
    {
        CountingAllocator limited;
        limited.refuse_after(refuse);
        QpackDecoder codec;
        CHECK_FALSE(codec.init(limited, config));
    }
    {
        CountingAllocator enough;
        enough.refuse_after(6U);
        QpackDecoder codec;
        CHECK(codec.init(enough, config));
    }
    {
        Decoder decoder;
        REQUIRE(decoder.feed(hex("3fbd01 4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565")));
        const usize calls = decoder.allocator.calls();
        CHECK(decoder.decode(0U, hex("0200 80")) == QpackStatus::Complete);
        REQUIRE(decoder.feed(hex("00")));
        (void)decoder.instructions();
        CHECK(decoder.allocator.calls() == calls);
    }
}

TEST_CASE("qpack encoder: static table lines only without the peer's settings", "[net][qpack]")
{
    Encoder encoder;
    const Text section = encoder.encode(0U, {{":method", "GET"}, {":path", "/index.html"}, {"x", "y"}});
    // Required Insert Count 0, Base 0; :method GET is static index 17; :path a literal with a static name; x a literal
    // name.
    CHECK(slice(section, 0U, 3U) == hex("0000 d1"));
    CHECK(encoder.instructions().empty());
    CHECK(encoder.codec.table().capacity() == 0U);

    Decoder decoder;
    REQUIRE(decoder.decode(0U, section) == QpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:path=/index.html\nx=y\n");
}

TEST_CASE("qpack encoder: a field goes in on its second sight and is referenced once acknowledged", "[net][qpack]")
{
    Encoder encoder;
    encoder.codec.peer_settings(220U, 0U);
    Decoder decoder;
    REQUIRE(decoder.feed(encoder.instructions()));
    CHECK(decoder.codec.table().capacity() == 220U);

    const cont::Array<TextPair> fields = {{"x-track", "abcdef"}};
    const Text first = encoder.encode(0U, fields);
    CHECK(encoder.codec.table().inserted() == 0U);
    REQUIRE(decoder.decode(0U, first) == QpackStatus::Complete);

    // Seen again: inserted, but the peer allows no blocked stream, so this section stays a literal.
    const Text second = encoder.encode(4U, fields);
    CHECK(encoder.codec.table().inserted() == 1U);
    CHECK(second == first);
    REQUIRE(decoder.feed(encoder.instructions()));
    REQUIRE(decoder.decode(4U, second) == QpackStatus::Complete);

    // The insert's increment arrives: now one reference line.
    REQUIRE(encoder.codec.on_decoder_stream(view(decoder.instructions())));
    CHECK(encoder.codec.known_received() == 1U);
    const Text third = encoder.encode(8U, fields);
    CHECK(third.size() < second.size());
    CHECK(encoder.codec.outstanding() == 1U);
    REQUIRE(decoder.decode(8U, third) == QpackStatus::Complete);
    CHECK(decoder.out == "x-track=abcdef\n");
    REQUIRE(encoder.codec.on_decoder_stream(view(decoder.instructions())));
    CHECK(encoder.codec.outstanding() == 0U);
}

TEST_CASE("qpack encoder: never-indexed fields stay out of the table", "[net][qpack]")
{
    Encoder encoder;
    encoder.codec.peer_settings(4096U, 8U);
    (void)encoder.instructions();
    for (u32 round = 0U; round < 3U; ++round)
    {
        const Text section =
            encoder.encode(round, {{"authorization", "Bearer abc"},
                                   {"proxy-authorization", "Basic abc"},
                                   {"cookie", "sid=1"},
                                   {"set-cookie", "sid=2"}});
        Decoder decoder(4096U);
        REQUIRE(decoder.decode(round, section) == QpackStatus::Complete);
        CHECK(decoder.out ==
              "!authorization=Bearer abc\n!proxy-authorization=Basic abc\n!cookie=sid=1\n!set-cookie=sid=2\n");
        const Text marked = encoder.encode(round + 10U, {{"x-secret", "s"}}, true);
        REQUIRE(decoder.decode(round + 10U, marked) == QpackStatus::Complete);
        CHECK(decoder.out == "!x-secret=s\n");
    }
    CHECK(encoder.codec.table().inserted() == 0U);
    CHECK(encoder.instructions().empty());
}

TEST_CASE("qpack encoder: blocked streams and post-base references", "[net][qpack]")
{
    Encoder encoder;
    encoder.codec.peer_settings(4096U, 1U);
    Decoder decoder(4096U, 1U);
    REQUIRE(decoder.feed(encoder.instructions()));
    const cont::Array<TextPair> fields = {{"x-a", "1"}, {"x-b", "2"}};
    (void)encoder.encode(0U, fields);
    const Text blocking = encoder.encode(4U, fields);
    CHECK(encoder.codec.table().inserted() == 2U);
    // Base 0, entries 0 and 1 are post-base; RIC 2.
    CHECK(static_cast<u8>(blocking.data()[0]) == 3U);
    CHECK(static_cast<u8>(blocking.data()[1]) == 0x81U);
    CHECK(slice(blocking, 2U) == hex("10 11"));

    // A second stream may not block while the first does: literals. It may not even if it has entries to reference.
    const Text other = encoder.encode(8U, fields);
    CHECK(other.size() > blocking.size());
    CHECK(encoder.codec.outstanding() == 1U);
    // The same stream may, it blocks only once.
    const Text same_stream = encoder.encode(4U, fields);
    CHECK(same_stream.size() < other.size());
    CHECK(encoder.codec.outstanding() == 2U);

    CHECK(decoder.decode(4U, blocking) == QpackStatus::Blocked);
    REQUIRE(decoder.feed(encoder.instructions()));
    u64 stream = 0U;
    REQUIRE(decoder.codec.next_unblocked(stream));
    REQUIRE(decoder.decode(4U, blocking) == QpackStatus::Complete);
    CHECK(decoder.out == "x-a=1\nx-b=2\n");
    REQUIRE(decoder.decode(8U, other) == QpackStatus::Complete);
    CHECK(decoder.out == "x-a=1\nx-b=2\n");
    REQUIRE(decoder.decode(4U, same_stream) == QpackStatus::Complete);
    CHECK(decoder.out == "x-a=1\nx-b=2\n");
    REQUIRE(encoder.codec.on_decoder_stream(view(decoder.instructions())));
    CHECK(encoder.codec.outstanding() == 0U);
    CHECK(encoder.codec.known_received() == 2U);
}

TEST_CASE("qpack encoder: an entry an outstanding section references is not evicted", "[net][qpack]")
{
    // Room for two 60-byte entries.
    Encoder encoder;
    encoder.codec.peer_settings(120U, 8U);
    (void)encoder.instructions();
    const auto seen_twice = [&encoder](u64 stream, std::string_view name)
    {
        const cont::Array<TextPair> field = {TextPair(Text(name), Text(18U, 'v'))};
        (void)encoder.encode(stream, field);
        return encoder.encode(stream + 4U, field);
    };
    (void)seen_twice(0U, "aaaaaaaaaa");
    (void)seen_twice(8U, "bbbbbbbbbb");
    CHECK(encoder.codec.table().inserted() == 2U);
    CHECK(encoder.codec.outstanding() == 2U);

    // Both are referenced and unacknowledged: a third cannot come in.
    (void)seen_twice(16U, "cccccccccc");
    CHECK(encoder.codec.table().inserted() == 2U);
    CHECK(encoder.codec.table().dropped() == 0U);

    // Acknowledged but still referenced: still no.
    REQUIRE(encoder.codec.on_decoder_stream(view(hex("02"))));
    (void)seen_twice(16U, "cccccccccc");
    CHECK(encoder.codec.table().inserted() == 2U);

    // Cancelling the section on entry 0 lets it go, and only it.
    REQUIRE(encoder.codec.on_decoder_stream(view(hex("44"))));
    CHECK(encoder.codec.outstanding() == 1U);
    (void)seen_twice(24U, "dddddddddd");
    CHECK(encoder.codec.table().inserted() == 3U);
    CHECK(encoder.codec.table().dropped() == 1U);
}

TEST_CASE("qpack encoder: an unreferenced entry is not evicted until it is acknowledged", "[net][qpack]")
{
    Encoder encoder;
    encoder.codec.peer_settings(64U, 0U); // room for one 60-byte entry; no blocked streams, so no section references it
    (void)encoder.instructions();
    const auto seen_twice = [&encoder](u64 stream, std::string_view name)
    {
        const cont::Array<TextPair> field = {TextPair(Text(name), Text(18U, 'v'))};
        (void)encoder.encode(stream, field);
        (void)encoder.encode(stream + 4U, field);
    };
    seen_twice(0U, "aaaaaaaaaa");
    CHECK(encoder.codec.table().inserted() == 1U);
    CHECK(encoder.codec.outstanding() == 0U);
    seen_twice(8U, "bbbbbbbbbb");
    CHECK(encoder.codec.table().inserted() == 1U); // RFC 9204 section 2.1.1
    REQUIRE(encoder.codec.on_decoder_stream(view(hex("01"))));
    seen_twice(16U, "cccccccccc");
    CHECK(encoder.codec.table().inserted() == 2U);
    CHECK(encoder.codec.table().dropped() == 1U);
}

TEST_CASE("qpack encoder: a field in use is duplicated before it falls out", "[net][qpack]")
{
    using Field = TextPair;
    const auto named = [](char letter) { return Field{Text(10U, letter), Text(18U, 'v')}; };
    const auto fill = [&named](Encoder& encoder, Decoder& decoder)
    {
        REQUIRE(decoder.feed(encoder.instructions()));
        for (u32 index = 0U; index < 8U; ++index)
        {
            const cont::Array<Field> fields = {named(static_cast<char>('a' + index))};
            (void)encoder.encode(u64{index} * 8U, fields);
            (void)encoder.encode((u64{index} * 8U) + 4U, fields);
        }
        REQUIRE(decoder.feed(encoder.instructions()));
        REQUIRE(encoder.codec.on_decoder_stream(view(decoder.instructions())));
        CHECK(encoder.codec.known_received() == 8U);
        for (u32 index = 0U; index < 8U; ++index)
        {
            const Text cancelled(1U, static_cast<char>(0x40U | ((index * 8U) + 4U)));
            REQUIRE(encoder.codec.on_decoder_stream(view(cancelled)));
        }
    };

    SECTION("a section that may not block references the original, and an older entry goes")
    {
        Encoder encoder;
        encoder.codec.peer_settings(480U, 0U);
        Decoder decoder(480U, 0U);
        fill(encoder, decoder);
        const Text section = encoder.encode(100U, {named('b')}); // entry 1 of the eight, in the oldest quarter
        CHECK(encoder.codec.table().inserted() == 9U);
        CHECK(encoder.codec.table().dropped() == 1U);
        // The duplicate arrives first: entry 1 must still be there.
        REQUIRE(decoder.feed(encoder.instructions()));
        REQUIRE(decoder.decode(100U, section) == QpackStatus::Complete);
        CHECK(decoder.out == Text(10U, 'b') + "=" + Text(18U, 'v') + "\n");
    }
    SECTION("a section that may block references the copy, and the original may go")
    {
        Encoder encoder;
        encoder.codec.peer_settings(480U, 8U);
        Decoder decoder(480U, 8U);
        fill(encoder, decoder);
        const Text section = encoder.encode(100U, {named('a')});
        CHECK(encoder.codec.table().inserted() == 9U);
        CHECK(encoder.codec.table().dropped() == 1U);
        CHECK(decoder.decode(100U, section) == QpackStatus::Blocked);
        REQUIRE(decoder.feed(encoder.instructions()));
        u64 stream = 0U;
        REQUIRE(decoder.codec.next_unblocked(stream));
        REQUIRE(decoder.decode(100U, section) == QpackStatus::Complete);
        CHECK(decoder.out == Text(10U, 'a') + "=" + Text(18U, 'v') + "\n");
    }
    SECTION("a table with room needs no copy")
    {
        Encoder encoder;
        encoder.codec.peer_settings(4096U, 8U);
        Decoder decoder(4096U, 8U);
        fill(encoder, decoder);
        const Text section = encoder.encode(100U, {named('a')});
        CHECK(encoder.codec.table().inserted() == 8U);
        REQUIRE(decoder.decode(100U, section) == QpackStatus::Complete);
    }
    SECTION("a field in the newer part of the table stays where it is")
    {
        Encoder encoder;
        encoder.codec.peer_settings(480U, 8U);
        Decoder decoder(480U, 8U);
        fill(encoder, decoder);
        const Text section = encoder.encode(100U, {named('h')});
        CHECK(encoder.codec.table().inserted() == 8U);
        REQUIRE(decoder.decode(100U, section) == QpackStatus::Complete);
    }
    SECTION("with no older entry to give up, the oldest is not copied at its own cost")
    {
        Encoder encoder;
        encoder.codec.peer_settings(480U, 0U);
        Decoder decoder(480U, 0U);
        fill(encoder, decoder);
        const Text section = encoder.encode(100U, {named('a')});
        CHECK(encoder.codec.table().inserted() == 8U);
        REQUIRE(decoder.decode(100U, section) == QpackStatus::Complete);
    }
}

TEST_CASE("qpack encoder: a name reference to an entry of this section is post-base", "[net][qpack]")
{
    Encoder encoder;
    encoder.codec.peer_settings(4096U, 8U);
    using Field = TextPair;
    const cont::Array<Field> fields = {{"x-name", "a"}};
    (void)encoder.encode(0U, fields);
    const Text section = encoder.encode(4U, {{"x-name", "a"}, {"x-name", "b"}});
    CHECK(encoder.codec.table().inserted() == 1U);
    // RIC 1 (encoded 2), Base 0, the first line post-base index 0, the second a literal naming post-base 0.
    CHECK(slice(section, 0U, 4U) == hex("0280 10 00"));
    Decoder decoder(4096U);
    REQUIRE(decoder.feed(encoder.instructions()));
    REQUIRE(decoder.decode(4U, section) == QpackStatus::Complete);
    CHECK(decoder.out == "x-name=a\nx-name=b\n");
}

TEST_CASE("qpack encoder: the section slots and the instruction room run out gently", "[net][qpack]")
{
    Encoder encoder(4096U, 1U);
    encoder.codec.peer_settings(4096U, 8U);
    (void)encoder.instructions();
    const cont::Array<TextPair> a = {{"x-a", "1"}};
    const cont::Array<TextPair> b = {{"x-b", "2"}};
    (void)encoder.encode(0U, a);
    (void)encoder.encode(4U, a); // takes the one slot
    CHECK(encoder.codec.outstanding() == 1U);
    const usize inserted = encoder.codec.table().inserted();
    (void)encoder.encode(8U, b);
    const Text literal = encoder.encode(12U, b); // a slot is needed even to insert
    CHECK(encoder.codec.table().inserted() == inserted);
    CHECK(encoder.codec.outstanding() == 1U);
    Decoder decoder(4096U);
    REQUIRE(decoder.decode(12U, literal) == QpackStatus::Complete);
    CHECK(decoder.out == "x-b=2\n");

    Encoder narrow(4096U, 64U, kQpackMinInstructionBytes);
    narrow.codec.peer_settings(4096U, 8U);
    (void)narrow.instructions();
    const Text big(100U, 'q');
    const cont::Array<TextPair> long_field = {TextPair(Text("x-long"), Text(big))};
    (void)narrow.encode(0U, long_field);
    const Text kept = narrow.encode(4U, long_field);
    CHECK(narrow.codec.table().inserted() == 0U); // no room for the instruction
    Decoder wide(4096U);
    REQUIRE(wide.decode(4U, kept) == QpackStatus::Complete);
    CHECK(wide.out == "x-long=" + big + "\n");
}

TEST_CASE("qpack encoder: a section that does not fit, and the size of its prefix", "[net][qpack]")
{
    Encoder encoder;
    u8 room[8];
    const Text name("x-name");
    const Text value(40U, 'v');
    const HttpField field{view(name), view(value), false};
    CHECK(encoder.codec.encode(0U, ConstSpan<HttpField>(&field, 1U), Span<u8>(room)) == 0U);
    u8 small[24];
    CHECK(encoder.codec.encode(0U, ConstSpan<HttpField>(&field, 1U), Span<u8>(small)) == 0U);
    u8 enough[128];
    CHECK(encoder.codec.encode(0U, ConstSpan<HttpField>(&field, 1U), Span<u8>(enough)) != 0U);
}

TEST_CASE("qpack encoder: decoder stream refusals", "[net][qpack]")
{
    const auto refused = [](std::string_view bytes)
    {
        Encoder encoder;
        encoder.codec.peer_settings(4096U, 8U);
        CHECK_FALSE(encoder.codec.on_decoder_stream(view(bytes)));
        CHECK(encoder.codec.error() == kQpackDecoderStreamError);
        CHECK(encoder.encode(0U, {{"a", "b"}}).empty());
    };
    refused(hex("84"));   // a Section Acknowledgment with nothing outstanding
    refused(hex("00"));   // an increment of zero
    refused(hex("01"));   // an increment past what was sent
    refused(hex("3f ff ff ff ff ff ff ff ff ff ff")); // an integer that never ends

    // A cancellation of nothing outstanding is fine.
    Encoder encoder;
    CHECK(encoder.codec.on_decoder_stream(view(hex("44"))));
    // Instructions split anywhere.
    Encoder split;
    split.codec.peer_settings(4096U, 8U);
    CHECK(split.codec.on_decoder_stream(view(hex("7f"))));
    CHECK(split.codec.on_decoder_stream(view(hex("01"))));
    CHECK(split.codec.error() == 0U);
}

TEST_CASE("qpack encoder: the peer's maximum, not this side's, sets the prefix", "[net][qpack]")
{
    // The peer allows 1024 (MaxEntries 32), this side keeps 128.
    Encoder encoder(128U);
    encoder.codec.peer_settings(1024U, 8U);
    CHECK(encoder.codec.table().capacity() == 128U);
    CHECK(encoder.instructions() == hex("3f61")); // Set Dynamic Table Capacity = 128
    Decoder decoder(1024U);
    REQUIRE(decoder.feed(hex("3f61")));
    const cont::Array<TextPair> fields = {{"x-a", "1"}};
    (void)encoder.encode(0U, fields);
    const Text section = encoder.encode(4U, fields);
    REQUIRE(decoder.feed(encoder.instructions()));
    REQUIRE(decoder.decode(4U, section) == QpackStatus::Complete);
    CHECK(decoder.out == "x-a=1\n");
}

TEST_CASE("qpack encoder: the Required Insert Count wraps at the peer's range, not this side's", "[net][qpack]")
{
    // This side keeps 64 bytes (two entries), the peer allows 4096 (128): a count past four needs the peer's range.
    Encoder encoder(64U);
    encoder.codec.peer_settings(4096U, 8U);
    Decoder decoder(4096U, 8U);
    REQUIRE(decoder.feed(encoder.instructions()));
    using Field = TextPair;
    for (u32 round = 0U; round < 10U; ++round)
    {
        const cont::Array<Field> fields = {TextPair("x-" + number_text(round), Text("v"))};
        for (const u64 stream : {u64{round} * 8U, (u64{round} * 8U) + 4U})
        {
            const Text section = encoder.encode(stream, fields);
            REQUIRE(decoder.feed(encoder.instructions()));
            u64 unblocked = 0U;
            while (decoder.codec.next_unblocked(unblocked))
            {
            }
            REQUIRE(decoder.decode(stream, section) == QpackStatus::Complete);
            REQUIRE(decoder.out == field_line(fields[0].first, "v"));
            REQUIRE(encoder.codec.on_decoder_stream(view(decoder.instructions())));
        }
    }
    CHECK(encoder.codec.table().inserted() == 10U);
}

TEST_CASE("qpack encoder: encoders and decoders agree over random traffic and delays", "[net][qpack]")
{
    for (u32 seed = 1U; seed <= 12U; ++seed)
    {
        std::mt19937 random(seed);
        const u32 capacities[] = {128U, 1024U, 4096U};
        const u32 capacity = capacities[seed % 3U];
        const u32 blocked = (seed % 2U == 0U) ? 0U : 4U;
        Encoder encoder;
        encoder.codec.peer_settings(capacity, blocked);
        Decoder decoder(capacity, blocked, 4096U);
        Text in_flight = encoder.instructions();
        const auto field = [&random](u32 at)
        {
            const char* const names[] = {"x-a", "x-bb", ":path", "accept", "content-type", "user-agent"};
            return TextPair{Text(names[random() % 6U]), "value-" + number_text((at * random()) % 9U)};
        };
        struct Waiting
        {
            u64 stream = 0U;
            Text section;
            Text expected;
        };
        cont::Array<Waiting> waiting;
        u64 next_stream = 0U;
        for (u32 round = 0U; round < 300U; ++round)
        {
            cont::Array<TextPair> fields;
            const u32 count = 1U + (random() % 8U);
            for (u32 at = 0U; at < count; ++at)
            {
                fields.push_back(field(at));
            }
            const Text section = encoder.encode(next_stream, fields);
            REQUIRE(!section.empty());
            Text expected;
            for (const auto& one : fields)
            {
                expected += one.first + "=" + one.second + "\n";
            }
            waiting.push_back(Waiting{next_stream, section, expected});
            next_stream += 4U;

            in_flight += encoder.instructions();
            // Part of the encoder stream arrives.
            if ((random() % 4U) != 0U)
            {
                const usize cut = random() % (in_flight.size() + 1U);
                REQUIRE(decoder.feed(slice(in_flight, 0U, cut)));
                erase_front(in_flight, cut);
            }
            // What the encoder stream unblocked first, then the rest out of order; the blocked ones wait.
            const auto settle = [&waiting, &decoder, seed, round](usize at)
            {
                const QpackStatus status = decoder.decode(waiting[at].stream, waiting[at].section);
                if (status == QpackStatus::Blocked)
                {
                    return;
                }
                INFO("seed " << seed << " round " << round << " error " << decoder.codec.error());
                REQUIRE(status == QpackStatus::Complete);
                REQUIRE(decoder.out == waiting[at].expected);
                waiting.erase(at);
            };
            u64 unblocked = 0U;
            while (decoder.codec.next_unblocked(unblocked))
            {
                for (usize at = 0U; at < waiting.size(); ++at)
                {
                    if (waiting[at].stream == unblocked)
                    {
                        settle(at);
                        break;
                    }
                }
            }
            for (usize at = waiting.size(); at-- > 0U;)
            {
                settle(at);
            }
            REQUIRE(encoder.codec.on_decoder_stream(view(decoder.instructions())));
        }
        CHECK(decoder.codec.error() == 0U);
        CHECK(encoder.codec.error() == 0U);
        // Every entry an encoder insert was valid: nothing unmatched.
        REQUIRE(decoder.feed(in_flight));
        CHECK(decoder.codec.table().inserted() == encoder.codec.table().inserted());
    }
}
