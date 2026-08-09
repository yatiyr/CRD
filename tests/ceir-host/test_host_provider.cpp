// CEIR-6b: the crd-jobs execution provider (crd-ceir-host). task.parallel_for lowered onto crd::jobs::parallel_for --
// map correctness on the fiber pool, num_jobs-INDEPENDENT results (the 6z determinism seed), the parallel-purity pre-flight,
// the typed errors, and the 6a Synchronization/audio composition. ⛔ The test binary owns the jobs pool (a Catch listener).
// ASCII test names ("ceir host: …" so the -R ceir gate regex catches them).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/ceir/semantics.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
struct HostJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(Catch::TestRunInfo const&) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 4, .frame_alloc_bytes = 16U << 20U});
    }
    void testCaseEnded(Catch::TestCaseStats const&) override { crd::jobs::frame_reset(); }
    void testRunEnded(Catch::TestRunStats const&) override { crd::jobs::shutdown(); }
};

struct Ops
{
    OpId cst, addi, muli, pfor, mr, scope, yield, state, cfor;
    explicit Ops(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          muli(ctx.intern_op("arith", "muli")), pfor(ctx.intern_op("task", "parallel_for")),
          mr(ctx.intern_op("task", "map_reduce")), scope(ctx.intern_op("async", "scope")),
          yield(ctx.intern_op("core", "yield")), state(ctx.intern_op("core", "state")),
          cfor(ctx.intern_op("core", "for"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)task::register_task_ops(ctx);
        (void)async::register_async_ops(ctx);
        (void)func::register_dialect(ctx);
    }
};
Block* body_block(Context& ctx, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = ctx.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
Operation* mkfunc(Context& ctx, Module& m, containers::StringView name, crd::u32 nparams)
{
    Operation* const f = func::create_func(ctx, m, name, Visibility::Public, nparams, ctx.type_i32());
    body_block(ctx, m)->append(f);
    return f;
}
Operation* konst(Context& ctx, const Ops& o, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
Operation* bin(Context& ctx, OpId k, Value* a, Value* b2, Block* b)
{
    Value* ops[2] = {a, b2};
    Operation* const o = ctx.create_operation(k, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(o);
    return o;
}
void yield1(Context& ctx, const Ops& o, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(ctx.create_operation(o.yield, ConstSpan<Value*>(a, 1U), 0U));
}
// A parallel_for(0, n, 1) in `parent`, returning the pf op; the caller fills its body block (which has the iv arg).
Operation* mk_pfor(Context& ctx, const Ops& o, Block* parent, i64 n, Block*& body_out)
{
    Value* lohilst[3] = {konst(ctx, o, parent, 0)->result(0U), konst(ctx, o, parent, n)->result(0U),
                         konst(ctx, o, parent, 1)->result(0U)};
    Operation* const pf = ctx.create_operation(o.pfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    parent->append(pf);
    body_out = ctx.create_block(1U, ctx.type_i32()); // the iv arg
    pf->region(0)->append(body_out);
    return pf;
}
// A map_reduce(0, n, 1, init) in `parent`, returning the op; the caller fills the MAP body (1 arg: iv) + COMBINE body
// (`combine_nargs` args -- 2 for a well-formed acc/elem; other counts exercise the arity pre-flight).
Operation* mk_map_reduce(Context& ctx, const Ops& o, Block* parent, i64 n, i64 init, Block*& map_out, Block*& combine_out,
                         crd::u32 combine_nargs = 2U)
{
    Value* ops4[4] = {konst(ctx, o, parent, 0)->result(0U), konst(ctx, o, parent, n)->result(0U),
                      konst(ctx, o, parent, 1)->result(0U), konst(ctx, o, parent, init)->result(0U)};
    Operation* const mr = ctx.create_operation(o.mr, ConstSpan<Value*>(ops4, 4U), 1U, ctx.type_i32(), 2U);
    parent->append(mr);
    map_out = ctx.create_block(1U, ctx.type_i32()); // the map iv arg
    mr->region(0)->append(map_out);
    combine_out = ctx.create_block(combine_nargs, ctx.type_i32()); // the combine (acc, elem) args
    mr->region(1)->append(combine_out);
    return mr;
}
// An async.scope in `parent`, returning the op; the caller fills its body block (empty args) and yields the scope result.
Operation* mk_scope(Context& ctx, const Ops& o, Block* parent, Block*& body_out)
{
    Operation* const sc = ctx.create_operation(o.scope, {}, 1U, ctx.type_i32(), 1U);
    parent->append(sc);
    body_out = ctx.create_block(0U);
    sc->region(0)->append(body_out);
    return sc;
}
} // namespace
CATCH_REGISTER_LISTENER(HostJobsListener)

TEST_CASE("ceir host: parallel_for maps iv*iv on the pool, num_jobs-independent", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // @main(): parallel_for(0,6,1){ iv: yield iv*iv }
    Module* const    m  = ctx.create_module();
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Block*           body = nullptr;
    Operation* const pf   = mk_pfor(ctx, o, mb, 6, body);
    yield1(ctx, o, body, bin(ctx, o.muli, body->arg(0U), body->arg(0U), body)->result(0U));
    mb->append(func::create_return(ctx, {}));

    const i64 expected[6] = {0, 1, 4, 9, 16, 25};
    for (const crd::u32 nj : {crd::u32{1}, crd::u32{8}}) // ⭐ num_jobs-INDEPENDENT (the 6z determinism seed)
    {
        crd::memory::MallocAllocator palloc;
        host::HostProvider          prov(&palloc, nj);
        const exec::ExecResult      r = prov.execute(ctx, *m, "main", {});
        REQUIRE(r.ok());
        const ConstSpan<i64> out = prov.map_output(pf);
        REQUIRE(out.size() == 6U);
        for (crd::u32 i = 0; i < 6U; ++i) { CHECK(out[i] == expected[i]); }
    }
}

TEST_CASE("ceir host: a parallel_for body may CALL a self-contained func", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // @sq(%x): return x*x. @main(): parallel_for(0,5,1){ iv: %r = call sq(iv); yield %r }
    Module* const    m  = ctx.create_module();
    Operation* const fs = mkfunc(ctx, *m, "sq", 1U);
    Block* const     sb = func::func_body_block(fs);
    Value*           rv[1] = {bin(ctx, o.muli, sb->arg(0U), sb->arg(0U), sb)->result(0U)};
    sb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Block*           body = nullptr;
    Operation* const pf   = mk_pfor(ctx, o, mb, 5, body);
    Value*           ca[1] = {body->arg(0U)};
    Operation* const call  = func::create_call(ctx, "sq", ConstSpan<Value*>(ca, 1U), 1U, ctx.type_i32());
    body->append(call);
    yield1(ctx, o, body, call->result(0U));
    mb->append(func::create_return(ctx, {}));

    crd::memory::MallocAllocator palloc;
    host::HostProvider          prov(&palloc, 4U);
    REQUIRE(prov.execute(ctx, *m, "main", {}).ok());
    const ConstSpan<i64> out = prov.map_output(pf);
    REQUIRE(out.size() == 5U);
    const i64 expected[5] = {0, 1, 4, 9, 16};
    for (crd::u32 i = 0; i < 5U; ++i) { CHECK(out[i] == expected[i]); }
}

TEST_CASE("ceir host: empty range yields an empty map; a non-positive step is BadForStep", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    SECTION("empty range (hi <= lo)")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           body = nullptr;
        Operation* const pf   = mk_pfor(ctx, o, mb, 0, body); // range [0,0) is empty
        yield1(ctx, o, body, konst(ctx, o, body, 7)->result(0U));
        mb->append(func::create_return(ctx, {}));
        crd::memory::MallocAllocator palloc;
        host::HostProvider          prov(&palloc, 4U);
        REQUIRE(prov.execute(ctx, *m, "main", {}).ok());
        CHECK(prov.map_output(pf).size() == 0U);
    }
    SECTION("step 0 is BadForStep")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Value* lohilst[3] = {konst(ctx, o, mb, 0)->result(0U), konst(ctx, o, mb, 5)->result(0U),
                             konst(ctx, o, mb, 0)->result(0U)}; // step 0
        Operation* const pf = ctx.create_operation(o.pfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
        mb->append(pf);
        Block* const body = ctx.create_block(1U, ctx.type_i32());
        pf->region(0)->append(body);
        yield1(ctx, o, body, konst(ctx, o, body, 1)->result(0U));
        mb->append(func::create_return(ctx, {}));
        crd::memory::MallocAllocator palloc;
        host::HostProvider          prov(&palloc, 4U);
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::BadForStep);
    }
    SECTION("an empty-range map_reduce returns init (the fold identity) -- and runs BARE, outside any async.scope")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           mapb = nullptr;
        Block*           cb   = nullptr;
        Operation* const mr   = mk_map_reduce(ctx, o, mb, /*n*/ 0, /*init*/ 42, mapb, cb); // range [0,0) is empty
        yield1(ctx, o, mapb, bin(ctx, o.muli, mapb->arg(0U), mapb->arg(0U), mapb)->result(0U));
        Value* const acc31 = bin(ctx, o.muli, cb->arg(0U), konst(ctx, o, cb, 31)->result(0U), cb)->result(0U);
        yield1(ctx, o, cb, bin(ctx, o.addi, acc31, cb->arg(1U), cb)->result(0U)); // a well-formed combine (pre-flight still checks it)
        Value* rv[1] = {mr->result(0U)};
        mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
        crd::memory::MallocAllocator palloc;
        host::HostProvider          prov(&palloc, 4U);
        const exec::ExecResult      r = prov.execute(ctx, *m, "main", {}); // the happy path with NO scope wrapper
        REQUIRE(r.ok());
        REQUIRE(r.values.size() == 1U);
        CHECK(r.values[0] == 42);                  // init, unfolded — the empty-range identity
        CHECK(prov.map_output(mr).size() == 0U);   // an empty intermediate map
    }
}

TEST_CASE("ceir host: a plain reference interpreter has no task semantics (NoSemantics)", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Operation* const             fm = mkfunc(ctx, *m, "main", 0U);
    Block* const                 mb = func::func_body_block(fm);
    Block*                       body = nullptr;
    (void)mk_pfor(ctx, o, mb, 3, body);
    yield1(ctx, o, body, konst(ctx, o, body, 1)->result(0U));
    mb->append(func::create_return(ctx, {}));
    // the crd-ceir reference interpreter installs NO task semantics -> parallel_for is a typed NoSemantics.
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    CHECK(in.invoke(*m, "main", {}).error == exec::ExecError::NoSemantics);
}

TEST_CASE("ceir host: the parallel-purity pre-flight", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    crd::memory::MallocAllocator palloc;
    host::HostProvider          prov(&palloc, 4U);

    SECTION("a stateful body is ParallelBodyStateful")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           body = nullptr;
        (void)mk_pfor(ctx, o, mb, 4, body);
        Value* sops[2] = {konst(ctx, o, body, 0)->result(0U), konst(ctx, o, body, 0)->result(0U)};
        Operation* const cell = ctx.create_operation(o.state, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
        body->append(cell);
        cell->set_operand(1U, bin(ctx, o.addi, cell->result(0U), body->arg(0U), body)->result(0U));
        yield1(ctx, o, body, cell->result(0U));
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::ParallelBodyStateful);
    }
    SECTION("a stateful CALLEE is ParallelBodyStateful (transitive)")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fc = mkfunc(ctx, *m, "counter", 1U);
        Block* const     cb = func::func_body_block(fc);
        Value*           sops[2] = {konst(ctx, o, cb, 0)->result(0U), konst(ctx, o, cb, 0)->result(0U)};
        Operation* const cell = ctx.create_operation(o.state, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
        cb->append(cell);
        cell->set_operand(1U, bin(ctx, o.addi, cell->result(0U), cb->arg(0U), cb)->result(0U));
        Value* cr[1] = {cell->result(0U)};
        cb->append(func::create_return(ctx, ConstSpan<Value*>(cr, 1U)));
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           body = nullptr;
        (void)mk_pfor(ctx, o, mb, 4, body);
        Value*           ca[1] = {body->arg(0U)};
        Operation* const call  = func::create_call(ctx, "counter", ConstSpan<Value*>(ca, 1U), 1U, ctx.type_i32());
        body->append(call);
        yield1(ctx, o, body, call->result(0U));
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::ParallelBodyStateful);
    }
    SECTION("an unresolved callee is UnresolvedCall")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           body = nullptr;
        (void)mk_pfor(ctx, o, mb, 4, body);
        Value*           ca[1] = {body->arg(0U)};
        body->append(func::create_call(ctx, "ghost", ConstSpan<Value*>(ca, 1U), 1U, ctx.type_i32()));
        yield1(ctx, o, body, konst(ctx, o, body, 0)->result(0U));
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::UnresolvedCall);
    }
    SECTION("a body that does not yield exactly one value is ParallelYieldArity")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           body = nullptr;
        (void)mk_pfor(ctx, o, mb, 4, body);
        (void)konst(ctx, o, body, 3); // no core.yield terminator at all -> yields 0, not 1
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::ParallelYieldArity);
    }
}

TEST_CASE("ceir host: a runaway parallel body is FuelExhausted, not a hang", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // body: for(0, 100000, 1) { } ; yield 0  -- burns fuel per iteration, exceeding a tiny sub-budget.
    Module* const    m  = ctx.create_module();
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Block*           body = nullptr;
    Operation* const pf   = mk_pfor(ctx, o, mb, 3, body);
    Value* lohilst[3] = {konst(ctx, o, body, 0)->result(0U), konst(ctx, o, body, 100000)->result(0U),
                         konst(ctx, o, body, 1)->result(0U)};
    Operation* const spin = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    body->append(spin);
    spin->region(0)->append(ctx.create_block(1U, ctx.type_i32())); // empty for-body
    yield1(ctx, o, body, konst(ctx, o, body, 0)->result(0U));
    mb->append(func::create_return(ctx, {}));

    crd::memory::MallocAllocator palloc;
    host::HostProvider          prov(&palloc, /*num_jobs*/ 4U, /*sub_fuel*/ 1000U); // small per-index budget
    const exec::ExecResult      r = prov.execute(ctx, *m, "main", {});
    CHECK(r.error == exec::ExecError::FuelExhausted);
    CHECK(r.op == pf); // the error points at the parallel_for (first-in-index-order)
}

TEST_CASE("ceir host: num_jobs > count clamps; advertises; a captured body is UndefinedValue", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    crd::memory::MallocAllocator palloc;
    host::HostProvider          prov(&palloc, /*num_jobs*/ 8U); // more jobs than items -> jobs clamps internally

    SECTION("num_jobs (8) > count (3) still maps correctly")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           body = nullptr;
        Operation* const pf   = mk_pfor(ctx, o, mb, 3, body);
        yield1(ctx, o, body, bin(ctx, o.muli, body->arg(0U), body->arg(0U), body)->result(0U));
        mb->append(func::create_return(ctx, {}));
        REQUIRE(prov.execute(ctx, *m, "main", {}).ok());
        const ConstSpan<i64> out = prov.map_output(pf);
        REQUIRE(out.size() == 3U);
        CHECK(out[0] == 0);
        CHECK(out[1] == 1);
        CHECK(out[2] == 4);
    }
    SECTION("advertises() reports the provider's capability")
    {
        CHECK(prov.advertises(ctx, ctx.intern_op("task", "parallel_for")));
        CHECK(prov.advertises(ctx, ctx.intern_op("task", "map_reduce")));
        CHECK_FALSE(prov.advertises(ctx, ctx.intern_op("arith", "addi")));
    }
    SECTION("a body that CAPTURES an outer value is UndefinedValue (self-contained-body contract)")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Operation* const outer = konst(ctx, o, mb, 10); // defined OUTSIDE the parallel_for body
        Block*           body  = nullptr;
        (void)mk_pfor(ctx, o, mb, 4, body);
        yield1(ctx, o, body, bin(ctx, o.addi, body->arg(0U), outer->result(0U), body)->result(0U)); // captures %outer
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::UndefinedValue);
    }
}

TEST_CASE("ceir host: parallel_for is a Synchronization barrier, illegal in an audio region (6a flip)", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   scope = ctx.intern_op("core", "scope");
    Module* const                m  = ctx.create_module();
    Block* const                 b  = body_block(ctx, *m);
    Operation* const             sc = ctx.create_operation(scope, {}, 0U, {}, 1U);
    ctx.set_region_exec(sc, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
    b->append(sc);
    Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    Block* body = nullptr;
    (void)mk_pfor(ctx, o, rb, 4, body); // a task.parallel_for (Synchronization) inside the audio region
    yield1(ctx, o, body, konst(ctx, o, body, 0)->result(0U));
    const DomainViolation v = ctx.find_domain_violation(*m);
    REQUIRE(v.op != nullptr);
    CHECK(v.effect == EffectFamily::Synchronization);
}

TEST_CASE("ceir host: RealtimeClass maps to a jobs dispatch Priority (sec 32)", "[ceir][host]")
{
    CHECK(host::priority_for(RealtimeClass::AudioRealTime) == crd::jobs::Priority::High);
    CHECK(host::priority_for(RealtimeClass::FrameCritical) == crd::jobs::Priority::High);
    CHECK(host::priority_for(RealtimeClass::Background) == crd::jobs::Priority::Low);
    CHECK(host::priority_for(RealtimeClass::Offline) == crd::jobs::Priority::Low);
    CHECK(host::priority_for(RealtimeClass::Unspecified) == crd::jobs::Priority::Normal);
    CHECK(host::priority_for(RealtimeClass::Throughput) == crd::jobs::Priority::Normal);
}

TEST_CASE("ceir host: request_cancel stops a running parallel_for (Cancelled, not FuelExhausted)", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // body: for(0, 1000000, 1) { } ; yield 0  -- a long spin; with HUGE sub-fuel only cancellation can stop it.
    Module* const    m  = ctx.create_module();
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Block*           body = nullptr;
    (void)mk_pfor(ctx, o, mb, 4, body);
    Value* lohilst[3] = {konst(ctx, o, body, 0)->result(0U), konst(ctx, o, body, 1000000)->result(0U),
                         konst(ctx, o, body, 1)->result(0U)};
    Operation* const spin = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    body->append(spin);
    spin->region(0)->append(ctx.create_block(1U, ctx.type_i32()));
    yield1(ctx, o, body, konst(ctx, o, body, 0)->result(0U));
    mb->append(func::create_return(ctx, {}));

    crd::memory::MallocAllocator palloc;
    host::HostProvider          prov(&palloc, /*num_jobs*/ 4U, /*sub_fuel*/ crd::u64{1} << 40U); // huge budget
    prov.request_cancel(); // §30 cooperative cancel, pre-requested -> the ranges observe it in their step loop
    CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::Cancelled);
}

// ⭐⭐ THE BAND-6 GATE (sec 30/sec 31): a parallel map-reduce AUTHORED as CEIR (map on the fiber pool, fixed index-order
// fold), wrapped in an async.scope (the scope x provider COMPOSITION SEAM -- async.scope had only ever run in-core), its
// scalar result BIT-IDENTICAL across EVERY num_jobs in {1..16}. The combine is NON-associative (acc*31 + elem) so the
// identity PROVES the fold order is fixed (index order) regardless of the parallel split -- and it is checked against an
// INDEPENDENT wrapping-u64 reference (the bit-exact-blind scar: cross-config identity alone would pass 16 identical wrongs).
TEST_CASE("ceir host: map_reduce fixed-order fold is bit-identical across num_jobs 1..16 (the band-6 gate)", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    constexpr i64                elem_count = 37; // a PRIME >= 16 -> every num_jobs in {1..16} splits the range raggedly (no even divide)

    // @main() -> i64: %s = async.scope { %r = map_reduce(0,elem_count,1, init=0) map{iv: iv*iv} combine{acc,elem: acc*31 + elem}; yield %r }; return %s
    Module* const    m  = ctx.create_module();
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Block*           sbody = nullptr;
    Operation* const sc    = mk_scope(ctx, o, mb, sbody);
    Block*           mapb = nullptr;
    Block*           cb   = nullptr;
    Operation* const mr   = mk_map_reduce(ctx, o, sbody, elem_count, /*init*/ 0, mapb, cb);
    yield1(ctx, o, mapb, bin(ctx, o.muli, mapb->arg(0U), mapb->arg(0U), mapb)->result(0U)); // map: iv*iv
    Value* const acc31 = bin(ctx, o.muli, cb->arg(0U), konst(ctx, o, cb, 31)->result(0U), cb)->result(0U); // acc*31
    yield1(ctx, o, cb, bin(ctx, o.addi, acc31, cb->arg(1U), cb)->result(0U)); // combine: acc*31 + elem (NON-associative)
    yield1(ctx, o, sbody, mr->result(0U)); // the scope forwards the reduced value
    Value* rv[1] = {sc->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    // INDEPENDENT reference: the same fold in WRAPPING u64 (the executor's i64 wraps identically) -- NOT self-comparison.
    crd::u64 ref = 0;
    for (i64 i = 0; i < elem_count; ++i) { ref = (ref * 31ULL) + (static_cast<crd::u64>(i) * static_cast<crd::u64>(i)); }
    const i64 expected = static_cast<i64>(ref);

    crd::memory::MallocAllocator pin_root;
    containers::Array<crd::u8>   first(&pin_root); // num_jobs==1's byte-pinned result (the reference bytes)
    for (crd::u32 nj = 1U; nj <= 16U; ++nj)
    {
        crd::memory::MallocAllocator palloc;
        host::HostProvider          prov(&palloc, nj); // ⭐ the ONLY knob that changes -- the result must not
        const exec::ExecResult      r = prov.execute(ctx, *m, "main", {});
        REQUIRE(r.ok());
        REQUIRE(r.values.size() == 1U);
        CHECK(r.values[0] == expected); // vs the independent fold
        const containers::Array<crd::u8> pinned =
            exec::pin_values(ConstSpan<i64>(r.values.data(), r.values.size()), &palloc);
        if (nj == 1U)
        {
            for (crd::u32 b = 0; b < static_cast<crd::u32>(pinned.size()); ++b) { first.push_back(pinned[b]); }
        }
        else
        {
            REQUIRE(pinned.size() == first.size());
            for (crd::u32 b = 0; b < static_cast<crd::u32>(pinned.size()); ++b) { CHECK(pinned[b] == first[b]); } // byte-identical
        }
    }
}

TEST_CASE("ceir host: map_reduce pre-flight -- the combine must be state-free and 2-arg", "[ceir][host]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    crd::memory::MallocAllocator palloc;
    host::HostProvider          prov(&palloc, 4U);

    SECTION("a stateful combine is ParallelBodyStateful")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           mapb = nullptr;
        Block*           cb   = nullptr;
        (void)mk_map_reduce(ctx, o, mb, 5, 0, mapb, cb);
        yield1(ctx, o, mapb, bin(ctx, o.muli, mapb->arg(0U), mapb->arg(0U), mapb)->result(0U));
        Value* sops[2] = {konst(ctx, o, cb, 0)->result(0U), konst(ctx, o, cb, 0)->result(0U)};
        Operation* const cell = ctx.create_operation(o.state, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
        cb->append(cell); // a §20 StateEdge cell in the combine -> the fold would depend on the split
        cell->set_operand(1U, bin(ctx, o.addi, cell->result(0U), cb->arg(1U), cb)->result(0U));
        yield1(ctx, o, cb, cell->result(0U));
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::ParallelBodyStateful);
    }
    SECTION("a combine with the wrong arg count is BadArity")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           mapb = nullptr;
        Block*           cb   = nullptr;
        (void)mk_map_reduce(ctx, o, mb, 5, 0, mapb, cb, /*combine_nargs*/ 1U); // 1 arg, not (acc, elem)
        yield1(ctx, o, mapb, bin(ctx, o.muli, mapb->arg(0U), mapb->arg(0U), mapb)->result(0U));
        yield1(ctx, o, cb, cb->arg(0U));
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::BadArity);
    }
    SECTION("a combine that does not yield exactly one value is ParallelYieldArity")
    {
        Module* const    m  = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Block*           mapb = nullptr;
        Block*           cb   = nullptr;
        (void)mk_map_reduce(ctx, o, mb, 5, 0, mapb, cb);
        yield1(ctx, o, mapb, bin(ctx, o.muli, mapb->arg(0U), mapb->arg(0U), mapb)->result(0U));
        (void)konst(ctx, o, cb, 3); // no core.yield terminator -> the combine yields 0, not 1
        mb->append(func::create_return(ctx, {}));
        CHECK(prov.execute(ctx, *m, "main", {}).error == exec::ExecError::ParallelYieldArity);
    }
}
