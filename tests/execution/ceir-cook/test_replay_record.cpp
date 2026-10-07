// DIAG.9a -- run records and replay. `replay.record` runs an authored program once through the compiled-plan executor
// and writes an immutable record: the program as its cooked blob, the build, the entry arguments, every replay input's
// need and state, a bounded trace and the outcome. `replay.run` replays a record from its own blob, never from the
// checkout, and reports the first divergence; it refuses an incompatible record before anything runs.
//
// The committed authored program assets/ceir/replay_demo.ceir fails on a seeded argument: main(1) selects a switch
// region that does not exist. The record of that run reproduces after the program file is edited (the record holds
// its own artifact), and replaying the same inputs against the edited file names the edited constant as the first
// divergent value, at its authored line. A record from another build, one missing an input, one with a bad checksum,
// another schema or a program that is not the recorded hash is refused with nothing run. Expected lines and columns
// come from scanning the text, never from the parser. ASCII test names (ctest by-name).

#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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

constexpr const char* kAsset = CRD_REPO_DIR "/assets/ceir/replay_demo.ceir";

// The line the edit changes, and what it becomes.
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

// The offset of the item whose text starts with `prefix` (npos when none).
usize item_at(StringView json, const char* prefix)
{
    return json.find(StringView{prefix});
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
    explicit Host(crd::perf::DiagAuthoritySet grant = kAll) : svc(grant, here())
    {
        cmd.registrar = &registrar;
        REQUIRE(crd::ceir::cook::register_replay_record(svc, cmd));
        REQUIRE(crd::ceir::cook::register_replay_run(svc, cmd));
        REQUIRE(crd::ceir::cook::register_replay_prepare(svc, prepare));
        prepare.registrar = &registrar;
    }

    ReplayCommands                        cmd;
    crd::ceir::cook::ReplayPrepareCommand prepare;
    DiagCommandService                    svc;
};

DiagResult record(Host& h, const char* program, const char* out, const char* args,
                  const char* max_events = nullptr, const std::atomic<bool>* cancel = nullptr)
{
    DiagArg a[3] = {{"out", StringView{out}}, {"args", StringView{args}}, {"max_events", StringView{}}};
    u32     n    = 2U;
    if (max_events != nullptr)
    {
        a[2].value = StringView{max_events};
        n          = 3U;
    }
    DiagRequest r;
    r.command    = crd::ceir::cook::kReplayRecordCommand;
    r.path       = StringView{program};
    r.args       = {a, n};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r, cancel);
}

DiagResult replay(Host& h, const char* path, const char* program = nullptr, const char* build = nullptr)
{
    DiagArg a[2];
    u32     n = 0U;
    if (program != nullptr)
    {
        a[n++] = {"program", StringView{program}};
    }
    if (build != nullptr)
    {
        a[n++] = {"build", StringView{build}};
    }
    DiagRequest r;
    r.command    = crd::ceir::cook::kReplayRunCommand;
    r.path       = StringView{path};
    r.args       = {a, n};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r);
}

// Write the committed asset (or its edit) as a scratch program in the working directory.
void write_program(const char* path, StringView text)
{
    spill(path, text.data(), text.size());
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

void forget(const char* path)
{
    (void)std::remove(path);
}

ReplayRecord decode_file(const char* path, crd::memory::IAllocator* a)
{
    const Array<u8> bytes = slurp_bytes(path, a);
    ReplayRecord    rec(a);
    REQUIRE(crd::ceir::cook::decode_record({bytes.data(), bytes.size()}, rec) == crd::ceir::cook::RecordError::Ok);
    return rec;
}

void encode_file(const ReplayRecord& rec, const char* path, crd::memory::IAllocator* a)
{
    Array<u8> bytes(a);
    crd::ceir::cook::encode_record(rec, bytes);
    forget(path);
    spill(path, bytes.data(), bytes.size());
}
} // namespace

TEST_CASE("diag 9a: a seeded failure is recorded with its artifact, build, arguments and trace", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const Where                        sw   = locate(view(text), "core.switch(%10)");
    const Where                        add  = locate(view(text), "%10 = arith.addi(%3, %9)");

    const char* const program = "diag9a_rec_demo.ceir";
    const char* const ok_out  = "diag9a_rec_ok.crpl";
    const char* const bad_out = "diag9a_rec_bad.crpl";
    const char* const again   = "diag9a_rec_again.crpl";
    forget(ok_out);
    forget(bad_out);
    forget(again);
    write_program(program, view(text));

    Host h;

    SECTION("main(0) finishes: one result, no fault, and every input but identity and arguments is not needed")
    {
        const DiagResult r = record(h, program, ok_out, "0");
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"none\"");
        CHECK(view(field(j, "replay", &alloc)) == "\"replayable\"");
        CHECK(view(field(j, "missing_inputs", &alloc)) == "\"\"");
        CHECK(has(j, R"({"kind":"result","index":0,"value":1})"));
        CHECK(has(j, R"("input":"program","guarantee":"identity","needed":"yes","state":"recorded")"));
        CHECK(has(j, R"("input":"build","guarantee":"identity","needed":"yes","state":"recorded")"));
        CHECK(has(j, R"("input":"entry-arguments","guarantee":"event","needed":"yes","state":"recorded")"));
        for (const char* input : {"random", "clock", "host-state", "external-results", "schedule", "device-tolerance"})
        {
            String needle(&alloc);
            needle.append(R"("input":")");
            needle.append(input);
            needle.append(R"(",)");
            const usize at = j.find(view(needle));
            REQUIRE(at != StringView::npos);
            CHECK(view(field(j, "state", &alloc, at)) == "\"not-needed\"");
        }
        CHECK(h.cmd.records_written.load() == 1U);
        CHECK(h.cmd.executions.load() == 1U);
    }

    SECTION("main(1) faults at the switch: the record holds the fault, the artifact, the build and every event")
    {
        const DiagResult r = record(h, program, bad_out, "1");
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(view(field(j, "error", &alloc)) == "\"selector-out-of-range\"");
        CHECK(view(field(j, "fault_file", &alloc)) == "\"diag9a_rec_demo.ceir\"");
        CHECK(number(j, "fault_line", &alloc) == sw.line);
        CHECK(number(j, "fault_col", &alloc) == sw.col);
        CHECK(number(j, "lost", &alloc) == 0U);
        CHECK(number(j, "results", &alloc) == 0U);

        const ReplayRecord rec = decode_file(bad_out, &alloc);
        String             differing(&alloc);
        CHECK(crd::ceir::cook::same_build(rec.build, crd::ceir::cook::current_build(&alloc), differing));
        CHECK(view(rec.program_path) == program);
        CHECK(view(rec.entry) == "main");
        REQUIRE(rec.args.size() == 1U);
        CHECK(rec.args[0] == 1);
        CHECK(rec.error == crd::ceir::plan::RunError::SelectorOutOfRange);
        CHECK(rec.events.size() == rec.events_total);
        CHECK(rec.events_total == number(j, "events_total", &alloc));
        REQUIRE_FALSE(rec.events.empty());
        CHECK(rec.events[rec.events.size() - 1U].op == rec.fault_op); // the switch is the last instr dispatched
        CHECK(rec.content_hash == number(j, "content_hash", &alloc));

        // The selector is read at the switch's safe point: main's addi event carries 1 + 1.
        Context                        ctx(&alloc);
        crd::ceir::cook::ReplayProgram p(&alloc);
        crd::ceir::cook::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main", &registrar, nullptr,
                                             p);
        REQUIRE(p.ok());
        CHECK(p.content_hash == rec.content_hash);
        bool seen_add = false;
        for (const crd::ceir::cook::ReplayEvent& ev : rec.events)
        {
            const crd::ceir::cook::ReplaySite site = crd::ceir::cook::replay_site_of_op(ctx, p, ev.op);
            if (site.line == add.line)
            {
                seen_add = true;
                CHECK(site.col == add.col);
                REQUIRE(ev.values == 1U);
                CHECK(ev.value[0] == 2);
            }
        }
        CHECK(seen_add);

        // One run, one record: recording it again gives the same bytes.
        REQUIRE(record(h, program, again, "1").status == DiagStatus::Ok);
        const Array<u8> first  = slurp_bytes(bad_out, &alloc);
        const Array<u8> second = slurp_bytes(again, &alloc);
        REQUIRE(first.size() == second.size());
        bool same = true;
        for (usize i = 0U; i < first.size(); ++i)
        {
            same = same && first[i] == second[i];
        }
        CHECK(same);

        // The decoder reads back what the encoder wrote, field for field.
        Array<u8> reencoded(&alloc);
        crd::ceir::cook::encode_record(rec, reencoded);
        REQUIRE(reencoded.size() == first.size());
        for (usize i = 0U; i < first.size(); ++i)
        {
            same = same && first[i] == reencoded[i];
        }
        CHECK(same);
    }
    forget(ok_out);
    forget(bad_out);
    forget(again);
    forget(program);
}

TEST_CASE("diag 9a: a record reproduces after the program is edited, and replaying the edit names the first "
          "divergence",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const String                       edit = edited(view(text), &alloc);
    const Where                        sw   = locate(view(text), "core.switch(%10)");
    const Where                        bias = locate(view(edit), kBiasEdited);

    const char* const program = "diag9a_edit_demo.ceir";
    const char* const pristine = "diag9a_edit_pristine.ceir";
    const char* const out      = "diag9a_edit.crpl";
    forget(out);
    write_program(program, view(text));
    write_program(pristine, view(text));

    {
        Host h;
        REQUIRE(record(h, program, out, "1").status == DiagStatus::Ok);
    }
    // The asset is edited after the run: the bias that made the selector out of range is now -1.
    write_program(program, view(edit));

    // A fresh host (nothing carried over from the recording one) replays the record from its own artifact.
    Host             h;
    const DiagResult same = replay(h, out);
    REQUIRE(same.status == DiagStatus::Ok);
    const StringView sj = view(same.json);
    CHECK(view(field(sj, "result", &alloc)) == "\"reproduced\"");
    CHECK(view(field(sj, "program_source", &alloc)) == "\"record\"");
    CHECK(view(field(sj, "program_matches", &alloc)) == "true");
    CHECK(view(field(sj, "build", &alloc)) == "\"same\"");
    CHECK(number(sj, "recorded_events", &alloc) == number(sj, "replayed_events", &alloc));
    CHECK(number(sj, "verified_events", &alloc) == number(sj, "recorded_events", &alloc));
    CHECK_FALSE(has(sj, R"("kind":"divergence")"));
    const usize recorded_at = item_at(sj, R"({"kind":"outcome","run":"recorded")");
    const usize replayed_at = item_at(sj, R"({"kind":"outcome","run":"replayed")");
    REQUIRE(recorded_at != StringView::npos);
    REQUIRE(replayed_at != StringView::npos);
    for (const usize at : {recorded_at, replayed_at})
    {
        CHECK(view(field(sj, "error", &alloc, at)) == "\"selector-out-of-range\"");
        CHECK(view(field(sj, "file", &alloc, at)) == "\"diag9a_edit_demo.ceir\"");
        CHECK(number(sj, "line", &alloc, at) == sw.line);
        CHECK(number(sj, "col", &alloc, at) == sw.col);
    }

    // The same inputs against the edited checkout: the first divergence is the edited constant's value.
    const DiagResult diff = replay(h, out, program);
    REQUIRE(diff.status == DiagStatus::Ok);
    const StringView dj = view(diff.json);
    CHECK(view(field(dj, "result", &alloc)) == "\"diverged\"");
    CHECK(view(field(dj, "divergence", &alloc)) == "\"value\"");
    CHECK(view(field(dj, "program_source", &alloc)) == "\"argument\"");
    CHECK(view(field(dj, "program_matches", &alloc)) == "false");
    const usize d = item_at(dj, R"({"kind":"divergence")");
    REQUIRE(d != StringView::npos);
    CHECK(view(field(dj, "divergence", &alloc, d)) == "\"value\"");
    CHECK(view(field(dj, "recorded", &alloc, d)) == "1");
    CHECK(view(field(dj, "observed", &alloc, d)) == "-1");
    CHECK(view(field(dj, "count", &alloc, d)) == "false");
    CHECK(field(dj, "recorded_op", &alloc, d).size() == field(dj, "observed_op", &alloc, d).size());
    CHECK(view(field(dj, "file", &alloc, d)) == "\"diag9a_edit_demo.ceir\"");
    CHECK(number(dj, "line", &alloc, d) == bias.line);
    CHECK(number(dj, "col", &alloc, d) == bias.col);
    const usize edited_run = item_at(dj, R"({"kind":"outcome","run":"replayed")");
    REQUIRE(edited_run != StringView::npos);
    CHECK(view(field(dj, "error", &alloc, edited_run)) == "\"none\"");

    // An unedited copy of the program is the recorded content: it reproduces, and says the program matches.
    const DiagResult copy = replay(h, out, pristine);
    REQUIRE(copy.status == DiagStatus::Ok);
    CHECK(view(field(view(copy.json), "result", &alloc)) == "\"reproduced\"");
    CHECK(view(field(view(copy.json), "program_matches", &alloc)) == "true");

    forget(out);
    forget(program);
    forget(pristine);
}

TEST_CASE("diag 9a: an incompatible record is refused before anything runs", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const char* const                  program  = "diag9a_bad_demo.ceir";
    const char* const                  original = "diag9a_bad.crpl";
    const char* const                  tampered = "diag9a_bad_tampered.crpl";
    forget(original);
    write_program(program, view(text));

    Host h;
    REQUIRE(record(h, program, original, "1").status == DiagStatus::Ok);
    const u64 executions = h.cmd.executions.load();

    const auto refused = [&](const DiagResult& r, DiagStatus status, const char* reason)
    {
        CHECK(r.status == status);
        CHECK(has(view(r.reason), StringView{reason}));
        CHECK(h.cmd.executions.load() == executions); // nothing ran
    };

    SECTION("another build is unavailable here, naming the field; build=any replays it and says it differs")
    {
        ReplayRecord rec = decode_file(original, &alloc);
        rec.build.config.clear();
        rec.build.config.append("other-config");
        encode_file(rec, tampered, &alloc);
        refused(replay(h, tampered), DiagStatus::Unavailable, "differs in config");

        const DiagResult any = replay(h, tampered, nullptr, "any");
        REQUIRE(any.status == DiagStatus::Ok);
        CHECK(view(field(view(any.json), "result", &alloc)) == "\"reproduced\"");
        CHECK(view(field(view(any.json), "build", &alloc)) == "\"differs\"");
        CHECK(view(field(view(any.json), "build_differs", &alloc)) == "\"config\"");
    }
    SECTION("a record missing an input its program needs is unavailable, naming the input")
    {
        ReplayRecord rec = decode_file(original, &alloc);
        rec.inputs[3]    = {crd::ceir::cook::ReplayNeed::Yes, crd::ceir::cook::ReplayInputState::Missing};
        encode_file(rec, tampered, &alloc);
        refused(replay(h, tampered), DiagStatus::Unavailable, "missing inputs its program needs (random)");
    }
    SECTION("a flipped payload byte fails its checksum")
    {
        Array<u8> bytes = slurp_bytes(original, &alloc);
        bytes[bytes.size() - 3U] ^= 0x5AU;
        forget(tampered);
        spill(tampered, bytes.data(), bytes.size());
        refused(replay(h, tampered), DiagStatus::Failed, "bad-checksum");
    }
    SECTION("another record schema is not read")
    {
        ReplayRecord rec = decode_file(original, &alloc);
        rec.schema       = crd::ceir::cook::kReplayRecordSchema + 1U;
        encode_file(rec, tampered, &alloc);
        refused(replay(h, tampered), DiagStatus::Failed, "unsupported-schema");
    }
    SECTION("a program that is not the recorded content is refused")
    {
        ReplayRecord rec = decode_file(original, &alloc);
        rec.content_hash ^= 1U;
        encode_file(rec, tampered, &alloc);
        refused(replay(h, tampered), DiagStatus::Failed, "does not match the content hash");

        ReplayRecord cut = decode_file(original, &alloc);
        cut.program.resize(cut.program.size() / 2U);
        encode_file(cut, tampered, &alloc);
        refused(replay(h, tampered), DiagStatus::Failed, "the record's program did not load");
    }
    SECTION("a program file is not a record; a truncated record is not either")
    {
        refused(replay(h, program), DiagStatus::Failed, "not-a-record");
        Array<u8> bytes = slurp_bytes(original, &alloc);
        bytes.resize(bytes.size() - 1U);
        forget(tampered);
        spill(tampered, bytes.data(), bytes.size());
        refused(replay(h, tampered), DiagStatus::Failed, "truncated");
    }
    SECTION("the arguments are checked before the record is read")
    {
        const u64 reads = h.cmd.bytes_read.load();
        refused(replay(h, original, "../escape.ceir"), DiagStatus::BadArgument, "'program' must be a relative path");
        refused(replay(h, original, nullptr, "maybe"), DiagStatus::BadArgument, "'build' must be 'match' or 'any'");
        CHECK(h.cmd.bytes_read.load() == reads);
    }
    forget(original);
    forget(tampered);
    forget(program);
}

TEST_CASE("diag 9a: the trace keeps its first events, counts the rest, and the outcome still catches a later edit",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const char* const                  program = "diag9a_bound_demo.ceir";
    const char* const                  edit    = "diag9a_bound_edit.ceir";
    const char* const                  out     = "diag9a_bound.crpl";
    forget(out);
    write_program(program, view(text));
    write_program(edit, view(edited(view(text), &alloc)));

    Host             h;
    const DiagResult r = record(h, program, out, "1", "5");
    REQUIRE(r.status == DiagStatus::Ok);
    const StringView j = view(r.json);
    CHECK(number(j, "events", &alloc) == 5U);
    const u64 total = number(j, "events_total", &alloc);
    CHECK(total > 5U);
    CHECK(number(j, "lost", &alloc) == total - 5U);

    const ReplayRecord rec = decode_file(out, &alloc);
    CHECK(rec.max_events == 5U);
    CHECK(rec.events.size() == 5U);
    CHECK(rec.events_total == total);

    const DiagResult same = replay(h, out);
    REQUIRE(same.status == DiagStatus::Ok);
    CHECK(view(field(view(same.json), "result", &alloc)) == "\"reproduced\"");
    CHECK(number(view(same.json), "verified_events", &alloc) == 5U);
    CHECK(number(view(same.json), "replayed_events", &alloc) == total);

    // The edited constant runs after the kept events: the event count is the same, the outcome is not.
    const DiagResult diff = replay(h, out, edit);
    REQUIRE(diff.status == DiagStatus::Ok);
    CHECK(view(field(view(diff.json), "divergence", &alloc)) == "\"outcome\"");

    forget(out);
    forget(program);
    forget(edit);
}

TEST_CASE("diag 9a: replay.record needs Execute and Record, and refuses before running anything", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const char* const                  program = "diag9a_auth_demo.ceir";
    const char* const                  out     = "diag9a_auth.crpl";
    forget(out);
    write_program(program, view(text));

    using crd::perf::authority_bit;
    for (const crd::perf::DiagAuthoritySet grant : {authority_bit(DiagAuthority::Execute),
                                                    authority_bit(DiagAuthority::Record),
                                                    authority_bit(DiagAuthority::Read)})
    {
        Host             h(grant);
        const DiagResult r = record(h, program, out, "1");
        CHECK(r.status == DiagStatus::Unauthorized);
        CHECK(h.cmd.record_runs.load() == 0U);
        CHECK_FALSE(exists(out));
    }

    Host h;
    const auto refused = [&](const DiagResult& r, DiagStatus status, const char* reason)
    {
        CHECK(r.status == status);
        CHECK(has(view(r.reason), StringView{reason}));
        CHECK(h.cmd.executions.load() == 0U);
        CHECK(h.cmd.records_written.load() == 0U);
    };
    refused(record(h, program, "../out.crpl", "1"), DiagStatus::BadArgument, "'out' must be a relative path");
    refused(record(h, program, out, "1", "0"), DiagStatus::BadArgument, "'max_events' must be 1 to 65536");
    refused(record(h, program, out, "x"), DiagStatus::BadArgument, "'args' must be at most 64");
    {
        DiagRequest r;
        r.command = crd::ceir::cook::kReplayRecordCommand;
        r.path    = StringView{program};
        refused(h.svc.execute(r), DiagStatus::BadArgument, "needs the argument 'out'");
    }
    CHECK(h.cmd.record_runs.load() == 0U); // every refusal so far came from the argument check

    std::atomic<bool> cancel{true};
    refused(record(h, program, out, "1", nullptr, &cancel), DiagStatus::Cancelled, "cancelled");
    CHECK_FALSE(exists(out));

    // An existing file is never overwritten.
    spill(out, "keep", 4U);
    refused(record(h, program, out, "1"), DiagStatus::Failed, "refusing to overwrite an existing file");
    CHECK(view(slurp(out, &alloc)) == "keep");

    // The listing says the command needs both classes.
    DiagRequest list;
    list.command    = StringView{"diag.commands"};
    list.page_items = 64U;
    list.page_bytes = crd::perf::kDiagMaxPageBytes;
    const DiagResult l = h.svc.execute(list);
    REQUIRE(l.status == DiagStatus::Ok);
    CHECK(has(view(l.json), R"("name":"replay.record","owner":"ceir","authority":"execute","also":"record")"));
    CHECK(has(view(l.json), R"("name":"replay.run","owner":"ceir","authority":"execute","granted")"));

    forget(out);
    forget(program);
}

TEST_CASE("diag 9a: replay.prepare answers a run record from what it holds", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text    = slurp(kAsset, &alloc);
    const char* const                  program = "diag9a_prep_demo.ceir";
    const char* const                  out     = "diag9a_prep.crpl";
    forget(out);
    write_program(program, view(text));

    Host h;
    REQUIRE(record(h, program, out, "1").status == DiagStatus::Ok);

    DiagRequest r;
    r.command    = crd::ceir::cook::kReplayPrepareCommand;
    r.path       = StringView{out};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    const DiagResult p = h.svc.execute(r);
    REQUIRE(p.status == DiagStatus::Ok);
    const StringView j = view(p.json);
    CHECK(view(field(j, "form", &alloc)) == "\"record\"");
    CHECK(view(field(j, "replay", &alloc)) == "\"prepared\"");
    CHECK(number(j, "missing", &alloc) == 0U);
    CHECK(has(j, R"("input":"build","guarantee":"identity","needed":"yes","state":"available")"));
    CHECK(has(j, R"("input":"entry-arguments","guarantee":"event","needed":"yes","state":"available")"));
    CHECK(has(j, "the build and configuration a run used is in the run record"));

    // The same program as a file still lacks a build and arguments.
    r.path              = StringView{program};
    const DiagResult pf = h.svc.execute(r);
    REQUIRE(pf.status == DiagStatus::Ok);
    CHECK(view(field(view(pf.json), "replay", &alloc)) == "\"unavailable\"");
    CHECK(view(field(view(pf.json), "missing_inputs", &alloc)) == "\"build,entry-arguments\"");

    forget(out);
    forget(program);
}

TEST_CASE("diag 9a: the first divergence is ordered path, value, length, outcome, results, cells", "[ceir][cook][diag]")
{
    using crd::ceir::cook::Divergence;
    using crd::ceir::cook::DivergenceKind;
    using crd::ceir::cook::ReplayEvent;
    using crd::ceir::cook::ReplayTrace;
    crd::memory::GrowableTlsfAllocator alloc;

    ReplayRecord rec(&alloc);
    ReplayTrace  run(&alloc);
    for (u64 i = 0U; i < 3U; ++i)
    {
        ReplayEvent ev;
        ev.op       = 10U + i;
        ev.depth    = i == 1U ? 1U : 0U;
        ev.values   = 1U;
        ev.value[0] = static_cast<i64>(i) * 7;
        rec.events.push_back(ev);
        run.events.push_back(ev);
        run.sites.push_back(crd::ceir::plan::InstrRef{0U, static_cast<u32>(i)});
    }
    rec.events_total = run.events_total = 3U;
    rec.results.push_back(5);
    run.results.push_back(5);
    rec.cells.push_back(9);
    run.cells.push_back(9);
    CHECK(crd::ceir::cook::first_divergence(rec, run).kind == DivergenceKind::None);

    // Each change alone, in a copy, is reported as its own kind at its own index.
    const auto diverge = [&](const auto& change)
    {
        ReplayTrace copy(&alloc);
        copy.events       = run.events;
        copy.sites        = run.sites;
        copy.events_total = run.events_total;
        copy.results      = run.results;
        copy.cells        = run.cells;
        change(copy);
        return crd::ceir::cook::first_divergence(rec, copy);
    };
    Divergence d = diverge([](ReplayTrace& t) { t.events[1].op = 99U; });
    CHECK(d.kind == DivergenceKind::Path);
    CHECK(d.index == 1U);
    CHECK(d.observed_op == 99U);
    CHECK(d.site.instr == 1U);
    d = diverge([](ReplayTrace& t) { t.events[2].depth = 4U; });
    CHECK(d.kind == DivergenceKind::Path);
    CHECK(d.index == 2U);
    d = diverge([](ReplayTrace& t) { t.events[2].values = 0U; });
    CHECK(d.kind == DivergenceKind::Value);
    CHECK(d.count);
    CHECK(d.recorded == 1);
    CHECK(d.observed == 0);
    d = diverge([](ReplayTrace& t) { t.events[1].value[0] = -3; });
    CHECK(d.kind == DivergenceKind::Value);
    CHECK_FALSE(d.count);
    CHECK(d.index == 1U);
    CHECK(d.recorded == 7);
    CHECK(d.observed == -3);
    d = diverge([](ReplayTrace& t) { t.events_total = 4U; });
    CHECK(d.kind == DivergenceKind::Length);
    CHECK(d.count);
    d = diverge([](ReplayTrace& t) { t.error = crd::ceir::plan::RunError::FuelExhausted; });
    CHECK(d.kind == DivergenceKind::Outcome);
    d = diverge([](ReplayTrace& t) { t.fault_op = 12U; });
    CHECK(d.kind == DivergenceKind::Outcome);
    CHECK(d.observed_op == 12U);
    d = diverge([](ReplayTrace& t) { t.results[0] = 6; });
    CHECK(d.kind == DivergenceKind::Results);
    CHECK(d.observed == 6);
    d = diverge([](ReplayTrace& t) { t.results.push_back(1); });
    CHECK(d.kind == DivergenceKind::Results);
    CHECK(d.count);
    d = diverge([](ReplayTrace& t) { t.cells[0] = 8; });
    CHECK(d.kind == DivergenceKind::Cells);
    CHECK(d.recorded == 9);

    // The earliest event wins over a later one, and an event wins over the outcome.
    d = diverge(
        [](ReplayTrace& t)
        {
            t.events[2].op       = 77U;
            t.events[0].value[0] = 1;
            t.error              = crd::ceir::plan::RunError::BadForStep;
        });
    CHECK(d.kind == DivergenceKind::Value);
    CHECK(d.index == 0U);

    // A truncated record (a replay keeps as many events, by the record's own bound) compares its kept events and its
    // whole count only.
    rec.max_events   = 2U;
    rec.events_total = 3U;
    rec.events.resize(2U);
    const auto truncated = [](ReplayTrace& t)
    {
        t.events.resize(2U);
        t.sites.resize(2U);
    };
    CHECK(diverge(truncated).kind == DivergenceKind::None);
    d = diverge(
        [&](ReplayTrace& t)
        {
            truncated(t);
            t.events_total = 5U;
        });
    CHECK(d.kind == DivergenceKind::Length);
    CHECK(d.index == 2U);
    CHECK(d.recorded == 3);
    CHECK(d.observed == 5);
}
