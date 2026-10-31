// crd-core -- the environment reader (crd/core/environment.hpp): a value reads as UTF-8 on every platform (Windows'
// wide environment transcoded, never the ANSI code page), unset and empty alike read as unset, a value one byte too
// long for its buffer is refused whole, and read_environment_text grows to a long non-ASCII value.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/utf16.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace
{
namespace cont = crd::containers;

// Sets (or, with nullptr, removes) a variable from UTF-8: on Windows through the wide API, so the test's own write
// never passes through the code page the reader must avoid.
void set_variable(const char* name, const char* utf8_value)
{
#if CRD_OS_WINDOWS
    wchar_t wide_name[64] = {};
    REQUIRE(crd::utf8_to_utf16(reinterpret_cast<const crd::u8*>(name), std::strlen(name), wide_name, 63U) < 64U);
    if (utf8_value == nullptr)
    {
        (void)::SetEnvironmentVariableW(wide_name, nullptr);
        return;
    }
    const crd::usize bytes = std::strlen(utf8_value);
    cont::Array<wchar_t> wide;
    wide.resize(bytes + 1U); // UTF-16 takes at most a unit per UTF-8 byte
    const crd::usize units =
        crd::utf8_to_utf16(reinterpret_cast<const crd::u8*>(utf8_value), bytes, wide.data(), bytes);
    REQUIRE(units <= bytes);
    wide.data()[units] = L'\0';
    REQUIRE(::SetEnvironmentVariableW(wide_name, wide.data()) != 0);
#else
    if (utf8_value == nullptr)
    {
        (void)::unsetenv(name);
        return;
    }
    REQUIRE(::setenv(name, utf8_value, 1) == 0);
#endif
}

struct ScopedVariable
{
    const char* name;
    ScopedVariable(const char* n, const char* value) : name(n) { set_variable(name, value); }
    ~ScopedVariable() { set_variable(name, nullptr); }
    ScopedVariable(const ScopedVariable&)            = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
};

constexpr const char* kTurkish = "DEN\xC4\xB0Z \xC5\x9E\x45HR\xC4\xB0 \xC4\x9F\xC3\xBC\xC3\xB6\xC3\xA7\xC4\xB1"; // DENİZ ŞEHRİ ğüöçı

} // namespace

TEST_CASE("environment: a non-ASCII value reads as its UTF-8 bytes", "[core][environment]")
{
    const ScopedVariable variable("CRD_TEST_ENVIRONMENT_TURKISH", kTurkish);
    const crd::usize     size = std::strlen(kTurkish);

    char exact[64] = {};
    REQUIRE(crd::read_environment("CRD_TEST_ENVIRONMENT_TURKISH", exact, size + 1U) == crd::EnvironmentRead::Set);
    CHECK(std::strcmp(exact, kTurkish) == 0);

    // One byte short of the value and its NUL: refused whole, the buffer left empty, never a cut sequence.
    char short_by_one[64];
    std::memset(short_by_one, 'x', sizeof(short_by_one));
    CHECK(crd::read_environment("CRD_TEST_ENVIRONMENT_TURKISH", short_by_one, size) == crd::EnvironmentRead::TooLong);
    CHECK(short_by_one[0] == '\0');

    CHECK(crd::environment_set("CRD_TEST_ENVIRONMENT_TURKISH"));
    const crd::EnvironmentValue<64> value("CRD_TEST_ENVIRONMENT_TURKISH");
    REQUIRE(value.c_str() != nullptr);
    CHECK(std::strcmp(value.c_str(), kTurkish) == 0);
}

TEST_CASE("environment: missing and empty variables read as unset", "[core][environment]")
{
    set_variable("CRD_TEST_ENVIRONMENT_MISSING", nullptr);
    char buffer[8] = {'x'};
    CHECK(crd::read_environment("CRD_TEST_ENVIRONMENT_MISSING", buffer, sizeof(buffer)) ==
          crd::EnvironmentRead::Unset);
    CHECK(buffer[0] == '\0');
    CHECK_FALSE(crd::environment_set("CRD_TEST_ENVIRONMENT_MISSING"));
#if !CRD_OS_WINDOWS // Windows removes a variable set to the empty string
    const ScopedVariable empty("CRD_TEST_ENVIRONMENT_EMPTY", "");
    CHECK(crd::read_environment("CRD_TEST_ENVIRONMENT_EMPTY", buffer, sizeof(buffer)) == crd::EnvironmentRead::Unset);
#endif
    const ScopedVariable set("CRD_TEST_ENVIRONMENT_SET", "1");
    CHECK(crd::read_environment("CRD_TEST_ENVIRONMENT_SET", nullptr, 0U) == crd::EnvironmentRead::TooLong);
}

TEST_CASE("environment: read_environment_text grows to a long non-ASCII value", "[core][environment]")
{
    // 400 repetitions of a two-byte letter and an ASCII one: 1200 bytes, 800 UTF-16 units, past the reader's stack
    // storage on Windows and past read_environment_text's first 255 bytes everywhere.
    cont::String long_value;
    for (int i = 0; i < 400; ++i)
    {
        long_value.append("\xC4\xB0i");
    }
    const ScopedVariable variable("CRD_TEST_ENVIRONMENT_LONG", long_value.c_str());

    cont::String read;
    REQUIRE(crd::read_environment_text("CRD_TEST_ENVIRONMENT_LONG", read));
    CHECK(cont::StringView(read) == cont::StringView(long_value));

    char too_small[1200];
    CHECK(crd::read_environment("CRD_TEST_ENVIRONMENT_LONG", too_small, sizeof(too_small)) ==
          crd::EnvironmentRead::TooLong);
    CHECK(too_small[0] == '\0');
    char fits[1201];
    REQUIRE(crd::read_environment("CRD_TEST_ENVIRONMENT_LONG", fits, sizeof(fits)) == crd::EnvironmentRead::Set);
    CHECK(std::strcmp(fits, long_value.c_str()) == 0);

    // 800 units in 700 bytes of room: refused before the value is transcoded.
    char units_too_many[700];
    CHECK(crd::read_environment("CRD_TEST_ENVIRONMENT_LONG", units_too_many, sizeof(units_too_many)) ==
          crd::EnvironmentRead::TooLong);
}
