// test_ceridc_inspect.cpp — DIAG.8b: the HEADLESS consumer of runtime inspection. `ceridc inspect` runs the committed
// authored program assets/ceir/inspect_demo.ceir under an inspect session on the shared InspectHost and reports every
// stop as JSON: the authored line and call depth, each watched line's typed value (available, not yet computed, out of
// scope, with its type text and unit flag), and the scripted action (step into and out, continue, cancel). Covered in
// process and through the real binary: the stepped run's report and result, a scripted cancel, the stop bound that
// cancels and marks the report truncated, a breakpoint on a line with no code, a source that does not cook (its line),
// requests rejected before anything runs, and the verb's absence from the MCP tool list (an agent reaches the same
// scripted run as the program.inspect diagnostic command, under the host's Execute grant: test_ceridc_diag.cpp).
// DIAG.9a: `--record` writes the inspected run as a run record (refusing an existing file before it runs, writing
// nothing for a cancelled run), and another ceridc process reproduces it through `replay.run` without a session.
// `--seed` gives the run a seeded host random source: the held run reads its draws, a malformed seed is refused
// before anything runs, and the record replays in another process without the seed after the program was edited.
// `--clock`, `--sim-time` and `--sim-step` give the run a host clock: the held run reads the given step, malformed
// values are refused before anything runs, and records of a step failure and of a live-wall run replay in other
// processes with no clock, the first after the program was edited, and name the edited read. `--events` gives the run
// an input event queue: the held switch reads the listed resize, handled events finish, an empty list reads none
// events, no list fails the first read, a malformed list is refused before anything runs, the binary's inspected
// record holds the same reads, trace and outcome as replay.record's unobserved one, and every record replays in
// another process with no queue after the program was edited, naming the edited read.
// Expected lines come from scanning the committed text; the expected JSON fragments are built here.

#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/input.hpp>
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

// DIAG.9a: the committed replay demo, the records the inspected runs write, and a binary's answer.
constexpr const char* kReplayDemo = CRD_REPO_DIR "/assets/ceir/replay_demo.ceir";
constexpr const char* kVerbRecord = "ceridc_inspect_verb.crpl";
constexpr const char* kCliRecord  = "ceridc_inspect_cli.crpl";
constexpr const char* kCancelled  = "ceridc_inspect_cancelled.crpl";
constexpr const char* kOut        = "ceridc_inspect_replay_out.json";

// DIAG.9a: the committed random demo, its scratch copy (edited after recording), and the seeded runs' records.
constexpr const char* kRandomDemo    = CRD_REPO_DIR "/assets/ceir/random_demo.ceir";
constexpr const char* kSeedProgram   = "ceridc_inspect_random.ceir";
constexpr const char* kSeedRecord    = "ceridc_inspect_seed_verb.crpl";
constexpr const char* kCliSeedRecord = "ceridc_inspect_seed_cli.crpl";

// DIAG.9a: the committed clock demo, its scratch copy (edited after recording), and the clocked runs' records.
constexpr const char* kClockDemo      = CRD_REPO_DIR "/assets/ceir/clock_demo.ceir";
constexpr const char* kClockProgram   = "ceridc_inspect_clock.ceir";
constexpr const char* kClockRecord    = "ceridc_inspect_clock_verb.crpl";
constexpr const char* kCliClockRecord = "ceridc_inspect_clock_cli.crpl";
constexpr const char* kCliWallRecord  = "ceridc_inspect_wall_cli.crpl";

// DIAG.9a: the committed event demo, its scratch copy (edited after recording), and the evented runs' records.
constexpr const char* kEventDemo        = CRD_REPO_DIR "/assets/ceir/event_demo.ceir";
constexpr const char* kEventProgram     = "ceridc_inspect_event.ceir";
constexpr const char* kEventRecord      = "ceridc_inspect_event_verb.crpl";
constexpr const char* kCliEventRecord   = "ceridc_inspect_event_cli.crpl";
constexpr const char* kPlainEventRecord = "ceridc_inspect_event_plain.crpl";

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

// Draw `n` of stream 0 for `seed`, reduced as random_demo.ceir's switch reads it.
[[nodiscard]] crd::i64 switch_draw(crd::u64 seed, crd::u64 n)
{
    return crd::ceir::input::reduce_draw(crd::ceir::input::SeededInputs::draw(seed, 0U, n), 4U);
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

TEST_CASE("diag 9a: ceridc inspect --record writes the inspected run as a record that replay.run reproduces",
          "[ceridc][inspect][diag]")
{
    for (const char* f : {kVerbRecord, kCliRecord, kCancelled, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kReplayDemo)), text));
    const u32 call = line_of(text, "func.call", 0U);
    REQUIRE(call != 0U);
    const crd::i64 args[1]  = {1};
    const u32      breaks[] = {call};
    const char*    steps[]  = {"into", "out"};

    // In process: stopped, stepped into the callee and out, and recorded; main(1) faults at the switch.
    const String report =
        crd::ceridc::verb_inspect(kReplayDemo, "main", ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U), {},
                                  ConstSpan<const char*>(steps, 2U), 0U, &g_alloc, kVerbRecord);
    INFO(report.c_str());
    CHECK(has(report, "\"action\":\"into\""));
    CHECK(has(report, "\"outcome\":\"error\",\"error\":\"selector-out-of-range\""));
    CHECK(has(report, "\"record\":{\"path\":\"ceridc_inspect_verb.crpl\",\"written\":true,\"status\":\"ok\","
                      "\"asset\":1,\"generation\":1,"));

    // An existing record is refused before anything runs.
    const String again = crd::ceridc::verb_inspect(kReplayDemo, "main", ConstSpan<crd::i64>(args, 1U), {}, {}, {}, 0U,
                                                   &g_alloc, kVerbRecord);
    CHECK(has(again, "\"ok\":false"));
    CHECK(has(again, "refusing to overwrite an existing --record file"));
    CHECK_FALSE(has(again, "\"stops\""));

    // A cancelled run is not a record: a replay would not stop where it stopped.
    const char*  cancel[] = {"cancel"};
    const String cut =
        crd::ceridc::verb_inspect(kReplayDemo, "main", ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U), {},
                                  ConstSpan<const char*>(cancel, 1U), 0U, &g_alloc, kCancelled);
    CHECK(has(cut, "\"outcome\":\"cancelled\""));
    CHECK(
        has(cut, "\"record\":{\"path\":\"ceridc_inspect_cancelled.crpl\",\"written\":false,\"status\":\"cancelled\"}"));
    CHECK_FALSE(fs::exists(fs::Path(crd::containers::StringView(kCancelled))));

    // The real binary records from its command line, and another process replays both records without a session.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr);
    char cmd[2048];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" inspect --program %s --arg 1 --break %u --step into --step out --record %s > %s", exe,
                        kReplayDemo, call, kCliRecord, kOut);
    CHECK(std::system(cmd) != 0); // the run faults, so the report's ok is false
    String cli(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), cli));
    INFO(cli.c_str());
    CHECK(has(cli, "\"written\":true,\"status\":\"ok\""));
    for (const char* rec : {kVerbRecord, kCliRecord})
    {
        (void)std::snprintf(cmd, sizeof(cmd),
                            "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s", exe, rec, kOut);
        REQUIRE(std::system(cmd) == 0);
        String replayed(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), replayed));
        INFO(replayed.c_str());
        CHECK(has(replayed, "\"result\":\"reproduced\""));
        CHECK(has(replayed, "\"asset\":1,\"generation\":1,"));
        CHECK(has(replayed, "\"run\":\"replayed\",\"error\":\"selector-out-of-range\""));
    }
    for (const char* f : {kVerbRecord, kCliRecord, kCancelled, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
}

TEST_CASE("diag 9a: ceridc inspect --seed reads a seeded host's draws and its record replays without the seed",
          "[ceridc][inspect][diag]")
{
    for (const char* f : {kSeedProgram, kSeedRecord, kCliSeedRecord, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kRandomDemo)), text));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(kSeedProgram)),
                                crd::containers::StringView(text.c_str(), text.size())));
    const u32 draw = line_of(text, "input.random() {stream = 0", 0U);
    const u32 sw   = line_of(text, "core.switch", 0U);
    REQUIRE(draw != 0U);
    REQUIRE(sw != 0U);

    // A seed whose main(4) fails, and the draw that fails: from SeededInputs::draw, not from ceridc.
    crd::u64 seed  = 0U;
    u32      fails = 0U;
    for (crd::u64 s = 1U; s < 10000U && seed == 0U; ++s)
    {
        for (u32 i = 0U; i < 4U; ++i)
        {
            if (switch_draw(s, i) == 3)
            {
                seed  = s;
                fails = i;
                break;
            }
        }
    }
    REQUIRE(seed != 0U);
    char seed_text[24];
    (void)std::snprintf(seed_text, sizeof(seed_text), "%llu", static_cast<unsigned long long>(seed));
    INFO("seed " << seed << " fails at draw " << fails);

    // In process: held at every switch, each watched draw is the seeded draw; the run fails at the failing one.
    const crd::i64 args[1]    = {4};
    const u32      breaks[1]  = {sw};
    const u32      watches[1] = {draw};
    const String   report =
        crd::ceridc::verb_inspect(kSeedProgram, "main", ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U),
                                  ConstSpan<u32>(watches, 1U), {}, 0U, &g_alloc, kSeedRecord, seed_text);
    INFO(report.c_str());
    char expect[256];
    (void)std::snprintf(expect, sizeof(expect), R"("random_source":"seeded","seed":%s,)", seed_text);
    CHECK(has(report, expect));
    CHECK(has(report, "\"outcome\":\"error\",\"error\":\"selector-out-of-range\""));
    CHECK(count(report, "\"reason\":\"breakpoint\"") == fails + 1U);
    for (u32 n = 0U; n <= fails; ++n)
    {
        (void)std::snprintf(expect, sizeof(expect),
                            "\"values\":[{\"line\":%u,\"status\":\"available\",\"type\":\"!i32\",\"unit\":false,"
                            "\"value\":%lld}]",
                            draw, static_cast<long long>(switch_draw(seed, n)));
        CHECK(has(report, expect));
    }
    (void)std::snprintf(expect, sizeof(expect), "\"input_reads\":%u}", fails + 1U);
    CHECK(has(report, "\"written\":true,\"status\":\"ok\""));
    CHECK(has(report, expect));

    // No seed: the host has no random source, and the first draw fails.
    const String none =
        crd::ceridc::verb_inspect(kSeedProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {}, {}, 0U, &g_alloc);
    CHECK(has(none, "\"random_source\":\"none\",\"seed\":0,"));
    CHECK(has(none, "\"outcome\":\"error\",\"error\":\"input-unavailable\""));

    // A malformed seed is refused before anything runs.
    for (const char* bad : {"x", "-1", "", "18446744073709551616", "12 "})
    {
        INFO(bad);
        const String refused = crd::ceridc::verb_inspect(kSeedProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {},
                                                         {}, 0U, &g_alloc, nullptr, bad);
        CHECK(has(refused, "a --seed is a decimal u64"));
        CHECK_FALSE(has(refused, "\"stops\""));
    }

    // The real binary records a seeded run; after the program is edited, another process reproduces it from the
    // record alone (no seed), and a third names the edited draw.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr);
    char cmd[2048];
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" inspect --program %s --arg 4 --seed %s --record %s > %s", exe,
                        kSeedProgram, seed_text, kCliSeedRecord, kOut);
    CHECK(std::system(cmd) != 0); // the run faults, so the report's ok is false
    String cli(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), cli));
    INFO(cli.c_str());
    CHECK(has(cli, "\"written\":true,\"status\":\"ok\""));
    CHECK(has(cli, expect));

    String edited(&g_alloc);
    const char* const from = std::strstr(text.c_str(), "{stream = 0, bound = 4}");
    REQUIRE(from != nullptr);
    edited.append(text.c_str(), static_cast<crd::usize>(from - text.c_str()));
    edited.append("{stream = 2, bound = 4}");
    edited.append(from + std::strlen("{stream = 0, bound = 4}"));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(kSeedProgram)),
                                crd::containers::StringView(edited.c_str(), edited.size())));
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kCliSeedRecord, kOut);
    REQUIRE(std::system(cmd) == 0);
    String replayed(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), replayed));
    INFO(replayed.c_str());
    CHECK(has(replayed, "\"result\":\"reproduced\""));
    CHECK(has(replayed, "\"run\":\"replayed\",\"error\":\"selector-out-of-range\""));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kCliSeedRecord, kSeedProgram, kOut);
    REQUIRE(std::system(cmd) == 0);
    String diverged(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), diverged));
    INFO(diverged.c_str());
    CHECK(has(diverged, "\"result\":\"diverged\""));
    CHECK(has(diverged, "\"divergence\":\"input\""));
    (void)std::snprintf(expect, sizeof(expect), "\"line\":%u,", draw);
    CHECK(has(diverged, expect));
    for (const char* f : {kSeedProgram, kSeedRecord, kCliSeedRecord, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
}

TEST_CASE("diag 9a: ceridc inspect reads the host clock it is given and its records replay with no clock",
          "[ceridc][inspect][diag]")
{
    for (const char* f : {kClockProgram, kClockRecord, kCliClockRecord, kCliWallRecord, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kClockDemo)), text));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(kClockProgram)),
                                crd::containers::StringView(text.c_str(), text.size())));
    const u32 step = line_of(text, R"(input.time_step() {domain = "sim"})", 0U);
    const u32 sw   = line_of(text, "core.switch", 0U);
    REQUIRE(step != 0U);
    REQUIRE(sw != 0U);

    // In process: a step over the program's budget (50 ms against 33.3 ms), held at the switch, which then fails.
    const crd::i64                       args[1]    = {7};
    const u32                            breaks[1]  = {sw};
    const u32                            watches[1] = {step};
    const crd::ceridc::InspectClockFlags hitch{nullptr, "4000000000", "50000000"};
    const String report =
        crd::ceridc::verb_inspect(kClockProgram, "main", ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U),
                                  ConstSpan<u32>(watches, 1U), {}, 0U, &g_alloc, kClockRecord, nullptr, &hitch);
    INFO(report.c_str());
    CHECK(has(report, R"("wall_clock":"none","sim_time":"set","sim_time_ns":4000000000,"sim_step":"set",)"
                      R"("sim_step_ns":50000000,)"));
    CHECK(has(report, fragment(R"("values":[{"line":%u,"status":"available","type":"!i64","unit":false,)"
                               R"("value":50000000}])",
                               step)));
    CHECK(has(report, "\"outcome\":\"error\",\"error\":\"selector-out-of-range\""));
    CHECK(has(report, "\"written\":true,\"status\":\"ok\""));
    CHECK(has(report, "\"input_reads\":1}"));

    // No clock: the first read, the step, fails.
    const String none =
        crd::ceridc::verb_inspect(kClockProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {}, {}, 0U, &g_alloc);
    CHECK(has(none, R"("wall_clock":"none","sim_time":"none","sim_time_ns":0,"sim_step":"none","sim_step_ns":0,)"));
    CHECK(has(none, "\"outcome\":\"error\",\"error\":\"input-unavailable\""));

    // A malformed clock flag is refused before anything runs.
    struct Bad
    {
        crd::ceridc::InspectClockFlags flags;
        const char*                    reason;
    };
    const Bad bad[] = {
        {{"frame", nullptr, nullptr}, "a --clock is wall"},
        {{"", nullptr, nullptr}, "a --clock is wall"},
        {{nullptr, "x", nullptr}, "a --sim-time is a decimal i64 count of nanoseconds"},
        {{nullptr, "9223372036854775808", nullptr}, "a --sim-time is a decimal i64 count of nanoseconds"},
        {{nullptr, nullptr, "1.5"}, "a --sim-step is a decimal i64 count of nanoseconds"},
        {{nullptr, nullptr, ""}, "a --sim-step is a decimal i64 count of nanoseconds"},
    };
    for (const Bad& b : bad)
    {
        INFO(b.reason);
        const String refused = crd::ceridc::verb_inspect(kClockProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {},
                                                         {}, 0U, &g_alloc, nullptr, nullptr, &b.flags);
        CHECK(has(refused, b.reason));
        CHECK_FALSE(has(refused, "\"stops\""));
    }

    // The real binary records the step failure, and a run within budget that reads the live wall.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr);
    char cmd[2048];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" inspect --program %s --arg 7 --sim-time 4000000000 --sim-step 50000000 --record %s "
                        "> %s",
                        exe, kClockProgram, kCliClockRecord, kOut);
    CHECK(std::system(cmd) != 0); // the run faults, so the report's ok is false
    String cli(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), cli));
    INFO(cli.c_str());
    CHECK(has(cli, "\"sim_step\":\"set\",\"sim_step_ns\":50000000,"));
    CHECK(has(cli, "\"written\":true,\"status\":\"ok\""));
    CHECK(has(cli, "\"input_reads\":1}"));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" inspect --program %s --arg 7 --clock wall --sim-time 4000000000 --sim-step 16666667 "
                        "--record %s > %s",
                        exe, kClockProgram, kCliWallRecord, kOut);
    REQUIRE(std::system(cmd) == 0); // the run finishes
    String wall(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), wall));
    INFO(wall.c_str());
    CHECK(has(wall, R"("wall_clock":"live","sim_time":"set","sim_time_ns":4000000000,"sim_step":"set",)"));
    CHECK(has(wall, "\"outcome\":\"finished\""));
    CHECK(has(wall, "\"input_reads\":3}"));

    // The program file is edited: its step read now asks for the frame domain.
    String            edited(&g_alloc);
    const char* const from = std::strstr(text.c_str(), R"(input.time_step() {domain = "sim"})");
    REQUIRE(from != nullptr);
    const char* const to = R"(input.time_step() {domain = "frame"})";
    edited.append(text.c_str(), static_cast<crd::usize>(from - text.c_str()));
    edited.append(to);
    edited.append(from + std::strlen(R"(input.time_step() {domain = "sim"})"));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(kClockProgram)),
                                crd::containers::StringView(edited.c_str(), edited.size())));

    // Other processes, with no clock: each record reproduces from its own program and reads alone (the in-process
    // record too), and against the edited file the step failure is named at the edited read.
    for (const char* rec : {kClockRecord, kCliClockRecord, kCliWallRecord})
    {
        INFO(rec);
        (void)std::snprintf(cmd, sizeof(cmd),
                            "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s", exe, rec, kOut);
        REQUIRE(std::system(cmd) == 0);
        String replayed(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), replayed));
        INFO(replayed.c_str());
        CHECK(has(replayed, "\"result\":\"reproduced\""));
    }
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kCliClockRecord, kClockProgram, kOut);
    REQUIRE(std::system(cmd) == 0);
    String diverged(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), diverged));
    INFO(diverged.c_str());
    CHECK(has(diverged, "\"result\":\"diverged\""));
    CHECK(has(diverged, "\"divergence\":\"input\""));
    CHECK(has(diverged, "\"recorded_input\":\"time_step\""));
    CHECK(has(diverged, fragment("\"line\":%u,", step)));
    for (const char* f : {kClockProgram, kClockRecord, kCliClockRecord, kCliWallRecord, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
}

TEST_CASE("diag 9a event: ceridc inspect reads the input events it is given and its records replay with no queue",
          "[ceridc][inspect][diag]")
{
    for (const char* f : {kEventProgram, kEventRecord, kCliEventRecord, kPlainEventRecord, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kEventDemo)), text));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(kEventProgram)),
                                crd::containers::StringView(text.c_str(), text.size())));
    const char* const second_read = "%6, %7, %8, %9, %10 = input.event() {queue = 0} : !i64";
    const u32         first       = line_of(text, "%1, %2, %3, %4, %5 = input.event()", 0U);
    const u32         second      = line_of(text, second_read, 0U);
    const u32         sw          = line_of(text, "core.switch", 0U);
    REQUIRE(first != 0U);
    REQUIRE(second != 0U);
    REQUIRE(sw != 0U);

    // In process: a key and an unhandled resize, held at the switch, which then fails.
    const crd::i64 args[1]    = {7};
    const u32      breaks[1]  = {sw};
    const u32      watches[1] = {second};
    const String   report     = crd::ceridc::verb_inspect(
        kEventProgram, "main", ConstSpan<crd::i64>(args, 1U), ConstSpan<u32>(breaks, 1U), ConstSpan<u32>(watches, 1U),
        {}, 0U, &g_alloc, kEventRecord, nullptr, nullptr, "key_down:65:3,resize:1280:720");
    INFO(report.c_str());
    CHECK(has(report, R"("sim_step_ns":0,"event_queue":"open","input_events":2,)"));
    CHECK(has(report, fragment(R"("values":[{"line":%u,"status":"available","type":"!i64","unit":false,)"
                               R"("value":8}])",
                               second)));
    CHECK(has(report, "\"outcome\":\"error\",\"error\":\"selector-out-of-range\""));
    CHECK(has(report, "\"written\":true,\"status\":\"ok\""));
    CHECK(has(report, "\"input_reads\":2}"));

    // Handled events finish on their values; an empty list reads none events; no list has no queue.
    const String handled = crd::ceridc::verb_inspect(kEventProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {}, {},
                                                     0U, &g_alloc, nullptr, nullptr, nullptr,
                                                     "key_down:65:3,mouse_move:5:9");
    CHECK(has(handled, "\"outcome\":\"finished\",\"results\":[86]"));
    const String empty = crd::ceridc::verb_inspect(kEventProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {}, {}, 0U,
                                                   &g_alloc, nullptr, nullptr, nullptr, "");
    CHECK(has(empty, "\"event_queue\":\"open\",\"input_events\":0,"));
    CHECK(has(empty, "\"outcome\":\"finished\",\"results\":[7]"));
    const String none =
        crd::ceridc::verb_inspect(kEventProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {}, {}, 0U, &g_alloc);
    CHECK(has(none, "\"event_queue\":\"none\",\"input_events\":0,"));
    CHECK(has(none, "\"outcome\":\"error\",\"error\":\"input-unavailable\""));
    CHECK(has(none, fragment("\"fault\":{\"line\":%u,", first)));

    // A malformed list is refused before anything runs.
    for (const char* bad : {"jump:1", "key_down:65536", "resize:1:2:3", "mouse_move:1", ","})
    {
        INFO(bad);
        const String refused = crd::ceridc::verb_inspect(kEventProgram, "main", ConstSpan<crd::i64>(args, 1U), {}, {},
                                                         {}, 0U, &g_alloc, nullptr, nullptr, nullptr, bad);
        CHECK(has(refused, "--events takes at most 32 comma-separated events"));
        CHECK_FALSE(has(refused, "\"stops\""));
    }

    // The real binary records the inspected run, and replay.record records the same run unobserved: the same reads,
    // trace and outcome.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr);
    char cmd[2048];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" inspect --program %s --arg 7 --break %u --events key_down:65:3,resize:1280:720 "
                        "--record %s > %s",
                        exe, kEventProgram, sw, kCliEventRecord, kOut);
    CHECK(std::system(cmd) != 0); // the run faults, so the report's ok is false
    String cli(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), cli));
    INFO(cli.c_str());
    CHECK(has(cli, "\"event_queue\":\"open\",\"input_events\":2,"));
    CHECK(has(cli, "\"written\":true,\"status\":\"ok\""));
    CHECK(has(cli, "\"input_reads\":2}"));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=7 "
                        "--param events=key_down:65:3,resize:1280:720 --grant execute,record --root . > %s",
                        exe, kEventProgram, kPlainEventRecord, kOut);
    REQUIRE(std::system(cmd) == 0);
    {
        const auto decode = [](const char* path, crd::ceir::cook::ReplayRecord& rec)
        {
            String bytes(&g_alloc);
            REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(path)), bytes));
            REQUIRE(crd::ceir::cook::decode_record({reinterpret_cast<const crd::u8*>(bytes.c_str()), bytes.size()},
                                                   rec) == crd::ceir::cook::RecordError::Ok);
        };
        crd::ceir::cook::ReplayRecord inspected(&g_alloc);
        crd::ceir::cook::ReplayRecord plain(&g_alloc);
        decode(kCliEventRecord, inspected);
        decode(kPlainEventRecord, plain);
        REQUIRE(inspected.input_reads.size() == 2U);
        REQUIRE(plain.input_reads.size() == 2U);
        CHECK(inspected.input_reads[0] == plain.input_reads[0]);
        CHECK(inspected.input_reads[1] == plain.input_reads[1]);
        CHECK(inspected.input_reads_total == plain.input_reads_total);
        CHECK(inspected.events_total == plain.events_total);
        REQUIRE(inspected.events.size() == plain.events.size());
        for (crd::usize i = 0; i < plain.events.size(); ++i)
        {
            CHECK(inspected.events[i] == plain.events[i]);
        }
        CHECK(inspected.error == plain.error);
        CHECK(inspected.fault_op == plain.fault_op);
        for (u32 i = 0U; i < crd::ceir::cook::kReplayInputs; ++i)
        {
            CHECK(inspected.inputs[i].state == plain.inputs[i].state);
        }
    }

    // The program file is edited: its second read now takes from queue 1.
    String            edited(&g_alloc);
    const char* const from = std::strstr(text.c_str(), second_read);
    REQUIRE(from != nullptr);
    edited.append(text.c_str(), static_cast<crd::usize>(from - text.c_str()));
    edited.append("%6, %7, %8, %9, %10 = input.event() {queue = 1} : !i64");
    edited.append(from + std::strlen(second_read));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(kEventProgram)),
                                crd::containers::StringView(edited.c_str(), edited.size())));

    // Other processes, with no event queue: each record reproduces from its own program and reads alone, and against
    // the edited file the second read is named.
    for (const char* rec : {kEventRecord, kCliEventRecord, kPlainEventRecord})
    {
        INFO(rec);
        (void)std::snprintf(cmd, sizeof(cmd),
                            "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s", exe, rec, kOut);
        REQUIRE(std::system(cmd) == 0);
        String replayed(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), replayed));
        INFO(replayed.c_str());
        CHECK(has(replayed, "\"result\":\"reproduced\""));
    }
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kCliEventRecord, kEventProgram, kOut);
    REQUIRE(std::system(cmd) == 0);
    String diverged(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(crd::containers::StringView(kOut)), diverged));
    INFO(diverged.c_str());
    CHECK(has(diverged, "\"result\":\"diverged\""));
    CHECK(has(diverged, "\"divergence\":\"input\",\"index\":1,"));
    CHECK(has(diverged, "\"recorded_input\":\"event\",\"observed_input\":\"event\""));
    CHECK(has(diverged, fragment("\"line\":%u,", second)));
    for (const char* f : {kEventProgram, kEventRecord, kCliEventRecord, kPlainEventRecord, kOut})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(f)));
    }
}
