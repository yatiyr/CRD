// crd-core -- UTF-16 decoding and encoding and the transcoding between UTF-16 and UTF-8 (crd/core/utf16.hpp): every
// length class, the surrogate pairs and the unpaired surrogates, the bytes that start no UTF-8 sequence, and the
// writes that stop at the first code point that does not fit.

#include <crd/core/utf16.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ios>

namespace
{
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;

// "Kuzey DENİZ Ticaret A.Ş. 𝔸" in UTF-16 and UTF-8: ASCII, two-byte Turkish letters and a supplementary code point.
constexpr char16_t kWide[] = u"Kuzey DENİZ Ticaret A.Ş. \U0001D538";
constexpr char     kUtf8[] = "Kuzey DEN\xC4\xB0Z Ticaret A.\xC5\x9E. \xF0\x9D\x94\xB8";
constexpr usize    kWideUnits = sizeof(kWide) / sizeof(kWide[0]) - 1U;
constexpr usize    kUtf8Bytes = sizeof(kUtf8) - 1U;

const u8* bytes_of(const char* s)
{
    return reinterpret_cast<const u8*>(s);
}

constexpr bool encodes_at_compile_time()
{
    u8          out[4] = {};
    const usize n      = crd::utf8_encode(0x130U, out);
    return n == 2U && out[0] == 0xC4U && out[1] == 0xB0U;
}
static_assert(encodes_at_compile_time());

} // namespace

TEST_CASE("utf16_decode reads single units, surrogate pairs and unpaired surrogates", "[core][utf16]")
{
    u32 cp = 0U;
    const char16_t bmp[] = {u'Ş'};
    CHECK(crd::utf16_decode(bmp, 1U, cp) == 1U);
    CHECK(cp == 0x15EU);

    const char16_t pair[] = {0xD835U, 0xDD38U};
    CHECK(crd::utf16_decode(pair, 2U, cp) == 2U);
    CHECK(cp == 0x1D538U);
    CHECK(crd::utf16_decode(pair, 1U, cp) == 1U); // the pair cut short: its lead alone
    CHECK(cp == crd::kReplacementCharacter);

    const char16_t lead_then_letter[] = {0xD800U, u'A'};
    CHECK(crd::utf16_decode(lead_then_letter, 2U, cp) == 1U);
    CHECK(cp == crd::kReplacementCharacter);
    CHECK(crd::utf16_decode(lead_then_letter + 1, 1U, cp) == 1U);
    CHECK(cp == u32{'A'});

    const u16 lead_then_past_trails[] = {0xDBFFU, 0xE000U}; // the unit after a trail surrogate's range
    CHECK(crd::utf16_decode(lead_then_past_trails, 2U, cp) == 1U);
    CHECK(cp == crd::kReplacementCharacter);

    const u16 trail_alone[] = {0xDC00U, 0xDFFFU};
    CHECK(crd::utf16_decode(trail_alone, 2U, cp) == 1U);
    CHECK(cp == crd::kReplacementCharacter);

    const u16 edges[] = {0xD7FFU, 0xE000U, 0xFFFFU};
    for (const u16 unit : edges)
    {
        CHECK(crd::utf16_decode(&unit, 1U, cp) == 1U);
        CHECK(cp == unit);
    }
    const u16 last[] = {0xDBFFU, 0xDFFFU};
    CHECK(crd::utf16_decode(last, 2U, cp) == 2U);
    CHECK(cp == 0x10FFFFU);
}

TEST_CASE("utf8_encode and utf16_encode write every length class and replace what they cannot encode",
          "[core][utf16]")
{
    struct Case
    {
        u32   code_point;
        usize utf8_length;
        u8    utf8[4];
        usize utf16_length;
        u16   utf16[2];
    };
    const Case cases[] = {
        {0x0U, 1U, {0x00U}, 1U, {0x0000U}},
        {0x7FU, 1U, {0x7FU}, 1U, {0x007FU}},
        {0x80U, 2U, {0xC2U, 0x80U}, 1U, {0x0080U}},
        {0x7FFU, 2U, {0xDFU, 0xBFU}, 1U, {0x07FFU}},
        {0x800U, 3U, {0xE0U, 0xA0U, 0x80U}, 1U, {0x0800U}},
        {0xD7FFU, 3U, {0xEDU, 0x9FU, 0xBFU}, 1U, {0xD7FFU}},
        {0xFFFFU, 3U, {0xEFU, 0xBFU, 0xBFU}, 1U, {0xFFFFU}},
        {0x10000U, 4U, {0xF0U, 0x90U, 0x80U, 0x80U}, 2U, {0xD800U, 0xDC00U}},
        {0x10FFFFU, 4U, {0xF4U, 0x8FU, 0xBFU, 0xBFU}, 2U, {0xDBFFU, 0xDFFFU}},
        {0xD800U, 3U, {0xEFU, 0xBFU, 0xBDU}, 1U, {0xFFFDU}},   // a surrogate is no code point to encode
        {0xDFFFU, 3U, {0xEFU, 0xBFU, 0xBDU}, 1U, {0xFFFDU}},
        {0x110000U, 3U, {0xEFU, 0xBFU, 0xBDU}, 1U, {0xFFFDU}}, // past U+10FFFF
    };
    for (const Case& c : cases)
    {
        INFO("U+" << std::hex << c.code_point);
        u8 utf8[4] = {};
        REQUIRE(crd::utf8_encode(c.code_point, utf8) == c.utf8_length);
        CHECK(std::memcmp(utf8, c.utf8, c.utf8_length) == 0);
        u16 utf16[2] = {};
        REQUIRE(crd::utf16_encode(c.code_point, utf16) == c.utf16_length);
        CHECK(utf16[0] == c.utf16[0]);
        CHECK(utf16[1] == c.utf16[1]);
    }
}

TEST_CASE("UTF-16 and UTF-8 transcode into each other, Turkish letters and supplementary code points included",
          "[core][utf16]")
{
    u8 utf8[64] = {};
    REQUIRE(crd::utf16_to_utf8(kWide, kWideUnits, utf8, sizeof(utf8)) == kUtf8Bytes);
    CHECK(std::memcmp(utf8, kUtf8, kUtf8Bytes) == 0);

    char16_t wide[64] = {};
    REQUIRE(crd::utf8_to_utf16(bytes_of(kUtf8), kUtf8Bytes, wide, 64U) == kWideUnits);
    CHECK(std::memcmp(wide, kWide, kWideUnits * sizeof(char16_t)) == 0);

    // Measuring: no storage at all, and nothing empty written anywhere.
    CHECK(crd::utf16_to_utf8(kWide, kWideUnits, static_cast<u8*>(nullptr), 0U) == kUtf8Bytes);
    CHECK(crd::utf8_to_utf16(bytes_of(kUtf8), kUtf8Bytes, static_cast<char16_t*>(nullptr), 0U) == kWideUnits);
    CHECK(crd::utf16_to_utf8(kWide, 0U, utf8, sizeof(utf8)) == 0U);
}

TEST_CASE("a transcoding that does not fit writes the code points before the first one that does not",
          "[core][utf16]")
{
    // "aİb": 1 + 2 + 1 bytes. Three bytes of room hold "aİ"; two hold "a" alone, never half of İ or a later "b".
    const char16_t in[] = {u'a', u'İ', u'b'};
    u8 out[4] = {0xEEU, 0xEEU, 0xEEU, 0xEEU};
    CHECK(crd::utf16_to_utf8(in, 3U, out, 3U) == 4U);
    CHECK(out[0] == 'a');
    CHECK(out[1] == 0xC4U);
    CHECK(out[2] == 0xB0U);
    CHECK(out[3] == 0xEEU);
    out[1] = out[2] = 0xEEU;
    CHECK(crd::utf16_to_utf8(in, 3U, out, 2U) == 4U);
    CHECK(out[0] == 'a');
    CHECK(out[1] == 0xEEU);
    CHECK(out[2] == 0xEEU);

    // A surrogate pair needs two units: one unit of room holds nothing of it, nor of anything after it.
    const char* const pair_then_a = "\xF0\x9D\x94\xB8" "a";
    char16_t wide[3] = {0x1111U, 0x1111U, 0x1111U};
    CHECK(crd::utf8_to_utf16(bytes_of(pair_then_a), 5U, wide, 1U) == 3U);
    CHECK(wide[0] == 0x1111U);
    CHECK(crd::utf8_to_utf16(bytes_of(pair_then_a), 5U, wide, 3U) == 3U);
    CHECK(wide[0] == 0xD835U);
    CHECK(wide[1] == 0xDD38U);
    CHECK(wide[2] == u'a');
}

TEST_CASE("every byte that starts no well-formed UTF-8 sequence becomes one U+FFFD", "[core][utf16]")
{
    struct Case
    {
        const char* bytes;
        usize       size;
        usize       units;
        u16         expected[4];
    };
    const Case cases[] = {
        {"\xC4", 1U, 1U, {0xFFFDU}},                                    // a sequence cut short
        {"\xC4" "A", 2U, 2U, {0xFFFDU, u'A'}},                          // its continuation missing
        {"\x80", 1U, 1U, {0xFFFDU}},                                    // a continuation byte alone
        {"\xC0\xAF", 2U, 2U, {0xFFFDU, 0xFFFDU}},                       // an overlong lead
        {"\xE0\x80\x80", 3U, 3U, {0xFFFDU, 0xFFFDU, 0xFFFDU}},          // an overlong form
        {"\xED\xA0\x80", 3U, 3U, {0xFFFDU, 0xFFFDU, 0xFFFDU}},          // an encoded surrogate
        {"\xF4\x90\x80\x80", 4U, 4U, {0xFFFDU, 0xFFFDU, 0xFFFDU, 0xFFFDU}}, // past U+10FFFF
        {"\xFF" "z", 2U, 2U, {0xFFFDU, u'z'}},
    };
    for (const Case& c : cases)
    {
        u16 wide[4] = {};
        REQUIRE(crd::utf8_to_utf16(bytes_of(c.bytes), c.size, wide, 4U) == c.units);
        for (usize i = 0U; i < c.units; ++i)
        {
            CHECK(wide[i] == c.expected[i]);
        }
    }

    // And back: an unpaired surrogate is U+FFFD in UTF-8 too.
    const u16 lone[] = {u'x', 0xDC01U, u'y'};
    u8 utf8[8] = {};
    REQUIRE(crd::utf16_to_utf8(lone, 3U, utf8, sizeof(utf8)) == 5U);
    CHECK(std::memcmp(utf8, "x\xEF\xBF\xBDy", 5U) == 0);
}
