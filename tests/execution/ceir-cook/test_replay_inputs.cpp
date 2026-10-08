// DIAG.9a -- recorded host inputs. A program draws random values through the input seam (input.random); a run record
// keeps every draw, in order, and a replay feeds them back instead of asking a live source, so a seeded failure
// reproduces in a later process from the record alone, even after the program file was edited.
//
// The committed authored program assets/ceir/random_demo.ceir draws main(n) values from stream 0 and switches on each
// (three cases: a draw of 3 is out of range), then returns one draw from stream 1. Which seed fails, and at which draw,
// is computed here from SeededInputs::draw, independently of both executors; positions come from scanning the text.
// The cases: a seeded failure recorded and reproduced after a restart and an edit, and replaying the same draws
// against an edit that reads another stream names that read; a passing seed and a host without a random source; a
// replay whose record cannot answer a read (exhausted, a different stream, draws left over) and an earlier value
// difference reported first; an incomplete or uncaptured stream stored missing and refused; the record format's
// bounds; the inspect host, which keeps no draws, recording random as missing. ASCII test names (ctest by-name).

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

constexpr const char* kAsset = CRD_REPO_DIR "/assets/ceir/random_demo.ceir";

// The draw the switch reads, and the edit that makes it read another stream.
constexpr const char* kDrawLine   = "%4 = input.random() {stream = 0, bound = 4} : !i32";
constexpr const char* kDrawEdited = "%4 = input.random() {stream = 2, bound = 4} : !i32";
constexpr u32         kDraws      = 4U; // main(4)

// The effect of a probe op that draws randomness itself, outside the input seam.
constexpr crd::ceir::EffectRecord kRandomRead[] = {
    {crd::ceir::EffectFamily::RandomRead, crd::ceir::EffectTarget::None, 0U, 0U}};

// main(n) never fails: the draw is in [0, 3) and the switch has three cases.
constexpr const char* kMany = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i32):
        %1 = arith.const() {value = 0} : !i32
        %2 = arith.const() {value = 1} : !i32
        core.for(%1, %0, %2) {
          ^bb0(%3 : !i32):
            %4 = input.random() {stream = 0, bound = 3} : !i32
            core.switch(%4) {
              ^bb0:
                core.yield()
            } {
              ^bb0:
                core.yield()
            } {
              ^bb0:
                core.yield()
            }
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

// The draw of stream 0 the switch reads, reduced as the op reduces it.
i64 switch_draw(u64 seed, u64 n)
{
    return input::reduce_draw(input::SeededInputs::draw(seed, 0U, n), 4U);
}

// The first seed for which main(kDraws) fails (`fails`) or finishes; `failing` gets the failing draw's index.
u64 seed_where(bool fails, u32& failing)
{
    for (u64 seed = 1U; seed < 10000U; ++seed)
    {
        failing = kDraws;
        for (u32 i = 0U; i < kDraws && failing == kDraws; ++i)
        {
            if (switch_draw(seed, i) == 3)
            {
                failing = i;
            }
        }
        if ((failing < kDraws) == fails)
        {
            return seed;
        }
    }
    FAIL("no seed found");
    return 0U;
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

DiagResult record(Host& h, const char* program, const char* out, const char* args, const char* seed,
                  const char* max_events = nullptr)
{
    DiagArg a[4] = {{"out", StringView{out}}, {"args", StringView{args}}, {}, {}};
    u32     n    = 2U;
    if (seed != nullptr)
    {
        a[n++] = {"seed", StringView{seed}};
    }
    if (max_events != nullptr)
    {
        a[n++] = {"max_events", StringView{max_events}};
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
    Replayed out;
    out.d    = ck::first_divergence(rec, t);
    out.line = ck::replay_site(ctx, p, out.d.site).line;
    return out;
}

String decimal(u64 v, crd::memory::IAllocator* a)
{
    char buf[32];
    (void)std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    String s(a);
    s.append(buf);
    return s;
}
} // namespace

TEST_CASE("diag 9a input: a seeded failure records every draw and reproduces after a restart and an edit",
          "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        draw = locate(view(text), kDrawLine);
    const Where                        sw   = locate(view(text), "core.switch(%4)");
    u32                                k    = 0U;
    const u64                          seed = seed_where(true, k);
    const String                       seed_text = decimal(seed, &alloc);
    INFO("seed " << seed << " fails at draw " << k);

    const char* const program = "diag9a_in_demo.ceir";
    const char* const out     = "diag9a_in_fail.crpl";
    const char* const again   = "diag9a_in_again.crpl";
    forget(out);
    forget(again);
    spill(program, view(text));

    {
        Host             h;
        const DiagResult r = record(h, program, out, "4", seed_text.c_str());
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"selector-out-of-range\"");
        CHECK(number(j, "fault_line", &alloc) == sw.line);
        CHECK(number(j, "fault_col", &alloc) == sw.col);
        CHECK(view(field(j, "random_source", &alloc)) == "\"seeded\"");
        CHECK(static_cast<u64>(number(j, "seed", &alloc)) == seed);
        CHECK(number(j, "input_reads", &alloc) == static_cast<i64>(k) + 1);
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
        CHECK(has(j, R"("input":"random","guarantee":"event","needed":"yes","state":"recorded")"));

        // The record holds each draw the run made, raw, in order: exactly the seeded host's.
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.input_reads.size() == k + 1U);
        CHECK(rec.input_reads_total == k + 1U);
        for (u32 i = 0U; i <= k; ++i)
        {
            const ReplayInputRead expected{input::InputKind::Random, 0U, true, input::SeededInputs::draw(seed, 0U, i)};
            CHECK(rec.input_reads[i] == expected);
        }
        CHECK(rec.inputs[3].state == ck::ReplayInputState::Recorded);

        // Recording again gives the same bytes.
        REQUIRE(record(h, program, again, "4", seed_text.c_str()).status == DiagStatus::Ok);
        CHECK(view(slurp(out, &alloc)) == view(slurp(again, &alloc)));
    }

    // A later process: a fresh service and Context, with the program file edited after the record was made.
    const String edit = replaced(view(text), kDrawLine, kDrawEdited, &alloc);
    spill(program, view(edit));
    {
        Host             h;
        const DiagResult r = replay(h, out);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "result", &alloc)) == "\"reproduced\"");
        CHECK(number(j, "recorded_input_reads", &alloc) == static_cast<i64>(k) + 1);
        CHECK(number(j, "replayed_input_reads", &alloc) == static_cast<i64>(k) + 1);
        const usize replayed = j.find(StringView{R"("run":"replayed")"});
        REQUIRE(replayed != StringView::npos);
        CHECK(view(field(j, "error", &alloc, replayed)) == "\"selector-out-of-range\"");
        CHECK(number(j, "line", &alloc, replayed) == sw.line);

        // The same draws against the edited file: its first read asks for stream 2 where the record holds stream 0.
        const DiagResult e = replay(h, out, program);
        INFO(e.json.c_str());
        REQUIRE(e.status == DiagStatus::Ok);
        const StringView ej = view(e.json);
        CHECK(view(field(ej, "result", &alloc)) == "\"diverged\"");
        const usize at = ej.find(StringView{R"("kind":"divergence")"});
        REQUIRE(at != StringView::npos);
        CHECK(view(field(ej, "divergence", &alloc, at)) == "\"input\"");
        CHECK(number(ej, "index", &alloc, at) == 0);
        CHECK(number(ej, "recorded", &alloc, at) == 0);
        CHECK(number(ej, "observed", &alloc, at) == 2);
        CHECK(number(ej, "line", &alloc, at) == draw.line);
        CHECK(number(ej, "col", &alloc, at) == draw.col);
    }
    forget(out);
    forget(again);
    forget(program);
}

TEST_CASE("diag 9a input: a passing seed and a host without a random source are recorded as they ran",
          "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        draw = locate(view(text), kDrawLine);
    const char* const                  program = "diag9a_in_pass.ceir";
    const char* const                  out     = "diag9a_in_pass.crpl";
    forget(out);
    spill(program, view(text));
    Host h;

    SECTION("a passing seed: all five draws and the returned stream-1 draw")
    {
        u32              k    = 0U;
        const u64        seed = seed_where(false, k);
        const DiagResult r    = record(h, program, out, "4", decimal(seed, &alloc).c_str());
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"none\"");
        CHECK(number(j, "input_reads", &alloc) == 5);
        const i64 value = input::reduce_draw(input::SeededInputs::draw(seed, 1U, 0U), 100U);
        String    item(&alloc);
        item.append(R"({"kind":"result","index":0,"value":)");
        item.append(view(decimal(static_cast<u64>(value), &alloc)));
        item.append("}");
        CHECK(has(j, view(item)));
        const DiagResult p = replay(h, out);
        REQUIRE(p.status == DiagStatus::Ok);
        CHECK(view(field(view(p.json), "result", &alloc)) == "\"reproduced\"");
    }
    SECTION("no seed: the host has no random source, the first draw fails and that read is what the record holds")
    {
        const DiagResult r = record(h, program, out, "4", nullptr);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"input-unavailable\"");
        CHECK(number(j, "fault_line", &alloc) == draw.line);
        CHECK(view(field(j, "random_source", &alloc)) == "\"none\"");
        CHECK(number(j, "input_reads", &alloc) == 1);
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.input_reads.size() == 1U);
        CHECK(rec.input_reads[0] == ReplayInputRead{input::InputKind::Random, 0U, false, 0});
        const DiagResult p = replay(h, out);
        INFO(p.json.c_str());
        REQUIRE(p.status == DiagStatus::Ok);
        const StringView pj = view(p.json);
        CHECK(view(field(pj, "result", &alloc)) == "\"reproduced\"");
        const usize replayed = pj.find(StringView{R"("run":"replayed")"});
        REQUIRE(replayed != StringView::npos);
        CHECK(view(field(pj, "error", &alloc, replayed)) == "\"input-unavailable\"");
    }
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a input: a replay answers reads only from its record and names the first read it cannot answer",
          "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        draw = locate(view(text), kDrawLine);
    u32                                k    = 0U;
    const u64                          seed = seed_where(true, k);
    const char* const                  program = "diag9a_in_feed.ceir";
    const char* const                  out     = "diag9a_in_feed.crpl";
    forget(out);
    spill(program, view(text));
    Host h;
    REQUIRE(record(h, program, out, "4", decimal(seed, &alloc).c_str()).status == DiagStatus::Ok);

    // The record as made reproduces.
    CHECK(replay_feed(decode_file(out, &alloc), &alloc).d.kind == ck::DivergenceKind::None);

    SECTION("the record lost its last draw: the read past its end is refused there")
    {
        ReplayRecord rec = decode_file(out, &alloc);
        rec.input_reads.pop_back();
        --rec.input_reads_total;
        const Replayed r = replay_feed(rec, &alloc);
        CHECK(r.d.kind == ck::DivergenceKind::Input);
        CHECK(r.d.count);
        CHECK(r.d.index == k);
        CHECK(r.d.recorded == static_cast<i64>(k));
        CHECK(r.d.observed == static_cast<i64>(k) + 1);
        CHECK(r.line == draw.line);
    }
    SECTION("the record names another stream at the first read: refused there")
    {
        ReplayRecord rec          = decode_file(out, &alloc);
        rec.input_reads[0].channel = 9U;
        const Replayed r          = replay_feed(rec, &alloc);
        CHECK(r.d.kind == ck::DivergenceKind::Input);
        CHECK_FALSE(r.d.count);
        CHECK(r.d.index == 0U);
        CHECK(r.d.recorded == 9);
        CHECK(r.d.observed == 0);
        CHECK(r.line == draw.line);
    }
    SECTION("the record holds a draw the replay never reads: the read counts differ")
    {
        ReplayRecord rec = decode_file(out, &alloc);
        rec.input_reads.push_back(ReplayInputRead{input::InputKind::Random, 0U, true, 5});
        ++rec.input_reads_total;
        const Replayed r = replay_feed(rec, &alloc);
        CHECK(r.d.kind == ck::DivergenceKind::Input);
        CHECK(r.d.count);
        CHECK(r.d.recorded == static_cast<i64>(k) + 2);
        CHECK(r.d.observed == static_cast<i64>(k) + 1);
    }
    SECTION("a different failing draw: the value it gave comes first, before the read the record then cannot answer")
    {
        ReplayRecord rec          = decode_file(out, &alloc);
        rec.input_reads[k].value = 8; // reduces to 0: the switch takes a case and the run draws again
        const Replayed r          = replay_feed(rec, &alloc);
        CHECK(r.d.kind == ck::DivergenceKind::Value);
        CHECK(r.d.recorded == 3);
        CHECK(r.d.observed == 0);
        CHECK(r.line == draw.line);
    }
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a input: an incomplete or uncaptured random stream is stored missing and the replay is refused",
          "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator alloc;

    SECTION("a run that drew more than a record keeps")
    {
        const char* const program = "diag9a_in_many.ceir";
        const char* const out     = "diag9a_in_many.crpl";
        forget(out);
        spill(program, StringView{kMany});
        Host             h;
        const DiagResult r = record(h, program, out, "65537", "1", "16");
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"none\"");
        CHECK(number(j, "input_reads", &alloc) == 65537);
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"random\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"incomplete\"");
        CHECK(has(j, R"("input":"random","guarantee":"event","needed":"yes","state":"missing")"));
        const ReplayRecord rec = decode_file(out, &alloc);
        CHECK(rec.input_reads.size() == ck::kReplayMaxInputReads);
        CHECK(rec.input_reads_total == 65537U);
        const DiagResult p = replay(h, out);
        CHECK(p.status == DiagStatus::Unavailable);
        CHECK(has(view(p.json), "missing inputs its program needs (random)"));
        forget(out);
        forget(program);
    }
    SECTION("held or not, and a draw outside the seam")
    {
        Context ctx(&alloc);
        registrar(ctx, nullptr);
        // rp.rng declares RandomRead itself: nothing at the input seam sees what it draws.
        crd::ceir::Dialect* const d = ctx.register_dialect("rp");
        (void)d->register_op("rng", {.effects = ConstSpan<crd::ceir::EffectRecord>(kRandomRead, 1U),
                                     .determinism = crd::ceir::DeterminismClass::Nondeterministic});
        const String              seam   = slurp(kAsset, &alloc);
        const crd::ceir::ParseResult a = crd::ceir::parse(ctx, view(seam));
        REQUIRE(a.module != nullptr);
        const crd::ceir::ParseResult b = crd::ceir::parse(
            ctx, StringView{"module {\n  ^bb0:\n    func.func() {sym_name = \"main\"} {\n      ^bb0:\n"
                            "        %0 = input.random() {stream = 0, bound = 4} : !i32\n"
                            "        %1 = rp.rng() : !i32\n        func.return(%0)\n    }\n}\n"});
        REQUIRE(b.module != nullptr);

        const auto random_state = [&](const crd::ceir::Module& m, bool held, String& missing)
        {
            ck::ReplayInput inputs[ck::kReplayInputs];
            missing.clear();
            REQUIRE(ck::classify_replay_inputs(ctx, m, ck::ReplayExecutorKind::Plan, held, &alloc, nullptr, inputs,
                                               &missing));
            CHECK(inputs[3].need == ck::ReplayNeed::Yes);
            return inputs[3].state;
        };
        String missing(&alloc);
        CHECK(random_state(*a.module, true, missing) == ck::ReplayInputState::Recorded);
        CHECK(view(missing).empty());
        CHECK(random_state(*a.module, false, missing) == ck::ReplayInputState::Missing);
        CHECK(view(missing) == "random");
        CHECK(random_state(*b.module, true, missing) == ck::ReplayInputState::Missing);
        CHECK(view(missing) == "random");
    }
}

TEST_CASE("diag 9a input: the record format holds the reads exactly and refuses a damaged stream",
          "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    u32                                k       = 0U;
    const u64                          seed    = seed_where(true, k);
    const char* const                  program = "diag9a_in_format.ceir";
    const char* const                  out     = "diag9a_in_format.crpl";
    forget(out);
    spill(program, view(text));
    Host h;
    REQUIRE(record(h, program, out, "4", decimal(seed, &alloc).c_str()).status == DiagStatus::Ok);
    const String file = slurp(out, &alloc);

    const auto encode = [&](const ReplayRecord& rec)
    {
        Array<u8> bytes(&alloc);
        ck::encode_record(rec, bytes);
        return bytes;
    };
    const auto decode = [&](const Array<u8>& bytes)
    {
        ReplayRecord rec(&alloc);
        return ck::decode_record({bytes.data(), bytes.size()}, rec);
    };

    // Decoding and encoding again gives the file's bytes.
    const Array<u8> same = encode(decode_file(out, &alloc));
    REQUIRE(same.size() == file.size());
    CHECK(StringView{reinterpret_cast<const char*>(same.data()), same.size()} == view(file));

    ReplayRecord rec = decode_file(out, &alloc);
    rec.schema       = 3U;
    CHECK(decode(encode(rec)) == ck::RecordError::UnsupportedSchema);

    rec                     = decode_file(out, &alloc);
    rec.input_reads[0].kind = static_cast<input::InputKind>(1U); // past the last kind
    CHECK(decode(encode(rec)) == ck::RecordError::Malformed);

    rec                          = decode_file(out, &alloc);
    rec.input_reads[0].delivered = false; // a value the host never delivered
    CHECK(decode(encode(rec)) == ck::RecordError::Malformed);

    rec                   = decode_file(out, &alloc);
    rec.input_reads_total = 0U; // fewer reads counted than kept
    CHECK(decode(encode(rec)) == ck::RecordError::Malformed);

    rec                   = decode_file(out, &alloc);
    rec.input_reads_total = 70000U; // more counted than a record keeps, with fewer than the bound kept
    CHECK(decode(encode(rec)) == ck::RecordError::Malformed);

    Array<u8> cut = encode(decode_file(out, &alloc));
    cut.pop_back();
    CHECK(decode(cut) == ck::RecordError::Truncated);
    forget(out);
    forget(program);
}

TEST_CASE("diag 9a input: the inspect host keeps no draws, so its record stores random missing",
          "[ceir][cook][diag][input]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    ck::InspectHost                    host(&alloc, &registrar, nullptr);
    REQUIRE(host.load(ck::AssetId{9200U}, view(text), "ceir/random_demo.ceir", "main").ok());
    const i64 args[1] = {4};
    REQUIRE(host.start({args, 1U}, ck::HostRecording{true, 0U}) == crd::ceir::inspect::Refusal::None);
    REQUIRE(host.wait_finished(20000U));
    CHECK(host.result().error == crd::ceir::plan::RunError::InputUnavailable);

    ReplayRecord rec(&alloc);
    REQUIRE(host.record(rec) == ck::HostRecord::Ok);
    CHECK(rec.error == crd::ceir::plan::RunError::InputUnavailable);
    CHECK(rec.inputs[3].state == ck::ReplayInputState::Missing);
    CHECK(rec.input_reads.empty());
    String missing(&alloc);
    CHECK_FALSE(ck::record_missing_inputs(rec, missing));
    CHECK(view(missing) == "random");
}
