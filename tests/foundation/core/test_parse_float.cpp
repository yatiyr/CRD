#include <catch2/catch_test_macros.hpp>
#include <crd/core/parse_float.hpp>
#include <crd/core/platform.hpp>

#include <cmath>
#include <cstring>
#include <limits>

// The engine's one locale-independent number parse (capability text.parse-float): std::from_chars's general format,
// and, on every POSIX host, the strtod_l reading Apple's libc++ falls back to, held to the same answers.

namespace
{
bool parse(const char* text, crd::f64& out)
{
    return crd::parse_f64(text, text + std::strlen(text), out);
}

struct Case
{
    const char* text;
    bool        accepted;
    crd::f64    value;
};

constexpr Case kCases[] = {
    {"0", true, 0.0},
    {"-0", true, -0.0},
    {"1", true, 1.0},
    {"-2.5", true, -2.5},
    {".5", true, 0.5},
    {"5.", true, 5.0},
    {"1e3", true, 1000.0},
    {"1E-3", true, 0.001},
    {"6.02214076e23", true, 6.02214076e23},
    {"1.7976931348623157e308", true, 1.7976931348623157e308},
    {"4.9406564584124654e-324", true, 4.9406564584124654e-324}, // the least subnormal
    {"0.1", true, 0.1},
    {"", false, 0.0},
    {"-", false, 0.0},
    {"+1", false, 0.0},   // strtod reads a '+'; the general format does not
    {" 1", false, 0.0},   // nor leading space
    {"\t1", false, 0.0},
    {"1 ", false, 0.0},   // a partial match
    {"1e", false, 0.0},
    {"1.2.3", false, 0.0},
    {"0x1p3", false, 0.0}, // strtod's hexadecimal form
    {"-0X10", false, 0.0},
    {"1e400", false, 0.0},  // overflows a double
    {"-1e400", false, 0.0},
    {"1e-400", false, 0.0}, // underflows to zero
    {"abc", false, 0.0},
};
} // namespace

TEST_CASE("parse_f64 reads exactly one general-format number", "[core][parse-float]")
{
    for (const Case& c : kCases)
    {
        INFO(c.text);
        crd::f64 out = 42.0;
        REQUIRE(parse(c.text, out) == c.accepted);
        if (c.accepted)
        {
            CHECK(out == c.value);
            CHECK(std::signbit(out) == std::signbit(c.value));
        }
        else
        {
            CHECK(out == 42.0); // unchanged when refused
        }
    }
    crd::f64 out = 0.0;
    REQUIRE(parse("inf", out));
    CHECK(out == std::numeric_limits<crd::f64>::infinity());
    REQUIRE(parse("-Infinity", out));
    CHECK(out == -std::numeric_limits<crd::f64>::infinity());
    REQUIRE(parse("nan", out));
    CHECK(std::isnan(out));
    CHECK_FALSE(crd::parse_f64(nullptr, nullptr, out));

    // A span ends where it says: the digits after it are not read.
    const char* text = "12345";
    REQUIRE(crd::parse_f64(text, text + 3, out));
    CHECK(out == 123.0);
}

TEST_CASE("parse_f64's strtod_l reading agrees with it", "[core][parse-float]")
{
#if CRD_OS_WINDOWS
    SUCCEED("Windows has a floating std::from_chars and no strtod_l reading");
#else
    for (const Case& c : kCases)
    {
        INFO(c.text);
        crd::f64 fallback = 42.0;
        REQUIRE(crd::detail::parse_f64_c_locale(c.text, c.text + std::strlen(c.text), fallback) == c.accepted);
        if (c.accepted)
        {
            CHECK(fallback == c.value);
            CHECK(std::signbit(fallback) == std::signbit(c.value));
        }
        else
        {
            CHECK(fallback == 42.0);
        }
    }
    // The fallback's bound: "0.5000..." one byte longer than its buffer is refused, and one that fits is read.
    char long_number[crd::kParseFloatMaxBytes + 1U];
    std::memset(long_number, '0', sizeof(long_number));
    long_number[1] = '.';
    long_number[2] = '5';
    crd::f64 out = 0.0;
    CHECK_FALSE(crd::detail::parse_f64_c_locale(long_number, long_number + sizeof(long_number), out));
    REQUIRE(crd::detail::parse_f64_c_locale(long_number, long_number + crd::kParseFloatMaxBytes, out));
    CHECK(out == 0.5);
#endif
}
