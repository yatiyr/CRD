// test_ceridc.cpp — GEO-11 (D-007 row 76): THE AGENT GATE. An agent, through the MCP protocol handler ALONE
// (initialize → tools/list → tools/call), imports a model, cooks a pack, queries the resource graph, composes
// a scene (transactionally — the malformed edit rejects atomically, dry-run applies nothing), sequences a
// 2-shot timeline, renders it to EXR frames, and exports the interchange twin — every effect verified through
// files on disk and query verbs, zero GUI. A second smoke drives the REAL binary's stdio loop end to end.

#include <crd/assetio/otio.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/hdr_image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// one JSON-RPC call through the REAL protocol handler
crd::containers::String rpc(const char* request)
{
    return crd::ceridc::mcp_handle(
        {reinterpret_cast<const crd::u8*>(request), std::strlen(request)}, &g_alloc);
}

[[nodiscard]] bool response_has(const crd::containers::String& response, const char* needle)
{
    // tool reports ride inside the MCP response as ESCAPED JSON text (\" for ") — strip the escapes so one
    // needle form asserts at both the protocol level and inside the report
    crd::containers::String flat(&g_alloc);
    for (crd::usize i = 0; i < response.size(); ++i)
    {
        if (response.c_str()[i] == '\\') { continue; }
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
    for (crd::u8 x : raw) { b.push_back(x); }
}

// a 1-triangle binary STL — the model the agent imports and cooks
void write_stl(const char* path)
{
    crd::containers::Array<crd::u8> stl(&g_alloc);
    for (int i = 0; i < 80; ++i) { stl.push_back(0); }
    const crd::u8 one[4] = {1, 0, 0, 0};
    for (crd::u8 x : one) { stl.push_back(x); }
    const crd::f32 tri[12] = {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (crd::f32 v : tri) { push_f32(stl, v); }
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

    // ── the handshake ─────────────────────────────────────────────────────────────────────────────────────────
    const auto init = rpc(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    CHECK(response_has(init, "\"serverInfo\""));
    CHECK(response_has(init, "ceridc"));
    CHECK(rpc(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty()); // notification: no reply

    const auto tools = rpc(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
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
        rpc(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"import","arguments":{"path":"ceridc_gate_src/tri.stl"}}})");
    CHECK(response_has(import_report, "\"ok\":true"));
    CHECK(response_has(import_report, "\"meshes\":1"));

    // ── cook: the pack materializes ───────────────────────────────────────────────────────────────────────────
    const auto cook_report =
        rpc(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"cook","arguments":{"root":"ceridc_gate_src","out":"ceridc_gate.pack.crdr"}}})");
    CHECK(response_has(cook_report, "\"ok\":true"));
    REQUIRE(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate.pack.crdr"))));

    // ── query: the resource graph is visible ──────────────────────────────────────────────────────────────────
    const auto query_report =
        rpc(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"query","arguments":{"pack":"ceridc_gate.pack.crdr"}}})");
    CHECK(response_has(query_report, "\"ok\":true"));
    CHECK(response_has(query_report, "MESH"));
    CHECK(response_has(query_report, "tri"));

    // ── instantiate: TRANSACTIONAL scene composition ──────────────────────────────────────────────────────────
    // (a) the malformed edit rejects atomically — nothing lands on disk
    const auto bad =
        rpc(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"instantiate","arguments":{"pack":"ceridc_gate.pack.crdr","asset":"no-such-mesh","x":1,"out":"ceridc_gate_bad.scen"}}})");
    CHECK(response_has(bad, "\"isError\":true"));
    CHECK_FALSE(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate_bad.scen"))));
    // (b) dry-run validates without applying
    const auto dry =
        rpc(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"instantiate","arguments":{"pack":"ceridc_gate.pack.crdr","asset":"tri.stl","x":2,"y":0,"z":-3,"out":"ceridc_gate.scen","dry_run":true}}})");
    CHECK(response_has(dry, "\"would_apply\":true"));
    CHECK_FALSE(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate.scen"))));
    // (c) the real edit applies — the SCEN artifact exists
    const auto apply =
        rpc(R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"instantiate","arguments":{"pack":"ceridc_gate.pack.crdr","asset":"tri.stl","x":2,"y":0,"z":-3,"out":"ceridc_gate.scen"}}})");
    CHECK(response_has(apply, "\"applied\":true"));
    CHECK(fs::is_file(fs::Path(crd::containers::StringView("ceridc_gate.scen"))));

    // ── sequence: the 2-shot timeline ─────────────────────────────────────────────────────────────────────────
    const auto seq =
        rpc(R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"sequence","arguments":{"name":"gate-cut","clip_a":"shotA","frames_a":48,"clip_b":"shotB","frames_b":48,"transition":12,"out_timl":"ceridc_gate.timl.crdr","out_otio":"ceridc_gate.otio"}}})");
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
        rpc(R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"render","arguments":{"otio":"ceridc_gate.otio","dir":"ceridc_gate_frames","max_frames":24}}})");
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
        rpc(R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"export","arguments":{"timl":"ceridc_gate.timl.crdr","out":"ceridc_gate_export.otio"}}})");
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

    // ── protocol faults answer as JSON-RPC errors ─────────────────────────────────────────────────────────────
    CHECK(response_has(rpc(R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"no-such-tool","arguments":{}}})"),
                       "-32602"));
    CHECK(response_has(rpc(R"({"jsonrpc":"2.0","id":13,"method":"bogus/method"})"), "-32601"));
    CHECK(response_has(rpc("this is not json"), "-32700"));
}

TEST_CASE("the stdio transport: the real binary answers over pipes", "[ceridc][agent]")
{
    // scripted stdin → the REAL `ceridc mcp` process → captured stdout (cmd redirection — the honest
    // end-to-end transport smoke; the protocol semantics are the in-process gate's job)
    write_text("ceridc_mcp_in.jsonl",
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
               "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n"
               "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n");
    const char* exe = std::getenv("CRD_CERIDC_EXE");
    REQUIRE(exe != nullptr); // wired by CMake — the test always knows its binary
    // the absolute exe path has no spaces ⇒ one level of quotes; cmd opens the redirection files in the CWD
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" mcp < ceridc_mcp_in.jsonl > ceridc_mcp_out.jsonl", exe);
    REQUIRE(std::system(cmd) == 0);
    crd::containers::Array<crd::u8> out(&g_alloc);
    REQUIRE(fs::read_file_binary(fs::Path(crd::containers::StringView("ceridc_mcp_out.jsonl")), out));
    out.push_back(0);
    const char* text = reinterpret_cast<const char*>(out.data());
    CHECK(std::strstr(text, "serverInfo") != nullptr);
    CHECK(std::strstr(text, "\"sequence\"") != nullptr);
    CHECK(std::strstr(text, "\"id\":3") != nullptr);
}
