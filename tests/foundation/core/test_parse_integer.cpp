#include <crd/core/parse_integer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <initializer_list>
#include <limits>

// The engine's one decimal integer parse (capability text.parse-integer): digits only, bounded, never wrapped.

namespace
{
bool unsigned_of(const char* text, crd::u64 max, crd::u64& out)
{
    return crd::parse_u64(text, text + std::strlen(text), max, out);
}

bool signed_of(const char* text, crd::i64& out)
{
    return crd::parse_i64(text, text + std::strlen(text), out);
}
} // namespace

TEST_CASE("parse_u64: digits up to the bound, the whole text or nothing", "[core][parse-integer]")
{
    crd::u64 value = 7U;
    CHECK(unsigned_of("0", 9U, value));
    CHECK(value == 0U);
    CHECK(unsigned_of("42", 100U, value));
    CHECK(value == 42U);
    CHECK(unsigned_of("100", 100U, value));
    CHECK(value == 100U);
    CHECK(unsigned_of("0007", 9U, value)); // leading zeros are digits
    CHECK(value == 7U);
    CHECK(unsigned_of("18446744073709551615", ~crd::u64{0}, value));
    CHECK(value == ~crd::u64{0});
    CHECK(unsigned_of("00000000000000000001", 1U, value)); // twenty digits
    CHECK(value == 1U);

    value = 7U;
    for (const char* refused :
         {"", "-1", "+1", " 1", "1 ", "1x", "x", "0x10", "1.0", "18446744073709551616", "000000000000000000001"})
    {
        INFO(refused);
        CHECK_FALSE(unsigned_of(refused, ~crd::u64{0}, value));
        CHECK(value == 7U);
    }
    CHECK_FALSE(unsigned_of("101", 100U, value));
    CHECK_FALSE(unsigned_of("5", 0U, value)); // a digit past a bound below 9
    CHECK_FALSE(unsigned_of("10", 9U, value));
    CHECK(value == 7U);
    CHECK_FALSE(crd::parse_u64(nullptr, nullptr, value));

    crd::u64 full = 0U;
    const char* text = "123";
    CHECK(crd::parse_u64(text, text + 3, full));
    CHECK(full == 123U);
}

TEST_CASE("parse_i64: an optional '-' and digits an i64 holds", "[core][parse-integer]")
{
    crd::i64 value = 7;
    CHECK(signed_of("0", value));
    CHECK(value == 0);
    CHECK(signed_of("-0", value));
    CHECK(value == 0);
    CHECK(signed_of("-42", value));
    CHECK(value == -42);
    CHECK(signed_of("9223372036854775807", value));
    CHECK(value == std::numeric_limits<crd::i64>::max());
    CHECK(signed_of("-9223372036854775808", value));
    CHECK(value == std::numeric_limits<crd::i64>::min());

    value = 7;
    for (const char* refused :
         {"", "-", "--1", "+1", "1-", " -1", "9223372036854775808", "-9223372036854775809", "-18446744073709551615"})
    {
        INFO(refused);
        CHECK_FALSE(signed_of(refused, value));
        CHECK(value == 7);
    }
    CHECK_FALSE(crd::parse_i64(nullptr, nullptr, value));
}
