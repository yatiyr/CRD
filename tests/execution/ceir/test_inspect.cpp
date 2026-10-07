// DIAG.8b — runtime INSPECTION (inspect.hpp): line breakpoints, stepping, safe stop and typed value snapshots on the
// two existing executors. One authored CEIR text program is parsed under a registered file, optimized by CSE through
// the PassManager, serialized and loaded into a fresh Context, then compiled. The program runs on a SECOND thread
// (the executing thread) while this test thread is the controller: it waits for the stop, reads values through the
// paused executor, steps, resumes and cancels. Expected lines come from scanning the text itself, never from the
// parser or the session. Controls: a stale or unbound generation is refused before any work, a non-pausable session
// and a safe point on the controller's own thread never pause, a whole-host pause runs the host's freeze/thaw, and a
// redacted value keeps its type but not its bits. ASCII test names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/cse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
namespace insp = crd::ceir::inspect;

namespace
{
constexpr const char* kFile    = "programs/diag/inspect_main.ceir";
constexpr u32         kWaitMs  = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
constexpr u64         kGen     = 1U;

void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)func::register_dialect(ctx);
}

Operation* konst(Context& ctx, Block* b, i64 v, TypeId type)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, type);
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
}

Operation* binop(Context& ctx, Block* b, const char* name, Value* x, Value* y)
{
    Value*           in[2] = {x, y};
    Operation* const op =
        ctx.create_operation(ctx.intern_op("arith", name), ConstSpan<Value*>(in, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op;
}

// @helper(%p) { %h = muli(%p, %p); return %h }
// @main(%n) { %zero = 0; %one = 1; %two = 2; %len = 7 : qty<i32,L>; %a = addi(%one,%two); %b = addi(%one,%two);
//             %s = addi(%a,%b); %t = call @helper(%s); core.for(%zero, %n, %one) { %x = addi(%iv,%s); yield };
//             return %t }
// %b duplicates %a, so CSE keeps %a as the survivor of both authored addi lines. main(n) returns 36 for any n >= 0.
Module* build_program(Context& ctx)
{
    Module* const m = ctx.create_module();
    m->body()->append(ctx.create_block(0U));

    Operation* const helper = func::create_func(ctx, *m, "helper", Visibility::Public, 1U, ctx.type_i32());
    m->body()->first_block()->append(helper);
    Block* const     hb = func::func_body_block(helper);
    Operation* const h  = binop(ctx, hb, "muli", hb->arg(0U), hb->arg(0U));
    Value*           hr[1] = {h->result(0U)};
    hb->append(func::create_return(ctx, ConstSpan<Value*>(hr, 1U)));

    Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 1U, ctx.type_i32());
    m->body()->first_block()->append(fn);
    Block* const      body = func::func_body_block(fn);
    Operation* const  zero = konst(ctx, body, 0, ctx.type_i32());
    Operation* const  one  = konst(ctx, body, 1, ctx.type_i32());
    Operation* const  two  = konst(ctx, body, 2, ctx.type_i32());
    QuantityDim       length;
    length.exp[0] = 1;
    (void)konst(ctx, body, 7, ctx.type_quantity(ctx.type_i32(), length));
    Operation* const a = binop(ctx, body, "addi", one->result(0U), two->result(0U));
    Operation* const b = binop(ctx, body, "addi", one->result(0U), two->result(0U));
    Operation* const s = binop(ctx, body, "addi", a->result(0U), b->result(0U));
    Value*           args[1] = {s->result(0U)};
    Operation* const t = func::create_call(ctx, "helper", ConstSpan<Value*>(args, 1U), 1U, ctx.type_i32());
    body->append(t);
    Value*           range[3] = {zero->result(0U), body->arg(0U), one->result(0U)};
    Operation* const loop =
        ctx.create_operation(ctx.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const loop_body = ctx.create_block(1U, ctx.type_i32());
    loop->region(0)->append(loop_body);
    (void)binop(ctx, loop_body, "addi", loop_body->arg(0U), s->result(0U));
    loop_body->append(ctx.create_operation(ctx.intern_op("core", "yield"), {}, 0U));
    body->append(loop);
    Value* rv[1] = {t->result(0U)};
    body->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    return m;
}

// The 1-based line holding the n-th occurrence of `needle` in the authored text (the independent oracle).
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

bool contains(const String& s, StringView n)
{
    const StringView hay(s.data(), s.size());
    for (usize i = 0; i + n.size() <= hay.size(); ++i)
    {
        if (StringView(hay.data() + i, n.size()) == n)
        {
            return true;
        }
    }
    return false;
}

// The authored program, prepared exactly as production loads it, plus the lines and stable ids the tests name.
struct Program
{
    memory::GrowableTlsfAllocator root;
    String                        text{&root};
    Context                       loaded{&root};
    Module*                       module = nullptr;
    plan::CompileResult           compiled{&root};
    u32                           line_a = 0U;
    u32                           line_b = 0U;
    u32                           line_s = 0U;
    u32                           line_call = 0U;
    u32                           line_for = 0U;
    u32                           line_x = 0U;
    u32                           line_h = 0U;
    u32                           line_ret = 0U;
    u32                           line_len = 0U;
    StableId                      id_a{};
    StableId                      id_b{};
    StableId                      id_s{};
    StableId                      id_call{};
    StableId                      id_for{};
    StableId                      id_x{};
    StableId                      id_h{};
    StableId                      id_len{};

    Program()
    {
        Context builder(&root);
        register_dialects(builder);
        text = print(builder, *build_program(builder), &root);
        const StringView src(text.data(), text.size());
        line_h    = line_of(src, "arith.muli", 0U);
        line_len  = line_of(src, "qty<", 0U);
        line_a    = line_of(src, "arith.addi", 0U);
        line_b    = line_of(src, "arith.addi", 1U);
        line_s    = line_of(src, "arith.addi", 2U);
        line_x    = line_of(src, "arith.addi", 3U);
        line_call = line_of(src, "func.call", 0U);
        line_for  = line_of(src, "core.for", 0U);
        line_ret  = line_of(src, "func.return", 1U);

        Context ctx(&root);
        register_dialects(ctx);
        const ParseResult pr = parse(ctx, src, ctx.register_file(kFile));
        REQUIRE(pr.ok);
        DiagnosticEngine diag(ctx, &root);
        AnalysisManager  am(&root);
        PassManager      pm(&root);
        pm.add_pass(cse_pass());
        pm.run(ctx, *pr.module, am, diag);
        REQUIRE_FALSE(diag.has_errors());
        const Array<u8> blob = serialize(ctx, *pr.module, &root);
        register_dialects(loaded);
        const ParseResult lr = deserialize(loaded, ConstSpan<u8>(blob.data(), blob.size()));
        REQUIRE(lr.ok);
        module   = lr.module;
        compiled = plan::compile(loaded, *module, "main", &root);
        REQUIRE(compiled.ok());

        id_a    = op_at(line_a);
        id_s    = op_at(line_s);
        id_x    = op_at(line_x);
        id_h    = op_at(line_h);
        id_len  = op_at(line_len);
        id_call = op_at(line_call);
        id_for  = op_at(line_for);
        CHECK(op_at(line_b) == id_a); // the CSE survivor carries the duplicate's line
        // The erased duplicate's own identity survives only as an origin of the survivor.
        const plan::CompiledPlan& p = compiled.plan;
        for (u32 i = 0; i < static_cast<u32>(p.sites.size()); ++i)
        {
            for (u32 o = 0; o < p.sites[i].origins_cnt; ++o)
            {
                const Origin& origin = p.site_origins[p.sites[i].origins_off + o];
                if (origin.loc.line == line_b && origin.node != p.sites[i].op)
                {
                    id_b = origin.node;
                }
            }
        }
        REQUIRE(id_b.valid());
        REQUIRE(id_b != id_a);
    }

    // The compiled op whose authored origins carry `line` (the first such site).
    [[nodiscard]] StableId op_at(u32 line) const
    {
        const plan::CompiledPlan& p = compiled.plan;
        for (u32 i = 0; i < static_cast<u32>(p.sites.size()); ++i)
        {
            for (u32 o = 0; o < p.sites[i].origins_cnt; ++o)
            {
                if (p.site_origins[p.sites[i].origins_off + o].loc.line == line)
                {
                    return p.sites[i].op;
                }
            }
        }
        return StableId{};
    }
};

// Runs `body` on a second thread (the executing thread). If the test leaves early (a failed REQUIRE) while the
// execution is still running or paused, the destructor cancels it until the thread ends, so a failure can neither
// hang the test nor destroy a joinable thread.
class Worker
{
public:
    template <typename F> Worker(insp::Session& s, u64 gen, F&& body) : m_session(s), m_gen(gen)
    {
        m_thread = std::thread(
            [this, b = std::forward<F>(body)]() mutable
            {
                b();
                m_done.store(true);
            });
    }
    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&)                 = delete;
    Worker& operator=(Worker&&)      = delete;
    ~Worker()
    {
        stop();
    }
    // Wait for the execution to end; one still running or paused after kWaitMs (a failed check left it stopped) is
    // cancelled, so the join always returns.
    void join()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        while (!m_done.load() && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop();
    }

private:
    void stop()
    {
        if (!m_thread.joinable())
        {
            return;
        }
        while (!m_done.load())
        {
            (void)m_session.cancel(m_gen);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_thread.join();
    }

    insp::Session&    m_session;
    u64               m_gen;
    std::atomic<bool> m_done{false};
    std::thread       m_thread;
};

// Runs the bound plan through the session on a Worker with its own allocator; `join` returns its result.
class PlanRunner
{
public:
    PlanRunner(insp::Session& s, const plan::CompiledPlan& p, i64 n, u64 gen = kGen)
        : m_result(&m_alloc), m_arg(n),
          m_worker(s, gen, [this, &s, &p] { m_result = s.run(p, ConstSpan<i64>(&m_arg, 1U), &m_alloc); })
    {
    }
    const plan::RunResult& join()
    {
        m_worker.join();
        return m_result;
    }

private:
    memory::GrowableTlsfAllocator m_alloc;
    plan::RunResult               m_result;
    i64                           m_arg = 0;
    Worker                        m_worker; // last: it runs while the members above are alive
};

// Request a pause once the execution has attached (a request before it starts is refused NotRunning).
insp::Refusal request_pause_when_running(insp::Session& s, u64 gen)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    for (;;)
    {
        const insp::Refusal r = s.request_pause(gen);
        if (r != insp::Refusal::NotRunning || std::chrono::steady_clock::now() > until)
        {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

insp::ValueSnapshot read(insp::Session& s, StableId op, memory::IAllocator* alloc)
{
    insp::ValueSnapshot v(alloc);
    REQUIRE(s.snapshot(kGen, insp::ValueRef{op, 0U}, v, kWaitMs) == insp::Refusal::None);
    return v;
}

void bind_plan(insp::Session& s, Program& prog)
{
    Array<insp::BindReport> rep(&prog.root);
    REQUIRE(s.bind(prog.compiled.plan, prog.loaded, kGen, rep) == insp::Refusal::None);
}
} // namespace

TEST_CASE("diag 8b: a plan breakpoint pauses the executing thread and snapshots typed values", "[ceir][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator ctl;
    memory::GrowableTlsfAllocator sess_alloc;
    insp::Session                 s(&sess_alloc);
    u32                           bp_s     = 0U;
    u32                           bp_ret   = 0U;
    u32                           bp_blank = 0U;
    u32                           bp_other = 0U;
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_s, bp_s) == insp::Refusal::None);
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_ret, bp_ret) == insp::Refusal::None);
    REQUIRE(s.add_line_breakpoint(kFile, 1U, bp_blank) == insp::Refusal::None); // the `module {` line
    REQUIRE(s.add_line_breakpoint("programs/diag/elsewhere.ceir", prog.line_s, bp_other) == insp::Refusal::None);

    insp::StopRecord early;
    CHECK(s.wait_for_stop(kGen, 1U, early) == insp::Refusal::NotBound); // nothing is bound yet

    Array<insp::BindReport> rep(&ctl);
    REQUIRE(s.bind(prog.compiled.plan, prog.loaded, kGen, rep) == insp::Refusal::None);
    REQUIRE(rep.size() == 4U);
    CHECK(rep[bp_s].status == insp::BindStatus::Bound);
    CHECK(rep[bp_s].first_op == prog.id_s);
    CHECK(rep[bp_ret].status == insp::BindStatus::NoCodeAtLine); // a terminator is not a compiled instr
    CHECK(rep[bp_blank].status == insp::BindStatus::NoCodeAtLine);
    CHECK(rep[bp_other].status == insp::BindStatus::UnknownFile);

    PlanRunner       runner(s, prog.compiled.plan, 3);
    insp::StopRecord stop;
    REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.reason == insp::StopReason::Breakpoint);
    CHECK(stop.breakpoint == bp_s);
    CHECK(stop.op == prog.id_s);
    CHECK(stop.depth == 0U);
    CHECK(stop.generation == kGen);
    CHECK(stop.sequence == 1U);
    REQUIRE(stop.at.valid());
    CHECK(prog.compiled.plan.seqs[stop.at.seq].instrs[stop.at.instr].op == plan::Op::AddI);

    const insp::ValueSnapshot a = read(s, prog.id_a, &ctl);
    CHECK(a.status == insp::ValueStatus::Available);
    CHECK(a.bits == 3);
    CHECK(StringView(a.type_text.data(), a.type_text.size()) == StringView("!i32"));
    CHECK_FALSE(a.has_unit);

    const insp::ValueSnapshot len = read(s, prog.id_len, &ctl);
    CHECK(len.status == insp::ValueStatus::Available);
    CHECK(len.bits == 7);
    CHECK(len.has_unit);
    CHECK(len.unit.exp[0] == 1);
    CHECK(len.unit.exp[2] == 0);
    CHECK(contains(len.type_text, StringView("qty<")));

    const insp::ValueSnapshot here = read(s, prog.id_s, &ctl); // the op at the safe point has not run
    CHECK(here.status == insp::ValueStatus::NotYetComputed);
    CHECK(read(s, prog.id_call, &ctl).status == insp::ValueStatus::NotYetComputed);
    CHECK(read(s, prog.id_x, &ctl).status == insp::ValueStatus::OutOfScope); // the loop body is not on the path
    CHECK(read(s, prog.id_h, &ctl).status == insp::ValueStatus::OutOfScope); // another function

    const insp::ValueSnapshot gone = read(s, prog.id_b, &ctl);
    CHECK(gone.status == insp::ValueStatus::OptimizedAway);
    CHECK(gone.survivor == prog.id_a);
    CHECK(read(s, StableId{0xFFFFFFF0U}, &ctl).status == insp::ValueStatus::NoSuchValue);
    insp::ValueSnapshot past(&ctl);
    REQUIRE(s.snapshot(kGen, insp::ValueRef{prog.id_a, 1U}, past, kWaitMs) == insp::Refusal::None);
    CHECK(past.status == insp::ValueStatus::NoSuchValue); // addi has one result

    // Stale and configuration requests are refused before any work, and leave the pause intact.
    insp::ValueSnapshot stale(&ctl);
    CHECK(s.snapshot(kGen + 1U, insp::ValueRef{prog.id_a, 0U}, stale, kWaitMs) == insp::Refusal::StaleGeneration);
    CHECK(s.resume(kGen + 1U, insp::Resume::Continue) == insp::Refusal::StaleGeneration);
    CHECK(s.cancel(kGen - 1U) == insp::Refusal::StaleGeneration);
    u32 late = 0U;
    CHECK(s.add_line_breakpoint(kFile, prog.line_a, late) == insp::Refusal::Busy);
    CHECK(s.bind(prog.compiled.plan, prog.loaded, kGen, rep) == insp::Refusal::Busy);
    CHECK(read(s, prog.id_a, &ctl).bits == 3);

    REQUIRE(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::None);
    CHECK(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::NotPaused); // accepted once
    CHECK(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::Finished);
    const plan::RunResult& r = runner.join();
    REQUIRE(r.ok());
    REQUIRE(r.values.size() == 1U);
    CHECK(r.values[0] == 36);
    CHECK(s.refused_pauses() == 0U);
}

TEST_CASE("diag 8b: plan stepping follows call frames", "[ceir][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator ctl;
    memory::GrowableTlsfAllocator sess_alloc;

    SECTION("step into enters the callee; step out returns; step over stays in the frame")
    {
        insp::Session s(&sess_alloc);
        u32           bp = 0U;
        REQUIRE(s.add_line_breakpoint(kFile, prog.line_call, bp) == insp::Refusal::None);
        bind_plan(s, prog);
        PlanRunner       runner(s, prog.compiled.plan, 2);
        insp::StopRecord stop;
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.op == prog.id_call);
        CHECK(stop.depth == 0U);

        REQUIRE(s.resume(kGen, insp::Resume::StepInto) == insp::Refusal::None);
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.reason == insp::StopReason::Step);
        CHECK(stop.op == prog.id_h);
        CHECK(stop.depth == 1U);
        CHECK(read(s, prog.id_s, &ctl).status == insp::ValueStatus::OutOfScope); // the caller's frame
        CHECK(read(s, prog.id_h, &ctl).status == insp::ValueStatus::NotYetComputed);

        REQUIRE(s.resume(kGen, insp::Resume::StepOut) == insp::Refusal::None);
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.op == prog.id_for);
        CHECK(stop.depth == 0U);
        const insp::ValueSnapshot t = read(s, prog.id_call, &ctl);
        CHECK(t.status == insp::ValueStatus::Available);
        CHECK(t.bits == 36);

        REQUIRE(s.resume(kGen, insp::Resume::StepOver) == insp::Refusal::None); // the loop body is the same frame
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.op == prog.id_x);
        CHECK(stop.depth == 0U);
        CHECK(read(s, prog.id_s, &ctl).bits == 6); // an outer region of the same frame is on the path

        REQUIRE(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::None);
        CHECK(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::Finished);
        const plan::RunResult& r = runner.join();
        REQUIRE(r.ok());
        CHECK(r.values[0] == 36);
    }

    SECTION("step over a call does not stop inside the callee")
    {
        insp::Session s(&sess_alloc);
        u32           bp = 0U;
        REQUIRE(s.add_line_breakpoint(kFile, prog.line_call, bp) == insp::Refusal::None);
        bind_plan(s, prog);
        PlanRunner       runner(s, prog.compiled.plan, 0);
        insp::StopRecord stop;
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        REQUIRE(s.resume(kGen, insp::Resume::StepOver) == insp::Refusal::None);
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.op == prog.id_for);
        CHECK(stop.depth == 0U);
        REQUIRE(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::None);
        CHECK(runner.join().ok());
    }
}

TEST_CASE("diag 8b: a pause request and a cancel stop a long run safely", "[ceir][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator ctl;
    memory::GrowableTlsfAllocator sess_alloc;
    constexpr i64                 long_run = i64{1} << 40; // fuel, not the bound, ends it: seconds, never ms

    SECTION("pause while running, inspect, then cancel at the pause")
    {
        insp::Session s(&sess_alloc);
        bind_plan(s, prog);
        PlanRunner runner(s, prog.compiled.plan, long_run);
        REQUIRE(request_pause_when_running(s, kGen) == insp::Refusal::None);
        insp::StopRecord stop;
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.reason == insp::StopReason::PauseRequest);
        CHECK(stop.breakpoint == insp::kNoBreakpoint);
        CHECK(read(s, stop.op, &ctl).status == insp::ValueStatus::NotYetComputed); // the held op has not run
        REQUIRE(s.cancel(kGen) == insp::Refusal::None);
        const plan::RunResult& r = runner.join();
        CHECK(r.error == plan::RunError::Cancelled);
        REQUIRE(r.fault.valid()); // blamed on the instr the pause held, which never ran
        CHECK(r.fault.seq == stop.at.seq);
        CHECK(r.fault.instr == stop.at.instr);
        CHECK(s.cancel(kGen) == insp::Refusal::NotRunning);
    }

    SECTION("cancel while running, with no pause")
    {
        insp::Session s(&sess_alloc);
        bind_plan(s, prog);
        PlanRunner runner(s, prog.compiled.plan, long_run);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        insp::Refusal c  = s.cancel(kGen);
        while (c == insp::Refusal::NotRunning && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            c = s.cancel(kGen);
        }
        REQUIRE(c == insp::Refusal::None);
        const plan::RunResult& r = runner.join();
        CHECK(r.error == plan::RunError::Cancelled);
        CHECK(r.fault.valid());
    }

    SECTION("a run with no debugger still ends at its fuel budget")
    {
        const i64             n = long_run;
        const plan::RunResult r = plan::run(prog.compiled.plan, ConstSpan<i64>(&n, 1U), &ctl);
        CHECK(r.error == plan::RunError::FuelExhausted);
    }
}

TEST_CASE("diag 8b: pause scopes refuse instead of blocking", "[ceir][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator ctl;
    memory::GrowableTlsfAllocator sess_alloc;
    const i64                     n = 2;

    SECTION("a non-pausable session counts its hits and never waits")
    {
        insp::Session s(&sess_alloc, insp::PauseScope::NonPausable);
        u32           bp = 0U;
        REQUIRE(s.add_line_breakpoint(kFile, prog.line_x, bp) == insp::Refusal::None);
        bind_plan(s, prog);
        CHECK(s.request_pause(kGen) == insp::Refusal::NonPausable);
        const plan::RunResult r = s.run(prog.compiled.plan, ConstSpan<i64>(&n, 1U), &ctl); // this thread
        REQUIRE(r.ok());
        CHECK(r.values[0] == 36);
        CHECK(s.refused_pauses() == 2U); // the loop body ran twice
        CHECK(s.last_refusal() == insp::Refusal::NonPausable);
    }

    SECTION("a safe point on the controller's own thread is refused")
    {
        insp::Session s(&sess_alloc);
        s.connect_controller();
        u32 bp = 0U;
        REQUIRE(s.add_line_breakpoint(kFile, prog.line_s, bp) == insp::Refusal::None);
        bind_plan(s, prog);
        const plan::RunResult r = s.run(prog.compiled.plan, ConstSpan<i64>(&n, 1U), &ctl);
        REQUIRE(r.ok());
        CHECK(s.refused_pauses() == 1U);
        CHECK(s.last_refusal() == insp::Refusal::SameThread);
    }

    SECTION("a whole-host pause without the host's hooks is refused")
    {
        insp::Session s(&sess_alloc, insp::PauseScope::WholeHost);
        u32           bp = 0U;
        REQUIRE(s.add_line_breakpoint(kFile, prog.line_s, bp) == insp::Refusal::None);
        bind_plan(s, prog);
        CHECK(s.request_pause(kGen) == insp::Refusal::NoHostPause);
        const plan::RunResult r = s.run(prog.compiled.plan, ConstSpan<i64>(&n, 1U), &ctl);
        REQUIRE(r.ok());
        CHECK(s.last_refusal() == insp::Refusal::NoHostPause);
    }

    SECTION("a whole-host pause freezes the host for exactly the pause")
    {
        struct Host
        {
            std::atomic<u32> freezes{0U};
            std::atomic<u32> thaws{0U};
        } host;
        const insp::HostPause hooks{[](void* u) { static_cast<Host*>(u)->freezes.fetch_add(1U); },
                                    [](void* u) { static_cast<Host*>(u)->thaws.fetch_add(1U); }, &host};
        insp::Session         s(&sess_alloc, insp::PauseScope::WholeHost, hooks);
        u32                   bp = 0U;
        REQUIRE(s.add_line_breakpoint(kFile, prog.line_s, bp) == insp::Refusal::None);
        bind_plan(s, prog);
        PlanRunner       runner(s, prog.compiled.plan, 2);
        insp::StopRecord stop;
        REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
        CHECK(host.freezes.load() == 1U);
        CHECK(host.thaws.load() == 0U);
        REQUIRE(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::None);
        CHECK(runner.join().ok());
        CHECK(host.thaws.load() == 1U);
    }

    SECTION("a wait on an execution that never stops times out instead of hanging")
    {
        insp::Session s(&sess_alloc);
        bind_plan(s, prog);
        insp::StopRecord stop;
        CHECK(s.wait_for_stop(kGen, 5U, stop) == insp::Refusal::Timeout); // bound, nothing running
        insp::ValueSnapshot v(&ctl);
        CHECK(s.snapshot(kGen, insp::ValueRef{prog.id_a, 0U}, v, 5U) == insp::Refusal::NotPaused);
    }
}

TEST_CASE("diag 8b: a redaction policy withholds bits but not the type", "[ceir][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator ctl;
    memory::GrowableTlsfAllocator sess_alloc;
    insp::Session                 s(&sess_alloc);
    StableId                      secret = prog.id_a;
    s.set_redaction(
        [](StableId op, TypeId, void* u)
        {
            return (op == *static_cast<const StableId*>(u)) ? insp::RedactionClass::Restricted
                                                             : insp::RedactionClass::Public;
        },
        &secret);
    u32 bp = 0U;
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_s, bp) == insp::Refusal::None);
    bind_plan(s, prog);
    PlanRunner       runner(s, prog.compiled.plan, 1);
    insp::StopRecord stop;
    REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
    const insp::ValueSnapshot a = read(s, prog.id_a, &ctl);
    CHECK(a.status == insp::ValueStatus::Redacted);
    CHECK(a.bits == 0);
    CHECK(a.redaction == insp::RedactionClass::Restricted);
    CHECK(StringView(a.type_text.data(), a.type_text.size()) == StringView("!i32"));
    const insp::ValueSnapshot len = read(s, prog.id_len, &ctl);
    CHECK(len.status == insp::ValueStatus::Available);
    CHECK(len.redaction == insp::RedactionClass::Public);
    REQUIRE(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::None);
    CHECK(runner.join().ok());
}

TEST_CASE("diag 8b: the reference interpreter pauses at the same authored line", "[ceir][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator ctl;
    memory::GrowableTlsfAllocator sess_alloc;
    memory::GrowableTlsfAllocator exec_alloc;
    insp::Session                 s(&sess_alloc);
    u32                           bp_s    = 0U;
    u32                           bp_call = 0U;
    u32                           bp_ret  = 0U;
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_s, bp_s) == insp::Refusal::None);
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_call, bp_call) == insp::Refusal::None);
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_ret, bp_ret) == insp::Refusal::None);
    Array<insp::BindReport> rep(&ctl);
    REQUIRE(s.bind(*prog.module, prog.loaded, kGen, rep) == insp::Refusal::None);
    CHECK(rep[bp_s].status == insp::BindStatus::Bound);
    CHECK(rep[bp_ret].status == insp::BindStatus::Bound); // the interpreter dispatches terminators

    exec::Interpreter in(prog.loaded, crd::u64{1} << 24U, &exec_alloc);
    exec::install_builtin_semantics(in);
    exec::ExecResult result(&exec_alloc);
    const i64        n = 2;
    Worker           exec_thread(s, kGen, [&] { result = s.invoke(in, *prog.module, "main", ConstSpan<i64>(&n, 1U)); });

    insp::StopRecord stop;
    REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.breakpoint == bp_s);
    CHECK(stop.op == prog.id_s);
    CHECK_FALSE(stop.at.valid()); // no compiled instr
    const insp::ValueSnapshot a = read(s, prog.id_a, &ctl);
    CHECK(a.status == insp::ValueStatus::Available);
    CHECK(a.bits == 3);
    CHECK(StringView(a.type_text.data(), a.type_text.size()) == StringView("!i32"));
    CHECK(read(s, prog.id_len, &ctl).has_unit);
    CHECK(read(s, prog.id_s, &ctl).status == insp::ValueStatus::NotYetComputed);
    CHECK(read(s, prog.id_x, &ctl).status == insp::ValueStatus::OutOfScope);
    const insp::ValueSnapshot gone = read(s, prog.id_b, &ctl);
    CHECK(gone.status == insp::ValueStatus::OptimizedAway);
    CHECK(gone.survivor == prog.id_a);

    REQUIRE(s.resume(kGen, insp::Resume::Continue) == insp::Refusal::None);
    REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.breakpoint == bp_call);
    REQUIRE(s.resume(kGen, insp::Resume::StepInto) == insp::Refusal::None);
    REQUIRE(s.wait_for_stop(kGen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.reason == insp::StopReason::Step);
    CHECK(stop.op == prog.id_h);
    CHECK(stop.depth == 1U);
    CHECK(read(s, prog.id_s, &ctl).status == insp::ValueStatus::OutOfScope);

    REQUIRE(s.cancel(kGen) == insp::Refusal::None); // a cancel at the pause: the held op never runs
    exec_thread.join();
    CHECK(result.error == exec::ExecError::Cancelled);
    REQUIRE(result.op != nullptr);
    CHECK(result.op->stable_id() == prog.id_h);
}
