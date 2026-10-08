// DIAG.9a -- host records through the replay diagnostic commands. crd-ceir-cook's `replay.record executor=host` and
// `replay.run` reach the host provider only through the executor crd-ceir-host provides
// (crd/ceir/host/host_replay_diag.hpp); with none bound, a host request is refused before anything is read or run.
//
// The committed authored program assets/ceir/host_replay_demo.ceir pools an async.launch whose body calls a
// function, folds a task.map_reduce, keeps a state cell and ends in a loop whose step is the entry argument, so
// main(0) fails bad-for-step at that loop. The command's record is the library's record of the same artifact, byte
// for byte; a fresh service reproduces it after the program file is edited (the record holds its own artifact) and on
// another job split; replaying the same inputs against the edited file names the edited launch constant at the await
// that reads its value (recorded 25, observed 36); a small step budget's failure is reproduced only because the
// budget is in the record. Incompatible records and bad arguments are refused with nothing run. Expected lines and
// columns come from scanning the text, never from the parser.
// ⛔ The jobs pool is owned by the listener in test_host_provider.cpp (same binary). ASCII test names.

#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_replay.hpp>
#include <crd/ceir/host/host_replay_diag.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <initializer_list>

namespace
{
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::cook::ReplayCommands;
using crd::ceir::cook::ReplayRecord;
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
namespace ck = crd::ceir::cook;
namespace hs = crd::ceir::host;

constexpr const char* kAsset = CRD_REPO_DIR "/assets/ceir/host_replay_demo.ceir";

// The launched constant the edit changes, and what it becomes.
constexpr const char* kLaunchLine   = "%4 = arith.const() {value = 5} : !i32";
constexpr const char* kLaunchEdited = "%4 = arith.const() {value = 6} : !i32";

void registrar(Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::task::register_task_ops(ctx);
    (void)crd::ceir::async::register_async_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
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

Array<u8> slurp_bytes(const char* path, crd::memory::IAllocator* a)
{
    const String s = slurp(path, a);
    Array<u8>    out(a);
    for (usize i = 0U; i < s.size(); ++i)
    {
        out.push_back(static_cast<u8>(s.data()[i]));
    }
    return out;
}

void spill(const char* path, const void* data, usize size)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    REQUIRE(f.good());
}

bool exists(const char* path)
{
    const std::ifstream f(path, std::ios::binary);
    return f.good();
}

void forget(const char* path)
{
    (void)std::remove(path);
}

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

bool has(StringView hay, StringView needle)
{
    return hay.find(needle) != StringView::npos;
}

// The independent oracle for an authored op: the 1-based line holding `needle` and the column of that line's first
// non-blank character.
struct Where
{
    u32 line = 0U;
    u32 col  = 0U;
};

Where locate(StringView text, StringView needle)
{
    const usize at = text.find(needle);
    REQUIRE(at != StringView::npos);
    REQUIRE(text.find(needle, at + 1U) == StringView::npos); // the needle names one line
    Where w;
    w.line           = 1U;
    usize line_start = 0U;
    for (usize i = 0; i < at; ++i)
    {
        if (text[i] == '\n')
        {
            ++w.line;
            line_start = i + 1U;
        }
    }
    usize first = line_start;
    while (text[first] == ' ')
    {
        ++first;
    }
    w.col = static_cast<u32>(first - line_start + 1U);
    return w;
}

// The raw text of `"key":<value>` in `json` (the first occurrence at or after `from`), up to the next ',' or '}'
// (a string value keeps its quotes).
String field(StringView json, const char* key, crd::memory::IAllocator* a, usize from = 0U)
{
    String needle(a);
    needle.append("\"");
    needle.append(key);
    needle.append("\":");
    const usize at = json.find(view(needle), from);
    REQUIRE(at != StringView::npos);
    usize       end   = at + needle.size();
    const usize start = end;
    if (json[end] == '"')
    {
        end = json.find('"', end + 1U) + 1U;
    }
    else
    {
        while (end < json.size() && json[end] != ',' && json[end] != '}')
        {
            ++end;
        }
    }
    String out(a);
    out.append(json.substr(start, end - start));
    return out;
}

i64 number(StringView json, const char* key, crd::memory::IAllocator* a, usize from = 0U)
{
    const String v    = field(json, key, a, from);
    i64          n    = 0;
    usize        i    = 0U;
    const bool   sign = !v.empty() && v.data()[0] == '-';
    if (sign)
    {
        i = 1U;
    }
    for (; i < v.size(); ++i)
    {
        REQUIRE(v.data()[i] >= '0');
        REQUIRE(v.data()[i] <= '9');
        n = n * 10 + static_cast<i64>(v.data()[i] - '0');
    }
    return sign ? -n : n;
}

String edited(StringView text, crd::memory::IAllocator* a)
{
    const usize at = text.find(StringView{kLaunchLine});
    REQUIRE(at != StringView::npos);
    String out(a);
    out.append(text.substr(0U, at));
    out.append(kLaunchEdited);
    out.append(text.substr(at + StringView{kLaunchLine}.size()));
    return out;
}

ReplayRecord decode_file(const char* path, crd::memory::IAllocator* a)
{
    const Array<u8> bytes = slurp_bytes(path, a);
    ReplayRecord    rec(a);
    REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, rec) == ck::RecordError::Ok);
    return rec;
}

void encode_file(const ReplayRecord& rec, const char* path, crd::memory::IAllocator* a)
{
    Array<u8> bytes(a);
    ck::encode_record(rec, bytes);
    forget(path);
    spill(path, bytes.data(), bytes.size());
}

constexpr crd::perf::DiagAuthoritySet kAll = crd::perf::authority_bit(DiagAuthority::Read) |
                                             crd::perf::authority_bit(DiagAuthority::Record) |
                                             crd::perf::authority_bit(DiagAuthority::Execute);

DiagServiceConfig here()
{
    DiagServiceConfig c;
    c.root = StringView{"."};
    return c;
}

// A diagnostic host. `bound`: the host provider is the replay commands' host executor.
struct Host
{
    explicit Host(bool bound = true, crd::perf::DiagAuthoritySet grant = kAll) : svc(grant, here())
    {
        cmd.registrar     = &registrar;
        prepare.registrar = &registrar;
        if (bound)
        {
            cmd.host = &hs::host_replay_executor();
        }
        REQUIRE(ck::register_replay_record(svc, cmd));
        REQUIRE(ck::register_replay_run(svc, cmd));
        REQUIRE(ck::register_replay_prepare(svc, prepare));
    }

    ReplayCommands           cmd;
    ck::ReplayPrepareCommand prepare;
    DiagCommandService       svc;
};

DiagResult send(Host& h, StringView command, const char* path, std::initializer_list<DiagArg> args)
{
    DiagRequest r;
    r.command    = command;
    r.path       = StringView{path};
    r.args       = {args.begin(), args.size()};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r);
}

void refused(const DiagResult& r, DiagStatus want, const char* reason)
{
    INFO(r.json.c_str());
    CHECK(r.status == want);
    CHECK(has(StringView{r.reason.data(), r.reason.size()}, StringView{reason}));
}
} // namespace

TEST_CASE("diag 9a: replay.record executor=host makes the library's host record of the committed program",
          "[ceir][host][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text  = slurp(kAsset, &alloc);
    const Where                        guard = locate(view(text), "core.for(%25, %26, %2)");

    const char* const program = "diag9a_hostcmd_record.ceir";
    const char* const out     = "diag9a_hostcmd_record.crpl";
    forget(out);
    spill(program, text.data(), text.size());

    Host             h;
    const DiagResult r = send(h, ck::kReplayRecordCommand, program,
                              {{"out", out}, {"args", "0"}, {"executor", "host"}, {"jobs", "3"}});
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    const StringView j = view(r.json);
    CHECK(field(j, "executor", &alloc) == "\"host\"");
    CHECK(number(j, "jobs", &alloc) == 3);
    CHECK(number(j, "sub_fuel", &alloc) == (i64{1} << 20));
    CHECK(field(j, "error", &alloc) == "\"bad-for-step\"");
    CHECK(field(j, "fault_file", &alloc) == "\"diag9a_hostcmd_record.ceir\"");
    CHECK(number(j, "fault_line", &alloc) == guard.line);
    CHECK(number(j, "fault_col", &alloc) == guard.col);
    CHECK(field(j, "replay", &alloc) == "\"replayable\"");
    CHECK(has(j, R"("input":"schedule","guarantee":"schedule","needed":"yes","state":"recorded")"));
    CHECK(h.cmd.executions.load() == 1U);
    CHECK(h.cmd.records_written.load() == 1U);

    // The file is a host record of the authored artifact, and it is the library's record of that artifact.
    const ReplayRecord rec = decode_file(out, &alloc);
    CHECK(rec.executor == ck::ReplayExecutorKind::Host);
    CHECK(rec.host_jobs == 3U);
    CHECK(rec.host_sub_fuel == (u64{1} << 20U));
    CHECK(rec.host_error == crd::ceir::exec::ExecError::BadForStep);
    REQUIRE(rec.args.size() == 1U);
    CHECK(rec.args[0] == 0);
    CHECK(view(rec.program_path) == StringView{program});

    Context cctx(&alloc);
    registrar(cctx, nullptr);
    const ck::CookResult cooked = ck::cook_program_text(cctx, view(text), StringView{program}, 1U, &alloc, &alloc);
    REQUIRE(cooked.ok());
    CHECK(cooked.blob == rec.program); // the authored text's artifact, cooked under its relative path
    ReplayRecord lib(&alloc);
    hs::HostSite fault(&alloc);
    const i64    zero[1] = {0};
    String       missing(&alloc);
    REQUIRE(hs::record_host_run({rec.program.data(), rec.program.size()}, StringView{program}, StringView{"main"},
                                {zero, 1U}, hs::HostSchedule{3U, u64{1} << 20U}, ck::kReplayDefaultMaxEvents,
                                &registrar, nullptr, lib, &missing, &fault) == hs::HostReplayStatus::Ok);
    CHECK(missing.empty());
    CHECK(fault.line == guard.line);
    Array<u8> mine(&alloc);
    Array<u8> theirs(&alloc);
    ck::encode_record(rec, mine);
    ck::encode_record(lib, theirs);
    CHECK(mine.size() == theirs.size());
    CHECK(mine == theirs);

    // replay.prepare reads it: the schedule is in the record.
    const DiagResult prep = send(h, ck::kReplayPrepareCommand, out, {});
    INFO(prep.json.c_str());
    REQUIRE(prep.status == DiagStatus::Ok);
    CHECK(has(view(prep.json), R"("input":"schedule")"));

    forget(out);
    forget(program);
}

TEST_CASE("diag 9a: a host record reproduces in a fresh service after its program file is edited",
          "[ceir][host][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text  = slurp(kAsset, &alloc);
    const Where                        await = locate(view(text), "%6 = async.await(%3)");
    const Where                        guard = locate(view(text), "core.for(%25, %26, %2)");

    const char* const program = "diag9a_hostcmd_edit.ceir";
    const char* const out     = "diag9a_hostcmd_edit.crpl";
    forget(out);
    spill(program, text.data(), text.size());
    {
        Host             h;
        const DiagResult r =
            send(h, ck::kReplayRecordCommand, program, {{"out", out}, {"args", "0"}, {"executor", "host"}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
    }

    // The program file is edited after the run.
    const String edit = edited(view(text), &alloc);
    spill(program, edit.data(), edit.size());

    SECTION("a fresh service replays the record's own artifact: the failure reproduces at its authored loop")
    {
        Host             h;
        const DiagResult r = send(h, ck::kReplayRunCommand, out, {});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(field(j, "result", &alloc) == "\"reproduced\"");
        CHECK(field(j, "executor", &alloc) == "\"host\"");
        CHECK(number(j, "recorded_jobs", &alloc) == 8);
        CHECK(number(j, "jobs", &alloc) == 8);
        CHECK(field(j, "program_matches", &alloc) == "true");
        const usize recorded = j.find(StringView{R"("run":"recorded")"});
        const usize replayed = j.find(StringView{R"("run":"replayed")"});
        REQUIRE(recorded != StringView::npos);
        REQUIRE(replayed != StringView::npos);
        CHECK(field(j, "error", &alloc, recorded) == "\"bad-for-step\"");
        CHECK(number(j, "line", &alloc, recorded) == guard.line);
        CHECK(field(j, "error", &alloc, replayed) == "\"bad-for-step\"");
        CHECK(number(j, "line", &alloc, replayed) == guard.line);
        CHECK(number(j, "col", &alloc, replayed) == guard.col);
        CHECK(h.cmd.executions.load() == 1U);
    }
    SECTION("on another job split it still reproduces, and says which split ran")
    {
        Host             h;
        const DiagResult r = send(h, ck::kReplayRunCommand, out, {{"jobs", "16"}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(field(view(r.json), "result", &alloc) == "\"reproduced\"");
        CHECK(number(view(r.json), "recorded_jobs", &alloc) == 8);
        CHECK(number(view(r.json), "jobs", &alloc) == 16);
    }
    SECTION("the same inputs against the edited file name the launched constant at the await that reads it")
    {
        Host             h;
        const DiagResult r = send(h, ck::kReplayRunCommand, out, {{"program", program}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(field(j, "result", &alloc) == "\"diverged\"");
        CHECK(field(j, "program_source", &alloc) == "\"argument\"");
        CHECK(field(j, "program_matches", &alloc) == "false");
        const usize d = j.find(StringView{R"("kind":"divergence")"});
        REQUIRE(d != StringView::npos);
        CHECK(field(j, "divergence", &alloc, d) == "\"value\"");
        CHECK(number(j, "recorded", &alloc, d) == 25);
        CHECK(number(j, "observed", &alloc, d) == 36);
        CHECK(field(j, "file", &alloc, d) == "\"diag9a_hostcmd_edit.ceir\"");
        CHECK(number(j, "line", &alloc, d) == await.line);
        CHECK(number(j, "col", &alloc, d) == await.col);
    }

    forget(out);
    forget(program);
}

TEST_CASE("diag 9a: a host record's step budget is replayed from the record", "[ceir][host][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text   = slurp(kAsset, &alloc);
    const Where                        reduce = locate(view(text), "%11 = task.map_reduce(");
    const Where                        guard  = locate(view(text), "core.for(%25, %26, %2)");

    const char* const program = "diag9a_hostcmd_fuel.ceir";
    const char* const small   = "diag9a_hostcmd_fuel_small.crpl";
    const char* const full    = "diag9a_hostcmd_fuel_full.crpl";
    forget(small);
    forget(full);
    spill(program, text.data(), text.size());

    Host             h;
    const DiagResult s = send(h, ck::kReplayRecordCommand, program,
                              {{"out", small}, {"args", "1"}, {"executor", "host"}, {"sub_fuel", "16"}});
    INFO(s.json.c_str());
    REQUIRE(s.status == DiagStatus::Ok);
    CHECK(field(view(s.json), "error", &alloc) == "\"fuel-exhausted\"");
    CHECK(number(view(s.json), "sub_fuel", &alloc) == 16);
    const i64 fault_line = number(view(s.json), "fault_line", &alloc);
    CHECK(fault_line > reduce.line); // inside the map body, run on a sub-interpreter
    CHECK(fault_line < guard.line);

    const DiagResult f =
        send(h, ck::kReplayRecordCommand, program, {{"out", full}, {"args", "1"}, {"executor", "host"}});
    REQUIRE(f.status == DiagStatus::Ok);
    CHECK(field(view(f.json), "error", &alloc) == "\"none\"");

    // A fresh service reproduces the small budget's failure: the budget is the record's, not the host's default.
    Host             fresh;
    const DiagResult r = send(fresh, ck::kReplayRunCommand, small, {});
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(field(view(r.json), "result", &alloc) == "\"reproduced\"");
    CHECK(number(view(r.json), "sub_fuel", &alloc) == 16);
    const usize replayed = view(r.json).find(StringView{R"("run":"replayed")"});
    REQUIRE(replayed != StringView::npos);
    CHECK(field(view(r.json), "error", &alloc, replayed) == "\"fuel-exhausted\"");

    forget(small);
    forget(full);
    forget(program);
}

TEST_CASE("diag 9a: host requests are refused before anything is read or run", "[ceir][host][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);

    const char* const program = "diag9a_hostcmd_refuse.ceir";
    const char* const out     = "diag9a_hostcmd_refuse.crpl";
    const char* const tamper  = "diag9a_hostcmd_refuse_tampered.crpl";
    forget(out);
    forget(tamper);
    spill(program, text.data(), text.size());
    {
        Host             h;
        const DiagResult r =
            send(h, ck::kReplayRecordCommand, program, {{"out", out}, {"args", "0"}, {"executor", "host"}});
        REQUIRE(r.status == DiagStatus::Ok);
    }
    const ReplayRecord original = decode_file(out, &alloc);

    SECTION("with no host executor bound, executor=host and a host record are unavailable, unread and unrun")
    {
        Host             h(false);
        const DiagResult r = send(h, ck::kReplayRecordCommand, program,
                                  {{"out", tamper}, {"args", "0"}, {"executor", "host"}});
        refused(r, DiagStatus::Unavailable, "binds no host executor");
        CHECK(h.cmd.bytes_read.load() == 0U);
        CHECK_FALSE(exists(tamper));
        const DiagResult p = send(h, ck::kReplayRunCommand, out, {});
        refused(p, DiagStatus::Unavailable, "made by the host executor and this host binds no host executor");
        CHECK(h.cmd.executions.load() == 0U);
    }
    SECTION("the host schedule's arguments are checked by the service before the command runs")
    {
        Host h;
        refused(send(h, ck::kReplayRecordCommand, program, {{"out", tamper}, {"jobs", "2"}}), DiagStatus::BadArgument,
                "need executor=host");
        refused(send(h, ck::kReplayRecordCommand, program, {{"out", tamper}, {"sub_fuel", "9"}}),
                DiagStatus::BadArgument, "need executor=host");
        refused(send(h, ck::kReplayRecordCommand, program, {{"out", tamper}, {"executor", "gpu"}}),
                DiagStatus::BadArgument, "must be 'plan', 'host' or 'device'");
        refused(send(h, ck::kReplayRecordCommand, program, {{"out", tamper}, {"executor", "host"}, {"jobs", "0"}}),
                DiagStatus::BadArgument, "must be 1 to 256");
        refused(send(h, ck::kReplayRecordCommand, program, {{"out", tamper}, {"executor", "host"}, {"jobs", "257"}}),
                DiagStatus::BadArgument, "must be 1 to 256");
        refused(send(h, ck::kReplayRecordCommand, program,
                     {{"out", tamper}, {"executor", "host"}, {"sub_fuel", "0"}}),
                DiagStatus::BadArgument, "must be 1 to 4294967296");
        refused(send(h, ck::kReplayRunCommand, out, {{"jobs", "0"}}), DiagStatus::BadArgument, "must be 1 to 256");
        CHECK(h.cmd.record_runs.load() == 0U); // the service's argument check refused them all
        CHECK(h.cmd.replay_runs.load() == 0U);
        CHECK_FALSE(exists(tamper));
    }
    SECTION("a plan record takes no job split")
    {
        ReplayRecord plan  = decode_file(out, &alloc);
        plan.executor      = ck::ReplayExecutorKind::Plan;
        plan.host_jobs     = 0U;
        plan.host_sub_fuel = 0U;
        plan.host_error    = crd::ceir::exec::ExecError::None;
        encode_file(plan, tamper, &alloc);
        Host h;
        refused(send(h, ck::kReplayRunCommand, tamper, {{"jobs", "2"}}), DiagStatus::BadArgument,
                "is a host record's job split");
        CHECK(h.cmd.executions.load() == 0U);
    }
    SECTION("another build is unavailable unless build=any; a missing input is unavailable")
    {
        ReplayRecord other = decode_file(out, &alloc);
        other.build.version.append("-other");
        encode_file(other, tamper, &alloc);
        Host h;
        refused(send(h, ck::kReplayRunCommand, tamper, {}), DiagStatus::Unavailable, "differs in version");
        CHECK(h.cmd.executions.load() == 0U);
        const DiagResult any = send(h, ck::kReplayRunCommand, tamper, {{"build", "any"}});
        INFO(any.json.c_str());
        REQUIRE(any.status == DiagStatus::Ok);
        CHECK(field(view(any.json), "build", &alloc) == "\"differs\"");
        CHECK(field(view(any.json), "result", &alloc) == "\"reproduced\"");
        CHECK(h.cmd.executions.load() == 1U);

        ReplayRecord lacking = decode_file(out, &alloc);
        lacking.inputs[3].need  = ck::ReplayNeed::Yes; // random
        lacking.inputs[3].state = ck::ReplayInputState::Missing;
        encode_file(lacking, tamper, &alloc);
        refused(send(h, ck::kReplayRunCommand, tamper, {}), DiagStatus::Unavailable, "missing inputs");
        CHECK(h.cmd.executions.load() == 1U);
    }
    SECTION("a record whose program is not its recorded content is refused by the host executor")
    {
        ReplayRecord wrong = decode_file(out, &alloc);
        wrong.content_hash ^= 1U;
        encode_file(wrong, tamper, &alloc);
        Host h;
        refused(send(h, ck::kReplayRunCommand, tamper, {}), DiagStatus::Failed, "does not match the content hash");
        CHECK(h.cmd.executions.load() == 0U);
    }
    SECTION("recording needs the Record authority as well as Execute")
    {
        Host h(true, crd::perf::authority_bit(DiagAuthority::Execute));
        const DiagResult r = send(h, ck::kReplayRecordCommand, program,
                                  {{"out", tamper}, {"args", "0"}, {"executor", "host"}});
        CHECK(r.status == DiagStatus::Unauthorized);
        CHECK(h.cmd.record_runs.load() == 0U);
        CHECK_FALSE(exists(tamper));
    }

    CHECK(original.executor == ck::ReplayExecutorKind::Host);
    forget(out);
    forget(tamper);
    forget(program);
}
