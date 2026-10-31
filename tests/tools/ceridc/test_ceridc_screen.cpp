// COMPLY.1: `ceridc screen` runs crd-comply's sanctions and embargo screening over the cerid-sanctions/1 lists in a
// directory (docs/design/registers.md#screening): every verdict and hit field, failing closed on a missing, stale or
// not yet published list, every malformed request refused before a file is read, malformed and misnamed lists
// refused with their file, and the real binary answering the same bytes. Every name is invented.

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace
{
namespace fs = crd::platform::fs;
namespace cont = crd::containers;

constexpr const char* kOfac = R"toml(format = "cerid-sanctions/1"
authority = "test.ofac"
jurisdiction = "US"
generation = 2026103101
published = "2026-10-31"
max_age_days = 2
source = "https://example.test/sdn.xml"
source_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[destination]]
region = "CU"
program = "TEST-CUBA"
measure = "comprehensive"

[[destination]]
region = "UA-43"
program = "TEST-CRIMEA"
measure = "comprehensive"

[[destination]]
region = "RU"
program = "TEST-RUSSIA-SOFTWARE"
measure = "sectoral"

[[party]]
id = "test.ofac.1001"
kind = "individual"
names = ["Aras Demirkol", "Aras Demirkoll"]
programs = ["TEST-SDGT"]
listed = "2020-05-01"
births = ["1971-03-14"]
countries = ["TR"]
identifiers = ["passport:A 1234-567"]

[[party]]
id = "test.ofac.1002"
kind = "entity"
names = ["Kuzey Deniz Ticaret A.\u015E."]
programs = ["TEST-SDGT"]
listed = "2021-01"
births = []
countries = ["TR", "AE"]
identifiers = ["registration:998877"]
)toml";

// Published a month before the screening day and stale a week after it.
constexpr const char* kUn = R"toml(format = "cerid-sanctions/1"
authority = "test.un"
jurisdiction = "INT"
generation = 7
published = "2026-10-01"
max_age_days = 7
source = "https://example.test/consolidated.xml"
source_sha256 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

[[party]]
id = "test.un.2001"
kind = "vessel"
names = ["Sea Falcon"]
programs = ["TEST-SEA"]
listed = "2022"
births = []
countries = ["IR"]
identifiers = ["imo:9123456"]
)toml";

// The lists are written into the case's own working directory.
void write_lists()
{
    REQUIRE(fs::write_file_text(fs::Path("test.ofac.sanctions.toml"), kOfac));
    REQUIRE(fs::write_file_text(fs::Path("test.un.sanctions.toml"), kUn));
}

struct Screen
{
    const char* lists = ".";
    const char* authorities = "test.ofac";
    const char* day = "2026-11-01";
    const char* destination = nullptr;
    const char* kind = nullptr;
    std::initializer_list<const char*> names = {};
    const char* birth = nullptr;
    std::initializer_list<const char*> countries = {};
    std::initializer_list<const char*> identifiers = {};
    const char* review = nullptr;
    const char* match = nullptr;
};

cont::String screen(const Screen& s)
{
    crd::ceridc::ScreenFlags flags;
    flags.lists = s.lists;
    flags.authorities = s.authorities;
    flags.day = s.day;
    flags.destination = s.destination;
    flags.kind = s.kind;
    flags.names = cont::ConstSpan<const char*>(s.names.begin(), s.names.size());
    flags.birth = s.birth;
    flags.countries = cont::ConstSpan<const char*>(s.countries.begin(), s.countries.size());
    flags.identifiers = cont::ConstSpan<const char*>(s.identifiers.begin(), s.identifiers.size());
    flags.review = s.review;
    flags.match = s.match;
    return crd::ceridc::verb_screen(flags, crd::memory::default_allocator());
}

bool has(const cont::String& text, cont::StringView needle)
{
    return cont::StringView(text).find(needle) != cont::StringView::npos;
}

crd::usize occurrences(const cont::String& text, cont::StringView needle)
{
    crd::usize count = 0U;
    for (crd::usize at = cont::StringView(text).find(needle); at != cont::StringView::npos;
         at = cont::StringView(text).find(needle, at + needle.size()))
    {
        ++count;
    }
    return count;
}
} // namespace

TEST_CASE("screen verb: a listed person is blocked and every hit field is written", "[ceridc][comply][screen]")
{
    write_lists();
    const cont::String out =
        screen({.kind = "individual", .names = {"Aras Demirkol"}, .birth = "1971-03-14", .countries = {"DE"}});
    INFO(out.c_str());
    CHECK(has(out,
              R"({"verb":"screen","ok":true,"day":"2026-11-01",)"
              R"("lists":[{"authority":"test.ofac","present":true,"generation":2026103101}],)"
              R"("verdict":"blocked","gaps":[],"parties":[)"
              R"({"authority":"test.ofac","generation":2026103101,"party":"test.ofac.1001",)"
              R"("name":"Aras Demirkol","score":1000,"disposition":"match","birth_agrees":true,"country_agrees":false,)"
              R"("identifier_agrees":false}],"destinations":[]})"));

    // The subject is screened, never echoed.
    const cont::String clear = screen({.kind = "individual", .names = {"Zeynep Yildirim"}});
    INFO(clear.c_str());
    CHECK(has(clear, R"("verdict":"clear","gaps":[],"parties":[],"destinations":[]})"));
    CHECK_FALSE(has(clear, "Zeynep"));
}

TEST_CASE("screen verb: dispositions of names written in Turkish, corroborated or not", "[ceridc][comply][screen]")
{
    write_lists();
    // The name in capitals with the dotted capital I and S with cedilla folds to the listed one.
    const char* turkish = "KUZEY DEN\xC4\xB0Z T\xC4\xB0"
                          "CARET A.\xC5\x9E.";
    const cont::String possible = screen({.kind = "entity", .names = {turkish}});
    INFO(possible.c_str());
    CHECK(has(possible, R"("verdict":"review")"));
    CHECK(has(possible, R"("party":"test.ofac.1002","name":"Kuzey Deniz Ticaret A.)"
                        "\xC5\x9E"
                        R"(.","score":1000,"disposition":"possible","birth_agrees":false,"country_agrees":false,)"
                        R"("identifier_agrees":false})"));

    const cont::String by_id =
        screen({.kind = "entity", .names = {"Kuzey Denizcilik"}, .identifiers = {"registration:998877"}});
    INFO(by_id.c_str());
    CHECK(has(by_id, R"("verdict":"blocked")"));
    CHECK(has(by_id, R"("disposition":"match","birth_agrees":false,"country_agrees":false,"identifier_agrees":true})"));

    const cont::String by_country = screen({.kind = "entity", .names = {turkish}, .countries = {"AE"}});
    CHECK(has(by_country, R"("disposition":"match","birth_agrees":false,"country_agrees":true,)"));

    // A kind or a birth that contradicts the listed party discounts the hit: reported, never dropped, never blocking.
    const cont::String other_kind = screen({.kind = "entity", .names = {"Aras Demirkol"}});
    INFO(other_kind.c_str());
    CHECK(has(other_kind, R"("verdict":"clear")"));
    CHECK(
        has(other_kind, R"("party":"test.ofac.1001","name":"Aras Demirkol","score":1000,"disposition":"discounted")"));
    const cont::String other_birth = screen({.kind = "individual", .names = {"Aras Demirkol"}, .birth = "1990"});
    CHECK(has(other_birth, R"("disposition":"discounted")"));

    // The thresholds are the caller's: a strict review threshold leaves a near spelling out.
    const cont::String near = screen({.kind = "individual", .names = {"Aras Demirkool"}});
    INFO(near.c_str());
    CHECK(has(near, R"("verdict":"review")"));
    CHECK(has(screen({.kind = "individual", .names = {"Aras Demirkool"}, .review = "999", .match = "1000"}),
              R"("verdict":"clear")"));
}

TEST_CASE("screen verb: destinations by embargo, region and measure", "[ceridc][comply][screen]")
{
    write_lists();
    const cont::String cuba = screen({.destination = "CU"});
    INFO(cuba.c_str());
    CHECK(has(cuba, R"("verdict":"blocked","gaps":[],"parties":[],"destinations":[)"
                    R"({"authority":"test.ofac","generation":2026103101,"region":"CU","program":"TEST-CUBA",)"
                    R"("measure":"comprehensive","disposition":"match"}]})"));
    CHECK(has(screen({.destination = "UA-43"}), R"("verdict":"blocked")"));
    CHECK(has(screen({.destination = "UA"}), R"("region":"UA-43","program":"TEST-CRIMEA","measure":"comprehensive",)"
                                             R"("disposition":"possible"}]})"));
    CHECK(has(screen({.destination = "UA"}), R"("verdict":"review")"));
    CHECK(has(screen({.destination = "UA-12"}), R"("verdict":"clear")"));
    CHECK(has(screen({.destination = "RU"}), R"("measure":"sectoral","disposition":"match"}]})"));
    CHECK(has(screen({.destination = "RU"}), R"("verdict":"review")"));
    CHECK(has(screen({.destination = "FR"}), R"("verdict":"clear","gaps":[],"parties":[],"destinations":[]})"));
    // A subject and a destination together: the stronger verdict.
    CHECK(has(screen({.destination = "CU", .kind = "individual", .names = {"Zeynep Yildirim"}}),
              R"("verdict":"blocked")"));
}

TEST_CASE("screen verb: a missing, stale or not yet published list fails closed", "[ceridc][comply][screen]")
{
    write_lists();
    const cont::String missing = screen({.authorities = "test.ofac,test.absent", .destination = "FR"});
    INFO(missing.c_str());
    CHECK(has(missing, R"("lists":[{"authority":"test.ofac","present":true,"generation":2026103101},)"
                       R"({"authority":"test.absent","present":false,"generation":0}],"verdict":"unavailable",)"
                       R"("gaps":[{"authority":"test.absent","gap":"missing"}])"));

    // A stale list is still screened: what it lists blocks, and nothing else reads clear.
    const cont::String stale_match =
        screen({.authorities = "test.un", .kind = "vessel", .names = {"Sea Falcon"}, .identifiers = {"imo:9123456"}});
    INFO(stale_match.c_str());
    CHECK(has(stale_match, R"("verdict":"blocked","gaps":[{"authority":"test.un","gap":"stale"}])"));
    CHECK(has(screen({.authorities = "test.un", .destination = "FR"}), R"("verdict":"unavailable")"));
    CHECK(has(screen({.authorities = "test.un", .day = "2026-10-08", .destination = "FR"}), R"("verdict":"clear")"));

    const cont::String early = screen({.day = "2026-10-30", .destination = "FR"});
    CHECK(has(early, R"("verdict":"unavailable","gaps":[{"authority":"test.ofac","gap":"not-yet-published"}])"));

    // An authority named twice is read and screened once.
    const cont::String twice = screen({.authorities = "test.ofac,test.ofac", .destination = "CU"});
    INFO(twice.c_str());
    CHECK(has(twice, R"({"authority":"test.ofac","present":true,"generation":2026103101},)"
                     R"({"authority":"test.ofac","present":true,"generation":2026103101}])"));
    CHECK(occurrences(twice, "TEST-CUBA") == 1U);

    // A directory without lists fails closed on every authority.
    CHECK(has(screen({.lists = "no-such-directory", .destination = "FR"}),
              R"("verdict":"unavailable","gaps":[{"authority":"test.ofac","gap":"missing"}])"));
}

TEST_CASE("screen verb: malformed requests are refused before a list is read", "[ceridc][comply][screen]")
{
    // No list is written: a request that passed validation would answer, so each refusal is the request's own.
    const auto refused = [](const Screen& s, cont::StringView reason)
    {
        const cont::String out = screen(s);
        INFO(out.c_str());
        CHECK(has(out, R"({"ok":false,"verb":"screen","reason":")"));
        CHECK(has(out, reason));
    };
    refused({.lists = nullptr, .destination = "CU"}, "screen needs --lists, --authority and --day");
    refused({.authorities = nullptr, .destination = "CU"}, "screen needs --lists, --authority and --day");
    refused({.day = nullptr, .destination = "CU"}, "screen needs --lists, --authority and --day");
    for (const char* day : {"2026-11", "2026-02-30", "2026-11-01x", ""})
    {
        refused({.day = day, .destination = "CU"}, "--day must be");
    }
    for (const char* list : {"", "test.ofac,", ",test.ofac", "test.ofac,,test.un"})
    {
        refused({.authorities = list, .destination = "CU"}, "--authority is a comma-separated list");
    }
    cont::String many;
    for (int i = 0; i < 33; ++i)
    {
        many.append(i == 0 ? "a" : ",a");
    }
    refused({.authorities = many.c_str(), .destination = "CU"}, "--authority is a comma-separated list");
    refused({}, "screen needs a subject (--name) or a --destination");
    refused({.destination = "CU", .kind = "entity"}, "describe a subject --name names");
    refused({.destination = "CU", .birth = "1971"}, "describe a subject --name names");
    refused({.destination = "CU", .countries = {"TR"}}, "describe a subject --name names");
    refused({.destination = "CU", .identifiers = {"imo:1"}}, "describe a subject --name names");
    refused({.names = {"Aras Demirkol"}}, "--name needs --kind");
    refused({.kind = "person", .names = {"Aras Demirkol"}}, "--name needs --kind");
    for (const char* threshold : {"", "9x", "-1", "12345"})
    {
        refused({.destination = "CU", .review = threshold}, "--review and --match are thresholds");
        refused({.destination = "CU", .match = threshold}, "--review and --match are thresholds");
    }

    // The module's request rules, with their error and key.
    const auto rule = [](const Screen& s, cont::StringView tail)
    {
        const cont::String out = screen(s);
        INFO(out.c_str());
        CHECK(has(out, R"({"ok":false,"verb":"screen","reason":"the screening request was refused",)"));
        CHECK(has(out, tail));
    };
    rule({.destination = "CU", .review = "960"}, R"("error":"bad-value","line":0,"key":"policy"})");
    rule({.destination = "CU", .review = "1001", .match = "1001"}, R"("error":"bad-value","line":0,"key":"policy"})");
    rule({.authorities = "../test.ofac", .destination = "CU"}, R"("key":"authorities"})");
    rule({.authorities = "Test.ofac", .destination = "CU"}, R"("key":"authorities"})");
    rule({.destination = "C"}, R"("key":"destination"})");
    rule({.destination = "cu"}, R"("key":"destination"})");
    rule({.kind = "individual", .names = {" - "}}, R"("key":"names"})");
    rule({.kind = "individual", .names = {"\xC3"}}, R"("key":"names"})");
    rule({.kind = "individual", .names = {"Aras Demirkol"}, .birth = "1971-13"},
         R"("error":"bad-day","line":0,"key":"birth"})");
    rule({.kind = "individual", .names = {"Aras Demirkol"}, .countries = {"tr"}}, R"("key":"countries"})");
    rule({.kind = "individual", .names = {"Aras Demirkol"}, .identifiers = {"A 1234"}}, R"("key":"identifiers"})");
}

TEST_CASE("screen verb: a malformed or misnamed list is refused with its file", "[ceridc][comply][screen]")
{
    REQUIRE(fs::write_file_text(fs::Path("test.ofac.sanctions.toml"), "format = \"cerid-sanctions/2\"\n"));
    const cont::String malformed = screen({.destination = "CU"});
    INFO(malformed.c_str());
    CHECK(has(malformed, R"({"ok":false,"verb":"screen","reason":"a sanctions list was refused","file":")"));
    CHECK(has(malformed, R"(test.ofac.sanctions.toml","error":"unsupported-format","line":1,"key":"format"})"));

    // test.un's file holding test.ofac's list is never screened as test.un's.
    REQUIRE(fs::write_file_text(fs::Path("test.un.sanctions.toml"), kOfac));
    const cont::String misnamed = screen({.authorities = "test.un", .destination = "CU"});
    INFO(misnamed.c_str());
    CHECK(has(misnamed, R"("reason":"a sanctions list file holds another authority's list","file":")"));
    CHECK(has(misnamed, R"(test.un.sanctions.toml","authority":"test.un"})"));
}

TEST_CASE("screen verb: the real ceridc binary answers the same bytes", "[ceridc][comply][screen]")
{
    write_lists();
    cont::String exe;
    REQUIRE(crd::read_environment_text("CRD_CERIDC_EXE", exe)); // wired by CMake
    // cmd.exe drops the first and the last quote of a command line that holds more than two, so on Windows the whole
    // command is quoted once more; sh takes it as it is.
#if CRD_OS_WINDOWS
    constexpr const char* shell_quote = "\"";
#else
    constexpr const char* shell_quote = "";
#endif
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "%s\"%s\" screen --lists . --authority test.ofac,test.un,test.absent --day 2026-11-01 "
                        "--kind individual --name \"Aras Demirkoll\" --name \"A. Demirkol\" --birth 1971 "
                        "--country TR --identifier \"passport:A1234567\" --destination UA --review 800 --match 940 "
                        "> ceridc_screen_out.json%s",
                        shell_quote, exe.c_str(), shell_quote);
    REQUIRE(std::system(cmd) == 0); // a blocked verdict is an answer, not a failure
    cont::String out;
    REQUIRE(fs::read_file_text(fs::Path("ceridc_screen_out.json"), out));
    while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
    {
        out.pop_back();
    }
    const cont::String expected = screen({.authorities = "test.ofac,test.un,test.absent",
                                          .destination = "UA",
                                          .kind = "individual",
                                          .names = {"Aras Demirkoll", "A. Demirkol"},
                                          .birth = "1971",
                                          .countries = {"TR"},
                                          .identifiers = {"passport:A1234567"},
                                          .review = "800",
                                          .match = "940"});
    INFO(expected.c_str());
    CHECK(has(expected, R"("verdict":"blocked")"));
    CHECK(has(expected, R"("identifier_agrees":true})"));
    CHECK(cont::StringView(out) == cont::StringView(expected));

    (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" screen --lists . --authority test.ofac > ceridc_screen_out.json%s)",
                        shell_quote, exe.c_str(), shell_quote);
    CHECK(std::system(cmd) != 0);
}
