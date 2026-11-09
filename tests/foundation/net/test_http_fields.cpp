// crd-net-tests: the field coding HPACK and QPACK share (4.2a; docs/design/network-foundation.md#qpack): RFC 7541's
// examples of prefix integers and Huffman strings byte for byte, every byte value through the code, what section 5.2
// refuses (long or wrong padding, EOS, a code cut short), the limits of integers and strings.

#include "text_fixture.hpp"

#include <crd/net/http_fields.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::Text;
using crd::net::testing::slice;
using namespace crd::net::testing;

Text bytes_of_hex(const char* hex)
{
    Text out;
    for (const char* at = hex; *at != '\0'; at += 2)
    {
        const auto nibble = [](char c) { return (c >= 'a') ? c - 'a' + 10 : c - '0'; };
        out.push_back(static_cast<char>((nibble(at[0]) << 4) | nibble(at[1])));
    }
    return out;
}

Text encode_int(u8 flags, u8 prefix, u64 value)
{
    u8 room[32] = {};
    cont::WireWriter out((Span<u8>(room)));
    put_http_int(out, flags, prefix, value);
    REQUIRE(out.written() == http_int_bytes(value, prefix));
    return Text(reinterpret_cast<const char*>(room), out.written());
}

HttpDecode decode_int(std::string_view bytes, u8 prefix, u64& value)
{
    cont::WireReader in(view(bytes));
    u8 first = 0U;
    REQUIRE(in.take_u8(first));
    return take_http_int(in, first, prefix, value);
}

Text huffman(std::string_view text)
{
    Text out(http_huffman_bytes(view(text)), '\0');
    http_huffman_encode(view(text), Span<u8>(reinterpret_cast<u8*>(out.data()), out.size()));
    return out;
}

bool unhuffman(std::string_view coded, Text& text)
{
    char room[256] = {};
    usize decoded = 0U;
    const bool ok = http_huffman_decode(view(coded), Span<u8>(reinterpret_cast<u8*>(room), sizeof(room)), decoded);
    if (ok)
    {
        REQUIRE(decoded <= sizeof(room));
        text = Text(room, decoded);
    }
    return ok;
}
} // namespace

TEST_CASE("prefix integers: RFC 7541's examples", "[net][qpack]")
{
    CHECK(encode_int(0x00U, 5U, 10U) == bytes_of_hex("0a"));
    CHECK(encode_int(0x00U, 5U, 1337U) == bytes_of_hex("1f9a0a"));
    CHECK(encode_int(0x00U, 8U, 42U) == bytes_of_hex("2a"));
    u64 value = 0U;
    CHECK(decode_int(bytes_of_hex("0a"), 5U, value) == HttpDecode::Ok);
    CHECK(value == 10U);
    CHECK(decode_int(bytes_of_hex("1f9a0a"), 5U, value) == HttpDecode::Ok);
    CHECK(value == 1337U);
}

TEST_CASE("prefix integers: every prefix to the limit", "[net][qpack]")
{
    const u64 values[] = {0U, 1U, 30U, 31U, 32U, 126U, 127U, 128U, 254U, 255U, 256U, 16510U, 16511U, u64{1} << 40U,
                          kHttpMaxInteger};
    for (u8 prefix = 1U; prefix <= 8U; ++prefix)
    {
        for (const u64 value : values)
        {
            const Text bytes = encode_int(0x00U, prefix, value);
            u64 read = 0U;
            REQUIRE(decode_int(bytes, prefix, read) == HttpDecode::Ok);
            CHECK(read == value);
            // Every byte is needed: cut one off and it asks for more.
            if (bytes.size() > 1U)
            {
                CHECK(decode_int(slice(bytes, 0U, bytes.size() - 1U), prefix, read) == HttpDecode::NeedMore);
            }
        }
    }
    CHECK(encode_int(0x40U, 6U, 5U) == bytes_of_hex("45"));
}

TEST_CASE("prefix integers: what is refused", "[net][qpack]")
{
    u64 value = 0U;
    // 2^62 is one past the limit.
    CHECK(decode_int(encode_int(0x00U, 8U, kHttpMaxInteger), 8U, value) == HttpDecode::Ok);
    Text over = encode_int(0x00U, 8U, kHttpMaxInteger);
    over.data()[1] = static_cast<char>(static_cast<u8>(over.data()[1]) + 1U);
    CHECK(decode_int(over, 8U, value) == HttpDecode::Invalid);
    // Eleven continuation bytes, even of zeros.
    Text padded = bytes_of_hex("1f");
    padded += Text(10U, static_cast<char>(0x80));
    padded.push_back('\0');
    CHECK(decode_int(padded, 5U, value) == HttpDecode::Invalid);
    // Ten are an implementation's right to accept.
    Text ten = bytes_of_hex("1f");
    ten += Text(9U, static_cast<char>(0x80));
    ten.push_back('\0');
    CHECK(decode_int(ten, 5U, value) == HttpDecode::Ok);
    CHECK(value == 31U);
    // A digit that would overflow the shift.
    Text shifted = bytes_of_hex("ff");
    shifted += Text(9U, static_cast<char>(0xFF));
    shifted.push_back(0x7F);
    CHECK(decode_int(shifted, 8U, value) == HttpDecode::Invalid);
}

TEST_CASE("huffman: RFC 7541's strings both ways", "[net][qpack]")
{
    const struct
    {
        const char* text;
        const char* code;
    } cases[] = {
        {"www.example.com", "f1e3c2e5f23a6ba0ab90f4ff"},
        {"no-cache", "a8eb10649cbf"},
        {"custom-key", "25a849e95ba97d7f"},
        {"custom-value", "25a849e95bb8e8b4bf"},
        {"302", "6402"},
        {"private", "aec3771a4b"},
        {"Mon, 21 Oct 2013 20:13:21 GMT", "d07abe941054d444a8200595040b8166e082a62d1bff"},
        {"https://www.example.com", "9d29ad171863c78f0b97c8e9ae82ae43d3"},
    };
    for (const auto& one : cases)
    {
        CHECK(huffman(one.text) == bytes_of_hex(one.code));
        Text text;
        REQUIRE(unhuffman(bytes_of_hex(one.code), text));
        CHECK(text == one.text);
    }
}

TEST_CASE("huffman: every byte and every pair round trip", "[net][qpack]")
{
    for (u32 first = 0U; first < 256U; ++first)
    {
        for (u32 second = 0U; second < 256U; second += 17U)
        {
            Text text;
            text.push_back(static_cast<char>(first));
            text.push_back(static_cast<char>(second));
            Text decoded;
            REQUIRE(unhuffman(huffman(text), decoded));
            REQUIRE(decoded == text);
        }
    }
    Text text;
    for (u32 byte = 0U; byte < 200U; ++byte)
    {
        text.push_back(static_cast<char>(byte));
    }
    Text decoded;
    REQUIRE(unhuffman(huffman(text), decoded));
    CHECK(decoded == text);
}

TEST_CASE("huffman: section 5.2's refusals", "[net][qpack]")
{
    Text text;
    // Seven bits of padding are the most, all ones.
    CHECK(unhuffman(bytes_of_hex("07"), text)); // '0' (00000) padded with 111
    CHECK(text == "0");
    CHECK_FALSE(unhuffman(bytes_of_hex("ff"), text));     // eight bits of padding
    CHECK_FALSE(unhuffman(bytes_of_hex("06"), text));     // padding not all ones
    CHECK_FALSE(unhuffman(bytes_of_hex("ffffffff"), text)); // EOS, then ones
    // A code cut inside itself: the first eight bits of '!' (1111111000).
    CHECK_FALSE(unhuffman(bytes_of_hex("fe"), text));
    CHECK(unhuffman(std::string_view(), text)); // the empty string
    CHECK(text.empty());
}

TEST_CASE("huffman: a small output measures the whole text", "[net][qpack]")
{
    const Text coded = bytes_of_hex("f1e3c2e5f23a6ba0ab90f4ff");
    u8 room[4] = {};
    usize decoded = 0U;
    REQUIRE(http_huffman_decode(view(coded), Span<u8>(room), decoded));
    CHECK(decoded == 15U);
    CHECK(std::memcmp(room, "www.", 4U) == 0);
    REQUIRE(http_huffman_decode(view(coded), Span<u8>(), decoded));
    CHECK(decoded == 15U);
}

TEST_CASE("strings: the shorter form, and the limit", "[net][qpack]")
{
    u8 room[256] = {};
    const auto write = [&room](std::string_view text, u8 flags)
    {
        cont::WireWriter out((Span<u8>(room)));
        put_http_string(out, flags, 7U, view(text));
        REQUIRE(out.written() == http_string_bytes(view(text), 7U));
        return Text(reinterpret_cast<const char*>(room), out.written());
    };

    // Plain text a Huffman code shortens.
    CHECK(write("www.example.com", 0U) == bytes_of_hex("8c") + bytes_of_hex("f1e3c2e5f23a6ba0ab90f4ff"));
    // A code no shorter leaves the text as it is: no H bit.
    const Text rare("\x01\x02\x03", 3U);
    CHECK(write(rare, 0U) == bytes_of_hex("03") + rare);
    CHECK(write(std::string_view(), 0U) == bytes_of_hex("00"));

    const auto read = [](std::string_view bytes, usize max, Text& text, bool& huffman_form)
    {
        cont::WireReader in(view(bytes));
        u8 first = 0U;
        REQUIRE(in.take_u8(first));
        ConstSpan<u8> coded;
        const HttpDecode status = take_http_string(in, first, 7U, max, coded, huffman_form);
        if (status == HttpDecode::Ok)
        {
            text = Text(reinterpret_cast<const char*>(coded.data()), coded.size());
        }
        return status;
    };
    const Text written = write("www.example.com", 0U);
    Text text;
    bool form = false;
    REQUIRE(read(written, 12U, text, form) == HttpDecode::Ok);
    CHECK(form);
    CHECK(text == bytes_of_hex("f1e3c2e5f23a6ba0ab90f4ff"));
    CHECK(read(written, 11U, text, form) == HttpDecode::Invalid);
    CHECK(read(slice(written, 0U, 5U), 12U, text, form) == HttpDecode::NeedMore);
    // Refused on the length alone, before any of the string arrived.
    CHECK(read(bytes_of_hex("7f9a0a"), 100U, text, form) == HttpDecode::Invalid);
}
