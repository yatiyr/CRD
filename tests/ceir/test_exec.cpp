// CEIR-5z (sec 118): the REFERENCE EXECUTOR unit suite. Per-op reference semantics (arith / ceir.core control flow +
// sec 20 state / ceir.func calls), the state ring (state=delay=history(1); history(N) sequence), fuel, the typed error
// modes, and the open-world install path. The band GATE (a pinned program, builder == text-parsed) is test_band5_gate.cpp.
// ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
struct Ops
{
    OpId cst, addi, muli, cmpi, cif, cfor, cwhile, cswitch, yield, state, history, foreach;
    explicit Ops(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          muli(ctx.intern_op("arith", "muli")), cmpi(ctx.intern_op("arith", "cmpi")), cif(ctx.intern_op("core", "if")),
          cfor(ctx.intern_op("core", "for")), cwhile(ctx.intern_op("core", "while")),
          cswitch(ctx.intern_op("core", "switch")), yield(ctx.intern_op("core", "yield")),
          state(ctx.intern_op("core", "state")), history(ctx.intern_op("core", "history")),
          foreach(ctx.intern_op("core", "foreach"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
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
Operation* unary(Context& ctx, OpId k, Value* a, Block* b, crd::u32 nresults, crd::u32 nregions)
{
    Value* ops[1] = {a};
    Operation* const o = ctx.create_operation(k, ConstSpan<Value*>(ops, 1U), nresults, ctx.type_i32(), nregions);
    b->append(o);
    return o;
}
void yield1(Context& ctx, const Ops& o, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(ctx.create_operation(o.yield, ConstSpan<Value*>(a, 1U), 0U));
}
void ret1(Context& ctx, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(func::create_return(ctx, ConstSpan<Value*>(a, 1U)));
}
void ret0(Context& ctx, Block* b) { b->append(func::create_return(ctx, {})); }
Operation* calln(Context& ctx, containers::StringView name, Value* a0, Block* b)
{
    Value* a[1] = {a0};
    Operation* const c = func::create_call(ctx, name, ConstSpan<Value*>(a, 1U), 1U);
    b->append(c);
    return c;
}

i64 run(Context& ctx, Module& m, containers::StringView entry, ConstSpan<i64> args, exec::ExecError& err)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    const exec::ExecResult r = in.invoke(m, entry, args);
    err                      = r.error;
    return (r.ok() && r.values.size() > 0U) ? r.values[0] : 0;
}
i64 run_ok(Context& ctx, Module& m, containers::StringView entry, ConstSpan<i64> args)
{
    exec::ExecError e = exec::ExecError::None;
    const i64       v = run(ctx, m, entry, args, e);
    REQUIRE(e == exec::ExecError::None);
    return v;
}
} // namespace

TEST_CASE("ceir exec: arith const/addi/muli/cmpi reference semantics", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();

    Operation* const fa = mkfunc(ctx, *m, "add", 0U);
    Block* const     ba = func::func_body_block(fa);
    ret1(ctx, ba, bin(ctx, o.addi, konst(ctx, o, ba, 20)->result(0U), konst(ctx, o, ba, 22)->result(0U), ba)->result(0U));
    CHECK(run_ok(ctx, *m, "add", {}) == 42);

    Operation* const fm = mkfunc(ctx, *m, "mul", 0U);
    Block* const     bm = func::func_body_block(fm);
    ret1(ctx, bm, bin(ctx, o.muli, konst(ctx, o, bm, 6)->result(0U), konst(ctx, o, bm, 7)->result(0U), bm)->result(0U));
    CHECK(run_ok(ctx, *m, "mul", {}) == 42);

    Operation* const fc = mkfunc(ctx, *m, "lt", 0U);
    Block* const     bc = func::func_body_block(fc);
    Operation* const cm = bin(ctx, o.cmpi, konst(ctx, o, bc, 3)->result(0U), konst(ctx, o, bc, 5)->result(0U), bc);
    ctx.set_attr(cm, "predicate", ctx.attr_string("slt"));
    ret1(ctx, bc, cm->result(0U));
    CHECK(run_ok(ctx, *m, "lt", {}) == 1);
}

TEST_CASE("ceir exec: addi wraps two's-complement (u64 cast, no signed UB)", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Operation* const             f  = mkfunc(ctx, *m, "wrap", 0U);
    Block* const                 b  = func::func_body_block(f);
    Operation* const             s  = bin(ctx, o.addi, konst(ctx, o, b, static_cast<i64>(0x7FFFFFFFFFFFFFFFLL))->result(0U),
                              konst(ctx, o, b, 1)->result(0U), b);
    ret1(ctx, b, s->result(0U));
    CHECK(run_ok(ctx, *m, "wrap", {}) == static_cast<i64>(0x8000000000000000ULL)); // INT64_MIN (wrapped)
}

TEST_CASE("ceir exec: core.if selects the branch and forwards the yield to results", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             f = mkfunc(ctx, *m, "pick", 1U);
    Block* const                 b = func::func_body_block(f);
    Operation* const             ifop = unary(ctx, o.cif, b->arg(0U), b, 1U, 2U);
    Block* const                 tb   = ctx.create_block(0U);
    ifop->region(0)->append(tb);
    yield1(ctx, o, tb, konst(ctx, o, tb, 10)->result(0U));
    Block* const eb = ctx.create_block(0U);
    ifop->region(1)->append(eb);
    yield1(ctx, o, eb, konst(ctx, o, eb, 20)->result(0U));
    ret1(ctx, b, ifop->result(0U));

    i64 one[1] = {1};
    i64 zero[1] = {0};
    CHECK(run_ok(ctx, *m, "pick", ConstSpan<i64>(one, 1U)) == 10);
    CHECK(run_ok(ctx, *m, "pick", ConstSpan<i64>(zero, 1U)) == 20);
}

TEST_CASE("ceir exec: a for loop with a state accumulator sums 0..n-1", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             f = mkfunc(ctx, *m, "main", 1U);
    Block* const                 b = func::func_body_block(f);
    Value*  lohilst[3] = {konst(ctx, o, b, 0)->result(0U), b->arg(0U), konst(ctx, o, b, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    b->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(body);
    Operation* const init = konst(ctx, o, body, 0);
    Value*           sops[2] = {init->result(0U), init->result(0U)};
    Operation* const acc = ctx.create_operation(o.state, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    body->append(acc);
    acc->set_operand(1U, bin(ctx, o.addi, acc->result(0U), body->arg(0U), body)->result(0U)); // next = acc + iv
    ret0(ctx, b);

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    i64                    five[1] = {5};
    const exec::ExecResult r       = in.invoke(*m, "main", ConstSpan<i64>(five, 1U));
    REQUIRE(r.ok());
    i64 sum = -1;
    REQUIRE(in.cell_value(acc, sum));
    CHECK(sum == 0 + 1 + 2 + 3 + 4); // 10
}

TEST_CASE("ceir exec: the state ring (history(1) is a plain register; history(3) delays by three)", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    // build @main(): for(0,iters,1) { h = history(depth, init=-1); next = iv }, observe cell_value(h) after.
    const auto history_after = [&root](i64 iters, crd::u32 depth) -> i64 {
        Context    c(&root);
        const Ops  oo(c);
        Module*    mm = c.create_module();
        Operation* f  = mkfunc(c, *mm, "main", 0U);
        Block*     b  = func::func_body_block(f);
        Value* lh[3] = {konst(c, oo, b, 0)->result(0U), konst(c, oo, b, iters)->result(0U),
                        konst(c, oo, b, 1)->result(0U)};
        Operation* forop = c.create_operation(oo.cfor, ConstSpan<Value*>(lh, 3U), 0U, {}, 1U);
        b->append(forop);
        Block* body = c.create_block(1U, c.type_i32());
        forop->region(0)->append(body);
        Value*     sops[2] = {konst(c, oo, body, -1)->result(0U), konst(c, oo, body, -1)->result(0U)};
        Operation* h       = c.create_operation(oo.history, ConstSpan<Value*>(sops, 2U), 1U, c.type_i32());
        c.set_attr(h, "depth", c.attr_int(static_cast<i64>(depth)));
        body->append(h);
        h->set_operand(1U, bin(c, oo.addi, body->arg(0U), konst(c, oo, body, 0)->result(0U), body)->result(0U)); // next=iv
        ret0(c, b);
        exec::Interpreter in(c);
        exec::install_builtin_semantics(in);
        REQUIRE(in.invoke(*mm, "main", {}).ok());
        i64 cur = -999;
        REQUIRE(in.cell_value(h, cur));
        return cur;
    };
    CHECK(history_after(6, 3U) == 3);  // pushed 0..5; current after 6 latches = pushed[6-3] = 3
    CHECK(history_after(6, 1U) == 5);  // a plain register: the last pushed
    CHECK(history_after(2, 3U) == -1); // still warming up -> init
}

TEST_CASE("ceir exec: a func.call binds args, returns, and a state cell persists across CALLS", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Operation* const             fa = mkfunc(ctx, *m, "acc", 1U);
    Block* const                 ba = func::func_body_block(fa);
    Value*                       sops[2] = {konst(ctx, o, ba, 0)->result(0U), konst(ctx, o, ba, 0)->result(0U)};
    Operation* const             c  = ctx.create_operation(o.state, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    ba->append(c);
    Operation* const n = bin(ctx, o.addi, c->result(0U), ba->arg(0U), ba);
    c->set_operand(1U, n->result(0U));
    ret1(ctx, ba, n->result(0U));

    Operation* const fmain = mkfunc(ctx, *m, "main", 0U);
    Block* const     bmain = func::func_body_block(fmain);
    (void)calln(ctx, "acc", konst(ctx, o, bmain, 10)->result(0U), bmain);
    (void)calln(ctx, "acc", konst(ctx, o, bmain, 5)->result(0U), bmain);
    Operation* const last = calln(ctx, "acc", konst(ctx, o, bmain, 0)->result(0U), bmain);
    ret1(ctx, bmain, last->result(0U));
    CHECK(run_ok(ctx, *m, "main", {}) == 15); // 10 + 5 + 0, the cell accumulated across calls
}

TEST_CASE("ceir exec: switch selects a region; an out-of-range selector errors", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Operation* const             f  = mkfunc(ctx, *m, "pick", 1U);
    Block* const                 b  = func::func_body_block(f);
    Operation* const             sw = unary(ctx, o.cswitch, b->arg(0U), b, 0U, 2U);
    Block* const                 a0 = ctx.create_block(0U);
    sw->region(0)->append(a0);
    (void)konst(ctx, o, a0, 111);
    Block* const a1 = ctx.create_block(0U);
    sw->region(1)->append(a1);
    (void)konst(ctx, o, a1, 222);
    ret0(ctx, b);

    exec::ExecError e = exec::ExecError::None;
    i64             s0[1] = {0};
    i64             s1[1] = {1};
    i64             s2[1] = {2};
    i64             sneg[1] = {-1};
    (void)run(ctx, *m, "pick", ConstSpan<i64>(s0, 1U), e);
    CHECK(e == exec::ExecError::None);
    (void)run(ctx, *m, "pick", ConstSpan<i64>(s1, 1U), e);
    CHECK(e == exec::ExecError::None);
    (void)run(ctx, *m, "pick", ConstSpan<i64>(s2, 1U), e);
    CHECK(e == exec::ExecError::SelectorOutOfRange);
    (void)run(ctx, *m, "pick", ConstSpan<i64>(sneg, 1U), e);
    CHECK(e == exec::ExecError::SelectorOutOfRange);
}

TEST_CASE("ceir exec: the typed error modes", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();

    SECTION("a missing entry function is NoEntry")
    {
        exec::ExecError e = exec::ExecError::None;
        (void)run(ctx, *m, "nope", {}, e);
        CHECK(e == exec::ExecError::NoEntry);
    }
    SECTION("a for loop with step 0 is BadForStep")
    {
        Operation* const f = mkfunc(ctx, *m, "bad", 0U);
        Block* const     b = func::func_body_block(f);
        Value* lh[3] = {konst(ctx, o, b, 0)->result(0U), konst(ctx, o, b, 5)->result(0U),
                        konst(ctx, o, b, 0)->result(0U)}; // step 0
        Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lh, 3U), 0U, {}, 1U);
        b->append(forop);
        forop->region(0)->append(ctx.create_block(1U, ctx.type_i32()));
        ret0(ctx, b);
        exec::ExecError e = exec::ExecError::None;
        (void)run(ctx, *m, "bad", {}, e);
        CHECK(e == exec::ExecError::BadForStep);
    }
    SECTION("an unresolved call is UnresolvedCall")
    {
        Operation* const f = mkfunc(ctx, *m, "u", 0U);
        Block* const     b = func::func_body_block(f);
        b->append(func::create_call(ctx, "ghost", {}, 0U));
        ret0(ctx, b);
        exec::ExecError e = exec::ExecError::None;
        (void)run(ctx, *m, "u", {}, e);
        CHECK(e == exec::ExecError::UnresolvedCall);
    }
    SECTION("an unrecognized cmpi predicate is UnknownPredicate")
    {
        Operation* const f = mkfunc(ctx, *m, "p", 0U);
        Block* const     b = func::func_body_block(f);
        Operation* const cm = bin(ctx, o.cmpi, konst(ctx, o, b, 1)->result(0U), konst(ctx, o, b, 2)->result(0U), b);
        ctx.set_attr(cm, "predicate", ctx.attr_string("bogus"));
        ret1(ctx, b, cm->result(0U));
        exec::ExecError e = exec::ExecError::None;
        (void)run(ctx, *m, "p", {}, e);
        CHECK(e == exec::ExecError::UnknownPredicate);
    }
    SECTION("core.foreach has no installed semantics -> NoSemantics (named deferral)")
    {
        Operation* const f  = mkfunc(ctx, *m, "fe", 0U);
        Block* const     b  = func::func_body_block(f);
        Operation* const fe = unary(ctx, o.foreach, konst(ctx, o, b, 3)->result(0U), b, 0U, 1U);
        fe->region(0)->append(ctx.create_block(1U, ctx.type_i32()));
        ret0(ctx, b);
        exec::ExecError e = exec::ExecError::None;
        (void)run(ctx, *m, "fe", {}, e);
        CHECK(e == exec::ExecError::NoSemantics);
    }
    SECTION("a runaway while is FuelExhausted, not a hang")
    {
        Operation* const f = mkfunc(ctx, *m, "spin", 0U);
        Block* const     b = func::func_body_block(f);
        Operation* const w = ctx.create_operation(o.cwhile, {}, 0U, {}, 2U);
        b->append(w);
        Block* const cond = ctx.create_block(0U);
        w->region(0)->append(cond);
        yield1(ctx, o, cond, konst(ctx, o, cond, 1)->result(0U)); // always true
        w->region(1)->append(ctx.create_block(0U));               // empty body
        ret0(ctx, b);
        exec::Interpreter in(ctx, 1000U); // a small fuel budget
        exec::install_builtin_semantics(in);
        CHECK(in.invoke(*m, "spin", {}).error == exec::ExecError::FuelExhausted);
    }
}

TEST_CASE("ceir exec: open-world -- a hand-registered dialect op evaluates via an installed EvalFn", "[ceir][exec]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   dbl = ctx.register_dialect("hw")->register_op("dbl", {}); // custom "double" op
    Module* const                m   = ctx.create_module();
    Operation* const             f   = mkfunc(ctx, *m, "main", 0U);
    Block* const                 b   = func::func_body_block(f);
    Operation* const             d   = unary(ctx, dbl, konst(ctx, o, b, 21)->result(0U), b, 1U, 0U);
    ret1(ctx, b, d->result(0U));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    CHECK(in.invoke(*m, "main", {}).error == exec::ExecError::NoSemantics); // hw.dbl not installed
    in.install(dbl, [](exec::Interpreter& it, const Operation& op) -> exec::ExecError {
        i64 x = 0;
        if (!it.value_of(op.operand(0U), x)) { return it.fail(exec::ExecError::UndefinedValue, &op); }
        it.set_value(op.result(0U), x * 2);
        return exec::ExecError::None;
    });
    const exec::ExecResult r = in.invoke(*m, "main", {});
    REQUIRE(r.ok());
    CHECK(r.values[0] == 42);
}

TEST_CASE("ceir exec: a prototype clone runs correctly with independent per-clone state + scratch", "[ceir][exec]")
{
    // CEIR-6b: the parallel provider builds one installed PROTOTYPE, then constructs a fresh Interpreter per range from
    // it -- each with its OWN scratch allocator (so worker ranges never touch the shared Context arena) and INDEPENDENT
    // state cells / env / fuel. This pins that contract.
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // @acc(%d): a cross-call state counter; @main(): acc(10); acc(5); return acc(0) == 15 on a FRESH session.
    Module* const    m  = ctx.create_module();
    Operation* const fa = mkfunc(ctx, *m, "acc", 1U);
    Block* const     ba = func::func_body_block(fa);
    Value*           sops[2] = {konst(ctx, o, ba, 0)->result(0U), konst(ctx, o, ba, 0)->result(0U)};
    Operation* const c  = ctx.create_operation(o.state, ConstSpan<Value*>(sops, 2U), 1U, ctx.type_i32());
    ba->append(c);
    Operation* const n = bin(ctx, o.addi, c->result(0U), ba->arg(0U), ba);
    c->set_operand(1U, n->result(0U));
    ret1(ctx, ba, n->result(0U));
    Operation* const fmain = mkfunc(ctx, *m, "main", 0U);
    Block* const     bmain = func::func_body_block(fmain);
    (void)calln(ctx, "acc", konst(ctx, o, bmain, 10)->result(0U), bmain);
    (void)calln(ctx, "acc", konst(ctx, o, bmain, 5)->result(0U), bmain);
    ret1(ctx, bmain, calln(ctx, "acc", konst(ctx, o, bmain, 0)->result(0U), bmain)->result(0U));

    exec::Interpreter proto(ctx);
    exec::install_builtin_semantics(proto);

    crd::memory::MallocAllocator sa; // clone A's own scratch
    crd::memory::MallocAllocator sb; // clone B's own scratch
    exec::Interpreter            a(proto, &sa, crd::u64{1} << 20U);
    exec::Interpreter            b(proto, &sb, crd::u64{1} << 20U);
    CHECK(a.allocator() == &sa); // each clone uses its OWN scratch (not the Context arena)
    CHECK(b.allocator() == &sb);

    CHECK(a.invoke(*m, "main", {}).values[0] == 15); // fresh session in A: 10+5+0
    CHECK(b.invoke(*m, "main", {}).values[0] == 15); // B is INDEPENDENT of A (its own fresh cells), not 25
    CHECK(a.invoke(*m, "main", {}).values[0] == 30); // A's cell PERSISTED across A's invokes (15 + 10+5+0)
    CHECK(b.invoke(*m, "main", {}).values[0] == 30); // B persisted independently
}
