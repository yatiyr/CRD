// DIAG.9a -- recorded clock and time-step inputs. A program reads a time domain's current reading (input.clock) and
// its current step (input.time_step) through the host input seam; a run record keeps every read, in order, and a
// replay feeds them back instead of reading a live clock, so a failure that depends on the host's time step
// reproduces in a later process from the record alone, even after the program file was edited, and a result that
// depends on the live wall clock replays to the recorded value.
//
// The committed authored program assets/ceir/clock_demo.ceir reads the sim domain's step and switches on whether it is
// over a budget of 33,333,333 ns (one case: a step over budget is out of range), then returns the wall reading plus
// the sim reading plus its argument. Lines and columns come from scanning the text. The cases: a time-step failure
// recorded and reproduced after a restart and an edit (the edit reads another domain, named at its read); a passing
// step with a live wall clock, whose fed reading the replay returns, and a tampered reading named at its op; a host
// with no clock; the clock input stored missing when a read is past the bound or outside the seam, and the replay
// refused; malformed clock arguments refused before anything runs; the record format refusing a time read of an
// unknown domain; and the inspect host, given a host clock, recording exactly the unobserved run's reads, and past the
// bound storing the clock missing. The clock arguments every one-shot host shares parse whole, and RunInputs (the
// inputs program.inspect, ceridc inspect and the sandbox panel give a run) routes a seed and a clock spec and starts
// every run over. ASCII test names (ctest by-name).

#include <crd/ceir/cook/inspect_host.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/effect.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/semantics.hpp>
#include <crd/ceir/time.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
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
using crd::ceir::cook::ReplayInputRead;
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
namespace ck    = crd::ceir::cook;
namespace input = crd::ceir::input;

constexpr const char* kAsset = CRD_REPO_DIR "/assets/ceir/clock_demo.ceir";

// The step read the switch tests, and the edit that makes it read the frame domain's step instead.
constexpr const char* kStepLine   = "%1 = input.time_step() {domain = \"sim\"} : !i64";
constexpr const char* kStepEdited = "%1 = input.time_step() {domain = \"frame\"} : !i64";
constexpr const char* kWallLine   = "%4 = input.clock() {domain = \"wall\"} : !i64";
constexpr const char* kSwitchLine = "core.switch(%3)";

constexpr i64 kBudget  = 33333333; // the program's step budget, in nanoseconds
constexpr i64 kHitch   = 50000000; // a step over budget: the switch has no case for it
constexpr i64 kSteady  = 16666667; // a step within budget
constexpr i64 kSimTime = 4000000000;
constexpr u32 kSim     = 1U; // the sim domain's ordinal, the seam channel
constexpr u32 kFrame   = 2U;
constexpr u32 kWall    = 0U;
constexpr u32 kClockIn = 4U; // the clock input's slot in a record

// The effect of a probe op that reads the time itself, outside the input seam.
constexpr crd::ceir::EffectRecord kTimeRead[] = {
    {crd::ceir::EffectFamily::TimeRead, crd::ceir::EffectTarget::None, 0U, 0U}};

// main(n) reads the sim domain's reading n times and returns the first.
constexpr const char* kManyReads = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i64):
        %1 = arith.const() {value = 0} : !i64
        %2 = arith.const() {value = 1} : !i64
        core.for(%1, %0, %2) {
          ^bb0(%3 : !i64):
            %4 = input.clock() {domain = "sim"} : !i64
            core.yield()
        }
        func.return(%1)
    }
})";

void registrar(Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
    (void)input::register_input_ops(ctx);
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

bool exists(const char* path)
{
    const std::ifstream f(path, std::ios::binary);
    return f.good();
}

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

bool has(StringView hay, StringView needle)
{
    return hay.find(needle) != StringView::npos;
}

struct Where
{
    u32 line = 0U;
    u32 col  = 0U;
};

// The 1-based line holding `needle` (once) and the column of its first non-blank character.
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

String replaced(StringView text, const char* from, const char* to, crd::memory::IAllocator* a)
{
    const usize at = text.find(StringView{from});
    REQUIRE(at != StringView::npos);
    String out(a);
    out.append(text.substr(0U, at));
    out.append(to);
    out.append(text.substr(at + StringView{from}.size()));
    return out;
}

// The raw text of `"key":<value>` in `json`, the first at or after `from` (a string keeps its quotes).
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
    const String v        = field(json, key, a, from);
    i64          n        = 0;
    usize        i        = 0U;
    const bool   negative = v.size() > 0U && v.data()[0] == '-';
    if (negative)
    {
        i = 1U;
    }
    for (; i < v.size(); ++i)
    {
        REQUIRE(v.data()[i] >= '0');
        REQUIRE(v.data()[i] <= '9');
        n = n * 10 + static_cast<i64>(v.data()[i] - '0');
    }
    return negative ? -n : n;
}

String decimal(i64 v, crd::memory::IAllocator* a)
{
    char buf[32];
    (void)std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    String s(a);
    s.append(buf);
    return s;
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

struct Host
{
    Host() : svc(kAll, here())
    {
        cmd.registrar = &registrar;
        REQUIRE(ck::register_replay_record(svc, cmd));
        REQUIRE(ck::register_replay_run(svc, cmd));
    }

    ReplayCommands     cmd;
    DiagCommandService svc;
};

// replay.record of `program` into `out` with the arguments `extra` (name, value pairs) after `out` and `args`.
DiagResult record(Host& h, const char* program, const char* out, const char* args,
                  std::initializer_list<DiagArg> extra = {})
{
    DiagArg a[8] = {{"out", StringView{out}}, {"args", StringView{args}}};
    u32     n    = 2U;
    for (const DiagArg& e : extra)
    {
        REQUIRE(n < 8U);
        a[n++] = e;
    }
    DiagRequest r;
    r.command    = ck::kReplayRecordCommand;
    r.path       = StringView{program};
    r.args       = {a, n};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r);
}

DiagResult replay(Host& h, const char* path, const char* program = nullptr)
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
    return h.svc.execute(r);
}

ReplayRecord decode_file(const char* path, crd::memory::IAllocator* a)
{
    const String bytes = slurp(path, a);
    ReplayRecord rec(a);
    REQUIRE(ck::decode_record({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}, rec) == ck::RecordError::Ok);
    return rec;
}

// Replay `rec` from its own blob through an InputFeed over its reads: the first divergence and its authored line.
struct Replayed
{
    ck::Divergence d;
    u32            line = 0U;
    Array<i64>     results;
};
Replayed replay_feed(const ReplayRecord& rec, crd::memory::IAllocator* a)
{
    Context           ctx(a);
    ck::ReplayProgram p(a);
    ck::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, view(rec.entry), &registrar, nullptr, p);
    REQUIRE(p.ok());
    ck::ReplayTrace t(a);
    ck::InputFeed   feed({rec.input_reads.data(), rec.input_reads.size()}, t);
    ck::run_traced(p, {rec.args.data(), rec.args.size()}, rec.max_events, nullptr, t, feed.source());
    Replayed out{ck::first_divergence(rec, t), 0U, Array<i64>(a)};
    out.line = ck::replay_site(ctx, p, out.d.site).line;
    for (const i64 v : t.results)
    {
        out.results.push_back(v);
    }
    return out;
}

} // namespace

TEST_CASE("diag 9a clock: a time-step failure records every time read and reproduces after a restart and an edit",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        step = locate(view(text), kStepLine);
    const Where                        sw   = locate(view(text), kSwitchLine);
    const String                       hitch     = decimal(kHitch, &alloc);
    const String                       sim_time  = decimal(kSimTime, &alloc);
    REQUIRE(kHitch > kBudget);

    const char* const program = "diag9a_clk_demo.ceir";
    const char* const out     = "diag9a_clk_fail.crpl";
    const char* const again   = "diag9a_clk_again.crpl";
    forget(out);
    forget(again);
    spill(program, view(text));

    const std::initializer_list<DiagArg> clock = {
        {"clock", "wall"}, {"sim_time", view(sim_time)}, {"sim_step", view(hitch)}};
    {
        Host             h;
        const DiagResult r = record(h, program, out, "7", clock);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"selector-out-of-range\"");
        CHECK(number(j, "fault_line", &alloc) == sw.line);
        CHECK(number(j, "fault_col", &alloc) == sw.col);
        CHECK(view(field(j, "wall_clock", &alloc)) == "\"live\"");
        CHECK(view(field(j, "sim_time", &alloc)) == "\"set\"");
        CHECK(number(j, "sim_time_ns", &alloc) == kSimTime);
        CHECK(view(field(j, "sim_step", &alloc)) == "\"set\"");
        CHECK(number(j, "sim_step_ns", &alloc) == kHitch);
        CHECK(number(j, "input_reads", &alloc) == 1);
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
        CHECK(has(j, R"("input":"clock","guarantee":"event","needed":"yes","state":"recorded")"));
        CHECK(has(j, R"("input":"random","guarantee":"event","needed":"no","state":"not-needed")"));

        // The record holds the one read the run made before it failed: the sim domain's step, raw.
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.input_reads.size() == 1U);
        CHECK(rec.input_reads_total == 1U);
        CHECK(rec.input_reads[0] == ReplayInputRead{input::InputKind::TimeStep, kSim, true, kHitch});
        CHECK(rec.inputs[kClockIn].state == ck::ReplayInputState::Recorded);

        // Recording again gives the same bytes: the run failed before it read the live wall.
        REQUIRE(record(h, program, again, "7", clock).status == DiagStatus::Ok);
        CHECK(view(slurp(out, &alloc)) == view(slurp(again, &alloc)));
    }

    // A later process: a fresh service and Context, with the program file edited after the record was made.
    const String edit = replaced(view(text), kStepLine, kStepEdited, &alloc);
    spill(program, view(edit));
    {
        Host             h;
        const DiagResult r = replay(h, out);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "result", &alloc)) == "\"reproduced\"");
        CHECK(number(j, "recorded_input_reads", &alloc) == 1);
        CHECK(number(j, "replayed_input_reads", &alloc) == 1);
        const usize replayed = j.find(StringView{R"("run":"replayed")"});
        REQUIRE(replayed != StringView::npos);
        CHECK(view(field(j, "error", &alloc, replayed)) == "\"selector-out-of-range\"");
        CHECK(number(j, "line", &alloc, replayed) == sw.line);

        // The same reads against the edited file: its first read asks for the frame domain's step where the record
        // holds the sim domain's.
        const DiagResult e = replay(h, out, program);
        INFO(e.json.c_str());
        REQUIRE(e.status == DiagStatus::Ok);
        const StringView ej = view(e.json);
        CHECK(view(field(ej, "result", &alloc)) == "\"diverged\"");
        const usize at = ej.find(StringView{R"("kind":"divergence")"});
        REQUIRE(at != StringView::npos);
        CHECK(view(field(ej, "divergence", &alloc, at)) == "\"input\"");
        CHECK(number(ej, "index", &alloc, at) == 0);
        CHECK(number(ej, "recorded", &alloc, at) == kSim);
        CHECK(number(ej, "observed", &alloc, at) == kFrame);
        CHECK(view(field(ej, "recorded_input", &alloc, at)) == "\"time_step\"");
        CHECK(view(field(ej, "observed_input", &alloc, at)) == "\"time_step\"");
        CHECK(number(ej, "line", &alloc, at) == step.line);
        CHECK(number(ej, "col", &alloc, at) == step.col);
    }
    forget(out);
    forget(again);
    forget(program);
}

TEST_CASE("diag 9a clock: a live wall reading is fed back on replay, never read again",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const Where                        wall    = locate(view(text), kWallLine);
    const char* const                  program = "diag9a_clk_pass.ceir";
    const char* const                  out     = "diag9a_clk_pass.crpl";
    forget(out);
    spill(program, view(text));
    const String steady   = decimal(kSteady, &alloc);
    const String sim_time = decimal(kSimTime, &alloc);

    Host             h;
    const DiagResult r =
        record(h, program, out, "7", {{"clock", "wall"}, {"sim_time", view(sim_time)}, {"sim_step", view(steady)}});
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(view(field(view(r.json), "error", &alloc)) == "\"none\"");
    CHECK(number(view(r.json), "input_reads", &alloc) == 3);

    // The step, the live wall reading (nanoseconds since the run's clock was made) and the sim reading, in order.
    const ReplayRecord rec = decode_file(out, &alloc);
    REQUIRE(rec.input_reads.size() == 3U);
    CHECK(rec.input_reads[0] == ReplayInputRead{input::InputKind::TimeStep, kSim, true, kSteady});
    CHECK(rec.input_reads[1].kind == input::InputKind::Clock);
    CHECK(rec.input_reads[1].channel == kWall);
    CHECK(rec.input_reads[1].delivered);
    CHECK(rec.input_reads[1].value >= 0);
    CHECK(rec.input_reads[2] == ReplayInputRead{input::InputKind::Clock, kSim, true, kSimTime});
    const i64 w = rec.input_reads[1].value;
    REQUIRE(rec.results.size() == 1U);
    CHECK(rec.results[0] == w + kSimTime + 7);

    // The replay installs no clock at all: the result it returns is the recorded wall reading's.
    const DiagResult p = replay(h, out);
    INFO(p.json.c_str());
    REQUIRE(p.status == DiagStatus::Ok);
    CHECK(view(field(view(p.json), "result", &alloc)) == "\"reproduced\"");
    const Replayed fed = replay_feed(rec, &alloc);
    CHECK(fed.d.kind == ck::DivergenceKind::None);
    REQUIRE(fed.results.size() == 1U);
    CHECK(fed.results[0] == w + kSimTime + 7);

    // A record whose wall reading was changed: the replay returns the changed reading, named at the wall read.
    ReplayRecord tampered        = decode_file(out, &alloc);
    tampered.input_reads[1].value = w + 1000;
    const Replayed t             = replay_feed(tampered, &alloc);
    CHECK(t.d.kind == ck::DivergenceKind::Value);
    CHECK(t.d.recorded == w);
    CHECK(t.d.observed == w + 1000);
    CHECK(t.line == wall.line);
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a clock: a host without a clock fails the read at its op and records that read",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const Where                        step    = locate(view(text), kStepLine);
    const char* const                  program = "diag9a_clk_none.ceir";
    const char* const                  out     = "diag9a_clk_none.crpl";
    forget(out);
    spill(program, view(text));

    Host h;
    SECTION("no clock arguments: no domain has a step")
    {
        const DiagResult r = record(h, program, out, "7");
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"input-unavailable\"");
        CHECK(number(j, "fault_line", &alloc) == step.line);
        CHECK(number(j, "fault_col", &alloc) == step.col);
        CHECK(view(field(j, "wall_clock", &alloc)) == "\"none\"");
        CHECK(view(field(j, "sim_step", &alloc)) == "\"none\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
    }
    SECTION("a sim reading without a step: the step read still has no value")
    {
        const DiagResult r = record(h, program, out, "7", {{"sim_time", "5"}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(view(field(view(r.json), "error", &alloc)) == "\"input-unavailable\"");
        CHECK(view(field(view(r.json), "sim_time", &alloc)) == "\"set\"");
    }
    const ReplayRecord rec = decode_file(out, &alloc);
    REQUIRE(rec.input_reads.size() == 1U);
    CHECK(rec.input_reads[0] == ReplayInputRead{input::InputKind::TimeStep, kSim, false, 0});
    CHECK(rec.inputs[kClockIn].state == ck::ReplayInputState::Recorded);
    const DiagResult p = replay(h, out);
    INFO(p.json.c_str());
    REQUIRE(p.status == DiagStatus::Ok);
    const StringView pj = view(p.json);
    CHECK(view(field(pj, "result", &alloc)) == "\"reproduced\"");
    const usize replayed = pj.find(StringView{R"("run":"replayed")"});
    REQUIRE(replayed != StringView::npos);
    CHECK(view(field(pj, "error", &alloc, replayed)) == "\"input-unavailable\"");
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a clock: an incomplete or uncaptured clock is stored missing and the replay is refused",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;

    SECTION("a run that read the clock more often than a record keeps")
    {
        const char* const program = "diag9a_clk_many.ceir";
        const char* const out     = "diag9a_clk_many.crpl";
        forget(out);
        spill(program, StringView{kManyReads});
        Host             h;
        const DiagResult r = record(h, program, out, "65537", {{"sim_time", "9"}, {"max_events", "16"}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"none\"");
        CHECK(number(j, "input_reads", &alloc) == 65537);
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"clock\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"incomplete\"");
        CHECK(has(j, R"("input":"clock","guarantee":"event","needed":"yes","state":"missing")"));
        const ReplayRecord rec = decode_file(out, &alloc);
        CHECK(rec.input_reads.size() == ck::kReplayMaxInputReads);
        CHECK(rec.input_reads_total == 65537U);
        const DiagResult p = replay(h, out);
        CHECK(p.status == DiagStatus::Unavailable);
        CHECK(has(view(p.json), "missing inputs its program needs (clock)"));
        forget(out);
        forget(program);
    }
    SECTION("held or not, and a time read outside the seam")
    {
        Context ctx(&alloc);
        registrar(ctx, nullptr);
        // tp.now declares TimeRead itself: nothing at the input seam sees what it reads.
        crd::ceir::Dialect* const d = ctx.register_dialect("tp");
        (void)d->register_op("now", {.effects     = ConstSpan<crd::ceir::EffectRecord>(kTimeRead, 1U),
                                     .determinism = crd::ceir::DeterminismClass::ExternalNondeterminism});
        const String                 seam = slurp(kAsset, &alloc);
        const crd::ceir::ParseResult a    = crd::ceir::parse(ctx, view(seam));
        REQUIRE(a.module != nullptr);
        const crd::ceir::ParseResult b = crd::ceir::parse(
            ctx, StringView{"module {\n  ^bb0:\n    func.func() {sym_name = \"main\"} {\n      ^bb0:\n"
                            "        %0 = input.clock() {domain = \"sim\"} : !i64\n"
                            "        %1 = tp.now() : !i64\n        func.return(%0)\n    }\n}\n"});
        REQUIRE(b.module != nullptr);

        const auto clock_state = [&](const crd::ceir::Module& m, bool held, String& missing)
        {
            ck::ReplayInput inputs[ck::kReplayInputs];
            missing.clear();
            REQUIRE(ck::classify_replay_inputs(ctx, m, ck::ReplayExecutorKind::Plan, held, &alloc, nullptr, inputs,
                                               &missing));
            CHECK(inputs[kClockIn].need == ck::ReplayNeed::Yes);
            return inputs[kClockIn].state;
        };
        String missing(&alloc);
        CHECK(clock_state(*a.module, true, missing) == ck::ReplayInputState::Recorded);
        CHECK(view(missing).empty());
        CHECK(clock_state(*a.module, false, missing) == ck::ReplayInputState::Missing);
        CHECK(view(missing) == "clock");
        CHECK(clock_state(*b.module, true, missing) == ck::ReplayInputState::Missing);
        CHECK(view(missing) == "clock");
    }
}

TEST_CASE("diag 9a clock: malformed clock arguments are refused before anything is read or run",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const char* const                  out = "diag9a_clk_refused.crpl";
    forget(out);
    Host h;
    // The program path does not exist: a request that got past its arguments would fail on the read instead.
    const char* const program = "diag9a_clk_absent.ceir";
    forget(program);
    const DiagArg bad[] = {{"clock", "sim"},       {"clock", ""},         {"sim_time", "1.5"},
                           {"sim_time", "x"},      {"sim_step", ""},      {"sim_step", "99999999999999999999"}};
    for (const DiagArg& a : bad)
    {
        const DiagResult r = record(h, program, out, "7", {a});
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::BadArgument);
    }
    CHECK(h.cmd.record_runs.load() == 0U);
    CHECK(h.cmd.executions.load() == 0U);
    CHECK(h.cmd.records_written.load() == 0U);
    CHECK_FALSE(exists(out));
}

TEST_CASE("diag 9a clock: the record format refuses a time read of an unknown domain",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const char* const                  program = "diag9a_clk_format.ceir";
    const char* const                  out     = "diag9a_clk_format.crpl";
    forget(out);
    spill(program, view(text));
    Host h;
    REQUIRE(record(h, program, out, "7", {{"sim_step", "5"}, {"sim_time", "6"}, {"clock", "wall"}}).status ==
            DiagStatus::Ok);

    const auto decode = [&](const ReplayRecord& rec)
    {
        Array<u8> bytes(&alloc);
        ck::encode_record(rec, bytes);
        ReplayRecord back(&alloc);
        return ck::decode_record({bytes.data(), bytes.size()}, back);
    };
    ReplayRecord rec = decode_file(out, &alloc);
    REQUIRE(rec.input_reads.size() == 3U);
    CHECK(decode(rec) == ck::RecordError::Ok);
    rec.input_reads[2].channel = crd::ceir::time::kBuiltinDomainCount; // past the built-in domains
    CHECK(decode(rec) == ck::RecordError::Malformed);
    rec                        = decode_file(out, &alloc);
    rec.input_reads[0].channel = crd::ceir::time::kBuiltinDomainCount - 1U; // logical: a valid domain
    CHECK(decode(rec) == ck::RecordError::Ok);
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a clock: the inspect host given a host clock records the reads of the unobserved run",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const Where                        sw      = locate(view(text), kSwitchLine);
    const char* const                  program = "diag9a_clk_insp.ceir";
    const char* const                  plain_at = "diag9a_clk_insp.crpl";
    forget(plain_at);
    spill(program, view(text));
    const String hitch = decimal(kHitch, &alloc);

    // The unobserved run: replay.record with the sim step over budget, no session.
    Host h;
    REQUIRE(record(h, program, plain_at, "7", {{"sim_step", view(hitch)}}).status == DiagStatus::Ok);
    const ReplayRecord plain = decode_file(plain_at, &alloc);

    // The same run on the inspect host, reading a host clock with the same step. The clock never allocates.
    input::HostClock clock;
    clock.set_step(kSim, kHitch);
    ck::InspectHost host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(ck::AssetId{9300U}, view(text), StringView{program}, "main").ok());
    const i64 args[1] = {7};
    REQUIRE(host.start({args, 1U}, ck::HostRecording{true, 0U}, clock.source()) ==
            crd::ceir::inspect::Refusal::None);
    REQUIRE(host.wait_finished(20000U));
    CHECK(host.result().error == crd::ceir::plan::RunError::SelectorOutOfRange);

    ReplayRecord rec(&alloc);
    REQUIRE(host.record(rec) == ck::HostRecord::Ok);
    CHECK(rec.inputs[kClockIn].state == ck::ReplayInputState::Recorded);
    REQUIRE(rec.input_reads.size() == 1U);
    CHECK(rec.input_reads[0] == plain.input_reads[0]);
    CHECK(rec.input_reads_total == plain.input_reads_total);
    CHECK(rec.events_total == plain.events_total);
    CHECK(rec.fault_op == plain.fault_op);
    String missing(&alloc);
    CHECK(ck::record_missing_inputs(rec, missing));
    const Replayed fed = replay_feed(rec, &alloc);
    CHECK(fed.d.kind == ck::DivergenceKind::None);
    Context           ctx(&alloc);
    ck::ReplayProgram p(&alloc);
    ck::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main", &registrar, nullptr, p);
    REQUIRE(p.ok());
    CHECK(ck::replay_site_of_op(ctx, p, rec.fault_op).line == sw.line);
    forget(plain_at);
    forget(program);
}

TEST_CASE("diag 9a clock: an inspected run that reads the clock more often than a record keeps stores it missing",
          "[ceir][cook][diag][input][clock]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    input::HostClock                   clock;
    clock.set_reading(kSim, 9);
    ck::InspectHost host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(ck::AssetId{9301U}, StringView{kManyReads}, "many.ceir", "main").ok());
    const i64 args[1] = {static_cast<i64>(ck::kReplayMaxInputReads) + 1};
    REQUIRE(host.start({args, 1U}, ck::HostRecording{true, 16U}, clock.source()) ==
            crd::ceir::inspect::Refusal::None);
    REQUIRE(host.wait_finished(120000U));
    CHECK(host.result().error == crd::ceir::plan::RunError::None);

    ReplayRecord rec(&alloc);
    REQUIRE(host.record(rec) == ck::HostRecord::Ok);
    CHECK(rec.input_reads_total == ck::kReplayMaxInputReads + 1U);
    CHECK(rec.input_reads.size() == ck::kReplayMaxInputReads);
    CHECK(rec.inputs[kClockIn].state == ck::ReplayInputState::Missing);
    String missing(&alloc);
    CHECK_FALSE(ck::record_missing_inputs(rec, missing));
    CHECK(view(missing) == "clock");
}

TEST_CASE("diag 9a clock: the shared clock arguments parse whole, and a one-shot host's inputs route them",
          "[ceir][cook][diag][input][clock]")
{
    using ck::ClockArgument;
    StringView requirement;

    // Only the three clock names are clock arguments; each value is checked whole.
    CHECK(ck::parse_clock_argument("seed", "1", nullptr, requirement) == ClockArgument::NotClock);
    CHECK(ck::parse_clock_argument("Clock", "wall", nullptr, requirement) == ClockArgument::NotClock);
    struct Bad
    {
        const char* name;
        const char* value;
    };
    const Bad bad_values[] = {
        {"clock", "frame"}, {"clock", ""},      {"clock", "wall "}, {"sim_time", "x"},
        {"sim_time", ""},   {"sim_step", "1.5"}, {"sim_step", "+1"}, {"sim_time", "9223372036854775808"},
    };
    for (const Bad& bad : bad_values)
    {
        INFO(bad.name << "=" << bad.value);
        ck::HostClockSpec spec;
        requirement = StringView{};
        CHECK(ck::parse_clock_argument(StringView{bad.name}, StringView{bad.value}, &spec, requirement) ==
              ClockArgument::Refused);
        CHECK_FALSE(requirement.empty());
        CHECK_FALSE(spec.live_wall);
        CHECK_FALSE(spec.has_sim_time);
        CHECK_FALSE(spec.has_sim_step);
    }
    ck::HostClockSpec spec;
    CHECK(ck::parse_clock_argument("clock", "wall", &spec, requirement) == ClockArgument::Ok);
    CHECK(ck::parse_clock_argument("sim_time", "-9223372036854775808", &spec, requirement) == ClockArgument::Ok);
    CHECK(ck::parse_clock_argument("sim_step", "50000000", &spec, requirement) == ClockArgument::Ok);
    CHECK(ck::parse_clock_argument("sim_step", "7", nullptr, requirement) == ClockArgument::Ok); // validates only
    CHECK(spec.live_wall);
    CHECK(spec.has_sim_time);
    CHECK(spec.sim_time == -9223372036854775807LL - 1);
    CHECK(spec.has_sim_step);
    CHECK(spec.sim_step == 50000000);

    // A RunInputs routes a seed's streams and a spec's domains; every `set` starts the next run over.
    using K = input::InputKind;
    ck::RunInputs             inputs("diag9a-run-inputs");
    const input::InputSource* src = inputs.source();
    i64                       v   = 0;
    inputs.set(false, 0U, ck::HostClockSpec{});
    CHECK_FALSE(input::read_input(src, K::Random, 0U, v));
    CHECK_FALSE(input::read_input(src, K::Clock, kSim, v));
    CHECK_FALSE(input::read_input(src, K::TimeStep, kSim, v));

    ck::HostClockSpec sim;
    sim.has_sim_time = true;
    sim.sim_time     = kSimTime;
    sim.has_sim_step = true;
    sim.sim_step     = kHitch;
    for (u32 run = 0U; run < 2U; ++run)
    {
        inputs.set(true, 7U, sim);
        REQUIRE(input::read_input(src, K::Random, 0U, v));
        CHECK(v == input::SeededInputs::draw(7U, 0U, 0U));
        REQUIRE(input::read_input(src, K::Random, 0U, v));
        CHECK(v == input::SeededInputs::draw(7U, 0U, 1U));
        REQUIRE(input::read_input(src, K::Clock, kSim, v));
        CHECK(v == kSimTime);
        REQUIRE(input::read_input(src, K::TimeStep, kSim, v));
        CHECK(v == kHitch);
        CHECK_FALSE(input::read_input(src, K::Clock, kWall, v));
        CHECK_FALSE(input::read_input(src, K::Clock, kFrame, v));
    }

    // Another run: no seed, a live wall only; the sim domain the last run set is gone.
    ck::HostClockSpec wall;
    wall.live_wall = true;
    inputs.set(false, 7U, wall);
    CHECK_FALSE(input::read_input(src, K::Random, 0U, v));
    REQUIRE(input::read_input(src, K::Clock, kWall, v));
    CHECK(v >= 0);
    CHECK_FALSE(input::read_input(src, K::Clock, kSim, v));
    CHECK_FALSE(input::read_input(src, K::TimeStep, kSim, v));

    // A host that sets more domains than a spec does (the sandbox's frame clock) sets them on its clock.
    inputs.clock().set_reading(ck::kFrameDomain, 42);
    inputs.clock().set_step(ck::kFrameDomain, 1);
    REQUIRE(input::read_input(src, K::Clock, kFrame, v));
    CHECK(v == 42);
    REQUIRE(input::read_input(src, K::TimeStep, kFrame, v));
    CHECK(v == 1);
    CHECK(ck::kWallDomain == kWall);
    CHECK(ck::kSimDomain == kSim);
    CHECK(ck::kFrameDomain == kFrame);
    u32 ordinal = 99U;
    REQUIRE(crd::ceir::time::builtin_domain_index("frame", ordinal));
    CHECK(ordinal == ck::kFrameDomain);
}
