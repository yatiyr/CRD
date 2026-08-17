// CEIR-11a (sec 38): the six residual ceir.task host-execution ops (spawn / continuation / main_thread / worker /
// fiber_wait / group) + their SEQUENTIAL reference semantics (the sec 118 oracle, host-neutral). ⭐ The open-world
// payoff, ASSERTED: the 6a find_token_misuse verifier covers the new TokenProducer/Consumer ops with ZERO verifier edits.
// The sec 32 audio-RT flip fires on a task op (Synchronization). Jobs-backed placement is CEIR-11a stage 3. ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/semantics.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
struct Ops
{
    OpId spawn, cont, main_thread, worker, fiber_wait, group, addi, cst, yield;
    explicit Ops(Context& ctx)
        : spawn(ctx.intern_op("task", "spawn")), cont(ctx.intern_op("task", "continuation")),
          main_thread(ctx.intern_op("task", "main_thread")), worker(ctx.intern_op("task", "worker")),
          fiber_wait(ctx.intern_op("task", "fiber_wait")), group(ctx.intern_op("task", "group")),
          addi(ctx.intern_op("arith", "addi")), cst(ctx.intern_op("arith", "const")),
          yield(ctx.intern_op("core", "yield"))
    {
        (void)task::register_task_ops(ctx);
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)func::register_dialect(ctx);
    }
};
Block* body(Context& ctx, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr) { b = ctx.create_block(0U); m.body()->append(b); }
    return b;
}
Operation* konst(Context& ctx, const Ops& o, Block* b, i64 v)
{
    Operation* const op = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
}
// task.spawn { %c = const v; core.yield %c } -> %token
Operation* mkspawn(Context& ctx, const Ops& o, Block* b, i64 v)
{
    Operation* const s  = ctx.create_operation(o.spawn, {}, 1U, ctx.type_i32(), 1U);
    Block* const     sb = ctx.create_block(0U);
    s->region(0)->append(sb);
    Value* yv[1] = {konst(ctx, o, sb, v)->result(0U)};
    sb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(yv, 1U), 0U));
    b->append(s);
    return s;
}
i64 run_main(Context& ctx, Module& m)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult r = in.invoke(m, "main", {});
    REQUIRE(r.error == exec::ExecError::None);
    REQUIRE(r.values.size() == 1U);
    return r.values[0];
}
} // namespace

TEST_CASE("ceir task: spawn then fiber_wait round-trips a value (sequential reference)", "[ceir][task]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    Operation* const s   = mkspawn(ctx, o, mb, 42);            // %t = task.spawn { yield 42 }
    Value*           tk[1] = {s->result(0U)};
    Operation* const fw  = ctx.create_operation(o.fiber_wait, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32()); // fiber_wait(%t)
    mb->append(fw);
    Value* rv[1] = {fw->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None); // well-formed
    CHECK(run_main(ctx, *m) == 42);
}

TEST_CASE("ceir task: a continuation runs after its antecedent, receiving its value", "[ceir][task]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    Operation* const s = mkspawn(ctx, o, mb, 10);              // %t = task.spawn { yield 10 }
    // %t2 = task.continuation(%t) { ^body(%v): %r = addi %v, 5; yield %r }
    Value*           tk[1] = {s->result(0U)};
    Operation* const cn    = ctx.create_operation(o.cont, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32(), 1U);
    Block* const     cb    = ctx.create_block(1U, ctx.type_i32()); // one block-arg: the antecedent's yielded value
    cn->region(0)->append(cb);
    Value* av[2] = {cb->arg(0U), konst(ctx, o, cb, 5)->result(0U)};
    Operation* const add = ctx.create_operation(o.addi, ConstSpan<Value*>(av, 2U), 1U, ctx.type_i32());
    cb->append(add);
    Value* cy[1] = {add->result(0U)};
    cb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(cy, 1U), 0U));
    mb->append(cn);
    // %r = task.fiber_wait(%t2)
    Value*           t2[1] = {cn->result(0U)};
    Operation* const fw    = ctx.create_operation(o.fiber_wait, ConstSpan<Value*>(t2, 1U), 1U, ctx.type_i32());
    mb->append(fw);
    Value* rv[1] = {fw->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None); // %t consumed by continuation, %t2 by fiber_wait
    CHECK(run_main(ctx, *m) == 15);                                 // 10 + 5, the antecedent's value threaded in
}

TEST_CASE("ceir task: the 6a token verifier covers the new ops with ZERO edits (open-world)", "[ceir][task]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);

    SECTION("a spawned token never consumed is Unconsumed")
    {
        (void)mkspawn(ctx, o, mb, 1); // %t = task.spawn {...}  -- leaked (no fiber_wait / cancel)
        mb->append(func::create_return(ctx, {}));
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::Unconsumed);
    }
    SECTION("a token fiber_wait-ed twice is MultiplyConsumed")
    {
        Operation* const s = mkspawn(ctx, o, mb, 1);
        Value*           tk[1] = {s->result(0U)};
        mb->append(ctx.create_operation(o.fiber_wait, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32()));
        mb->append(ctx.create_operation(o.fiber_wait, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32())); // 2nd use
        mb->append(func::create_return(ctx, {}));
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::MultiplyConsumed);
    }
}

TEST_CASE("ceir task: a task op in an audio-real-time region is a domain violation (the sec 32 flip)", "[ceir][task]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    // core.scope { %t = task.spawn {...}; task.fiber_wait(%t) } tagged AudioRealTime -> spawn's Synchronization is illegal
    Operation* const sc = ctx.create_operation(ctx.intern_op("core", "scope"), {}, 0U, {}, 1U);
    Block* const     scb = ctx.create_block(0U);
    sc->region(0)->append(scb);
    Operation* const s = mkspawn(ctx, o, scb, 1);
    Value*           tk[1] = {s->result(0U)};
    scb->append(ctx.create_operation(o.fiber_wait, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32()));
    scb->append(ctx.create_operation(o.yield, {}, 0U));
    mb->append(sc);
    mb->append(func::create_return(ctx, {}));
    ctx.set_region_exec(sc, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});

    const DomainViolation v = ctx.find_domain_violation(*m);
    CHECK(v.op != nullptr); // a blocking task op in an audio-RT region is forbidden (the 6a flip, task side)
}

namespace
{
struct HookCounts
{
    crd::u32 pre  = 0U;
    crd::u32 post = 0U;
};
void count_pre(const Operation& /*op*/, void* user) { ++static_cast<HookCounts*>(user)->pre; }
void count_post(const Operation& /*op*/, void* user) { ++static_cast<HookCounts*>(user)->post; }
} // namespace

TEST_CASE("ceir task: the sec 112 step hooks fire pre/post per dispatched op (exact counts)", "[ceir][task]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    // @main() -> i32 { %c = const 42; return %c }  -- exactly TWO dispatched ops (const, return).
    Value* rv[1] = {konst(ctx, o, mb, 42)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    HookCounts       counts;
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    in.set_step_hooks(&count_pre, &count_post, &counts);
    const exec::ExecResult r = in.invoke(*m, "main", {});
    REQUIRE(r.error == exec::ExecError::None);
    CHECK(r.values.size() == 1U);
    CHECK(r.values[0] == 42);
    CHECK(counts.pre == 2U);  // ⛔ EXACT (not ≥): const + return
    CHECK(counts.post == 2U); // both dispatched successfully → post fired for each

    // null-default: a fresh interpreter with NO hooks runs identically (zero-cost when unset).
    exec::Interpreter in2(ctx);
    exec::install_builtin_semantics(in2);
    CHECK(in2.invoke(*m, "main", {}).values[0] == 42);
}

// ─────────────────── CEIR-11a stage 4: the DoD gate — the full host subset composes in ONE reference program ───────────────────
namespace
{
struct HookTot
{
    crd::u32 pre  = 0U;
    crd::u32 post = 0U;
};
void tot_pre(const Operation& /*op*/, void* u) { ++static_cast<HookTot*>(u)->pre; }
void tot_post(const Operation& /*op*/, void* u) { ++static_cast<HookTot*>(u)->post; }
} // namespace

TEST_CASE("ceir 11a: the full host subset composes in one reference program", "[ceir][task]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    (void)async::register_async_ops(ctx);
    const OpId muli   = ctx.intern_op("arith", "muli");
    const OpId cmpi   = ctx.intern_op("arith", "cmpi");
    const OpId cif    = ctx.intern_op("core", "if");
    const OpId cfor   = ctx.intern_op("core", "for");
    const OpId statek = ctx.intern_op("core", "state");
    const OpId launch = ctx.intern_op("async", "launch");
    const OpId awaitk = ctx.intern_op("async", "await");
    const OpId callk  = ctx.intern_op("func", "call");
    (void)callk;
    Module* const m = ctx.create_module();

    // @sq(%x) -> i32 { %r = muli(%x,%x); return %r }   (func + arith)
    Operation* const fsq = func::create_func(ctx, *m, "sq", Visibility::Public, 1U, ctx.type_i32());
    body(ctx, *m)->append(fsq);
    Block* const sb = func::func_body_block(fsq);
    Value* mops[2] = {sb->arg(0U), sb->arg(0U)};
    Operation* const mul = ctx.create_operation(muli, ConstSpan<Value*>(mops, 2U), 1U, ctx.type_i32());
    sb->append(mul);
    Value* sr[1] = {mul->result(0U)};
    sb->append(func::create_return(ctx, ConstSpan<Value*>(sr, 1U)));

    // @main() -> i32 { ...spans arith + core(for/if/state) + func(call) + async(launch/await) + task(spawn/fiber_wait)... }
    Operation* const fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);

    // A: core.for(0,3,1) { iv: %acc = state(0, acc+iv) }  -- a state cell latching across the loop block.
    Value* lohilst[3] = {konst(ctx, o, mb, 0)->result(0U), konst(ctx, o, mb, 3)->result(0U),
                         konst(ctx, o, mb, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    mb->append(forop);
    Block* const forb = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(forb);
    Operation* const initk = konst(ctx, o, forb, 0);
    Value* sops[2] = {initk->result(0U), initk->result(0U)};
    Operation* const acc = ctx.create_operation(statek, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    forb->append(acc);
    Value* nx[2] = {acc->result(0U), forb->arg(0U)};
    Operation* const nxo = ctx.create_operation(o.addi, ConstSpan<Value*>(nx, 2U), 1U, ctx.type_i32());
    forb->append(nxo);
    acc->set_operand(1U, nxo->result(0U)); // next = acc + iv

    // B: %t = async.launch { %a = const 4; %b = call sq(%a); yield %b }   (async + func, in-frame -> the call resolves)
    Operation* const l  = ctx.create_operation(launch, {}, 1U, ctx.type_i32(), 1U);
    Block* const     lb = ctx.create_block(0U);
    l->region(0)->append(lb);
    Value* ca[1] = {konst(ctx, o, lb, 4)->result(0U)};
    Operation* const cl = func::create_call(ctx, "sq", ConstSpan<Value*>(ca, 1U), 1U, ctx.type_i32());
    lb->append(cl);
    Value* ly[1] = {cl->result(0U)};
    lb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(ly, 1U), 0U));
    mb->append(l);
    Value* tk[1] = {l->result(0U)};
    Operation* const aw = ctx.create_operation(awaitk, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32());
    mb->append(aw); // %r = 16

    // C: %s = task.spawn { const 5 }; %u = task.fiber_wait(%s)   (task -> 5)
    Operation* const sp = mkspawn(ctx, o, mb, 5);
    Value* st[1] = {sp->result(0U)};
    Operation* const fw = ctx.create_operation(o.fiber_wait, ConstSpan<Value*>(st, 1U), 1U, ctx.type_i32());
    mb->append(fw); // %u = 5

    // D: %cmp = cmpi(sgt, %r, %u); %mx = core.if(%cmp) { yield %r } else { yield %u }  -> 16
    Value* cops[2] = {aw->result(0U), fw->result(0U)};
    Operation* const cmp = ctx.create_operation(cmpi, ConstSpan<Value*>(cops, 2U), 1U, ctx.type_i32());
    ctx.set_attr(cmp, "predicate", ctx.attr_string("sgt"));
    mb->append(cmp);
    Value* ic[1] = {cmp->result(0U)};
    Operation* const iff = ctx.create_operation(cif, ConstSpan<Value*>(ic, 1U), 1U, ctx.type_i32(), 2U);
    Block* const thenb = ctx.create_block(0U);
    Block* const elseb = ctx.create_block(0U);
    iff->region(0)->append(thenb);
    iff->region(1)->append(elseb);
    Value* ty[1] = {aw->result(0U)};
    thenb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(ty, 1U), 0U));
    Value* ey[1] = {fw->result(0U)};
    elseb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(ey, 1U), 0U));
    mb->append(iff);
    // %sum = addi(%mx, %u)   (16 + 5 = 21)
    Value* su[2] = {iff->result(0U), fw->result(0U)};
    Operation* const sum = ctx.create_operation(o.addi, ConstSpan<Value*>(su, 2U), 1U, ctx.type_i32());
    mb->append(sum);
    Value* rv[1] = {sum->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    // token hygiene across dialects (async launch + task spawn producers/consumers in ONE module).
    CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);

    // run through the CORE reference (builtin + async + task) -- the §118 oracle covers the full host subset.
    HookTot           hooks;
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    in.set_step_hooks(&tot_pre, &tot_post, &hooks);
    const exec::ExecResult r = in.invoke(*m, "main", {});
    REQUIRE(r.ok());
    REQUIRE(r.values.size() == 1U);
    CHECK(r.values[0] == 21);       // 16 (sq(4)) via if-then, + 5 (spawn) -- the composed result
    i64 accv = -1;
    REQUIRE(in.cell_value(acc, accv));
    CHECK(accv == 0 + 1 + 2);        // the state cell latched across the 3 loop iterations
    CHECK(hooks.pre == hooks.post);  // every dispatched op succeeded -> post fired for each (§112)
    CHECK(hooks.pre == 29U);         // ⛔ EXACT §112 dispatch count over the whole composition (nested regions + the
                                     // 3-iteration state loop + the sq call + the launch/spawn bodies) — the not-≥ discipline
}
