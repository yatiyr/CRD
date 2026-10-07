// DIAG.8b: runtime inspection across a HOT RELOAD. A session binds the generation a ReloadSet installed (its RAF-11
// generation number) and resolves an authored `file:line` breakpoint through that generation's provenance. After a
// body edit hot-swaps generation 2 (a changed constant, and a blank line above the breakpoint line, so the breakpoint
// op moves down a line while it keeps its stable id and its compiled position), requests naming generation 2 are
// refused until the session REBINDS, and after the rebind requests naming generation 1 are refused. The rebind
// resolves each line to whatever generation 2 authored there: the old line is now blank (no code), and the moved op is
// found on its new line. A binding kept by op identity or by compiled position would stop at the wrong breakpoint.
// Plans run on a second thread; this test thread is the controller. Expected lines come from scanning the text itself.
// ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
namespace insp = crd::ceir::inspect;

namespace
{
constexpr const char* kFile   = "programs/diag/reload_inspect.ceir";
constexpr u32         kWaitMs = 20000U;

void register_dialects(Context& c)
{
    (void)arith::register_arith_ops(c);
    (void)core::register_core_ops(c);
    (void)func::register_dialect(c);
}
void registrar(Context& c, void* /*user*/)
{
    register_dialects(c);
}

Operation* konst(Context& c, Block* b, i64 v)
{
    Operation* const op = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_i32());
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}

// @main(%step) { %lo = 0; %hi = `hi`; %s = addi(%hi,%hi); core.for(%lo,%hi,%step) { yield }; return %s }. Changing
// `hi` changes the body but not the contract, so a reload hot-swaps.
String source(crd::memory::IAllocator* alloc, i64 hi_value)
{
    Context c(alloc);
    register_dialects(c);
    Module* const    m  = c.create_module();
    Operation* const fn = func::create_func(c, *m, "main", Visibility::Public, 1U, c.type_i32());
    m->body()->append(c.create_block(0U));
    m->body()->first_block()->append(fn);
    Block* const     body = func::func_body_block(fn);
    Operation* const lo   = konst(c, body, 0);
    Operation* const hi   = konst(c, body, hi_value);
    Value*           ab[2] = {hi->result(0U), hi->result(0U)};
    Operation* const s = c.create_operation(c.intern_op("arith", "addi"), ConstSpan<Value*>(ab, 2U), 1U, c.type_i32());
    body->append(s);
    Value*           range[3] = {lo->result(0U), hi->result(0U), body->arg(0U)};
    Operation* const loop = c.create_operation(c.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const     loop_body = c.create_block(1U, c.type_i32());
    loop->region(0)->append(loop_body);
    loop_body->append(c.create_operation(c.intern_op("core", "yield"), {}, 0U));
    body->append(loop);
    Value* rv[1] = {s->result(0U)};
    body->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));
    return print(c, *m, alloc);
}

StringView sv(const String& s)
{
    return StringView(s.data(), s.size());
}

// `text` with an empty line inserted before 1-based line `line`.
String insert_blank_line(StringView text, u32 line, crd::memory::IAllocator* alloc)
{
    String out(alloc);
    u32    at = 1U;
    for (usize i = 0; i < text.size(); ++i)
    {
        if (at == line && (i == 0U || text[i - 1U] == '\n'))
        {
            out.push_back('\n');
        }
        out.push_back(text[i]);
        if (text[i] == '\n')
        {
            ++at;
        }
    }
    return out;
}

// The 1-based line holding the n-th occurrence of `needle` (the independent oracle).
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

// The compiled op whose authored origins carry `line`.
StableId op_at(const plan::CompiledPlan& p, u32 line)
{
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

// Run `p` through the session on a Worker with its own allocator.
class PlanRunner
{
public:
    PlanRunner(insp::Session& s, const plan::CompiledPlan& p, u64 gen)
        : m_result(&m_alloc), m_worker(s, gen,
                                       [this, &s, &p]
                                       {
                                           const i64 step = 1;
                                           m_result       = s.run(p, ConstSpan<i64>(&step, 1U), &m_alloc);
                                       })
    {
    }
    const plan::RunResult& join()
    {
        m_worker.join();
        return m_result;
    }

private:
    crd::memory::GrowableTlsfAllocator m_alloc;
    plan::RunResult                    m_result;
    Worker                             m_worker; // last: it runs while the members above are alive
};
} // namespace

TEST_CASE("diag 8b: a hot reload rebinds line breakpoints and refuses the other generation", "[ceir][reload][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::memory::GrowableTlsfAllocator ctl;
    crd::memory::GrowableTlsfAllocator sess_alloc;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{4600U};
    const String                       text_1 = source(&root, 3);
    const u32                          s1     = line_of(sv(text_1), "arith.addi", 0U);
    REQUIRE(s1 != 0U);
    const String text_2 = insert_blank_line(sv(source(&root, 4)), s1, &root);
    const u32    s2     = line_of(sv(text_2), "arith.addi", 0U);
    const u32    hi2    = line_of(sv(text_2), "arith.const", 1U);
    REQUIRE(s2 == s1 + 1U); // generation 2's addi sits one line lower, where generation 1 had its core.for
    REQUIRE(line_of(sv(text_1), "core.for", 0U) == s2);

    REQUIRE(set.add_source(id, sv(text_1), StringView(kFile)).ok());
    const ProgramHandle       h1 = set.handle(id);
    Generation* const         g1 = set.generation(id);
    const u64                 gen1 = h1.generation.value;
    const plan::CompileResult p1 = plan::compile(*g1->ctx, *g1->program.module, "main", &root);
    REQUIRE(p1.ok());
    const StableId add_1 = op_at(p1.plan, s1);
    REQUIRE(add_1.valid());

    insp::Session s(&sess_alloc);
    u32           bp_line = 0U;
    u32           bp_next = 0U;
    REQUIRE(s.add_line_breakpoint(kFile, s1, bp_line) == insp::Refusal::None);
    REQUIRE(s.add_line_breakpoint(kFile, s2, bp_next) == insp::Refusal::None);
    Array<insp::BindReport> rep(&ctl);
    REQUIRE(s.bind(p1.plan, *g1->ctx, gen1, rep) == insp::Refusal::None);
    CHECK(rep[bp_line].status == insp::BindStatus::Bound);
    CHECK(rep[bp_line].first_op == add_1);
    CHECK(rep[bp_next].status == insp::BindStatus::Bound); // generation 1's core.for sits on that line

    {
        PlanRunner       runner(s, p1.plan, gen1);
        insp::StopRecord stop;
        REQUIRE(s.wait_for_stop(gen1, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.generation == gen1);
        CHECK(stop.breakpoint == bp_line);
        CHECK(stop.op == add_1);
        REQUIRE(s.resume(gen1, insp::Resume::Continue) == insp::Refusal::None);
        REQUIRE(s.wait_for_stop(gen1, kWaitMs, stop) == insp::Refusal::None);
        CHECK(stop.breakpoint == bp_next);
        REQUIRE(s.resume(gen1, insp::Resume::Continue) == insp::Refusal::None);
        const plan::RunResult& r = runner.join();
        REQUIRE(r.ok());
        CHECK(r.values[0] == 6);
    }

    // A body edit hot-swaps generation 2; the session is still bound to generation 1.
    const ReloadResult rr = set.reload_source(id, sv(text_2), StringView(kFile));
    REQUIRE(rr.decision == ReloadDecision::HotSwap);
    REQUIRE(rr.installed);
    const ProgramHandle h2   = set.handle(id);
    Generation* const   g2   = set.generation(id);
    const u64           gen2 = h2.generation.value;
    REQUIRE(gen2 != gen1);
    CHECK_FALSE(set.is_current(id, h1));
    insp::StopRecord idle;
    CHECK(s.wait_for_stop(gen2, 1U, idle) == insp::Refusal::StaleGeneration); // not rebound yet

    const plan::CompileResult p2 = plan::compile(*g2->ctx, *g2->program.module, "main", &root);
    REQUIRE(p2.ok());
    const StableId add_2   = op_at(p2.plan, s2);
    const StableId konst_2 = op_at(p2.plan, hi2);
    REQUIRE(add_2.valid());
    REQUIRE(konst_2.valid());
    CHECK(add_2 == add_1);                // the edit kept the addi's identity and compiled position
    CHECK_FALSE(op_at(p2.plan, s1).valid()); // and left generation 1's addi line blank
    REQUIRE(s.bind(p2.plan, *g2->ctx, gen2, rep) == insp::Refusal::None);
    CHECK(rep[bp_line].status == insp::BindStatus::NoCodeAtLine);
    CHECK(rep[bp_next].status == insp::BindStatus::Bound);
    CHECK(rep[bp_next].first_op == add_2); // the moved addi is found on its new line

    PlanRunner       runner(s, p2.plan, gen2);
    insp::StopRecord stop;
    CHECK(s.wait_for_stop(gen1, 1U, stop) == insp::Refusal::StaleGeneration);
    REQUIRE(s.wait_for_stop(gen2, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.generation == gen2);
    CHECK(stop.breakpoint == bp_next);
    CHECK(stop.op == add_2);
    insp::ValueSnapshot v(&ctl);
    CHECK(s.snapshot(gen1, insp::ValueRef{konst_2, 0U}, v, kWaitMs) == insp::Refusal::StaleGeneration);
    CHECK(s.resume(gen1, insp::Resume::Continue) == insp::Refusal::StaleGeneration);
    CHECK(s.cancel(gen1) == insp::Refusal::StaleGeneration);
    REQUIRE(s.snapshot(gen2, insp::ValueRef{konst_2, 0U}, v, kWaitMs) == insp::Refusal::None);
    CHECK(v.status == insp::ValueStatus::Available);
    CHECK(v.bits == 4); // generation 2's value, read from generation 2's frame
    REQUIRE(s.resume(gen2, insp::Resume::Continue) == insp::Refusal::None);
    CHECK(s.wait_for_stop(gen2, kWaitMs, stop) == insp::Refusal::Finished); // no other line is bound
    const plan::RunResult& r = runner.join();
    REQUIRE(r.ok());
    CHECK(r.values[0] == 8);
}
