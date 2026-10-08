// DIAG.9a -- recorded input events. A program takes the next event of a host input event queue (input.event) through
// the host input seam; a run record keeps every event read, raw and in order, and a replay feeds them back instead of
// taking them from a live queue, so a failure that depends on what the user did reproduces in a later process from the
// record alone, even after the program file was edited.
//
// The committed authored program assets/ceir/event_demo.ceir takes two events from queue 0 and switches on whether the
// second is a resize (type 8; one case, so a resize is out of range), then returns the first event's code plus the
// second's x and y plus its argument. Lines and columns come from scanning the text; packed events are spelled out here
// from the layout, independently of input::pack_event. The cases: an unhandled resize recorded and reproduced after a
// restart and an edit (the edit takes from another queue, named at its read); a passing run whose events the replay
// feeds back, and a tampered event named at its read; no queue and an empty queue; host-state stored missing when a
// run read past the bound or a scene read bypasses the seam, and the replay refused; malformed events refused before
// anything runs, and the parser's forms and bounds; the inspect host, given an event queue, recording exactly the
// unobserved run's reads; and a network effect beside an event read keeping external results missing, so a record
// that lacks them is refused before anything runs (never rerun). RunInputs, the input bundle of the interactive hosts,
// gives each run the event queue its spec describes, from the first event, and lets a host queue more between runs.
// ASCII test names (ctest by-name).

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

constexpr const char* kAsset = CRD_REPO_DIR "/assets/ceir/event_demo.ceir";

// The two event reads, the edit that makes the second take from queue 1 instead, and the switch on the second's type.
constexpr const char* kFirstLine   = "%1, %2, %3, %4, %5 = input.event() {queue = 0} : !i64";
constexpr const char* kSecondLine  = "%6, %7, %8, %9, %10 = input.event() {queue = 0} : !i64";
constexpr const char* kSecondEdit  = "%6, %7, %8, %9, %10 = input.event() {queue = 1} : !i64";
constexpr const char* kSwitchLine  = "core.switch(%12)";
constexpr u32         kHostStateIn = 5U; // the host-state input's slot in a record
constexpr u32         kExternalIn  = 6U; // the external-results input's slot

// The effect of a probe op that reads scene state itself, outside the input seam.
constexpr crd::ceir::EffectRecord kSceneRead[] = {
    {crd::ceir::EffectFamily::SceneRead, crd::ceir::EffectTarget::None, 0U, 0U}};
// The effect of a probe op that sends over a network: its results are external, never a host input.
constexpr crd::ceir::EffectRecord kNetworkIo[] = {
    {crd::ceir::EffectFamily::NetworkIO, crd::ceir::EffectTarget::None, 0U, 0U}};

// main(n) takes n events from queue 0 and returns 0.
constexpr const char* kManyReads = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i64):
        %1 = arith.const() {value = 0} : !i64
        %2 = arith.const() {value = 1} : !i64
        core.for(%1, %0, %2) {
          ^bb0(%3 : !i64):
            %4, %5, %6, %7, %8 = input.event() {queue = 0} : !i64
            core.yield()
        }
        func.return(%1)
    }
})";

// The packed layout, spelled out here independently of input::pack_event: type, code << 8, mods << 24, x << 32 and
// y << 48, each of x and y as 16 two's-complement bits.
i64 packed(u64 type, u64 code, u64 mods, i64 x, i64 y)
{
    const u64 ux = static_cast<u64>(x) & 0xFFFFU;
    const u64 uy = static_cast<u64>(y) & 0xFFFFU;
    return static_cast<i64>(type | (code << 8U) | (mods << 24U) | (ux << 32U) | (uy << 48U));
}

ReplayInputRead event_read(i64 raw, u32 queue = 0U, bool delivered = true)
{
    return ReplayInputRead{input::InputKind::Event, queue, delivered, raw};
}

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

TEST_CASE("diag 9a event: an unhandled event is recorded with its reads and reproduces after a restart and an edit",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text   = slurp(kAsset, &alloc);
    const Where                        second = locate(view(text), kSecondLine);
    const Where                        sw     = locate(view(text), kSwitchLine);

    const char* const program = "diag9a_ev_demo.ceir";
    const char* const out     = "diag9a_ev_fail.crpl";
    const char* const again   = "diag9a_ev_again.crpl";
    forget(out);
    forget(again);
    spill(program, view(text));

    // A ctrl+shift key_down of key 65, then a resize the program has no case for.
    const std::initializer_list<DiagArg> events = {{"events", "key_down:65:3,resize:1280:720"}};
    {
        Host             h;
        const DiagResult r = record(h, program, out, "7", events);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"selector-out-of-range\"");
        CHECK(number(j, "fault_line", &alloc) == sw.line);
        CHECK(number(j, "fault_col", &alloc) == sw.col);
        CHECK(view(field(j, "event_queue", &alloc)) == "\"open\"");
        CHECK(number(j, "input_events", &alloc) == 2);
        CHECK(number(j, "input_reads", &alloc) == 2);
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
        CHECK(has(j, R"("input":"host-state","guarantee":"event","needed":"yes","state":"recorded")"));
        CHECK(has(j, R"("input":"random","guarantee":"event","needed":"no","state":"not-needed")"));
        CHECK(has(j, R"("input":"clock","guarantee":"event","needed":"no","state":"not-needed")"));

        // The record holds both reads, raw and in order.
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.input_reads.size() == 2U);
        CHECK(rec.input_reads_total == 2U);
        CHECK(rec.input_reads[0] == event_read(packed(1U, 65U, 3U, 0, 0)));
        CHECK(rec.input_reads[1] == event_read(packed(8U, 0U, 0U, 1280, 720)));
        CHECK(rec.inputs[kHostStateIn].state == ck::ReplayInputState::Recorded);

        // Recording again gives the same bytes.
        REQUIRE(record(h, program, again, "7", events).status == DiagStatus::Ok);
        CHECK(view(slurp(out, &alloc)) == view(slurp(again, &alloc)));
    }

    // A later process: a fresh service and Context, with the program file edited after the record was made.
    const String edit = replaced(view(text), kSecondLine, kSecondEdit, &alloc);
    spill(program, view(edit));
    {
        Host             h;
        const DiagResult r = replay(h, out);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "result", &alloc)) == "\"reproduced\"");
        CHECK(number(j, "recorded_input_reads", &alloc) == 2);
        CHECK(number(j, "replayed_input_reads", &alloc) == 2);
        const usize replayed = j.find(StringView{R"("run":"replayed")"});
        REQUIRE(replayed != StringView::npos);
        CHECK(view(field(j, "error", &alloc, replayed)) == "\"selector-out-of-range\"");
        CHECK(number(j, "line", &alloc, replayed) == sw.line);

        // The same reads against the edited file: its second read takes from queue 1 where the record holds queue 0.
        const DiagResult e = replay(h, out, program);
        INFO(e.json.c_str());
        REQUIRE(e.status == DiagStatus::Ok);
        const StringView ej = view(e.json);
        CHECK(view(field(ej, "result", &alloc)) == "\"diverged\"");
        const usize at = ej.find(StringView{R"("kind":"divergence")"});
        REQUIRE(at != StringView::npos);
        CHECK(view(field(ej, "divergence", &alloc, at)) == "\"input\"");
        CHECK(number(ej, "index", &alloc, at) == 1);
        CHECK(number(ej, "recorded", &alloc, at) == 0);
        CHECK(number(ej, "observed", &alloc, at) == 1);
        CHECK(view(field(ej, "recorded_input", &alloc, at)) == "\"event\"");
        CHECK(view(field(ej, "observed_input", &alloc, at)) == "\"event\"");
        CHECK(number(ej, "line", &alloc, at) == second.line);
        CHECK(number(ej, "col", &alloc, at) == second.col);
    }
    forget(out);
    forget(again);
    forget(program);
}

TEST_CASE("diag 9a event: the recorded events are fed back on replay, never taken from a live queue",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const Where                        second  = locate(view(text), kSecondLine);
    const char* const                  program = "diag9a_ev_pass.ceir";
    const char* const                  out     = "diag9a_ev_pass.crpl";
    forget(out);
    spill(program, view(text));

    // key 65, then a pointer move to (10, -20): main(7) returns 65 + 10 - 20 + 7.
    Host             h;
    const DiagResult r = record(h, program, out, "7", {{"events", "key_down:65,mouse_move:10:-20"}});
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(view(field(view(r.json), "error", &alloc)) == "\"none\"");
    const ReplayRecord rec = decode_file(out, &alloc);
    REQUIRE(rec.input_reads.size() == 2U);
    CHECK(rec.input_reads[0] == event_read(packed(1U, 65U, 0U, 0, 0)));
    CHECK(rec.input_reads[1] == event_read(packed(6U, 0U, 0U, 10, -20)));
    REQUIRE(rec.results.size() == 1U);
    CHECK(rec.results[0] == 65 + 10 - 20 + 7);

    // The replay installs no event queue at all: what it returns comes from the record's reads.
    const DiagResult p = replay(h, out);
    INFO(p.json.c_str());
    REQUIRE(p.status == DiagStatus::Ok);
    CHECK(view(field(view(p.json), "result", &alloc)) == "\"reproduced\"");
    const Replayed fed = replay_feed(rec, &alloc);
    CHECK(fed.d.kind == ck::DivergenceKind::None);
    REQUIRE(fed.results.size() == 1U);
    CHECK(fed.results[0] == 65 + 10 - 20 + 7);

    // A record whose second event was changed (x 10 to 11): the replay takes the changed event, and the first result
    // that differs is named at the second read.
    ReplayRecord tampered         = decode_file(out, &alloc);
    tampered.input_reads[1].value = packed(6U, 0U, 0U, 11, -20);
    const Replayed t              = replay_feed(tampered, &alloc);
    CHECK(t.d.kind == ck::DivergenceKind::Value);
    CHECK(t.d.recorded == 10);
    CHECK(t.d.observed == 11);
    CHECK(t.line == second.line);
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a event: no event queue fails the first read at its op; an empty queue reads none events",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const Where                        first   = locate(view(text), kFirstLine);
    const char* const                  program = "diag9a_ev_none.ceir";
    const char* const                  out     = "diag9a_ev_none.crpl";
    forget(out);
    spill(program, view(text));
    Host h;

    SECTION("no events argument: the host has no queue")
    {
        const DiagResult r = record(h, program, out, "7");
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"input-unavailable\"");
        CHECK(number(j, "fault_line", &alloc) == first.line);
        CHECK(number(j, "fault_col", &alloc) == first.col);
        CHECK(view(field(j, "event_queue", &alloc)) == "\"none\"");
        CHECK(number(j, "input_events", &alloc) == 0);
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.input_reads.size() == 1U);
        CHECK(rec.input_reads[0] == event_read(0, 0U, false));
        CHECK(rec.inputs[kHostStateIn].state == ck::ReplayInputState::Recorded);
        const DiagResult p = replay(h, out);
        INFO(p.json.c_str());
        REQUIRE(p.status == DiagStatus::Ok);
        const StringView pj = view(p.json);
        CHECK(view(field(pj, "result", &alloc)) == "\"reproduced\"");
        const usize replayed = pj.find(StringView{R"("run":"replayed")"});
        REQUIRE(replayed != StringView::npos);
        CHECK(view(field(pj, "error", &alloc, replayed)) == "\"input-unavailable\"");
    }
    SECTION("an empty events argument: an open queue with nothing in it")
    {
        const DiagResult r = record(h, program, out, "7", {{"events", ""}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"none\"");
        CHECK(view(field(j, "event_queue", &alloc)) == "\"open\"");
        CHECK(number(j, "input_events", &alloc) == 0);
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.input_reads.size() == 2U);
        CHECK(rec.input_reads[0] == event_read(0));
        CHECK(rec.input_reads[1] == event_read(0));
        REQUIRE(rec.results.size() == 1U);
        CHECK(rec.results[0] == 7);
        CHECK(view(field(view(replay(h, out).json), "result", &alloc)) == "\"reproduced\"");
    }
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a event: incomplete or uncaptured host state is stored missing and the replay is refused",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;

    SECTION("a run that took more events than a record keeps")
    {
        const char* const program = "diag9a_ev_many.ceir";
        const char* const out     = "diag9a_ev_many.crpl";
        forget(out);
        spill(program, StringView{kManyReads});
        Host             h;
        const DiagResult r = record(h, program, out, "65537", {{"events", ""}, {"max_events", "16"}});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"none\"");
        CHECK(number(j, "input_reads", &alloc) == 65537);
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"host-state\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"incomplete\"");
        CHECK(has(j, R"("input":"host-state","guarantee":"event","needed":"yes","state":"missing")"));
        const ReplayRecord rec = decode_file(out, &alloc);
        CHECK(rec.input_reads.size() == ck::kReplayMaxInputReads);
        CHECK(rec.input_reads_total == 65537U);
        const DiagResult p = replay(h, out);
        CHECK(p.status == DiagStatus::Unavailable);
        CHECK(has(view(p.json), "missing inputs its program needs (host-state)"));
        forget(out);
        forget(program);
    }
    SECTION("held or not, and a scene read outside the seam")
    {
        Context ctx(&alloc);
        registrar(ctx, nullptr);
        // hp.scene declares SceneRead itself: nothing at the input seam sees what it reads.
        crd::ceir::Dialect* const d = ctx.register_dialect("hp");
        (void)d->register_op("scene", {.effects     = ConstSpan<crd::ceir::EffectRecord>(kSceneRead, 1U),
                                       .determinism = crd::ceir::DeterminismClass::ExternalNondeterminism});
        const String                 seam = slurp(kAsset, &alloc);
        const crd::ceir::ParseResult a    = crd::ceir::parse(ctx, view(seam));
        REQUIRE(a.module != nullptr);
        const crd::ceir::ParseResult b = crd::ceir::parse(
            ctx, StringView{"module {\n  ^bb0:\n    func.func() {sym_name = \"main\"} {\n      ^bb0:\n"
                            "        %0, %1, %2, %3, %4 = input.event() {queue = 0} : !i64\n"
                            "        %5 = hp.scene() : !i64\n        func.return(%0)\n    }\n}\n"});
        REQUIRE(b.module != nullptr);

        const auto host_state = [&](const crd::ceir::Module& m, bool held, String& missing)
        {
            ck::ReplayInput inputs[ck::kReplayInputs];
            missing.clear();
            REQUIRE(ck::classify_replay_inputs(ctx, m, ck::ReplayExecutorKind::Plan, held, &alloc, nullptr, inputs,
                                               &missing));
            CHECK(inputs[kHostStateIn].need == ck::ReplayNeed::Yes);
            return inputs[kHostStateIn].state;
        };
        String missing(&alloc);
        CHECK(host_state(*a.module, true, missing) == ck::ReplayInputState::Recorded);
        CHECK(view(missing).empty());
        CHECK(host_state(*a.module, false, missing) == ck::ReplayInputState::Missing);
        CHECK(view(missing) == "host-state");
        CHECK(host_state(*b.module, true, missing) == ck::ReplayInputState::Missing);
        CHECK(view(missing) == "host-state");
    }
}

TEST_CASE("diag 9a event: malformed events are refused before anything is read or run",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const char* const                  out = "diag9a_ev_refused.crpl";
    forget(out);
    Host h;
    // The program path does not exist: a request that got past its arguments would fail on the read instead.
    const char* const program = "diag9a_ev_absent.ceir";
    forget(program);
    String too_many(&alloc);
    for (u32 i = 0U; i <= ck::kMaxArgumentEvents; ++i)
    {
        too_many.append(i == 0U ? "key_up:1" : ",key_up:1");
    }
    const char* const bad[] = {"key_down",           "key_down:65536",  "key_down:65:16",  "key_down:65:1:2",
                               "key_down:-1",        "key_down:65:",    "mouse_move:1",    "mouse_move:40000:0",
                               "scroll:0:-32769",    "resize:1:2:3",    "none:0",          "click:1",
                               "key_down:65,,key_up:65", ",",           "KEY_DOWN:65",     "mouse_down:1.5"};
    for (const char* const v : bad)
    {
        const DiagResult r = record(h, program, out, "7", {{"events", StringView{v}}});
        INFO(v);
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::BadArgument);
    }
    const DiagResult r = record(h, program, out, "7", {{"events", view(too_many)}});
    CHECK(r.status == DiagStatus::BadArgument);
    CHECK(h.cmd.record_runs.load() == 0U);
    CHECK(h.cmd.executions.load() == 0U);
    CHECK(h.cmd.records_written.load() == 0U);
    CHECK_FALSE(exists(out));

    // The parser itself: every event form, and the bounds of each field, packed as the layout says.
    Array<i64> events(&alloc);
    StringView requirement;
    REQUIRE(ck::parse_events_argument("key_down:65:15,key_up:0,key_repeat:65535,mouse_down:1:2,mouse_up:4,"
                                      "mouse_move:-32768:32767,scroll:0:-150,resize:1920:1080",
                                      &events, requirement));
    const i64 want[] = {packed(1U, 65U, 15U, 0, 0),     packed(2U, 0U, 0U, 0, 0),   packed(3U, 65535U, 0U, 0, 0),
                        packed(4U, 1U, 2U, 0, 0),       packed(5U, 4U, 0U, 0, 0),   packed(6U, 0U, 0U, -32768, 32767),
                        packed(7U, 0U, 0U, 0, -150),    packed(8U, 0U, 0U, 1920, 1080)};
    REQUIRE(events.size() == 8U);
    for (usize i = 0U; i < 8U; ++i)
    {
        CHECK(events[i] == want[i]);
    }
    events.clear();
    REQUIRE(ck::parse_events_argument("", &events, requirement));
    CHECK(events.empty());
    REQUIRE(ck::parse_events_argument(view(too_many).substr(0U, view(too_many).size() - 9U), &events, requirement));
    CHECK(events.size() == ck::kMaxArgumentEvents);
    CHECK_FALSE(ck::parse_events_argument("mouse_move:1", nullptr, requirement));
    CHECK(has(requirement, "mouse_move, scroll or resize:<x>:<y>"));
}

TEST_CASE("diag 9a event: the inspect host given an event queue records the reads of the unobserved run",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text     = slurp(kAsset, &alloc);
    const Where                        sw       = locate(view(text), kSwitchLine);
    const char* const                  program  = "diag9a_ev_insp.ceir";
    const char* const                  plain_at = "diag9a_ev_insp.crpl";
    forget(plain_at);
    spill(program, view(text));

    // The unobserved run: replay.record with the unhandled resize, no session.
    Host h;
    REQUIRE(record(h, program, plain_at, "7", {{"events", "key_down:65:3,resize:1280:720"}}).status ==
            DiagStatus::Ok);
    const ReplayRecord plain = decode_file(plain_at, &alloc);
    REQUIRE(plain.input_reads.size() == 2U);

    // The same run on the inspect host, taking the same events from a host queue. Reads never allocate.
    input::HostEvents events(&alloc);
    REQUIRE(events.push(0U, packed(1U, 65U, 3U, 0, 0)));
    REQUIRE(events.push(0U, packed(8U, 0U, 0U, 1280, 720)));
    ck::InspectHost host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(ck::AssetId{9400U}, view(text), StringView{program}, "main").ok());
    const i64 args[1] = {7};
    REQUIRE(host.start({args, 1U}, ck::HostRecording{true, 0U}, events.source()) ==
            crd::ceir::inspect::Refusal::None);
    REQUIRE(host.wait_finished(20000U));
    CHECK(host.result().error == crd::ceir::plan::RunError::SelectorOutOfRange);

    ReplayRecord rec(&alloc);
    REQUIRE(host.record(rec) == ck::HostRecord::Ok);
    CHECK(rec.inputs[kHostStateIn].state == ck::ReplayInputState::Recorded);
    REQUIRE(rec.input_reads.size() == 2U);
    CHECK(rec.input_reads[0] == plain.input_reads[0]);
    CHECK(rec.input_reads[1] == plain.input_reads[1]);
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

TEST_CASE("diag 9a event: external results are no seam input, and a record missing them is refused before it runs",
          "[ceir][cook][diag][input][event]")
{
    crd::memory::GrowableTlsfAllocator alloc;

    // xr.send declares NetworkIO: an event read beside it records host-state, never the network's results.
    Context ctx(&alloc);
    registrar(ctx, nullptr);
    crd::ceir::Dialect* const d = ctx.register_dialect("xr");
    (void)d->register_op("send", {.effects     = ConstSpan<crd::ceir::EffectRecord>(kNetworkIo, 1U),
                                  .determinism = crd::ceir::DeterminismClass::ExternalNondeterminism});
    const crd::ceir::ParseResult m = crd::ceir::parse(
        ctx, StringView{"module {\n  ^bb0:\n    func.func() {sym_name = \"main\"} {\n      ^bb0:\n"
                        "        %0, %1, %2, %3, %4 = input.event() {queue = 0} : !i64\n"
                        "        %5 = xr.send(%0) : !i64\n        func.return(%5)\n    }\n}\n"});
    REQUIRE(m.module != nullptr);
    ck::ReplayInput inputs[ck::kReplayInputs];
    String          missing(&alloc);
    REQUIRE(ck::classify_replay_inputs(ctx, *m.module, ck::ReplayExecutorKind::Plan, true, &alloc, nullptr, inputs,
                                       &missing));
    CHECK(inputs[kHostStateIn].state == ck::ReplayInputState::Recorded);
    CHECK(inputs[kExternalIn].need == ck::ReplayNeed::Yes);
    CHECK(inputs[kExternalIn].state == ck::ReplayInputState::Missing);
    CHECK(view(missing) == "external-results");

    // A record that says its program needs external results it does not hold is refused, and nothing runs.
    const String      text    = slurp(kAsset, &alloc);
    const char* const program = "diag9a_ev_ext.ceir";
    const char* const out     = "diag9a_ev_ext.crpl";
    const char* const forged  = "diag9a_ev_ext_forged.crpl";
    forget(out);
    forget(forged);
    spill(program, view(text));
    Host h;
    REQUIRE(record(h, program, out, "7", {{"events", "key_down:65"}}).status == DiagStatus::Ok);
    ReplayRecord rec         = decode_file(out, &alloc);
    rec.inputs[kExternalIn] = {ck::ReplayNeed::Yes, ck::ReplayInputState::Missing};
    Array<u8> bytes(&alloc);
    ck::encode_record(rec, bytes);
    spill(forged, StringView{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    const u64        before = h.cmd.executions.load();
    const DiagResult p      = replay(h, forged);
    INFO(p.json.c_str());
    CHECK(p.status == DiagStatus::Unavailable);
    CHECK(has(view(p.json), "missing inputs its program needs (external-results)"));
    CHECK(h.cmd.executions.load() == before);
    forget(out);
    forget(forged);
    forget(program);
}

TEST_CASE("diag 9a event: RunInputs gives each run the event queue its spec describes, from the first event",
          "[ceir][cook][diag][input][event]")
{
    using K = input::InputKind;
    ck::RunInputs             inputs("diag9a-run-inputs-events");
    const input::InputSource* src = inputs.source();
    i64                       v   = 0;

    // No spec: the host has no event queue.
    inputs.set(false, 0U, ck::HostClockSpec{});
    CHECK_FALSE(input::read_input(src, K::Event, 0U, v));

    // An open spec with no event: queue 0 reads none events, and only queue 0 exists.
    inputs.set(false, 0U, ck::HostClockSpec{}, ck::HostEventsSpec{true, {}});
    REQUIRE(input::read_input(src, K::Event, 0U, v));
    CHECK(v == 0);
    CHECK_FALSE(input::read_input(src, K::Event, 1U, v));

    // Two events: every set starts the queue over from its first event, and a draw never moves the queue.
    const i64 events[2] = {packed(1U, 65U, 3U, 0, 0), packed(8U, 0U, 0U, 1280, 720)};
    for (u32 run = 0U; run < 2U; ++run)
    {
        inputs.set(true, 7U, ck::HostClockSpec{}, ck::HostEventsSpec{true, ConstSpan<i64>(events, 2U)});
        REQUIRE(input::read_input(src, K::Event, 0U, v));
        CHECK(v == events[0]);
        REQUIRE(input::read_input(src, K::Random, 0U, v));
        CHECK(v == input::SeededInputs::draw(7U, 0U, 0U));
        REQUIRE(input::read_input(src, K::Event, 0U, v));
        CHECK(v == events[1]);
        REQUIRE(input::read_input(src, K::Event, 0U, v));
        CHECK(v == 0);
    }

    // A later set without a spec forgets the queue; the other inputs still route.
    inputs.set(true, 7U, ck::HostClockSpec{});
    CHECK_FALSE(input::read_input(src, K::Event, 0U, v));
    REQUIRE(input::read_input(src, K::Random, 0U, v));
    CHECK(v == input::SeededInputs::draw(7U, 0U, 0U));

    // A host that queues more than a spec does (the sandbox's window events) pushes between runs.
    inputs.set(false, 0U, ck::HostClockSpec{}, ck::HostEventsSpec{true, {}});
    REQUIRE(inputs.events().push(ck::kEventQueue, events[1]));
    REQUIRE(input::read_input(src, K::Event, 0U, v));
    CHECK(v == events[1]);
    REQUIRE(input::read_input(src, K::Event, 0U, v));
    CHECK(v == 0);
}
