// The typed, bounded diagnostic command service, driven by a native caller with no transport at all: this executable
// links neither ceridc nor an MCP or network library, so every case here is the process-local proof. Covers the fixed
// refusal order and that a refusal does no work (no handler run, no file byte read, the retained snapshot kept),
// deterministic pagination and stale cursors, the byte bounds, authority classes that no grant implies, named
// arguments bounded and checked in the arguments step, the bundle importer route, the capture window and the job wait
// graph.

#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/scope.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace cont = crd::containers;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;

namespace
{

// Scratch root for the path-taking commands: this test's build directory.
constexpr const char* kScratch = CRD_DIAG_COMMANDS_SCRATCH;

[[nodiscard]] bool contains(const cont::String& s, const char* needle)
{
    return std::strstr(s.c_str(), needle) != nullptr;
}

[[nodiscard]] DiagRequest request(const char* command)
{
    DiagRequest r;
    r.command = command;
    return r;
}

// A path command that keeps the root the service handed it.
struct RootCommand
{
    crd::u64     runs = 0U;
    cont::String root;
};

DiagStatus run_root(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    (void)out;
    auto* cmd = static_cast<RootCommand*>(context);
    ++cmd->runs;
    cmd->root.clear();
    cmd->root.append(call.root);
    return DiagStatus::Ok;
}

// A Read command producing `count` items whose text is fixed by their index, counting its own runs.
struct ItemsCommand
{
    crd::u32 count     = 100U;
    crd::u32 pad_bytes = 0U; // extra payload per item, to exercise the byte bound
    crd::u64 runs      = 0U;
};

DiagStatus run_items(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    auto* cmd = static_cast<ItemsCommand*>(context);
    ++cmd->runs;
    (void)call;
    cont::String pad(out.allocator());
    pad.resize(cmd->pad_bytes, 'x');
    crd::perf::DiagFields item(out.allocator());
    for (crd::u32 i = 0U; i < cmd->count; ++i)
    {
        item.clear();
        item.u64("i", i).u64("square", static_cast<crd::u64>(i) * i);
        if (cmd->pad_bytes > 0U)
        {
            item.str("pad", cont::StringView{pad.data(), pad.size()});
        }
        (void)out.add_item(item);
    }
    out.summary.u64("count", cmd->count);
    return DiagStatus::Ok;
}

// A command that must never run (its authority is withheld); counts runs to prove it.
struct NeverCommand
{
    crd::u64 runs = 0U;
};

DiagStatus run_never(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    (void)call;
    (void)out;
    ++static_cast<NeverCommand*>(context)->runs;
    return DiagStatus::Ok;
}

// A command taking named arguments: its check accepts the names "line" and "mode" (with a "mode" value of "a" or "b")
// and counts its own runs; its handler answers one item per argument it was given.
struct ArgsCommand
{
    crd::u64 checks = 0U;
    crd::u64 runs   = 0U;
};

DiagStatus check_args(void* context, cont::ConstSpan<crd::perf::DiagArg> args, cont::String& reason)
{
    ++static_cast<ArgsCommand*>(context)->checks;
    for (const crd::perf::DiagArg& a : args)
    {
        if (a.name == "mode" && a.value != "a" && a.value != "b")
        {
            reason.append("mode must be a or b");
            return DiagStatus::BadArgument;
        }
        if (a.name != "mode" && a.name != "line")
        {
            reason.append("unknown argument");
            return DiagStatus::BadArgument;
        }
    }
    return DiagStatus::Ok;
}

DiagStatus run_args(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    ++static_cast<ArgsCommand*>(context)->runs;
    crd::perf::DiagFields item(out.allocator());
    for (const crd::perf::DiagArg& a : call.request->args)
    {
        item.clear();
        item.str("name", a.name).str("value", a.value);
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
}

// One item with an over-long string and many fields, to exercise both clips.
DiagStatus run_wide(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    (void)context;
    (void)call;
    cont::String long_value(out.allocator());
    long_value.resize(2000U, 'v');
    crd::perf::DiagFields item(out.allocator());
    item.str("long", cont::StringView{long_value.data(), long_value.size()});
    for (crd::u32 i = 0U; i < 200U; ++i)
    {
        char key[16];
        (void)std::snprintf(key, sizeof(key), "field%u", i);
        item.u64(key, i);
    }
    (void)out.add_item(item);
    item.clear();
    item.str("short", "fits");
    (void)out.add_item(item);
    return DiagStatus::Ok;
}

[[nodiscard]] cont::String scratch_file(const char* name, crd::memory::IAllocator* alloc)
{
    cont::String path(kScratch, alloc);
    path.push_back('/');
    path.append(name);
    return path;
}

void write_file(const char* path, const crd::u8* bytes, crd::usize n)
{
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    REQUIRE(fopen_s(&f, path, "wb") == 0);
#else
    f = std::fopen(path, "wb");
#endif
    REQUIRE(f != nullptr);
    REQUIRE(std::fwrite(bytes, 1U, n, f) == n);
    REQUIRE(std::fclose(f) == 0);
}

// The items array of a response document (the text between "items":[ and the closing ]}).
[[nodiscard]] cont::StringView items_text(const cont::String& json)
{
    const cont::StringView doc{json.data(), json.size()};
    const crd::usize       at = doc.find("\"items\":[");
    REQUIRE(at != cont::StringView::npos);
    REQUIRE(doc.size() >= at + 9U + 2U);
    return doc.substr(at + 9U, doc.size() - (at + 9U) - 2U);
}

} // namespace

TEST_CASE("diag commands: every refusal comes before any work and in the fixed order", "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-refusals"};
    DiagServiceConfig          config;
    config.root = kScratch;
    DiagCommandService svc{crd::perf::authority_bit(DiagAuthority::Read), config, &alloc};

    ItemsCommand items;
    REQUIRE(svc.register_command({"test.items", "test", "fixed items", DiagAuthority::Read, false}, &run_items,
                                 &items));

    // A retained snapshot that every refusal below must leave usable.
    DiagRequest first = request("test.items");
    first.page_items  = 10U;
    const DiagResult held = svc.execute(first);
    REQUIRE(held.status == DiagStatus::Ok);
    REQUIRE(held.next_cursor != 0U);
    const crd::u64 runs_before  = svc.handler_runs();
    const crd::u64 bytes_before = svc.file_bytes_read();
    REQUIRE(runs_before == 1U);

    struct Case
    {
        const char* name;
        DiagRequest req;
        DiagStatus  expected;
    };
    cont::String long_path(&alloc);
    long_path.resize(crd::perf::kDiagMaxPathBytes + 1U, 'a');

    Case cases[] = {
        {"schema", request("diag.commands"), DiagStatus::UnsupportedSchema},
        {"schema beats unknown", request("no.such"), DiagStatus::UnsupportedSchema},
        {"unknown", request("no.such"), DiagStatus::UnknownCommand},
        {"unknown beats authority and bounds", request("capture.startx"), DiagStatus::UnknownCommand},
        {"authority", request("capture.start"), DiagStatus::Unauthorized},
        {"authority beats bounds", request("capture.stop"), DiagStatus::Unauthorized},
        {"page items", request("diag.commands"), DiagStatus::Oversized},
        {"page bytes", request("diag.commands"), DiagStatus::Oversized},
        {"path length", request("bundle.inspect"), DiagStatus::Oversized},
        {"bounds beat arguments", request("bundle.inspect"), DiagStatus::Oversized},
        {"page bytes below the minimum", request("diag.commands"), DiagStatus::BadArgument},
        {"path on a command without one", request("diag.commands"), DiagStatus::BadArgument},
        {"missing path", request("bundle.inspect"), DiagStatus::BadArgument},
        {"parent component", request("bundle.inspect"), DiagStatus::BadArgument},
        {"absolute", request("bundle.inspect"), DiagStatus::BadArgument},
        {"drive", request("bundle.inspect"), DiagStatus::BadArgument},
        {"backslash", request("bundle.inspect"), DiagStatus::BadArgument},
        {"empty component", request("bundle.inspect"), DiagStatus::BadArgument},
        {"arguments beat the cursor", request("bundle.inspect"), DiagStatus::BadArgument},
        {"stale generation", request("test.items"), DiagStatus::StaleCursor},
        {"another command's cursor", request("diag.commands"), DiagStatus::StaleCursor},
        {"cursor past the snapshot", request("test.items"), DiagStatus::BadArgument},
    };
    cases[0].req.schema_version = 2U;
    cases[1].req.schema_version = 0U;
    cases[3].req.page_items     = 100000U;
    cases[5].req.page_items     = 100000U;
    cases[6].req.page_items     = crd::perf::kDiagMaxPageItems + 1U;
    cases[7].req.page_bytes     = crd::perf::kDiagMaxPageBytes + 1U;
    cases[8].req.path           = cont::StringView{long_path.data(), long_path.size()};
    cases[9].req.path           = cont::StringView{long_path.data(), long_path.size()};
    cases[9].req.page_bytes     = 1U;
    cases[10].req.page_bytes    = crd::perf::kDiagMinPageBytes - 1U;
    cases[11].req.path          = "bundle.cdb";
    cases[13].req.path          = "a/../bundle.cdb";
    cases[14].req.path          = "/bundle.cdb";
    cases[15].req.path          = "C:/bundle.cdb";
    cases[16].req.path          = "a\\bundle.cdb";
    cases[17].req.path          = "a//bundle.cdb";
    cases[18].req.path          = "../bundle.cdb";
    cases[18].req.cursor        = 12345U;
    cases[19].req.cursor        = ((held.generation + 1U) << crd::perf::kDiagCursorOffsetBits) | 3U;
    cases[20].req.cursor        = held.next_cursor;
    cases[21].req.cursor        = (held.generation << crd::perf::kDiagCursorOffsetBits) | 101U;

    for (const Case& c : cases)
    {
        INFO(c.name);
        const DiagResult r = svc.execute(c.req);
        CHECK(r.status == c.expected);
        CHECK(r.generation == 0U);
        CHECK(r.items == 0U);
        CHECK_FALSE(r.reason.empty());
        CHECK(contains(r.json, "\"ok\":false"));
        CHECK(contains(r.json, "\"items\":[]"));
        CHECK(svc.handler_runs() == runs_before);
        CHECK(svc.file_bytes_read() == bytes_before);
    }

    // A cancelled request is refused after every other check and before the handler.
    std::atomic<bool> cancel{true};
    const DiagResult  cancelled = svc.execute(request("diag.commands"), &cancel);
    CHECK(cancelled.status == DiagStatus::Cancelled);
    CHECK(svc.handler_runs() == runs_before);
    // ...and a refused cursor is checked before the cancel flag.
    DiagRequest stale = request("test.items");
    stale.cursor      = ((held.generation + 1U) << crd::perf::kDiagCursorOffsetBits);
    CHECK(svc.execute(stale, &cancel).status == DiagStatus::StaleCursor);

    // A host without a file root refuses path commands as unavailable, before opening anything.
    DiagCommandService no_root{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    DiagRequest        inspect = request("bundle.inspect");
    inspect.path               = "bundle.cdb";
    const DiagResult unrooted  = no_root.execute(inspect);
    CHECK(unrooted.status == DiagStatus::Unavailable);
    CHECK(no_root.handler_runs() == 0U);
    CHECK(no_root.file_bytes_read() == 0U);

    // Every refusal left the retained snapshot alone: its next page still comes from the same run.
    DiagRequest next = request("test.items");
    next.cursor      = held.next_cursor;
    next.page_items  = 10U;
    const DiagResult second = svc.execute(next);
    CHECK(second.status == DiagStatus::Ok);
    CHECK(second.generation == held.generation);
    CHECK(items.runs == 1U);
    CHECK(contains(second.json, "{\"i\":10,"));
}

TEST_CASE("diag commands: pagination is deterministic and a new snapshot makes old cursors stale",
          "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-pages"};
    DiagCommandService         svc{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    ItemsCommand               items;
    REQUIRE(svc.register_command({"test.items", "test", "fixed items", DiagAuthority::Read, false}, &run_items,
                                 &items));

    // Walk every page; the pages concatenate to exactly the snapshot, in order, from one handler run.
    DiagRequest req = request("test.items");
    req.page_items  = 7U;
    DiagResult page = svc.execute(req);
    REQUIRE(page.status == DiagStatus::Ok);
    CHECK(page.total == 100U);
    CHECK(page.items == 7U);
    CHECK(page.cursor == (page.generation << crd::perf::kDiagCursorOffsetBits));
    const crd::u64 generation = page.generation;
    cont::String   all(&alloc);
    all.append(items_text(page.json));
    cont::Array<crd::u64> cursors(&alloc);
    crd::u32              pages = 1U;
    while (page.next_cursor != 0U)
    {
        cursors.push_back(page.next_cursor);
        req.cursor = page.next_cursor;
        page       = svc.execute(req);
        REQUIRE(page.status == DiagStatus::Ok);
        CHECK(page.generation == generation);
        all.push_back(',');
        all.append(items_text(page.json));
        ++pages;
    }
    CHECK(pages == 15U); // 14 pages of 7 and one of 2
    CHECK(page.items == 2U);
    CHECK(page.complete);
    CHECK(items.runs == 1U);

    cont::String expected(&alloc);
    for (crd::u32 i = 0U; i < 100U; ++i)
    {
        char buf[64];
        (void)std::snprintf(buf, sizeof(buf), R"(%s{"i":%u,"square":%u})", i == 0U ? "" : ",", i, i * i);
        expected.append(buf);
    }
    CHECK(cont::StringView{all.data(), all.size()} == cont::StringView{expected.data(), expected.size()});

    // The same cursor and bounds always return the same bytes.
    req.cursor               = cursors[5];
    const DiagResult again_a = svc.execute(req);
    const DiagResult again_b = svc.execute(req);
    REQUIRE(again_a.status == DiagStatus::Ok);
    CHECK(cont::StringView{again_a.json.data(), again_a.json.size()} ==
          cont::StringView{again_b.json.data(), again_b.json.size()});
    CHECK(items.runs == 1U);

    // Another command's new snapshot replaces the retained one: every cursor of the old snapshot is stale.
    REQUIRE(svc.execute(request("diag.commands")).status == DiagStatus::Ok);
    for (const crd::u64 c : cursors)
    {
        req.cursor = c;
        CHECK(svc.execute(req).status == DiagStatus::StaleCursor);
    }
    // So is a cursor from an earlier snapshot of the same command.
    req.cursor                 = 0U;
    const DiagResult refreshed = svc.execute(req);
    REQUIRE(refreshed.status == DiagStatus::Ok);
    CHECK(refreshed.generation == generation + 2U);
    CHECK(items.runs == 2U);
    req.cursor = cursors[0];
    CHECK(svc.execute(req).status == DiagStatus::StaleCursor);
    req.cursor = refreshed.next_cursor;
    CHECK(svc.execute(req).status == DiagStatus::Ok);
}

TEST_CASE("diag commands: pages and items stay inside their byte bounds", "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-bounds"};
    DiagCommandService         svc{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    ItemsCommand               items;
    items.count     = 20U;
    items.pad_bytes = 150U; // each item is 177 or 178 bytes
    REQUIRE(svc.register_command({"test.items", "test", "padded items", DiagAuthority::Read, false}, &run_items,
                                 &items));
    REQUIRE(svc.register_command({"test.wide", "test", "clipped item", DiagAuthority::Read, false}, &run_wide,
                                 nullptr));

    DiagRequest req = request("test.items");
    req.page_items  = 100U;
    req.page_bytes  = crd::perf::kDiagMinPageBytes;
    const DiagResult page = svc.execute(req);
    REQUIRE(page.status == DiagStatus::Ok);
    CHECK(page.byte_bounded);
    CHECK(page.items == 5U); // five items and their commas take 890 bytes; a sixth would take 1069
    CHECK(items_text(page.json).size() <= crd::perf::kDiagMinPageBytes);
    CHECK(page.next_cursor != 0U);

    // An item never grows past its bound; an over-long string and the fields that do not fit are clipped, and the
    // object says so while staying well-formed.
    const DiagResult wide = svc.execute(request("test.wide"));
    REQUIRE(wide.status == DiagStatus::Ok);
    REQUIRE(wide.total == 2U);
    const cont::StringView text  = items_text(wide.json);
    const crd::usize       split = text.find("},{");
    REQUIRE(split != cont::StringView::npos);
    const cont::StringView first = text.substr(0U, split + 1U);
    CHECK(first.size() <= crd::perf::kDiagMaxItemBytes);
    CHECK(first.substr(first.size() - 16U) == ",\"clipped\":true}");
    const crd::usize long_at = first.find(R"("long":")");
    REQUIRE(long_at != cont::StringView::npos);
    const crd::usize long_end = first.find('"', long_at + 8U);
    CHECK(long_end - (long_at + 8U) == crd::perf::kDiagMaxFieldBytes);
    CHECK(text.substr(split + 2U) == "{\"short\":\"fits\"}");
}

TEST_CASE("diag commands: authority classes are distinct and registered commands get the same checks",
          "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-authority"};

    crd::perf::DiagAuthoritySet parsed = 0U;
    CHECK(crd::perf::parse_authority_list("read,record", parsed));
    CHECK(parsed == (crd::perf::authority_bit(DiagAuthority::Read) | crd::perf::authority_bit(DiagAuthority::Record)));
    CHECK(crd::perf::parse_authority_list("none", parsed));
    CHECK(parsed == 0U);
    parsed = 7U;
    CHECK_FALSE(crd::perf::parse_authority_list("read,,record", parsed));
    CHECK_FALSE(crd::perf::parse_authority_list("read,admin", parsed));
    CHECK_FALSE(crd::perf::parse_authority_list("", parsed));
    CHECK_FALSE(crd::perf::parse_authority_list("none,read", parsed));
    CHECK(parsed == 7U);

    CHECK(crd::perf::parse_authority_list("execute", parsed));
    CHECK(parsed == crd::perf::authority_bit(DiagAuthority::Execute));
    parsed = 7U;

    // Every class but Inject is granted; an Inject command is still refused, and never runs.
    const crd::perf::DiagAuthoritySet all_but_inject =
        crd::perf::kDiagAllAuthorities & ~crd::perf::authority_bit(DiagAuthority::Inject);
    DiagCommandService svc{all_but_inject, DiagServiceConfig{}, &alloc};
    NeverCommand       inject;
    NeverCommand       upload;
    REQUIRE(svc.register_command({"test.inject", "test", "inject", DiagAuthority::Inject, false}, &run_never, &inject));
    const DiagResult refused = svc.execute(request("test.inject"));
    CHECK(refused.status == DiagStatus::Unauthorized);
    CHECK(contains(refused.json, "the command needs inject authority"));
    CHECK(inject.runs == 0U);
    CHECK(svc.handler_runs() == 0U);

    // Read and Record do not imply Upload, RemoteEnable, ProcessMemory or Execute either.
    const crd::perf::DiagAuthoritySet read_record =
        crd::perf::authority_bit(DiagAuthority::Read) | crd::perf::authority_bit(DiagAuthority::Record);
    DiagCommandService narrow{read_record, DiagServiceConfig{}, &alloc};
    REQUIRE(narrow.register_command({"test.upload", "test", "upload", DiagAuthority::Upload, false}, &run_never,
                                    &upload));
    REQUIRE(narrow.register_command({"test.remote", "test", "remote", DiagAuthority::RemoteEnable, false}, &run_never,
                                    &upload));
    REQUIRE(narrow.register_command({"test.memory", "test", "memory", DiagAuthority::ProcessMemory, false},
                                    &run_never, &upload));
    REQUIRE(narrow.register_command({"test.execute", "test", "execute", DiagAuthority::Execute, false}, &run_never,
                                    &upload));
    CHECK(narrow.execute(request("test.upload")).status == DiagStatus::Unauthorized);
    CHECK(narrow.execute(request("test.remote")).status == DiagStatus::Unauthorized);
    CHECK(narrow.execute(request("test.memory")).status == DiagStatus::Unauthorized);
    CHECK(narrow.execute(request("test.execute")).status == DiagStatus::Unauthorized);
    CHECK(upload.runs == 0U);

    // With the class granted, the same command runs.
    DiagCommandService granted{crd::perf::authority_bit(DiagAuthority::Inject), DiagServiceConfig{}, &alloc};
    REQUIRE(granted.register_command({"test.inject", "test", "inject", DiagAuthority::Inject, false}, &run_never,
                                     &inject));
    CHECK(granted.execute(request("test.inject")).status == DiagStatus::Ok);
    CHECK(inject.runs == 1U);

    // Registration refuses malformed declarations and duplicates.
    CHECK_FALSE(svc.register_command({"test.inject", "test", "dup", DiagAuthority::Read, false}, &run_never, &inject));
    CHECK_FALSE(
        svc.register_command({"diag.commands", "test", "dup", DiagAuthority::Read, false}, &run_never, &inject));
    CHECK_FALSE(svc.register_command({"Test.Upper", "test", "bad", DiagAuthority::Read, false}, &run_never, &inject));
    CHECK_FALSE(svc.register_command({"", "test", "bad", DiagAuthority::Read, false}, &run_never, &inject));
    CHECK_FALSE(svc.register_command({"test.none", "test", "bad", DiagAuthority::None, false}, &run_never, &inject));
    CHECK_FALSE(svc.register_command({"test.two", "test", "bad", static_cast<DiagAuthority>(3U), false}, &run_never,
                                     &inject));
    CHECK_FALSE(svc.register_command({"test.null", "test", "bad", DiagAuthority::Read, false}, nullptr, &inject));

    // The listing reports each command's authority and whether this host granted it.
    const DiagResult listing = svc.execute(request("diag.commands"));
    REQUIRE(listing.status == DiagStatus::Ok);
    CHECK(listing.total == 8U); // seven built-ins and test.inject
    CHECK(contains(listing.json, "{\"name\":\"capture.start\",\"owner\":\"perf\",\"authority\":\"record\","
                                 "\"granted\":true,"));
    CHECK(contains(listing.json, "{\"name\":\"test.inject\",\"owner\":\"test\",\"authority\":\"inject\","
                                 "\"granted\":false,"));
    CHECK(contains(listing.json, "\"grant\":\"read,record,remote-enable,upload,process-memory,execute\""));

    // The table is bounded.
    DiagCommandService full{0U, DiagServiceConfig{}, &alloc};
    char               names[crd::perf::kDiagMaxCommands][16];
    crd::u32           added = 0U;
    for (crd::u32 i = 0U; i < crd::perf::kDiagMaxCommands; ++i)
    {
        (void)std::snprintf(names[i], sizeof(names[i]), "test.c%u", i);
        added += full.register_command({names[i], "test", "x", DiagAuthority::Read, false}, &run_never, &upload) ? 1U
                                                                                                                 : 0U;
    }
    CHECK(added == crd::perf::kDiagMaxCommands - 7U);
    CHECK(full.command_count() == crd::perf::kDiagMaxCommands);
}

TEST_CASE("diag commands: a command declaring a second class needs both grants", "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-also"};
    using crd::perf::authority_bit;
    const crd::perf::DiagCommandSpec both{"test.both", "test", "both", DiagAuthority::Execute, false,
                                          DiagAuthority::Record};

    // Either class alone (with Read, for the listing) is refused, naming the missing class; the command never runs.
    NeverCommand cmd;
    for (const DiagAuthority alone : {DiagAuthority::Execute, DiagAuthority::Record})
    {
        DiagCommandService svc{authority_bit(alone) | authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
        REQUIRE(svc.register_command(both, &run_never, &cmd));
        const DiagResult r = svc.execute(request("test.both"));
        CHECK(r.status == DiagStatus::Unauthorized);
        CHECK(contains(r.json, alone == DiagAuthority::Execute ? "the command needs record authority"
                                                               : "the command needs execute authority"));
        const DiagResult listing = svc.execute(request("diag.commands"));
        CHECK(contains(listing.json, "{\"name\":\"test.both\",\"owner\":\"test\",\"authority\":\"execute\","
                                     "\"also\":\"record\",\"granted\":false,"));
    }
    CHECK(cmd.runs == 0U);

    DiagCommandService svc{authority_bit(DiagAuthority::Execute) | authority_bit(DiagAuthority::Record) |
                               authority_bit(DiagAuthority::Read),
                           DiagServiceConfig{}, &alloc};
    REQUIRE(svc.register_command(both, &run_never, &cmd));
    CHECK(svc.execute(request("test.both")).status == DiagStatus::Ok);
    CHECK(cmd.runs == 1U);
    CHECK(contains(svc.execute(request("diag.commands")).json,
                   "\"authority\":\"execute\",\"also\":\"record\",\"granted\":true,"));

    // A second class equal to the first, or not a single known class, is a malformed declaration.
    CHECK_FALSE(svc.register_command({"test.same", "test", "x", DiagAuthority::Read, false, DiagAuthority::Read},
                                     &run_never, &cmd));
    CHECK_FALSE(svc.register_command(
        {"test.pair", "test", "x", DiagAuthority::Read, false, static_cast<DiagAuthority>(6U)}, &run_never, &cmd));

    // A handler sees the host's root, and the service's path rule is the public one.
    DiagServiceConfig rooted;
    rooted.root = cont::StringView{"some/root"};
    DiagCommandService paths{authority_bit(DiagAuthority::Read), rooted, &alloc};
    RootCommand        root_cmd;
    REQUIRE(paths.register_command({"test.root", "test", "root", DiagAuthority::Read, true}, &run_root, &root_cmd));
    DiagRequest r = request("test.root");
    r.path        = cont::StringView{"a/b.txt"};
    CHECK(paths.execute(r).status == DiagStatus::Ok);
    CHECK(cont::StringView{root_cmd.root.data(), root_cmd.root.size()} == "some/root");
    CHECK(crd::perf::diag_path_is_safe("a/b-c_d.e"));
    for (const char* unsafe : {"", "../x", "a/../b", "a//b", "./a", "C:/x", "a\b", "/abs", "a/b/"})
    {
        CHECK_FALSE(crd::perf::diag_path_is_safe(unsafe));
        r.path = cont::StringView{unsafe};
        CHECK(paths.execute(r).status == DiagStatus::BadArgument);
    }
    CHECK(root_cmd.runs == 1U);
}

TEST_CASE("diag commands: named arguments are bounded and checked in the arguments step", "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-arguments"};
    DiagCommandService         svc{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    ArgsCommand                args;
    ItemsCommand               items;
    NeverCommand               record;
    REQUIRE(svc.register_command({"test.args", "test", "named arguments", DiagAuthority::Read, false}, &run_args,
                                 &args, &check_args));
    REQUIRE(svc.register_command({"test.items", "test", "fixed items", DiagAuthority::Read, false}, &run_items,
                                 &items));
    REQUIRE(svc.register_command({"test.record", "test", "record", DiagAuthority::Record, false}, &run_never,
                                 &record, &check_args));

    // A retained snapshot whose cursor the argument refusals below must not reach.
    const crd::perf::DiagArg good[] = {{"line", "16"}, {"mode", "b"}};
    DiagRequest              first  = request("test.args");
    first.args                      = {good, 2U};
    first.page_items                = 1U;
    const DiagResult held           = svc.execute(first);
    REQUIRE(held.status == DiagStatus::Ok);
    CHECK(held.total == 2U);
    CHECK(contains(held.json, "{\"name\":\"line\",\"value\":\"16\"}"));
    REQUIRE(args.checks == 1U);
    REQUIRE(args.runs == 1U);

    cont::String long_name(&alloc);
    long_name.resize(crd::perf::kDiagMaxArgNameBytes + 1U, 'n');
    cont::String long_value(&alloc);
    long_value.resize(crd::perf::kDiagMaxArgValueBytes + 1U, 'v');
    crd::perf::DiagArg many[crd::perf::kDiagMaxArgs + 1U];
    for (crd::u32 i = 0U; i <= crd::perf::kDiagMaxArgs; ++i)
    {
        many[i] = crd::perf::DiagArg{"line", "1"};
    }

    struct Case
    {
        const char*                         name;
        DiagRequest                         req;
        cont::ConstSpan<crd::perf::DiagArg> list;
        DiagStatus                          expected;
        bool                                checked; // the command's own check ran
    };
    const crd::perf::DiagArg on_items[]   = {{"line", "1"}};
    const crd::perf::DiagArg bad_name[]   = {{"Line", "1"}};
    const crd::perf::DiagArg empty_name[] = {{"", "1"}};
    const crd::perf::DiagArg twice[]      = {{"line", "1"}, {"line", "2"}};
    const crd::perf::DiagArg refused[]    = {{"mode", "c"}};
    const crd::perf::DiagArg unknown[]    = {{"depth", "1"}};
    const crd::perf::DiagArg too_long_n[] = {{cont::StringView{long_name.data(), long_name.size()}, "1"}};
    const crd::perf::DiagArg too_long_v[] = {{"line", cont::StringView{long_value.data(), long_value.size()}}};
    Case                     cases[]      = {
        {"arguments on a command without a check", request("test.items"), {on_items, 1U}, DiagStatus::BadArgument,
         false},
        {"too many", request("test.args"), {many, crd::perf::kDiagMaxArgs + 1U}, DiagStatus::Oversized, false},
        {"name over its bound", request("test.args"), {too_long_n, 1U}, DiagStatus::Oversized, false},
        {"value over its bound", request("test.args"), {too_long_v, 1U}, DiagStatus::Oversized, false},
        {"name charset", request("test.args"), {bad_name, 1U}, DiagStatus::BadArgument, false},
        {"empty name", request("test.args"), {empty_name, 1U}, DiagStatus::BadArgument, false},
        {"a name twice", request("test.args"), {twice, 2U}, DiagStatus::BadArgument, false},
        {"the command's check refuses a value", request("test.args"), {refused, 1U}, DiagStatus::BadArgument, true},
        {"the command's check refuses a name", request("test.args"), {unknown, 1U}, DiagStatus::BadArgument, true},
        {"authority beats arguments", request("test.record"), {too_long_v, 1U}, DiagStatus::Unauthorized, false},
        {"arguments beat the cursor", request("test.args"), {refused, 1U}, DiagStatus::BadArgument, true},
        {"arguments beat the cancel flag", request("test.args"), {refused, 1U}, DiagStatus::BadArgument, true},
    };
    cases[10].req.cursor = ((held.generation + 1U) << crd::perf::kDiagCursorOffsetBits) | 1U;
    std::atomic<bool> cancel{true};

    for (crd::usize i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        Case& c = cases[i];
        INFO(c.name);
        c.req.args             = c.list;
        const crd::u64   before = args.checks;
        const DiagResult r      = svc.execute(c.req, i == 11U ? &cancel : nullptr);
        CHECK(r.status == c.expected);
        CHECK(r.items == 0U);
        CHECK_FALSE(r.reason.empty());
        CHECK(args.checks == before + (c.checked ? 1U : 0U));
        CHECK(args.runs == 1U);
        CHECK(items.runs == 0U);
        CHECK(record.runs == 0U);
    }
    DiagRequest refused_mode = request("test.args");
    refused_mode.args        = {refused, 1U};
    CHECK(contains(svc.execute(refused_mode).json, "mode must be a or b"));

    // The refusals left the retained snapshot: its next page still comes from the first run.
    DiagRequest next = request("test.args");
    next.cursor      = held.next_cursor;
    next.page_items  = 1U;
    const DiagResult second = svc.execute(next);
    CHECK(second.status == DiagStatus::Ok);
    CHECK(contains(second.json, "{\"name\":\"mode\",\"value\":\"b\"}"));
    CHECK(args.runs == 1U);

    // A command with a check runs it on a request without arguments too, and the listing says which take arguments.
    const crd::u64 checks_before = args.checks;
    CHECK(svc.execute(request("test.args")).status == DiagStatus::Ok);
    CHECK(args.checks == checks_before + 1U);
    const DiagResult listing = svc.execute(request("diag.commands"));
    CHECK(contains(listing.json, "\"name\":\"test.args\",\"owner\":\"test\",\"authority\":\"read\","
                                 "\"granted\":true,\"takes_path\":false,\"takes_args\":true,"));
    CHECK(contains(listing.json, "\"name\":\"test.items\",\"owner\":\"test\",\"authority\":\"read\","
                                 "\"granted\":true,\"takes_path\":false,\"takes_args\":false,"));
}

TEST_CASE("diag commands: capabilities report the doctor, the grant and the authority classes",
          "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-capabilities"};
    DiagCommandService         svc{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    DiagRequest                req = request("diag.capabilities");
    req.page_items                 = crd::perf::kDiagMaxPageItems;
    req.page_bytes                 = crd::perf::kDiagMaxPageBytes;
    const DiagResult caps          = svc.execute(req);
    REQUIRE(caps.status == DiagStatus::Ok);
    CHECK(caps.complete);
    CHECK(contains(caps.json, "\"schema\":\"cerid-diagnostics/1\""));
    CHECK(contains(caps.json, "\"command_schema\":1"));
    CHECK(contains(caps.json, "\"grant\":\"read\""));
    CHECK(contains(caps.json, CRD_PERF_ENABLED ? "\"profiling_compiled\":true" : "\"profiling_compiled\":false"));
    CHECK(contains(caps.json, "{\"kind\":\"authority\",\"name\":\"read\",\"granted\":true}"));
    CHECK(contains(caps.json, "{\"kind\":\"authority\",\"name\":\"record\",\"granted\":false}"));
    CHECK(contains(caps.json, "{\"kind\":\"authority\",\"name\":\"process-memory\",\"granted\":false}"));
    CHECK(contains(caps.json, "{\"kind\":\"authority\",\"name\":\"execute\",\"granted\":false}"));
    CHECK(contains(caps.json, "{\"kind\":\"mode\",\"name\":"));
    CHECK(contains(caps.json, "{\"kind\":\"dependency\",\"name\":\"sanitizer_runtime\""));
}

TEST_CASE("diag commands: bundle.inspect reads through the bounded importer and refuses a large file unread",
          "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-bundle"};

    crd::perf::BundleManifest man;
    man.schema_version = crd::perf::kDiagnosticSchemaVersion;
    man.absent_tags.push_back(static_cast<crd::u32>(crd::perf::BundleSectionTag::LogTail));
    const cont::Array<crd::u8> manifest = crd::perf::serialize_manifest(man);
    const crd::u8              crash[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    crd::perf::BundleWriter    writer;
    REQUIRE(writer.add_section(crd::perf::BundleSectionTag::Manifest, {manifest.data(), manifest.size()}) ==
            crd::perf::BundleWriter::AddStatus::Ok);
    REQUIRE(writer.add_section(crd::perf::BundleSectionTag::CrashRecord, {crash, sizeof(crash)}) ==
            crd::perf::BundleWriter::AddStatus::Ok);
    REQUIRE(writer.add_absent(crd::perf::BundleSectionTag::LogTail) ==
            crd::perf::BundleWriter::AddStatus::StoredAbsent);
    const cont::Array<crd::u8> bundle = writer.finish(0U);
    const cont::String         path   = scratch_file("diag_commands_bundle.cdb", &alloc);
    write_file(path.c_str(), bundle.data(), bundle.size());
    const cont::String not_bundle = scratch_file("diag_commands_not_bundle.cdb", &alloc);
    const crd::u8      junk[64]   = {'n', 'o', 't', ' ', 'a', ' ', 'b', 'u', 'n', 'd', 'l', 'e'};
    write_file(not_bundle.c_str(), junk, sizeof(junk));

    DiagServiceConfig config;
    config.root = kScratch;
    DiagCommandService svc{crd::perf::authority_bit(DiagAuthority::Read), config, &alloc};

    DiagRequest req = request("bundle.inspect");
    req.path        = "diag_commands_bundle.cdb";
    const DiagResult ok = svc.execute(req);
    REQUIRE(ok.status == DiagStatus::Ok);
    CHECK(svc.file_bytes_read() == bundle.size());
    CHECK(ok.total == 3U);
    CHECK(contains(ok.json, "\"status\":\"ok\",\"reject\":\"none\""));
    CHECK(contains(ok.json, "\"manifest\":true"));
    CHECK(contains(ok.json, "\"tag_name\":\"crash-record\",\"absent\":false"));
    CHECK(contains(ok.json, "\"tag_name\":\"log-tail\",\"absent\":true"));
    CHECK_FALSE(contains(ok.json, kScratch)); // the host's root never appears in the answer

    // Bytes that are not a bundle are inspected and reported rejected, never executed.
    req.path                  = "diag_commands_not_bundle.cdb";
    const DiagResult rejected = svc.execute(req);
    REQUIRE(rejected.status == DiagStatus::Ok);
    CHECK(contains(rejected.json, "\"status\":\"rejected\",\"reject\":\"bad-magic\""));
    CHECK(rejected.total == 0U);

    // A missing file fails after the checks, reading nothing.
    const crd::u64 read_before = svc.file_bytes_read();
    req.path                   = "diag_commands_missing.cdb";
    CHECK(svc.execute(req).status == DiagStatus::Failed);
    CHECK(svc.file_bytes_read() == read_before);

    // A file over the host's limit is refused on its size, before any byte is read.
    DiagServiceConfig small = config;
    small.max_bundle_bytes  = bundle.size() - 1U;
    DiagCommandService limited{crd::perf::authority_bit(DiagAuthority::Read), small, &alloc};
    req.path                   = "diag_commands_bundle.cdb";
    const DiagResult oversized = limited.execute(req);
    CHECK(oversized.status == DiagStatus::Oversized);
    CHECK(limited.handler_runs() == 1U);
    CHECK(limited.file_bytes_read() == 0U);

    (void)std::remove(path.c_str());
    (void)std::remove(not_bundle.c_str());
}

TEST_CASE("diag commands: a capture window records to a new CPROF file only with Record authority",
          "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{16U << 20U, nullptr, "diag-commands-capture"};
    DiagServiceConfig          config;
    config.root = kScratch;
    const cont::String path = scratch_file("diag_commands_capture.cprof", &alloc);
    (void)std::remove(path.c_str());

    DiagCommandService reader{crd::perf::authority_bit(DiagAuthority::Read), config, &alloc};
    CHECK(reader.execute(request("capture.start")).status == DiagStatus::Unauthorized);
    CHECK_FALSE(reader.capture_open());

    DiagCommandService recorder{crd::perf::authority_bit(DiagAuthority::Record), config, &alloc};
    DiagRequest        stop = request("capture.stop");
    stop.path               = "diag_commands_capture.cprof";

#if CRD_PERF_ENABLED
    // Before the profiler exists there is nothing to capture.
    const DiagResult inactive = recorder.execute(request("capture.start"));
    CHECK(inactive.status == DiagStatus::Unavailable);
    CHECK(contains(inactive.json, "the profiler is not initialised"));

    crd::perf::init({});
    CHECK(recorder.execute(stop).status == DiagStatus::Failed); // no window open
    const DiagResult started = recorder.execute(request("capture.start"));
    REQUIRE(started.status == DiagStatus::Ok);
    CHECK(recorder.capture_open());
    CHECK(recorder.capture_window() == 1U);
    CHECK(recorder.execute(request("capture.start")).status == DiagStatus::Failed); // already open
    {
        CRD_PERF_SCOPE("diag-commands-capture-scope");
    }
    const DiagResult stopped = recorder.execute(stop);
    REQUIRE(stopped.status == DiagStatus::Ok);
    CHECK_FALSE(recorder.capture_open());
    CHECK(contains(stopped.json, "\"valid\":true"));
    CHECK(contains(stopped.json, "\"samples_complete\":true"));
    const cont::Array<crd::u8> written = crd::perf::load_capture_from_file(path.c_str(), &alloc);
    CHECK(crd::perf::validate_capture_buffer({written.data(), written.size()}));

    // A second window never overwrites the first file.
    REQUIRE(recorder.execute(request("capture.start")).status == DiagStatus::Ok);
    const DiagResult again = recorder.execute(stop);
    CHECK(again.status == DiagStatus::Failed);
    CHECK(contains(again.json, "refusing to overwrite"));
    CHECK(recorder.capture_open());
    crd::perf::shutdown();
#else
    // Profiling compiled out: both commands say so rather than pretending.
    const DiagResult started = recorder.execute(request("capture.start"));
    CHECK(started.status == DiagStatus::Unavailable);
    CHECK(contains(started.json, "profiling is compiled out"));
    CHECK(recorder.execute(stop).status == DiagStatus::Unavailable);
    DiagCommandService alloc_reader{crd::perf::authority_bit(DiagAuthority::Read), config, &alloc};
    CHECK(alloc_reader.execute(request("memory.allocators")).status == DiagStatus::Unavailable);
#endif
    (void)std::remove(path.c_str());
}

#if CRD_PERF_ENABLED
TEST_CASE("diag commands: memory.allocators lists the profiler's registered allocators", "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-allocators"};
    DiagCommandService         svc{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    CHECK(svc.execute(request("memory.allocators")).status == DiagStatus::Unavailable);

    crd::perf::init({});
    crd::memory::TlsfAllocator tracked{1U << 16U, nullptr, "diag-commands-tracked"};
    const crd::u32             slot = crd::perf::register_allocator("diag-commands-tracked", &tracked);
    REQUIRE(slot != crd::perf::kInvalidAllocatorIdx);
    void* block = tracked.allocate(256U, 16U);
    REQUIRE(block != nullptr);
    const DiagResult listed = svc.execute(request("memory.allocators"));
    REQUIRE(listed.status == DiagStatus::Ok);
    CHECK(listed.total >= 1U);
    CHECK(contains(listed.json, "\"name\":\"diag-commands-tracked\",\"allocations\":1,"));
    tracked.deallocate(block);
    crd::perf::unregister_allocator(slot);
    crd::perf::shutdown();
}
#endif

namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) -- job callbacks are plain function pointers.
std::atomic<bool>     g_gate{false};
std::atomic<crd::u64> g_child_task{0U};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void gated_child(void* /*data*/) noexcept
{
    g_child_task.store(crd::jobs::current_task_id(), std::memory_order_relaxed);
    while (!g_gate.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

void parking_root(void* /*data*/) noexcept
{
    crd::jobs::JobDecl child{};
    child.fn = &gated_child;
    crd::jobs::run_and_wait(child);
}
} // namespace

TEST_CASE("diag commands: jobs.waits reports a parked fiber's wait edge and the workers", "[perf][diag][commands]")
{
    crd::memory::TlsfAllocator alloc{8U << 20U, nullptr, "diag-commands-jobs"};
    DiagCommandService         svc{crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &alloc};
    const DiagResult           none = svc.execute(request("jobs.waits"));
    CHECK(none.status == DiagStatus::Unavailable);
    CHECK(contains(none.json, "the job pool is not initialised"));

    g_gate.store(false, std::memory_order_relaxed);
    g_child_task.store(0U, std::memory_order_relaxed);
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl root{};
    root.fn                    = &parking_root;
    crd::jobs::Counter* handle = crd::jobs::run(root);

    DiagResult waits{&alloc};
    bool       parked   = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!parked && std::chrono::steady_clock::now() < deadline)
    {
        waits  = svc.execute(request("jobs.waits"));
        parked = waits.status == DiagStatus::Ok && contains(waits.json, "\"parked\":1,") &&
                 g_child_task.load(std::memory_order_relaxed) != 0U;
        if (!parked)
        {
            std::this_thread::yield();
        }
    }
    g_gate.store(true, std::memory_order_release);
    crd::jobs::wait(handle);

    REQUIRE(parked);
    char edge[64];
    (void)std::snprintf(edge, sizeof(edge), "\"waiting_on\":%llu,",
                        static_cast<unsigned long long>(g_child_task.load(std::memory_order_relaxed)));
    CHECK(contains(waits.json, edge));
    CHECK(contains(waits.json, "\"remaining\":1}"));
    CHECK(contains(waits.json, "\"workers\":4,"));
    CHECK(contains(waits.json, "{\"kind\":\"worker\",\"thread\":0,"));
    crd::jobs::shutdown();
}
