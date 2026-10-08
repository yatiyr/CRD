// Run records of host-provider runs made while an inspection session drives them. The program pools an async.launch
// whose body calls a function (a detached body: a breakpoint there is counted and never pauses), calls the same
// function on the submitting thread, awaits the launch, folds a task.map_reduce, keeps a state cell and ends in a loop
// whose step is the entry argument (main(0) faults there). A controller stops the run at a breakpoint, steps into the
// call and out again, reads values at the stops and lets it finish; the record must be the one an unobserved run of
// the same program makes, byte for byte, and must replay from its file in a fresh Context (the fault at its authored
// line; an edited launch constant named at the await that reads it). A run the session cancels is refused and leaves
// the record untouched. Expected lines come from scanning the printed text, never from the parser or the session.
// The test thread is the pool's thread 0 and executes the program; a second thread is the controller. A controller
// that finds the run still going after its script cancels it (the backstop) and records that, so a broken path fails
// instead of hanging. The jobs pool is owned by the listener in test_host_provider.cpp (same binary). ASCII test names.

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
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/print.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <thread>
#include <utility>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
namespace ck   = crd::ceir::cook;
namespace hs   = crd::ceir::host;
namespace insp = crd::ceir::inspect;

namespace
{
constexpr const char* kFile   = "programs/diag/host_session_record.ceir";
constexpr u32         kWaitMs = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
constexpr u64         kGen    = 1U;

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

Value* call_square(Context& ctx, Block* b, Value* v)
{
    Value*           arg[1] = {v};
    Operation* const call   = func::create_call(ctx, "square", ConstSpan<Value*>(arg, 1U), 1U, ctx.type_i32());
    b->append(call);
    return call->result(0U);
}

// @square(%v) { return %v * %v }
// @main(%x) {
//   %t = async.launch { %k = call @square(<launched>); yield %k }        pooled (25 for launched = 5): detached
//   %c = call @square(%x)                                                on the submitting thread: step in and out
//   %w = async.await %t
//   %m = task.map_reduce(0, 8, 1, 0) map { iv: yield iv * iv } combine { acc, e: yield acc + e }       140
//   %y = %w + %m + %c
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
    Value* lv[1] = {call_square(ctx, lb, konst(ctx, lb, launched))};
    yield_values(ctx, lb, ConstSpan<Value*>(lv, 1U));
    fb->append(launch);
    Value* const c = call_square(ctx, fb, x);

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
    Value* mv[1] = {bin(ctx, "muli", mapb, mapb->arg(0U), mapb->arg(0U))};
    yield_values(ctx, mapb, ConstSpan<Value*>(mv, 1U));
    Block* const cb = ctx.create_block(2U, ctx.type_i32());
    mr->region(1)->append(cb);
    Value* cv[1] = {bin(ctx, "addi", cb, cb->arg(0U), cb->arg(1U))};
    yield_values(ctx, cb, ConstSpan<Value*>(cv, 1U));

    Value* const y       = bin(ctx, "addi", fb, bin(ctx, "addi", fb, aw->result(0U), mr->result(0U)), c);
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

// The authored program as text, its cooked blob and the scanned lines the test stops at or expects.
struct Authored
{
    explicit Authored(memory::IAllocator* a) : text(a), blob(a) {}

    String    text;
    Array<u8> blob;
    u32       body_call_line = 0U; // the call inside the pooled launch body (detached)
    u32       call_line      = 0U; // the call on the submitting thread
    u32       await_line     = 0U;
    u32       state_line     = 0U;
    u32       guard_line     = 0U;
};

void author(i64 launched, memory::IAllocator* alloc, Authored& out)
{
    Context builder(alloc);
    register_dialects(builder, nullptr);
    out.text = print(builder, *build(builder, launched), alloc);
    const StringView src(out.text.data(), out.text.size());
    out.body_call_line = line_of(src, "func.call", 0U);
    out.call_line      = line_of(src, "func.call", 1U);
    out.await_line     = line_of(src, "async.await", 0U);
    out.state_line     = line_of(src, "core.state", 0U);
    out.guard_line     = line_of(src, "core.for", 0U);
    REQUIRE(out.body_call_line != 0U);
    REQUIRE(out.call_line > out.body_call_line);
    REQUIRE(out.await_line > out.call_line);
    REQUIRE(out.state_line > out.await_line);
    REQUIRE(out.guard_line > out.state_line);

    Context ctx(alloc);
    register_dialects(ctx, nullptr);
    ck::CookResult cr = ck::cook_program_text(ctx, src, StringView(kFile), 1U, alloc, alloc);
    REQUIRE(cr.ok());
    out.blob = std::move(cr.blob);
}

void collect(const Region* r, Array<const Operation*>& out)
{
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.push_back(op);
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                collect(op->region(i), out);
            }
        }
    }
}

// The stable ids of the loaded program the test names, found by kind in pre-order.
struct Ids
{
    explicit Ids(memory::IAllocator* a) : bodies(a) {}

    u64        square_mul = 0U; // @square's muli (pre-order first muli)
    u64        call       = 0U; // the submitting thread's call (the second func.call)
    u64        await      = 0U;
    u64        reduce     = 0U;
    u64        state      = 0U;
    u64        guard      = 0U; // the entry's core.for
    Array<u64> bodies;          // every op of the launch body, the map and the combine
};

void ids_of(Context& ctx, const Module& m, memory::IAllocator* alloc, Ids& out)
{
    Array<const Operation*> ops(alloc);
    collect(m.body(), ops);
    const OpId muli   = ctx.intern_op("arith", "muli");
    const OpId call   = ctx.intern_op("func", "call");
    const OpId launch = ctx.intern_op("async", "launch");
    const OpId mr     = ctx.intern_op("task", "map_reduce");
    u32        calls  = 0U;
    for (const Operation* op : ops)
    {
        const u64 id = op->stable_id().value;
        if (op->kind() == muli && out.square_mul == 0U)
        {
            out.square_mul = id;
        }
        else if (op->kind() == call && calls++ == 1U)
        {
            out.call = id;
        }
        else if (op->kind() == ctx.intern_op("async", "await"))
        {
            out.await = id;
        }
        else if (op->kind() == mr)
        {
            out.reduce = id;
        }
        else if (op->kind() == ctx.intern_op("core", "state"))
        {
            out.state = id;
        }
        if (op->kind() == launch || op->kind() == mr)
        {
            Array<const Operation*> inner(alloc);
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                collect(op->region(i), inner);
            }
            for (const Operation* in : inner)
            {
                out.bodies.push_back(in->stable_id().value);
            }
        }
    }
    // The guarding loop is the last core.for outside the bodies.
    for (const Operation* op : ops)
    {
        if (op->kind() == ctx.intern_op("core", "for"))
        {
            out.guard = op->stable_id().value;
        }
    }
    REQUIRE(out.square_mul != 0U);
    REQUIRE(out.call != 0U);
    REQUIRE(out.await != 0U);
    REQUIRE(out.reduce != 0U);
    REQUIRE(out.state != 0U);
    REQUIRE(out.guard != 0U);
}

bool contains(const Array<u64>& ids, u64 id)
{
    return std::ranges::any_of(ids, [id](u64 v) { return v == id; });
}

StringView view(const String& s)
{
    return StringView(s.data(), s.size());
}

Array<u8> encoded(const ck::ReplayRecord& rec, memory::IAllocator* alloc)
{
    Array<u8> bytes(alloc);
    ck::encode_record(rec, bytes);
    return bytes;
}

bool same_bytes(const Array<u8>& a, const Array<u8>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// The record an unobserved run of the same artifact makes.
ck::ReplayRecord unobserved(const Authored& a, i64 arg, memory::IAllocator* alloc)
{
    ck::ReplayRecord rec(alloc);
    const i64        args[1] = {arg};
    REQUIRE(hs::record_host_run({a.blob.data(), a.blob.size()}, StringView(kFile), StringView("main"), {args, 1U},
                                hs::HostSchedule{4U, u64{1} << 20U}, ck::kReplayDefaultMaxEvents, &register_dialects,
                                nullptr, rec, nullptr) == hs::HostReplayStatus::Ok);
    return rec;
}

Array<u8> slurp_bytes(const char* path, memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    Array<u8> out(a);
    out.resize(static_cast<usize>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    REQUIRE(f.good());
    return out;
}

void forget(const char* path)
{
    (void)std::remove(path);
}

bool wait_flag(const std::atomic<bool>& flag, u32 timeout_ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load())
    {
        if (std::chrono::steady_clock::now() > until)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Wait until the session reports the execution finished (a stop that is still held is not an end).
insp::Refusal wait_finished(insp::Session& s)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    for (;;)
    {
        insp::StopRecord    ignored;
        const insp::Refusal r = s.wait_for_stop(kGen, 50U, ignored);
        if (r == insp::Refusal::Finished || std::chrono::steady_clock::now() > until)
        {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The controller thread. `script` drives the session; afterwards the run must return by itself within kWaitMs. One
// that does not is cancelled through the session (its cancel raises the provider's flag) until it returns, and
// `backstop()` reports it.
class Controller
{
public:
    template <typename F>
    Controller(insp::Session& s, F&& script) : m_session(s)
    {
        m_thread = std::thread(
            [this, f = std::forward<F>(script)]() mutable
            {
                f();
                settle();
            });
    }
    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;
    Controller(Controller&&)                 = delete;
    Controller& operator=(Controller&&)      = delete;
    ~Controller()
    {
        returned();
    }
    // The test thread: the run returned. Joins the controller.
    void returned()
    {
        m_returned.store(true);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
    [[nodiscard]] bool backstop() const noexcept { return m_backstop.load(); }

private:
    void settle()
    {
        if (wait_flag(m_returned, kWaitMs))
        {
            return;
        }
        m_backstop.store(true);
        while (!m_returned.load())
        {
            (void)m_session.cancel(kGen);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    insp::Session&    m_session;
    std::atomic<bool> m_returned{false};
    std::atomic<bool> m_backstop{false};
    std::thread       m_thread;
};

// One stop as the controller saw it, plus one value read made while it was held.
struct Seen
{
    insp::Refusal       wait = insp::Refusal::Timeout;
    insp::StopRecord    stop;
    insp::ValueSnapshot value;
    explicit Seen(memory::IAllocator* a) : value(a) {}
};

void read_into(insp::Session& s, u64 op, insp::ValueSnapshot& out)
{
    insp::ValueRef ref;
    ref.op = StableId{op};
    (void)s.snapshot(kGen, ref, out, kWaitMs);
}

// Breakpoints on the detached launch body's call, the submitting call and the state cell, bound to `program`.
void bind(insp::Session& s, hs::HostProgram& program, const Authored& a, memory::IAllocator* alloc, u32 (&bps)[3])
{
    const u32 lines[3] = {a.body_call_line, a.call_line, a.state_line};
    for (u32 i = 0U; i < 3U; ++i)
    {
        REQUIRE(s.add_line_breakpoint(kFile, lines[i], bps[i]) == insp::Refusal::None);
    }
    Array<insp::BindReport> rep(alloc);
    REQUIRE(s.bind(*program.module(), program.context(), kGen, rep) == insp::Refusal::None);
    REQUIRE(rep.size() == 3U);
    for (const insp::BindReport& r : rep)
    {
        CHECK(r.status == insp::BindStatus::Bound);
    }
}
} // namespace

TEST_CASE("diag 9a: a host run stopped, stepped and read under a session records what an unobserved run records",
          "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    memory::GrowableTlsfAllocator sess_alloc;
    memory::GrowableTlsfAllocator ctl_alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);

    for (const i64 arg : {i64{3}, i64{0}})
    {
        hs::HostProgram program(&alloc, {a.blob.data(), a.blob.size()}, StringView(kFile), &register_dialects, nullptr);
        REQUIRE(program.status() == hs::HostReplayStatus::Ok);
        Ids ids(&alloc);
        ids_of(program.context(), *program.module(), &alloc, ids);

        insp::Session s(&sess_alloc);
        u32           bps[3] = {};
        bind(s, program, a, &ctl_alloc, bps);

        Seen          at_call(&ctl_alloc);
        Seen          inside(&ctl_alloc);
        Seen          back(&ctl_alloc);
        Seen          at_state(&ctl_alloc);
        insp::Refusal end    = insp::Refusal::None;
        const auto    script = [&]
        {
            at_call.wait = s.wait_for_stop(kGen, kWaitMs, at_call.stop);
            read_into(s, ids.call, at_call.value);
            (void)s.resume(kGen, insp::Resume::StepInto);
            inside.wait = s.wait_for_stop(kGen, kWaitMs, inside.stop);
            (void)s.resume(kGen, insp::Resume::StepOut);
            back.wait = s.wait_for_stop(kGen, kWaitMs, back.stop);
            read_into(s, ids.call, back.value);
            (void)s.resume(kGen, insp::Resume::Continue);
            at_state.wait = s.wait_for_stop(kGen, kWaitMs, at_state.stop);
            read_into(s, ids.reduce, at_state.value);
            (void)s.resume(kGen, insp::Resume::Continue);
            end = wait_finished(s);
        };

        ck::ReplayRecord     rec(&alloc);
        String               missing(&alloc);
        hs::HostSite         fault(&alloc);
        const i64            args[1] = {arg};
        hs::HostReplayStatus status  = hs::HostReplayStatus::NotLoaded;
        {
            Controller ctl(s, script);
            status = hs::record_host_run(program, StringView("main"), {args, 1U}, hs::HostSchedule{4U, u64{1} << 20U},
                                         ck::kReplayDefaultMaxEvents, &s, rec, &missing, &fault);
            ctl.returned();
            CHECK_FALSE(ctl.backstop());
        }
        REQUIRE(status == hs::HostReplayStatus::Ok);
        CHECK(missing.empty());

        // The controller stopped at the submitting call with the launch still pending, stepped into @square and
        // out to the await, read the call's value there and stopped again at the state cell.
        REQUIRE(at_call.wait == insp::Refusal::None);
        CHECK(at_call.stop.reason == insp::StopReason::Breakpoint);
        CHECK(at_call.stop.breakpoint == bps[1]);
        CHECK(at_call.stop.op.value == ids.call);
        CHECK(at_call.stop.depth == 0U);
        CHECK(at_call.value.status == insp::ValueStatus::NotYetComputed);
        REQUIRE(inside.wait == insp::Refusal::None);
        CHECK(inside.stop.reason == insp::StopReason::Step);
        CHECK(inside.stop.op.value == ids.square_mul);
        CHECK(inside.stop.depth == 1U);
        REQUIRE(back.wait == insp::Refusal::None);
        CHECK(back.stop.reason == insp::StopReason::Step);
        CHECK(back.stop.op.value == ids.await);
        CHECK(back.stop.depth == 0U);
        CHECK(back.value.status == insp::ValueStatus::Available);
        CHECK(back.value.bits == arg * arg);
        REQUIRE(at_state.wait == insp::Refusal::None);
        CHECK(at_state.stop.breakpoint == bps[2]);
        CHECK(at_state.stop.op.value == ids.state);
        CHECK(at_state.value.status == insp::ValueStatus::Available);
        CHECK(at_state.value.bits == 140);
        CHECK(end == insp::Refusal::Finished);
        // The breakpoint in the pooled launch body was hit on a pool worker and refused, never paused.
        CHECK(s.detached_hits() == 1U);

        // The record is the unobserved run's, byte for byte: the stops, the steps and the reads left no mark.
        const ck::ReplayRecord plain = unobserved(a, arg, &alloc);
        CHECK(same_bytes(encoded(rec, &alloc), encoded(plain, &alloc)));
        CHECK(rec.executor == ck::ReplayExecutorKind::Host);
        CHECK(rec.events_total == rec.events.size());
        bool stepped = false;
        for (const ck::ReplayEvent& ev : rec.events)
        {
            CHECK_FALSE(contains(ids.bodies, ev.op)); // a detached body leaves no event
            stepped =
                stepped || (ev.op == ids.square_mul && ev.depth == 1U && ev.values == 1U && ev.value[0] == arg * arg);
        }
        CHECK(stepped); // the frame the controller stepped through is traced with its value

        // Written, read back and replayed in a fresh Context, it reproduces; main(0)'s fault is at its line.
        const char* const path = arg == 0 ? "diag9a_host_session_fail.crpl" : "diag9a_host_session_ok.crpl";
        forget(path);
        REQUIRE(ck::write_record_file(StringView{path}, rec) == ck::RecordWrite::Ok);
        const Array<u8> bytes = slurp_bytes(path, &alloc);
        forget(path);
        ck::ReplayRecord from_file(&alloc);
        REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, from_file) == ck::RecordError::Ok);
        hs::HostReplay rp(&alloc);
        REQUIRE(hs::replay_host_record(from_file, &register_dialects, nullptr, {}, rp) == hs::HostReplayStatus::Ok);
        CHECK(rp.divergence.kind == ck::DivergenceKind::None);
        if (arg == 0)
        {
            CHECK(rec.host_error == exec::ExecError::BadForStep);
            CHECK(rec.fault_op == ids.guard);
            CHECK(fault.op == ids.guard);
            CHECK(view(fault.file) == StringView(kFile));
            CHECK(fault.line == a.guard_line);
            CHECK(rp.fault.line == a.guard_line);
            CHECK(view(rp.recorded_fault.file) == StringView(kFile));
            CHECK(rp.recorded_fault.line == a.guard_line);
        }
        else
        {
            CHECK(rec.host_error == exec::ExecError::None);
            REQUIRE(rec.results.size() == 1U);
            CHECK(rec.results[0] == 25 + 140 + 9);
            REQUIRE(rec.cells.size() == 1U);
            CHECK(rec.cells[0] == 174);

            // The same inputs against an edited launch body diverge at the await that reads it, at its line.
            Authored edited(&alloc);
            author(6, &alloc, edited);
            REQUIRE(edited.await_line == a.await_line);
            hs::HostReplayOptions against;
            against.against = {edited.blob.data(), edited.blob.size()};
            hs::HostReplay other(&alloc);
            REQUIRE(hs::replay_host_record(from_file, &register_dialects, nullptr, against, other) ==
                    hs::HostReplayStatus::Ok);
            CHECK(other.divergence.kind == ck::DivergenceKind::Value);
            CHECK(other.divergence.recorded_op == ids.await);
            CHECK(other.divergence.recorded == 25);
            CHECK(other.divergence.observed == 36);
            CHECK(other.site.line == a.await_line);
        }
    }
}

TEST_CASE("diag 9a: a host run the session cancels is not recorded", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    memory::GrowableTlsfAllocator sess_alloc;
    memory::GrowableTlsfAllocator ctl_alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);
    hs::HostProgram program(&alloc, {a.blob.data(), a.blob.size()}, StringView(kFile), &register_dialects, nullptr);
    REQUIRE(program.status() == hs::HostReplayStatus::Ok);

    insp::Session s(&sess_alloc);
    u32           bps[3] = {};
    bind(s, program, a, &ctl_alloc, bps);
    Seen          at_call(&ctl_alloc);
    insp::Refusal cancelled = insp::Refusal::Timeout;
    const auto    script    = [&]
    {
        at_call.wait = s.wait_for_stop(kGen, kWaitMs, at_call.stop);
        cancelled    = s.cancel(kGen);
    };

    ck::ReplayRecord rec(&alloc);
    rec.entry.append("untouched");
    String               missing(&alloc);
    const i64            args[1] = {3};
    hs::HostReplayStatus status  = hs::HostReplayStatus::Ok;
    {
        Controller ctl(s, script);
        status = hs::record_host_run(program, StringView("main"), {args, 1U}, hs::HostSchedule{4U, u64{1} << 20U},
                                     ck::kReplayDefaultMaxEvents, &s, rec, &missing);
        ctl.returned();
        CHECK_FALSE(ctl.backstop());
    }
    REQUIRE(at_call.wait == insp::Refusal::None);
    CHECK(at_call.stop.breakpoint == bps[1]);
    CHECK(cancelled == insp::Refusal::None);
    CHECK(status == hs::HostReplayStatus::Cancelled);
    CHECK(hs::host_replay_status_name(status) == "cancelled");
    // Nothing of the cancelled run reached the record or the missing list.
    CHECK(view(rec.entry) == "untouched");
    CHECK(rec.program.empty());
    CHECK(rec.events.empty());
    CHECK(rec.events_total == 0U);
    CHECK(rec.executor == ck::ReplayExecutorKind::Plan);
    CHECK(missing.empty());

    // The same loaded program records again, unobserved, as the blob form does.
    ck::ReplayRecord again(&alloc);
    REQUIRE(hs::record_host_run(program, StringView("main"), {args, 1U}, hs::HostSchedule{4U, u64{1} << 20U},
                                ck::kReplayDefaultMaxEvents, nullptr, again, nullptr) == hs::HostReplayStatus::Ok);
    CHECK(same_bytes(encoded(again, &alloc), encoded(unobserved(a, 3, &alloc), &alloc)));
}

TEST_CASE("diag 9a: a host program that does not load records nothing", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Authored                      a(&alloc);
    author(5, &alloc, a);
    const i64 args[1] = {3};

    String long_path(&alloc);
    for (u32 i = 0U; i <= ck::kReplayMaxStringBytes; ++i)
    {
        long_path.push_back('p');
    }
    hs::HostProgram too_long(&alloc, {a.blob.data(), a.blob.size()}, view(long_path), &register_dialects, nullptr);
    CHECK(too_long.status() == hs::HostReplayStatus::BadSchedule);
    CHECK(too_long.module() == nullptr);

    hs::HostProgram cut(&alloc, {a.blob.data(), a.blob.size() / 2U}, StringView(kFile), &register_dialects, nullptr);
    CHECK(cut.status() == hs::HostReplayStatus::NotLoaded);
    CHECK(cut.module() == nullptr);

    for (hs::HostProgram* p : {&too_long, &cut})
    {
        ck::ReplayRecord rec(&alloc);
        CHECK(hs::record_host_run(*p, StringView("main"), {args, 1U}, hs::HostSchedule{4U, u64{1} << 20U},
                                  ck::kReplayDefaultMaxEvents, nullptr, rec, nullptr) == p->status());
        CHECK(rec.program.empty());
        CHECK(rec.events_total == 0U);
    }

    // A loaded program still refuses a schedule out of range before running anything.
    hs::HostProgram  good(&alloc, {a.blob.data(), a.blob.size()}, StringView(kFile), &register_dialects, nullptr);
    ck::ReplayRecord rec(&alloc);
    CHECK(hs::record_host_run(good, StringView("main"), {args, 1U}, hs::HostSchedule{0U, 16U}, 64U, nullptr, rec,
                              nullptr) == hs::HostReplayStatus::BadSchedule);
    CHECK(rec.events_total == 0U);
}
