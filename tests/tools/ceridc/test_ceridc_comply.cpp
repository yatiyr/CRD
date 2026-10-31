// COMPLY.0: `ceridc comply` answers crd-comply's query over the committed rule generations (assets/comply); the verb
// and the real binary give the same bytes, and every malformed request or generation is refused before it answers.

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/environment.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>

namespace
{
namespace fs = crd::platform::fs;
namespace cont = crd::containers;

constexpr const char* kRules = CRD_REPO_DIR "/assets/comply";

cont::String comply(const char* rules, const char* scope, const char* jurisdictions, const char* day)
{
    return crd::ceridc::verb_comply(rules, scope, jurisdictions, day, crd::memory::default_allocator());
}

bool has(const cont::String& text, cont::StringView needle)
{
    return cont::StringView(text).find(needle) != cont::StringView::npos;
}
} // namespace

TEST_CASE("comply verb: the rules binding the crash pool in Turkey and the EU", "[ceridc][comply]")
{
    const cont::String out = comply(kRules, "flow.crash-reports", "TR,EU,XX", "2026-11-01");
    INFO(out.c_str());
    CHECK(has(out, R"("verb":"comply","ok":true,"scope":"flow.crash-reports","day":"2026-11-01")"));
    CHECK(has(out, R"({"code":"TR","present":true,"generation":1})"));
    CHECK(has(out, R"({"code":"EU","present":true,"generation":1})"));
    CHECK(has(out, R"({"code":"XX","present":false,"generation":0})"));
    CHECK(has(out, R"("jurisdiction":"TR","generation":1,"id":"tr.kvkk.processing","kind":"obligation")"));
    CHECK(has(out, R"("proof":"unproven","scope_state":"planned")"));
    CHECK(has(out, R"("proven":0,)"));
    CHECK_FALSE(has(out, R"("proof":"proven")"));
    CHECK(has(out, R"("counsel":{"status":"needed","reviewed":""})"));
    CHECK(has(out, R"("sunset":"")"));
    // Turkish rules come first, as requested.
    CHECK(cont::StringView(out).find(R"("jurisdiction":"TR")") < cont::StringView(out).find(R"("jurisdiction":"EU")"));

    // A day before KVKK took effect answers nothing from it.
    const cont::String before = comply(kRules, "flow.crash-reports", "TR", "2016-04-06");
    CHECK_FALSE(has(before, "tr.kvkk.processing"));
    CHECK(has(before, R"("ok":true)"));
}

TEST_CASE("comply verb: malformed requests and generations are refused", "[ceridc][comply]")
{
    CHECK(has(comply(nullptr, "flow.crash-reports", "TR", "2026-11-01"), R"("ok":false)"));
    CHECK(has(comply(kRules, nullptr, "TR", "2026-11-01"), R"("ok":false)"));
    for (const char* day : {"2026-11", "2026-02-30", "2026-11-01x", ""})
    {
        INFO(day);
        CHECK(has(comply(kRules, "flow.crash-reports", "TR", day), "--day must be"));
    }
    for (const char* list : {"", "TR,", ",TR", "TR,,EU"})
    {
        INFO(list);
        CHECK(has(comply(kRules, "flow.crash-reports", list, "2026-11-01"), R"("ok":false)"));
    }

    // A generation that fails to decode names its file, error, line and key.
    REQUIRE(fs::write_file_text(fs::Path("tr.rules.toml"), "format = \"cerid-rules/2\"\n"));
    const cont::String refused = comply(".", "flow.crash-reports", "TR", "2026-11-01");
    INFO(refused.c_str());
    CHECK(has(refused, R"("ok":false)"));
    CHECK(has(refused, R"("error":"unsupported-format","line":1,"key":"format")"));
    CHECK(has(refused, "tr.rules.toml"));
}

TEST_CASE("comply verb: the real ceridc binary answers the same bytes", "[ceridc][comply]")
{
    cont::String exe;
    REQUIRE(crd::read_environment_text("CRD_CERIDC_EXE", exe)); // wired by CMake
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" comply --rules \"%s\" --scope product.purchases --jurisdiction TR,EU,PLATFORM "
                        "--day 2027-12-11 > ceridc_comply_out.json",
                        exe.c_str(), kRules);
    REQUIRE(std::system(cmd) == 0);
    cont::String out;
    REQUIRE(fs::read_file_text(fs::Path("ceridc_comply_out.json"), out));
    while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
    {
        out.pop_back();
    }
    const cont::String expected = comply(kRules, "product.purchases", "TR,EU,PLATFORM", "2027-12-11");
    CHECK(cont::StringView(out) == cont::StringView(expected));

    (void)std::snprintf(cmd, sizeof(cmd), R"("%s" comply --rules "%s" --scope x > ceridc_comply_out.json)", exe.c_str(),
                        kRules);
    CHECK(std::system(cmd) != 0);
}
