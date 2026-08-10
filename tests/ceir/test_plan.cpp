// CEIR-11b (Â§84 Â· Â§119 Â· Â§153): the COMPILED execution tier â€” a plan compiler (Module â†’ dense CompiledPlan) + executor +
// the DIFFERENTIAL harness (reference interpreter vs compiled plan, pin_values byte-compare). Stage 2 = STRAIGHT-LINE
// arith (const/addi/muli/cmpi) + func.return. â›” The compiled thunks are INDEPENDENT of the reference EvalFns (the
// differential's value IS that independence â€” a shared-code oracle would be bit-exact-blind). ASCII test names.

#include <crd/ceir/binary.hpp> // 4c/5: serialize/deserialize for the Â§121 plan-layer twin
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/plan.hpp>

#include "corpus.hpp" // CEIR-11b stage 5: the SHARED corpus builder (5z/6z/async/tasks/composing)

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct Ops
{
    OpId cst, addi, muli, cmpi;
    explicit Ops(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          muli(ctx.intern_op("arith", "muli")), cmpi(ctx.intern_op("arith", "cmpi"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)async::register_async_ops(ctx); // 4b: async/task dialects (launch/await/join/race/cancel + the six task ops)
        (void)task::register_task_ops(ctx);
    }
};
Operation* konst(Context& ctx, const Ops& o, Block* b, i64 v)
{
    Operation* const op = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
}
Operation* bin(Context& ctx, OpId k, Value* a, Value* b2, Block* b)
{
    Value* ops[2] = {a, b2};
    Operation* const op = ctx.create_operation(k, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op;
}
// â­ THE DIFFERENTIAL: run @entry(args) through the reference interpreter AND the compiled plan; assert pin_values
// byte-identical; return the (shared) result values.
Array<i64> differential(Context& ctx, Module& m, StringView entry, ConstSpan<i64> args, memory::IAllocator* alloc)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in); // 4b: async/task run on SEPARATE installers (the yield-store dialects)
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(m, entry, args);
    REQUIRE(ref.ok());

    const plan::CompileResult cr = plan::compile(ctx, m, entry, alloc);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, args, alloc);
    REQUIRE(got.ok());

    const Array<crd::u8> a = exec::pin_values(ConstSpan<i64>(ref.values.data(), ref.values.size()), alloc);
    const Array<crd::u8> b = exec::pin_values(ConstSpan<i64>(got.values.data(), got.values.size()), alloc);
    REQUIRE(a.size() == b.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(a.size()); ++i) { CHECK(a[i] == b[i]); } // byte-identical
    Array<i64> out(alloc);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(got.values.size()); ++i) { out.push_back(got.values[i]); }
    return out;
}
} // namespace

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on straight-line arith", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(%x) -> i32 { %sq = muli(%x,%x); %s = addi(%sq, const 3); return %s }
    Operation* const fm = func::create_func(ctx, *m, "main", Visibility::Public, 1U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fm);
    Block* const mb = func::func_body_block(fm);
    Operation* const sq = bin(ctx, o.muli, mb->arg(0U), mb->arg(0U), mb);
    Operation* const s  = bin(ctx, o.addi, sq->result(0U), konst(ctx, o, mb, 3)->result(0U), mb);
    Value* rv[1] = {s->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    i64 x5[1] = {5};
    const Array<i64> r = differential(ctx, *m, "main", ConstSpan<i64>(x5, 1U), &root);
    REQUIRE(r.size() == 1U);
    CHECK(r[0] == 5 * 5 + 3); // 28 â€” both engines agree AND it's the right value
}

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on cmpi predicates", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @cmp(%x) -> i32 { %c = cmpi(sgt, %x, const 10); return %c }
    Operation* const fm = func::create_func(ctx, *m, "cmp", Visibility::Public, 1U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fm);
    Block* const mb = func::func_body_block(fm);
    Operation* const c = bin(ctx, o.cmpi, mb->arg(0U), konst(ctx, o, mb, 10)->result(0U), mb);
    ctx.set_attr(c, "predicate", ctx.attr_string("sgt"));
    Value* rv[1] = {c->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    i64 lo[1] = {5};
    CHECK(differential(ctx, *m, "cmp", ConstSpan<i64>(lo, 1U), &root)[0] == 0);  // 5 > 10 -> 0
    i64 hi[1] = {20};
    CHECK(differential(ctx, *m, "cmp", ConstSpan<i64>(hi, 1U), &root)[0] == 1);  // 20 > 10 -> 1
}

TEST_CASE("ceir 11b: the compiler REJECTS a not-yet-supported op with a typed CompileError", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main() -> i32 { core.foreach { ... }; return const 0 } -- core.foreach has NO compiled semantics (no collection
    // value domain until types land, CEIR-6; the reference intentionally does not install it either) -> UnsupportedOp.
    const OpId feach = ctx.intern_op("core", "foreach");
    Operation* const fm = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fm);
    Block* const mb = func::func_body_block(fm);
    Operation* const l = ctx.create_operation(feach, {}, 0U, {}, 1U);
    l->region(0)->append(ctx.create_block(1U, ctx.type_i32()));
    mb->append(l);
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    CHECK_FALSE(cr.ok());
    CHECK(cr.error == plan::CompileError::UnsupportedOp); // a legitimate tier difference (the differential covers programs that compile)

    // NoEntry: an absent entry name.
    const plan::CompileResult ne = plan::compile(ctx, *m, "nope", &root);
    CHECK(ne.error == plan::CompileError::NoEntry);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ CEIR-11b stage 3a: control flow (if / for / while / switch) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
namespace
{
Operation* mkfunc(Context& ctx, Module& m, StringView name, crd::u32 nparams)
{
    Operation* const f = func::create_func(ctx, m, name, Visibility::Public, nparams, ctx.type_i32());
    if (m.body()->first_block() == nullptr) { m.body()->append(ctx.create_block(0U)); }
    m.body()->first_block()->append(f);
    return f;
}
void yield1(Context& ctx, Block* b, Value* v)
{
    const OpId y = ctx.intern_op("core", "yield");
    Value* a[1] = {v};
    b->append(ctx.create_operation(y, ConstSpan<Value*>(a, 1U), 0U));
}
} // namespace

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on core.if branch forwarding", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   cif = ctx.intern_op("core", "if");
    Module* const                m = ctx.create_module();
    // @sel(%x) -> i32 { %c = cmpi(sgt, %x, 5); %r = if(%c){ yield 100 } else { yield 200 }; return %r }
    Operation* const fm = mkfunc(ctx, *m, "sel", 1U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const c  = bin(ctx, o.cmpi, mb->arg(0U), konst(ctx, o, mb, 5)->result(0U), mb);
    ctx.set_attr(c, "predicate", ctx.attr_string("sgt"));
    Value* ic[1] = {c->result(0U)};
    Operation* const iff = ctx.create_operation(cif, ConstSpan<Value*>(ic, 1U), 1U, ctx.type_i32(), 2U);
    Block* const thenb = ctx.create_block(0U);
    Block* const elseb = ctx.create_block(0U);
    iff->region(0)->append(thenb);
    iff->region(1)->append(elseb);
    yield1(ctx, thenb, konst(ctx, o, thenb, 100)->result(0U));
    yield1(ctx, elseb, konst(ctx, o, elseb, 200)->result(0U));
    mb->append(iff);
    Value* rv[1] = {iff->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    i64 hi[1] = {10};
    CHECK(differential(ctx, *m, "sel", ConstSpan<i64>(hi, 1U), &root)[0] == 100); // 10 > 5 -> then
    i64 lo[1] = {3};
    CHECK(differential(ctx, *m, "sel", ConstSpan<i64>(lo, 1U), &root)[0] == 200); // 3 > 5 false -> else
}

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on a bounded core.for (statement)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   cfor = ctx.intern_op("core", "for");
    Module* const                m = ctx.create_module();
    // @loop() -> i32 { for(0, 3, 1){ iv: <empty> }; return 42 }  (a for with a pure body is a no-op statement)
    Operation* const fm = mkfunc(ctx, *m, "loop", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* lohilst[3] = {konst(ctx, o, mb, 0)->result(0U), konst(ctx, o, mb, 3)->result(0U),
                         konst(ctx, o, mb, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    forop->region(0)->append(ctx.create_block(1U, ctx.type_i32())); // body: 1 arg (iv), empty
    mb->append(forop);
    Value* rv[1] = {konst(ctx, o, mb, 42)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "loop", ConstSpan<i64>(), &root)[0] == 42); // the for runs 3Ã— (no-op); returns 42
}

TEST_CASE("ceir 11b: a runtime RunError (BadForStep / SelectorOutOfRange) agrees with the reference", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    SECTION("for step 0 -> BadForStep in both engines")
    {
        const OpId    cfor = ctx.intern_op("core", "for");
        Module* const m    = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "bad", 0U);
        Block* const     mb = func::func_body_block(fm);
        Value* lhs[3] = {konst(ctx, o, mb, 0)->result(0U), konst(ctx, o, mb, 3)->result(0U),
                         konst(ctx, o, mb, 0)->result(0U)}; // step 0
        Operation* const forop = ctx.create_operation(cfor, ConstSpan<Value*>(lhs, 3U), 0U, {}, 1U);
        forop->region(0)->append(ctx.create_block(1U, ctx.type_i32()));
        mb->append(forop);
        mb->append(func::create_return(ctx, {}));

        exec::Interpreter in(ctx);
        exec::install_builtin_semantics(in);
        CHECK(in.invoke(*m, "bad", {}).error == exec::ExecError::BadForStep); // reference errors
        const plan::CompileResult cr = plan::compile(ctx, *m, "bad", &root);
        REQUIRE(cr.ok());
        CHECK(plan::run(cr.plan, ConstSpan<i64>(), &root).error == plan::RunError::BadForStep); // compiled agrees
    }
    SECTION("switch selector out of range -> SelectorOutOfRange in both engines")
    {
        const OpId    csw = ctx.intern_op("core", "switch");
        Module* const m   = ctx.create_module();
        Operation* const fm = mkfunc(ctx, *m, "sw", 0U);
        Block* const     mb = func::func_body_block(fm);
        Value* sel[1] = {konst(ctx, o, mb, 5)->result(0U)}; // out of range (2 regions)
        Operation* const swop = ctx.create_operation(csw, ConstSpan<Value*>(sel, 1U), 0U, {}, 2U);
        swop->region(0)->append(ctx.create_block(0U));
        swop->region(1)->append(ctx.create_block(0U));
        mb->append(swop);
        mb->append(func::create_return(ctx, {}));

        exec::Interpreter in(ctx);
        exec::install_builtin_semantics(in);
        CHECK(in.invoke(*m, "sw", {}).error == exec::ExecError::SelectorOutOfRange);
        const plan::CompileResult cr = plan::compile(ctx, *m, "sw", &root);
        REQUIRE(cr.ok());
        CHECK(plan::run(cr.plan, ConstSpan<i64>(), &root).error == plan::RunError::SelectorOutOfRange);
    }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ CEIR-11b stage 3b: Â§20 state (dense cells + latch lists) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on a state cell read (returns init)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   statek = ctx.intern_op("core", "state");
    Module* const                m = ctx.create_module();
    // @main() -> i32 { %s = state(7, 0); return %s }  -- a single cell_read returns the init (7).
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* sops[2] = {konst(ctx, o, mb, 7)->result(0U), konst(ctx, o, mb, 0)->result(0U)};
    Operation* const s = ctx.create_operation(statek, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    mb->append(s);
    Value* rv[1] = {s->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 7); // cell_read returns init, both engines agree
}

TEST_CASE("ceir 11b: a for+state accumulator's LATCHED cell agrees with the reference (cell inspection)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   statek = ctx.intern_op("core", "state");
    const OpId                   cfor   = ctx.intern_op("core", "for");
    Module* const                m = ctx.create_module();
    // @main() -> i32 { for(0,3,1){ iv: %acc = state(0, acc+iv) }; return 0 }  -- acc latches 0+1+2 = 3.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
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
    acc->set_operand(1U, bin(ctx, o.addi, acc->result(0U), forb->arg(0U), forb)->result(0U)); // next = acc + iv
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    // reference: the accumulator cell latches across 3 iterations.
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    i64 ref_acc = -1;
    REQUIRE(in.cell_value(acc, ref_acc));
    CHECK(ref_acc == 0 + 1 + 2); // 3

    // compiled: cell 0 (the only state op) latches the same value â€” the differential over CELL inspection (Â§118 parity).
    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    REQUIRE(got.cells.size() == 1U);
    CHECK(got.cells[0] == ref_acc);      // â­ the compiled latch agrees with the reference (dense cells + latch lists)
    CHECK(got.values[0] == ref.values[0]); // the return (0) also agrees
}

TEST_CASE("ceir 11b: a depth-2 state ring wraps in lockstep with the reference (delay-line witness)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   statek = ctx.intern_op("core", "state");
    const OpId                   cfor   = ctx.intern_op("core", "for");
    Module* const                m = ctx.create_module();
    // @main() -> i32 { for(0,4,1){ iv: %d = state[depth=2](0, iv) }; return 0 } -- reads 0,0,0,1; ring[pos] ends at 2.
    // A depth-2 WITNESS: the same program on a depth-1 cell would leave 3 (the last iv). So cells[0]==2 proves the ring
    // wraparound + the whole-ring init-fill, not just that a cell exists.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* lohilst[3] = {konst(ctx, o, mb, 0)->result(0U), konst(ctx, o, mb, 4)->result(0U),
                         konst(ctx, o, mb, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    mb->append(forop);
    Block* const forb = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(forb);
    Operation* const initk = konst(ctx, o, forb, 0);
    Value* sops[2] = {initk->result(0U), forb->arg(0U)}; // state(init=0, next=iv)
    Operation* const d = ctx.create_operation(statek, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    ctx.set_attr(d, "depth", ctx.attr_int(2)); // â­ depth-2 ring
    forb->append(d);
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    i64 ref_cell = -1;
    REQUIRE(in.cell_value(d, ref_cell));
    CHECK(ref_cell == 2); // the depth-2 trajectory (a depth-1 cell would read 0,0,1,2 and end at 3)

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    REQUIRE(got.cells.size() == 1U);
    CHECK(got.cells[0] == ref_cell); // â­ compiled depth-2 ring wraps identically (whole-ring init-fill + %-wrap)
}

TEST_CASE("ceir 11b: core.delay and core.history route to the same cell mechanism as core.state", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   delayk = ctx.intern_op("core", "delay");
    const OpId                   histk  = ctx.intern_op("core", "history");
    Module* const                m = ctx.create_module();
    // @main() -> i32 { %d = delay(3, 0); %h = history(4, 0); return d + h } -> 3 + 4 = 7.
    // The reference installs delay/history on the SAME eval_state; the compiler's classifier routes both to Op::State.
    // Two cells coexist (num_cells==2) -- a cell_read yields each init, and the two engines byte-agree on 7.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* dops[2] = {konst(ctx, o, mb, 3)->result(0U), konst(ctx, o, mb, 0)->result(0U)};
    Operation* const d = ctx.create_operation(delayk, ConstSpan<Value*>(dops, 2U), 1U, ctx.type_i32());
    mb->append(d);
    Value* hops[2] = {konst(ctx, o, mb, 4)->result(0U), konst(ctx, o, mb, 0)->result(0U)};
    Operation* const h = ctx.create_operation(histk, ConstSpan<Value*>(hops, 2U), 1U, ctx.type_i32());
    mb->append(h);
    Value* rv[1] = {bin(ctx, o.addi, d->result(0U), h->result(0U), mb)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 7); // delay(3)+history(4), both engines agree
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ CEIR-11b stage 4a: func.call (compiled-function indices + arena frame windows) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on a basic func.call", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @add1(%x) { return x + 1 }   @main() { return call add1(41) } -> 42
    Operation* const fa = mkfunc(ctx, *m, "add1", 1U);
    Block* const     ab = func::func_body_block(fa);
    Value* rva[1] = {bin(ctx, o.addi, ab->arg(0U), konst(ctx, o, ab, 1)->result(0U), ab)->result(0U)};
    ab->append(func::create_return(ctx, ConstSpan<Value*>(rva, 1U)));
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* cargs[1] = {konst(ctx, o, mb, 41)->result(0U)};
    Operation* const call = func::create_call(ctx, "add1", ConstSpan<Value*>(cargs, 1U), 1U, ctx.type_i32());
    mb->append(call);
    Value* rvm[1] = {call->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rvm, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 42); // callee frame window + arg binding + return
}

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on a nested func.call", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @inc(%x){ return x+1 }  @add2(%x){ return inc(inc(x)) }  @main(){ return add2(40) } -> 42 (two stacked frames)
    Operation* const fi = mkfunc(ctx, *m, "inc", 1U);
    Block* const     ib = func::func_body_block(fi);
    Value* rvi[1] = {bin(ctx, o.addi, ib->arg(0U), konst(ctx, o, ib, 1)->result(0U), ib)->result(0U)};
    ib->append(func::create_return(ctx, ConstSpan<Value*>(rvi, 1U)));
    Operation* const f2 = mkfunc(ctx, *m, "add2", 1U);
    Block* const     b2 = func::func_body_block(f2);
    Value* c1a[1] = {b2->arg(0U)};
    Operation* const c1 = func::create_call(ctx, "inc", ConstSpan<Value*>(c1a, 1U), 1U, ctx.type_i32());
    b2->append(c1);
    Value* c2a[1] = {c1->result(0U)};
    Operation* const c2 = func::create_call(ctx, "inc", ConstSpan<Value*>(c2a, 1U), 1U, ctx.type_i32());
    b2->append(c2);
    Value* rv2[1] = {c2->result(0U)};
    b2->append(func::create_return(ctx, ConstSpan<Value*>(rv2, 1U)));
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* ma[1] = {konst(ctx, o, mb, 40)->result(0U)};
    Operation* const cm = func::create_call(ctx, "add2", ConstSpan<Value*>(ma, 1U), 1U, ctx.type_i32());
    mb->append(cm);
    Value* rvm[1] = {cm->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rvm, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 42); // stacked call frames (add2 -> inc -> inc)
}

TEST_CASE("ceir 11b: the compiled plan byte-matches the reference on shallow RECURSION (sum n..1)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   cif = ctx.intern_op("core", "if");
    Module* const                m = ctx.create_module();
    // @sum(%n){ %c = cmpi(sle, n, 0); %r = if(c){ yield 0 } else { %s = call sum(n-1); yield n + s }; return r }
    // @main(){ return call sum(5) } -> 5+4+3+2+1 = 15 (recursion resolves via the pre-assigned fn index)
    Operation* const fs = mkfunc(ctx, *m, "sum", 1U);
    Block* const     sb = func::func_body_block(fs);
    Operation* const c  = bin(ctx, o.cmpi, sb->arg(0U), konst(ctx, o, sb, 0)->result(0U), sb);
    ctx.set_attr(c, "predicate", ctx.attr_string("sle"));
    Value* ifops[1] = {c->result(0U)};
    Operation* const iff = ctx.create_operation(cif, ConstSpan<Value*>(ifops, 1U), 1U, ctx.type_i32(), 2U);
    sb->append(iff);
    Block* const thenb = ctx.create_block(0U);
    iff->region(0)->append(thenb);
    yield1(ctx, thenb, konst(ctx, o, thenb, 0)->result(0U)); // base case: 0
    Block* const elseb = ctx.create_block(0U);
    iff->region(1)->append(elseb);
    Operation* const nm1 = bin(ctx, o.addi, sb->arg(0U), konst(ctx, o, elseb, -1)->result(0U), elseb); // n-1
    Value* rca[1] = {nm1->result(0U)};
    Operation* const rc = func::create_call(ctx, "sum", ConstSpan<Value*>(rca, 1U), 1U, ctx.type_i32());
    elseb->append(rc);
    yield1(ctx, elseb, bin(ctx, o.addi, sb->arg(0U), rc->result(0U), elseb)->result(0U)); // n + sum(n-1)
    Value* rvs[1] = {iff->result(0U)};
    sb->append(func::create_return(ctx, ConstSpan<Value*>(rvs, 1U)));
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* ma[1] = {konst(ctx, o, mb, 5)->result(0U)};
    Operation* const cm = func::create_call(ctx, "sum", ConstSpan<Value*>(ma, 1U), 1U, ctx.type_i32());
    mb->append(cm);
    Value* rvm[1] = {cm->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rvm, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 5 + 4 + 3 + 2 + 1); // 15
}

TEST_CASE("ceir 11b: a state cell in a callee called TWICE is GLOBAL per-op, agreeing with the reference", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   statek = ctx.intern_op("core", "state");
    Module* const                m = ctx.create_module();
    // @tick(){ %s = state(0, s+1); return s }  @main(){ %a = tick(); %b = tick(); return a + b }
    // GLOBAL cell (keyed per-OP, like the reference m_cells): a=0, b=1 -> 1. A per-FRAME cell would give 0+0=0.
    Operation* const ft = mkfunc(ctx, *m, "tick", 0U);
    Block* const     tb = func::func_body_block(ft);
    Value* sops[2] = {konst(ctx, o, tb, 0)->result(0U), konst(ctx, o, tb, 0)->result(0U)};
    Operation* const s = ctx.create_operation(statek, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    tb->append(s);
    s->set_operand(1U, bin(ctx, o.addi, s->result(0U), konst(ctx, o, tb, 1)->result(0U), tb)->result(0U)); // next = s+1
    Value* rvt[1] = {s->result(0U)};
    tb->append(func::create_return(ctx, ConstSpan<Value*>(rvt, 1U)));
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const ca = func::create_call(ctx, "tick", ConstSpan<Value*>(), 1U, ctx.type_i32());
    mb->append(ca);
    Operation* const cb = func::create_call(ctx, "tick", ConstSpan<Value*>(), 1U, ctx.type_i32());
    mb->append(cb);
    Value* rvm[1] = {bin(ctx, o.addi, ca->result(0U), cb->result(0U), mb)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rvm, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    CHECK(ref.values[0] == 1); // the reference global cell: 0 + 1
    i64 ref_cell = -1;
    REQUIRE(in.cell_value(s, ref_cell));
    CHECK(ref_cell == 2); // latched twice (once per call)

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    CHECK(got.values[0] == ref.values[0]);   // compiled global cell agrees (a per-frame cell would give 0, not 1)
    REQUIRE(got.cells.size() == 1U);
    CHECK(got.cells[0] == ref_cell);          // the cell latched across BOTH call frames -- one global cell, not two
}

TEST_CASE("ceir 11b: the compiler REJECTS an unresolved callee and a call/param arity mismatch", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    {
        Module* const    m  = ctx.create_module(); // @main(){ return call ghost() } -- ghost undefined
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Operation* const cg = func::create_call(ctx, "ghost", ConstSpan<Value*>(), 1U, ctx.type_i32());
        mb->append(cg);
        Value* rv[1] = {cg->result(0U)};
        mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
        const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
        CHECK(!cr.ok());
        CHECK(cr.error == plan::CompileError::UnresolvedCall); // resolved at COMPILE (the reference fails at runtime, sec-4)
    }
    {
        Module* const    m  = ctx.create_module(); // @one(%x){ return x }  @main(){ return call one(1, 2) } -- 2 args, 1 param
        Operation* const f1 = mkfunc(ctx, *m, "one", 1U);
        Block* const     b1 = func::func_body_block(f1);
        Value* rv1[1] = {b1->arg(0U)};
        b1->append(func::create_return(ctx, ConstSpan<Value*>(rv1, 1U)));
        Operation* const fm = mkfunc(ctx, *m, "main", 0U);
        Block* const     mb = func::func_body_block(fm);
        Value* ca[2] = {konst(ctx, o, mb, 1)->result(0U), konst(ctx, o, mb, 2)->result(0U)};
        Operation* const callop = func::create_call(ctx, "one", ConstSpan<Value*>(ca, 2U), 1U, ctx.type_i32());
        mb->append(callop);
        Value* rvm[1] = {callop->result(0U)};
        mb->append(func::create_return(ctx, ConstSpan<Value*>(rvm, 1U)));
        const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
        CHECK(!cr.ok());
        CHECK(cr.error == plan::CompileError::CallArity); // structural arity -> compile reject, not runtime BadArity
    }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ CEIR-11b stage 4b: async/task (run-global token store; SEQUENTIAL Â§37/Â§38 reference) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
namespace
{
// `<dialect>.<name> { yield const v } -> the token op (result(0) is the handle). launch-likes store the body's yields.
Operation* mktok(Context& ctx, const Ops& o, Block* b, StringView dialect, StringView name, i64 v)
{
    Operation* const l  = ctx.create_operation(ctx.intern_op(dialect, name), {}, 1U, ctx.type_i32(), 1U);
    Block* const     lb = ctx.create_block(0U);
    l->region(0)->append(lb);
    yield1(ctx, lb, konst(ctx, o, lb, v)->result(0U));
    b->append(l);
    return l;
}
} // namespace

TEST_CASE("ceir 11b: async.launch -> await round-trips a token's yields", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %t = launch { yield 42 }; %r = await(t); return r } -> 42
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const l  = mktok(ctx, o, mb, "async", "launch", 42);
    Value* aw[1] = {l->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(a);
    Value* rv[1] = {a->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 42); // store handle -> stored yields, both engines
}

TEST_CASE("ceir 11b: a token launched in a CALLEE is awaited in the CALLER (run-global store witness)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @producer(){ %t = launch { yield 7 }; return %t }  @main(){ %tok = call producer(); return await(tok) } -> 7
    // â­ the launch runs in producer's frame (popped at return), but the TOKEN STORE is run-global, so the handle is
    // still live when the caller awaits it. A per-FRAME store would be discarded on the pop -> BadToken.
    Operation* const fp = mkfunc(ctx, *m, "producer", 0U);
    Block* const     pb = func::func_body_block(fp);
    Value* rvp[1] = {mktok(ctx, o, pb, "async", "launch", 7)->result(0U)};
    pb->append(func::create_return(ctx, ConstSpan<Value*>(rvp, 1U)));
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const cp = func::create_call(ctx, "producer", ConstSpan<Value*>(), 1U, ctx.type_i32());
    mb->append(cp);
    Value* aw[1] = {cp->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(a);
    Value* rv[1] = {a->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 7); // run-global store survives the callee frame pop
}

TEST_CASE("ceir 11b: a task.continuation binds the antecedent yields as its body args", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %t = launch { yield 10 }; %c = continuation(t){ (x): yield x+5 }; return await(c) } -> 15
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const l  = mktok(ctx, o, mb, "async", "launch", 10);
    Value* ct[1] = {l->result(0U)};
    Operation* const cn = ctx.create_operation(ctx.intern_op("task", "continuation"), ConstSpan<Value*>(ct, 1U), 1U,
                                               ctx.type_i32(), 1U);
    Block* const cb = ctx.create_block(1U, ctx.type_i32()); // the body binds the antecedent's one yield as arg(0)
    cn->region(0)->append(cb);
    yield1(ctx, cb, bin(ctx, o.addi, cb->arg(0U), konst(ctx, o, cb, 5)->result(0U), cb)->result(0U)); // x + 5
    mb->append(cn);
    Value* aw[1] = {cn->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(a);
    Value* rv[1] = {a->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 15); // antecedent [10] -> x -> body yields 15
}

TEST_CASE("ceir 11b: async.join concatenates its input tokens' yields", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %a=launch{yield 3}; %b=launch{yield 4}; %j=join(a,b); %x,%y=await(j); return x+y } -> 3+4 = 7
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const la = mktok(ctx, o, mb, "async", "launch", 3);
    Operation* const lb = mktok(ctx, o, mb, "async", "launch", 4);
    Value* jt[2] = {la->result(0U), lb->result(0U)};
    Operation* const j = ctx.create_operation(ctx.intern_op("async", "join"), ConstSpan<Value*>(jt, 2U), 1U, ctx.type_i32());
    mb->append(j);
    Value* aw[1] = {j->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 2U, ctx.type_i32());
    mb->append(a); // await with TWO results -> the concatenated [3, 4]
    Value* rv[1] = {bin(ctx, o.addi, a->result(0U), a->result(1U), mb)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 7); // join([3],[4]) = [3,4]; both retrieved
}

TEST_CASE("ceir 11b: async.race yields the deterministic winner index 0", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %a=launch{yield 3}; %b=launch{yield 4}; %w=race(a,b); return w } -> 0 (sequential first-ready)
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* rt[2] = {mktok(ctx, o, mb, "async", "launch", 3)->result(0U), mktok(ctx, o, mb, "async", "launch", 4)->result(0U)};
    Operation* const w = ctx.create_operation(ctx.intern_op("async", "race"), ConstSpan<Value*>(rt, 2U), 1U, ctx.type_i32());
    mb->append(w);
    Value* rv[1] = {w->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 0); // race validates NO handle; winner index 0
}

TEST_CASE("ceir 11b: async.cancel consumes a token and no-ops (both engines)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %t=launch{yield 5}; cancel(t); return const 8 } -> 8 (cancel is a no-op at eval time)
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* ct[1] = {mktok(ctx, o, mb, "async", "launch", 5)->result(0U)};
    mb->append(ctx.create_operation(ctx.intern_op("async", "cancel"), ConstSpan<Value*>(ct, 1U), 0U));
    Value* rv[1] = {konst(ctx, o, mb, 8)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 8);
}

TEST_CASE("ceir 11b: task.spawn -> fiber_wait routes to the same launch/await mechanism as async", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %t = task.spawn { yield 9 }; %r = task.fiber_wait(t); return r } -> 9 (task dialect -> Op::Launch/Await)
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const t  = mktok(ctx, o, mb, "task", "spawn", 9);
    Value* fw[1] = {t->result(0U)};
    Operation* const r = ctx.create_operation(ctx.intern_op("task", "fiber_wait"), ConstSpan<Value*>(fw, 1U), 1U, ctx.type_i32());
    mb->append(r);
    Value* rv[1] = {r->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 9);
}

TEST_CASE("ceir 11b: await of an invalid token handle -> BadToken agrees with the reference", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %bad = const 99; %r = await(bad); return r } -- 99 is not a live handle -> BadToken in BOTH engines
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* aw[1] = {konst(ctx, o, mb, 99)->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(a);
    Value* rv[1] = {a->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    CHECK(ref.error == exec::ExecError::BadToken); // â›” the EXACT error (not merely !ok â€” else NoSemantics would pass too)

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok()); // it COMPILES (the token value is dynamic) -- the error is a RUNTIME agreement
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    CHECK(got.error == plan::RunError::BadToken); // both engines reject the bad handle at run time
}

TEST_CASE("ceir 11b: a continuation body-arity mismatch -> ContinuationArity agrees with the reference", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %t = launch { yield 10 }; %c = continuation(t){ (x, y): yield x }; return await(c) }
    // the antecedent yields ONE value but the body binds TWO args -> reference BadArity AND compiled ContinuationArity.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Operation* const l  = mktok(ctx, o, mb, "async", "launch", 10);
    Value* ct[1] = {l->result(0U)};
    Operation* const cn = ctx.create_operation(ctx.intern_op("task", "continuation"), ConstSpan<Value*>(ct, 1U), 1U,
                                               ctx.type_i32(), 1U);
    Block* const cb = ctx.create_block(2U, ctx.type_i32()); // TWO args, but the antecedent yields only one
    cn->region(0)->append(cb);
    yield1(ctx, cb, cb->arg(0U));
    mb->append(cn);
    Value* aw[1] = {cn->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(a);
    Value* rv[1] = {a->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    CHECK(ref.error == exec::ExecError::BadArity); // the reference: body num_args != antecedent yield count

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok()); // it COMPILES (the antecedent's yield count is dynamic) -- a RUNTIME agreement
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    CHECK(got.error == plan::RunError::ContinuationArity); // both engines reject the arity mismatch at run time
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ CEIR-11b stage 4c: data-parallel (isolated body fns + per-index run + index-order fold) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
namespace
{
// a task.parallel_for / map_reduce op over [lo,hi,step(,init)] with `nres` results and `nregions` body regions.
Operation* mkdp(Context& ctx, const Ops& o, Block* b, StringView name, ConstSpan<i64> ranges, crd::u32 nres,
                crd::u32 nregions)
{
    Value* rv[4] = {nullptr, nullptr, nullptr, nullptr};
    for (crd::u32 i = 0; i < static_cast<crd::u32>(ranges.size()); ++i) { rv[i] = konst(ctx, o, b, ranges[i])->result(0U); }
    Operation* const op = ctx.create_operation(ctx.intern_op("task", name),
                                               ConstSpan<Value*>(rv, static_cast<crd::u32>(ranges.size())), nres,
                                               nres != 0U ? ctx.type_i32() : TypeId{}, nregions);
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 11b: a task.parallel_for's per-index map agrees with the reference (map inspection)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ parallel_for(0,4,1){ (iv): yield iv*iv }; return 0 } -- map = [0,1,4,9]
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    i64 rng[3] = {0, 4, 1};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    yield1(ctx, body, bin(ctx, o.muli, body->arg(0U), body->arg(0U), body)->result(0U)); // iv*iv
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    const ConstSpan<i64> refmap = in.map_output(pf);

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    REQUIRE(got.map_outputs.size() == 1U);
    REQUIRE(got.map_outputs[0].size() == refmap.size());
    REQUIRE(got.map_outputs[0].size() == 4U);
    const i64 expect[4] = {0, 1, 4, 9};
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK(got.map_outputs[0][i] == refmap[i]); // â­ compiled per-index map == reference map_output
        CHECK(got.map_outputs[0][i] == expect[i]); // ... and it is the right value
    }
}

TEST_CASE("ceir 11b: a NON-ASSOCIATIVE map_reduce folds in index order, agreeing with the reference", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %r = map_reduce(0,4,1, init=0) map{(iv): yield iv} combine{(acc,elem): yield acc*2 + elem}; return r }
    // map = [0,1,2,3]; fold: 0 -> 0 -> 1 -> 4 -> 11. NON-associative (acc*2+elem) so ONLY index order gives 11.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    i64 rng[4] = {0, 4, 1, 0};
    Operation* const mr = mkdp(ctx, o, mb, "map_reduce", ConstSpan<i64>(rng, 4U), 1U, 2U);
    Block* const     mapb = ctx.create_block(1U, ctx.type_i32());
    mr->region(0)->append(mapb);
    yield1(ctx, mapb, mapb->arg(0U)); // map: identity (yield iv)
    Block* const combb = ctx.create_block(2U, ctx.type_i32());
    mr->region(1)->append(combb);
    Operation* const dbl = bin(ctx, o.muli, combb->arg(0U), konst(ctx, o, combb, 2)->result(0U), combb); // acc*2
    yield1(ctx, combb, bin(ctx, o.addi, dbl->result(0U), combb->arg(1U), combb)->result(0U));            // acc*2 + elem
    Value* rv[1] = {mr->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    CHECK(differential(ctx, *m, "main", ConstSpan<i64>(), &root)[0] == 11); // the index-order fold; both engines agree
}

TEST_CASE("ceir 11b: an ISOLATED parallel_for body that reads an outer capture is a compile reject", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %cap = const 99; parallel_for(0,2,1){ (iv): yield iv + cap }; return 0 } -- `cap` is an OUTER capture.
    // the body is an ISOLATED mini-function (fresh slot map), so `cap` fails slot_of at COMPILE (reference: runtime
    // UndefinedValue -- the sec-4 pattern). CapturedValue distinguishes it from an unsupported op.
    Operation* const fm  = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb  = func::func_body_block(fm);
    Operation* const cap = konst(ctx, o, mb, 99);
    i64 rng[3] = {0, 2, 1};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    yield1(ctx, body, bin(ctx, o.addi, body->arg(0U), cap->result(0U), body)->result(0U)); // iv + cap  <- capture
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    CHECK(!cr.ok());
    CHECK(cr.error == plan::CompileError::CapturedValue);
}

TEST_CASE("ceir 11b: a STATEFUL parallel_for body rejects in both engines (compile vs runtime)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   statek = ctx.intern_op("core", "state");
    Module* const                m = ctx.create_module();
    // @main(){ parallel_for(0,2,1){ (iv): %s = state(iv, iv); yield s }; return 0 } -- a state cell makes the body NOT
    // state-free; the SHARED preflight rejects it: reference at RUNTIME (ParallelBodyStateful), compiled at COMPILE.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    i64 rng[3] = {0, 2, 1};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    Value* sops[2] = {body->arg(0U), body->arg(0U)};
    Operation* const s = ctx.create_operation(statek, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    body->append(s);
    yield1(ctx, body, s->result(0U));
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    CHECK(ref.error == exec::ExecError::ParallelBodyStateful); // reference: RUNTIME reject

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    CHECK(!cr.ok());
    CHECK(cr.error == plan::CompileError::ParallelStateful); // compiled: COMPILE reject (shared preflight, Â§4)
}

TEST_CASE("ceir 11b: a parallel_for with step 0 -> BadForStep agrees with the reference (runtime)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ parallel_for(0,4,0){ (iv): yield iv }; return 0 } -- step 0 is a dynamic value -> a RUNTIME differential.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    i64 rng[3] = {0, 4, 0};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    yield1(ctx, body, body->arg(0U));
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    CHECK(ref.error == exec::ExecError::BadForStep);

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok()); // it COMPILES (step is a dynamic operand)
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    CHECK(got.error == plan::RunError::BadForStep); // both engines reject the bad step at run time
}

TEST_CASE("ceir 11b: an empty-range parallel_for yields an empty map in both engines", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ parallel_for(4,0,1){ (iv): yield iv }; return 0 } -- lo > hi -> count 0 -> an empty map (both engines)
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    i64 rng[3] = {4, 0, 1};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    yield1(ctx, body, body->arg(0U));
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    CHECK(in.map_output(pf).size() == 0U);

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    REQUIRE(got.map_outputs.size() == 1U);
    CHECK(got.map_outputs[0].size() == 0U); // empty map, agreeing with the reference's find-fail/empty
    CHECK(got.values[0] == ref.values[0]);  // return 0
}

TEST_CASE("ceir 11b: a parallel_for body has a FRESH token store -> await of a parent handle is BadToken", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %p = launch{yield 5}; parallel_for(0,2,1){ (iv): %r = await(const 0); yield r }; return 0 }
    // the parent launches handle 0, but the body runs on a FRESH per-phase store (the reference sub) -> await(0) is
    // BadToken in BOTH. â›” Without the token swap the compiled body would resolve the PARENT's handle (accept what the
    // reference rejects â€” the dangerous direction).
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    (void)mktok(ctx, o, mb, "async", "launch", 5); // parent handle 0 (unused otherwise)
    i64 rng[3] = {0, 2, 1};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    Value* aw[1] = {konst(ctx, o, body, 0)->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    body->append(a);
    yield1(ctx, body, a->result(0U));
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    CHECK(ref.error == exec::ExecError::BadToken); // reference: the sub's store is empty

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    CHECK(got.error == plan::RunError::BadToken); // compiled: the per-phase store is empty too
}

TEST_CASE("ceir 11b: a parallel_for body's launched-token handles number from 0 (fresh store), not parent-global", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %p = launch{yield 99}; parallel_for(0,2,1){ (iv): %t = launch{yield 7}; yield t }; return 0 }
    // the parent holds handle 0; the body (fresh per-phase store) numbers ITS launches from 0 -> map = [0, 1] in BOTH.
    // â›” Without the swap the compiled body would number from the parent-global 1 -> [1, 2] (a value leak).
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    (void)mktok(ctx, o, mb, "async", "launch", 99); // parent handle 0
    i64 rng[3] = {0, 2, 1};
    Operation* const pf = mkdp(ctx, o, mb, "parallel_for", ConstSpan<i64>(rng, 3U), 0U, 1U);
    Block* const     body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    Operation* const bl = ctx.create_operation(ctx.intern_op("async", "launch"), {}, 1U, ctx.type_i32(), 1U);
    Block* const     blb = ctx.create_block(0U);
    bl->region(0)->append(blb);
    yield1(ctx, blb, konst(ctx, o, blb, 7)->result(0U));
    body->append(bl);
    yield1(ctx, body, bl->result(0U)); // yield the token HANDLE
    Value* rv[1] = {konst(ctx, o, mb, 0)->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    const ConstSpan<i64> refmap = in.map_output(pf);

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    REQUIRE(got.map_outputs.size() == 1U);
    REQUIRE(got.map_outputs[0].size() == refmap.size());
    REQUIRE(got.map_outputs[0].size() == 2U);
    CHECK(got.map_outputs[0][0] == refmap[0]);
    CHECK(got.map_outputs[0][1] == refmap[1]);
    CHECK(got.map_outputs[0][0] == 0); // fresh-store numbering, not the parent-global 1
    CHECK(got.map_outputs[0][1] == 1);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ CEIR-11b stage 5 â€” the FULL-CORPUS differential (the shared builder, run through BOTH engines) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// â›” The differential contract (the Â§4 tier boundary + the accumulated watch items, stated ONCE here):
//   - VALUES: `pin_values` byte-identical (the reference interpreter vs the compiled plan).
//   - CELLS (watch a): the builder declares its Â§20 state ops in COMPILE ORDER; `cell_value(op)` true â‡’ equals compiled
//     `cells[i]`; false (never read â€” an untaken branch) â‡’ compiled `cells[i]` is 0. (b) a cell's `next` is block-local
//     (the 5d verifier forbids escape). (c) a program whose reference run hits UndefinedValue (an over-declared call
//     result / dead-branch capture) is OUTSIDE the contract â€” same as programs that don't compile.
//   - MAP_OUTPUTS: handle-based per dp op, in compile order. (d) a dp op NESTED inside a parallel body has a sub-local
//     (discarded) map_output in the reference, so the generic compare SKIPS nested dp ops (the corpus uses top-level dp).
//   - The programs that COMPILE are the contract; a typed CompileError (UnsupportedOp / Bad* / Call* / Parallel* /
//     CapturedValue) is a legitimate tier difference, not a differential axis.
namespace
{
// run a corpus program through BOTH engines; assert values + cells + map_outputs byte-agree; return the shared values.
Array<i64> corpus_differential(Context& ctx, const corpus::Built& built, StringView entry, ConstSpan<i64> args,
                               memory::IAllocator* alloc)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*built.m, entry, args);
    REQUIRE(ref.ok());

    const plan::CompileResult cr = plan::compile(ctx, *built.m, entry, alloc);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, args, alloc);
    REQUIRE(got.ok());

    // VALUES â€” byte-identical
    const Array<crd::u8> a = exec::pin_values(ConstSpan<i64>(ref.values.data(), ref.values.size()), alloc);
    const Array<crd::u8> b = exec::pin_values(ConstSpan<i64>(got.values.data(), got.values.size()), alloc);
    REQUIRE(a.size() == b.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(a.size()); ++i) { CHECK(a[i] == b[i]); }

    // CELLS â€” handle-based, compile order (watch (a))
    REQUIRE(got.cells.size() >= static_cast<crd::usize>(built.cells.size()));
    for (crd::u32 i = 0; i < static_cast<crd::u32>(built.cells.size()); ++i)
    {
        i64 rv = 0;
        if (in.cell_value(built.cells[i], rv)) { CHECK(got.cells[i] == rv); }
        else { CHECK(got.cells[i] == 0); }
    }

    // MAP_OUTPUTS â€” handle-based, compile order (top-level dp ops; watch (d))
    REQUIRE(got.map_outputs.size() >= static_cast<crd::usize>(built.maps.size()));
    for (crd::u32 j = 0; j < static_cast<crd::u32>(built.maps.size()); ++j)
    {
        const ConstSpan<i64> refmap = in.map_output(built.maps[j]);
        REQUIRE(got.map_outputs[j].size() == refmap.size());
        for (crd::u32 e = 0; e < static_cast<crd::u32>(refmap.size()); ++e) { CHECK(got.map_outputs[j][e] == refmap[e]); }
    }
    Array<i64> out(alloc);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(got.values.size()); ++i) { out.push_back(got.values[i]); }
    return out;
}
} // namespace

TEST_CASE("ceir 11b corpus: the 5z band-5 gate (loop+match+if+calls+state) agrees end-to-end", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    const corpus::Built          g = corpus::build_5z(ctx, o);
    i64 n6[1] = {6};
    const Array<i64> v = corpus_differential(ctx, g, "main", ConstSpan<i64>(n6, 1U), &root);
    REQUIRE(v.size() == 1U);
    CHECK(v[0] == 118); // the pinned band-5 result: (0+2+4)+(3+4+5)=18 acc; ok -> bonus 100
}

TEST_CASE("ceir 11b corpus: the 6z non-associative map_reduce agrees end-to-end", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    const corpus::Built          g = corpus::build_6z(ctx, o);
    CHECK(corpus_differential(ctx, g, "main", ConstSpan<i64>(), &root)[0] == 33930); // map [0,1,4,9,16]; fold acc*31+elem
}

TEST_CASE("ceir 11b corpus: the async program (launch/await/join/continuation) agrees end-to-end", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    const corpus::Built          g = corpus::build_async(ctx, o);
    CHECK(corpus_differential(ctx, g, "main", ConstSpan<i64>(), &root)[0] == 29); // 7 + (3+4) + (10+5)
}

TEST_CASE("ceir 11b corpus: the six task ops (spawn/main_thread/worker/group/fiber_wait/continuation) agree", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    const corpus::Built          g = corpus::build_tasks(ctx, o);
    CHECK(corpus_differential(ctx, g, "main", ConstSpan<i64>(), &root)[0] == 46); // 1 + 2 + 3 + 40
}

TEST_CASE("ceir 11b corpus: the COMPOSING program (all dialects: arith+cf+state+calls+async+task+dp) agrees", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    const corpus::Built          g = corpus::build_composing(ctx, o);
    const Array<i64> v = corpus_differential(ctx, g, "main", ConstSpan<i64>(), &root);
    REQUIRE(v.size() == 1U);
    CHECK(v[0] == 118); // a_read(0)+b_read(5)+inc(5)=6 + await(7) + fiber_wait(100); cells latch [10,20]; map [0,1,4]
}

TEST_CASE("ceir 11b corpus: the sec-121 no-privileged-path property at the PLAN layer (cooked twin)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    // â­ a cooked->loaded module compiles to BYTE-IDENTICAL results vs its builder-form twin. Compile BOTH twins, run both
    // through the COMPILED tier, compare values + cells + map_outputs ARRAYS. â›” NO handles â€” the loaded module's ops are
    // different pointers, but dense cell/map indices are assigned by COMPILE ORDER, so structurally-identical twins align.
    Context           ctx_a(&root);
    const corpus::Kit ka(ctx_a);
    const corpus::Built g = corpus::build_composing(ctx_a, ka);
    const Array<crd::u8> blob = serialize(ctx_a, *g.m, &root);

    Context           ctx_b(&root);
    const corpus::Kit kb(ctx_b); // â›” re-register the dialects in the loading context (the 5d trait-is-registry finding)
    (void)kb;
    const ParseResult dr = deserialize(ctx_b, ConstSpan<crd::u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);

    const plan::CompileResult ca = plan::compile(ctx_a, *g.m, "main", &root);
    REQUIRE(ca.ok());
    const plan::CompileResult cb = plan::compile(ctx_b, *dr.module, "main", &root);
    REQUIRE(cb.ok());
    const plan::RunResult ra = plan::run(ca.plan, ConstSpan<i64>(), &root);
    const plan::RunResult rb = plan::run(cb.plan, ConstSpan<i64>(), &root);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());

    REQUIRE(ra.values.size() == rb.values.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(ra.values.size()); ++i) { CHECK(ra.values[i] == rb.values[i]); }
    REQUIRE(ra.cells.size() == rb.cells.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(ra.cells.size()); ++i) { CHECK(ra.cells[i] == rb.cells[i]); }
    REQUIRE(ra.map_outputs.size() == rb.map_outputs.size());
    for (crd::u32 j = 0; j < static_cast<crd::u32>(ra.map_outputs.size()); ++j)
    {
        REQUIRE(ra.map_outputs[j].size() == rb.map_outputs[j].size());
        for (crd::u32 e = 0; e < static_cast<crd::u32>(ra.map_outputs[j].size()); ++e)
        {
            CHECK(ra.map_outputs[j][e] == rb.map_outputs[j][e]);
        }
    }
    CHECK(ra.values[0] == 118); // and the twin runs to the pinned composing result
}

TEST_CASE("ceir 11b: a control-flow op NESTED in a control-flow op selects the right child (nested-cf regression)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ for(0,4,1){ iv: %big = cmpi slt(iv,2); %inc = if(big){yield 10}{yield 1}; %acc = state(0, acc+inc) };
    //          return 0 } — the acc cell = 10+10+1+1 = 22. ⛔ The child_pool aliasing bug (the for compiling BEFORE its
    // nested if) made the For run the if's then-block instead of the loop body; the 5z corpus differential caught it.
    Block* bm = nullptr;
    (void)corpus::mkfunc(ctx, *m, "main", 0U, bm);
    Value* lohi[3] = {corpus::konst(ctx, o, bm, 0)->result(0U), corpus::konst(ctx, o, bm, 4)->result(0U),
                      corpus::konst(ctx, o, bm, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohi, 3U), 0U, {}, 1U);
    bm->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(body);
    Operation* const big = corpus::bin(ctx, o.cmpi, body->arg(0U), corpus::konst(ctx, o, body, 2)->result(0U), body);
    ctx.set_attr(big, "predicate", ctx.attr_string("slt")); // iv < 2
    Value* ic[1] = {big->result(0U)};
    Operation* const iff = ctx.create_operation(o.cif, ConstSpan<Value*>(ic, 1U), 1U, ctx.type_i32(), 2U);
    body->append(iff);
    Block* const tb = ctx.create_block(0U);
    iff->region(0)->append(tb);
    corpus::yield1(ctx, o, tb, corpus::konst(ctx, o, tb, 10)->result(0U));
    Block* const eb = ctx.create_block(0U);
    iff->region(1)->append(eb);
    corpus::yield1(ctx, o, eb, corpus::konst(ctx, o, eb, 1)->result(0U));
    Operation* const acc = corpus::mk_state(ctx, o, corpus::konst(ctx, o, body, 0)->result(0U),
                                            corpus::konst(ctx, o, body, 0)->result(0U), body);
    acc->set_operand(1U, corpus::bin(ctx, o.addi, acc->result(0U), iff->result(0U), body)->result(0U)); // acc + inc
    corpus::ret1(ctx, bm, corpus::konst(ctx, o, bm, 0)->result(0U));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    i64 ref_acc = -1;
    REQUIRE(in.cell_value(acc, ref_acc));
    CHECK(ref_acc == 22); // 10 + 10 + 1 + 1

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const plan::RunResult got = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(got.ok());
    REQUIRE(got.cells.size() == 1U);
    CHECK(got.cells[0] == ref_acc); // ⭐ the For runs its BODY (with the nested if), not the if's then-block
}

// ─────────────────── CEIR-11c — the profiling SEAM (null-default pre/post op hooks; a perf bridge is the consumer) ───────────────────
namespace
{
struct SeamCount
{
    crd::u64 pre  = 0;
    crd::u64 post = 0;
};
void seam_pre(crd::u8 /*op*/, void* u) { static_cast<SeamCount*>(u)->pre++; }
void seam_post(crd::u8 /*op*/, void* u) { static_cast<SeamCount*>(u)->post++; }
} // namespace

TEST_CASE("ceir 11c: the run seam fires pre/post per dispatched instr and is OBSERVATION-ONLY", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const corpus::Kit            o(ctx);
    const corpus::Built          g = corpus::build_composing(ctx, o); // the richest program (all dialects)
    const plan::CompileResult    cr = plan::compile(ctx, *g.m, "main", &root);
    REQUIRE(cr.ok());
    // ⭐ the plan-compile SHAPE stats (11c) — composing has 2 §20 cells, 1 dp op, >=2 funcs (@inc + @main + the map body).
    CHECK(cr.stats.num_cells == 2U);
    CHECK(cr.stats.num_maps == 1U);
    CHECK(cr.stats.num_funcs >= 3U);
    CHECK(cr.stats.num_instrs > 0U);
    CHECK(cr.stats.num_seqs > 0U);

    const plan::RunResult r0 = plan::run(cr.plan, ConstSpan<i64>(), &root); // NO hooks
    REQUIRE(r0.ok());
    SeamCount             sc;
    const plan::RunResult r1 = plan::run(cr.plan, ConstSpan<i64>(), &root, plan::RunHooks{seam_pre, seam_post, &sc});
    REQUIRE(r1.ok());

    // OBSERVATION-ONLY: the hooks read only the op id, so results are byte-identical with vs without the seam.
    REQUIRE(r0.values.size() == r1.values.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(r0.values.size()); ++i) { CHECK(r0.values[i] == r1.values[i]); }
    REQUIRE(r0.cells.size() == r1.cells.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(r0.cells.size()); ++i) { CHECK(r0.cells[i] == r1.cells[i]); }
    CHECK(r1.values[0] == 118); // the pinned composing result, unchanged under observation

    CHECK(sc.pre == sc.post); // balanced on a SUCCESSFUL run
    CHECK(sc.pre == 35U);     // the exact dispatched-instr count (observe-once, the 11a count-29 precedent)
}

TEST_CASE("ceir 11c: an erroring run leaves the seam UNBALANCED (pre > post)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @main(){ %r = await(const 99); return r } -> BadToken: the Await case returns BEFORE post fires.
    Operation* const fm = mkfunc(ctx, *m, "main", 0U);
    Block* const     mb = func::func_body_block(fm);
    Value* aw[1] = {konst(ctx, o, mb, 99)->result(0U)};
    Operation* const a = ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(aw, 1U), 1U, ctx.type_i32());
    mb->append(a);
    Value* rv[1] = {a->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    SeamCount             sc;
    const plan::RunResult r = plan::run(cr.plan, ConstSpan<i64>(), &root, plan::RunHooks{seam_pre, seam_post, &sc});
    CHECK(r.error == plan::RunError::BadToken);
    CHECK(sc.pre > sc.post); // the erroring await fired `pre` but returned before `post` — the consumer tolerates this
}

// ─────────────────── CEIR-11z — the §153 hot-loop AUDIT (allocation-free + no-string-table-touch; the shipping path) ───────────────────
namespace
{
// a counting decorator: forwards to a backing allocator + tallies every memory-ACQUIRING call (the §153 audit lever).
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* backing) : m_backing(backing) { m_name = "CountingAllocator"; }
    void* allocate(crd::usize size, crd::usize align = crd::memory::kDefaultAlignment) override
    {
        ++m_allocs;
        return m_backing->allocate(size, align);
    }
    void  deallocate(void* p) noexcept override { m_backing->deallocate(p); }
    bool  owns(const void* p) const noexcept override { return m_backing->owns(p); }
    void* reallocate(void* p, crd::usize os, crd::usize ns, crd::usize a = crd::memory::kDefaultAlignment) override
    {
        ++m_allocs;
        return m_backing->reallocate(p, os, ns, a);
    }
    void* try_allocate(crd::usize size, crd::usize align = crd::memory::kDefaultAlignment) override
    {
        ++m_allocs;
        return m_backing->try_allocate(size, align);
    }
    [[nodiscard]] crd::u64 allocs() const noexcept { return m_allocs; }

private:
    crd::memory::IAllocator* m_backing;
    crd::u64                 m_allocs = 0;
};
} // namespace

TEST_CASE("ceir 11z: the compiled hot loop is ALLOCATION-FREE (alloc count independent of loop trip count)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    // @main(){ for(0,M,1){ iv: muli(iv,iv) }; return const 0 } — a pure-arith loop body. The RUN allocates only its fixed
    // setup (the frame stack + the result array); the HOT LOOP does ZERO heap per iteration, so the alloc COUNT is
    // INDEPENDENT of M. ⛔ run with hooks NULL — the shipping path 11z audits. Compile uses `root` (uncounted); the RUN
    // uses the CountingAllocator.
    const auto measure = [&root](i64 trip) -> crd::u64 {
        Context           ctx(&root);
        const corpus::Kit o(ctx);
        Block*            bm = nullptr;
        Module* const     m  = ctx.create_module();
        (void)corpus::mkfunc(ctx, *m, "main", 0U, bm);
        Value* lohi[3] = {corpus::konst(ctx, o, bm, 0)->result(0U), corpus::konst(ctx, o, bm, trip)->result(0U),
                          corpus::konst(ctx, o, bm, 1)->result(0U)};
        Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohi, 3U), 0U, {}, 1U);
        bm->append(forop);
        Block* const body = ctx.create_block(1U, ctx.type_i32());
        forop->region(0)->append(body);
        (void)corpus::bin(ctx, o.muli, body->arg(0U), body->arg(0U), body); // iv*iv (pure)
        corpus::ret1(ctx, bm, corpus::konst(ctx, o, bm, 0)->result(0U));

        const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root); // compile: uncounted
        REQUIRE(cr.ok());
        CountingAllocator     ca(&root);
        const plan::RunResult r = plan::run(cr.plan, ConstSpan<i64>(), &ca); // ⛔ hooks NULL (default) — the shipping path
        REQUIRE(r.ok());
        return ca.allocs();
    };

    const crd::u64 a10   = measure(10);
    const crd::u64 a1000 = measure(1000);
    CHECK(a10 == a1000);  // ⭐ per-iteration ZERO-alloc — the count does not grow with the loop trip count (§153)
    CHECK(a10 <= 4U);     // ... and it is a small FIXED setup (frame stack + result array), not O(ops)
}

TEST_CASE("ceir 11z: a compiled plan runs with NO Context -- the hot loop touches no string / attr table", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    plan::CompileResult          cr(&root); // outer scope — outlives the Context below
    {
        Context           ctx(&root);
        const corpus::Kit o(ctx);
        Block*            bm = nullptr;
        Module* const     m  = ctx.create_module();
        (void)corpus::mkfunc(ctx, *m, "main", 0U, bm);
        // @main(){ return (5 * 5) + 3 } = 28 — an attr-bearing program (const "value", implicit): all folded at compile.
        Operation* const five = corpus::konst(ctx, o, bm, 5);
        Operation* const sq   = corpus::bin(ctx, o.muli, five->result(0U), five->result(0U), bm);
        corpus::ret1(ctx, bm, corpus::bin(ctx, o.addi, sq->result(0U), corpus::konst(ctx, o, bm, 3)->result(0U), bm)->result(0U));
        cr = plan::compile(ctx, *m, "main", &root);
        REQUIRE(cr.ok());
    } // ⛔ the Context — and its op/attr/string INTERNING tables — is DESTROYED here.

    // the plan is a self-contained DENSE object in `root` (no Operation*, no OpId/AttrId, no string-table indices). Run it
    // with NO Context in scope: correctness here PROVES the hot loop touched no string/attr table (§153, by construction).
    const plan::RunResult r = plan::run(cr.plan, ConstSpan<i64>(), &root);
    REQUIRE(r.ok());
    CHECK(r.values[0] == 28);
}

TEST_CASE("ceir 11z: cells + call frames are AMORTIZED-arena (steady-state zero-alloc across loop iterations)", "[ceir][plan]")
{
    crd::memory::MallocAllocator root;
    // @inc(%x){ return x + 1 }   @main(){ for(0,M,1){ iv: %c = call inc(iv); %acc = state(0, acc + c) }; return 0 }
    // ⭐ The §153 amortized-arena claim (ADR-0123 §2.2) — the interesting op classes the arith-only audit does NOT cover:
    // the FIRST iteration init-fills the state ring + grows the call-frame stack (one-time); STEADY STATE is ZERO
    // (the ring is sized, the frame reuses capacity after the pop). So the alloc COUNT is INDEPENDENT of the trip count M
    // — the equality `count(10) == count(1000)` IS the amortization proof (both include the same one-time first-iter cost).
    const auto measure = [&root](i64 trip) -> crd::u64 {
        Context           ctx(&root);
        const corpus::Kit o(ctx);
        Module* const     m  = ctx.create_module();
        Block*            bi = nullptr;
        (void)corpus::mkfunc(ctx, *m, "inc", 1U, bi); // @inc(%x){ return x + 1 }
        corpus::ret1(ctx, bi, corpus::bin(ctx, o.addi, bi->arg(0U), corpus::konst(ctx, o, bi, 1)->result(0U), bi)->result(0U));
        Block* bm = nullptr;
        (void)corpus::mkfunc(ctx, *m, "main", 0U, bm);
        Value* lohi[3] = {corpus::konst(ctx, o, bm, 0)->result(0U), corpus::konst(ctx, o, bm, trip)->result(0U),
                          corpus::konst(ctx, o, bm, 1)->result(0U)};
        Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohi, 3U), 0U, {}, 1U);
        bm->append(forop);
        Block* const body = ctx.create_block(1U, ctx.type_i32());
        forop->region(0)->append(body);
        Operation* const c   = corpus::call_fn(ctx, "inc", body->arg(0U), body); // a CALL in the loop (frame window)
        Operation* const acc = corpus::mk_state(ctx, o, corpus::konst(ctx, o, body, 0)->result(0U),
                                                corpus::konst(ctx, o, body, 0)->result(0U), body); // a §20 cell (ring)
        acc->set_operand(1U, corpus::bin(ctx, o.addi, acc->result(0U), c->result(0U), body)->result(0U)); // next = acc + c

        corpus::ret1(ctx, bm, corpus::konst(ctx, o, bm, 0)->result(0U));

        const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root); // compile: uncounted
        REQUIRE(cr.ok());
        CountingAllocator     ca(&root);
        const plan::RunResult r = plan::run(cr.plan, ConstSpan<i64>(), &ca); // ⛔ hooks NULL — the shipping path
        REQUIRE(r.ok());
        return ca.allocs();
    };

    CHECK(measure(10) == measure(1000)); // ⭐ steady-state ZERO: the ring init-fill + first frame growth are one-time, not
                                         //    per-iteration — so cells + call frames are amortized-arena (§153).
}
