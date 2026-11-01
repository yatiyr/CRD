// test_ceridc.cpp — GEO-11 (D-007 row 76): THE AGENT GATE. An agent, through the MCP protocol handler ALONE
// (server/discover → tools/list → tools/call), imports a model, cooks a pack, queries the resource graph, composes
// a scene (transactionally — the malformed edit rejects atomically, dry-run applies nothing), sequences a
// 2-shot timeline, renders it to EXR frames, and exports the interchange twin — every effect verified through
// files on disk and query verbs, zero GUI. A second case drives the REAL binary's stdio loop end to end, and the
// MCP-ADAPT.0 cases hold the protocol: revision 2026-07-28, the 2025-11-25 handshake and the argument checks.

#include <crd/assetio/otio.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/containers/string.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/core/build_config.hpp>
#include <crd/core/environment.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/hdr_image.hpp>

#include "mcp_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace fs = crd::platform::fs;

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc{crd::usize{64} << 20U, nullptr, "ceridc-tests"};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// One JSON-RPC message through the REAL server, as the stdio loop hands it a line.
crd::containers::String rpc(crd::ceridc::McpServer& server, crd::containers::StringView message)
{
    return server.handle({reinterpret_cast<const crd::u8*>(message.data()), message.size()});
}

// One revision 2026-07-28 request: `request` with the per-request _meta added to its params.
crd::containers::String call(crd::ceridc::McpServer& server, const char* request)
{
    const crd::containers::String message = crd::ceridc::test::modern(request, &g_alloc);
    return rpc(server, crd::containers::StringView{message.data(), message.size()});
}

[[nodiscard]] bool response_has(const crd::containers::String& response, const char* needle)
{
    // tool reports ride inside the MCP response as ESCAPED JSON text (\" for ") — strip the escapes so one
    // needle form asserts at both the protocol level and inside the report
    crd::containers::String flat(&g_alloc);
    for (crd::usize i = 0; i < response.size(); ++i)
    {
        if (response.c_str()[i] == '\\')
        {
            continue;
        }
        flat.push_back(response.c_str()[i]);
    }
    return std::strstr(flat.c_str(), needle) != nullptr;
}

void write_text(const char* path, const char* text)
{
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(path)),
                                crd::containers::StringView(text, std::strlen(text))));
}

void push_f32(crd::containers::Array<crd::u8>& b, crd::f32 v)
{
    crd::u8 raw[4];
    std::memcpy(raw, &v, 4);
    for (crd::u8 x : raw)
    {
        b.push_back(x);
    }
}

// a 1-triangle binary STL — the model the agent imports and cooks
void write_stl(const char* path)
{
    crd::containers::Array<crd::u8> stl(&g_alloc);
    for (int i = 0; i < 80; ++i)
    {
        stl.push_back(0);
    }
    const crd::u8 one[4] = {1, 0, 0, 0};
    for (crd::u8 x : one)
    {
        stl.push_back(x);
    }
    const crd::f32 tri[12] = {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (crd::f32 v : tri)
    {
        push_f32(stl, v);
    }
    stl.push_back(0);
    stl.push_back(0);
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(path)),
                                  crd::containers::as_const_span(stl)));
}

void ensure_handlers()
{
    static bool registered = false;
    if (!registered)
    {
        crd::cooker::register_builtin_handlers();
        registered = true;
    }
}

} // namespace

TEST_CASE("THE AGENT GATE: the full scenario through MCP alone", "[ceridc][agent]")
{
    ensure_handlers();

    // HYGIENE: the scenario asserts on file NON-existence at intermediate steps (e.g. a dry-run creates nothing),
    // so it must start from a clean slate — outputs left by a PRIOR run would fail those negative checks (and the
    // `create_directories` below). Remove every artifact this test can produce before running. (REN-1 close
    // 2026-07-24: the sweep caught this non-idempotency — CHECK_FALSE(is_file("ceridc_gate.scen")) tripped on a
    // stale file from the previous run.)
    for (const char* out : {"ceridc_gate.scen", "ceridc_gate_bad.scen", "ceridc_gate.pack.crdr", "ceridc_gate.otio",
                            "ceridc_gate_export.otio", "ceridc_gate.timl.crdr"})
    {
        (void)fs::remove_file(fs::Path(crd::containers::StringView(out)));
    }
    for (const char* dir : {"ceridc_gate_src", "ceridc_gate_frames"})
    {
        (void)fs::remove_all(fs::Path(crd::containers::StringView(dir)));
    }

    // ── discovery: no handshake (revision 2026-07-28); every request carries its version in params._meta ──────────
    crd::ceridc::McpServer server(&g_alloc);
    const auto discover = call(server, R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})");
    CHECK(response_has(discover, "\"supportedVersions\":[\"2026-07-28\",\"2025-11-25\"]"));
    CHECK(response_has(discover, "\"name\":\"ceridc\""));
    CHECK(rpc(server, R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})").empty());

    const auto tools = call(server, R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    for (const char* t : {"\"import\"", "\"cook\"", "\"query\"", "\"instantiate\"", "\"sequence\"",
                          "\"render\"", "\"export\""})
    {
        CHECK(response_has(tools, t));
    }

    // ── import: the agent inspects a model before cooking ─────────────────────────────────────────────────────
    REQUIRE(fs::create_directories(fs::Path(crd::containers::StringView("ceridc_gate_src"))));
    write_stl("ceridc_gate_src/tri.stl");
    write_text("ceridc_gate_src/tri.stl.meta",
               "[id]\nuuid = \"00112233445566770011223344556677\"\n");
    const auto import_report =
        call(server, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"import","arguments":{"path":"ceridc_gate_src/tri.stl"}}})");
    CHECK(response_has(import_report, "\"ok\":true"));
    CHECK(response_has(import_report, "\"meshes\":1"));

    // ── cook: the pack materializes ───────────────────────────────────────────────────────────────────────────
    const auto cook_report =
        call(server, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"cook","arguments":{"root":"ceridc_gate_src","out":"ceridc_gate.pack.crdr"}}})");
    CHECK(response_has(cook_report, "\"ok\":true"));
    REQUIRE(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate.pack.crdr"))));

    // ── query: the resource graph is visible ──────────────────────────────────────────────────────────────────
    const auto query_report =
        call(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"query","arguments":{"pack":"ceridc_gate.pack.crdr"}}})");
    CHECK(response_has(query_report, "\"ok\":true"));
    CHECK(response_has(query_report, "MESH"));
    CHECK(response_has(query_report, "tri"));

    // ── instantiate: TRANSACTIONAL scene composition ──────────────────────────────────────────────────────────
    // (a) the malformed edit rejects atomically — nothing lands on disk
    const auto bad =
        call(server, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"instantiate","arguments":{"pack":"ceridc_gate.pack.crdr","asset":"no-such-mesh","x":1,"out":"ceridc_gate_bad.scen"}}})");
    CHECK(response_has(bad, "\"isError\":true"));
    CHECK_FALSE(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate_bad.scen"))));
    // (b) dry-run validates without applying
    const auto dry =
        call(server, R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"instantiate","arguments":{"pack":"ceridc_gate.pack.crdr","asset":"tri.stl","x":2,"y":0,"z":-3,"out":"ceridc_gate.scen","dry_run":true}}})");
    CHECK(response_has(dry, "\"would_apply\":true"));
    CHECK_FALSE(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate.scen"))));
    // (c) the real edit applies — the SCEN artifact exists
    const auto apply =
        call(server, R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"instantiate","arguments":{"pack":"ceridc_gate.pack.crdr","asset":"tri.stl","x":2,"y":0,"z":-3,"out":"ceridc_gate.scen"}}})");
    CHECK(response_has(apply, "\"applied\":true"));
    CHECK(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate.scen"))));

    // ── sequence: the 2-shot timeline ─────────────────────────────────────────────────────────────────────────
    const auto seq =
        call(server, R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"sequence","arguments":{"name":"gate-cut","clip_a":"shotA","frames_a":48,"clip_b":"shotB","frames_b":48,"transition":12,"out_timl":"ceridc_gate.timl.crdr","out_otio":"ceridc_gate.otio"}}})");
    CHECK(response_has(seq, "\"ok\":true"));
    { // the .otio twin re-imports with the exact structure
        crd::containers::Array<crd::u8> bytes(&g_alloc);
        REQUIRE(fs::read_file_binary(fs::Path(crd::containers::StringView("ceridc_gate.otio")), bytes));
        crd::assetio::ImportedTimeline tl(&g_alloc);
        REQUIRE(crd::assetio::otio_parse(crd::containers::as_const_span(bytes), tl, nullptr) ==
                crd::assetio::OtioResult::Ok);
        REQUIRE(tl.tracks.size() == 1);
        REQUIRE(tl.items.size() == 3); // clip · transition · clip
        CHECK(tl.items[1].type == crd::assetio::OtioItemType::Transition);
        CHECK(tl.items[1].in_offset.value == 6);
    }

    // ── render: EXR frames land on disk and decode through OUR codec ──────────────────────────────────────────
    const auto render =
        call(server, R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"render","arguments":{"otio":"ceridc_gate.otio","dir":"ceridc_gate_frames","max_frames":24}}})");
    CHECK(response_has(render, "\"ok\":true"));
    CHECK(response_has(render, "\"frames\":24"));
    {
        crd::containers::Array<crd::u8> exr(&g_alloc);
        REQUIRE(fs::read_file_binary(fs::Path(crd::containers::StringView("ceridc_gate_frames/f0000.exr")),
                                     exr));
        crd::resources::HdrImage img(&g_alloc);
        REQUIRE(crd::resources::hdr_decode_exr(crd::containers::as_const_span(exr), img, &g_alloc) ==
                crd::resources::HdrError::Ok);
        CHECK(img.width == 64);
        CHECK(img.height == 36);
    }

    // ── export: TIML → .otio (the interchange edge, resource-side) ────────────────────────────────────────────
    const auto exported =
        call(server, R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"export","arguments":{"timl":"ceridc_gate.timl.crdr","out":"ceridc_gate_export.otio"}}})");
    CHECK(response_has(exported, "\"ok\":true"));
    {
        crd::containers::Array<crd::u8> bytes(&g_alloc);
        REQUIRE(
            fs::read_file_binary(fs::Path(crd::containers::StringView("ceridc_gate_export.otio")), bytes));
        crd::assetio::ImportedTimeline tl(&g_alloc);
        REQUIRE(crd::assetio::otio_parse(crd::containers::as_const_span(bytes), tl, nullptr) ==
                crd::assetio::OtioResult::Ok);
        CHECK(tl.items.size() == 3);
    }

    // ── the reports are the structured results, and a failed verb is a tool error ─────────────────────────────────
    CHECK(response_has(import_report, "\"structuredContent\":{\"verb\":\"import\",\"path\":\"ceridc_gate_src/tri.stl\","
                                      "\"ok\":true,\"format\":\"mesh-asset\",\"meshes\":1,"));
    CHECK(response_has(bad, "\"structuredContent\":{\"ok\":false,\"verb\":\"instantiate\","
                            "\"reason\":\"no MESH with that name in the pack\"},\"isError\":true"));
    CHECK(response_has(render, "\"structuredContent\":{\"verb\":\"render\",\"ok\":true,\"frames\":24,"));

    // ── protocol faults answer as JSON-RPC errors ─────────────────────────────────────────────────────────────
    CHECK(response_has(call(server, R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"no-such-tool",)"
                                    R"("arguments":{}}})"),
                       "\"id\":12,\"error\":{\"code\":-32602,\"message\":\"unknown tool: no-such-tool\""));
    CHECK(response_has(call(server, R"({"jsonrpc":"2.0","id":13,"method":"bogus/method","params":{}})"), "-32601"));
    CHECK(response_has(rpc(server, "this is not json"), "-32700"));
}

TEST_CASE("the stdio transport: the real binary answers over pipes, byte for byte", "[ceridc][agent][mcp]")
{
    // scripted stdin → the REAL `ceridc mcp` process → captured stdout (cmd redirection — the honest end-to-end
    // transport check; the protocol semantics are the in-process cases' job). One process serves both revisions: the
    // 2026-07-28 requests before the handshake, the 2025-11-25 ones after it. A line holding a NUL byte is one
    // message the parser refuses (not a ping cut at the NUL), a line past the bound is answered once, a 0x1A byte
    // does not end the input on Windows (the line holding it is refused and the next one answered), and the last line
    // needs no newline.
    crd::containers::String script(&g_alloc);
    const auto line = [&script](const char* request)
    {
        script.append(request);
        script.push_back('\n');
    };
    script.append(crd::ceridc::test::modern(R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})",
                                            &g_alloc));
    script.push_back('\n');
    script.append(crd::ceridc::test::modern(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})", &g_alloc));
    script.push_back('\n');
    line(R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{}})");
    line(R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":"2024-11-05",)"
         R"("capabilities":{},"clientInfo":{"name":"pipe","version":"1"}}})");
    line(R"({"jsonrpc":"2.0","id":5,"method":"tools/list","params":{}})");
    line(R"({"jsonrpc":"2.0","id":6,"method":"ping"})");
    script.append(R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
    script.push_back('\0');
    script.append("x\n");
    for (crd::usize i = 0U; i <= (crd::usize{1} << 20U); ++i)
    {
        script.push_back('a');
    }
    script.push_back('\n');
    script.append(R"({"jsonrpc":"2.0","id":8,"method":"ping","params":{"_meta":{"note":")");
    script.push_back('\x1A');
    script.append(R"("}}})");
    script.push_back('\n');
    script.append(R"({"jsonrpc":"2.0","id":9,"method":"ping"})"); // no newline: the input ends here
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView("ceridc_mcp_in.jsonl")),
                                  {reinterpret_cast<const crd::u8*>(script.data()), script.size()}));

    crd::containers::String exe_text;
    REQUIRE(crd::read_environment_text("CRD_CERIDC_EXE", exe_text)); // wired by CMake: the test knows its binary
    // the absolute exe path has no spaces ⇒ one level of quotes; cmd opens the redirection files in the CWD
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp < ceridc_mcp_in.jsonl > ceridc_mcp_out.jsonl", exe_text.c_str());
    REQUIRE(std::system(cmd) == 0);
    crd::containers::Array<crd::u8> out(&g_alloc);
    REQUIRE(fs::read_file_binary(fs::Path(crd::containers::StringView("ceridc_mcp_out.jsonl")), out));
    const crd::containers::StringView text{reinterpret_cast<const char*>(out.data()), out.size()};
    CHECK(text.find('\r') == crd::containers::StringView::npos); // binary streams: a reply ends in "\n" alone

    const char* const expected[] = {
        R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete","supportedVersions":["2026-07-28","2025-11-25"],)",
        R"({"jsonrpc":"2.0","id":2,"result":{"resultType":"complete","tools":[{"name":"cook",)",
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32602,)",
        R"({"jsonrpc":"2.0","id":4,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":)",
        R"({"jsonrpc":"2.0","id":5,"result":{"tools":[{"name":"cook",)",
        R"({"jsonrpc":"2.0","id":6,"result":{}})",
        R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"parse error"}})",
        R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"invalid request: a message is at most 1048576 bytes"}})",
        R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"parse error"}})", // a raw 0x1A is not JSON text
        R"({"jsonrpc":"2.0","id":9,"result":{}})",
    };
    crd::usize start = 0U;
    crd::usize n     = 0U;
    while (start < text.size())
    {
        const crd::usize end = text.find('\n', start);
        REQUIRE(end != crd::containers::StringView::npos); // every reply is a whole line
        const crd::containers::StringView reply = text.substr(start, end - start);
        INFO(n << ": " << reply);
        REQUIRE(n < sizeof(expected) / sizeof(expected[0]));
        CHECK(reply.substr(0U, std::strlen(expected[n])) == crd::containers::StringView{expected[n]});
        start = end + 1U;
        ++n;
    }
    CHECK(n == sizeof(expected) / sizeof(expected[0]));
}

TEST_CASE("MCP 2026-07-28: discovery, the per-request _meta and the version error", "[ceridc][mcp]")
{
    crd::ceridc::McpServer server(&g_alloc);
    const auto has = [](const crd::containers::String& reply, const char* needle)
    {
        return std::strstr(reply.c_str(), needle) != nullptr;
    };

    // Discovery names both revisions, the tools capability, the caching hints and the server in _meta.
    const auto discover = call(server, R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})");
    CHECK(has(discover, R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete",)"
                        R"("supportedVersions":["2026-07-28","2025-11-25"],)"
                        R"("capabilities":{"tools":{"listChanged":false}},)"));
    CHECK(has(discover, R"("ttlMs":3600000,"cacheScope":"public","_meta":{"io.modelcontextprotocol/serverInfo":)"
                        R"({"name":"ceridc","title":"Cerid command line","version":")" CRD_VERSION_STRING R"("}}}})"));
    CHECK_FALSE(server.legacy());

    // Another version is refused with the supported ones; 2025-11-25 belongs to the handshake, not to _meta.
    const char* const meta_tail = R"(","io.modelcontextprotocol/clientCapabilities":{}}}})";
    char request[256];
    (void)std::snprintf(request, sizeof(request),
                        R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{)"
                        R"("io.modelcontextprotocol/protocolVersion":"%s%s)",
                        "2099-01-01", meta_tail);
    CHECK(has(rpc(server, request), R"({"jsonrpc":"2.0","id":2,"error":{"code":-32022,"message":"Unsupported protocol )"
                                    R"(version","data":{"supported":["2026-07-28","2025-11-25"],)"
                                    R"("requested":"2099-01-01"}}})"));
    (void)std::snprintf(request, sizeof(request),
                        R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"_meta":{)"
                        R"("io.modelcontextprotocol/protocolVersion":"%s%s)",
                        "2025-11-25", meta_tail);
    CHECK(has(rpc(server, request), R"("id":3,"error":{"code":-32602,"message":"revision 2025-11-25 is served )"
                                    R"(after an )"));

    // A malformed _meta is refused before the method is looked at.
    for (const char* bad : {
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"_meta":[]}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"_meta":{)"
             R"("io.modelcontextprotocol/protocolVersion":7,"io.modelcontextprotocol/clientCapabilities":{}}}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"_meta":{)"
             R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"_meta":{)"
             R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
             R"("io.modelcontextprotocol/clientCapabilities":[]}}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"_meta":{)"
             R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
             R"("io.modelcontextprotocol/clientCapabilities":{},)"
             R"("io.modelcontextprotocol/clientInfo":{"name":"x"}}}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"_meta":{)"
             R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
             R"("io.modelcontextprotocol/clientCapabilities":{},)"
             R"("io.modelcontextprotocol/logLevel":"loud"}}})",
             R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":[]})",
         })
    {
        INFO(bad);
        CHECK(has(rpc(server, bad), R"({"jsonrpc":"2.0","id":4,"error":{"code":-32602,)"));
    }
    // ping is gone from this revision; tools/list has one page, so no cursor names a page of it.
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":5,"method":"ping","params":{}})"),
              R"("id":5,"error":{"code":-32601,)"));
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":6,"method":"tools/list","params":{"cursor":"x"}})"),
              R"("id":6,"error":{"code":-32602,"message":"unknown cursor)"));
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{}})"), R"("ttlMs":3600000,)"));

    // The list is deterministic: in name order, the same bytes each time, each tool with both schemas and hints.
    const auto first  = call(server, R"({"jsonrpc":"2.0","id":8,"method":"tools/list","params":{}})");
    const auto second = call(server, R"({"jsonrpc":"2.0","id":8,"method":"tools/list","params":{}})");
    CHECK(crd::containers::StringView{first.data(), first.size()} ==
          crd::containers::StringView{second.data(), second.size()});
    const char* last = first.c_str();
    for (const char* name : {"cook", "export", "import", "instantiate", "query", "render", "sequence"})
    {
        char needle[64];
        (void)std::snprintf(needle, sizeof(needle), R"({"name":"%s","title":")", name);
        const char* at = std::strstr(last, needle);
        INFO(name);
        REQUIRE(at != nullptr);
        last = at;
    }
    CHECK_FALSE(has(first, R"("name":"diag")")); // no service bound here
    CHECK(has(first, R"("inputSchema":{"type":"object","properties":{"path":{"type":"string","minLength":1,)"
                     R"j("maxLength":511,"description":"The interchange file to read (1 to 511 UTF-8 bytes, no NUL)"}},)j"
                     R"("required":["path"],"additionalProperties":false},"outputSchema":{"type":"object","anyOf":[)"));
    CHECK(has(first, R"("annotations":{"readOnlyHint":true,"openWorldHint":false})"));
    CHECK(has(first, R"("annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":false})"));

    // A notification is never answered, whatever its revision or shape.
    CHECK(rpc(server, R"({"jsonrpc":"2.0","method":"tools/list"})").empty());
}

TEST_CASE("MCP 2025-11-25: the initialize handshake selects the legacy revision for the connection", "[ceridc][mcp]")
{
    crd::ceridc::McpServer server(&g_alloc);
    const auto has = [](const crd::containers::String& reply, const char* needle)
    {
        return std::strstr(reply.c_str(), needle) != nullptr;
    };
    // Before the handshake a request without _meta is malformed, but ping is served, as 2025-11-25 allows.
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})"),
              R"("id":1,"error":{"code":-32602,)"));
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":2,"method":"ping"})"), R"({"jsonrpc":"2.0","id":2,"result":{}})"));

    // initialize needs its three fields; a refused handshake selects nothing.
    for (const char* bad : {R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{}})",
                            R"({"jsonrpc":"2.0","id":3,"method":"initialize"})",
                            R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-11-25",)"
                            R"("capabilities":{},"clientInfo":{"version":"1"}}})"})
    {
        INFO(bad);
        CHECK(has(rpc(server, bad), R"("id":3,"error":{"code":-32602,)"));
        CHECK_FALSE(server.legacy());
    }
    // Whatever the client proposes, the answer names 2025-11-25, the server's revision of that era.
    const auto init = rpc(server, R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":)"
                                  R"("2024-11-05","capabilities":{},"clientInfo":{"name":"c","version":"1"}}})");
    CHECK(has(init, R"({"jsonrpc":"2.0","id":4,"result":{"protocolVersion":"2025-11-25",)"
                    R"("capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"ceridc",)"));
    CHECK_FALSE(has(init, "resultType"));
    CHECK(server.legacy());
    CHECK(rpc(server, R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());

    // The legacy results: no resultType, no caching hints, no _meta; the same tools and reports.
    const auto list = rpc(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list","params":{}})");
    CHECK(has(list, R"({"jsonrpc":"2.0","id":5,"result":{"tools":[{"name":"cook",)"));
    CHECK_FALSE(has(list, "resultType"));
    CHECK_FALSE(has(list, "ttlMs"));
    CHECK(has(list, R"("outputSchema":{"type":"object","anyOf":[)"));
    const auto query = rpc(server, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"query",)"
                                   R"("arguments":{"pack":"ceridc_legacy_none.crdr"}}})");
    CHECK(has(query, R"("structuredContent":{"ok":false,"verb":"query","reason":"not a readable PACK"},)"
                     R"("isError":true}})"));
    CHECK_FALSE(has(query, "resultType"));
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":7,"method":"server/discover","params":{}})"),
              R"("id":7,"error":{"code":-32601,)"));

    // The server is dual-era: a 2026-07-28 request is still served statelessly on the same connection.
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":8,"method":"tools/list","params":{}})"),
              R"({"jsonrpc":"2.0","id":8,"result":{"resultType":"complete","tools":[)"));
}

TEST_CASE("the MCP envelope: ids echo exactly, names compare whole, arguments are checked before anything runs",
          "[ceridc][mcp][diag10a]")
{
    // DIAG.10a (crd-fuzz-ceridc-diag). Before: a string id was answered as 0, a fraction truncated and 1e300 cast to
    // i64 (undefined); a batch array or bare value got no reply at all; "ping\u0000x" was ping; a path past 511
    // bytes or one holding a NUL was cut to another path; dry_run sent as "true" wrote the file; frames of 1e300
    // were cast to i64 (undefined). MCP-ADAPT.0: a refused argument is a tool error naming it (the model can correct
    // the call), not a protocol error; an id that cannot be echoed is left out of the error, as MCP's schema says.
    crd::ceridc::McpServer server(&g_alloc);
    const auto has = [](const crd::containers::String& reply, const char* needle)
    {
        return std::strstr(reply.c_str(), needle) != nullptr;
    };

    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":"req-7","method":"ping"})"), R"("id":"req-7","result":{})"));
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":-9007199254740991,"method":"ping"})"),
              R"("id":-9007199254740991,)"));
    for (const char* id : {"null", "1.5", "9007199254740993", "1e300", "{}", "[1]", "true"})
    {
        char line[160];
        std::snprintf(line, sizeof(line), R"({"jsonrpc":"2.0","id":%s,"method":"ping"})", id);
        INFO(line);
        CHECK(has(rpc(server, line), R"({"jsonrpc":"2.0","error":{"code":-32600,)"));
    }
    CHECK(has(rpc(server, R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])"),
              R"({"jsonrpc":"2.0","error":{"code":-32600,)"));
    CHECK(has(rpc(server, "42"), R"({"jsonrpc":"2.0","error":{"code":-32600,)"));
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":2,"method":7})"), R"("id":2,"error":{"code":-32600,)"));
    CHECK(has(rpc(server, R"({"id":2,"method":"ping"})"), R"("id":2,"error":{"code":-32600,)"));
    CHECK(has(rpc(server, R"({"jsonrpc":"1.0","id":2,"method":"ping"})"), R"("id":2,"error":{"code":-32600,)"));
    CHECK(rpc(server, R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty()); // still no reply
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":3,"method":"ping\u0000x"})"), R"("id":3,"error":{"code":-32602,)"));
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"query\u0000",)"
                           R"("arguments":{"pack":"x.crdr"}}})"),
              R"("id":4,"error":{"code":-32602,"message":"unknown tool: query\u0000"})"));
    CHECK(has(rpc(server, R"({"jsonrpc":"2.0","id":5,"method":"ping","params":1e400})"),
              R"({"jsonrpc":"2.0","error":{"code":-32700,)"));
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"arguments":{}}})"),
              R"("id":5,"error":{"code":-32602,)"));
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"query",)"
                           R"("arguments":[]}})"),
              R"("id":5,"error":{"code":-32602,"message":"tools/call arguments must be an object"})"));

    // A path is used whole or refused: 511 bytes still reach the verb, 512 do not, and neither does a NUL.
    crd::containers::String exact(&g_alloc);
    exact.append(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"query","arguments":{"pack":")");
    for (int i = 0; i < 511; ++i)
    {
        exact.push_back('p');
    }
    crd::containers::String over(exact, &g_alloc);
    over.push_back('p');
    exact.append(R"("}}})");
    over.append(R"("}}})");
    CHECK(has(call(server, exact.c_str()), R"("structuredContent":{"ok":false,"verb":"query","reason":"not a readable )"
                                           R"(PACK"})")); // the verb ran and reported the missing pack
    const char* const refused_path = R"("structuredContent":{"ok":false,"verb":"query","reason":"argument 'pack' must )"
                                     R"(be a string of 1 to 511 UTF-8 bytes without NUL"},"isError":true)";
    CHECK(has(call(server, over.c_str()), refused_path));
    CHECK(has(call(server, R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"query",)"
                           R"("arguments":{"pack":"ceridc_envelope.crdr\u0000.txt"}}})"),
              refused_path));

    // Every other refusal names its argument, and nothing is written.
    struct Refusal
    {
        const char* arguments;
        const char* reason;
    };
    for (const Refusal& r : {
             Refusal{R"({"pack":"none.crdr","asset":"a","out":"ceridc_envelope.scen","dry_run":"true"})",
                     "argument 'dry_run' must be a boolean"},
             Refusal{R"({"pack":"none.crdr","asset":"a","out":"ceridc_envelope.scen","x":1e39})",
                     "argument 'x' must be a number within the f32 range"},
             Refusal{R"({"pack":"none.crdr","asset":"a","out":"ceridc_envelope.scen","dryrun":true})",
                     "unknown argument 'dryrun'"},
             Refusal{R"({"pack":"none.crdr","asset":"a","out":"ceridc_envelope.scen","x":1,"x":2})",
                     "argument 'x' is given more than once"},
             Refusal{R"({"pack":"none.crdr","out":"ceridc_envelope.scen"})", "argument 'asset' is required"},
             Refusal{R"({"pack":"none.crdr","asset":"","out":"ceridc_envelope.scen"})",
                     "argument 'asset' must be a string of 1 to 511 UTF-8 bytes without NUL"},
         })
    {
        char line[512];
        std::snprintf(line, sizeof(line),
                      R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"instantiate",)"
                      R"("arguments":%s}})",
                      r.arguments);
        char expected[256];
        std::snprintf(expected, sizeof(expected),
                      R"("structuredContent":{"ok":false,"verb":"instantiate","reason":"%s"},"isError":true)",
                      r.reason);
        INFO(line);
        CHECK(has(call(server, line), expected));
    }
    CHECK_FALSE(fs::exists(fs::Path(crd::containers::StringView("ceridc_envelope.scen"))));
    for (const char* frames : {"1e300", "1.5", "\"48\""})
    {
        char line[400];
        std::snprintf(line, sizeof(line),
                      R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"sequence","arguments":)"
                      R"({"name":"n","clip_a":"a","frames_a":%s,"clip_b":"b","frames_b":4,)"
                      R"("out_timl":"ceridc_envelope.timl.crdr","out_otio":"ceridc_envelope.otio"}}})",
                      frames);
        INFO(line);
        CHECK(has(call(server, line),
                  R"j("reason":"argument 'frames_a' must be a whole number within +-(2^53 - 1)"})j"));
    }
    CHECK_FALSE(fs::exists(fs::Path(crd::containers::StringView("ceridc_envelope.otio"))));
}
