// DIAG.8b — inspection of an execution that leaves work PENDING on crd-jobs. The HostProvider runs a program under an
// inspect::Session: the submitting thread stops at authored line breakpoints while a pooled launch it started keeps
// running on a pool worker; resume joins that launch, and a cancel at the pause (the session's or the provider's own)
// stops the pooled body too, so the execution returns. Bodies the provider runs on its own sub-interpreters (pooled
// launch bodies, parallel ranges, map_reduce fold steps) never pause: a breakpoint there is counted and refused
// `DetachedBody`, while a launch the provider runs in-frame (it captures an outer value) pauses normally.
//
// The program is authored as text under a registered file, optimized by CSE, serialized and loaded into a fresh
// Context; expected lines come from scanning the text, never from the parser or the session. The test thread is the
// pool's thread 0 and executes the program, as a host's main loop would; a second thread is the controller. It records
// what it saw and the test thread checks it after both have ended. A controller that finds the execution still running
// after its script cancels it through both flags (the backstop) and records that, so a broken cancel path fails the
// test instead of hanging it. ⛔ The jobs pool is owned by the listener in test_host_provider.cpp. ASCII test names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/cse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>
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
constexpr const char* kFile   = "programs/diag/host_inspect.ceir";
constexpr u32         kWaitMs = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
constexpr u64         kGen    = 1U;
constexpr i64         kSpin   = 0x7FFFFFFF; // a pooled body that cannot finish before it is cancelled

void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)task::register_task_ops(ctx);
    (void)async::register_async_ops(ctx);
    (void)func::register_dialect(ctx);
}

Operation* konst(Context& ctx, Block* b, i64 v)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32());
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
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

void yield_one(Context& ctx, Block* b, Value* v)
{
    Value* vs[1] = {v};
    yield_values(ctx, b, ConstSpan<Value*>(vs, 1U));
}

// core.for(0, %hi, 1) { %sq = muli(%iv, %iv); yield } in `b`, every bound defined in `b` (a pooled body captures none).
void busy_loop(Context& ctx, Block* b, i64 hi)
{
    Value*           range[3] = {konst(ctx, b, 0)->result(0U), konst(ctx, b, hi)->result(0U),
                                 konst(ctx, b, 1)->result(0U)};
    Operation* const loop =
        ctx.create_operation(ctx.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const lb = ctx.create_block(1U, ctx.type_i32());
    loop->region(0)->append(lb);
    (void)bin(ctx, "muli", lb, lb->arg(0U), lb->arg(0U));
    yield_values(ctx, lb, {});
    b->append(loop);
}

Operation* launch(Context& ctx, Block* parent, Block*& body)
{
    Operation* const l = ctx.create_operation(ctx.intern_op("async", "launch"), {}, 1U, ctx.type_i32(), 1U);
    body               = ctx.create_block(0U);
    l->region(0)->append(body);
    parent->append(l);
    return l;
}

Value* await_token(Context& ctx, Block* b, Value* token)
{
    Value*           tok[1] = {token};
    Operation* const aw =
        ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(tok, 1U), 1U, ctx.type_i32());
    b->append(aw);
    return aw->result(0U);
}

Block* add_func(Context& ctx, Module& m, const char* name)
{
    Operation* const fn = func::create_func(ctx, m, name, Visibility::Public, 0U, ctx.type_i32());
    m.body()->first_block()->append(fn);
    return func::func_body_block(fn);
}

// The markers are constants with distinctive values, so each breakpoint line is found by scanning the text.
// @main() {
//   %t  = async.launch { 701; for(0, 2000, 1) {..}; yield 7 }          pooled (captures nothing): detached
//   %c5 = 5
//   %u  = async.launch { 702; %y = addi(%c5, %c5); yield %y }           captures %c5: runs in-frame, pauses
//   task.parallel_for(0, 8, 1) { iv: 703; yield iv }                    8 detached ranges
//   %m  = task.map_reduce(0, 4, 1, 0) map { iv: yield iv } combine { acc, e: 704; yield addi(acc, e) }
//   705                                                                 %t not joined yet
//   %v = await %t; %w = await %u
//   706                                                                 every pooled launch joined
//   return addi(addi(%v, %m), %w)                                       7 + 6 + 10 = 23
// }
// @spin() { %t = async.launch { for(0, kSpin, 1) {..}; yield 9 }; 707; %v = await %t; return %v }
Module* build(Context& ctx)
{
    Module* const m = ctx.create_module();
    m->body()->append(ctx.create_block(0U));
    {
        Block* const     fb = add_func(ctx, *m, "main");
        Block*           lb = nullptr;
        Operation* const t  = launch(ctx, fb, lb);
        (void)konst(ctx, lb, 701);
        busy_loop(ctx, lb, 2000);
        yield_one(ctx, lb, konst(ctx, lb, 7)->result(0U));

        Value* const     c5 = konst(ctx, fb, 5)->result(0U);
        Block*           ub = nullptr;
        Operation* const u  = launch(ctx, fb, ub);
        (void)konst(ctx, ub, 702);
        yield_one(ctx, ub, bin(ctx, "addi", ub, c5, c5));

        Value* range[3] = {konst(ctx, fb, 0)->result(0U), konst(ctx, fb, 8)->result(0U), konst(ctx, fb, 1)->result(0U)};
        Operation* const pf =
            ctx.create_operation(ctx.intern_op("task", "parallel_for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
        fb->append(pf);
        Block* const pb = ctx.create_block(1U, ctx.type_i32());
        pf->region(0)->append(pb);
        (void)konst(ctx, pb, 703);
        yield_one(ctx, pb, pb->arg(0U));

        Value* ops4[4] = {konst(ctx, fb, 0)->result(0U), konst(ctx, fb, 4)->result(0U), konst(ctx, fb, 1)->result(0U),
                          konst(ctx, fb, 0)->result(0U)};
        Operation* const mr = ctx.create_operation(ctx.intern_op("task", "map_reduce"), ConstSpan<Value*>(ops4, 4U), 1U,
                                                   ctx.type_i32(), 2U);
        fb->append(mr);
        Block* const mapb = ctx.create_block(1U, ctx.type_i32());
        mr->region(0)->append(mapb);
        yield_one(ctx, mapb, mapb->arg(0U));
        Block* const cb = ctx.create_block(2U, ctx.type_i32());
        mr->region(1)->append(cb);
        (void)konst(ctx, cb, 704);
        yield_one(ctx, cb, bin(ctx, "addi", cb, cb->arg(0U), cb->arg(1U)));

        (void)konst(ctx, fb, 705);
        Value* const v = await_token(ctx, fb, t->result(0U));
        Value* const w = await_token(ctx, fb, u->result(0U));
        (void)konst(ctx, fb, 706);
        Value* const r  = bin(ctx, "addi", fb, bin(ctx, "addi", fb, v, mr->result(0U)), w);
        Value*       rv[1] = {r};
        fb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    }
    {
        Block* const     fb = add_func(ctx, *m, "spin");
        Block*           lb = nullptr;
        Operation* const t  = launch(ctx, fb, lb);
        busy_loop(ctx, lb, kSpin);
        yield_one(ctx, lb, konst(ctx, lb, 9)->result(0U));
        (void)konst(ctx, fb, 707);
        Value* rv[1] = {await_token(ctx, fb, t->result(0U))};
        fb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    }
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

void collect(const Region* r, Array<const Operation*>& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr)
    {
        return;
    }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.push_back(op);
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                collect(op->region(i), out);
            }
        }
    }
}

// The authored program, prepared exactly as production loads it, plus the lines and stable ids the tests name.
struct Program
{
    memory::GrowableTlsfAllocator root;
    String                        text{&root};
    Context                       loaded{&root};
    Module*                       module = nullptr;
    u32                           line_launch  = 0U; // 701, in the pooled launch body
    u32                           line_inframe = 0U; // 702, in the in-frame launch body
    u32                           line_range   = 0U; // 703, in the parallel_for body
    u32                           line_fold    = 0U; // 704, in the map_reduce combine
    u32                           line_after   = 0U; // 705
    u32                           line_joined  = 0U; // 706
    u32                           line_spin    = 0U; // 707
    StableId                      id_launch_marker{};
    StableId                      id_map_reduce{};
    StableId                      id_await_t{};
    StableId                      id_after{};
    StableId                      id_spin{};

    Program()
    {
        Context builder(&root);
        register_dialects(builder);
        text = print(builder, *build(builder), &root);
        const StringView src(text.data(), text.size());
        line_launch  = line_of(src, "{value = 701}", 0U);
        line_inframe = line_of(src, "{value = 702}", 0U);
        line_range   = line_of(src, "{value = 703}", 0U);
        line_fold    = line_of(src, "{value = 704}", 0U);
        line_after   = line_of(src, "{value = 705}", 0U);
        line_joined  = line_of(src, "{value = 706}", 0U);
        line_spin    = line_of(src, "{value = 707}", 0U);
        const u32 line_mr    = line_of(src, "task.map_reduce", 0U);
        const u32 line_await = line_of(src, "async.await", 0U);
        REQUIRE(line_launch != 0U);
        REQUIRE(line_inframe > line_launch);
        REQUIRE(line_range > line_inframe);
        REQUIRE(line_fold > line_range);
        REQUIRE(line_after > line_fold);
        REQUIRE(line_await > line_after);
        REQUIRE(line_joined > line_await);
        REQUIRE(line_spin > line_joined);

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
        module = lr.module;

        id_launch_marker = op_at(line_launch);
        id_map_reduce    = op_at(line_mr);
        id_await_t       = op_at(line_await);
        id_after         = op_at(line_after);
        id_spin          = op_at(line_spin);
        REQUIRE(id_launch_marker.valid());
        REQUIRE(id_map_reduce.valid());
        REQUIRE(id_await_t.valid());
        REQUIRE(id_after.valid());
        REQUIRE(id_spin.valid());
    }

    // The loaded op whose authored origin is `line` (the first in pre-order).
    [[nodiscard]] StableId op_at(u32 line)
    {
        Array<const Operation*> ops(&root);
        collect(module->body(), ops);
        Array<Origin> storage(&root);
        for (usize i = 0; i < ops.size(); ++i)
        {
            const Provenance    p = resolve_provenance(loaded, ops[i], storage);
            const Origin* const o = p.primary();
            if (o != nullptr && o->loc.line == line)
            {
                return ops[i]->stable_id();
            }
        }
        return StableId{};
    }
};

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

// The controller thread. `script` drives the session; afterwards the execution must return by itself within kWaitMs.
// One that does not is cancelled through the session and the provider until it returns, and `backstop()` reports it.
class Controller
{
public:
    template <typename F>
    Controller(insp::Session& s, host::HostProvider& p, F&& script) : m_session(s), m_provider(p)
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
    // The test thread: the provider returned. Joins the controller.
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
            m_provider.request_cancel();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    insp::Session&      m_session;
    host::HostProvider& m_provider;
    std::atomic<bool>   m_returned{false};
    std::atomic<bool>   m_backstop{false};
    std::thread         m_thread;
};

// One stop as the controller saw it, plus up to two value reads made while it was held.
struct Seen
{
    insp::Refusal       wait = insp::Refusal::Timeout;
    insp::StopRecord    stop;
    insp::ValueSnapshot first;
    insp::ValueSnapshot second;
    explicit Seen(memory::IAllocator* a) : first(a), second(a) {}
};

void read_into(insp::Session& s, StableId op, insp::ValueSnapshot& out)
{
    insp::ValueRef ref;
    ref.op = op;
    (void)s.snapshot(kGen, ref, out, kWaitMs);
}

u32 bind_all(insp::Session& s, Program& prog, memory::IAllocator* a, const u32 (&lines)[7], u32 (&bps)[7])
{
    for (u32 i = 0; i < 7U; ++i)
    {
        REQUIRE(s.add_line_breakpoint(kFile, lines[i], bps[i]) == insp::Refusal::None);
    }
    Array<insp::BindReport> rep(a);
    REQUIRE(s.bind(*prog.module, prog.loaded, kGen, rep) == insp::Refusal::None);
    u32 bound = 0U;
    for (usize i = 0; i < rep.size(); ++i)
    {
        bound += (rep[i].status == insp::BindStatus::Bound) ? 1U : 0U;
    }
    return bound;
}
} // namespace

TEST_CASE("diag 8b: a stop reports the pooled launch still pending and resume joins it", "[ceir][host][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator sess_alloc;
    memory::GrowableTlsfAllocator ctl_alloc;
    memory::GrowableTlsfAllocator prov_alloc;
    insp::Session                 s(&sess_alloc);
    const u32 lines[7] = {prog.line_launch, prog.line_inframe, prog.line_range,  prog.line_fold,
                          prog.line_after,  prog.line_joined,  prog.line_spin};
    u32       bps[7]   = {};
    CHECK(bind_all(s, prog, &ctl_alloc, lines, bps) == 7U); // every marker line, detached bodies included, binds

    host::HostProvider prov(&prov_alloc, 4U);
    Seen               inframe(&ctl_alloc);
    Seen               after(&ctl_alloc);
    Seen               joined(&ctl_alloc);
    insp::Refusal      end = insp::Refusal::None;
    const auto script = [&]
    {
        inframe.wait = s.wait_for_stop(kGen, kWaitMs, inframe.stop);
        read_into(s, prog.id_launch_marker, inframe.first);
        (void)s.resume(kGen, insp::Resume::Continue);
        after.wait = s.wait_for_stop(kGen, kWaitMs, after.stop);
        read_into(s, prog.id_map_reduce, after.first);
        (void)s.resume(kGen, insp::Resume::Continue);
        joined.wait = s.wait_for_stop(kGen, kWaitMs, joined.stop);
        read_into(s, prog.id_await_t, joined.first);
        (void)s.resume(kGen, insp::Resume::Continue);
        end = wait_finished(s);
    };
    Controller ctl(s, prov, script);
    const exec::ExecResult r = prov.execute(prog.loaded, *prog.module, "main", {}, s);
    ctl.returned();
    CHECK_FALSE(ctl.backstop());

    // The in-frame launch body pauses on the submitting thread while the pooled launch is still not joined.
    REQUIRE(inframe.wait == insp::Refusal::None);
    CHECK(inframe.stop.breakpoint == bps[1]);
    CHECK(inframe.stop.pending_jobs == 1U);
    CHECK(inframe.first.status == insp::ValueStatus::OutOfScope); // the pooled body's value lives on a worker
    REQUIRE(after.wait == insp::Refusal::None);
    CHECK(after.stop.breakpoint == bps[4]);
    CHECK(after.stop.op == prog.id_after);
    CHECK(after.stop.pending_jobs == 1U);
    CHECK(after.first.status == insp::ValueStatus::Available); // the fold ran on its own sub; its result is here
    CHECK(after.first.bits == 6);
    REQUIRE(joined.wait == insp::Refusal::None);
    CHECK(joined.stop.breakpoint == bps[5]);
    CHECK(joined.stop.pending_jobs == 0U); // the await joined it
    CHECK(joined.first.status == insp::ValueStatus::Available);
    CHECK(joined.first.bits == 7);
    CHECK(end == insp::Refusal::Finished);

    REQUIRE(r.ok());
    REQUIRE(r.values.size() == 1U);
    CHECK(r.values[0] == 23);
    CHECK(prov.pooled_count() == 1U); // the first launch really ran on a worker; the capturing one ran in-frame
    CHECK(prov.pooled_unjoined() == 0U);
    // Detached bodies never paused: 1 pooled launch body + 8 parallel ranges + 4 fold steps, each counted and refused.
    CHECK(s.detached_hits() == 13U);
    CHECK(s.refused_pauses() == 13U);
    CHECK(s.last_refusal() == insp::Refusal::DetachedBody);
}

TEST_CASE("diag 8b: a session cancel at the pause also cancels the pooled launch still running", "[ceir][host][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator sess_alloc;
    memory::GrowableTlsfAllocator ctl_alloc;
    memory::GrowableTlsfAllocator prov_alloc;
    insp::Session                 s(&sess_alloc);
    u32                           bp = 0U;
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_spin, bp) == insp::Refusal::None);
    Array<insp::BindReport> rep(&ctl_alloc);
    REQUIRE(s.bind(*prog.module, prog.loaded, kGen, rep) == insp::Refusal::None);

    // The pooled body's fuel outlasts its loop, so only a cancel can end it.
    host::HostProvider prov(&prov_alloc, 4U, crd::u64{1} << 40U);
    Seen               held(&ctl_alloc);
    insp::Refusal      cancel = insp::Refusal::Timeout;
    insp::Refusal      end    = insp::Refusal::None;
    const auto script = [&]
    {
        held.wait = s.wait_for_stop(kGen, kWaitMs, held.stop);
        cancel    = s.cancel(kGen);
        end       = wait_finished(s);
    };
    Controller ctl(s, prov, script);
    const exec::ExecResult r = prov.execute(prog.loaded, *prog.module, "spin", {}, s);
    ctl.returned();

    CHECK_FALSE(ctl.backstop()); // the pooled body observed the cancel; nothing had to be forced
    REQUIRE(held.wait == insp::Refusal::None);
    CHECK(held.stop.breakpoint == bp);
    CHECK(held.stop.op == prog.id_spin);
    CHECK(held.stop.pending_jobs == 1U);
    CHECK(cancel == insp::Refusal::None);
    CHECK(end == insp::Refusal::Finished);
    CHECK(r.error == exec::ExecError::Cancelled);
    REQUIRE(r.op != nullptr);
    CHECK(r.op->stable_id() == prog.id_spin); // the held op never ran
    CHECK(prov.pooled_count() == 1U);
}

TEST_CASE("diag 8b: the provider's own cancel ends an execution held at a pause", "[ceir][host][diag]")
{
    Program                       prog;
    memory::GrowableTlsfAllocator sess_alloc;
    memory::GrowableTlsfAllocator ctl_alloc;
    memory::GrowableTlsfAllocator prov_alloc;
    insp::Session                 s(&sess_alloc);
    u32                           bp = 0U;
    REQUIRE(s.add_line_breakpoint(kFile, prog.line_spin, bp) == insp::Refusal::None);
    Array<insp::BindReport> rep(&ctl_alloc);
    REQUIRE(s.bind(*prog.module, prog.loaded, kGen, rep) == insp::Refusal::None);

    host::HostProvider prov(&prov_alloc, 4U, crd::u64{1} << 40U);
    Seen               held(&ctl_alloc);
    insp::Refusal      end = insp::Refusal::None;
    const auto script = [&]
    {
        held.wait = s.wait_for_stop(kGen, kWaitMs, held.stop);
        prov.request_cancel(); // not the session's cancel: the provider's flag, raised without its lock
        end = wait_finished(s);
    };
    Controller ctl(s, prov, script);
    const exec::ExecResult r = prov.execute(prog.loaded, *prog.module, "spin", {}, s);
    ctl.returned();

    CHECK_FALSE(ctl.backstop());
    REQUIRE(held.wait == insp::Refusal::None);
    CHECK(held.stop.op == prog.id_spin);
    CHECK(end == insp::Refusal::Finished);
    CHECK(r.error == exec::ExecError::Cancelled);
    REQUIRE(r.op != nullptr);
    CHECK(r.op->stable_id() == prog.id_spin);
}
