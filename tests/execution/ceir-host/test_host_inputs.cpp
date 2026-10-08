// DIAG.9a -- host inputs on the host provider (crd-jobs). The provider installs the input ops on its submitting
// interpreter and hands it the host's input source; a host record keeps every draw and a host replay feeds them back
// (never a live source), on the recorded or any other job split. A pooled launch body may not draw (the order of its
// draws would depend on the schedule): one that does runs inline on the submitting thread, which has the source, while
// the same launch without a draw pools. A parallel body that draws is refused by the shared pre-flight. The committed
// assets/ceir/random_demo.ceir fails when a stream-0 draw reduces to 3; which seed fails is computed here from
// SeededInputs::draw. Lines come from scanning the text. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/ceir/host/host_replay.hpp>
#include <crd/ceir/host/host_replay_diag.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <utility>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
namespace ck = crd::ceir::cook;
namespace hs = crd::ceir::host;

namespace
{
constexpr const char* kAsset       = CRD_REPO_DIR "/assets/ceir/random_demo.ceir";
constexpr const char* kFile        = "ceir/random_demo.ceir";
constexpr const char* kDrawLine    = "%4 = input.random() {stream = 0, bound = 4} : !i32";
constexpr const char* kDrawEdited  = "%4 = input.random() {stream = 2, bound = 4} : !i32";
constexpr const char* kSwitchLine  = "core.switch(%4)";
constexpr u32         kDraws       = 4U;
constexpr u64         kSeed        = 77U; // the launch and parallel cases' host seed

// main() launches a body that draws once from stream 5 and awaits it; the second program's body only adds.
constexpr const char* kLaunchDraws = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0:
        %0 = async.launch() : !i32 {
          ^bb0:
            %1 = input.random() {stream = 5, bound = 1000} : !i32
            core.yield(%1)
        }
        %2 = async.await(%0) : !i32
        func.return(%2)
    }
})";
constexpr const char* kLaunchAdds = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0:
        %0 = async.launch() : !i32 {
          ^bb0:
            %1 = arith.const() {value = 20} : !i32
            %2 = arith.addi(%1, %1) : !i32
            core.yield(%2)
        }
        %3 = async.await(%0) : !i32
        func.return(%3)
    }
})";
constexpr const char* kParallelDraws = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0:
        %0 = arith.const() {value = 0} : !i32
        %1 = arith.const() {value = 4} : !i32
        %2 = arith.const() {value = 1} : !i32
        task.parallel_for(%0, %1, %2) {
          ^bb0(%3 : !i32):
            %4 = input.random() {stream = 0, bound = 10} : !i32
            core.yield(%4)
        }
        func.return(%0)
    }
})";

void register_dialects(Context& ctx, void* /*user*/)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)task::register_task_ops(ctx);
    (void)async::register_async_ops(ctx);
    (void)func::register_dialect(ctx);
    (void)input::register_input_ops(ctx);
}

String slurp(const char* path, memory::IAllocator* a)
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

u32 line_of(StringView text, const char* needle)
{
    const usize at = text.find(StringView{needle});
    REQUIRE(at != StringView::npos);
    u32 line = 1U;
    for (usize i = 0U; i < at; ++i)
    {
        line += text[i] == '\n' ? 1U : 0U;
    }
    return line;
}

String replaced(StringView text, const char* from, const char* to, memory::IAllocator* a)
{
    const usize at = text.find(StringView{from});
    REQUIRE(at != StringView::npos);
    String out(a);
    out.append(text.substr(0U, at));
    out.append(to);
    out.append(text.substr(at + StringView{from}.size()));
    return out;
}

Array<u8> cooked(StringView text, memory::IAllocator* alloc)
{
    Context ctx(alloc);
    register_dialects(ctx, nullptr);
    ck::CookResult cr = ck::cook_program_text(ctx, text, StringView{kFile}, 1U, alloc, alloc);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}

// The first seed for which main(kDraws) fails; `failing` gets the draw that fails.
u64 failing_seed(u32& failing)
{
    for (u64 seed = 1U; seed < 10000U; ++seed)
    {
        for (u32 i = 0U; i < kDraws; ++i)
        {
            if (input::reduce_draw(input::SeededInputs::draw(seed, 0U, i), 4U) == 3)
            {
                failing = i;
                return seed;
            }
        }
    }
    FAIL("no failing seed");
    return 0U;
}

const Operation* first_random(const Context& ctx, const Region* r)
{
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (input::reads_input(ctx, op->kind()))
            {
                return op;
            }
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                if (const Operation* const hit = first_random(ctx, op->region(i)); hit != nullptr)
                {
                    return hit;
                }
            }
        }
    }
    return nullptr;
}

Module* parse_ok(Context& ctx, const char* text)
{
    register_dialects(ctx, nullptr);
    const ParseResult pr = parse(ctx, StringView{text});
    REQUIRE(pr.module != nullptr);
    ctx.assign_stable_ids(*pr.module);
    return pr.module;
}
} // namespace

TEST_CASE("diag 9a input: a host record keeps every draw and replays them on any job split",
          "[ceir][host][diag][input]")
{
    memory::GrowableTlsfAllocator alloc;
    const String                  text  = slurp(kAsset, &alloc);
    const StringView              src   = StringView{text.data(), text.size()};
    const Array<u8>               blob  = cooked(src, &alloc);
    const u32                     draw  = line_of(src, kDrawLine);
    const u32                     sw    = line_of(src, kSwitchLine);
    u32                           k     = 0U;
    const u64                     seed  = failing_seed(k);
    const i64                     args[1] = {static_cast<i64>(kDraws)};
    INFO("seed " << seed << " fails at draw " << k);

    input::SeededInputs live(seed, &alloc);
    ck::ReplayRecord    rec(&alloc);
    String              missing(&alloc);
    hs::HostSite        fault(&alloc);
    REQUIRE(hs::record_host_run({blob.data(), blob.size()}, StringView{kFile}, StringView{"main"},
                                ConstSpan<i64>(args, 1U), hs::HostSchedule{}, 64U, &register_dialects, nullptr, rec,
                                &missing, &fault, live.source()) == hs::HostReplayStatus::Ok);
    CHECK(rec.host_error == exec::ExecError::SelectorOutOfRange);
    CHECK(fault.line == sw);
    CHECK(StringView{missing.data(), missing.size()}.empty());
    CHECK(rec.inputs[3].state == ck::ReplayInputState::Recorded);
    REQUIRE(rec.input_reads.size() == k + 1U);
    for (u32 i = 0U; i <= k; ++i)
    {
        CHECK(rec.input_reads[i] ==
              ck::ReplayInputRead{input::InputKind::Random, 0U, true, input::SeededInputs::draw(seed, 0U, i)});
    }

    // The host executor of the replay commands makes the same record from a seed.
    {
        ck::HostRecordRequest request;
        request.blob      = {blob.data(), blob.size()};
        request.path      = StringView{kFile};
        request.entry     = StringView{"main"};
        request.args      = ConstSpan<i64>(args, 1U);
        request.max_events = 64U;
        request.registrar = &register_dialects;
        request.has_seed  = true;
        request.seed      = seed;
        ck::ReplayRecord     via(&alloc);
        ck::OwnedReplaySite  site(&alloc);
        String               via_missing(&alloc);
        String               reason(&alloc);
        REQUIRE(hs::host_replay_executor().record(request, via, site, via_missing, reason) ==
                ck::HostExecutorStatus::Ok);
        Array<u8> a(&alloc);
        Array<u8> b(&alloc);
        ck::encode_record(rec, a);
        ck::encode_record(via, b);
        CHECK(a.size() == b.size());
        CHECK(StringView{reinterpret_cast<const char*>(a.data()), a.size()} ==
              StringView{reinterpret_cast<const char*>(b.data()), b.size()});
    }

    // Replays: from the record's own draws, on the recorded split and on 1 and 16 jobs.
    for (const u32 jobs : {0U, 1U, 16U})
    {
        INFO("jobs " << jobs);
        hs::HostReplayOptions options;
        options.num_jobs = jobs;
        hs::HostReplay out(&alloc);
        REQUIRE(hs::replay_host_record(rec, &register_dialects, nullptr, options, out) == hs::HostReplayStatus::Ok);
        CHECK(out.divergence.kind == ck::DivergenceKind::None);
        CHECK(out.trace.host_error == exec::ExecError::SelectorOutOfRange);
        CHECK(out.fault.line == sw);
        CHECK(out.trace.input_reads_total == k + 1U);
    }

    // The same draws against an edit that reads stream 2: refused at that read, named at its line.
    const String    edit = replaced(src, kDrawLine, kDrawEdited, &alloc);
    const Array<u8> other = cooked(StringView{edit.data(), edit.size()}, &alloc);
    hs::HostReplayOptions options;
    options.against = {other.data(), other.size()};
    hs::HostReplay out(&alloc);
    REQUIRE(hs::replay_host_record(rec, &register_dialects, nullptr, options, out) == hs::HostReplayStatus::Ok);
    CHECK(out.divergence.kind == ck::DivergenceKind::Input);
    CHECK(out.divergence.index == 0U);
    CHECK(out.divergence.recorded == 0);
    CHECK(out.divergence.observed == 2);
    CHECK(out.site.line == draw);

    // A host with no random source: the first draw fails, that read is recorded, and the replay reproduces it.
    ck::ReplayRecord none(&alloc);
    REQUIRE(hs::record_host_run({blob.data(), blob.size()}, StringView{kFile}, StringView{"main"},
                                ConstSpan<i64>(args, 1U), hs::HostSchedule{}, 64U, &register_dialects, nullptr, none,
                                nullptr, &fault) == hs::HostReplayStatus::Ok);
    CHECK(none.host_error == exec::ExecError::InputUnavailable);
    CHECK(fault.line == draw);
    REQUIRE(none.input_reads.size() == 1U);
    CHECK_FALSE(none.input_reads[0].delivered);
    hs::HostReplay again(&alloc);
    REQUIRE(hs::replay_host_record(none, &register_dialects, nullptr, hs::HostReplayOptions{}, again) ==
            hs::HostReplayStatus::Ok);
    CHECK(again.divergence.kind == ck::DivergenceKind::None);
}

TEST_CASE("diag 9a input: a launch body that draws runs inline with the source, and a parallel body may not draw",
          "[ceir][host][diag][input]")
{
    memory::GrowableTlsfAllocator alloc;

    SECTION("the launch that draws runs on the submitting thread; the same launch without a draw pools")
    {
        Context             ctx(&alloc);
        const Module* const m = parse_ok(ctx, kLaunchDraws);
        input::SeededInputs live(kSeed, &alloc);
        hs::HostProvider    provider(&alloc);
        provider.set_input_source(live.source());
        const exec::ExecResult r = provider.execute(ctx, *m, StringView{"main"}, {});
        REQUIRE(r.ok());
        REQUIRE(r.values.size() == 1U);
        CHECK(r.values[0] == input::reduce_draw(input::SeededInputs::draw(kSeed, 5U, 0U), 1000U));
        CHECK(provider.pooled_count() == 0U);

        Context             actx(&alloc);
        const Module* const am = parse_ok(actx, kLaunchAdds);
        hs::HostProvider    pooled(&alloc);
        const exec::ExecResult a = pooled.execute(actx, *am, StringView{"main"}, {});
        REQUIRE(a.ok());
        CHECK(a.values[0] == 40);
        CHECK(pooled.pooled_count() == 1U);
    }
    SECTION("without a source the inline launch fails at its draw")
    {
        Context                ctx(&alloc);
        const Module* const    m = parse_ok(ctx, kLaunchDraws);
        hs::HostProvider       provider(&alloc);
        const exec::ExecResult r = provider.execute(ctx, *m, StringView{"main"}, {});
        CHECK(r.error == exec::ExecError::InputUnavailable);
        CHECK(r.op == first_random(ctx, m->body()));
    }
    SECTION("a parallel body that draws is refused before anything runs")
    {
        Context             ctx(&alloc);
        const Module* const m = parse_ok(ctx, kParallelDraws);
        input::SeededInputs live(kSeed, &alloc);
        hs::HostProvider    provider(&alloc);
        provider.set_input_source(live.source());
        const exec::ExecResult r = provider.execute(ctx, *m, StringView{"main"}, {});
        CHECK(r.error == exec::ExecError::ParallelBodyStateful);
        CHECK(r.op == first_random(ctx, m->body()));
        i64 next = 0;
        REQUIRE(input::read_input(live.source(), input::InputKind::Random, 0U, next));
        CHECK(next == input::SeededInputs::draw(kSeed, 0U, 0U)); // nothing was drawn
    }
}
