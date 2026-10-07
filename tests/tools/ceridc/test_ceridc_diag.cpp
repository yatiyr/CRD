// test_ceridc_diag.cpp — the diag verb binds the host's typed diagnostic command service to the existing CLI and MCP
// transports. A native call, the in-process verb, the in-process MCP tool and the real ceridc binary (from the
// command line and over MCP stdio) return the same response bytes for the same service state, including pagination
// and refusals; the MCP tool's arguments cannot raise the grant the process started with; and malformed numeric
// arguments are protocol faults, while out-of-range counts reach the service and are refused there. MCP replies are
// parsed with the JSON reader and the tool text compared byte for byte. Every service here binds ceridc's own commands
// (bind_diag_commands), so the comparison covers program.provenance over the committed authored CEIR program and
// gpu.resources (no GPU context in ceridc, so its context and frame-graph evidence answer unavailable) too, and
// program.inspect: the authored program run to a script given as named arguments, which needs the Execute grant and
// reaches the agent transport only through this tool.

#include <crd/assetio/json.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

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
    // The commands, bundle, program and GPU snapshots, capabilities and the missing program; refusals ran nothing.
    CHECK(native.handler_runs() == 6U);
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
                         Cli{"program.provenance", kBinaryProgram, 4U}, Cli{"gpu.resources", nullptr, 0U}})
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
        const ToolReply third = reply_of(all.substr(eol2 + 1U));
        REQUIRE(third.parsed);
        CHECK_FALSE(third.is_error);
        CHECK(view(third.text) == view(program.json));
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
