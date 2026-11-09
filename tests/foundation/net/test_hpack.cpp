// crd-net-tests: HPACK (4.2a; docs/design/network-foundation.md#hpack): RFC 7541 appendix C byte for byte with its
// table states, the whole static table, every representation with every refusal, dynamic table size updates and what a
// limit obliges, an entry larger than the table, an entry naming the one it evicts, a block larger than its storage
// keeping the table in step, and the encoder: what it indexes, what it never indexes, the updates it signals, a refused
// block changing nothing, and random blocks through a real decoder across SETTINGS changes.

#include "ingress_fixture.hpp"
#include "text_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/net/hpack.hpp>

#include <catch2/catch_test_macros.hpp>
#include <random>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::CountingAllocator;
using crd::net::testing::hex;
using crd::net::testing::joined;
using crd::net::testing::number_text;
using crd::net::testing::Text;
using crd::net::testing::TextPair;
using crd::net::testing::view;
using namespace crd::net::testing;
namespace cont = crd::containers;

struct Decoder
{
    explicit Decoder(u32 max_size = 4096U) { REQUIRE(codec.init(allocator, max_size)); }

    HpackStatus decode(std::string_view block, usize field_slots = 64U, usize text_bytes = 8192U)
    {
        cont::Array<HttpField> fields;
        fields.resize(field_slots);
        cont::Array<u8> text;
        text.resize(text_bytes);
        usize count = 0U;
        const HpackStatus status = codec.decode(view(block), Span<HttpField>(fields.data(), field_slots),
                                                Span<u8>(text.data(), text_bytes), count);
        out = joined(ConstSpan<HttpField>(fields.data(), count));
        return status;
    }

    // Entry `number` (1 is the newest) as "name=value".
    [[nodiscard]] Text entry(u64 number) const
    {
        const HttpFieldTable& table = codec.table();
        const HttpFieldTable::Entry& at = table.entry(table.inserted() - number);
        Text name;
        name.resize(at.name_bytes);
        Text value;
        value.resize(at.value_bytes);
        table.copy_name(at, Span<u8>(reinterpret_cast<u8*>(name.data()), at.name_bytes));
        table.copy_value(at, Span<u8>(reinterpret_cast<u8*>(value.data()), at.value_bytes));
        return name + "=" + value;
    }

    [[nodiscard]] u64 entries() const { return codec.table().inserted() - codec.table().dropped(); }

    CountingAllocator allocator;
    HpackDecoder codec;
    Text out;
};

struct Fields
{
    Fields& add(std::string_view name, std::string_view value, bool never = false)
    {
        pairs.push_back(TextPair(Text(name), Text(value)));
        flags.push_back(never);
        return *this;
    }

    [[nodiscard]] cont::Array<HttpField> views() const
    {
        cont::Array<HttpField> all;
        for (usize index = 0U; index < pairs.size(); ++index)
        {
            all.push_back(HttpField{view(pairs[index].first), view(pairs[index].second), flags[index]});
        }
        return all;
    }

    cont::Array<TextPair> pairs;
    cont::Array<bool> flags;
};

struct Encoder
{
    explicit Encoder(u32 capacity = 4096U) { REQUIRE(codec.init(allocator, capacity)); }

    Text encode(const Fields& fields)
    {
        const cont::Array<HttpField> all = fields.views();
        Text block;
        block.resize(HpackEncoder::max_bytes(ConstSpan<HttpField>(all.data(), all.size())));
        const usize written = codec.encode(ConstSpan<HttpField>(all.data(), all.size()),
                                           Span<u8>(reinterpret_cast<u8*>(block.data()), block.size()));
        block.resize(written);
        return block;
    }

    CountingAllocator allocator;
    HpackEncoder codec;
};

Text repeated(char letter, usize count)
{
    Text out;
    out.resize(count, letter);
    return out;
}

u8 byte_at(std::string_view bytes, usize at)
{
    return static_cast<u8>(bytes[at]);
}

// A plain string: the length with the Huffman flag clear, and the bytes.
Text plain(std::string_view text)
{
    Text out;
    out.push_back(static_cast<char>(text.size()));
    out.append(text);
    return out;
}
} // namespace

// -- RFC 7541 appendix C ----------------------------------------------------------------------------------------------

TEST_CASE("hpack: C.2, one representation each", "[net][hpack]")
{
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572")) ==
              HpackStatus::Complete);
        CHECK(decoder.out == "custom-key=custom-header\n");
        CHECK(decoder.codec.table().size() == 55U);
        CHECK(decoder.entry(1U) == "custom-key=custom-header");
    }
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("040c 2f73 616d 706c 652f 7061 7468")) == HpackStatus::Complete);
        CHECK(decoder.out == ":path=/sample/path\n");
        CHECK(decoder.entries() == 0U);
    }
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("1008 7061 7373 776f 7264 0673 6563 7265 74")) == HpackStatus::Complete);
        CHECK(decoder.out == "!password=secret\n");
        CHECK(decoder.entries() == 0U);
    }
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("82")) == HpackStatus::Complete);
        CHECK(decoder.out == ":method=GET\n");
        CHECK(decoder.entries() == 0U);
    }
}

TEST_CASE("hpack: C.3, requests without Huffman coding", "[net][hpack]")
{
    Decoder decoder;
    CHECK(decoder.decode(hex("8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d")) == HpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:scheme=http\n:path=/\n:authority=www.example.com\n");
    CHECK(decoder.codec.table().size() == 57U);

    CHECK(decoder.decode(hex("8286 84be 5808 6e6f 2d63 6163 6865")) == HpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:scheme=http\n:path=/\n:authority=www.example.com\ncache-control=no-cache\n");
    CHECK(decoder.codec.table().size() == 110U);
    CHECK(decoder.entry(1U) == "cache-control=no-cache");

    CHECK(decoder.decode(hex("8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65")) ==
          HpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:scheme=https\n:path=/index.html\n:authority=www.example.com\n"
                         "custom-key=custom-value\n");
    CHECK(decoder.codec.table().size() == 164U);
    CHECK(decoder.entry(1U) == "custom-key=custom-value");
    CHECK(decoder.entry(2U) == "cache-control=no-cache");
    CHECK(decoder.entry(3U) == ":authority=www.example.com");
}

TEST_CASE("hpack: C.4, requests with Huffman coding, both directions byte for byte", "[net][hpack]")
{
    const Text first = hex("8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff");
    const Text second = hex("8286 84be 5886 a8eb 1064 9cbf");
    const Text third = hex("8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf");

    Decoder decoder;
    CHECK(decoder.decode(first) == HpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:scheme=http\n:path=/\n:authority=www.example.com\n");
    CHECK(decoder.decode(second) == HpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:scheme=http\n:path=/\n:authority=www.example.com\ncache-control=no-cache\n");
    CHECK(decoder.decode(third) == HpackStatus::Complete);
    CHECK(decoder.out == ":method=GET\n:scheme=https\n:path=/index.html\n:authority=www.example.com\n"
                         "custom-key=custom-value\n");
    CHECK(decoder.codec.table().size() == 164U);

    Encoder encoder;
    Fields request;
    request.add(":method", "GET").add(":scheme", "http").add(":path", "/").add(":authority", "www.example.com");
    CHECK(encoder.encode(request) == first);
    request.add("cache-control", "no-cache");
    CHECK(encoder.encode(request) == second);
    Fields last;
    last.add(":method", "GET").add(":scheme", "https").add(":path", "/index.html").add(":authority", "www.example.com");
    last.add("custom-key", "custom-value");
    CHECK(encoder.encode(last) == third);
    CHECK(encoder.codec.table().size() == 164U);
}

TEST_CASE("hpack: C.5, responses without Huffman coding, evicting at 256 bytes", "[net][hpack]")
{
    Decoder decoder;
    decoder.codec.set_limit(256U);
    CHECK(decoder.decode(hex("3fe1 0148 0333 3032 5807 7072 6976 6174 6561 1d4d 6f6e 2c20 3231 204f 6374 2032 3031 3320"
                             "3230 3a31 333a 3231 2047 4d54 6e17 6874 7470 733a 2f2f 7777 772e 6578 616d 706c 652e 636f"
                             "6d")) == HpackStatus::Complete);
    CHECK(decoder.out == ":status=302\ncache-control=private\ndate=Mon, 21 Oct 2013 20:13:21 GMT\n"
                         "location=https://www.example.com\n");
    CHECK(decoder.codec.table().size() == 222U);
    CHECK(decoder.entries() == 4U);

    CHECK(decoder.decode(hex("4803 3330 37c1 c0bf")) == HpackStatus::Complete);
    CHECK(decoder.out == ":status=307\ncache-control=private\ndate=Mon, 21 Oct 2013 20:13:21 GMT\n"
                         "location=https://www.example.com\n");
    CHECK(decoder.codec.table().size() == 222U);
    CHECK(decoder.entry(1U) == ":status=307");
    CHECK(decoder.entry(4U) == "cache-control=private");

    CHECK(decoder.decode(hex("88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d 54c0 5a04"
                             "677a 6970 7738 666f 6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49"
                             "553b 206d 6178 2d61 6765 3d33 3630 303b 2076 6572 7369 6f6e 3d31")) ==
          HpackStatus::Complete);
    CHECK(decoder.out == ":status=200\ncache-control=private\ndate=Mon, 21 Oct 2013 20:13:22 GMT\n"
                         "location=https://www.example.com\ncontent-encoding=gzip\n"
                         "set-cookie=foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n");
    CHECK(decoder.codec.table().size() == 215U);
    CHECK(decoder.entries() == 3U);
    CHECK(decoder.entry(1U) == "set-cookie=foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1");
    CHECK(decoder.entry(2U) == "content-encoding=gzip");
    CHECK(decoder.entry(3U) == "date=Mon, 21 Oct 2013 20:13:22 GMT");
}

TEST_CASE("hpack: C.6, responses with Huffman coding, both directions", "[net][hpack]")
{
    const Text first = hex("4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6 2d1b ff6e"
                           "919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae 43d3");
    const Text second = hex("4883 640e ffc1 c0bf");
    const Text third = hex("88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab 77ad 94e7"
                           "821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed"
                           "4ee5 b106 3d50 07");

    Decoder decoder;
    decoder.codec.set_limit(256U);
    CHECK(decoder.decode(hex("3fe1 01") + first) == HpackStatus::Complete);
    CHECK(decoder.codec.table().size() == 222U);
    CHECK(decoder.decode(second) == HpackStatus::Complete);
    CHECK(decoder.codec.table().size() == 222U);
    CHECK(decoder.decode(third) == HpackStatus::Complete);
    CHECK(decoder.out == ":status=200\ncache-control=private\ndate=Mon, 21 Oct 2013 20:13:22 GMT\n"
                         "location=https://www.example.com\ncontent-encoding=gzip\n"
                         "set-cookie=foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n");
    CHECK(decoder.codec.table().size() == 215U);

    // location rarely repeats, so this encoder leaves it out of the table; the rest of C.6 is what it writes.
    Encoder encoder(256U);
    Fields response;
    response.add(":status", "302").add("cache-control", "private").add("date", "Mon, 21 Oct 2013 20:13:21 GMT");
    CHECK(encoder.encode(response) ==
          hex("3fe1 01 4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6 2d1b ff"));
    Fields again;
    again.add(":status", "307").add("cache-control", "private").add("date", "Mon, 21 Oct 2013 20:13:21 GMT");
    // "307" is three bytes either way, and a string takes its Huffman form only when strictly shorter.
    CHECK(encoder.encode(again) == hex("4803 3330 37c0 bf"));
}

TEST_CASE("hpack: the static table, every index", "[net][hpack]")
{
    struct Row
    {
        u32 index;
        const char* line;
    };
    const Row rows[] = {
        {1U, ":authority=\n"},
        {2U, ":method=GET\n"},
        {3U, ":method=POST\n"},
        {4U, ":path=/\n"},
        {5U, ":path=/index.html\n"},
        {6U, ":scheme=http\n"},
        {7U, ":scheme=https\n"},
        {8U, ":status=200\n"},
        {9U, ":status=204\n"},
        {10U, ":status=206\n"},
        {11U, ":status=304\n"},
        {12U, ":status=400\n"},
        {13U, ":status=404\n"},
        {14U, ":status=500\n"},
        {15U, "accept-charset=\n"},
        {16U, "accept-encoding=gzip, deflate\n"},
        {17U, "accept-language=\n"},
        {18U, "accept-ranges=\n"},
        {19U, "accept=\n"},
        {20U, "access-control-allow-origin=\n"},
        {21U, "age=\n"},
        {22U, "allow=\n"},
        {23U, "authorization=\n"},
        {24U, "cache-control=\n"},
        {25U, "content-disposition=\n"},
        {26U, "content-encoding=\n"},
        {27U, "content-language=\n"},
        {28U, "content-length=\n"},
        {29U, "content-location=\n"},
        {30U, "content-range=\n"},
        {31U, "content-type=\n"},
        {32U, "cookie=\n"},
        {33U, "date=\n"},
        {34U, "etag=\n"},
        {35U, "expect=\n"},
        {36U, "expires=\n"},
        {37U, "from=\n"},
        {38U, "host=\n"},
        {39U, "if-match=\n"},
        {40U, "if-modified-since=\n"},
        {41U, "if-none-match=\n"},
        {42U, "if-range=\n"},
        {43U, "if-unmodified-since=\n"},
        {44U, "last-modified=\n"},
        {45U, "link=\n"},
        {46U, "location=\n"},
        {47U, "max-forwards=\n"},
        {48U, "proxy-authenticate=\n"},
        {49U, "proxy-authorization=\n"},
        {50U, "range=\n"},
        {51U, "referer=\n"},
        {52U, "refresh=\n"},
        {53U, "retry-after=\n"},
        {54U, "server=\n"},
        {55U, "set-cookie=\n"},
        {56U, "strict-transport-security=\n"},
        {57U, "transfer-encoding=\n"},
        {58U, "user-agent=\n"},
        {59U, "vary=\n"},
        {60U, "via=\n"},
        {61U, "www-authenticate=\n"},
    };
    Decoder decoder;
    u32 expected = 1U;
    for (const Row& row : rows)
    {
        REQUIRE(row.index == expected);
        Text block;
        block.push_back(static_cast<char>(0x80U | row.index));
        CHECK(decoder.decode(block) == HpackStatus::Complete);
        CHECK(decoder.out == row.line);
        ++expected;
    }
    CHECK(expected == kHpackStaticEntries + 1U);
    CHECK(decoder.decode(hex("be")) == HpackStatus::Failed); // 62: nothing is in the dynamic table
}

// -- refusals --------------------------------------------------------------------------------------------------------

TEST_CASE("hpack: what the decoder refuses", "[net][hpack]")
{
    const auto refused = [](const Text& block)
    {
        Decoder decoder;
        CHECK(decoder.decode(block) == HpackStatus::Failed);
        CHECK(decoder.codec.failed());
        CHECK(decoder.decode(hex("82")) == HpackStatus::Failed); // it stays failed
    };
    SECTION("index zero")
    {
        refused(hex("80"));
    }
    SECTION("an index past the table")
    {
        refused(hex("be"));
    }
    SECTION("a name index past the table")
    {
        refused(hex("7e 0161"));
    }
    SECTION("an unindexed name past the table")
    {
        refused(hex("0f30 0161"));
    }
    SECTION("a never-indexed name past the table")
    {
        refused(hex("1f30 0161"));
    }
    SECTION("an index that ends early")
    {
        refused(hex("ff"));
    }
    SECTION("an index past what 62 bits hold")
    {
        refused(hex("ffff ffff ffff ffff ffff ffff 7f"));
    }
    SECTION("no value")
    {
        refused(hex("44"));
    }
    SECTION("no name")
    {
        refused(hex("40"));
    }
    SECTION("no value after a literal name")
    {
        refused(hex("40 0161"));
    }
    SECTION("a name longer than the block")
    {
        refused(hex("40 0561"));
    }
    SECTION("a value longer than the block")
    {
        refused(hex("44 0561"));
    }
    SECTION("a string length that ends early")
    {
        refused(hex("44 ff"));
    }
    SECTION("a Huffman string padded with zeros")
    {
        refused(hex("44 81 00"));
    }
    SECTION("a Huffman string padded by a whole byte")
    {
        refused(hex("44 82 1f ff"));
    }
    SECTION("a Huffman string holding EOS")
    {
        refused(hex("44 84 ff ff ff ff"));
    }
    SECTION("a Huffman name that fails")
    {
        refused(hex("40 81 00 0161"));
    }
}

TEST_CASE("hpack: an empty block, and empty names and values", "[net][hpack]")
{
    Decoder decoder;
    CHECK(decoder.decode(Text()) == HpackStatus::Complete);
    CHECK(decoder.out.empty());
    CHECK(decoder.decode(hex("00 00 00")) == HpackStatus::Complete); // HTTP's rules on names are the caller's
    CHECK(decoder.out == "=\n");
    CHECK(decoder.decode(hex("40 00 00")) == HpackStatus::Complete);
    CHECK(decoder.codec.table().size() == kHttpFieldOverhead);
    CHECK(decoder.decode(hex("be")) == HpackStatus::Complete);
    CHECK(decoder.out == "=\n");
}

TEST_CASE("hpack: an index that shifts inside a block", "[net][hpack]")
{
    Decoder decoder;
    // Insert a=1, reference it as 62, insert b=2, then a=1 is 63 and b=2 is 62.
    Text block = hex("40") + plain("a") + plain("1") + hex("be 40") + plain("b") + plain("2") + hex("bf be");
    CHECK(decoder.decode(block) == HpackStatus::Complete);
    CHECK(decoder.out == "a=1\na=1\nb=2\na=1\nb=2\n");
    // A literal naming entry 62 takes its name; of the three entries now, 65 is past the last.
    CHECK(decoder.decode(hex("7e") + plain("3")) == HpackStatus::Complete);
    CHECK(decoder.out == "b=3\n");
    CHECK(decoder.entry(1U) == "b=3");
    CHECK(decoder.decode(hex("c1")) == HpackStatus::Failed);
}

// -- the table
// ---------------------------------------------------------------------------------------------------------

TEST_CASE("hpack: size updates", "[net][hpack]")
{
    SECTION("evicts, and zero clears")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("40") + plain("a") + plain("1")) == HpackStatus::Complete);
        CHECK(decoder.decode(hex("40") + plain("b") + plain("2")) == HpackStatus::Complete);
        CHECK(decoder.entries() == 2U);
        CHECK(decoder.decode(hex("3f 16")) == HpackStatus::Complete); // 53: one entry fits, the newest
        CHECK(decoder.codec.table().capacity() == 53U);
        CHECK(decoder.entries() == 1U);
        CHECK(decoder.entry(1U) == "b=2");
        CHECK(decoder.decode(hex("20")) == HpackStatus::Complete);
        CHECK(decoder.entries() == 0U);
        CHECK(decoder.codec.table().size() == 0U);
    }
    SECTION("above the limit")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("3f e2 1f")) == HpackStatus::Failed); // 4097
    }
    SECTION("at the limit")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("3f e1 1f")) == HpackStatus::Complete); // 4096
        CHECK(decoder.codec.table().capacity() == 4096U);
    }
    SECTION("a limit raised may be used")
    {
        Decoder decoder(8192U);
        decoder.codec.set_limit(8192U);
        CHECK(decoder.codec.table().capacity() == 4096U);                // the peer decides when
        CHECK(decoder.decode(hex("3f e1 3f")) == HpackStatus::Complete); // 8192
        CHECK(decoder.codec.table().capacity() == 8192U);
    }
    SECTION("after a field")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("82 20")) == HpackStatus::Failed);
    }
    SECTION("two, and not three")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("20 3f e1 1f 82")) == HpackStatus::Complete);
        CHECK(decoder.codec.table().capacity() == 4096U);
        CHECK(decoder.decode(hex("20 20 20")) == HpackStatus::Failed);
    }
    SECTION("the integer ends early")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("3f")) == HpackStatus::Failed);
    }
    SECTION("a lowered limit obliges the next block")
    {
        Decoder decoder;
        decoder.codec.set_limit(100U);
        CHECK(decoder.decode(hex("82")) == HpackStatus::Failed);
    }
    SECTION("an empty block does not discharge it")
    {
        Decoder decoder;
        decoder.codec.set_limit(100U);
        CHECK(decoder.decode(Text()) == HpackStatus::Failed);
    }
    SECTION("an update above the lowered limit does not either")
    {
        Decoder decoder;
        decoder.codec.set_limit(100U);
        CHECK(decoder.decode(hex("3f 46 82")) == HpackStatus::Failed); // 101
    }
    SECTION("an update at the limit does, once")
    {
        Decoder decoder;
        decoder.codec.set_limit(100U);
        CHECK(decoder.decode(hex("3f 45 82")) == HpackStatus::Complete); // 100
        CHECK(decoder.codec.table().capacity() == 100U);
        CHECK(decoder.decode(hex("82")) == HpackStatus::Complete);
    }
    SECTION("lowered then raised: the smallest first, then up to the limit")
    {
        Decoder decoder;
        decoder.codec.set_limit(100U);
        decoder.codec.set_limit(4096U);
        CHECK(decoder.decode(hex("3f 46 82")) == HpackStatus::Failed); // 101 first
    }
    SECTION("lowered then raised, signalled right")
    {
        Decoder decoder;
        decoder.codec.set_limit(100U);
        decoder.codec.set_limit(4096U);
        CHECK(decoder.decode(hex("3f 45 3f e1 1f 82")) == HpackStatus::Complete);
        CHECK(decoder.codec.table().capacity() == 4096U);
    }
    SECTION("lowered twice: the smaller binds")
    {
        Decoder decoder;
        decoder.codec.set_limit(50U);
        decoder.codec.set_limit(100U);
        CHECK(decoder.decode(hex("3f 14 82")) == HpackStatus::Failed); // 51
        Decoder other;
        other.codec.set_limit(50U);
        other.codec.set_limit(100U);
        CHECK(other.decode(hex("3f 13 82")) == HpackStatus::Complete); // 50
    }
    SECTION("a limit raised after the table shrank obliges nothing")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("3f 45")) == HpackStatus::Complete); // 100
        decoder.codec.set_limit(4096U);
        CHECK(decoder.decode(hex("82")) == HpackStatus::Complete);
    }
    SECTION("a limit above the table's size obliges nothing")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("3f 45")) == HpackStatus::Complete);
        decoder.codec.set_limit(1000U);
        CHECK(decoder.decode(hex("82")) == HpackStatus::Complete);
    }
}

TEST_CASE("hpack: an entry larger than the table empties it", "[net][hpack]")
{
    Decoder decoder;
    CHECK(decoder.decode(hex("3f 3a")) == HpackStatus::Complete); // 89
    CHECK(decoder.decode(hex("40") + plain("a") + plain("1")) == HpackStatus::Complete);
    CHECK(decoder.entries() == 1U);
    const Text value = repeated('v', 57U);
    CHECK(decoder.decode(hex("40") + plain("b") + plain(value)) == HpackStatus::Complete); // 90
    CHECK(decoder.out == "b=" + value + "\n");
    CHECK(decoder.entries() == 0U);
    CHECK(decoder.codec.table().size() == 0U);
    CHECK_FALSE(decoder.codec.failed());
    // Exactly the capacity fits.
    const Text fits = repeated('w', 56U);
    CHECK(decoder.decode(hex("40") + plain("c") + plain(fits)) == HpackStatus::Complete); // 89
    CHECK(decoder.entries() == 1U);
    CHECK(decoder.codec.table().size() == 89U);
}

TEST_CASE("hpack: an entry naming the one it evicts", "[net][hpack]")
{
    Decoder decoder;
    CHECK(decoder.decode(hex("3f 3a")) == HpackStatus::Complete);                                          // 89
    CHECK(decoder.decode(hex("40") + plain("name") + plain(repeated('v', 50U))) == HpackStatus::Complete); // 86
    // Index 62 is that entry; adding a value of 53 bytes needs all of the table.
    CHECK(decoder.decode(hex("7e") + plain(repeated('w', 53U))) == HpackStatus::Complete);
    CHECK(decoder.out == "name=" + repeated('w', 53U) + "\n");
    CHECK(decoder.entries() == 1U);
    CHECK(decoder.entry(1U) == "name=" + repeated('w', 53U));
    // One naming a table entry and too large: the name still comes back and the table empties.
    CHECK(decoder.decode(hex("7e") + plain(repeated('x', 60U))) == HpackStatus::Complete);
    CHECK(decoder.out == "name=" + repeated('x', 60U) + "\n");
    CHECK(decoder.entries() == 0U);
}

TEST_CASE("hpack: the table wraps over many insertions", "[net][hpack]")
{
    Decoder decoder;
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const Text name = "n" + number_text(round % 17U);
        const Text value = repeated(static_cast<char>('a' + (round % 26U)), (round * 7U) % 90U);
        REQUIRE(decoder.decode(hex("40") + plain(name) + plain(value)) == HpackStatus::Complete);
        REQUIRE(decoder.entry(1U) == name + "=" + value);
        REQUIRE(decoder.decode(hex("be")) == HpackStatus::Complete);
        REQUIRE(decoder.out == name + "=" + value + "\n");
    }
}

// -- storage
// -----------------------------------------------------------------------------------------------------------

TEST_CASE("hpack: a block past its storage keeps the table in step", "[net][hpack]")
{
    const Text one = hex("40") + plain("first") + plain("one");
    const Text huffman = hex("40 88 25a8 49e9 5ba9 7d7f 89 25a8 49e9 5bb8 e8b4 bf"); // custom-key: custom-value

    SECTION("no room for the text")
    {
        Decoder decoder;
        CHECK(decoder.decode(one, 8U, 3U) == HpackStatus::TooLarge);
        CHECK(decoder.out.empty());
        CHECK(decoder.entries() == 1U);
        CHECK(decoder.entry(1U) == "first=one");
    }
    SECTION("no room for a Huffman value")
    {
        Decoder decoder;
        CHECK(decoder.decode(huffman, 8U, 12U) == HpackStatus::TooLarge);
        CHECK(decoder.entry(1U) == "custom-key=custom-value");
    }
    SECTION("no room for anything, Huffman or not")
    {
        Decoder decoder;
        CHECK(decoder.decode(huffman + one, 8U, 0U) == HpackStatus::TooLarge);
        CHECK(decoder.entries() == 2U);
        CHECK(decoder.entry(1U) == "first=one");
        CHECK(decoder.entry(2U) == "custom-key=custom-value");
    }
    SECTION("no room for the fields")
    {
        Decoder decoder;
        CHECK(decoder.decode(hex("82") + one, 1U, 64U) == HpackStatus::TooLarge);
        CHECK(decoder.entry(1U) == "first=one");
    }
    SECTION("a dynamic name past the storage")
    {
        Decoder decoder;
        CHECK(decoder.decode(one) == HpackStatus::Complete);
        CHECK(decoder.decode(hex("7e") + plain("two"), 8U, 2U) == HpackStatus::TooLarge);
        CHECK(decoder.entries() == 2U);
        CHECK(decoder.entry(1U) == "first=two");
    }
    SECTION("an indexed dynamic entry past the storage")
    {
        Decoder decoder;
        CHECK(decoder.decode(one) == HpackStatus::Complete);
        CHECK(decoder.decode(hex("be"), 8U, 1U) == HpackStatus::TooLarge);
    }
    SECTION("past the storage and too large for the table: it still empties")
    {
        Decoder decoder;
        CHECK(decoder.decode(one) == HpackStatus::Complete);
        CHECK(decoder.decode(hex("3f 3a")) == HpackStatus::Complete); // 89, keeps it
        CHECK(decoder.entries() == 1U);
        CHECK(decoder.decode(hex("40") + plain("a") + plain(repeated('v', 120U)), 8U, 4U) == HpackStatus::TooLarge);
        CHECK(decoder.entries() == 0U);
    }
    SECTION("a Huffman line past the spill too: it empties the table")
    {
        Decoder decoder;
        CHECK(decoder.decode(one) == HpackStatus::Complete);
        // 5000 bytes of the 5-bit code of '0' are 8000 characters, more than a 4096-byte table's scratch holds.
        Text coded;
        coded.resize(5000U, static_cast<char>(0x00U));
        const Text block = hex("40") + plain("b") + hex("ff 89 26") + coded;
        CHECK(decoder.decode(block, 8U, 4U) == HpackStatus::TooLarge);
        CHECK(decoder.entries() == 0U);
        CHECK_FALSE(decoder.codec.failed());
    }
    SECTION("a Huffman line after the storage ran out goes to the spill, though the storage has room for it")
    {
        Decoder decoder;
        const Text overflowing = hex("00") + plain(repeated('n', 20U)) + plain("v");
        CHECK(decoder.decode(overflowing + hex("418c f1e3 c2e5 f23a 6ba0 ab90 f4ff"), 8U, 16U) == HpackStatus::TooLarge);
        CHECK(decoder.entry(1U) == ":authority=www.example.com");
    }
    SECTION("a refusal found past the storage is still a refusal")
    {
        Decoder decoder;
        CHECK(decoder.decode(one + hex("80"), 8U, 3U) == HpackStatus::Failed);
    }
    SECTION("a block that referenced what the larger one added")
    {
        Decoder decoder;
        CHECK(decoder.decode(one, 8U, 3U) == HpackStatus::TooLarge);
        CHECK(decoder.decode(hex("be")) == HpackStatus::Complete);
        CHECK(decoder.out == "first=one\n");
    }
}

TEST_CASE("hpack: a block of references costs its length, not what it expands to", "[net][hpack]")
{
    Decoder decoder;
    const Text entry = hex("40") + plain("n") + plain(repeated('v', 120U));
    CHECK(decoder.decode(entry) == HpackStatus::Complete);
    Text block;
    block.resize(1U << 20U, static_cast<char>(0xBEU));
    CHECK(decoder.decode(block, 16U, 1024U) == HpackStatus::TooLarge);
    CHECK_FALSE(decoder.codec.failed());
}

// -- the encoder
// -------------------------------------------------------------------------------------------------------

TEST_CASE("hpack: what the encoder writes", "[net][hpack]")
{
    SECTION("a static field is one byte")
    {
        Encoder encoder;
        Fields fields;
        fields.add(":method", "GET");
        CHECK(encoder.encode(fields) == hex("82"));
        CHECK(encoder.codec.table().inserted() == 0U);
    }
    SECTION("a field is indexed the first time and referenced the second")
    {
        Encoder encoder;
        Fields fields;
        fields.add("x-a", "1");
        CHECK(encoder.encode(fields).size() > 1U);
        CHECK(encoder.codec.table().inserted() == 1U);
        CHECK(encoder.encode(fields) == hex("be"));
    }
    SECTION("a dynamic name is reused with another value")
    {
        Encoder encoder;
        Fields first;
        first.add("x-name-long-enough", "1");
        (void)encoder.encode(first);
        Fields second;
        second.add("x-name-long-enough", "2");
        const Text block = encoder.encode(second);
        CHECK(byte_at(block, 0) == 0x7EU);
        CHECK(block.size() == 3U);
    }
    SECTION("a static name is preferred to a dynamic one")
    {
        Encoder encoder;
        Fields first;
        first.add("server", "1");
        const Text block = encoder.encode(first);
        CHECK(byte_at(block, 0) == (0x40U | 54U));
        Fields second;
        second.add("server", "2");
        CHECK(byte_at(encoder.encode(second), 0) == (0x40U | 54U));
    }
    SECTION("sensitive fields are never indexed and carry the flag")
    {
        for (const char* name : {"authorization", "proxy-authorization", "cookie", "set-cookie"})
        {
            Encoder encoder;
            Fields fields;
            fields.add(name, "secret");
            const Text block = encoder.encode(fields);
            CHECK((byte_at(block, 0) & 0xF0U) == 0x10U);
            CHECK(encoder.codec.table().inserted() == 0U);
            CHECK(encoder.encode(fields) == block); // not referenced the second time
            Decoder decoder;
            CHECK(decoder.decode(block) == HpackStatus::Complete);
            CHECK(decoder.out == "!" + Text(name) + "=secret\n");
        }
    }
    SECTION("a field marked never_index is never indexed, nor referenced if it is already there")
    {
        Encoder encoder;
        Fields indexed;
        indexed.add("x-a", "1");
        (void)encoder.encode(indexed);
        Fields secret;
        secret.add("x-a", "1", true);
        const Text block = encoder.encode(secret);
        CHECK((byte_at(block, 0) & 0xF0U) == 0x10U);
        CHECK(byte_at(block, 0U) == 0x1FU); // a name reference, 62: only the value stays out of the table
        CHECK(block.size() == 4U);
        CHECK(encoder.codec.table().inserted() == 1U);
    }
    SECTION("a static field marked never_index is written as a literal")
    {
        Encoder encoder;
        Fields fields;
        fields.add(":method", "GET", true);
        const Text block = encoder.encode(fields);
        CHECK(byte_at(block, 0) == 0x12U);
        Decoder decoder;
        CHECK(decoder.decode(block) == HpackStatus::Complete);
        CHECK(decoder.out == "!:method=GET\n");
    }
    SECTION("a field over three quarters of the table stays out")
    {
        Encoder encoder(1000U);
        Fields small;
        small.add("x-a", repeated('v', 1000U - 250U - 32U - 3U));
        (void)encoder.encode(small);
        CHECK(encoder.codec.table().inserted() == 1U);
        Fields large;
        large.add("x-b", repeated('v', 1000U - 250U - 32U - 3U + 1U));
        const Text block = encoder.encode(large);
        CHECK(encoder.codec.table().inserted() == 1U);
        CHECK((byte_at(block, 0) & 0xF0U) == 0x00U);
    }
    SECTION("a field that rarely repeats is written beside the table, not into it")
    {
        for (const char* name :
             {":path", "age", "content-length", "etag", "if-modified-since", "if-none-match", "location"})
        {
            Encoder encoder;
            Fields fields;
            fields.add(name, "one-off");
            const Text block = encoder.encode(fields);
            CHECK((byte_at(block, 0U) & 0xF0U) == 0x00U);
            CHECK(encoder.codec.table().inserted() == 0U);
        }
        Encoder encoder;
        Fields exact;
        exact.add(":path", "/");
        CHECK(encoder.encode(exact) == hex("84")); // the static table still answers
    }
    SECTION("a table of nothing writes literals")
    {
        Encoder encoder(0U);
        Fields fields;
        fields.add("x-a", "1");
        const Text block = encoder.encode(fields);
        CHECK(byte_at(block, 0) == 0x20U); // the update to 0
        CHECK(byte_at(block, 1) == 0x00U);
        CHECK(encoder.codec.table().inserted() == 0U);
        Decoder decoder;
        CHECK(decoder.decode(block) == HpackStatus::Complete);
        CHECK(decoder.codec.table().capacity() == 0U);
    }
}

TEST_CASE("hpack: the updates the encoder signals", "[net][hpack]")
{
    Fields fields;
    fields.add(":method", "GET");
    SECTION("none at the default")
    {
        Encoder encoder;
        CHECK(encoder.encode(fields) == hex("82"));
        encoder.codec.set_peer_limit(4096U);
        CHECK(encoder.encode(fields) == hex("82"));
    }
    SECTION("a table of ours under the default is signalled once")
    {
        Encoder encoder(100U);
        CHECK(encoder.encode(fields) == hex("3f 45 82"));
        CHECK(encoder.encode(fields) == hex("82"));
    }
    SECTION("the peer lowers it")
    {
        Encoder encoder;
        encoder.codec.set_peer_limit(100U);
        CHECK(encoder.encode(fields) == hex("3f 45 82"));
        CHECK(encoder.codec.table().capacity() == 100U);
    }
    SECTION("the peer lowers and raises it between blocks: two updates, the smallest first")
    {
        Encoder encoder;
        encoder.codec.set_peer_limit(100U);
        encoder.codec.set_peer_limit(4096U);
        CHECK(encoder.encode(fields) == hex("3f 45 3f e1 1f 82"));
        CHECK(encoder.codec.table().capacity() == 4096U);
        CHECK(encoder.encode(fields) == hex("82")); // said once
    }
    SECTION("the peer lowers it twice: one update")
    {
        Encoder encoder;
        encoder.codec.set_peer_limit(1000U);
        encoder.codec.set_peer_limit(100U);
        CHECK(encoder.encode(fields) == hex("3f 45 82"));
    }
    SECTION("a raise is used only up to what this side allows")
    {
        Encoder encoder;
        encoder.codec.set_peer_limit(65536U);
        CHECK(encoder.encode(fields) == hex("82"));
    }
    SECTION("a raise this side allows is signalled")
    {
        Encoder encoder(8192U);
        CHECK(encoder.codec.table().capacity() == 4096U);
        encoder.codec.set_peer_limit(8192U);
        CHECK(encoder.encode(fields) == hex("3f e1 3f 82"));
        CHECK(encoder.codec.table().capacity() == 8192U);
    }
    SECTION("lowering evicts")
    {
        Encoder encoder;
        Fields one;
        one.add("x-a", "1");
        (void)encoder.encode(one);
        CHECK(encoder.codec.table().size() != 0U);
        encoder.codec.set_peer_limit(0U);
        (void)encoder.encode(fields);
        CHECK(encoder.codec.table().size() == 0U);
        CHECK(encoder.codec.table().capacity() == 0U);
    }
    SECTION("lowered to nothing and raised again: entries are gone on both sides")
    {
        Encoder encoder;
        Decoder decoder;
        Fields one;
        one.add("x-a", "1");
        CHECK(decoder.decode(encoder.encode(one)) == HpackStatus::Complete);
        encoder.codec.set_peer_limit(0U);
        encoder.codec.set_peer_limit(4096U);
        const Text block = encoder.encode(one);
        CHECK(byte_at(block, 0) == 0x20U);
        CHECK(decoder.decode(block) == HpackStatus::Complete);
        CHECK(decoder.out == "x-a=1\n");
        CHECK(decoder.entries() == 1U);
    }
}

TEST_CASE("hpack: a block that does not fit changes nothing", "[net][hpack]")
{
    Encoder encoder;
    Fields one;
    one.add("x-field", "value");
    const cont::Array<HttpField> all = one.views();
    const ConstSpan<HttpField> fields(all.data(), all.size());
    encoder.codec.set_peer_limit(100U);
    u8 small[8];
    CHECK(encoder.codec.encode(fields, Span<u8>(small)) == 0U);
    CHECK(encoder.codec.table().inserted() == 0U);
    CHECK(encoder.codec.table().capacity() == 4096U);
    const Text block = encoder.encode(one);
    CHECK(byte_at(block, 0) == 0x3FU); // the update is still owed
    CHECK(encoder.codec.table().inserted() == 1U);
}

TEST_CASE("hpack: random blocks through a real decoder across SETTINGS changes", "[net][hpack]")
{
    std::mt19937 random(0x48504143U);
    Encoder encoder(16384U);
    Decoder decoder(16384U);
    u32 allowed = 4096U;
    const u32 sizes[] = {0U, 31U, 32U, 64U, 256U, 1024U, 4096U, 16384U};
    for (u32 round = 0U; round < 4000U; ++round)
    {
        if ((random() % 16U) == 0U)
        {
            const usize changes = (random() % 3U) + 1U;
            for (usize change = 0U; change < changes; ++change)
            {
                allowed = sizes[random() % 8U];
                decoder.codec.set_limit(allowed);
                encoder.codec.set_peer_limit(allowed);
            }
        }
        Fields fields;
        const usize lines = random() % 12U;
        Text expected;
        for (usize line = 0U; line < lines; ++line)
        {
            Text name = "x-" + number_text(random() % 24U);
            Text value = repeated(static_cast<char>('a' + (random() % 26U)), random() % 40U);
            const bool never = (random() % 8U) == 0U;
            expected += (never ? "!" : "");
            expected += name + "=" + value + "\n";
            fields.add(name, value, never);
        }
        const Text block = encoder.encode(fields);
        REQUIRE(decoder.decode(block) == HpackStatus::Complete);
        REQUIRE(decoder.out == expected);
        REQUIRE(decoder.codec.table().size() == encoder.codec.table().size());
        REQUIRE(decoder.codec.table().inserted() - decoder.codec.table().dropped() ==
                encoder.codec.table().inserted() - encoder.codec.table().dropped());
    }
}

TEST_CASE("hpack: configurations and allocation", "[net][hpack]")
{
    {
        CountingAllocator allocator;
        HpackDecoder codec;
        CHECK_FALSE(codec.init(allocator, kHpackMaxTableSize + 1U));
        CHECK(codec.init(allocator, kHpackMaxTableSize));
        CHECK_FALSE(codec.init(allocator, 4096U)); // once
        CHECK(codec.table().capacity() == kHpackDefaultTableSize);
    }
    {
        CountingAllocator allocator;
        HpackEncoder codec;
        CHECK_FALSE(codec.init(allocator, kHpackMaxTableSize + 1U));
        CHECK(codec.init(allocator, kHpackMaxTableSize));
        CHECK_FALSE(codec.init(allocator, 4096U));
    }
    // A decoder holds the default even when this side will ask for less: a peer may use it until acknowledged.
    {
        CountingAllocator allocator;
        HpackDecoder codec;
        CHECK(codec.init(allocator, 0U));
        CHECK(codec.table().max_capacity() == kHpackDefaultTableSize);
    }
    // A refusal at each allocation leaves nothing.
    for (usize refuse = 0U; refuse < 3U; ++refuse)
    {
        CountingAllocator limited;
        limited.refuse_after(refuse);
        HpackDecoder codec;
        CHECK_FALSE(codec.init(limited, 4096U));
    }
    for (usize refuse = 0U; refuse < 2U; ++refuse)
    {
        CountingAllocator limited;
        limited.refuse_after(refuse);
        HpackEncoder codec;
        CHECK_FALSE(codec.init(limited, 4096U));
    }
    // Nothing allocates after init().
    {
        Decoder decoder;
        Encoder encoder;
        const usize decoder_calls = decoder.allocator.calls();
        const usize encoder_calls = encoder.allocator.calls();
        Fields fields;
        fields.add("x-a", "1").add(":method", "GET");
        for (u32 round = 0U; round < 32U; ++round)
        {
            CHECK(decoder.decode(encoder.encode(fields)) == HpackStatus::Complete);
        }
        CHECK(decoder.allocator.calls() == decoder_calls);
        CHECK(encoder.allocator.calls() == encoder_calls);
    }
}
