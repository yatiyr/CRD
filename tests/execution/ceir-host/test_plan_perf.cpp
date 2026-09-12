// CEIR-11c part 2 — the crd-perf BRIDGE adapter for the compiled-plan profiling seam. Verifies the per-Op-class
// dispatch counts + the plan-shape stats + (perf-ON) the published crd-perf counters + the adapter's parent-pause
// state machine on both a nesting run and an ERRORING run. ⛔ Self-time is asserted only STRUCTURALLY (non-negative +
// the run took measurable time on a LOOP-bearing program, granularity-proof) — NEVER a quantitative duration (the
// fps-median / GPU-timing flake scar; a 3-op program's ns spans can round to 0 on a 100ns-granularity clock). ASCII names.

#include <crd/ceir/host/plan_perf.hpp>

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/plan.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#if CRD_PERF_ENABLED
#include <crd/perf/perf.hpp> // init/shutdown + counter readback
#endif

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
Operation* konst(Context& ctx, OpId cst, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
} // namespace

TEST_CASE("ceir 11c host: the perf bridge profiles a straight-line plan (exact dispatch counts + shape stats)",
          "[ceir][plan-perf]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const OpId                   cst  = ctx.intern_op("arith", "const");
    const OpId                   addi = ctx.intern_op("arith", "addi");
    (void)arith::register_arith_ops(ctx);
    (void)func::register_dialect(ctx);
    // @main() -> i32 { %a = const 5; %b = const 3; return addi(a, b) } -> 8; 3 DISPATCHED ops (return is a terminator).
    Module* const    m  = ctx.create_module();
    Operation* const fm = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fm);
    Block* const     mb = func::func_body_block(fm);
    Operation* const a  = konst(ctx, cst, mb, 5);
    Operation* const b  = konst(ctx, cst, mb, 3);
    Value* ops[2] = {a->result(0U), b->result(0U)};
    Operation* const c = ctx.create_operation(addi, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    mb->append(c);
    Value* rv[1] = {c->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

#if CRD_PERF_ENABLED
    crd::perf::init({});
#endif
    const plan::CompileResult cr = host::profiled_compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    CHECK(cr.stats.num_funcs == 1U);
    CHECK(cr.stats.num_instrs == 3U); // 2 const + 1 addi
    CHECK(cr.stats.num_seqs == 1U);
    CHECK(cr.stats.num_cells == 0U);
    CHECK(cr.stats.num_maps == 0U);

    host::PlanProfile     prof;
    const plan::RunResult r = host::profiled_run(cr.plan, ConstSpan<i64>(), &root, prof);
    REQUIRE(r.ok());
    CHECK(r.values[0] == 8);
    CHECK(prof.total_dispatch == 3U);
    CHECK(prof.dispatch[static_cast<u8>(plan::Op::ConstI)] == 2U);
    CHECK(prof.dispatch[static_cast<u8>(plan::Op::AddI)] == 1U);
    CHECK(prof.depth_overflow == 0U);
    for (crd::u32 i = 0; i < host::PlanProfile::kMaxOps; ++i) { CHECK(prof.self_s[i] >= 0.0); } // non-negative (monotonic)

#if CRD_PERF_ENABLED
    // the published counters read back (same name -> same interned CounterId -> same value).
    CHECK(crd::perf::counter_current_i64(crd::perf::register_counter_i64("ceir.plan.compile.funcs")) == 1);
    CHECK(crd::perf::counter_current_i64(crd::perf::register_counter_i64("ceir.plan.dispatch_total")) == 3);
    crd::perf::shutdown();
#endif
}

TEST_CASE("ceir 11c host: the perf bridge profiles a LOOP (nesting + granularity-proof self-time)", "[ceir][plan-perf]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const OpId                   cst  = ctx.intern_op("arith", "const");
    const OpId                   muli = ctx.intern_op("arith", "muli");
    const OpId                   cfor = ctx.intern_op("core", "for");
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)func::register_dialect(ctx);
    // @main(){ for(0,1000,1){ iv: muli(iv, iv) }; return const 0 } -- 1000 MulI dispatches nested under 1 For.
    Module* const    m  = ctx.create_module();
    Operation* const fm = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fm);
    Block* const mb = func::func_body_block(fm);
    Value* lohi[3] = {konst(ctx, cst, mb, 0)->result(0U), konst(ctx, cst, mb, 1000)->result(0U),
                      konst(ctx, cst, mb, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(cfor, ConstSpan<Value*>(lohi, 3U), 0U, {}, 1U);
    mb->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(body);
    Value* mops[2] = {body->arg(0U), body->arg(0U)};
    Operation* const mul = ctx.create_operation(muli, ConstSpan<Value*>(mops, 2U), 1U, ctx.type_i32());
    body->append(mul);
    Value* yv[1] = {mul->result(0U)};
    body->append(ctx.create_operation(ctx.intern_op("core", "yield"), ConstSpan<Value*>(yv, 1U), 0U));
    Value* rv[1] = {konst(ctx, cst, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    const plan::CompileResult cr = host::profiled_compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    host::PlanProfile     prof;
    const plan::RunResult r = host::profiled_run(cr.plan, ConstSpan<i64>(), &root, prof);
    REQUIRE(r.ok());
    CHECK(prof.dispatch[static_cast<u8>(plan::Op::For)] == 1U);
    CHECK(prof.dispatch[static_cast<u8>(plan::Op::MulI)] == 1000U);
    CHECK(prof.dispatch[static_cast<u8>(plan::Op::ConstI)] == 4U); // lo, hi, step, return-value
    CHECK(prof.total_dispatch == 1005U);
    CHECK(prof.max_depth >= 2U);      // the muli runs one level under the For (the adapter's first real nesting)
    CHECK(prof.depth_overflow == 0U);
    // STRUCTURAL self-time: a thousand dispatch spans -> µs-scale, granularity-proof on ANY clock (⛔ never quantitative).
    crd::f64 total_self = 0.0;
    for (crd::u32 i = 0; i < host::PlanProfile::kMaxOps; ++i)
    {
        CHECK(prof.self_s[i] >= 0.0);
        total_self += prof.self_s[i];
    }
    CHECK(total_self > 0.0); // the run measurably took time (structural, NOT a quantitative duration)
}

TEST_CASE("ceir 11c host: the perf bridge survives an ERRORING run (adapter parent-pause state, pre>post)", "[ceir][plan-perf]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const OpId                   cst = ctx.intern_op("arith", "const");
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)func::register_dialect(ctx);
    (void)async::register_async_ops(ctx);
    // @main(){ %r = await(const 99); return r } -> BadToken: the Await case returns BEFORE post fires (pre>post). The
    // adapter's parent-pause state (stack/mark/banking) must not crash or over-flow on the unbalanced stream.
    Module* const    m  = ctx.create_module();
    Operation* const fm = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fm);
    Block* const mb = func::func_body_block(fm);
    Value* aw[1] = {konst(ctx, cst, mb, 99)->result(0U)};
    Operation* const av = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(av);
    Value* rv[1] = {av->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    const plan::CompileResult cr = host::profiled_compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    host::PlanProfile     prof;
    const plan::RunResult r = host::profiled_run(cr.plan, ConstSpan<i64>(), &root, prof);
    CHECK(r.error == plan::RunError::BadToken); // the run errored
    CHECK(prof.total_dispatch >= 1U);           // at least the const + the await pre fired
    CHECK(prof.depth_overflow == 0U);           // shallow — no overflow despite the unbalanced pre>post
}
