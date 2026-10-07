// test_ceridc_inspect.cpp — DIAG.8b: the HEADLESS consumer of runtime inspection. `ceridc inspect` runs the committed
// authored program assets/ceir/inspect_demo.ceir under an inspect session on the shared InspectHost and reports every
// stop as JSON: the authored line and call depth, each watched line's typed value (available, not yet computed, out of
// scope, with its type text and unit flag), and the scripted action (step into and out, continue, cancel). Covered in
// process and through the real binary: the stepped run's report and result, a scripted cancel, the stop bound that
// cancels and marks the report truncated, a breakpoint on a line with no code, a source that does not cook (its line),
// requests rejected before anything runs, and the verb's absence from the MCP tool list (an agent reaches the same
// scripted run as the program.inspect diagnostic command, under the host's Execute grant: test_ceridc_diag.cpp).
// Expected lines come from scanning the committed text; the expected JSON fragments are built here.

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;

namespace
{

constexpr const char* kProgram = CRD_REPO_DIR "/assets/ceir/inspect_demo.ceir";

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc{crd::usize{16} << 20U, nullptr, "ceridc-inspect-tests"};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using crd::u32;
using crd::containers::ConstSpan;
using crd::containers::String;

[[nodiscard]] bool has(const String& report, const char* needle)
{
    return std::strstr(report.c_str(), needle) != nullptr;
}

// How many times `needle` occurs in `report`.
[[nodiscard]] u32 count(const String& report, const char* needle)
{
    u32         n = 0U;
    const char* p = report.c_str();
    while ((p = std::strstr(p, needle)) != nullptr)
    {
        ++n;
        p += std::strlen(needle);
    }
    return n;
}

// The 1-based line holding the n-th occurrence of `needle` in the committed program (the independent oracle).
[[nodiscard]] u32 line_of(const String& text, const char* needle, u32 nth)
{
    const crd::usize nlen = std::strlen(needle);
    u32              line = 1U;
    u32              seen = 0U;
    for (crd::usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text.c_str()[i] == '\n')
        {
            ++line;
            continue;
        }
        if (std::strncmp(text.c_str() + i, needle, nlen) == 0 && seen++ == nth)
        {
            return line;
        }
    }
    return 0U;
}

struct Lines
{
    u32 h    = 0U;
    u32 len  = 0U;
    u32 a    = 0U;
    u32 call = 0U;
    u32 loop = 0U;
    u32 x    = 0U;
    u32 last = 0U; // the closing brace: no code
};

[[nodiscard]] Lines scan()
{
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kProgram)), text));
    Lines l;
    l.h    = line_of(text, "arith.muli", 0U);
    l.len  = line_of(text, "qty<", 0U);
    l.a    = line_of(text, "arith.addi", 0U);
    l.call = line_of(text, "func.call", 0U);
    l.loop = line_of(text, "core.for", 0U);
    l.x    = line_of(text, "arith.addi", 2U);
    l.last = line_of(text, "\n}", 1U) + 1U;
    REQUIRE(l.h != 0U);
    REQUIRE(l.len != 0U);
    REQUIRE(l.a != 0U);
    REQUIRE(l.call != 0U);
    REQUIRE(l.loop != 0U);
    REQUIRE(l.x != 0U);
    return l;
}

// `pattern` with its one `%u` replaced by `line` in decimal (the expected JSON fragment).
[[nodiscard]] String fragment(const char* pattern, u32 line)
{
    char digits[16];
    (void)std::snprintf(digits, sizeof(digits), "%u", line);
    String           s(&g_alloc);
    const char* const at = std::strstr(pattern, "%u");
    REQUIRE(at != nullptr);
    s.append(pattern, static_cast<crd::usize>(at - pattern));
    s.append(digits);
    s.append(at + 2);
    return s;
}

[[nodiscard]] bool has(const String& report, const String& needle)
{
    return has(report, needle.c_str());
}

} // namespace

TEST_CASE("diag 8b: ceridc inspect steps an authored program and reports typed values", "[ceridc][inspect][diag]")
{
    const Lines    ln       = scan();
    const crd::i64 args[1]  = {3};
    const u32      breaks[] = {ln.call};
    const u32      watch[]  = {ln.a, ln.len, ln.call, ln.x};
    const char*    steps[]  = {"into", "out"};
    const String   report =
        crd::ceridc::verb_inspect(kProgram, "main", ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U),
                                  ConstSpan<u32>(watch, 4U), ConstSpan<const char*>(steps, 2U), 0U, &g_alloc);
    INFO(report.c_str());
    CHECK(has(report, "\"ok\":true"));
    CHECK(has(report, fragment("\"breakpoints\":[{\"line\":%u,\"status\":\"bound\",\"sites\":1}]", ln.call)));
    CHECK(count(report, "\"sequence\":") == 3U);

    // Stop 1, at the breakpoint: the caller's values are available (a quantity with its unit), the call's own result
    // is not yet computed and the loop body's value is out of scope.
    CHECK(has(report, fragment("\"sequence\":1,\"reason\":\"breakpoint\",\"line\":%u,", ln.call)));
    CHECK(has(report,
              fragment("{\"line\":%u,\"status\":\"available\",\"type\":\"!i32\",\"unit\":false,\"value\":3}", ln.a)));
    CHECK(has(report, fragment("{\"line\":%u,\"status\":\"available\",\"type\":\"!qty<!i32,L1>\",\"unit\":true,"
                               "\"value\":7}",
                               ln.len)));
    CHECK(has(report,
              fragment("{\"line\":%u,\"status\":\"not-yet-computed\",\"type\":\"!i32\",\"unit\":false}", ln.call)));
    CHECK(has(report, fragment("{\"line\":%u,\"status\":\"out-of-scope\",\"type\":\"!i32\",\"unit\":false}", ln.x)));
    CHECK(has(report, "\"action\":\"into\""));

    // Stop 2, stepped into the callee: its first instr, one frame down, and the caller's value is out of its scope.
    CHECK(has(report, fragment("\"sequence\":2,\"reason\":\"step\",\"line\":%u,", ln.h)));
    CHECK(has(report, "\"depth\":1,"));
    CHECK(has(report, fragment("{\"line\":%u,\"status\":\"out-of-scope\",\"type\":\"!i32\",\"unit\":false}", ln.a)));
    CHECK(has(report, "\"action\":\"out\""));

    // Stop 3, stepped out: back in main at the loop, with the call's result now available.
    CHECK(has(report, fragment("\"sequence\":3,\"reason\":\"step\",\"line\":%u,", ln.loop)));
    CHECK(has(report, fragment("{\"line\":%u,\"status\":\"available\",\"type\":\"!i32\",\"unit\":false,\"value\":36}",
                               ln.call)));
    CHECK(has(report, "\"action\":\"continue\""));
    CHECK(has(report, "\"truncated\":false,\"outcome\":\"finished\",\"results\":[36]"));
}

TEST_CASE("diag 8b: ceridc inspect cancels, bounds its stops and refuses before running", "[ceridc][inspect][diag]")
{
    const Lines    ln      = scan();
    const crd::i64 args[1] = {5};

    SECTION("a scripted cancel ends the run at the stop")
    {
        const u32    breaks[] = {ln.a};
        const char*  steps[]  = {"cancel"};
        const String report =
            crd::ceridc::verb_inspect(kProgram, nullptr, ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U), {},
                                      ConstSpan<const char*>(steps, 1U), 0U, &g_alloc);
        INFO(report.c_str());
        CHECK(has(report, "\"action\":\"cancel\""));
        CHECK(has(report, "\"outcome\":\"cancelled\""));
        CHECK_FALSE(has(report, "\"results\""));
        CHECK(has(report, "\"ok\":true"));
    }
    SECTION("the stop bound cancels the run and marks the report truncated")
    {
        const u32    breaks[] = {ln.x}; // the loop body: five hits for n = 5
        const String report   = crd::ceridc::verb_inspect(kProgram, "main", ConstSpan<crd::i64>(args, 1U),
                                                          ConstSpan<u32>(breaks, 1U), {}, {}, 2U, &g_alloc);
        INFO(report.c_str());
        CHECK(count(report, "\"sequence\":") == 2U);
        CHECK(count(report, fragment("\"reason\":\"breakpoint\",\"line\":%u,", ln.x).c_str()) == 2U);
        CHECK(has(report, "\"truncated\":true,\"outcome\":\"cancelled\""));
    }
    SECTION("a breakpoint on a line with no code binds nothing and the run finishes")
    {
        const u32    breaks[] = {ln.last};
        const String report   = crd::ceridc::verb_inspect(kProgram, "main", ConstSpan<crd::i64>(args, 1U),
                                                          ConstSpan<u32>(breaks, 1U), {}, {}, 0U, &g_alloc);
        INFO(report.c_str());
        CHECK(has(report, fragment("{\"line\":%u,\"status\":\"no-code-at-line\",\"sites\":0}", ln.last)));
        CHECK(has(report, "\"stops\":[]"));
        CHECK(has(report, "\"outcome\":\"finished\",\"results\":[36]"));
    }
    SECTION("requests are rejected before anything runs")
    {
        const char*  bad_step[] = {"jump"};
        const String step =
            crd::ceridc::verb_inspect(kProgram, "main", {}, {}, {}, ConstSpan<const char*>(bad_step, 1U), 0U, &g_alloc);
        CHECK(has(step, "\"ok\":false"));
        CHECK(has(step, "unknown --step action"));
        const u32    zero[] = {0U};
        const String line =
            crd::ceridc::verb_inspect(kProgram, "main", {}, ConstSpan<u32>(zero, 1U), {}, {}, 0U, &g_alloc);
        CHECK(has(line, "1-based"));
        const String missing = crd::ceridc::verb_inspect("no_such_program.ceir", "main", {}, {}, {}, {}, 0U, &g_alloc);
        CHECK(has(missing, "cannot read program"));
        const String entry   = crd::ceridc::verb_inspect(kProgram, "no_such_entry", {}, {}, {}, {}, 0U, &g_alloc);
        CHECK(has(entry, "\"reason\":\"compile-failed\",\"compile_error\":\"no-entry\""));
    }
    SECTION("a source that does not cook names its line")
    {
        String text(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kProgram)), text));
        String      broken(&g_alloc);
        const char* cut = std::strstr(text.c_str(), "arith.muli");
        REQUIRE(cut != nullptr);
        broken.append(text.c_str(), static_cast<crd::usize>(cut - text.c_str()));
        broken.append("arith.muli((");
        broken.append(cut + std::strlen("arith.muli"));
        REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView("ceridc_inspect_broken.ceir")),
                                    crd::containers::StringView(broken.c_str(), broken.size())));
        const String report =
            crd::ceridc::verb_inspect("ceridc_inspect_broken.ceir", "main", {}, {}, {}, {}, 0U, &g_alloc);
        INFO(report.c_str());
        CHECK(has(report, "\"reason\":\"cook-failed\""));
        CHECK(has(report, fragment("\"line\":%u,", ln.h)));
        (void)fs::remove_file(fs::Path(crd::containers::StringView("ceridc_inspect_broken.ceir")));
    }
}

TEST_CASE("diag 8b: the real ceridc binary inspects from the command line", "[ceridc][inspect][diag]")
{
    const Lines ln  = scan();
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    char cmd[2048];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" inspect --program %s --arg 3 --break %u --watch %u --step into > "
                        "ceridc_inspect_out.json",
                        exe, kProgram, ln.call, ln.a);
    REQUIRE(std::system(cmd) == 0); // the exit code mirrors the report's ok
    String out(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView("ceridc_inspect_out.json")), out));
    INFO(out.c_str());
    CHECK(has(out, fragment("\"sequence\":1,\"reason\":\"breakpoint\",\"line\":%u,", ln.call)));
    CHECK(has(out, fragment("\"sequence\":2,\"reason\":\"step\",\"line\":%u,", ln.h)));
    CHECK(has(out, "\"outcome\":\"finished\",\"results\":[36]"));
    (void)fs::remove_file(fs::Path(crd::containers::StringView("ceridc_inspect_out.json")));

    // The MCP transport does not offer the verb: its agent form is the diag tool's program.inspect under Execute.
    const char*  list = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})";
    const String tools = crd::ceridc::mcp_handle({reinterpret_cast<const crd::u8*>(list), std::strlen(list)}, &g_alloc);
    CHECK(has(tools, "import")); // the list answered
    CHECK_FALSE(has(tools, "inspect"));
}
