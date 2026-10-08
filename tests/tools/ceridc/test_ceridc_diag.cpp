// test_ceridc_diag.cpp — the diag verb binds the host's typed diagnostic command service to the existing CLI and MCP
// transports. A native call, the in-process verb, the in-process MCP tool and the real ceridc binary (from the
// command line and over MCP stdio) return the same response bytes for the same service state, including pagination
// and refusals; the MCP tool's arguments cannot raise the grant the process started with; and malformed numeric
// arguments are protocol faults, while out-of-range counts reach the service and are refused there. MCP replies are
// parsed with the JSON reader and the tool text compared byte for byte. Every service here binds ceridc's own commands
// (bind_diag_commands), so the comparison covers program.provenance and replay.prepare over the committed authored
// CEIR program, gpu.resources (no GPU context in ceridc, so its context and frame-graph evidence answer unavailable)
// and program.inspect: the authored program run to a script given as named arguments, which needs the Execute grant
// and reaches the agent transport only through this tool. DIAG.9a: replay.record (Execute and Record) writes a run
// record from one ceridc process, and replay.run in another process reproduces it after the program file is edited.
// The same holds for a host record (`executor=host`): ceridc binds the host provider as the replay commands' host
// executor, its diag and mcp verbs own the crd::jobs pool it runs on, and this binary's listener owns the pool for the
// native and in-process calls. A record of a program that draws random values (`seed=`) holds every draw, so another
// process reproduces the seeded failure without the seed.

#include <crd/assetio/json.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs   = crd::platform::fs;
namespace json = crd::assetio::json;

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc{crd::usize{16} << 20U, nullptr, "ceridc-diag-tests"};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagServiceConfig;

// Each case owns its files: CTest may run the cases in parallel in one working directory.
constexpr const char* kParityBundle  = "ceridc_diag_bundle_parity.cdb";
constexpr const char* kBinaryBundle  = "ceridc_diag_bundle_binary.cdb";
constexpr const char* kParityProgram = "ceridc_diag_program_parity.ceir";
constexpr const char* kBinaryProgram = "ceridc_diag_program_binary.ceir";
constexpr const char* kInspectProgram = "ceridc_diag_program_inspect.ceir";
constexpr const char* kInspectBinary  = "ceridc_diag_program_inspect_binary.ceir";
constexpr const char* kDemoProgram   = CRD_REPO_DIR "/assets/ceir/inspect_demo.ceir";
// DIAG.9a: the committed replay demo, its working copy, the run record and the binary's answer.
constexpr const char* kReplayDemo    = CRD_REPO_DIR "/assets/ceir/replay_demo.ceir";
constexpr const char* kReplayProgram = "ceridc_diag_replay.ceir";
constexpr const char* kReplayRecord  = "ceridc_diag_replay.crpl";
constexpr const char* kReplayOut     = "ceridc_diag_replay_out.json";
constexpr const char* kBiasLine      = "%9 = arith.const() {value = 1} : !i32";
// DIAG.9a: the committed host replay demo, its working copy, the host records and the binary's answer.
constexpr const char* kHostDemo    = CRD_REPO_DIR "/assets/ceir/host_replay_demo.ceir";
constexpr const char* kHostProgram = "ceridc_diag_host_replay.ceir";
constexpr const char* kHostRecord  = "ceridc_diag_host_replay.crpl";
constexpr const char* kHostNative  = "ceridc_diag_host_replay_native.crpl";
constexpr const char* kHostOut     = "ceridc_diag_host_replay_out.json";
constexpr const char* kHostIn      = "ceridc_diag_host_replay_in.jsonl";
constexpr const char* kLaunchLine  = "%4 = arith.const() {value = 5} : !i32";
// DIAG.9a host inputs: the committed random demo, its working copy, the records and the binary's answer.
constexpr const char* kRandomDemo    = CRD_REPO_DIR "/assets/ceir/random_demo.ceir";
constexpr const char* kRandomProgram = "ceridc_diag_random.ceir";
constexpr const char* kRandomRecord  = "ceridc_diag_random.crpl";
constexpr const char* kRandomNative  = "ceridc_diag_random_native.crpl";
constexpr const char* kRandomOut     = "ceridc_diag_random_out.json";
constexpr const char* kRandomIn      = "ceridc_diag_random_in.jsonl";
constexpr const char* kDrawLine      = "%4 = input.random() {stream = 0, bound = 4} : !i32";
// DIAG.9a clock inputs: the committed clock demo, its copy, records and outputs.
constexpr const char* kClockDemo    = CRD_REPO_DIR "/assets/ceir/clock_demo.ceir";
constexpr const char* kClockProgram = "ceridc_diag_clock.ceir";
constexpr const char* kClockRecord  = "ceridc_diag_clock.crpl";
constexpr const char* kClockNative  = "ceridc_diag_clock_native.crpl";
constexpr const char* kClockPass    = "ceridc_diag_clock_pass.crpl";
constexpr const char* kClockOut     = "ceridc_diag_clock_out.json";
constexpr const char* kClockIn      = "ceridc_diag_clock_in.jsonl";
constexpr const char* kStepLine     = "%1 = input.time_step() {domain = \"sim\"} : !i64";
constexpr const char* kAwaitLine   = "%6 = async.await(%3) : !i32";
// DIAG.9a input events: the committed event demo, its copy, records and outputs.
constexpr const char* kEventDemo       = CRD_REPO_DIR "/assets/ceir/event_demo.ceir";
constexpr const char* kEventProgram    = "ceridc_diag_event.ceir";
constexpr const char* kEventRecord     = "ceridc_diag_event.crpl";
constexpr const char* kEventNative     = "ceridc_diag_event_native.crpl";
constexpr const char* kEventHost       = "ceridc_diag_event_host.crpl";
constexpr const char* kEventOut        = "ceridc_diag_event_out.json";
constexpr const char* kEventIn         = "ceridc_diag_event_in.jsonl";
constexpr const char* kSecondEventLine = "%6, %7, %8, %9, %10 = input.event() {queue = 0} : !i64";

// The host provider's crd::jobs pool for the native and in-process calls of this binary (ceridc's own diag and mcp
// verbs own theirs).
struct DiagJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(Catch::TestRunInfo const& /*info*/) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 4U, .frame_alloc_bytes = 16U << 20U});
    }
    void testCaseEnded(Catch::TestCaseStats const& /*stats*/) override { crd::jobs::frame_reset(); }
    void testRunEnded(Catch::TestRunStats const& /*stats*/) override { crd::jobs::shutdown(); }
};

[[nodiscard]] bool has(const String& s, const char* needle)
{
    return std::strstr(s.c_str(), needle) != nullptr;
}

[[nodiscard]] StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

// The services under comparison: the cwd is the file root, as for the binary below.
[[nodiscard]] DiagServiceConfig rooted()
{
    DiagServiceConfig config;
    config.root = ".";
    return config;
}

void write_bundle(const char* name)
{
    crd::perf::BundleManifest man;
    man.schema_version = crd::perf::kDiagnosticSchemaVersion;
    man.absent_tags.push_back(static_cast<crd::u32>(crd::perf::BundleSectionTag::SymbolIndex));
    const crd::containers::Array<crd::u8> manifest = crd::perf::serialize_manifest(man, &g_alloc);
    const crd::u8                         crash[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    crd::perf::BundleWriter               writer(&g_alloc);
    REQUIRE(writer.add_section(crd::perf::BundleSectionTag::Manifest, {manifest.data(), manifest.size()}) ==
            crd::perf::BundleWriter::AddStatus::Ok);
    REQUIRE(writer.add_section(crd::perf::BundleSectionTag::CrashRecord, {crash, sizeof(crash)}) ==
            crd::perf::BundleWriter::AddStatus::Ok);
    REQUIRE(writer.add_absent(crd::perf::BundleSectionTag::SymbolIndex) ==
            crd::perf::BundleWriter::AddStatus::StoredAbsent);
    const crd::containers::Array<crd::u8> bytes = writer.finish(0U);
    REQUIRE(fs::write_file_binary(fs::Path(StringView(name)), {bytes.data(), bytes.size()}));
}

// A copy of the committed authored program in the working directory (the services' file root).
void write_program(const char* name)
{
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kDemoProgram)), text));
    REQUIRE(fs::write_file_text(fs::Path(StringView(name)), StringView{text.data(), text.size()}));
}

// The 1-based line of the n-th line of the committed program holding `needle` (the independent oracle).
[[nodiscard]] crd::u32 line_of(const char* needle, crd::u32 nth)
{
    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kDemoProgram)), text));
    crd::u32    line = 1U;
    crd::u32    seen = 0U;
    const char* p    = text.c_str();
    for (; *p != '\0'; ++p)
    {
        if (*p == '\n')
        {
            ++line;
            continue;
        }
        if (std::strncmp(p, needle, std::strlen(needle)) == 0 && seen++ == nth)
        {
            return line;
        }
    }
    FAIL("needle not in the program");
    return 0U;
}

// A service as every ceridc process builds one: crd-perf's built-ins plus ceridc's own commands.
void bind(DiagCommandService& service)
{
    REQUIRE(crd::ceridc::bind_diag_commands(service));
}

// One tools/call of the diag tool with `arguments` (a JSON object text), through the real protocol handler.
[[nodiscard]] String call(DiagCommandService* service, const char* arguments, crd::i64 id = 7)
{
    char request[1024];
    (void)std::snprintf(request, sizeof(request),
                        R"({"jsonrpc":"2.0","id":%lld,"method":"tools/call","params":{"name":"diag","arguments":%s}})",
                        static_cast<long long>(id), arguments);
    return crd::ceridc::mcp_handle({reinterpret_cast<const crd::u8*>(request), std::strlen(request)}, &g_alloc,
                                   service);
}

struct ToolReply
{
    bool   parsed   = false;
    bool   is_error = false;
    bool   protocol_error = false;
    String text{&g_alloc};
};

// The tool's text (decoded) and isError flag from one JSON-RPC reply line.
[[nodiscard]] ToolReply reply_of(StringView line)
{
    ToolReply     out;
    json::JsonDoc doc(&g_alloc);
    if (!json::parse({reinterpret_cast<const crd::u8*>(line.data()), line.size()}, doc))
    {
        return out;
    }
    out.parsed = true;
    if (json::find(doc, doc.root, "error") != json::kInvalid)
    {
        out.protocol_error = true;
        return out;
    }
    const crd::u32 result  = json::find(doc, doc.root, "result");
    const crd::u32 content = json::find(doc, result, "content");
    const crd::u32 text    = json::find(doc, json::at(doc, content, 0U), "text");
    if (text == json::kInvalid || doc.nodes[text].type != json::JsonType::String)
    {
        out.parsed = false;
        return out;
    }
    out.text.append(StringView{doc.strings.data() + doc.nodes[text].str_off, doc.nodes[text].str_len});
    out.is_error = json::as_bool(doc, json::find(doc, result, "isError"), false);
    return out;
}

// The MCP `arguments` object for a request (only the fields that differ from the defaults).
[[nodiscard]] String arguments_of(const DiagRequest& r)
{
    String a(&g_alloc);
    char   buf[512];
    (void)std::snprintf(buf, sizeof(buf), R"({"command":"%.*s")", static_cast<int>(r.command.size()), r.command.data());
    a.append(buf);
    if (!r.path.empty())
    {
        (void)std::snprintf(buf, sizeof(buf), R"(,"path":"%.*s")", static_cast<int>(r.path.size()), r.path.data());
        a.append(buf);
    }
    (void)std::snprintf(buf, sizeof(buf), R"(,"cursor":%llu,"page_items":%u,"page_bytes":%u,"schema":%u)",
                        static_cast<unsigned long long>(r.cursor), r.page_items, r.page_bytes, r.schema_version);
    a.append(buf);
    if (!r.args.empty())
    {
        a.append(R"(,"args":{)");
        for (crd::usize i = 0U; i < r.args.size(); ++i)
        {
            (void)std::snprintf(buf, sizeof(buf), R"(%s"%.*s":"%.*s")", i == 0U ? "" : ",",
                                static_cast<int>(r.args[i].name.size()), r.args[i].name.data(),
                                static_cast<int>(r.args[i].value.size()), r.args[i].value.data());
            a.append(buf);
        }
        a.push_back('}');
    }
    a.push_back('}');
    return a;
}

} // namespace

CATCH_REGISTER_LISTENER(DiagJobsListener)

TEST_CASE("diag: a native caller, the verb and the MCP tool return the same bounded result", "[ceridc][diag]")
{
    write_bundle(kParityBundle);
    write_program(kParityProgram);
    const crd::perf::DiagAuthoritySet read = crd::perf::authority_bit(DiagAuthority::Read);
    DiagCommandService                native(read, rooted(), &g_alloc);
    DiagCommandService                verb(read, rooted(), &g_alloc);
    DiagCommandService                mcp(read, rooted(), &g_alloc);
    bind(native);
    bind(verb);
    bind(mcp);

    // A script of requests: pages walked by cursor, refusals of every class, and a stale cursor after a new snapshot.
    struct Step
    {
        const char* command;
        const char* path;
        crd::u32    page_items;
        crd::u32    page_bytes;
        crd::u32    schema;
        int         cursor_from; // -1: 0; otherwise the next_cursor of that earlier step
    };
    const Step steps[] = {
        {"diag.commands", nullptr, 3U, 0U, 1U, -1},
        {"diag.commands", nullptr, 3U, 0U, 1U, 0},
        {"diag.commands", nullptr, 3U, 0U, 1U, 1},
        {"bundle.inspect", kParityBundle, 2U, 0U, 1U, -1},
        {"bundle.inspect", kParityBundle, 2U, 0U, 1U, 3},
        {"diag.commands", nullptr, 3U, 0U, 1U, 1}, // stale: bundle.inspect replaced the snapshot
        {"diag.capabilities", nullptr, 0U, 0U, 1U, -1},
        {"capture.start", nullptr, 0U, 0U, 1U, -1},   // unauthorized
        {"diag.commands", nullptr, 1000U, 0U, 1U, -1}, // oversized
        {"diag.commands", nullptr, 0U, 10U, 1U, -1},   // page bytes below the minimum
        {"bundle.inspect", "../x.cdb", 0U, 0U, 1U, -1}, // unsafe path
        {"diag.commands", nullptr, 0U, 0U, 2U, -1},    // schema
        {"no.such", nullptr, 0U, 0U, 1U, -1},
        {"program.provenance", kParityProgram, 4U, 0U, 1U, -1},
        {"program.provenance", kParityProgram, 4U, 0U, 1U, 13},
        {"program.provenance", "no_such.ceir", 0U, 0U, 1U, -1}, // the command's own failure, after the checks
        {"gpu.resources", nullptr, 2U, 0U, 1U, -1},
        {"gpu.resources", nullptr, 2U, 0U, 1U, 16},
        {"gpu.resources", "x.bin", 0U, 0U, 1U, -1}, // takes no path
        {"replay.prepare", kParityProgram, 4U, 0U, 1U, -1},
        {"replay.prepare", kParityProgram, 4U, 0U, 1U, 19},
    };
    crd::containers::Array<crd::u64> next(&g_alloc);
    for (const Step& s : steps)
    {
        DiagRequest r;
        r.command        = s.command;
        r.path           = s.path != nullptr ? StringView{s.path} : StringView{};
        r.page_items     = s.page_items;
        r.page_bytes     = s.page_bytes;
        r.schema_version = s.schema;
        r.cursor         = s.cursor_from < 0 ? 0U : next[static_cast<crd::usize>(s.cursor_from)];
        INFO(s.command);

        const crd::perf::DiagResult direct = native.execute(r);
        next.push_back(direct.next_cursor);
        const String    through_verb = crd::ceridc::verb_diag(verb, r, &g_alloc);
        const String    args         = arguments_of(r);
        const ToolReply through_mcp  = reply_of(view(call(&mcp, args.c_str())));
        INFO(direct.json.c_str());
        REQUIRE(through_mcp.parsed);
        CHECK(view(through_verb) == view(direct.json));
        CHECK(view(through_mcp.text) == view(direct.json));
        CHECK(through_mcp.is_error == (direct.status != crd::perf::DiagStatus::Ok));
    }
    // The commands, bundle, program, GPU and replay snapshots, capabilities and the missing program; refusals ran
    // nothing.
    CHECK(native.handler_runs() == 7U);
    CHECK(verb.handler_runs() == native.handler_runs());
    CHECK(mcp.handler_runs() == native.handler_runs());
    CHECK(mcp.file_bytes_read() == native.file_bytes_read());
    CHECK(native.file_bytes_read() > 0U);
    (void)fs::remove_file(fs::Path(StringView(kParityBundle)));
    (void)fs::remove_file(fs::Path(StringView(kParityProgram)));
}

TEST_CASE("diag: program.inspect answers the same bytes natively, through the verb and the MCP tool",
          "[ceridc][diag]")
{
    write_program(kInspectProgram);
    char call_line[16];
    char a_line[16];
    char watches[32];
    (void)std::snprintf(call_line, sizeof(call_line), "%u", line_of("func.call", 0U));
    (void)std::snprintf(a_line, sizeof(a_line), "%u", line_of("arith.addi", 0U));
    (void)std::snprintf(watches, sizeof(watches), "%s,%s", a_line, call_line);
    const crd::perf::DiagArg script[] = {
        {"args", "3"}, {"breaks", call_line}, {"watches", watches}, {"steps", "into,out"}};
    const crd::perf::DiagArg bad_step[] = {{"steps", "jump"}};
    const crd::perf::DiagArg on_list[]  = {{"breaks", call_line}};

    const crd::perf::DiagAuthoritySet read    = crd::perf::authority_bit(DiagAuthority::Read);
    const crd::perf::DiagAuthoritySet execute = read | crd::perf::authority_bit(DiagAuthority::Execute);
    for (const crd::perf::DiagAuthoritySet grant : {execute, read})
    {
        INFO(grant);
        DiagCommandService native(grant, rooted(), &g_alloc);
        DiagCommandService verb(grant, rooted(), &g_alloc);
        DiagCommandService mcp(grant, rooted(), &g_alloc);
        bind(native);
        bind(verb);
        bind(mcp);

        struct Step
        {
            crd::containers::ConstSpan<crd::perf::DiagArg> args;
            const char*                                    command;
            int                                            cursor_from; // -1: 0; else that step's next_cursor
        };
        const Step steps[] = {
            {{script, 4U}, "program.inspect", -1},
            {{script, 4U}, "program.inspect", 0},
            {{bad_step, 1U}, "program.inspect", -1},
            {{on_list, 1U}, "diag.commands", -1}, // a command without arguments refuses them
        };
        crd::containers::Array<crd::u64> next(&g_alloc);
        for (const Step& s : steps)
        {
            DiagRequest r;
            r.command    = s.command;
            r.path       = s.command[0] == 'p' ? StringView{kInspectProgram} : StringView{};
            r.args       = s.args;
            r.page_items = 6U;
            r.cursor     = s.cursor_from < 0 ? 0U : next[static_cast<crd::usize>(s.cursor_from)];
            const crd::perf::DiagResult direct = native.execute(r);
            next.push_back(direct.next_cursor);
            INFO(direct.json.c_str());
            const String    through_verb = crd::ceridc::verb_diag(verb, r, &g_alloc);
            const ToolReply through_mcp  = reply_of(view(call(&mcp, arguments_of(r).c_str())));
            REQUIRE(through_mcp.parsed);
            CHECK(view(through_verb) == view(direct.json));
            CHECK(view(through_mcp.text) == view(direct.json));
            CHECK(through_mcp.is_error == (direct.status != crd::perf::DiagStatus::Ok));
        }
        if (grant == execute)
        {
            // A breakpoint item, three stops with two value items each and the result: 11 items over two pages.
            CHECK(next[0] != 0U);
            CHECK(next[1] == 0U);
            CHECK(native.handler_runs() == 1U); // the second page is cut from the first run's snapshot
        }
        else
        {
            DiagRequest bare;
            bare.command                        = "program.inspect";
            bare.path                           = kInspectProgram;
            const crd::perf::DiagResult refused = native.execute(bare);
            CHECK(refused.status == crd::perf::DiagStatus::Unauthorized);
            CHECK(has(refused.json, "the command needs execute authority; the host granted read"));
            CHECK(native.handler_runs() == 0U);
        }
        CHECK(verb.handler_runs() == native.handler_runs());
        CHECK(mcp.handler_runs() == native.handler_runs());
    }

    // The named arguments are an object of strings; anything else is a protocol fault that reaches nothing.
    DiagCommandService svc(execute, rooted(), &g_alloc);
    bind(svc);
    for (const char* bad : {R"({"command":"program.inspect","path":"x.ceir","args":["3"]})",
                            R"({"command":"program.inspect","path":"x.ceir","args":{"args":3}})"})
    {
        INFO(bad);
        const ToolReply r = reply_of(view(call(&svc, bad)));
        REQUIRE(r.parsed);
        CHECK(r.protocol_error);
    }
    CHECK(svc.handler_runs() == 0U);
    (void)fs::remove_file(fs::Path(StringView(kInspectProgram)));
}

TEST_CASE("diag: the MCP tool cannot raise the host's grant and rejects malformed counts", "[ceridc][diag]")
{
    DiagCommandService svc(crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &g_alloc);
    bind(svc);

    // Authority smuggled into the arguments is ignored: the process's grant decides, before any work.
    const ToolReply smuggled = reply_of(view(
        call(&svc, R"({"command":"capture.start","grant":"read,record","authority":"record","granted":63})")));
    REQUIRE(smuggled.parsed);
    CHECK(smuggled.is_error);
    CHECK(has(smuggled.text, "\"status\":\"unauthorized\""));
    CHECK(has(smuggled.text, "\"grant\":\"read\""));
    CHECK(svc.handler_runs() == 0U);
    CHECK_FALSE(svc.capture_open());

    // A count beyond u32 reaches the service as the largest u32 and is refused there as oversized.
    const ToolReply huge = reply_of(view(call(&svc, R"({"command":"diag.commands","page_items":1e12})")));
    REQUIRE(huge.parsed);
    CHECK(has(huge.text, "\"status\":\"oversized\""));
    CHECK(svc.handler_runs() == 0U);

    // Negative, fractional, non-numeric and missing values are protocol faults; nothing reaches the service.
    for (const char* bad : {R"({"command":"diag.commands","cursor":-1})", R"({"command":"diag.commands","cursor":1.5})",
                            R"({"command":"diag.commands","page_items":"3"})", R"({"path":"x"})",
                            R"({"command":"bundle.inspect","path":7})"})
    {
        INFO(bad);
        const ToolReply r = reply_of(view(call(&svc, bad)));
        REQUIRE(r.parsed);
        CHECK(r.protocol_error);
    }
    CHECK(svc.handler_runs() == 0U);

    // The tool is listed only when a service is bound, and its schema has no authority field.
    const char*  list  = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})";
    const String bound = crd::ceridc::mcp_handle({reinterpret_cast<const crd::u8*>(list), std::strlen(list)},
                                                 &g_alloc, &svc);
    CHECK(has(bound, "\"name\":\"diag\""));
    CHECK(has(bound, "\"page_bytes\":{\"type\":\"number\"}"));
    CHECK_FALSE(has(bound, "grant"));
    CHECK_FALSE(has(bound, "authority"));
    const String unbound =
        crd::ceridc::mcp_handle({reinterpret_cast<const crd::u8*>(list), std::strlen(list)}, &g_alloc);
    CHECK(has(unbound, "\"name\":\"import\""));
    CHECK_FALSE(has(unbound, "\"name\":\"diag\""));
    CHECK(reply_of(view(call(nullptr, R"({"command":"diag.commands"})"))).protocol_error);
}

TEST_CASE("diag: the real ceridc binary answers the same bytes from the command line and over MCP stdio",
          "[ceridc][diag]")
{
    write_bundle(kBinaryBundle);
    write_program(kBinaryProgram);
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake

    const crd::perf::DiagAuthoritySet read = crd::perf::authority_bit(DiagAuthority::Read);
    char                              cmd[1024];

    // Command line, default grant: the same bytes as a fresh native service with the same root and commands (the
    // listing shows the binary binds the same commands; the program answer comes from its registered command).
    struct Cli
    {
        const char* command;
        const char* path;
        crd::u32    page_items;
    };
    for (const Cli& c : {Cli{"bundle.inspect", kBinaryBundle, 2U}, Cli{"diag.commands", nullptr, 0U},
                         Cli{"program.provenance", kBinaryProgram, 4U}, Cli{"gpu.resources", nullptr, 0U},
                         Cli{"replay.prepare", kBinaryProgram, 4U}})
    {
        INFO(c.command);
        DiagCommandService native(read, rooted(), &g_alloc);
        bind(native);
        DiagRequest r;
        r.command                            = c.command;
        r.path                               = c.path != nullptr ? StringView{c.path} : StringView{};
        r.page_items                         = c.page_items;
        const crd::perf::DiagResult expected = native.execute(r);
        REQUIRE(expected.status == crd::perf::DiagStatus::Ok);
        if (c.path != nullptr)
        {
            (void)std::snprintf(cmd, sizeof(cmd),
                                "\"%s\" diag --command %s --path %s --page-items %u --root . > ceridc_diag_out.json",
                                exe, c.command, c.path, c.page_items);
        }
        else
        {
            (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command %s --root . > ceridc_diag_out.json", exe,
                                c.command);
        }
        REQUIRE(std::system(cmd) == 0);
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_out.json")), out));
        while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
        {
            out.resize(out.size() - 1U);
        }
        CHECK(view(out) == view(expected.json));
    }

    // The grant is the binary's start-up flag: record is refused without it and reaches the command with it.
    {
        (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command capture.start > ceridc_diag_out.json", exe);
        CHECK(std::system(cmd) != 0); // ok:false
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_out.json")), out));
        CHECK(has(out, "\"status\":\"unauthorized\""));

        DiagCommandService native(crd::perf::authority_bit(DiagAuthority::Record), DiagServiceConfig{}, &g_alloc);
        DiagRequest        r;
        r.command                            = "capture.start";
        const crd::perf::DiagResult expected = native.execute(r);
        (void)std::snprintf(cmd, sizeof(cmd),
                            "\"%s\" diag --command capture.start --grant record > ceridc_diag_out.json", exe);
        (void)std::system(cmd);
        out.clear();
        REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_out.json")), out));
        CHECK(has(out, "\"grant\":\"record\""));
        CHECK_FALSE(has(out, "unauthorized"));
        CHECK(has(out, (String("\"status\":\"", &g_alloc) += crd::perf::status_name(expected.status)).c_str()));

        (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command diag.commands --grant root > ceridc_diag_out.json",
                            exe);
        CHECK(std::system(cmd) != 0);
        out.clear();
        REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_out.json")), out));
        CHECK(has(out, "\"ok\":false"));
    }

    // MCP over stdio: the tool's text is the native document, and smuggled authority is still refused.
    {
        DiagCommandService native(read, rooted(), &g_alloc);
        bind(native);
        DiagRequest r;
        r.command                            = "bundle.inspect";
        r.path                               = kBinaryBundle;
        r.page_items                         = 2U;
        const crd::perf::DiagResult expected = native.execute(r);
        r.command                            = "program.provenance";
        r.path                               = kBinaryProgram;
        r.page_items                         = 4U;
        const crd::perf::DiagResult program  = native.execute(r);
        REQUIRE(program.status == crd::perf::DiagStatus::Ok);
        r.command                            = "replay.prepare";
        const crd::perf::DiagResult replay   = native.execute(r);
        REQUIRE(replay.status == crd::perf::DiagStatus::Ok);
        String script(&g_alloc);
        script.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
        script.append(R"({"command":"bundle.inspect","path":"ceridc_diag_bundle_binary.cdb","page_items":2}}})");
        script.push_back('\n');
        script.append(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"diag","arguments":)");
        script.append(R"({"command":"capture.stop","path":"x.cprof","grant":"record"}}})");
        script.push_back('\n');
        script.append(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"diag","arguments":)");
        script.append(R"({"command":"program.provenance","path":"ceridc_diag_program_binary.ceir","page_items":4}}})");
        script.push_back('\n');
        script.append(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"diag","arguments":)");
        script.append(R"({"command":"replay.prepare","path":"ceridc_diag_program_binary.ceir","page_items":4}}})");
        script.push_back('\n');
        REQUIRE(fs::write_file_text(fs::Path(StringView("ceridc_diag_in.jsonl")), view(script)));
        (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp --diag-root . < ceridc_diag_in.jsonl > ceridc_diag_out.jsonl",
                            exe);
        REQUIRE(std::system(cmd) == 0);
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_out.jsonl")), out));
        const StringView all = view(out);
        const crd::usize eol = all.find('\n');
        REQUIRE(eol != StringView::npos);
        const ToolReply first = reply_of(all.substr(0U, eol));
        REQUIRE(first.parsed);
        CHECK_FALSE(first.is_error);
        CHECK(view(first.text) == view(expected.json));
        const crd::usize eol2 = all.find('\n', eol + 1U);
        REQUIRE(eol2 != StringView::npos);
        const ToolReply second = reply_of(all.substr(eol + 1U, eol2 - eol - 1U));
        REQUIRE(second.parsed);
        CHECK(second.is_error);
        CHECK(has(second.text, "\"status\":\"unauthorized\""));
        CHECK_FALSE(fs::exists(fs::Path(StringView("x.cprof"))));
        const crd::usize eol3 = all.find('\n', eol2 + 1U);
        REQUIRE(eol3 != StringView::npos);
        const ToolReply third = reply_of(all.substr(eol2 + 1U, eol3 - eol2 - 1U));
        REQUIRE(third.parsed);
        CHECK_FALSE(third.is_error);
        CHECK(view(third.text) == view(program.json));
        const ToolReply fourth = reply_of(all.substr(eol3 + 1U));
        REQUIRE(fourth.parsed);
        CHECK_FALSE(fourth.is_error);
        CHECK(view(fourth.text) == view(replay.json));
        (void)fs::remove_file(fs::Path(StringView("ceridc_diag_in.jsonl")));
        (void)fs::remove_file(fs::Path(StringView("ceridc_diag_out.jsonl")));
    }
    (void)fs::remove_file(fs::Path(StringView("ceridc_diag_out.json")));
    (void)fs::remove_file(fs::Path(StringView(kBinaryBundle)));
    (void)fs::remove_file(fs::Path(StringView(kBinaryProgram)));
}

TEST_CASE("diag: the real ceridc binary runs program.inspect only under its execute grant", "[ceridc][diag]")
{
    write_program(kInspectBinary);
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    const crd::u32 call_line = line_of("func.call", 0U);
    const crd::u32 a_line    = line_of("arith.addi", 0U);
    char           breaks[16];
    char           watches[32];
    (void)std::snprintf(breaks, sizeof(breaks), "%u", call_line);
    (void)std::snprintf(watches, sizeof(watches), "%u,%u", a_line, call_line);

    const crd::perf::DiagAuthoritySet execute =
        crd::perf::authority_bit(DiagAuthority::Read) | crd::perf::authority_bit(DiagAuthority::Execute);
    DiagCommandService native(execute, rooted(), &g_alloc);
    bind(native);
    const crd::perf::DiagArg script[] = {
        {"args", "3"}, {"breaks", breaks}, {"watches", watches}, {"steps", "into,out"}};
    DiagRequest r;
    r.command                            = "program.inspect";
    r.path                               = kInspectBinary;
    r.args                               = {script, 4U};
    r.page_items                         = crd::perf::kDiagMaxPageItems;
    const crd::perf::DiagResult expected = native.execute(r);
    INFO(expected.json.c_str());
    REQUIRE(expected.status == crd::perf::DiagStatus::Ok);
    CHECK(has(expected.json, "\"outcome\":\"finished\""));
    CHECK(has(expected.json, "{\"kind\":\"result\",\"index\":0,\"value\":36}"));

    // Command line: --param carries each named argument; the grant is the process's flag.
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command program.inspect --path %s --param args=3 --param breaks=%s "
                        "--param watches=%s --param steps=into,out --page-items %u --grant read,execute --root . "
                        "> ceridc_diag_inspect_out.json",
                        exe, kInspectBinary, breaks, watches, crd::perf::kDiagMaxPageItems);
    REQUIRE(std::system(cmd) == 0);
    String out(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_inspect_out.json")), out));
    while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
    {
        out.resize(out.size() - 1U);
    }
    CHECK(view(out) == view(expected.json));

    // The default grant (read) refuses it before the program is read.
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command program.inspect --path %s --param args=3 --root . "
                        "> ceridc_diag_inspect_out.json",
                        exe, kInspectBinary);
    CHECK(std::system(cmd) != 0);
    out.clear();
    REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_inspect_out.json")), out));
    CHECK(has(out, "\"status\":\"unauthorized\""));

    // MCP stdio under an execute grant: the tool text is the native document.
    String line(&g_alloc);
    line.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
    line.append(arguments_of(r).c_str());
    line.append("}}\n");
    REQUIRE(fs::write_file_text(fs::Path(StringView("ceridc_diag_inspect_in.jsonl")), view(line)));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" mcp --diag-grant read,execute --diag-root . < ceridc_diag_inspect_in.jsonl "
                        "> ceridc_diag_inspect_out.jsonl",
                        exe);
    REQUIRE(std::system(cmd) == 0);
    out.clear();
    REQUIRE(fs::read_file_text(fs::Path(StringView("ceridc_diag_inspect_out.jsonl")), out));
    const StringView all = view(out);
    const ToolReply  got = reply_of(all.substr(0U, all.find('\n')));
    REQUIRE(got.parsed);
    CHECK_FALSE(got.is_error);
    CHECK(view(got.text) == view(expected.json));

    (void)fs::remove_file(fs::Path(StringView("ceridc_diag_inspect_in.jsonl")));
    (void)fs::remove_file(fs::Path(StringView("ceridc_diag_inspect_out.jsonl")));
    (void)fs::remove_file(fs::Path(StringView("ceridc_diag_inspect_out.json")));
    (void)fs::remove_file(fs::Path(StringView(kInspectBinary)));
}

TEST_CASE("diag: a run record made by one ceridc process reproduces in another after the program is edited",
          "[ceridc][diag]")
{
    // DIAG.9a: the record is written by one process, the program file is edited, and a second process replays the
    // record from the artifact it holds; a third replays the same inputs against the edited file and names the edited
    // constant. Each answer equals this process's native call on a fresh service.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    (void)fs::remove_file(fs::Path(StringView(kReplayRecord)));

    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kReplayDemo)), text));
    const StringView pristine{text.data(), text.size()};
    REQUIRE(fs::write_file_text(fs::Path(StringView(kReplayProgram)), pristine));
    const crd::usize bias_at = pristine.find(StringView{kBiasLine});
    REQUIRE(bias_at != StringView::npos);
    crd::u32 bias_line = 1U;
    for (crd::usize i = 0U; i < bias_at; ++i)
    {
        bias_line += pristine[i] == '\n' ? 1U : 0U;
    }

    const auto read_out = [&]()
    {
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kReplayOut)), out));
        while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
        {
            out.resize(out.size() - 1U);
        }
        return out;
    };
    const crd::perf::DiagAuthoritySet execute = crd::perf::authority_bit(DiagAuthority::Execute);
    const auto                        native  = [&](const char* program)
    {
        DiagCommandService svc(execute, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg against[] = {{"program", program != nullptr ? program : ""}};
        DiagRequest              r;
        r.command = "replay.run";
        r.path    = kReplayRecord;
        r.args    = {against, program != nullptr ? 1U : 0U};
        return svc.execute(r);
    };

    // Process 1 records main(1), which selects a switch region that does not exist.
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=1 "
                        "--grant execute,record --root . > %s",
                        exe, kReplayProgram, kReplayRecord, kReplayOut);
    REQUIRE(std::system(cmd) == 0);
    const String recorded = read_out();
    INFO(recorded.c_str());
    CHECK(has(recorded, "\"error\":\"selector-out-of-range\""));
    CHECK(has(recorded, "\"replay\":\"replayable\""));

    // The program file is edited after the run.
    String edit(&g_alloc);
    edit.append(pristine.substr(0U, bias_at));
    edit.append("%9 = arith.const() {value = -1} : !i32");
    edit.append(pristine.substr(bias_at + StringView{kBiasLine}.size()));
    REQUIRE(fs::write_file_text(fs::Path(StringView(kReplayProgram)), StringView{edit.data(), edit.size()}));

    // Process 2 replays the record from its own artifact: the failure reproduces.
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kReplayRecord, kReplayOut);
    REQUIRE(std::system(cmd) == 0);
    const String same = read_out();
    INFO(same.c_str());
    CHECK(has(same, "\"result\":\"reproduced\""));
    CHECK(has(same, "\"run\":\"replayed\",\"error\":\"selector-out-of-range\""));
    const crd::perf::DiagResult same_native = native(nullptr);
    CHECK(view(same) == view(same_native.json));

    // Process 3 replays the same inputs against the edited file: the edited constant is the first divergence.
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kReplayRecord, kReplayProgram, kReplayOut);
    REQUIRE(std::system(cmd) == 0);
    const String diff = read_out();
    INFO(diff.c_str());
    CHECK(has(diff, "\"result\":\"diverged\""));
    char at_line[64];
    (void)std::snprintf(at_line, sizeof(at_line), R"("file":"%s","line":%u,)", kReplayProgram, bias_line);
    CHECK(has(diff, "\"divergence\":\"value\",\"index\":"));
    CHECK(has(diff, at_line));
    const crd::perf::DiagResult diff_native = native(kReplayProgram);
    CHECK(view(diff) == view(diff_native.json));

    // Over MCP stdio, under the process's execute grant, the tool text is the native document.
    DiagRequest r;
    r.command = "replay.run";
    r.path    = kReplayRecord;
    String line(&g_alloc);
    line.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
    line.append(arguments_of(r).c_str());
    line.append("}}\n");
    REQUIRE(fs::write_file_text(fs::Path(StringView("ceridc_diag_replay_in.jsonl")), view(line)));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" mcp --diag-grant execute --diag-root . < ceridc_diag_replay_in.jsonl > %s", exe,
                        kReplayOut);
    REQUIRE(std::system(cmd) == 0);
    const String     mcp = read_out();
    const StringView all = view(mcp);
    const ToolReply  got = reply_of(all.substr(0U, all.find('\n')));
    REQUIRE(got.parsed);
    CHECK_FALSE(got.is_error);
    CHECK(view(got.text) == view(same_native.json));

    // Without Record the binary refuses to write a record, before reading the program.
    (void)fs::remove_file(fs::Path(StringView(kReplayRecord)));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=1 "
                        "--grant execute --root . > %s",
                        exe, kReplayProgram, kReplayRecord, kReplayOut);
    CHECK(std::system(cmd) != 0);
    CHECK(has(read_out(), "the command needs record authority"));
    CHECK_FALSE(fs::exists(fs::Path(StringView(kReplayRecord))));

    (void)fs::remove_file(fs::Path(StringView("ceridc_diag_replay_in.jsonl")));
    (void)fs::remove_file(fs::Path(StringView(kReplayOut)));
    (void)fs::remove_file(fs::Path(StringView(kReplayProgram)));
}

TEST_CASE("diag: a seeded random failure recorded by one ceridc process reproduces in another from its draws",
          "[ceridc][diag]")
{
    // DIAG.9a host inputs: process 1 records main(4) of the committed random demo with a seed that fails (chosen here
    // from SeededInputs::draw); the record is the native record, byte for byte. The program file is edited so the draw
    // reads stream 2. Process 2 replays the record with no seed and reproduces the failure from the draws it holds;
    // process 3 replays the same draws against the edited file and names the read the record cannot answer. Each
    // answer equals this process's native call, and so does an MCP stdio process's tool text.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    (void)fs::remove_file(fs::Path(StringView(kRandomRecord)));
    (void)fs::remove_file(fs::Path(StringView(kRandomNative)));

    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kRandomDemo)), text));
    const StringView pristine{text.data(), text.size()};
    REQUIRE(fs::write_file_text(fs::Path(StringView(kRandomProgram)), pristine));
    const crd::usize draw_at = pristine.find(StringView{kDrawLine});
    REQUIRE(draw_at != StringView::npos);
    crd::u32 draw_line = 1U;
    for (crd::usize i = 0U; i < draw_at; ++i)
    {
        draw_line += pristine[i] == '\n' ? 1U : 0U;
    }

    // The first seed whose four stream-0 draws include one that reduces to 3 (the switch has three cases).
    crd::u64 seed    = 0U;
    crd::u32 failing = 0U;
    for (crd::u64 s = 1U; s < 10000U && seed == 0U; ++s)
    {
        for (crd::u32 i = 0U; i < 4U && seed == 0U; ++i)
        {
            if (crd::ceir::input::reduce_draw(crd::ceir::input::SeededInputs::draw(s, 0U, i), 4U) == 3)
            {
                seed    = s;
                failing = i;
            }
        }
    }
    REQUIRE(seed != 0U);
    char seed_text[32];
    (void)std::snprintf(seed_text, sizeof(seed_text), "%llu", static_cast<unsigned long long>(seed));

    const auto read_out = [&]()
    {
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kRandomOut)), out));
        while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
        {
            out.resize(out.size() - 1U);
        }
        return out;
    };
    const crd::perf::DiagAuthoritySet execute = crd::perf::authority_bit(DiagAuthority::Execute);
    const auto                        native  = [&](const char* program)
    {
        DiagCommandService svc(execute, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg against[] = {{"program", program != nullptr ? program : ""}};
        DiagRequest              r;
        r.command = "replay.run";
        r.path    = kRandomRecord;
        r.args    = {against, program != nullptr ? 1U : 0U};
        return svc.execute(r);
    };

    // Process 1 records the seeded failure.
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=4 --param seed=%s "
                        "--grant execute,record --root . > %s",
                        exe, kRandomProgram, kRandomRecord, seed_text, kRandomOut);
    REQUIRE(std::system(cmd) == 0);
    const String recorded = read_out();
    INFO(recorded.c_str());
    CHECK(has(recorded, "\"error\":\"selector-out-of-range\""));
    CHECK(has(recorded, "\"random_source\":\"seeded\""));
    char reads[48];
    (void)std::snprintf(reads, sizeof(reads), "\"input_reads\":%u,", failing + 1U);
    CHECK(has(recorded, reads));
    CHECK(has(recorded, "\"replay\":\"replayable\""));

    // This process's native record of the same run is the same file.
    {
        const crd::perf::DiagAuthoritySet both = execute | crd::perf::authority_bit(DiagAuthority::Record);
        DiagCommandService svc(both, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg args[] = {{"out", kRandomNative}, {"args", "4"}, {"seed", seed_text}};
        DiagRequest              r;
        r.command = "replay.record";
        r.path    = kRandomProgram;
        r.args    = {args, 3U};
        REQUIRE(svc.execute(r).status == crd::perf::DiagStatus::Ok);
        String a(&g_alloc);
        String b(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kRandomRecord)), a));
        REQUIRE(fs::read_file_text(fs::Path(StringView(kRandomNative)), b));
        CHECK(view(a) == view(b));
    }

    // The program file is edited after the run: the draw now reads stream 2.
    String edit(&g_alloc);
    edit.append(pristine.substr(0U, draw_at));
    edit.append("%4 = input.random() {stream = 2, bound = 4} : !i32");
    edit.append(pristine.substr(draw_at + StringView{kDrawLine}.size()));
    REQUIRE(fs::write_file_text(fs::Path(StringView(kRandomProgram)), StringView{edit.data(), edit.size()}));

    // Process 2 replays the record, with no seed: the failure reproduces from the recorded draws.
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kRandomRecord, kRandomOut);
    REQUIRE(std::system(cmd) == 0);
    const String same = read_out();
    INFO(same.c_str());
    CHECK(has(same, "\"result\":\"reproduced\""));
    CHECK(has(same, "\"run\":\"replayed\",\"error\":\"selector-out-of-range\""));
    const crd::perf::DiagResult same_native = native(nullptr);
    CHECK(view(same) == view(same_native.json));

    // Process 3 replays the same draws against the edited file: its first read asks for another stream.
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kRandomRecord, kRandomProgram, kRandomOut);
    REQUIRE(std::system(cmd) == 0);
    const String diff = read_out();
    INFO(diff.c_str());
    CHECK(has(diff, "\"result\":\"diverged\""));
    CHECK(has(diff, "\"divergence\":\"input\",\"index\":0,"));
    CHECK(has(diff, "\"recorded\":0,\"observed\":2,"));
    char at_line[96];
    (void)std::snprintf(at_line, sizeof(at_line), R"("file":"%s","line":%u,)", kRandomProgram, draw_line);
    CHECK(has(diff, at_line));
    const crd::perf::DiagResult diff_native = native(kRandomProgram);
    CHECK(view(diff) == view(diff_native.json));

    // Over MCP stdio, under the process's execute grant, the tool text is the native document.
    DiagRequest r;
    r.command = "replay.run";
    r.path    = kRandomRecord;
    String line(&g_alloc);
    line.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
    line.append(arguments_of(r).c_str());
    line.append("}}\n");
    REQUIRE(fs::write_file_text(fs::Path(StringView(kRandomIn)), view(line)));
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp --diag-grant execute --diag-root . < %s > %s", exe, kRandomIn,
                        kRandomOut);
    REQUIRE(std::system(cmd) == 0);
    const String     mcp = read_out();
    const StringView all = view(mcp);
    const ToolReply  got = reply_of(all.substr(0U, all.find('\n')));
    REQUIRE(got.parsed);
    CHECK_FALSE(got.is_error);
    CHECK(view(got.text) == view(same_native.json));

    (void)fs::remove_file(fs::Path(StringView(kRandomIn)));
    (void)fs::remove_file(fs::Path(StringView(kRandomOut)));
    (void)fs::remove_file(fs::Path(StringView(kRandomRecord)));
    (void)fs::remove_file(fs::Path(StringView(kRandomNative)));
    (void)fs::remove_file(fs::Path(StringView(kRandomProgram)));
}

TEST_CASE("diag: a host record made by one ceridc process reproduces in another after the program is edited",
          "[ceridc][diag]")
{
    // DIAG.9a: process 1 records main(0) of the committed host demo on the host provider (it fails bad-for-step);
    // the program file is edited (the launched constant 5 becomes 6); process 2 replays the record from the artifact
    // it holds and the failure reproduces; process 3 replays the same inputs against the edited file and names the
    // await that reads the launched value (recorded 25, observed 36); an MCP stdio process replays it on another job
    // split. Each answer equals this process's native call, and the binary's record is the native record, byte for
    // byte.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    (void)fs::remove_file(fs::Path(StringView(kHostRecord)));
    (void)fs::remove_file(fs::Path(StringView(kHostNative)));

    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kHostDemo)), text));
    const StringView pristine{text.data(), text.size()};
    REQUIRE(fs::write_file_text(fs::Path(StringView(kHostProgram)), pristine));
    const crd::usize launch_at = pristine.find(StringView{kLaunchLine});
    const crd::usize await_at  = pristine.find(StringView{kAwaitLine});
    REQUIRE(launch_at != StringView::npos);
    REQUIRE(await_at != StringView::npos);
    crd::u32 await_line = 1U;
    for (crd::usize i = 0U; i < await_at; ++i)
    {
        await_line += pristine[i] == '\n' ? 1U : 0U;
    }

    const auto read_out = [&]()
    {
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kHostOut)), out));
        while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
        {
            out.resize(out.size() - 1U);
        }
        return out;
    };
    const crd::perf::DiagAuthoritySet execute = crd::perf::authority_bit(DiagAuthority::Execute);
    const auto                        native  = [&](const char* program, const char* jobs)
    {
        DiagCommandService svc(execute, rooted(), &g_alloc);
        bind(svc);
        crd::perf::DiagArg args[2];
        crd::u32           n = 0U;
        if (program != nullptr)
        {
            args[n++] = {"program", program};
        }
        if (jobs != nullptr)
        {
            args[n++] = {"jobs", jobs};
        }
        DiagRequest r;
        r.command = "replay.run";
        r.path    = kHostRecord;
        r.args    = {args, n};
        return svc.execute(r);
    };

    // Process 1 records main(0) on the host provider with a job split of 3.
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=0 "
                        "--param executor=host --param jobs=3 --grant execute,record --root . > %s",
                        exe, kHostProgram, kHostRecord, kHostOut);
    REQUIRE(std::system(cmd) == 0);
    const String recorded = read_out();
    INFO(recorded.c_str());
    CHECK(has(recorded, "\"executor\":\"host\",\"jobs\":3,"));
    CHECK(has(recorded, "\"error\":\"bad-for-step\""));
    CHECK(has(recorded, "\"replay\":\"replayable\""));

    // This process records the same run natively: the two records are the same bytes.
    {
        const crd::perf::DiagAuthoritySet grant = execute | crd::perf::authority_bit(DiagAuthority::Record);
        DiagCommandService                svc(grant, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg args[] = {{"out", kHostNative}, {"args", "0"}, {"executor", "host"}, {"jobs", "3"}};
        DiagRequest              r;
        r.command = "replay.record";
        r.path    = kHostProgram;
        r.args    = {args, 4U};
        REQUIRE(svc.execute(r).status == crd::perf::DiagStatus::Ok);
        crd::containers::Array<crd::u8> theirs(&g_alloc);
        crd::containers::Array<crd::u8> mine(&g_alloc);
        REQUIRE(fs::read_file_binary(fs::Path(StringView(kHostRecord)), theirs));
        REQUIRE(fs::read_file_binary(fs::Path(StringView(kHostNative)), mine));
        CHECK(theirs == mine);
    }

    // The program file is edited after the run.
    String edit(&g_alloc);
    edit.append(pristine.substr(0U, launch_at));
    edit.append("%4 = arith.const() {value = 6} : !i32");
    edit.append(pristine.substr(launch_at + StringView{kLaunchLine}.size()));
    REQUIRE(fs::write_file_text(fs::Path(StringView(kHostProgram)), StringView{edit.data(), edit.size()}));

    // Process 2 replays the record from its own artifact: the failure reproduces.
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kHostRecord, kHostOut);
    REQUIRE(std::system(cmd) == 0);
    const String same = read_out();
    INFO(same.c_str());
    CHECK(has(same, "\"result\":\"reproduced\""));
    CHECK(has(same, "\"run\":\"replayed\",\"error\":\"bad-for-step\""));
    CHECK(view(same) == view(native(nullptr, nullptr).json));

    // Process 3 replays the same inputs against the edited file: the await reading the launched value diverges.
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kHostRecord, kHostProgram, kHostOut);
    REQUIRE(std::system(cmd) == 0);
    const String diff = read_out();
    INFO(diff.c_str());
    CHECK(has(diff, "\"result\":\"diverged\""));
    CHECK(has(diff, "\"divergence\":\"value\",\"index\":"));
    CHECK(has(diff, "\"recorded\":25,\"observed\":36,"));
    char at_line[96];
    (void)std::snprintf(at_line, sizeof(at_line), R"("file":"%s","line":%u,)", kHostProgram, await_line);
    CHECK(has(diff, at_line));
    CHECK(view(diff) == view(native(kHostProgram, nullptr).json));

    // Over MCP stdio, under the process's execute grant, on another job split: the tool text is the native document.
    DiagRequest              r;
    const crd::perf::DiagArg jobs[] = {{"jobs", "16"}};
    r.command                       = "replay.run";
    r.path                          = kHostRecord;
    r.args                          = {jobs, 1U};
    String line(&g_alloc);
    line.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
    line.append(arguments_of(r).c_str());
    line.append("}}\n");
    REQUIRE(fs::write_file_text(fs::Path(StringView(kHostIn)), view(line)));
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp --diag-grant execute --diag-root . < %s > %s", exe, kHostIn,
                        kHostOut);
    REQUIRE(std::system(cmd) == 0);
    const String     mcp = read_out();
    const StringView all = view(mcp);
    const ToolReply  got = reply_of(all.substr(0U, all.find('\n')));
    REQUIRE(got.parsed);
    CHECK_FALSE(got.is_error);
    const crd::perf::DiagResult split = native(nullptr, "16");
    CHECK(has(split.json, "\"recorded_jobs\":3,\"jobs\":16,"));
    CHECK(has(split.json, "\"result\":\"reproduced\""));
    CHECK(view(got.text) == view(split.json));

    (void)fs::remove_file(fs::Path(StringView(kHostIn)));
    (void)fs::remove_file(fs::Path(StringView(kHostOut)));
    (void)fs::remove_file(fs::Path(StringView(kHostRecord)));
    (void)fs::remove_file(fs::Path(StringView(kHostNative)));
    (void)fs::remove_file(fs::Path(StringView(kHostProgram)));
}

TEST_CASE("diag: a time-step failure recorded by one ceridc process reproduces in another from its time reads",
          "[ceridc][diag]")
{
    // DIAG.9a clock inputs: process 1 records main(7) of the committed clock demo with the sim domain's step over the
    // program's budget (and a live wall clock it never reaches); the record is this process's native record, byte for
    // byte. The program file is edited so the step read asks for the frame domain. Process 2 replays the record with no
    // clock and reproduces the failure from the read it holds; process 3 replays it against the edited file and names
    // the read the record cannot answer. Process 4 records a steady step, reading the live wall; process 5 reproduces
    // that run, wall-dependent result included, from the reads alone. Each replay answer equals this process's native
    // call, and so does an MCP stdio process's tool text.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    (void)fs::remove_file(fs::Path(StringView(kClockRecord)));
    (void)fs::remove_file(fs::Path(StringView(kClockNative)));
    (void)fs::remove_file(fs::Path(StringView(kClockPass)));

    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kClockDemo)), text));
    const StringView pristine{text.data(), text.size()};
    REQUIRE(fs::write_file_text(fs::Path(StringView(kClockProgram)), pristine));
    const crd::usize step_at = pristine.find(StringView{kStepLine});
    REQUIRE(step_at != StringView::npos);
    crd::u32 step_line = 1U;
    for (crd::usize i = 0U; i < step_at; ++i)
    {
        step_line += pristine[i] == '\n' ? 1U : 0U;
    }

    const auto read_out = [&]()
    {
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kClockOut)), out));
        while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
        {
            out.resize(out.size() - 1U);
        }
        return out;
    };
    const crd::perf::DiagAuthoritySet execute = crd::perf::authority_bit(DiagAuthority::Execute);
    const auto                        native  = [&](const char* record, const char* program)
    {
        DiagCommandService svc(execute, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg against[] = {{"program", program != nullptr ? program : ""}};
        DiagRequest              r;
        r.command = "replay.run";
        r.path    = record;
        r.args    = {against, program != nullptr ? 1U : 0U};
        return svc.execute(r);
    };

    // Process 1 records the over-budget step.
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=7 "
                        "--param sim_step=50000000 --param sim_time=4000000000 --param clock=wall "
                        "--grant execute,record --root . > %s",
                        exe, kClockProgram, kClockRecord, kClockOut);
    REQUIRE(std::system(cmd) == 0);
    const String recorded = read_out();
    INFO(recorded.c_str());
    CHECK(has(recorded, "\"error\":\"selector-out-of-range\""));
    CHECK(has(recorded, "\"wall_clock\":\"live\""));
    CHECK(has(recorded, "\"sim_step\":\"set\",\"sim_step_ns\":50000000,"));
    CHECK(has(recorded, "\"input_reads\":1,"));
    CHECK(has(recorded, R"("input":"clock","guarantee":"event","needed":"yes","state":"recorded")"));
    CHECK(has(recorded, "\"replay\":\"replayable\""));

    // This process's native record of the same run is the same file.
    {
        const crd::perf::DiagAuthoritySet both = execute | crd::perf::authority_bit(DiagAuthority::Record);
        DiagCommandService                svc(both, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg args[] = {{"out", kClockNative},
                                           {"args", "7"},
                                           {"sim_step", "50000000"},
                                           {"sim_time", "4000000000"},
                                           {"clock", "wall"}};
        DiagRequest              r;
        r.command = "replay.record";
        r.path    = kClockProgram;
        r.args    = {args, 5U};
        REQUIRE(svc.execute(r).status == crd::perf::DiagStatus::Ok);
        String a(&g_alloc);
        String b(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kClockRecord)), a));
        REQUIRE(fs::read_file_text(fs::Path(StringView(kClockNative)), b));
        CHECK(view(a) == view(b));
    }

    // The program file is edited after the run: the step read now asks for the frame domain.
    String edit(&g_alloc);
    edit.append(pristine.substr(0U, step_at));
    edit.append("%1 = input.time_step() {domain = \"frame\"} : !i64");
    edit.append(pristine.substr(step_at + StringView{kStepLine}.size()));
    REQUIRE(fs::write_file_text(fs::Path(StringView(kClockProgram)), StringView{edit.data(), edit.size()}));

    // Process 2 replays the record, with no clock: the failure reproduces from the recorded step.
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kClockRecord, kClockOut);
    REQUIRE(std::system(cmd) == 0);
    const String same = read_out();
    INFO(same.c_str());
    CHECK(has(same, "\"result\":\"reproduced\""));
    CHECK(has(same, "\"run\":\"replayed\",\"error\":\"selector-out-of-range\""));
    const crd::perf::DiagResult same_native = native(kClockRecord, nullptr);
    CHECK(view(same) == view(same_native.json));

    // Process 3 replays the same reads against the edited file: its first read asks for another domain.
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kClockRecord, kClockProgram, kClockOut);
    REQUIRE(std::system(cmd) == 0);
    const String diff = read_out();
    INFO(diff.c_str());
    CHECK(has(diff, "\"result\":\"diverged\""));
    CHECK(has(diff, "\"divergence\":\"input\",\"index\":0,"));
    CHECK(has(diff, "\"recorded\":1,\"observed\":2,"));
    CHECK(has(diff, "\"recorded_input\":\"time_step\",\"observed_input\":\"time_step\""));
    char at_line[96];
    (void)std::snprintf(at_line, sizeof(at_line), R"("file":"%s","line":%u,)", kClockProgram, step_line);
    CHECK(has(diff, at_line));
    const crd::perf::DiagResult diff_native = native(kClockRecord, kClockProgram);
    CHECK(view(diff) == view(diff_native.json));

    // Process 4 records a steady step from the pristine file: the run reads the live wall and returns a value from it.
    REQUIRE(fs::write_file_text(fs::Path(StringView(kClockProgram)), pristine));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=7 "
                        "--param sim_step=16666667 --param sim_time=4000000000 --param clock=wall "
                        "--grant execute,record --root . > %s",
                        exe, kClockProgram, kClockPass, kClockOut);
    REQUIRE(std::system(cmd) == 0);
    const String passed = read_out();
    INFO(passed.c_str());
    CHECK(has(passed, "\"error\":\"none\""));
    CHECK(has(passed, "\"input_reads\":3,"));
    CHECK(has(passed, "{\"kind\":\"result\",\"index\":0,\"value\":"));

    // Process 5, with no clock, reproduces it: the wall reading it returns is the one process 4 read.
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kClockPass, kClockOut);
    REQUIRE(std::system(cmd) == 0);
    const String again = read_out();
    INFO(again.c_str());
    CHECK(has(again, "\"result\":\"reproduced\""));
    CHECK(has(again, "\"recorded_input_reads\":3,"));
    CHECK(view(again) == view(native(kClockPass, nullptr).json));

    // Over MCP stdio, under the process's execute grant, the tool text is the native document.
    DiagRequest r;
    r.command = "replay.run";
    r.path    = kClockRecord;
    String line(&g_alloc);
    line.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
    line.append(arguments_of(r).c_str());
    line.append("}}\n");
    REQUIRE(fs::write_file_text(fs::Path(StringView(kClockIn)), view(line)));
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp --diag-grant execute --diag-root . < %s > %s", exe, kClockIn,
                        kClockOut);
    REQUIRE(std::system(cmd) == 0);
    const String     mcp = read_out();
    const StringView all = view(mcp);
    const ToolReply  got = reply_of(all.substr(0U, all.find('\n')));
    REQUIRE(got.parsed);
    CHECK_FALSE(got.is_error);
    CHECK(view(got.text) == view(same_native.json));

    (void)fs::remove_file(fs::Path(StringView(kClockIn)));
    (void)fs::remove_file(fs::Path(StringView(kClockOut)));
    (void)fs::remove_file(fs::Path(StringView(kClockRecord)));
    (void)fs::remove_file(fs::Path(StringView(kClockNative)));
    (void)fs::remove_file(fs::Path(StringView(kClockPass)));
    (void)fs::remove_file(fs::Path(StringView(kClockProgram)));
}

TEST_CASE("diag: an unhandled event recorded by one ceridc process reproduces in another from its event reads",
          "[ceridc][diag]")
{
    // DIAG.9a input events: process 1 records main(7) of the committed event demo given a key and then a resize the
    // program has no case for; the record is this process's native record, byte for byte. The program file is edited
    // so the second read takes from queue 1. Process 2 replays the record with no event queue and reproduces the
    // failure from the reads it holds; process 3 replays it against the edited file and names the read the record
    // cannot answer. Process 4 records the same run on the host executor and process 5 reproduces that host record.
    // Each replay answer equals this process's native call, and so does an MCP stdio process's tool text.
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake
    (void)fs::remove_file(fs::Path(StringView(kEventRecord)));
    (void)fs::remove_file(fs::Path(StringView(kEventNative)));
    (void)fs::remove_file(fs::Path(StringView(kEventHost)));

    String text(&g_alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView(kEventDemo)), text));
    const StringView pristine{text.data(), text.size()};
    REQUIRE(fs::write_file_text(fs::Path(StringView(kEventProgram)), pristine));
    const crd::usize second_at = pristine.find(StringView{kSecondEventLine});
    REQUIRE(second_at != StringView::npos);
    crd::u32 second_line = 1U;
    for (crd::usize i = 0U; i < second_at; ++i)
    {
        second_line += pristine[i] == '\n' ? 1U : 0U;
    }

    const auto read_out = [&]()
    {
        String out(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kEventOut)), out));
        while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
        {
            out.resize(out.size() - 1U);
        }
        return out;
    };
    const crd::perf::DiagAuthoritySet execute = crd::perf::authority_bit(DiagAuthority::Execute);
    const auto                        native  = [&](const char* record, const char* program)
    {
        DiagCommandService svc(execute, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg against[] = {{"program", program != nullptr ? program : ""}};
        DiagRequest              r;
        r.command = "replay.run";
        r.path    = record;
        r.args    = {against, program != nullptr ? 1U : 0U};
        return svc.execute(r);
    };

    // Process 1 records the key and the unhandled resize.
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=7 "
                        "--param events=key_down:65:3,resize:1280:720 --grant execute,record --root . > %s",
                        exe, kEventProgram, kEventRecord, kEventOut);
    REQUIRE(std::system(cmd) == 0);
    const String recorded = read_out();
    INFO(recorded.c_str());
    CHECK(has(recorded, "\"error\":\"selector-out-of-range\""));
    CHECK(has(recorded, "\"event_queue\":\"open\",\"input_events\":2,\"input_reads\":2,"));
    CHECK(has(recorded, R"("input":"host-state","guarantee":"event","needed":"yes","state":"recorded")"));
    CHECK(has(recorded, "\"replay\":\"replayable\""));

    // This process's native record of the same run is the same file.
    {
        const crd::perf::DiagAuthoritySet both = execute | crd::perf::authority_bit(DiagAuthority::Record);
        DiagCommandService                svc(both, rooted(), &g_alloc);
        bind(svc);
        const crd::perf::DiagArg args[] = {
            {"out", kEventNative}, {"args", "7"}, {"events", "key_down:65:3,resize:1280:720"}};
        DiagRequest r;
        r.command = "replay.record";
        r.path    = kEventProgram;
        r.args    = {args, 3U};
        REQUIRE(svc.execute(r).status == crd::perf::DiagStatus::Ok);
        String a(&g_alloc);
        String b(&g_alloc);
        REQUIRE(fs::read_file_text(fs::Path(StringView(kEventRecord)), a));
        REQUIRE(fs::read_file_text(fs::Path(StringView(kEventNative)), b));
        CHECK(view(a) == view(b));
    }

    // The program file is edited after the run: the second read now takes from queue 1.
    String edit(&g_alloc);
    edit.append(pristine.substr(0U, second_at));
    edit.append("%6, %7, %8, %9, %10 = input.event() {queue = 1} : !i64");
    edit.append(pristine.substr(second_at + StringView{kSecondEventLine}.size()));
    REQUIRE(fs::write_file_text(fs::Path(StringView(kEventProgram)), StringView{edit.data(), edit.size()}));

    // Process 2 replays the record with no event queue: the failure reproduces from the recorded events.
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kEventRecord, kEventOut);
    REQUIRE(std::system(cmd) == 0);
    const String same = read_out();
    INFO(same.c_str());
    CHECK(has(same, "\"result\":\"reproduced\""));
    CHECK(has(same, "\"run\":\"replayed\",\"error\":\"selector-out-of-range\""));
    const crd::perf::DiagResult same_native = native(kEventRecord, nullptr);
    CHECK(view(same) == view(same_native.json));

    // Process 3 replays the same reads against the edited file: its second read takes from another queue.
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.run --path %s --param program=%s --grant execute --root . > %s",
                        exe, kEventRecord, kEventProgram, kEventOut);
    REQUIRE(std::system(cmd) == 0);
    const String diff = read_out();
    INFO(diff.c_str());
    CHECK(has(diff, "\"result\":\"diverged\""));
    CHECK(has(diff, "\"divergence\":\"input\",\"index\":1,"));
    CHECK(has(diff, "\"recorded\":0,\"observed\":1,"));
    CHECK(has(diff, "\"recorded_input\":\"event\",\"observed_input\":\"event\""));
    char at_line[96];
    (void)std::snprintf(at_line, sizeof(at_line), R"("file":"%s","line":%u,)", kEventProgram, second_line);
    CHECK(has(diff, at_line));
    const crd::perf::DiagResult diff_native = native(kEventRecord, kEventProgram);
    CHECK(view(diff) == view(diff_native.json));

    // Process 4 records the same run from the pristine file on the host executor; process 5 reproduces it.
    REQUIRE(fs::write_file_text(fs::Path(StringView(kEventProgram)), pristine));
    (void)std::snprintf(cmd, sizeof(cmd),
                        "\"%s\" diag --command replay.record --path %s --param out=%s --param args=7 "
                        "--param events=key_down:65:3,resize:1280:720 --param executor=host "
                        "--grant execute,record --root . > %s",
                        exe, kEventProgram, kEventHost, kEventOut);
    REQUIRE(std::system(cmd) == 0);
    const String host = read_out();
    INFO(host.c_str());
    CHECK(has(host, "\"executor\":\"host\""));
    CHECK(has(host, "\"error\":\"selector-out-of-range\""));
    CHECK(has(host, "\"input_reads\":2,"));
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" diag --command replay.run --path %s --grant execute --root . > %s",
                        exe, kEventHost, kEventOut);
    REQUIRE(std::system(cmd) == 0);
    const String host_again = read_out();
    INFO(host_again.c_str());
    CHECK(has(host_again, "\"result\":\"reproduced\""));
    CHECK(view(host_again) == view(native(kEventHost, nullptr).json));

    // Over MCP stdio, under the process's execute grant, the tool text is the native document.
    DiagRequest r;
    r.command = "replay.run";
    r.path    = kEventRecord;
    String line(&g_alloc);
    line.append(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"diag","arguments":)");
    line.append(arguments_of(r).c_str());
    line.append("}}\n");
    REQUIRE(fs::write_file_text(fs::Path(StringView(kEventIn)), view(line)));
    (void)std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp --diag-grant execute --diag-root . < %s > %s", exe, kEventIn,
                        kEventOut);
    REQUIRE(std::system(cmd) == 0);
    const String     mcp = read_out();
    const StringView all = view(mcp);
    const ToolReply  got = reply_of(all.substr(0U, all.find('\n')));
    REQUIRE(got.parsed);
    CHECK_FALSE(got.is_error);
    CHECK(view(got.text) == view(same_native.json));

    (void)fs::remove_file(fs::Path(StringView(kEventIn)));
    (void)fs::remove_file(fs::Path(StringView(kEventOut)));
    (void)fs::remove_file(fs::Path(StringView(kEventRecord)));
    (void)fs::remove_file(fs::Path(StringView(kEventNative)));
    (void)fs::remove_file(fs::Path(StringView(kEventHost)));
    (void)fs::remove_file(fs::Path(StringView(kEventProgram)));
}
