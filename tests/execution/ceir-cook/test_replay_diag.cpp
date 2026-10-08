// DIAG.8c -- the replay-preparation diagnostic command. `replay.prepare` is registered into crd-perf's command service
// and answers, for an authored program file under the host's root, one item per replay input: whether the program
// needs it (from each op's effective effects, a call charged with its callee's), whether it exists, and the precise
// reason it is missing. A program file holds no run, so its answer is always "unavailable" and names the missing
// inputs; the program's content hash is the one input that exists (a run record's answer is test_replay_record.cpp's).
//
// The committed authored program assets/ceir/inspect_demo.ceir needs only its identity and its entry arguments, in
// all three forms. A specimen program whose host registers ops with RandomRead, TimeRead, SceneRead, FileIO,
// Synchronization and GPUCommand effects, with the random and scene reads inside a callee defined after its caller,
// blames each input on the first op that reaches it: the call, not the callee's op. A program holding an op the host
// never registered makes every effect-derived need unknown, never "not needed". Refusals run nothing; an oversized
// file is refused unread. Expected lines and columns come from scanning the text, never from the parser. ASCII test
// names (ctest by-name).

#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_diag.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/effect.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/semantics.hpp>

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
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::DeterminismClass;
using crd::ceir::EffectFamily;
using crd::ceir::EffectRecord;
using crd::ceir::EffectTarget;
using crd::ceir::cook::ReplayPrepareCommand;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;

constexpr const char* kFile   = "ceir/inspect_demo.ceir";
constexpr const char* kAssets = CRD_REPO_DIR "/assets";
constexpr const char* kAsset  = CRD_REPO_DIR "/assets/ceir/inspect_demo.ceir";

// Each case owns its scratch files in the working directory (the file root ".").
constexpr const char* kBinaryFile  = "diag8c_replay_demo.ceirb";
constexpr const char* kCookedFile  = "diag8c_replay_demo.crdr";
constexpr const char* kEffectsFile = "diag8c_replay_effects.ceir";
constexpr const char* kOpaqueFile  = "diag8c_replay_opaque.ceir";
constexpr const char* kBrokenFile  = "diag8c_replay_broken.ceir";

// The specimen: main reads the clock, calls @noise (defined after it), reads a file, synchronizes, dispatches device
// work and calls @noise again; @noise draws a random number and reads the scene. sync makes no determinism claim.
constexpr const char* kEffectsText = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i32):
        %1 = rp.clock() : !i32
        %2 = func.call(%0) {callee = @noise} : !i32
        %3 = rp.fread() : !i32
        %4 = rp.sync() : !i32
        %5 = rp.dispatch() : !i32
        %6 = func.call(%5) {callee = @noise} : !i32
        %7 = arith.addi(%2, %6) : !i32
        func.return(%7)
    }
    func.func() {sym_name = "noise"} {
      ^bb0(%8 : !i32):
        %9 = rp.rng() : !i32
        %10 = rp.scene() : !i32
        %11 = arith.addi(%8, %9) : !i32
        func.return(%11)
    }
}
)";

// An op no host registers sits between two the host knows.
constexpr const char* kOpaqueText = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i32):
        %1 = arith.addi(%0, %0) : !i32
        %2 = zz.mystery(%1) : !i32
        %3 = rp.clock() : !i32
        func.return(%2)
    }
}
)";

constexpr EffectRecord kTime[]     = {{EffectFamily::TimeRead, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kRandom[]   = {{EffectFamily::RandomRead, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kScene[]    = {{EffectFamily::SceneRead, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kFileRead[] = {{EffectFamily::FileIO, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kSync[]     = {{EffectFamily::Synchronization, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kDispatch[] = {{EffectFamily::GPUCommand, EffectTarget::None, 0U, 0U}};

ConstSpan<EffectRecord> one(const EffectRecord* e)
{
    return ConstSpan<EffectRecord>(e, 1U);
}

void register_dialects(Context& ctx)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
}
void registrar(Context& ctx, void* /*user*/)
{
    register_dialects(ctx);
}

// The host of the specimen: the base dialects and the probe ops, each with one effect.
void probe_registrar(Context& ctx, void* /*user*/)
{
    register_dialects(ctx);
    crd::ceir::Dialect* const d = ctx.register_dialect("rp");
    (void)d->register_op("clock", {.effects = one(kTime), .determinism = DeterminismClass::ExternalNondeterminism});
    (void)d->register_op("rng", {.effects = one(kRandom), .determinism = DeterminismClass::BitExact});
    (void)d->register_op("scene", {.effects = one(kScene), .determinism = DeterminismClass::DeterministicWithinTarget});
    (void)d->register_op("fread", {.effects = one(kFileRead), .determinism = DeterminismClass::ExternalNondeterminism});
    (void)d->register_op("sync", {.effects = one(kSync)});
    (void)d->register_op("dispatch",
                         {.effects = one(kDispatch), .determinism = DeterminismClass::DeterministicWithinBackend});
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

void spill(const char* path, const void* data, usize size)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
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

void append_u64(String& s, u64 v)
{
    char buf[24];
    (void)std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    s.append(buf);
}

// The independent oracle for a blamed op: the 1-based line holding `needle` and the column of that line's first
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
    w.line            = 1U;
    usize line_start  = 0U;
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

// Count the scanned text's ops whose name starts with `prefix` ("func." finds the ops that make no claim).
u32 count_ops(StringView text, StringView prefix)
{
    u32   n   = 0U;
    usize pos = 0U;
    while ((pos = text.find(prefix, pos)) != StringView::npos)
    {
        const usize paren = text.find('(', pos);
        const usize space = text.find(' ', pos);
        n += paren != StringView::npos && paren < space ? 1U : 0U;
        pos += prefix.size();
    }
    return n;
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
        usize next = json.find("{\"kind\":", pos + 1U);
        usize stop = next == StringView::npos ? json.size() - 2U : next - 1U; // drop "]}" or the separating comma
        String item(a);
        item.append(json.substr(pos, stop - pos));
        out.push_back(std::move(item));
        pos = next;
    }
    return out;
}

// One input item's state fields, in the command's order.
String state_fields(const char* input, const char* guarantee, const char* needed, const char* state, u64 ops,
                    crd::memory::IAllocator* a)
{
    String s(a);
    s.append(R"({"kind":"input","input":")");
    s.append(input);
    s.append(R"(","guarantee":")");
    s.append(guarantee);
    s.append(R"(","needed":")");
    s.append(needed);
    s.append(R"(","state":")");
    s.append(state);
    s.append(R"(","ops":)");
    append_u64(s, ops);
    s.append(",");
    return s;
}

// The blamed op's fields and the reason that follows them.
String blame_fields(const char* op_name, const char* file, Where w, const char* reason, crd::memory::IAllocator* a)
{
    String s(a);
    s.append(R"("op_name":")");
    s.append(op_name);
    s.append(R"(","file":")");
    s.append(file);
    s.append(R"(","line":)");
    append_u64(s, w.line);
    s.append(R"(,"col":)");
    append_u64(s, w.col);
    s.append(R"(,"reason":")");
    s.append(reason);
    s.append("\"");
    return s;
}

// An item that blames no op.
String no_blame(const char* reason, crd::memory::IAllocator* a)
{
    String s(a);
    s.append(R"("op":0,"op_name":"","file":"","line":0,"col":0,"reason":")");
    s.append(reason);
    s.append("\"");
    return s;
}

DiagResult run(DiagCommandService& svc, const char* path, u32 page_items = 64U)
{
    DiagRequest r;
    r.command    = crd::ceir::cook::kReplayPrepareCommand;
    r.path       = StringView{path};
    r.page_items = page_items;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return svc.execute(r);
}

DiagServiceConfig rooted(const char* root)
{
    DiagServiceConfig c;
    c.root = StringView{root};
    return c;
}

constexpr crd::perf::DiagAuthoritySet kRead = crd::perf::authority_bit(DiagAuthority::Read);
} // namespace

TEST_CASE("diag 8c: replay.prepare says the committed program needs only its identity and entry arguments",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text      = slurp(kAsset, &root);
    const u32                          unclaimed = count_ops(view(text), "func.");
    REQUIRE(unclaimed == 5U); // the oracle itself: two func.func, one func.call, two func.return

    SECTION("the committed text: every effect-derived input is not needed, and the replay is unavailable")
    {
        ReplayPrepareCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService svc(kRead, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_replay_prepare(svc, cmd));
        CHECK_FALSE(crd::ceir::cook::register_replay_prepare(svc, cmd)); // one name, one command

        const DiagResult r = run(svc, kFile);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(r.total == 9U);
        CHECK(r.complete);
        CHECK(has(view(r.json), R"("form":"text")"));
        String claim(&root);
        claim.append(R"("ops":15,"unregistered":0,"opaque":0,"weakest_claim":"bit-exact","unclaimed":)");
        append_u64(claim, unclaimed);
        claim.append(R"(,"needed":3,"unknown":0,"missing":2,"replay":"unavailable",)");
        claim.append(R"("missing_inputs":"build,entry-arguments")");
        CHECK(has(view(r.json), view(claim)));
        CHECK_FALSE(has(view(r.json), StringView{CRD_REPO_DIR})); // the host's root never reaches the answer

        const Array<String> items = items_of(r, &root);
        REQUIRE(items.size() == 9U);
        CHECK(has(view(items[0]), view(state_fields("program", "identity", "yes", "available", 0U, &root))));
        CHECK(has(view(items[0]), view(no_blame("the program's content hash is computed from the file", &root))));
        CHECK(has(view(items[1]), view(state_fields("build", "identity", "yes", "missing", 0U, &root))));
        CHECK(has(view(items[2]), view(state_fields("entry-arguments", "event", "yes", "missing", 0U, &root))));
        CHECK(has(view(items[1]), view(no_blame("a program file names no build; replay.record writes the build a "
                                                "run used into a run record",
                                                &root))));
        CHECK(has(view(items[2]), view(no_blame("a program file holds no run's entry arguments; replay.record writes "
                                                "them into a run record",
                                                &root))));
        const char* const derived[6][2] = {
            {"random", "event"},           {"clock", "event"},    {"host-state", "event"},
            {"external-results", "event"}, {"schedule", "schedule"}, {"device-tolerance", "numeric"},
        };
        for (usize i = 0; i < 6U; ++i)
        {
            INFO(items[3U + i].c_str());
            CHECK(has(view(items[3U + i]),
                      view(state_fields(derived[i][0], derived[i][1], "no", "not-needed", 0U, &root))));
            CHECK(has(view(items[3U + i]), R"("op":0,"op_name":"","file":"","line":0,"col":0,"reason":"no op needs )"));
        }
        CHECK(cmd.runs.load() == 1U);
        CHECK(cmd.bytes_read.load() == text.size());

        // Pages are cut from the one snapshot.
        DiagRequest next;
        next.command    = crd::ceir::cook::kReplayPrepareCommand;
        next.path       = StringView{kFile};
        next.page_items = 4U;
        u64   cursor    = 0U;
        usize seen      = 0U;
        do
        {
            next.cursor           = cursor;
            const DiagResult page = svc.execute(next);
            REQUIRE(page.status == DiagStatus::Ok);
            const Array<String> got = items_of(page, &root);
            for (usize k = 0; k < got.size(); ++k)
            {
                CHECK(view(got[k]) == view(items[seen + k]));
            }
            seen += got.size();
            cursor = page.next_cursor;
        } while (cursor != 0U);
        CHECK(seen == 9U);
    }

    SECTION("the binary and cooked forms give the same answer and the same content hash")
    {
        Context ctx(&root);
        register_dialects(ctx);
        const crd::ceir::ParseResult pr = crd::ceir::parse(ctx, view(text), ctx.register_file(StringView{kFile}));
        REQUIRE(pr.ok);
        const Array<u8> blob = crd::ceir::serialize(ctx, *pr.module, &root);
        spill(kBinaryFile, blob.data(), blob.size());

        Context cook_ctx(&root);
        register_dialects(cook_ctx);
        const crd::ceir::cook::CookResult cr =
            crd::ceir::cook::cook_program_text(cook_ctx, view(text), StringView{kFile}, 4802U, &root, &root);
        REQUIRE(cr.ok());
        spill(kCookedFile, cr.blob.data(), cr.blob.size());

        ReplayPrepareCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService text_svc(kRead, rooted(kAssets), &root);
        DiagCommandService file_svc(kRead, rooted("."), &root);
        REQUIRE(crd::ceir::cook::register_replay_prepare(text_svc, cmd));
        REQUIRE(crd::ceir::cook::register_replay_prepare(file_svc, cmd));

        const DiagResult from_text = run(text_svc, kFile);
        const DiagResult binary    = run(file_svc, kBinaryFile);
        const DiagResult cooked    = run(file_svc, kCookedFile);
        INFO(binary.json.c_str());
        INFO(cooked.json.c_str());
        REQUIRE(from_text.status == DiagStatus::Ok);
        REQUIRE(binary.status == DiagStatus::Ok);
        REQUIRE(cooked.status == DiagStatus::Ok);
        CHECK(has(view(binary.json), R"("form":"binary")"));
        CHECK(has(view(cooked.json), R"("form":"cooked")"));

        String hash(&root);
        hash.append("\"content_hash\":");
        append_u64(hash, cr.content_hash);
        hash.append(",\"recorded_hash\":");
        String recorded = hash;
        append_u64(recorded, cr.content_hash);
        CHECK(has(view(cooked.json), view(recorded)));
        CHECK(has(view(binary.json), view(hash)));
        CHECK(has(view(from_text.json), view(hash)));

        const Array<String> t = items_of(from_text, &root);
        const Array<String> b = items_of(binary, &root);
        const Array<String> c = items_of(cooked, &root);
        REQUIRE(t.size() == 9U);
        REQUIRE(b.size() == 9U);
        REQUIRE(c.size() == 9U);
        for (usize i = 0; i < 9U; ++i)
        {
            CHECK(view(b[i]) == view(t[i]));
            CHECK(view(c[i]) == view(t[i]));
        }
        (void)std::remove(kBinaryFile);
        (void)std::remove(kCookedFile);
    }
}

TEST_CASE("diag 8c: replay.prepare blames each needed input on the first op that reaches it, through calls",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const StringView                   text{kEffectsText};
    spill(kEffectsFile, text.data(), text.size());

    ReplayPrepareCommand cmd;
    cmd.registrar = &probe_registrar;
    DiagCommandService svc(kRead, rooted("."), &root);
    REQUIRE(crd::ceir::cook::register_replay_prepare(svc, cmd));
    const DiagResult r = run(svc, kEffectsFile);
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);

    // func.* (6 ops) and rp.sync make no claim; rp.clock and rp.fread are the weakest claims.
    CHECK(has(view(r.json), R"("ops":14,"unregistered":0,"opaque":0,"weakest_claim":"external-nondeterminism",)"
                            R"("unclaimed":7,"needed":9,"unknown":0,"missing":8,"replay":"unavailable",)"
                            R"("missing_inputs":"build,entry-arguments,random,clock,host-state,external-results,)"
                            R"(schedule,device-tolerance")"));

    const Array<String> items = items_of(r, &root);
    REQUIRE(items.size() == 9U);
    const Where call  = locate(text, "func.call(%0) {callee = @noise}");
    const Where clock = locate(text, "rp.clock()");
    const Where fread = locate(text, "rp.fread()");
    const Where sync  = locate(text, "rp.sync()");
    const Where gpu   = locate(text, "rp.dispatch()");

    // The random draw and the scene read sit in @noise, after main: the first call that reaches them is blamed, and
    // each call counts as well as the callee's own op.
    CHECK(has(view(items[3]), view(state_fields("random", "event", "yes", "missing", 3U, &root))));
    CHECK(has(view(items[3]),
              view(blame_fields("func.call", kEffectsFile, call,
                                "the random draws are not held; replay.record keeps every input.random draw", &root))));
    CHECK(has(view(items[5]), view(state_fields("host-state", "event", "yes", "missing", 3U, &root))));
    CHECK(has(view(items[5]), view(blame_fields("func.call", kEffectsFile, call,
                                                "nothing records the host and world state read", &root))));
    CHECK(has(view(items[4]), view(state_fields("clock", "event", "yes", "missing", 1U, &root))));
    CHECK(has(view(items[4]), view(blame_fields("rp.clock", kEffectsFile, clock,
                                                "the time reads are not held; replay.record keeps every "
                                                "input.clock and input.time_step read",
                                                &root))));

    // A resolved call is not an opaque one: only the file read needs external results.
    CHECK(has(view(items[6]), view(state_fields("external-results", "event", "yes", "missing", 1U, &root))));
    CHECK(has(view(items[6]),
              view(blame_fields("rp.fread", kEffectsFile, fread,
                                "nothing records the external I/O completions and opaque call results", &root))));
    CHECK(has(view(items[7]), view(state_fields("schedule", "schedule", "yes", "missing", 1U, &root))));
    CHECK(has(view(items[7]), view(blame_fields("rp.sync", kEffectsFile, sync,
                                                "nothing records the schedule choices a run made", &root))));
    CHECK(has(view(items[8]), view(state_fields("device-tolerance", "numeric", "yes", "missing", 1U, &root))));
    const char* const tolerance = "nothing declares a tolerance or oracle for backend-specific device numerics";
    CHECK(has(view(items[8]), view(blame_fields("rp.dispatch", kEffectsFile, gpu, tolerance, &root))));
    for (usize i = 0; i < items.size(); ++i)
    {
        CHECK_FALSE(has(view(items[i]), R"("clipped":true)"));
    }
    (void)std::remove(kEffectsFile);
}

TEST_CASE("diag 8c: replay.prepare reports an op it cannot know as an unknown need, never as no need",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const StringView                   text{kOpaqueText};
    spill(kOpaqueFile, text.data(), text.size());

    ReplayPrepareCommand cmd;
    cmd.registrar = &probe_registrar; // registers rp, never zz
    DiagCommandService svc(kRead, rooted("."), &root);
    REQUIRE(crd::ceir::cook::register_replay_prepare(svc, cmd));
    const DiagResult r = run(svc, kOpaqueFile);
    INFO(r.json.c_str());
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(has(view(r.json), R"("unregistered":1,"opaque":1,)"));
    CHECK(has(view(r.json), R"("needed":5,"unknown":4,"missing":8,"replay":"unavailable",)"));

    const Array<String> items   = items_of(r, &root);
    const Where         mystery = locate(text, "zz.mystery(%1)");
    const Where         clock   = locate(text, "rp.clock()");
    REQUIRE(items.size() == 9U);

    // The unknown op is opaque, so it needs external results, and may need every other effect-derived input.
    CHECK(has(view(items[6]), view(state_fields("external-results", "event", "yes", "missing", 1U, &root))));
    CHECK(has(view(items[6]),
              view(blame_fields("zz.mystery", kOpaqueFile, mystery,
                                "nothing records the external I/O completions and opaque call results", &root))));
    CHECK(has(view(items[3]), view(state_fields("random", "event", "unknown", "missing", 0U, &root))));
    CHECK(has(view(items[3]), view(blame_fields("zz.mystery", kOpaqueFile, mystery,
                                                "an opaque op may need random streams; the random draws are not "
                                                "held; replay.record keeps every input.random draw",
                                                &root))));
    CHECK(has(view(items[5]), view(state_fields("host-state", "event", "unknown", "missing", 0U, &root))));
    CHECK(has(view(items[7]), view(state_fields("schedule", "schedule", "unknown", "missing", 0U, &root))));
    CHECK(has(view(items[8]), view(state_fields("device-tolerance", "numeric", "unknown", "missing", 0U, &root))));

    // A known op that does need an input is still blamed by name.
    CHECK(has(view(items[4]), view(state_fields("clock", "event", "yes", "missing", 1U, &root))));
    CHECK(has(view(items[4]), view(blame_fields("rp.clock", kOpaqueFile, clock,
                                                "the time reads are not held; replay.record keeps every "
                                                "input.clock and input.time_step read",
                                                &root))));
    (void)std::remove(kOpaqueFile);
}

TEST_CASE("diag 8c: replay.prepare refuses before reading and names where a text stopped parsing", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;

    SECTION("service refusals never reach the command")
    {
        ReplayPrepareCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService record_only(crd::perf::authority_bit(DiagAuthority::Record), rooted(kAssets), &root);
        DiagCommandService no_root(kRead, DiagServiceConfig{}, &root);
        DiagCommandService svc(kRead, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_replay_prepare(record_only, cmd));
        REQUIRE(crd::ceir::cook::register_replay_prepare(no_root, cmd));
        REQUIRE(crd::ceir::cook::register_replay_prepare(svc, cmd));

        CHECK(run(record_only, kFile).status == DiagStatus::Unauthorized);
        CHECK(run(no_root, kFile).status == DiagStatus::Unavailable);
        CHECK(run(svc, "../assets/ceir/inspect_demo.ceir").status == DiagStatus::BadArgument);
        std::atomic<bool> cancel{true};
        DiagRequest       r;
        r.command = crd::ceir::cook::kReplayPrepareCommand;
        r.path    = StringView{kFile};
        CHECK(svc.execute(r, &cancel).status == DiagStatus::Cancelled);
        CHECK(cmd.runs.load() == 0U);
        CHECK(cmd.bytes_read.load() == 0U);
    }

    SECTION("a program over the host's limit is refused without reading a byte")
    {
        ReplayPrepareCommand cmd;
        cmd.registrar         = &registrar;
        cmd.max_program_bytes = 64U;
        DiagCommandService svc(kRead, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_replay_prepare(svc, cmd));
        const DiagResult r = run(svc, kFile);
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Oversized);
        CHECK(has(view(r.json), "the host's limit is 64"));
        CHECK(cmd.runs.load() == 1U);
        CHECK(cmd.bytes_read.load() == 0U);
    }

    SECTION("a text that does not parse is refused with its file, line and column")
    {
        const StringView good{kEffectsText};
        const usize      at = good.find("        %7 = arith.addi");
        REQUIRE(at != StringView::npos);
        String bad(&root);
        bad.append(good.substr(0U, at));
        bad.append("    )\n");
        bad.append(good.substr(at));
        const Where broken = locate(view(bad), "    )\n");
        spill(kBrokenFile, bad.data(), bad.size());

        ReplayPrepareCommand cmd;
        cmd.registrar = &probe_registrar;
        DiagCommandService svc(kRead, rooted("."), &root);
        REQUIRE(crd::ceir::cook::register_replay_prepare(svc, cmd));
        const DiagResult r = run(svc, kBrokenFile);
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Failed);
        String where(&root);
        where.append("the program text did not parse at ");
        where.append(kBrokenFile);
        where.push_back(':');
        append_u64(where, broken.line);
        where.append(":5:"); // the ")" after the four-space indent
        CHECK(has(view(r.json), view(where)));
        CHECK(r.items == 0U);
        (void)std::remove(kBrokenFile);
    }
}
