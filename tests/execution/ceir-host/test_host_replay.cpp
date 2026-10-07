// DIAG.9a -- run records of the host provider. A host record traces the provider's submitting interpreter and stores
// the provider's schedule settings (job split, per-body step budget) as its schedule input. The program pools an
// async.launch whose body calls a function, folds a task.map_reduce whose map body loops, keeps a state cell and ends
// in a loop whose step is the entry argument (main(0) faults there). The trace must not depend on the job split and
// must never show an op of a body the provider ran on a sub-interpreter; the record must survive its encoding and
// replay in a fresh Context with the recorded schedule (a small step budget's failure is reproduced only because the
// budget is replayed); an edited constant inside the pooled body must be named at the await that reads its value;
// incompatible records must be refused before anything runs. Expected positions come from scanning the printed text.
// ⛔ The jobs pool is owned by the listener in test_host_provider.cpp (same binary). ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_replay.hpp>
#include <crd/ceir/print.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
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
constexpr const char* kFile = "programs/diag/host_replay.ceir";

void register_dialects(Context& ctx, void* /*user*/)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)task::register_task_ops(ctx);
    (void)async::register_async_ops(ctx);
    (void)func::register_dialect(ctx);
}

Value* konst(Context& ctx, Block* b, i64 v)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32());
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op->result(0U);
}

Value* bin(Context& ctx, const char* name, Block* b, Value* x, Value* y)
{
    Value*           in[2] = {x, y};
    Operation* const op =
        ctx.create_operation(ctx.intern_op("arith", name), ConstSpan<Value*>(in, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op->result(0U);
}

void yield_values(Context& ctx, Block* b, ConstSpan<Value*> vs)
{
    b->append(ctx.create_operation(ctx.intern_op("core", "yield"), vs, 0U));
}

// core.for(lo, hi, step) { yield } in `b`.
void loop(Context& ctx, Block* b, Value* lo, Value* hi, Value* step)
{
    Value*           range[3] = {lo, hi, step};
    Operation* const op = ctx.create_operation(ctx.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const body = ctx.create_block(1U, ctx.type_i32());
    op->region(0)->append(body);
    yield_values(ctx, body, {});
    b->append(op);
}

// @square(%v) { return %v * %v }
// @main(%x) {
//   %t = async.launch { %k = call @square(<launched>); yield %k }        (pooled: 25 for launched = 5)
//   %w = async.await %t
//   %m = task.map_reduce(0, 8, 1, 0) map { iv: for(0, 32, 1) {}; yield iv * iv } combine { acc, e: yield acc + e }
//   %y = %w + %m + %x
//   %s = core.state(0, %y)
//   for(0, 1, %x) {}                                                    (BadForStep when %x <= 0)
//   return %y
// }
Module* build(Context& ctx, i64 launched)
{
    Module* const m = ctx.create_module();
    m->body()->append(ctx.create_block(0U));
    {
        Operation* const fn = func::create_func(ctx, *m, "square", Visibility::Public, 1U, ctx.type_i32());
        m->body()->first_block()->append(fn);
        Block* const fb   = func::func_body_block(fn);
        Value* const v    = fb->arg(0U);
        Value*       r[1] = {bin(ctx, "muli", fb, v, v)};
        fb->append(func::create_return(ctx, ConstSpan<Value*>(r, 1U)));
    }
    Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 1U, ctx.type_i32());
    m->body()->first_block()->append(fn);
    Block* const fb = func::func_body_block(fn);
    Value* const x  = fb->arg(0U);

    Operation* const launch = ctx.create_operation(ctx.intern_op("async", "launch"), {}, 1U, ctx.type_i32(), 1U);
    Block* const     lb     = ctx.create_block(0U);
    launch->region(0)->append(lb);
    Value*           carg[1] = {konst(ctx, lb, launched)};
    Operation* const call    = func::create_call(ctx, "square", ConstSpan<Value*>(carg, 1U), 1U, ctx.type_i32());
    lb->append(call);
    Value* lv[1] = {call->result(0U)};
    yield_values(ctx, lb, ConstSpan<Value*>(lv, 1U));
    fb->append(launch);
    Value*           tok[1] = {launch->result(0U)};
    Operation* const aw =
        ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(tok, 1U), 1U, ctx.type_i32());
    fb->append(aw);

    Value* ops4[4] = {konst(ctx, fb, 0), konst(ctx, fb, 8), konst(ctx, fb, 1), konst(ctx, fb, 0)};
    Operation* const mr =
        ctx.create_operation(ctx.intern_op("task", "map_reduce"), ConstSpan<Value*>(ops4, 4U), 1U, ctx.type_i32(), 2U);
    fb->append(mr);
    Block* const mapb = ctx.create_block(1U, ctx.type_i32());
    mr->region(0)->append(mapb);
    loop(ctx, mapb, konst(ctx, mapb, 0), konst(ctx, mapb, 32), konst(ctx, mapb, 1));
    Value* mv[1] = {bin(ctx, "muli", mapb, mapb->arg(0U), mapb->arg(0U))};
    yield_values(ctx, mapb, ConstSpan<Value*>(mv, 1U));
    Block* const cb = ctx.create_block(2U, ctx.type_i32());
    mr->region(1)->append(cb);
    Value* cv[1] = {bin(ctx, "addi", cb, cb->arg(0U), cb->arg(1U))};
    yield_values(ctx, cb, ConstSpan<Value*>(cv, 1U));

    Value* const y       = bin(ctx, "addi", fb, bin(ctx, "addi", fb, aw->result(0U), mr->result(0U)), x);
    Value*       seed[2] = {konst(ctx, fb, 0), y};
    fb->append(ctx.create_operation(ctx.intern_op("core", "state"), ConstSpan<Value*>(seed, 2U), 1U, ctx.type_i32()));
    loop(ctx, fb, konst(ctx, fb, 0), konst(ctx, fb, 1), x);
    Value* rv[1] = {y};
    fb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    return m;
}

// The 1-based line holding the n-th occurrence of `needle`.
u32 line_of(StringView text, const char* needle, u32 nth)
{
    const usize nlen = StringView(needle).size();
    u32         line = 1U;
    u32         seen = 0U;
    for (usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            continue;
        }
        if (StringView(text.data() + i, nlen) == StringView(needle) && seen++ == nth)
        {
            return line;
        }
    }
    return 0U;
}

// The authored program as text and its cooked blob.
struct Authored
{
    explicit Authored(memory::IAllocator* a) : text(a), blob(a) {}

    String    text;
    Array<u8> blob;
    u32       await_line  = 0U;
    u32       reduce_line = 0U;
    u32       guard_line  = 0U;
};

void author(i64 launched, memory::IAllocator* alloc, Authored& out)
{
    Context builder(alloc);
    register_dialects(builder, nullptr);
    out.text = print(builder, *build(builder, launched), alloc);
    const StringView src(out.text.data(), out.text.size());
    out.await_line  = line_of(src, "async.await", 0U);
    out.reduce_line = line_of(src, "task.map_reduce", 0U);
    out.guard_line  = line_of(src, "core.for", 1U); // 0: the map body's loop
    REQUIRE(out.await_line != 0U);
    REQUIRE(out.reduce_line > out.await_line);
    REQUIRE(out.guard_line > out.reduce_line);

    Context ctx(alloc);
    register_dialects(ctx, nullptr);
    ck::CookResult cr = ck::cook_program_text(ctx, src, StringView(kFile), 1U, alloc, alloc);
    REQUIRE(cr.ok());
    out.blob = std::move(cr.blob);
}

void collect_ids(const Region* r, Array<u64>& out)
{
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.push_back(op->stable_id().value);
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                collect_ids(op->region(i), out);
            }
        }
    }
}

const Operation* first_of(const Context& ctx, const Region* r, OpId kind)
{
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->kind() == kind)
            {
                return op;
            }
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                if (const Operation* const hit = first_of(ctx, op->region(i), kind))
                {
                    return hit;
                }
            }
        }
    }
    return nullptr;
}

// The stable ids the trace refers to: the await, the reduction, and every op inside a body the provider runs on a
// sub-interpreter (the launch body, the map and the combine).
struct Ids
{
    explicit Ids(memory::IAllocator* a) : bodies(a), map(a) {}

    u64        await  = 0U;
    u64        reduce = 0U;
    u64        guard  = 0U;
    Array<u64> bodies;
    Array<u64> map;
};

void ids_of(ConstSpan<u8> blob, memory::IAllocator* alloc, Ids& out)
{
    Context ctx(alloc);
    register_dialects(ctx, nullptr);
    const ck::ReadResult rr = ck::read_program(ctx, blob, alloc);
    REQUIRE(rr.ok());
    ctx.assign_stable_ids(*rr.module);
    const Operation* const launch = first_of(ctx, rr.module->body(), ctx.intern_op("async", "launch"));
    const Operation* const aw     = first_of(ctx, rr.module->body(), ctx.intern_op("async", "await"));
    const Operation* const mr     = first_of(ctx, rr.module->body(), ctx.intern_op("task", "map_reduce"));
    REQUIRE(launch != nullptr);
    REQUIRE(aw != nullptr);
    REQUIRE(mr != nullptr);
    out.await  = aw->stable_id().value;
    out.reduce = mr->stable_id().value;
    collect_ids(launch->region(0), out.bodies);
    collect_ids(mr->region(0), out.map);
    collect_ids(mr->region(0), out.bodies);
    collect_ids(mr->region(1), out.bodies);
    // The guarding loop is the entry's last core.for: the one after the reduction.
    for (const Operation* op = mr->next_in_block(); op != nullptr; op = op->next_in_block())
    {
        if (op->kind() == ctx.intern_op("core", "for"))
        {
            out.guard = op->stable_id().value;
        }
    }
    REQUIRE(out.guard != 0U);
}

bool contains(const Array<u64>& ids, u64 id)
{
    return std::ranges::any_of(ids, [id](u64 v) { return v == id; });
}

const ck::ReplayEvent* event_of(const ck::ReplayRecord& rec, u64 op)
{
    for (const ck::ReplayEvent& ev : rec.events)
    {
        if (ev.op == op)
        {
            return &ev;
        }
    }
    return nullptr;
}

ck::ReplayRecord record(const Authored& a, i64 arg, hs::HostSchedule schedule, memory::IAllocator* alloc)
{
    ck::ReplayRecord rec(alloc);
    const i64        args[1] = {arg};
    String           missing(alloc);
    REQUIRE(hs::record_host_run({a.blob.data(), a.blob.size()}, StringView(kFile), StringView("main"), {args, 1U},
                                schedule, ck::kReplayDefaultMaxEvents, &register_dialects, nullptr, rec,
                                &missing) == hs::HostReplayStatus::Ok);
    CHECK(missing.empty());
    return rec;
}

bool same_events(const ck::ReplayRecord& a, const ck::ReplayRecord& b)
{
    if (a.events.size() != b.events.size() || a.events_total != b.events_total)
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

StringView view(const String& s)
{
    return StringView(s.data(), s.size());
}
} // namespace

TEST_CASE("diag 9a: a host record traces the submitting thread only, the same on every job split", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);
    Ids ids(&alloc);
    ids_of({a.blob.data(), a.blob.size()}, &alloc, ids);

    const ck::ReplayRecord eight = record(a, 1, hs::HostSchedule{8U, u64{1} << 20U}, &alloc);
    CHECK(eight.executor == ck::ReplayExecutorKind::Host);
    CHECK(eight.host_jobs == 8U);
    CHECK(eight.host_sub_fuel == (u64{1} << 20U));
    CHECK(eight.error == plan::RunError::None);
    CHECK(eight.host_error == exec::ExecError::None);
    CHECK(eight.fault_op == 0U);
    REQUIRE(eight.results.size() == 1U);
    CHECK(eight.results[0] == 25 + 140 + 1); // square(5) + sum of iv*iv over 0..7 + x
    REQUIRE(eight.cells.size() == 1U);
    CHECK(eight.cells[0] == 166); // the state cell latched %y

    // The program needs its schedule (launch, await and the reduction synchronize); the host record holds it.
    CHECK(eight.inputs[7].need == ck::ReplayNeed::Yes);
    CHECK(eight.inputs[7].state == ck::ReplayInputState::Recorded);
    for (const u32 i : {0U, 1U, 2U})
    {
        CHECK(eight.inputs[i].state == ck::ReplayInputState::Recorded);
    }
    for (const u32 i : {3U, 4U, 5U, 6U, 8U})
    {
        CHECK(eight.inputs[i].state == ck::ReplayInputState::NotNeeded);
    }

    // What the pooled launch and the parallel reduction produced is read where the submitting thread joins them.
    const ck::ReplayEvent* const aw = event_of(eight, ids.await);
    const ck::ReplayEvent* const mr = event_of(eight, ids.reduce);
    REQUIRE(aw != nullptr);
    REQUIRE(mr != nullptr);
    CHECK(aw->values == 1U);
    CHECK(aw->value[0] == 25);
    CHECK(mr->values == 1U);
    CHECK(mr->value[0] == 140);
    CHECK(aw->depth == 0U);

    // No op of a body run on a sub-interpreter is ever an event; the called function's ops run at depth 1 only
    // inside the launch body, so no event is deeper than the entry.
    REQUIRE(ids.bodies.size() > 6U);
    for (const ck::ReplayEvent& ev : eight.events)
    {
        CHECK_FALSE(contains(ids.bodies, ev.op));
        CHECK(ev.depth == 0U);
    }
    CHECK(eight.events_total == eight.events.size());

    // The job split changes nothing the record holds but the split itself.
    for (const u32 jobs : {1U, 2U, 3U, 16U})
    {
        const ck::ReplayRecord other = record(a, 1, hs::HostSchedule{jobs, u64{1} << 20U}, &alloc);
        CHECK(other.host_jobs == jobs);
        CHECK(same_events(eight, other));
        CHECK(other.results[0] == eight.results[0]);
        CHECK(other.cells[0] == eight.cells[0]);
    }
}

TEST_CASE("diag 9a: a host record replays in a fresh Context from its bytes, its failure at the authored line",
          "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);
    Ids ids(&alloc);
    ids_of({a.blob.data(), a.blob.size()}, &alloc, ids);

    for (const i64 arg : {i64{1}, i64{0}})
    {
        const ck::ReplayRecord rec = record(a, arg, hs::HostSchedule{8U, u64{1} << 20U}, &alloc);
        Array<u8>              bytes(&alloc);
        ck::encode_record(rec, bytes);
        ck::ReplayRecord back(&alloc);
        REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Ok);
        CHECK(back.executor == ck::ReplayExecutorKind::Host);
        CHECK(back.host_jobs == 8U);
        CHECK(back.host_sub_fuel == (u64{1} << 20U));
        CHECK(back.host_error == rec.host_error);
        CHECK(same_events(rec, back));
        Array<u8> again(&alloc);
        ck::encode_record(back, again);
        CHECK(again.size() == bytes.size());
        CHECK(std::equal(bytes.begin(), bytes.end(), again.begin()));

        hs::HostReplay rp(&alloc);
        REQUIRE(hs::replay_host_record(back, &register_dialects, nullptr, {}, rp) == hs::HostReplayStatus::Ok);
        CHECK(rp.divergence.kind == ck::DivergenceKind::None);
        CHECK(rp.num_jobs == 8U);
        CHECK(rp.replayed_hash == back.content_hash);
        CHECK_FALSE(rp.build_differs);

        // Another job split, asked for explicitly, reproduces it as well.
        hs::HostReplayOptions one;
        one.num_jobs = 1U;
        hs::HostReplay rp1(&alloc);
        REQUIRE(hs::replay_host_record(back, &register_dialects, nullptr, one, rp1) == hs::HostReplayStatus::Ok);
        CHECK(rp1.divergence.kind == ck::DivergenceKind::None);
        CHECK(rp1.num_jobs == 1U);

        if (arg == 0)
        {
            CHECK(rec.host_error == exec::ExecError::BadForStep);
            CHECK(rec.fault_op == ids.guard);
            CHECK(rp.trace.host_error == exec::ExecError::BadForStep);
            CHECK(rp.fault.op == ids.guard);
            CHECK(view(rp.fault.file) == StringView(kFile));
            CHECK(rp.fault.line == a.guard_line);
            CHECK(rp.fault.col > 0U);
        }
        else
        {
            CHECK(rec.host_error == exec::ExecError::None);
            CHECK(rp.fault.op == 0U);
        }
    }
}

TEST_CASE("diag 9a: a host replay runs the recorded step budget and names an edit at the op that reads it",
          "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);
    Ids ids(&alloc);
    ids_of({a.blob.data(), a.blob.size()}, &alloc, ids);

    SECTION("a budget too small for the map body's loop fails there; the replay fails the same way")
    {
        const ck::ReplayRecord rec = record(a, 1, hs::HostSchedule{4U, 16U}, &alloc);
        CHECK(rec.host_sub_fuel == 16U);
        CHECK(rec.host_error == exec::ExecError::FuelExhausted);
        CHECK(contains(ids.map, rec.fault_op)); // blamed inside the map body, on a sub-interpreter
        CHECK(rec.results.empty());

        hs::HostReplay rp(&alloc);
        REQUIRE(hs::replay_host_record(rec, &register_dialects, nullptr, {}, rp) == hs::HostReplayStatus::Ok);
        CHECK(rp.divergence.kind == ck::DivergenceKind::None);
        CHECK(rp.trace.host_error == exec::ExecError::FuelExhausted);
        CHECK(rp.fault.op == rec.fault_op);
        CHECK(rp.fault.line > a.reduce_line);
        CHECK(rp.fault.line < a.guard_line);

        // The same record with the default budget would have finished: the budget is what the replay reproduces.
        const ck::ReplayRecord full = record(a, 1, hs::HostSchedule{4U, u64{1} << 20U}, &alloc);
        CHECK(full.host_error == exec::ExecError::None);
    }
    SECTION("the same inputs against an edited launch body diverge at the await, at its authored line")
    {
        Authored edited(&alloc);
        author(6, &alloc, edited);
        REQUIRE(edited.await_line == a.await_line);

        const ck::ReplayRecord rec = record(a, 1, hs::HostSchedule{8U, u64{1} << 20U}, &alloc);
        hs::HostReplayOptions  against;
        against.against = {edited.blob.data(), edited.blob.size()};
        hs::HostReplay rp(&alloc);
        REQUIRE(hs::replay_host_record(rec, &register_dialects, nullptr, against, rp) == hs::HostReplayStatus::Ok);
        CHECK(rp.replayed_hash != rec.content_hash);
        CHECK(rp.divergence.kind == ck::DivergenceKind::Value);
        CHECK(rp.divergence.recorded_op == ids.await);
        CHECK(rp.divergence.recorded == 25);
        CHECK(rp.divergence.observed == 36);
        CHECK(rp.site.op == ids.await);
        CHECK(view(rp.site.file) == StringView(kFile));
        CHECK(rp.site.line == a.await_line);

        // Without the edit, the record's own program reproduces it.
        hs::HostReplay same(&alloc);
        REQUIRE(hs::replay_host_record(rec, &register_dialects, nullptr, {}, same) == hs::HostReplayStatus::Ok);
        CHECK(same.divergence.kind == ck::DivergenceKind::None);
    }
}

TEST_CASE("diag 9a: an incompatible host record is refused before anything runs", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);
    const ck::ReplayRecord original = record(a, 1, hs::HostSchedule{8U, u64{1} << 20U}, &alloc);

    const auto refused = [&alloc](const ck::ReplayRecord& rec, const hs::HostReplayOptions& options,
                                  hs::HostReplayStatus want, const char* reason)
    {
        hs::HostReplay rp(&alloc);
        CHECK(hs::replay_host_record(rec, &register_dialects, nullptr, options, rp) == want);
        CHECK(rp.trace.events_total == 0U); // nothing ran
        CHECK(view(rp.reason) == StringView(reason));
    };
    const auto copy = [&alloc, &original]()
    {
        Array<u8> bytes(&alloc);
        ck::encode_record(original, bytes);
        ck::ReplayRecord rec(&alloc);
        REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, rec) == ck::RecordError::Ok);
        return rec;
    };

    SECTION("a plan record is the cook executor's")
    {
        ck::ReplayRecord rec = copy();
        rec.executor         = ck::ReplayExecutorKind::Plan;
        refused(rec, {}, hs::HostReplayStatus::WrongExecutor, "plan");
        CHECK(hs::host_replay_status_name(hs::HostReplayStatus::WrongExecutor) == "wrong-executor");
    }
    SECTION("another build is refused naming the field, unless the request allows it")
    {
        ck::ReplayRecord rec = copy();
        rec.build.config.clear();
        rec.build.config.append("other-config");
        refused(rec, {}, hs::HostReplayStatus::OtherBuild, "config");
        hs::HostReplayOptions any;
        any.any_build = true;
        hs::HostReplay rp(&alloc);
        REQUIRE(hs::replay_host_record(rec, &register_dialects, nullptr, any, rp) == hs::HostReplayStatus::Ok);
        CHECK(rp.build_differs);
        CHECK(rp.divergence.kind == ck::DivergenceKind::None);
    }
    SECTION("a missing input is named")
    {
        ck::ReplayRecord rec = copy();
        rec.inputs[3]        = {ck::ReplayNeed::Yes, ck::ReplayInputState::Missing};
        refused(rec, {}, hs::HostReplayStatus::MissingInputs, "random");
    }
    SECTION("a blob that is not the recorded content, or does not read")
    {
        ck::ReplayRecord rec = copy();
        rec.content_hash ^= 1U;
        refused(rec, {}, hs::HostReplayStatus::ContentMismatch, "");
        ck::ReplayRecord cut = copy();
        cut.program.resize(cut.program.size() / 2U);
        refused(cut, {}, hs::HostReplayStatus::NotLoaded, "");
    }
    SECTION("a job split past the bound")
    {
        hs::HostReplayOptions wide;
        wide.num_jobs = ck::kReplayMaxHostJobs + 1U;
        refused(original, wide, hs::HostReplayStatus::BadSchedule, "");
        ck::ReplayRecord rec(&alloc);
        const i64        args[1] = {1};
        CHECK(hs::record_host_run({a.blob.data(), a.blob.size()}, StringView(kFile), StringView("main"), {args, 1U},
                                  hs::HostSchedule{0U, 16U}, 64U, &register_dialects, nullptr, rec,
                                  nullptr) == hs::HostReplayStatus::BadSchedule);
        CHECK(hs::record_host_run({a.blob.data(), a.blob.size()}, StringView(kFile), StringView("main"), {args, 1U},
                                  hs::HostSchedule{2U, 0U}, 64U, &register_dialects, nullptr, rec,
                                  nullptr) == hs::HostReplayStatus::BadSchedule);
    }
    SECTION("the record format keeps each executor's fields apart")
    {
        Array<u8>        bytes(&alloc);
        ck::ReplayRecord back(&alloc);
        ck::ReplayRecord rec = copy();
        rec.host_jobs        = 0U;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec           = copy();
        rec.host_jobs = ck::kReplayMaxHostJobs + 1U;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec               = copy();
        rec.host_sub_fuel = 0U;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec       = copy();
        rec.error = plan::RunError::BadForStep; // a plan error in a host record
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec          = copy();
        rec.executor = ck::ReplayExecutorKind::Plan; // a plan record carrying a schedule
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec               = copy();
        rec.executor      = ck::ReplayExecutorKind::Plan;
        rec.host_jobs     = 0U;
        rec.host_sub_fuel = 0U;
        rec.host_error    = exec::ExecError::BadForStep; // a host error in a plan record
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Malformed);
        rec.host_error = exec::ExecError::None;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Ok);
        rec        = copy();
        rec.schema = 2U;
        ck::encode_record(rec, bytes);
        CHECK(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::UnsupportedSchema);
        CHECK(ck::replay_executor_name(ck::ReplayExecutorKind::Host) == "host");
    }
}
