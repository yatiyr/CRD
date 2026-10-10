// DIAG.9c -- minimizing the recorded inputs of a failing run (crd/ceir/cook/replay_minimize.hpp). A record is made by
// `replay.record`, minimized, written, and reproduced by `replay.run` like any other record.
//
// The expected inputs are worked out here from the programs, not from the minimizer. A program that fails at its first
// switch for an argument of 10 or more, or below 0, and at a second switch for 2 to 9, shrinks from 1000 to exactly 10
// (shrinking further fails at the other switch, which is another failure) and from -1000 to -1. The committed
// assets/ceir/random_demo.ceir draws main(n) values and fails on a draw that reduces to 3: a seeded run failing at draw
// k shrinks to main(k + 1), its earlier draws to 0 and its failing draw to a value that still reduces to 3 while the
// value one step closer to 0 does not. Records that cannot be minimized are refused before anything runs. ASCII test
// names (ctest by-name).

#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_minimize.hpp>
#include <crd/ceir/cook/replay_record.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/effect.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/semantics.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/minimize.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <utility>

namespace
{
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::cook::RecordMinimizeOptions;
using crd::ceir::cook::RecordMinimizeResult;
using crd::ceir::cook::RecordMinimizeStatus;
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
namespace ck    = crd::ceir::cook;
namespace input = crd::ceir::input;

constexpr const char* kRandomAsset = CRD_REPO_DIR "/assets/ceir/random_demo.ceir";

void registrar(Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
    (void)input::register_input_ops(ctx);
}

// A host that also knows an op reading a file, so a program can need external results (DIAG.9b).
constexpr crd::ceir::EffectRecord kFileRead[] = {
    {crd::ceir::EffectFamily::FileIO, crd::ceir::EffectTarget::None, 0U, 0U}};

void io_registrar(Context& ctx, void* user)
{
    registrar(ctx, user);
    crd::ceir::Dialect* const d = ctx.register_dialect("xio");
    (void)d->register_op("read", {.effects = crd::containers::ConstSpan<crd::ceir::EffectRecord>(kFileRead, 1U),
                                  .determinism = crd::ceir::DeterminismClass::ExternalNondeterminism});
}

// main(a): the first switch has ten cases (a outside [0, 10) fails there), the second two (a in [2, 10) fails there).
constexpr const char* kFirstSwitch  = "core.switch(%0)";
constexpr const char* kSecondSwitch = "core.switch(%2)";

void append_cases(String& text, u32 cases)
{
    for (u32 i = 0U; i < cases; ++i)
    {
        text.append(" {\n");
        text.append("          ^bb0:\n            core.yield()\n        }");
    }
    text.append("\n");
}

String two_switch_program(crd::memory::IAllocator* a)
{
    String text(a);
    text.append("module {\n  ^bb0:\n    func.func() {sym_name = \"main\"} {\n      ^bb0(%0 : !i32):\n");
    text.append("        %1 = arith.const() {value = 0} : !i32\n");
    text.append("        ");
    text.append(kFirstSwitch);
    append_cases(text, 10U);
    text.append("        %2 = arith.addi(%0, %1) : !i32\n");
    text.append("        ");
    text.append(kSecondSwitch);
    append_cases(text, 2U);
    text.append("        func.return(%0)\n    }\n}\n");
    return text;
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

// The 1-based line holding `needle` (once).
u32 line_of(StringView text, StringView needle)
{
    const usize at = text.find(needle);
    REQUIRE(at != StringView::npos);
    REQUIRE(text.find(needle, at + 1U) == StringView::npos);
    u32 line = 1U;
    for (usize i = 0; i < at; ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
        }
    }
    return line;
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

DiagResult record(Host& h, const char* program, const char* out, const char* args, const char* seed = nullptr)
{
    DiagArg a[3] = {{"out", StringView{out}}, {"args", StringView{args}}, {}};
    u32     n    = 2U;
    if (seed != nullptr)
    {
        a[n++] = {"seed", StringView{seed}};
    }
    DiagRequest r;
    r.command    = ck::kReplayRecordCommand;
    r.path       = StringView{program};
    r.args       = {a, n};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r);
}

DiagResult replay(Host& h, const char* path)
{
    DiagRequest r;
    r.command    = ck::kReplayRunCommand;
    r.path       = StringView{path};
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

// What a run of `rec`'s program on `args` and `reads` ends with: its error and the authored line it blamed.
struct Ending
{
    crd::ceir::plan::RunError error = crd::ceir::plan::RunError::None;
    u64                       op    = 0U;
    u32                       line  = 0U;
    bool                      refused = false;
};
Ending run_inputs(const ReplayRecord& rec, const Array<i64>& args, const Array<ck::ReplayInputRead>& reads,
                  crd::memory::IAllocator* a)
{
    Context           ctx(a);
    ck::ReplayProgram p(a);
    ck::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, view(rec.entry), &registrar, nullptr, p);
    REQUIRE(p.ok());
    ck::ReplayTrace t(a);
    ck::InputFeed   feed({reads.data(), reads.size()}, t);
    ck::run_traced(p, {args.data(), args.size()}, rec.max_events, nullptr, t, feed.source());
    Ending e;
    e.error   = t.error;
    e.op      = t.fault_op;
    e.line    = ck::replay_site(ctx, p, t.fault).line;
    e.refused = t.input_refusal.refused;
    return e;
}

// Write `rec` as a new record file and reproduce it with `replay.run`: the replayed outcome's authored line.
u32 reproduced_line(Host& h, const ReplayRecord& rec, const char* path, crd::memory::IAllocator* a)
{
    forget(path);
    REQUIRE(ck::write_record_file(StringView{path}, rec) == ck::RecordWrite::Ok);
    const DiagResult r = replay(h, path);
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    const StringView j = view(r.json);
    CHECK(view(field(j, "result", a)) == "\"reproduced\"");
    CHECK_FALSE(has(j, R"("kind":"divergence")"));
    const usize replayed = j.find(StringView{R"("run":"replayed")"});
    REQUIRE(replayed != StringView::npos);
    CHECK(view(field(j, "error", a, replayed)) == "\"selector-out-of-range\"");
    const u32 line = static_cast<u32>(number(j, "line", a, replayed));
    forget(path);
    return line;
}

RecordMinimizeOptions options_k(u32 k)
{
    RecordMinimizeOptions o;
    o.budget.runs_per_candidate = k;
    return o;
}
} // namespace

TEST_CASE("diag 9c input: a failing argument shrinks to the smallest value failing at the same op, in both signs",
          "[ceir][cook][diag][diag9c][minimize]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text   = two_switch_program(&alloc);
    const u32                          first  = line_of(view(text), StringView{kFirstSwitch});
    const u32                          second = line_of(view(text), StringView{kSecondSwitch});
    const char* const                  program = "diag9c_min_switch.ceir";
    const char* const                  out     = "diag9c_min_switch.crpl";
    const char* const                  shrunk  = "diag9c_min_switch_small.crpl";
    spill(program, view(text));

    struct Case
    {
        const char* args;
        i64         expected;
    };
    const Case cases[] = {{"1000", 10}, {"-1000", -1}};
    Host       h;
    for (const Case& c : cases)
    {
        INFO(c.args);
        forget(out);
        const DiagResult r = record(h, program, out, c.args);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const ReplayRecord rec = decode_file(out, &alloc);
        REQUIRE(rec.error == crd::ceir::plan::RunError::SelectorOutOfRange);

        ReplayRecord         small(&alloc);
        RecordMinimizeResult result(&alloc);
        ck::minimize_record_inputs(rec, &registrar, nullptr, options_k(2U), nullptr, small, result);
        INFO(result.reason.c_str());
        CHECK(result.status == RecordMinimizeStatus::Minimal);
        REQUIRE(small.args.size() == 1U);
        CHECK(small.args[0] == c.expected);
        CHECK(result.args_changed == 1U);
        CHECK(result.stats.flaky == 0U);
        CHECK(small.error == rec.error);
        CHECK(small.fault_op == rec.fault_op);
        CHECK(small.content_hash == rec.content_hash);

        // Independently of the minimizer: the value one step closer to 0 fails at the other switch, or not at all.
        const Array<ck::ReplayInputRead> no_reads(&alloc);
        Array<i64>                       closer(&alloc);
        closer.push_back(c.expected > 0 ? c.expected - 1 : c.expected + 1);
        const Ending e = run_inputs(rec, closer, no_reads, &alloc);
        CHECK((e.error == crd::ceir::plan::RunError::None || e.op != rec.fault_op));
        if (c.expected > 0)
        {
            CHECK(e.line == second);
        }

        // The new record reproduces through replay.run, blamed at the first switch as the original was.
        CHECK(reproduced_line(h, small, shrunk, &alloc) == first);
    }
    forget(out);
    forget(program);
}

TEST_CASE("diag 9c input: a seeded random failure shrinks to the loop count and draws that fail at the same op",
          "[ceir][cook][diag][diag9c][minimize]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text   = slurp(kRandomAsset, &alloc);
    const u32                          sw     = line_of(view(text), StringView{"core.switch(%4)"});
    const char* const                  program = "diag9c_min_random.ceir";
    const char* const                  out     = "diag9c_min_random.crpl";
    const char* const                  shrunk  = "diag9c_min_random_small.crpl";
    forget(out);
    spill(program, view(text));

    // The first seed whose run of main(4) fails at draw k >= 2 (stream 0's draw reduces to 3 there, and not before).
    u64 seed = 0U;
    u32 k    = 0U;
    for (u64 s = 1U; s < 100000U && seed == 0U; ++s)
    {
        for (u32 i = 0U; i < 4U; ++i)
        {
            if (input::reduce_draw(input::SeededInputs::draw(s, 0U, i), 4U) == 3)
            {
                if (i >= 2U)
                {
                    seed = s;
                    k    = i;
                }
                break;
            }
        }
    }
    REQUIRE(seed != 0U);
    char seed_text[32];
    (void)std::snprintf(seed_text, sizeof(seed_text), "%llu", static_cast<unsigned long long>(seed));
    INFO("seed " << seed << " fails at draw " << k);

    Host             h;
    const DiagResult r = record(h, program, out, "4", seed_text);
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    const ReplayRecord rec = decode_file(out, &alloc);
    REQUIRE(rec.error == crd::ceir::plan::RunError::SelectorOutOfRange);
    REQUIRE(rec.input_reads.size() == k + 1U);

    ReplayRecord         small(&alloc);
    RecordMinimizeResult result(&alloc);
    ck::minimize_record_inputs(rec, &registrar, nullptr, options_k(2U), nullptr, small, result);
    INFO(result.reason.c_str());
    INFO("runs " << result.stats.runs << ", candidates " << result.stats.candidates);
    CHECK(result.status == RecordMinimizeStatus::Minimal);
    CHECK(result.stats.runs <= options_k(2U).budget.max_runs);
    CHECK(result.reads_changed >= k); // the k earlier draws, and the failing one unless it was already minimal
    REQUIRE(small.args.size() == 1U);
    CHECK(small.args[0] == static_cast<i64>(k) + 1); // main(k + 1): the failing draw is its last
    CHECK(small.fault_op == rec.fault_op);
    REQUIRE(small.input_reads.size() == k + 1U);
    CHECK(small.input_reads_total == k + 1U);
    for (u32 i = 0U; i < k; ++i)
    {
        INFO(i);
        CHECK(small.input_reads[i].kind == input::InputKind::Random);
        CHECK(small.input_reads[i].channel == 0U);
        CHECK(small.input_reads[i].delivered);
        CHECK(small.input_reads[i].value == 0); // case 0: the run goes on to the failing draw
    }
    const i64 failing  = small.input_reads[k].value;
    const i64 original = rec.input_reads[k].value;
    CHECK(input::reduce_draw(failing, 4U) == 3);
    CHECK((failing < 0 ? 0U - static_cast<u64>(failing) : static_cast<u64>(failing)) <=
          (original < 0 ? 0U - static_cast<u64>(original) : static_cast<u64>(original)));

    // Independently of the minimizer: the failing draw one step closer to 0 no longer fails at the switch.
    Array<ck::ReplayInputRead> closer(small.input_reads, &alloc);
    closer[k].value = failing > 0 ? failing - 1 : failing + 1;
    const Ending e  = run_inputs(rec, small.args, closer, &alloc);
    CHECK_FALSE((e.error == rec.error && e.op == rec.fault_op && !e.refused));

    // The new record reproduces through replay.run, at the same switch.
    CHECK(reproduced_line(h, small, shrunk, &alloc) == sw);
    forget(out);
    forget(program);
}

TEST_CASE("diag 9c input: records that cannot be minimized are refused before anything runs",
          "[ceir][cook][diag][diag9c][minimize]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = two_switch_program(&alloc);
    const char* const                  program = "diag9c_min_refuse.ceir";
    const char* const                  failed  = "diag9c_min_refuse_fail.crpl";
    const char* const                  passed  = "diag9c_min_refuse_pass.crpl";
    forget(failed);
    forget(passed);
    spill(program, view(text));
    Host h;
    REQUIRE(record(h, program, failed, "1000").status == DiagStatus::Ok);
    REQUIRE(record(h, program, passed, "0").status == DiagStatus::Ok);
    const ReplayRecord fail_rec = decode_file(failed, &alloc);
    const ReplayRecord pass_rec = decode_file(passed, &alloc);

    const auto refused = [&](const ReplayRecord& rec, RecordMinimizeStatus status, const char* reason,
                             crd::ceir::cook::Registrar dialects = &registrar)
    {
        ReplayRecord         untouched(&alloc);
        RecordMinimizeResult result(&alloc);
        ck::minimize_record_inputs(rec, dialects, nullptr, options_k(2U), nullptr, untouched, result);
        INFO(result.reason.c_str());
        CHECK(result.status == status);
        CHECK(has(view(result.reason), StringView{reason}));
        CHECK(result.stats.runs == 0U);
        CHECK(untouched.args.empty());
        CHECK(untouched.program.empty());
    };

    SECTION("a run that did not fail has nothing to minimize")
    {
        refused(pass_rec, RecordMinimizeStatus::NotAFailure, "did not fail");
    }
    SECTION("another build is refused unless allowed")
    {
        ReplayRecord other(&alloc);
        other = fail_rec;
        other.build.version.append("-other");
        refused(other, RecordMinimizeStatus::Refused, "another build (differs in version)");
        ReplayRecord         small(&alloc);
        RecordMinimizeResult result(&alloc);
        RecordMinimizeOptions allow = options_k(1U);
        allow.any_build             = true;
        ck::minimize_record_inputs(other, &registrar, nullptr, allow, nullptr, small, result);
        CHECK(result.status == RecordMinimizeStatus::Minimal);
        REQUIRE(small.args.size() == 1U);
        CHECK(small.args[0] == 10);
    }
    SECTION("a program that is not the recorded content hash is refused")
    {
        ReplayRecord tampered(&alloc);
        tampered = fail_rec;
        tampered.content_hash ^= 1U;
        refused(tampered, RecordMinimizeStatus::Refused, "content hash");
    }
    SECTION("a host record and an incomplete input stream are not minimized")
    {
        ReplayRecord host(&alloc);
        host          = fail_rec;
        host.executor = ck::ReplayExecutorKind::Host;
        refused(host, RecordMinimizeStatus::Unsupported, "host record");
        ReplayRecord incomplete(&alloc);
        incomplete                   = fail_rec;
        incomplete.input_reads_total = 1U;
        refused(incomplete, RecordMinimizeStatus::Unsupported, "incomplete");
    }
    SECTION("a program that needs external I/O results is refused (no build replays them)")
    {
        // The failing record's program with its constant read from a file instead, cooked as a record holds it.
        const usize at = view(text).find(StringView{"%1 = arith.const() {value = 0} : !i32"});
        REQUIRE(at != StringView::npos);
        String read_text(&alloc);
        read_text.append(view(text).substr(0U, at));
        read_text.append("%1 = xio.read() : !i32");
        read_text.append(view(text).substr(at + StringView{"%1 = arith.const() {value = 0} : !i32"}.size()));
        Context ctx(&alloc);
        io_registrar(ctx, nullptr);
        ck::CookResult cooked = ck::cook_program_text(ctx, view(read_text), StringView{program}, 1U, &alloc, &alloc);
        REQUIRE(cooked.ok());
        ReplayRecord reading(&alloc);
        reading              = fail_rec;
        reading.program      = std::move(cooked.blob);
        reading.content_hash = cooked.content_hash;
        // Whatever the record claims about external results, the program's own ops decide.
        reading.inputs[ck::kReplayExternalResultsInput].need  = ck::ReplayNeed::No;
        reading.inputs[ck::kReplayExternalResultsInput].state = ck::ReplayInputState::NotNeeded;
        refused(reading, RecordMinimizeStatus::Refused, "no build replays external I/O", &io_registrar);
    }
    forget(failed);
    forget(passed);
    forget(program);
}
