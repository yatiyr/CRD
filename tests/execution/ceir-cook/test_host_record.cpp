// DIAG.9a -- recording at the inspect host's boundary. InspectHost is the production host of the headless and sandbox
// inspection consumers: it runs an authored program from a ReloadSet generation under a debug session. A start may
// record its execution: the host re-cooks the generation that runs into the record's blob and traces the run as the
// session's safe-point observer.
//
// Covered: a run stopped at breakpoints and stepped into and out records the same trace, outcome and inputs as the
// unobserved run `replay.record` makes of the same program (the debugger is invisible to the record); the record names
// the host's asset and the generation that ran, its blob replays in a fresh Context at the authored positions, and the
// replay.run command reproduces it from the file; after a hot reload the record still names, and holds, the generation
// that ran, and replaying its inputs against the edited program names the edited constant; a start without recording
// forgets the record; a running, cancelled or unrecorded execution gives no record; a record file is never
// overwritten; a schema 1 record and a generation without an asset are refused; and an observer's Cancel ends a
// session's run. Expected lines and columns come from scanning the text, never from the parser. ASCII test names.

#include <crd/ceir/cook/inspect_host.hpp>
#include <crd/ceir/cook/inspect_script.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/plan.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>

namespace
{
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::cook::AssetId;
using crd::ceir::cook::HostRecord;
using crd::ceir::cook::HostRecording;
using crd::ceir::cook::InspectHost;
using crd::ceir::cook::ReplayRecord;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagArg;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;
namespace ck   = crd::ceir::cook;
namespace insp = crd::ceir::inspect;
namespace plan = crd::ceir::plan;

constexpr const char* kAsset  = CRD_REPO_DIR "/assets/ceir/replay_demo.ceir";
constexpr const char* kFile   = "ceir/replay_demo.ceir"; // the name the host cooks the program under
constexpr u32         kWaitMs = 20000U;                  // generous: a sanitizer lane is slow; a pass never waits
const AssetId         kId{9100U};

constexpr const char* kBiasLine   = "%9 = arith.const() {value = 1} : !i32";
constexpr const char* kBiasEdited = "%9 = arith.const() {value = -1} : !i32";

void registrar(Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
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

void spill(const char* path, StringView text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(f.good());
}

void forget(const char* path)
{
    (void)std::remove(path);
}

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

// The 1-based line holding `needle` (once) and the column of that line's first non-blank character.
struct Where
{
    u32 line = 0U;
    u32 col  = 0U;
};
Where locate(StringView text, StringView needle)
{
    const usize at = text.find(needle);
    REQUIRE(at != StringView::npos);
    REQUIRE(text.find(needle, at + 1U) == StringView::npos);
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

String edited(StringView text, crd::memory::IAllocator* a)
{
    const usize at = text.find(StringView{kBiasLine});
    REQUIRE(at != StringView::npos);
    String out(a);
    out.append(text.substr(0U, at));
    out.append(kBiasEdited);
    out.append(text.substr(at + StringView{kBiasLine}.size()));
    return out;
}

// The raw text of `"key":<value>` in `json` at or after `from`, up to the next ',' or '}'.
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

u64 number(StringView json, const char* key, crd::memory::IAllocator* a, usize from = 0U)
{
    const String v = field(json, key, a, from);
    u64          n = 0U;
    for (usize i = 0U; i < v.size(); ++i)
    {
        REQUIRE(v.data()[i] >= '0');
        REQUIRE(v.data()[i] <= '9');
        n = n * 10U + static_cast<u64>(v.data()[i] - '0');
    }
    return n;
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

// The replay commands, as a host binds them.
struct Commands
{
    Commands() : svc(kAll, here())
    {
        cmd.registrar = &registrar;
        REQUIRE(ck::register_replay_record(svc, cmd));
        REQUIRE(ck::register_replay_run(svc, cmd));
    }

    ck::ReplayCommands cmd;
    DiagCommandService svc;
};

DiagResult record_unobserved(Commands& c, const char* program, const char* out, const char* args)
{
    const DiagArg a[2] = {{"out", StringView{out}}, {"args", StringView{args}}};
    DiagRequest   r;
    r.command    = ck::kReplayRecordCommand;
    r.path       = StringView{program};
    r.args       = {a, 2U};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return c.svc.execute(r);
}

DiagResult replay(Commands& c, const char* path, const char* program = nullptr)
{
    DiagArg a[1];
    u32     n = 0U;
    if (program != nullptr)
    {
        a[n++] = {"program", StringView{program}};
    }
    DiagRequest r;
    r.command    = ck::kReplayRunCommand;
    r.path       = StringView{path};
    r.args       = {a, n};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return c.svc.execute(r);
}

ReplayRecord decode_file(const char* path, crd::memory::IAllocator* a)
{
    const Array<u8> bytes = slurp_bytes(path, a);
    ReplayRecord    rec(a);
    REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, rec) == ck::RecordError::Ok);
    return rec;
}

bool same_events(const ReplayRecord& a, const ReplayRecord& b)
{
    if (a.events_total != b.events_total || a.events.size() != b.events.size())
    {
        return false;
    }
    for (usize i = 0U; i < a.events.size(); ++i)
    {
        if (!(a.events[i] == b.events[i]))
        {
            return false;
        }
    }
    return true;
}

bool same_values(const Array<i64>& a, const Array<i64>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize i = 0U; i < a.size(); ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

// Replay `rec` from its own blob in a fresh Context; the first divergence.
ck::Divergence replay_blob(const ReplayRecord& rec, crd::memory::IAllocator* a)
{
    Context           ctx(a);
    ck::ReplayProgram p(a);
    ck::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, view(rec.entry), &registrar, nullptr, p);
    REQUIRE(p.ok());
    CHECK(p.content_hash == rec.content_hash);
    ck::ReplayTrace t(a);
    ck::run_traced(p, {rec.args.data(), rec.args.size()}, rec.max_events, nullptr, t);
    return ck::first_divergence(rec, t);
}

u64 installed_hash(const InspectHost& host)
{
    return host.programs().generation(kId)->program.content_hash;
}
} // namespace

TEST_CASE("diag 9a: a run inspected on the inspect host records the trace of the unobserved run", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        call = locate(view(text), "%8 = func.call(%7, %3)");
    const Where                        add  = locate(view(text), "%10 = arith.addi(%3, %9)");
    const Where                        sw   = locate(view(text), "core.switch(%10)");

    const char* const program   = "diag9a_host_demo.ceir";
    const char* const plain_out = "diag9a_host_plain.crpl";
    const char* const host_out  = "diag9a_host_inspected.crpl";
    forget(plain_out);
    forget(host_out);
    spill(program, view(text));

    // The unobserved run of the same program: replay.record, with no session.
    Commands c;
    REQUIRE(record_unobserved(c, program, plain_out, "1").status == DiagStatus::Ok);
    const ReplayRecord plain = decode_file(plain_out, &alloc);

    // The same run on the inspect host, stopped at two breakpoints, stepped into the callee and out, and recorded.
    InspectHost host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(kId, view(text), kFile, "main").ok());
    ReplayRecord rec(&alloc);
    CHECK(host.record(rec) == HostRecord::NotRecorded);

    const i64              args[1]    = {1};
    const u32              breaks[2]  = {call.line, add.line};
    const u32              watches[1] = {add.line};
    const ck::ScriptAction actions[3] = {ck::ScriptAction::Into, ck::ScriptAction::Out, ck::ScriptAction::Over};
    ck::InspectScript      script;
    script.file           = StringView{kFile};
    script.args           = {args, 1U};
    script.breaks         = {breaks, 2U};
    script.watches        = {watches, 1U};
    script.actions        = {actions, 3U};
    script.record.enabled = true;
    ck::InspectReport report(&alloc);
    ck::run_inspect_script(host, script, report);
    REQUIRE(report.outcome == ck::ScriptOutcome::Error);
    CHECK(report.error == plan::RunError::SelectorOutOfRange);
    // The debugger really held the run: three call hits and the addi, with a step into the callee among them.
    REQUIRE(report.stops.size() >= 5U);
    bool stepped_in = false;
    for (const ck::ScriptStop& s : report.stops)
    {
        stepped_in = stepped_in || (s.reason == insp::StopReason::Step && s.depth == 1U);
    }
    CHECK(stepped_in);

    REQUIRE(host.record(rec) == HostRecord::Ok);
    CHECK(rec.schema == ck::kReplayRecordSchema);
    CHECK(rec.asset == kId.value);
    CHECK(rec.generation == host.generation());
    CHECK(rec.generation != 0U);
    CHECK(view(rec.program_path) == kFile);
    CHECK(view(rec.entry) == "main");
    REQUIRE(rec.args.size() == 1U);
    CHECK(rec.args[0] == 1);
    CHECK(rec.content_hash == installed_hash(host));
    String differing(&alloc);
    CHECK(ck::same_build(rec.build, ck::current_build(&alloc), differing));

    // The debugger is invisible to the record: the same program content, inputs, trace and outcome as the unobserved
    // run (only where the program came from differs).
    CHECK(rec.content_hash == plain.content_hash);
    CHECK(plain.asset == 0U);
    CHECK(plain.generation == 0U);
    for (u32 i = 0U; i < ck::kReplayInputs; ++i)
    {
        CHECK(rec.inputs[i].need == plain.inputs[i].need);
        CHECK(rec.inputs[i].state == plain.inputs[i].state);
    }
    CHECK(rec.max_events == plain.max_events);
    CHECK(same_events(rec, plain));
    CHECK(rec.error == plain.error);
    CHECK(rec.fault_op == plain.fault_op);
    CHECK(same_values(rec.results, plain.results));
    CHECK(same_values(rec.cells, plain.cells));

    // Its blob, re-cooked from the generation, replays in a fresh Context and keeps the authored positions.
    CHECK(replay_blob(rec, &alloc).kind == ck::DivergenceKind::None);
    {
        Context           ctx(&alloc);
        ck::ReplayProgram p(&alloc);
        ck::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main", &registrar, nullptr, p);
        REQUIRE(p.ok());
        const ck::ReplaySite fault = ck::replay_site_of_op(ctx, p, rec.fault_op);
        CHECK(fault.file == StringView{kFile});
        CHECK(fault.line == sw.line);
        CHECK(fault.col == sw.col);
    }

    // The replay.run command reproduces it from the file, naming the asset and the generation.
    REQUIRE(ck::write_record_file(StringView{host_out}, rec) == ck::RecordWrite::Ok);
    const DiagResult r = replay(c, host_out);
    REQUIRE(r.status == DiagStatus::Ok);
    const StringView j = view(r.json);
    CHECK(view(field(j, "result", &alloc)) == "\"reproduced\"");
    CHECK(number(j, "asset", &alloc) == kId.value);
    const usize summary = j.find(StringView{R"("asset":)"}); // the service also names its snapshot generation
    REQUIRE(summary != StringView::npos);
    CHECK(number(j, "generation", &alloc, summary) == rec.generation);
    CHECK(view(field(j, "program", &alloc)) == "\"ceir/replay_demo.ceir\"");

    // A record is never overwritten.
    const Array<u8> before = slurp_bytes(host_out, &alloc);
    ReplayRecord    other  = decode_file(plain_out, &alloc);
    CHECK(ck::write_record_file(StringView{host_out}, other) == ck::RecordWrite::Exists);
    const Array<u8> after = slurp_bytes(host_out, &alloc);
    REQUIRE(after.size() == before.size());
    bool unchanged = true;
    for (usize i = 0U; i < after.size(); ++i)
    {
        unchanged = unchanged && after[i] == before[i];
    }
    CHECK(unchanged);

    forget(plain_out);
    forget(host_out);
    forget(program);
}

TEST_CASE("diag 9a: a host record holds the generation that ran, after the host reloads", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const String                       edit = edited(view(text), &alloc);
    const Where                        bias = locate(view(edit), kBiasEdited);

    const char* const edited_program = "diag9a_host_edited.ceir";
    const char* const out            = "diag9a_host_gen1.crpl";
    forget(out);
    spill(edited_program, view(edit));

    InspectHost host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(kId, view(text), kFile, "main").ok());
    const u64 gen1  = host.generation();
    const u64 hash1 = installed_hash(host);
    const i64 one[] = {1};

    REQUIRE(host.start({one, 1U}, HostRecording{true, 0U}) == insp::Refusal::None);
    REQUIRE(host.wait_finished(kWaitMs));
    CHECK(host.result().error == plan::RunError::SelectorOutOfRange);

    // The edit hot-swaps a new generation in.
    const ck::HostLoadResult lr = host.load(kId, view(edit), kFile, "main");
    REQUIRE(lr.ok());
    CHECK(lr.decision == ck::ReloadDecision::HotSwap);
    const u64 gen2 = host.generation();
    CHECK(gen2 != gen1);
    CHECK(installed_hash(host) != hash1);

    // The record is still generation 1's: its number, its content and its failure.
    ReplayRecord rec(&alloc);
    REQUIRE(host.record(rec) == HostRecord::Ok);
    CHECK(rec.generation == gen1);
    CHECK(rec.content_hash == hash1);
    CHECK(rec.max_events == ck::kReplayDefaultMaxEvents); // 0 asks for the default
    CHECK(rec.error == plan::RunError::SelectorOutOfRange);
    CHECK(replay_blob(rec, &alloc).kind == ck::DivergenceKind::None);

    // Replaying its inputs against the edited program names the edited constant first.
    REQUIRE(ck::write_record_file(StringView{out}, rec) == ck::RecordWrite::Ok);
    Commands         c;
    const DiagResult r = replay(c, out, edited_program);
    REQUIRE(r.status == DiagStatus::Ok);
    const StringView j  = view(r.json);
    const usize      at = j.find(StringView{R"("kind":"divergence")"});
    REQUIRE(at != StringView::npos);
    CHECK(view(field(j, "divergence", &alloc, at)) == "\"value\"");
    CHECK(number(j, "line", &alloc, at) == bias.line);
    CHECK(number(j, "col", &alloc, at) == bias.col);
    CHECK(number(j, "generation", &alloc, j.find(StringView{R"("asset":)"})) == gen1);

    // A start without recording forgets the record.
    REQUIRE(host.start({one, 1U}) == insp::Refusal::None);
    REQUIRE(host.wait_finished(kWaitMs));
    CHECK(host.result().error == plan::RunError::None); // generation 2 finishes
    CHECK(host.record(rec) == HostRecord::NotRecorded);

    // A recorded run of generation 2 names it, finishes, and keeps only its first events.
    REQUIRE(host.start({one, 1U}, HostRecording{true, 3U}) == insp::Refusal::None);
    REQUIRE(host.wait_finished(kWaitMs));
    REQUIRE(host.record(rec) == HostRecord::Ok);
    CHECK(rec.generation == gen2);
    CHECK(rec.content_hash == installed_hash(host));
    CHECK(rec.error == plan::RunError::None);
    CHECK(rec.max_events == 3U);
    CHECK(rec.events.size() == 3U);
    CHECK(rec.events_total > 3U);
    CHECK(replay_blob(rec, &alloc).kind == ck::DivergenceKind::None);

    forget(out);
    forget(edited_program);
}

TEST_CASE("diag 9a: the inspect host gives no record of a running, cancelled or unrecorded execution",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        add  = locate(view(text), "%10 = arith.addi(%3, %9)");
    const i64                          one[] = {1};

    InspectHost  host(&alloc, &registrar, nullptr);
    ReplayRecord rec(&alloc);
    CHECK(host.record(rec) == HostRecord::NotRecorded); // nothing loaded, nothing started
    REQUIRE(host.load(kId, view(text), kFile, "main").ok());
    u32 index = 0U;
    REQUIRE(host.add_line_breakpoint(StringView{kFile}, add.line, index) == insp::Refusal::None);

    SECTION("held at a breakpoint the execution is running; cancelled there, it gives no record")
    {
        REQUIRE(host.start({one, 1U}, HostRecording{true, 0U}) == insp::Refusal::None);
        const u64        gen = host.generation();
        insp::StopRecord stop;
        REQUIRE(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(host.record(rec) == HostRecord::Running);
        REQUIRE(host.session().cancel(gen) == insp::Refusal::None);
        REQUIRE(host.wait_finished(kWaitMs));
        CHECK(host.result().error == plan::RunError::Cancelled);
        CHECK(host.record(rec) == HostRecord::Cancelled);
        CHECK(ck::host_record_name(HostRecord::Cancelled) == "cancelled");
    }
    SECTION("a schema 1 record and a generation without an asset are refused")
    {
        REQUIRE(host.start({one, 1U}, HostRecording{true, 0U}) == insp::Refusal::None);
        const u64        gen = host.generation();
        insp::StopRecord stop;
        REQUIRE(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::None);
        REQUIRE(host.session().resume(gen, insp::Resume::Continue) == insp::Refusal::None);
        REQUIRE(host.wait_finished(kWaitMs));
        REQUIRE(host.record(rec) == HostRecord::Ok);

        Array<u8>    bytes(&alloc);
        ReplayRecord back(&alloc);
        rec.schema = 1U;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::UnsupportedSchema);
        rec.schema = ck::kReplayRecordSchema;
        rec.asset  = 0U;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec.asset = kId.value;
        ck::encode_record(rec, bytes);
        REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Ok);
        CHECK(back.asset == kId.value);
        CHECK(back.generation == gen);
    }
}

TEST_CASE("diag 9a: a session's safe-point observer sees every safe point and its Cancel ends the run",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    InspectHost                        host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(kId, view(text), kFile, "main").ok());
    const i64 zero[] = {0};

    struct Count
    {
        u32 seen      = 0U;
        u32 cancel_at = 0U; // 0: never
    };
    const auto observe = [](const plan::CompiledPlan& /*plan*/, const plan::SafePoint& /*at*/, void* user)
    {
        auto& c = *static_cast<Count*>(user);
        ++c.seen;
        return (c.seen == c.cancel_at) ? plan::SafePointAction::Cancel : plan::SafePointAction::Continue;
    };

    // Unobserved, the plan dispatches some number of instrs; observed through a session, the observer sees each one.
    ck::ReplayTrace       t(&alloc);
    ck::ReplayRecorder    rec(t, ck::kReplayDefaultMaxEvents);
    const plan::RunControl direct = rec.control();
    const plan::RunResult plain   = plan::run(host.compiled_plan(), {zero, 1U}, &alloc, plan::RunHooks{}, &direct);
    REQUIRE(plain.error == plan::RunError::None);
    REQUIRE(t.events_total > 4U);

    insp::Session          session(&alloc);
    Count                  all;
    const plan::RunControl count_all{+observe, &all, nullptr};
    CHECK(session.run(host.compiled_plan(), {zero, 1U}, &alloc, &count_all).error == plan::RunError::None);
    CHECK(all.seen == t.events_total);

    Count                  stop_early;
    stop_early.cancel_at = 3U;
    const plan::RunControl cancel_third{+observe, &stop_early, nullptr};
    CHECK(session.run(host.compiled_plan(), {zero, 1U}, &alloc, &cancel_third).error == plan::RunError::Cancelled);
    CHECK(stop_early.seen == 3U);
}
