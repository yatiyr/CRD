// DIAG.8c -- the program-inspection diagnostic command. `program.inspect` is registered into crd-perf's command service
// under its own authority class (Execute) and runs the committed authored program assets/ceir/inspect_demo.ceir to a
// script given as named arguments: breakpoints, watched lines and the step at each stop. The answer is items in the
// service's bounded, paginated document: how each breakpoint bound, every stop at its authored file:line:col with one
// item per watched value, and the results. Refusals (authority, malformed or oversized arguments, an unsafe path, a
// raised cancel) run nothing and read nothing; an oversized file is refused unread; a source that does not cook is
// refused with its line and column. The stop bound truncates; a scripted cancel ends the run; the caller's cancel flag
// ends it at a stop and while it runs. DIAG.9a: `seed=` gives the run a seeded host random source, whose draws the
// held run reads (assets/ceir/random_demo.ceir); without it a draw fails where it is authored. Expected lines come from
// scanning the committed text, never from the parser. `clock=`, `sim_time=` and `sim_step=` give the run a host clock
// (assets/ceir/clock_demo.ceir): the held run reads the step it was given, a live wall lets it finish, and a domain the
// request leaves unset fails its read where it is authored. ASCII test names (ctest by-name).

#include <crd/ceir/cook/inspect_diag.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/input.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <thread>
#include <utility>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::cook::ProgramInspectCommand;
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagArg;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;

constexpr const char* kFile        = "ceir/inspect_demo.ceir";
constexpr const char* kAssets      = CRD_REPO_DIR "/assets";
constexpr const char* kAsset       = CRD_REPO_DIR "/assets/ceir/inspect_demo.ceir";
constexpr const char* kRandomFile  = "ceir/random_demo.ceir"; // DIAG.9a
constexpr const char* kRandomAsset = CRD_REPO_DIR "/assets/ceir/random_demo.ceir";
constexpr const char* kClockFile   = "ceir/clock_demo.ceir"; // DIAG.9a
constexpr const char* kClockAsset  = CRD_REPO_DIR "/assets/ceir/clock_demo.ceir";

// This case's own scratch file in the working directory (the file root ".").
constexpr const char* kBrokenFile = "diag8c_inspect_broken.ceir";

void registrar(Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
    (void)crd::ceir::input::register_input_ops(ctx); // DIAG.9a: random_demo.ceir
}

String slurp(const char* path, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    String buf(a);
    buf.resize(static_cast<usize>(sz));
    f.read(buf.data(), sz);
    return buf;
}

void spill(const char* path, const String& text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(f.good());
}

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

bool has(StringView hay, StringView needle)
{
    return hay.find(needle) != StringView::npos;
}

u32 count(StringView hay, StringView needle)
{
    u32   n   = 0U;
    usize pos = hay.find(needle);
    while (pos != StringView::npos)
    {
        ++n;
        pos = hay.find(needle, pos + needle.size());
    }
    return n;
}

// The independent oracle: an authored line by the n-th occurrence of `needle`, and its first non-blank column.
struct Site
{
    u32 line = 0U;
    u32 col  = 0U;
};
Site site_of(StringView text, StringView needle, u32 nth)
{
    u32   line  = 1U;
    u32   seen  = 0U;
    usize start = 0U;
    while (start < text.size())
    {
        usize end = text.find('\n', start);
        if (end == StringView::npos)
        {
            end = text.size();
        }
        const StringView l = text.substr(start, end - start);
        usize            at = l.find(needle);
        while (at != StringView::npos)
        {
            if (seen++ == nth)
            {
                usize indent = 0U;
                while (indent < l.size() && l[indent] == ' ')
                {
                    ++indent;
                }
                return Site{line, static_cast<u32>(indent + 1U)};
            }
            at = l.find(needle, at + needle.size());
        }
        ++line;
        start = end + 1U;
    }
    return Site{};
}

struct Lines
{
    Site h;    // the callee's multiply
    Site len;  // the quantity constant
    Site a;    // the first add
    Site call; // the call
    Site loop; // the loop
    Site x;    // the loop body's add
};

Lines scan(StringView text)
{
    Lines l;
    l.h    = site_of(text, "arith.muli", 0U);
    l.len  = site_of(text, "qty<", 0U);
    l.a    = site_of(text, "arith.addi", 0U);
    l.call = site_of(text, "func.call", 0U);
    l.loop = site_of(text, "core.for", 0U);
    l.x    = site_of(text, "arith.addi", 2U);
    REQUIRE(l.h.line != 0U);
    REQUIRE(l.len.line != 0U);
    REQUIRE(l.a.line != 0U);
    REQUIRE(l.call.line != 0U);
    REQUIRE(l.loop.line != 0U);
    REQUIRE(l.x.line != 0U);
    return l;
}

// `pattern` with each `%u` replaced, in order, by the next of `values` in decimal (the expected JSON fragment).
String fragment(const char* pattern, std::initializer_list<u64> values, crd::memory::IAllocator* a)
{
    String      s(a);
    const u64*  next = values.begin();
    const char* p    = pattern;
    while (*p != '\0')
    {
        if (p[0] == '%' && p[1] == 'u')
        {
            REQUIRE(next != values.end());
            char digits[24];
            (void)std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(*next++));
            s.append(digits);
            p += 2;
            continue;
        }
        s.push_back(*p++);
    }
    REQUIRE(next == values.end());
    return s;
}

String join(std::initializer_list<u64> values, crd::memory::IAllocator* a)
{
    String s(a);
    for (const u64 v : values)
    {
        if (!s.empty())
        {
            s.push_back(',');
        }
        char digits[24];
        (void)std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(v));
        s.append(digits);
    }
    return s;
}

// The page's items, in order: each object of the "items" array.
Array<String> items_of(const DiagResult& r, crd::memory::IAllocator* a)
{
    Array<String>    out(a);
    const StringView json = view(r.json);
    const usize      at   = json.find("\"items\":[");
    REQUIRE(at != StringView::npos);
    usize pos = json.find("{\"kind\":", at);
    while (pos != StringView::npos)
    {
        const usize next = json.find("{\"kind\":", pos + 1U);
        const usize stop = next == StringView::npos ? json.size() - 2U : next - 1U; // drop "]}" or the comma
        String      item(a);
        item.append(json.substr(pos, stop - pos));
        out.push_back(std::move(item));
        pos = next;
    }
    return out;
}

DiagRequest inspect_request(const char* path, crd::containers::ConstSpan<DiagArg> args)
{
    DiagRequest r;
    r.command    = crd::ceir::cook::kProgramInspectCommand;
    r.path       = StringView{path};
    r.args       = args;
    r.page_items = crd::perf::kDiagMaxPageItems;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return r;
}

DiagServiceConfig rooted(const char* root)
{
    DiagServiceConfig c;
    c.root = StringView{root};
    return c;
}

constexpr crd::perf::DiagAuthoritySet kExecute = crd::perf::authority_bit(DiagAuthority::Execute);

// The stop observer the at-a-stop cancel uses: raises the caller's flag at the first stop.
void raise_at_stop(void* user, u64 /*sequence*/)
{
    static_cast<std::atomic<bool>*>(user)->store(true, std::memory_order_release);
}

// DIAG.9a: the committed random_demo.ceir draws main(4) values from stream 0 and switches on each (a draw of 3 has no
// case). Which seed fails, and at which draw, is computed from SeededInputs::draw, independently of the executor.
constexpr u32 kRandomDraws = 4U;

crd::i64 random_switch_draw(u64 seed, u64 n)
{
    return crd::ceir::input::reduce_draw(crd::ceir::input::SeededInputs::draw(seed, 0U, n), 4U);
}

// The first seed whose main(4) fails (`fails`) or finishes; `failing` gets the failing draw's index.
u64 random_seed_where(bool fails, u32& failing)
{
    for (u64 seed = 1U; seed < 10000U; ++seed)
    {
        failing = kRandomDraws;
        for (u32 i = 0U; i < kRandomDraws && failing == kRandomDraws; ++i)
        {
            if (random_switch_draw(seed, i) == 3)
            {
                failing = i;
            }
        }
        if ((failing < kRandomDraws) == fails)
        {
            return seed;
        }
    }
    FAIL("no seed found");
    return 0U;
}
} // namespace

TEST_CASE("diag 8c: program.inspect runs an authored program to its script under Execute", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = slurp(kAsset, &root);
    const Lines                        ln   = scan(view(text));

    ProgramInspectCommand cmd;
    cmd.registrar = &registrar;
    DiagCommandService svc(kExecute, rooted(kAssets), &root);
    REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
    CHECK_FALSE(crd::ceir::cook::register_program_inspect(svc, cmd)); // one name, one command

    const String  breaks  = join({ln.call.line}, &root);
    const String  watches = join({ln.a.line, ln.len.line, ln.call.line, ln.x.line}, &root);
    const DiagArg args[]  = {
        {"args", "3"}, {"breaks", view(breaks)}, {"watches", view(watches)}, {"steps", "into,out"}};
    const DiagResult r = svc.execute(inspect_request(kFile, {args, 4U}));
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(r.complete);
    CHECK(has(view(r.json), R"("path":"ceir/inspect_demo.ceir","entry":"main",)"));
    CHECK(has(view(r.json), R"("breakpoints":1,"stops":3,"max_stops":64,"random_source":"none","seed":0,)"
                            R"("wall_clock":"none","sim_time":"none","sim_time_ns":0,"sim_step":"none",)"
                            R"("sim_step_ns":0,"truncated":false,"outcome":"finished",)"));
    CHECK(has(view(r.json), R"("error":"none","fault_line":0,"fault_col":0,"results":1})"));
    CHECK_FALSE(has(view(r.json), StringView{CRD_REPO_DIR})); // the host's root never reaches the answer

    // One breakpoint item, three stops with four value items each, one result.
    const Array<String> items = items_of(r, &root);
    REQUIRE(items.size() == 1U + 3U * 5U + 1U);
    CHECK(view(items[0]) ==
          view(fragment(R"({"kind":"breakpoint","line":%u,"status":"bound","sites":1})", {ln.call.line}, &root)));

    // Stop 1, at the breakpoint: the caller's values are available (a quantity with its unit), the call's own result
    // is not yet computed and the loop body's value is out of scope.
    CHECK(has(view(items[1]), view(fragment(R"({"kind":"stop","sequence":1,"reason":"breakpoint",)"
                                            R"("file":"ceir/inspect_demo.ceir","line":%u,"col":%u,"depth":0,"op":)",
                                            {ln.call.line, ln.call.col}, &root))));
    CHECK(has(view(items[1]), R"("action":"into"})"));
    CHECK(view(items[2]) == view(fragment(R"({"kind":"value","stop":1,"line":%u,"status":"available","type":"!i32",)"
                                          R"("unit":false,"value":3})",
                                          {ln.a.line}, &root)));
    CHECK(view(items[3]) == view(fragment(R"({"kind":"value","stop":1,"line":%u,"status":"available",)"
                                          R"("type":"!qty<!i32,L1>","unit":true,"value":7})",
                                          {ln.len.line}, &root)));
    CHECK(view(items[4]) == view(fragment(R"({"kind":"value","stop":1,"line":%u,"status":"not-yet-computed",)"
                                          R"("type":"!i32","unit":false})",
                                          {ln.call.line}, &root)));
    CHECK(view(items[5]) == view(fragment(R"({"kind":"value","stop":1,"line":%u,"status":"out-of-scope",)"
                                          R"("type":"!i32","unit":false})",
                                          {ln.x.line}, &root)));

    // Stop 2, stepped into the callee: its first instr, one frame down; the caller's value is out of its scope.
    CHECK(has(view(items[6]), view(fragment(R"({"kind":"stop","sequence":2,"reason":"step",)"
                                            R"("file":"ceir/inspect_demo.ceir","line":%u,"col":%u,"depth":1,)",
                                            {ln.h.line, ln.h.col}, &root))));
    CHECK(has(view(items[6]), R"("action":"out"})"));
    CHECK(view(items[7]) == view(fragment(R"({"kind":"value","stop":2,"line":%u,"status":"out-of-scope",)"
                                          R"("type":"!i32","unit":false})",
                                          {ln.a.line}, &root)));

    // Stop 3, stepped out: back in main at the loop, with the call's result now available.
    CHECK(has(view(items[11]), view(fragment(R"({"kind":"stop","sequence":3,"reason":"step",)"
                                             R"("file":"ceir/inspect_demo.ceir","line":%u,"col":%u,"depth":0,)",
                                             {ln.loop.line, ln.loop.col}, &root))));
    CHECK(has(view(items[11]), R"("action":"continue"})"));
    CHECK(view(items[14]) == view(fragment(R"({"kind":"value","stop":3,"line":%u,"status":"available",)"
                                           R"("type":"!i32","unit":false,"value":36})",
                                           {ln.call.line}, &root)));
    CHECK(view(items[16]) == view(String(R"({"kind":"result","index":0,"value":36})", &root)));
    CHECK(cmd.runs.load() == 1U);
    CHECK(cmd.starts.load() == 1U);
    CHECK(cmd.bytes_read.load() == text.size());

    // Pages are cut from one run's snapshot: three items a page walk the same items, and a cursor repeats its bytes.
    DiagRequest next = inspect_request(kFile, {args, 4U});
    next.page_items  = 3U;
    u64   cursor     = 0U;
    usize seen       = 0U;
    do
    {
        next.cursor            = cursor;
        const DiagResult page  = svc.execute(next);
        REQUIRE(page.status == DiagStatus::Ok);
        const Array<String> got = items_of(page, &root);
        for (usize k = 0; k < got.size(); ++k)
        {
            CHECK(view(got[k]) == view(items[seen + k]));
        }
        if (cursor != 0U)
        {
            CHECK(view(svc.execute(next).json) == view(page.json));
        }
        seen += got.size();
        cursor = page.next_cursor;
    } while (cursor != 0U);
    CHECK(seen == items.size());
    CHECK(cmd.runs.load() == 2U); // the walk ran the program once more, then paged its snapshot
}

TEST_CASE("diag 8c: program.inspect refuses before any work and names where a source failed", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = slurp(kAsset, &root);
    const Lines                        ln   = scan(view(text));

    SECTION("authority, arguments, the path and a raised cancel never reach the command")
    {
        ProgramInspectCommand cmd;
        cmd.registrar = &registrar;
        const crd::perf::DiagAuthoritySet all_but_execute = crd::perf::kDiagAllAuthorities & ~kExecute;
        DiagCommandService                read(crd::perf::authority_bit(DiagAuthority::Read), rooted(kAssets), &root);
        DiagCommandService                others(all_but_execute, rooted(kAssets), &root);
        DiagCommandService                svc(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(read, cmd));
        REQUIRE(crd::ceir::cook::register_program_inspect(others, cmd));
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));

        // No other class, together or alone, implies Execute.
        const DiagArg three[] = {{"args", "3"}};
        CHECK(read.execute(inspect_request(kFile, {three, 1U})).status == DiagStatus::Unauthorized);
        const DiagResult refused = others.execute(inspect_request(kFile, {three, 1U}));
        CHECK(refused.status == DiagStatus::Unauthorized);
        CHECK(has(view(refused.json), "the command needs execute authority"));

        struct Bad
        {
            const char* name;
            DiagArg     arg;
            DiagStatus  expected;
        };
        String long_value(&root);
        long_value.resize(crd::perf::kDiagMaxArgValueBytes + 1U, '1');
        const Bad bad[] = {
            {"unknown step", {"steps", "into,jump"}, DiagStatus::BadArgument},
            {"line zero", {"breaks", "0"}, DiagStatus::BadArgument},
            {"line not a number", {"watches", "x"}, DiagStatus::BadArgument},
            {"empty list item", {"breaks", "3,,4"}, DiagStatus::BadArgument},
            {"fractional argument", {"args", "1.5"}, DiagStatus::BadArgument},
            {"argument overflow", {"args", "9223372036854775808"}, DiagStatus::BadArgument},
            {"no stops", {"max_stops", "0"}, DiagStatus::BadArgument},
            {"stops over the host's limit", {"max_stops", "257"}, DiagStatus::BadArgument},
            {"entry name", {"entry", "main()"}, DiagStatus::BadArgument},
            {"unknown name", {"verbose", "1"}, DiagStatus::BadArgument},
            {"name charset", {"Breaks", "3"}, DiagStatus::BadArgument},
            {"seed not a number", {"seed", "x"}, DiagStatus::BadArgument},
            {"negative seed", {"seed", "-1"}, DiagStatus::BadArgument},
            {"seed overflow", {"seed", "18446744073709551616"}, DiagStatus::BadArgument},
            {"clock not wall", {"clock", "frame"}, DiagStatus::BadArgument},
            {"sim time not a number", {"sim_time", "x"}, DiagStatus::BadArgument},
            {"fractional sim step", {"sim_step", "1.5"}, DiagStatus::BadArgument},
            {"sim time overflow", {"sim_time", "9223372036854775808"}, DiagStatus::BadArgument},
            {"value over its bound", {"args", view(long_value)}, DiagStatus::Oversized},
        };
        for (const Bad& b : bad)
        {
            INFO(b.name);
            const DiagResult res = svc.execute(inspect_request(kFile, {&b.arg, 1U}));
            CHECK(res.status == b.expected);
            CHECK(res.items == 0U);
        }
        const DiagArg twice[] = {{"breaks", "3"}, {"breaks", "4"}};
        CHECK(svc.execute(inspect_request(kFile, {twice, 2U})).status == DiagStatus::BadArgument);
        DiagArg many[crd::perf::kDiagMaxArgs + 1U];
        for (DiagArg& m : many)
        {
            m = DiagArg{"args", "1"};
        }
        CHECK(svc.execute(inspect_request(kFile, {many, crd::perf::kDiagMaxArgs + 1U})).status ==
              DiagStatus::Oversized);
        CHECK(svc.execute(inspect_request("../assets/ceir/inspect_demo.ceir", {})).status == DiagStatus::BadArgument);
        std::atomic<bool> cancel{true};
        CHECK(svc.execute(inspect_request(kFile, {}), &cancel).status == DiagStatus::Cancelled);

        CHECK(cmd.runs.load() == 0U);
        CHECK(cmd.bytes_read.load() == 0U);
        CHECK(cmd.starts.load() == 0U);
    }

    SECTION("a program over the host's limit is refused without reading a byte")
    {
        ProgramInspectCommand cmd;
        cmd.registrar         = &registrar;
        cmd.max_program_bytes = 64U;
        DiagCommandService svc(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
        const DiagResult r = svc.execute(inspect_request(kFile, {}));
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Oversized);
        CHECK(has(view(r.json), "the host's limit is 64"));
        CHECK(cmd.bytes_read.load() == 0U);
        CHECK(cmd.starts.load() == 0U);
    }

    SECTION("a source that does not cook is refused at its line and column; a missing entry is named")
    {
        const usize cut = view(text).find("arith.muli");
        REQUIRE(cut != StringView::npos);
        String broken(&root);
        broken.append(view(text).substr(0U, cut));
        broken.append("arith.muli((");
        broken.append(view(text).substr(cut + std::strlen("arith.muli")));
        spill(kBrokenFile, broken);

        ProgramInspectCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService svc(kExecute, rooted("."), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
        const DiagResult r = svc.execute(inspect_request(kBrokenFile, {}));
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Failed);
        String where(&root);
        where.append(" at ");
        where.append(kBrokenFile);
        where.append(view(fragment(":%u:", {ln.h.line}, &root)));
        CHECK(has(view(r.json), "the program did not load: cook-failed ("));
        CHECK(has(view(r.json), view(where)));
        CHECK(cmd.starts.load() == 0U);
        (void)std::remove(kBrokenFile);

        DiagCommandService assets(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(assets, cmd));
        const DiagArg    entry[] = {{"entry", "no_such_entry"}};
        const DiagResult missing = assets.execute(inspect_request(kFile, {entry, 1U}));
        CHECK(missing.status == DiagStatus::Failed);
        CHECK(has(view(missing.json), "the program did not load: compile-failed (no-entry)"));
    }
}

TEST_CASE("diag 8c: program.inspect bounds its stops and stops when cancelled", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = slurp(kAsset, &root);
    const Lines                        ln   = scan(view(text));
    const String                       x    = join({ln.x.line}, &root);

    SECTION("the stop bound cancels the run and the answer says truncated")
    {
        ProgramInspectCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService svc(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
        const DiagArg    args[] = {{"args", "5"}, {"breaks", view(x)}, {"max_stops", "2"}};
        const DiagResult r      = svc.execute(inspect_request(kFile, {args, 3U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), R"("stops":2,"max_stops":2,"random_source":"none","seed":0,"wall_clock":"none",)"
                                R"("sim_time":"none","sim_time_ns":0,"sim_step":"none","sim_step_ns":0,)"
                                R"("truncated":true,"outcome":"cancelled",)"));
        CHECK(count(view(r.json), view(fragment(R"("reason":"breakpoint","file":"ceir/inspect_demo.ceir","line":%u,)",
                                                {ln.x.line}, &root))) == 2U);
        CHECK_FALSE(has(view(r.json), R"("kind":"result")"));
    }

    SECTION("a scripted cancel ends the run at its stop")
    {
        ProgramInspectCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService svc(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
        const DiagArg    args[] = {{"args", "5"}, {"breaks", view(x)}, {"steps", "continue,cancel"}};
        const DiagResult r      = svc.execute(inspect_request(kFile, {args, 3U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), R"("stops":2,"max_stops":64,"random_source":"none","seed":0,"wall_clock":"none",)"
                                R"("sim_time":"none","sim_time_ns":0,"sim_step":"none","sim_step_ns":0,)"
                                R"("truncated":false,"outcome":"cancelled",)"));
        CHECK(has(view(r.json), R"("sequence":2,)"));
        CHECK(has(view(r.json), R"("action":"cancel"})"));
    }

    SECTION("the caller's cancel raised at a stop ends the run at its next safe point and answers cancelled")
    {
        std::atomic<bool>     cancel{false};
        ProgramInspectCommand cmd;
        cmd.registrar = &registrar;
        cmd.on_stop   = &raise_at_stop;
        cmd.stop_user = &cancel;
        DiagCommandService svc(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
        const DiagArg    args[] = {{"args", "5"}, {"breaks", view(x)}};
        const DiagResult r      = svc.execute(inspect_request(kFile, {args, 2U}), &cancel);
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Cancelled);
        CHECK(has(view(r.json), "cancelled by the caller after 1 stops"));
        CHECK(cmd.starts.load() == 1U);

        // The service is not left holding anything: the next request runs to the end.
        cancel.store(false);
        cmd.on_stop = nullptr;
        const DiagResult again = svc.execute(inspect_request(kFile, {args, 2U}), &cancel);
        CHECK(again.status == DiagStatus::Ok);
        CHECK(has(view(again.json), R"("stops":5,)"));
    }

    SECTION("the caller's cancel while the program runs between stops ends it")
    {
        // A loop far longer than the host's wait bound and no breakpoint: only the cancel can end it in time.
        std::atomic<bool>     cancel{false};
        ProgramInspectCommand cmd;
        cmd.registrar = &registrar;
        cmd.wait_ms   = 5000U;
        DiagCommandService svc(kExecute, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
        std::atomic<bool> answered{false}; // a request refused before the run must not leave the raiser waiting
        std::thread       raiser(
            [&]
            {
                while (cmd.starts.load(std::memory_order_acquire) == 0U && !answered.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                cancel.store(true, std::memory_order_release);
            });
        const DiagArg    args[] = {{"args", "1000000000000"}};
        const DiagResult r      = svc.execute(inspect_request(kFile, {args, 1U}), &cancel);
        answered.store(true, std::memory_order_release);
        raiser.join();
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Cancelled);
        CHECK(has(view(r.json), "cancelled by the caller after 0 stops"));
    }
}

TEST_CASE("diag 9a: program.inspect reads the draws of the seed it is given", "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = slurp(kRandomAsset, &root);
    const Site                         draw = site_of(view(text), "input.random() {stream = 0", 0U);
    const Site                         sw   = site_of(view(text), "core.switch", 0U);
    REQUIRE(draw.line != 0U);
    REQUIRE(sw.line != 0U);

    ProgramInspectCommand cmd;
    cmd.registrar = &registrar;
    DiagCommandService svc(kExecute, rooted(kAssets), &root);
    REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));
    const String breaks  = join({sw.line}, &root);
    const String watches = join({draw.line}, &root);

    SECTION("a failing seed: each held switch reads the seeded draw, and the run fails at the switch")
    {
        u32          k    = 0U;
        const u64    seed = random_seed_where(true, k);
        const String s    = join({seed}, &root);
        INFO("seed " << seed << " fails at draw " << k);
        const DiagArg args[] = {{"args", "4"}, {"breaks", view(breaks)}, {"watches", view(watches)}, {"seed", view(s)}};
        const DiagResult r   = svc.execute(inspect_request(kRandomFile, {args, 4U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), view(fragment(R"("random_source":"seeded","seed":%u,)", {seed}, &root))));
        CHECK(has(view(r.json), R"("outcome":"error","error":"selector-out-of-range",)"));
        CHECK(has(view(r.json), view(fragment(R"("fault_line":%u,"fault_col":%u,)", {sw.line, sw.col}, &root))));
        const Array<String> items = items_of(r, &root);
        REQUIRE(items.size() == 1U + 2U * (k + 1U));
        for (u32 n = 0U; n <= k; ++n)
        {
            const u64 v = static_cast<u64>(random_switch_draw(seed, n));
            CHECK(view(items[2U + 2U * n]) ==
                  view(fragment(R"({"kind":"value","stop":%u,"line":%u,"status":"available","type":"!i32",)"
                                R"("unit":false,"value":%u})",
                                {n + 1U, draw.line, v}, &root)));
        }
    }
    SECTION("a passing seed returns the seeded stream-1 draw")
    {
        u32           k      = 0U;
        const u64     seed   = random_seed_where(false, k);
        const String  s      = join({seed}, &root);
        const DiagArg args[] = {{"args", "4"}, {"seed", view(s)}};
        const DiagResult r   = svc.execute(inspect_request(kRandomFile, {args, 2U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const u64 value =
            static_cast<u64>(crd::ceir::input::reduce_draw(crd::ceir::input::SeededInputs::draw(seed, 1U, 0U), 100U));
        CHECK(has(view(r.json), view(fragment(R"({"kind":"result","index":0,"value":%u})", {value}, &root))));
        CHECK(has(view(r.json), R"("outcome":"finished","error":"none",)"));
    }
    SECTION("no seed: the host has no random source and the first draw fails where it is authored")
    {
        const DiagArg    args[] = {{"args", "4"}};
        const DiagResult r      = svc.execute(inspect_request(kRandomFile, {args, 1U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), R"("random_source":"none","seed":0,)"));
        CHECK(has(view(r.json), R"("outcome":"error","error":"input-unavailable",)"));
        CHECK(has(view(r.json), view(fragment(R"("fault_line":%u,"fault_col":%u,)", {draw.line, draw.col}, &root))));
    }
}

TEST_CASE("diag 9a clock: program.inspect reads the host clock it is given", "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = slurp(kClockAsset, &root);
    const Site                         step = site_of(view(text), R"(input.time_step() {domain = "sim"})", 0U);
    const Site                         wall = site_of(view(text), R"(input.clock() {domain = "wall"})", 0U);
    const Site                         simr = site_of(view(text), R"(input.clock() {domain = "sim"})", 0U);
    const Site                         sum  = site_of(view(text), "%6 = arith.addi", 0U);
    const Site                         sw   = site_of(view(text), "core.switch", 0U);
    REQUIRE(step.line != 0U);
    REQUIRE(wall.line != 0U);
    REQUIRE(simr.line != 0U);
    REQUIRE(sum.line != 0U);
    REQUIRE(sw.line != 0U);
    constexpr u64 hitch_ns    = 50000000U; // over the program's 33,333,333 ns budget: the switch has no case for it
    constexpr u64 steady_ns   = 16666667U;
    constexpr u64 sim_time_ns = 4000000000U;

    ProgramInspectCommand cmd;
    cmd.registrar = &registrar;
    DiagCommandService svc(kExecute, rooted(kAssets), &root);
    REQUIRE(crd::ceir::cook::register_program_inspect(svc, cmd));

    SECTION("a step over budget: the held switch reads the given step, and the run fails there")
    {
        const String     breaks  = join({sw.line}, &root);
        const String     watches = join({step.line}, &root);
        const String     hitch   = join({hitch_ns}, &root);
        const DiagArg    args[]  = {
            {"args", "7"}, {"breaks", view(breaks)}, {"watches", view(watches)}, {"sim_step", view(hitch)}};
        const DiagResult r = svc.execute(inspect_request(kClockFile, {args, 4U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), view(fragment(R"("wall_clock":"none","sim_time":"none","sim_time_ns":0,)"
                                              R"("sim_step":"set","sim_step_ns":%u,)",
                                              {hitch_ns}, &root))));
        CHECK(has(view(r.json), R"("outcome":"error","error":"selector-out-of-range",)"));
        CHECK(has(view(r.json), view(fragment(R"("fault_line":%u,"fault_col":%u,)", {sw.line, sw.col}, &root))));
        const Array<String> items = items_of(r, &root);
        REQUIRE(items.size() == 3U); // the breakpoint, its one stop and the watched step
        CHECK(view(items[2]) == view(fragment(R"({"kind":"value","stop":1,"line":%u,"status":"available",)"
                                              R"("type":"!i64","unit":false,"value":%u})",
                                              {step.line, hitch_ns}, &root)));
    }
    SECTION("a step within budget, a sim reading and the live wall: the run finishes on their sum")
    {
        const String     breaks   = join({sum.line}, &root);
        const String     watches  = join({simr.line}, &root);
        const String     steady   = join({steady_ns}, &root);
        const String     sim_time = join({sim_time_ns}, &root);
        const DiagArg    args[]   = {{"args", "7"},     {"breaks", view(breaks)},     {"watches", view(watches)},
                                     {"clock", "wall"}, {"sim_time", view(sim_time)}, {"sim_step", view(steady)}};
        const DiagResult r        = svc.execute(inspect_request(kClockFile, {args, 6U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), view(fragment(R"("wall_clock":"live","sim_time":"set","sim_time_ns":%u,)"
                                              R"("sim_step":"set","sim_step_ns":%u,)",
                                              {sim_time_ns, steady_ns}, &root))));
        CHECK(has(view(r.json), R"("outcome":"finished","error":"none",)"));
        const Array<String> items = items_of(r, &root);
        REQUIRE(items.size() == 4U); // the breakpoint, its stop, the watched sim reading and the result
        CHECK(view(items[2]) == view(fragment(R"({"kind":"value","stop":1,"line":%u,"status":"available",)"
                                              R"("type":"!i64","unit":false,"value":%u})",
                                              {simr.line, sim_time_ns}, &root)));
        // The result is the wall reading (nanoseconds since the run's clock was made) plus the sim reading plus 7.
        const StringView result = view(items[3]);
        const StringView key    = R"("value":)";
        const usize      at     = result.find(key);
        REQUIRE(at != StringView::npos);
        u64 total = 0U;
        for (usize i = at + key.size(); i < result.size() && result[i] >= '0' && result[i] <= '9'; ++i)
        {
            total = total * 10U + static_cast<u64>(result[i] - '0');
        }
        CHECK(total >= sim_time_ns + 7U);
    }
    SECTION("no wall: the step passes and the wall read fails where it is authored")
    {
        const String     steady = join({steady_ns}, &root);
        const DiagArg    args[] = {{"args", "7"}, {"sim_step", view(steady)}};
        const DiagResult r      = svc.execute(inspect_request(kClockFile, {args, 2U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), R"("outcome":"error","error":"input-unavailable",)"));
        CHECK(has(view(r.json), view(fragment(R"("fault_line":%u,"fault_col":%u,)", {wall.line, wall.col}, &root))));
    }
    SECTION("no clock at all: the first read, the step, fails where it is authored")
    {
        const DiagArg    args[] = {{"args", "7"}};
        const DiagResult r      = svc.execute(inspect_request(kClockFile, {args, 1U}));
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), R"("wall_clock":"none","sim_time":"none","sim_time_ns":0,"sim_step":"none",)"));
        CHECK(has(view(r.json), R"("outcome":"error","error":"input-unavailable",)"));
        CHECK(has(view(r.json), view(fragment(R"("fault_line":%u,"fault_col":%u,)", {step.line, step.col}, &root))));
    }
}
