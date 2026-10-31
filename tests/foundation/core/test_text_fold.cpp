// crd-core -- UTF-8 decoding and search folding (COMPLY.1; docs/design/registers.md#screening): every class of code
// point, the Turkish letters, Latin diacritics, ligatures and fullwidth forms to ASCII, Greek and Cyrillic case and
// accents, marks and apostrophes elided, and code points outside the covered blocks kept as they are.

#include <crd/core/text_fold.hpp>
#include <crd/core/utf8.hpp>

#include <catch2/catch_test_macros.hpp>
#include <ios>

namespace
{
using crd::CodePointFold;
using crd::fold_code_point;
using crd::FoldKind;
using crd::u32;
using crd::u8;
using crd::usize;

// The letter's fold as up to two code points (0 for none).
void check_letter(u32 code_point, u32 first, u32 second = 0U)
{
    const CodePointFold fold = fold_code_point(code_point);
    INFO("U+" << std::hex << code_point);
    CHECK(fold.kind == FoldKind::Letter);
    CHECK(fold.count == (second != 0U ? 2U : 1U));
    CHECK(fold.out[0] == first);
    CHECK(fold.out[1] == second);
}

void check_kind(u32 code_point, FoldKind kind)
{
    const CodePointFold fold = fold_code_point(code_point);
    INFO("U+" << std::hex << code_point);
    CHECK(fold.kind == kind);
    CHECK(fold.count == 0U);
}
} // namespace

TEST_CASE("utf8: decoding gives each well-formed sequence's code point and refuses the rest", "[core][utf8]")
{
    struct Case
    {
        const char* bytes;
        usize length;
        u32 code_point;
    };
    const Case cases[] = {
        {"A", 1U, 0x41U},
        {"\x7F", 1U, 0x7FU},
        {"\xC2\x80", 2U, 0x80U},
        {"\xC4\xB0", 2U, 0x130U}, // dotted capital I
        {"\xDF\xBF", 2U, 0x7FFU},
        {"\xE0\xA0\x80", 3U, 0x800U},
        {"\xE2\x80\x99", 3U, 0x2019U},
        {"\xEF\xBF\xBF", 3U, 0xFFFFU},
        {"\xF0\x90\x80\x80", 4U, 0x10000U},
        {"\xF4\x8F\xBF\xBF", 4U, 0x10FFFFU},
    };
    for (const Case& c : cases)
    {
        u32 code_point = 0U;
        const auto* bytes = reinterpret_cast<const u8*>(c.bytes); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        CHECK(crd::utf8_decode(bytes, c.length, code_point) == c.length);
        CHECK(code_point == c.code_point);
    }
    const char* refused[] = {"\x80",         "\xC0\x80",         "\xC1\xBF",         "\xE0\x9F\xBF",
                             "\xED\xA0\x80", "\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
                             "\xC4",         "\xE2\x80"};
    for (const char* text : refused)
    {
        u32 code_point = 7U;
        const auto* bytes = reinterpret_cast<const u8*>(text); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        usize length = 0U;
        while (text[length] != '\0')
        {
            ++length;
        }
        CHECK(crd::utf8_decode(bytes, length, code_point) == 0U);
        CHECK(code_point == 7U);
    }
    u32 unused = 0U;
    CHECK(crd::utf8_decode(nullptr, 0U, unused) == 0U);
}

TEST_CASE("text fold: ASCII letters fold to lower case, digits stay, everything else separates", "[core][text-fold]")
{
    for (u32 c = 'A'; c <= 'Z'; ++c)
    {
        check_letter(c, c - 'A' + 'a');
    }
    for (u32 c = 'a'; c <= 'z'; ++c)
    {
        check_letter(c, c);
    }
    for (u32 c = '0'; c <= '9'; ++c)
    {
        check_letter(c, c);
    }
    for (const u32 c :
         {0x00U, 0x09U, 0x0AU, 0x20U, 0x21U, 0x2CU, 0x2DU, 0x2EU, 0x2FU, 0x3AU, 0x40U, 0x5FU, 0x7EU, 0x7FU})
    {
        check_kind(c, FoldKind::Separator);
    }
    check_kind('\'', FoldKind::Elided);
    check_kind('`', FoldKind::Elided);
}

TEST_CASE("text fold: the Turkish letters reach ASCII, dotted and dotless i alike", "[core][text-fold]")
{
    check_letter(0x130U, 'i'); // I with dot above
    check_letter(0x131U, 'i'); // dotless i
    check_letter(0x15EU, 's');
    check_letter(0x15FU, 's');
    check_letter(0x11EU, 'g');
    check_letter(0x11FU, 'g');
    check_letter(0xC7U, 'c');
    check_letter(0xE7U, 'c');
    check_letter(0xD6U, 'o');
    check_letter(0xF6U, 'o');
    check_letter(0xDCU, 'u');
    check_letter(0xFCU, 'u');
    check_letter(0x18FU, 'a'); // schwa, Azerbaijani
    check_letter(0x259U, 'a');
}

TEST_CASE("text fold: Latin diacritics, strokes, ligatures and fullwidth forms reach ASCII", "[core][text-fold]")
{
    check_letter(0xC5U, 'a');        // A with ring
    check_letter(0xD8U, 'o');        // O with stroke (by its name)
    check_letter(0x142U, 'l');       // l with stroke
    check_letter(0x111U, 'd');       // d with stroke
    check_letter(0x127U, 'h');       // h with stroke
    check_letter(0x219U, 's');       // s with comma below (Romanian)
    check_letter(0x21BU, 't');       // t with comma below
    check_letter(0x1EA1U, 'a');      // a with dot below (Vietnamese)
    check_letter(0x1EC7U, 'e');      // e with circumflex and dot below
    check_letter(0xDFU, 's', 's');   // sharp s
    check_letter(0x1E9EU, 's', 's'); // capital sharp s
    check_letter(0xC6U, 'a', 'e');   // AE
    check_letter(0x153U, 'o', 'e');  // oe ligature
    check_letter(0x152U, 'o', 'e');  // OE ligature
    check_letter(0x132U, 'i', 'j');  // IJ
    check_letter(0x1C5U, 'd', 'z');  // D with small z with caron
    check_letter(0xDEU, 't', 'h');   // thorn
    check_letter(0xF0U, 'd');        // eth
    check_letter(0x14BU, 'n');       // eng
    check_letter(0x1EEU, 'z');       // EZH with caron: small ezh, which is z
    check_letter(0xFF21U, 'a');      // fullwidth A
    check_letter(0xFF5AU, 'z');      // fullwidth z
    check_letter(0xFF11U, '1');      // fullwidth 1
    check_letter(0xB2U, '2');        // superscript two
}

TEST_CASE("text fold: Greek and Cyrillic lose case and accents and stay in their script", "[core][text-fold]")
{
    check_letter(0x416U, 0x436U); // Zhe
    check_letter(0x436U, 0x436U);
    check_letter(0x419U, 0x438U); // short I is I
    check_letter(0x401U, 0x435U); // Io is Ie
    check_letter(0x386U, 0x3B1U); // Alpha with tonos
    check_letter(0x3A3U, 0x3C3U); // Sigma
    check_letter(0x3C2U, 0x3C3U); // final sigma
    check_letter(0x623U, 0x627U); // Alef with hamza above is Alef
}

TEST_CASE("text fold: marks and apostrophes vanish, punctuation and spaces separate", "[core][text-fold]")
{
    for (const u32 c :
         {0x0300U, 0x0301U, 0x0307U, 0x036FU, 0x2019U, 0x2018U, 0x00B4U, 0x02BCU, 0x02BEU, 0x02BFU, 0x064EU})
    {
        check_kind(c, FoldKind::Elided);
    }
    for (const u32 c : {0x00A0U, 0x00B7U, 0x00D7U, 0x00F7U, 0x2010U, 0x2013U, 0x2014U, 0x201CU, 0x2026U, 0x3000U,
                        0x3001U, 0xFF0CU, 0x060CU})
    {
        check_kind(c, FoldKind::Separator);
    }
}

TEST_CASE("text fold: outside the covered blocks a code point is a letter kept as it is", "[core][text-fold]")
{
    check_letter(0x4E2DU, 0x4E2DU);   // a CJK ideograph
    check_letter(0xAC00U, 0xAC00U);   // a Hangul syllable
    check_letter(0x1F600U, 0x1F600U); // an emoji: no category table outside the covered blocks
    check_kind(0xD800U, FoldKind::Separator);
    check_kind(0x110000U, FoldKind::Separator);
}
