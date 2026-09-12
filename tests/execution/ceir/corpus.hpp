#pragma once
// CEIR-11b stage 5 — the SHARED corpus builder header (the rich_graph.hpp / 1f precedent: build each program ONCE, run
// it through BOTH engines). The CEIR-11b differential (test_plan.cpp) runs every program through the reference
// interpreter AND the compiled plan and byte-compares values + cells + map_outputs + error. ⛔ build_5z is REUSED EXACTLY
// by test_band5_gate.cpp (no copy-pasted IR that could drift — the "richest single program" is defined ONCE, here).
//
// Each builder returns `Built` = the module + the §20 state ops and the data-parallel ops IN COMPILE ORDER (the dense
// cell / map indices are assigned by compile order, so `cells[i]`/`map_outputs[j]` line up with `handles.cells[i]` /
// `.maps[j]`). Inspection is HANDLE-BASED (the reference `cell_value(op)` / `map_output(op)` key on the live Operation*;
// a round-tripped twin has different pointers, so the §121 twin compares the dense ARRAYS, not handles).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>

namespace crd::ceir::corpus
{
// The op-id kit + dialect registration (arith/core/func/async/task) — one per Context.
struct Kit
{
    OpId cst, addi, muli, cmpi, cfor, cmatch, cif, state, yield;
    OpId launch, awaitk, join, cont;
    OpId spawn, mainth, worker, group, fiber, pfor, mreduce;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          muli(ctx.intern_op("arith", "muli")), cmpi(ctx.intern_op("arith", "cmpi")), cfor(ctx.intern_op("core", "for")),
          cmatch(ctx.intern_op("core", "match")), cif(ctx.intern_op("core", "if")),
          state(ctx.intern_op("core", "state")), yield(ctx.intern_op("core", "yield")),
          launch(ctx.intern_op("async", "launch")), awaitk(ctx.intern_op("async", "await")),
          join(ctx.intern_op("async", "join")), cont(ctx.intern_op("task", "continuation")),
          spawn(ctx.intern_op("task", "spawn")), mainth(ctx.intern_op("task", "main_thread")),
          worker(ctx.intern_op("task", "worker")), group(ctx.intern_op("task", "group")),
          fiber(ctx.intern_op("task", "fiber_wait")), pfor(ctx.intern_op("task", "parallel_for")),
          mreduce(ctx.intern_op("task", "map_reduce"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)async::register_async_ops(ctx);
        (void)task::register_task_ops(ctx);
    }
};

// a program + the state / data-parallel op handles IN COMPILE ORDER (for the reference cell_value/map_output inspection).
struct Built
{
    Module*                             m;
    containers::Array<Operation*>       cells; // §20 state ops, dense-cell order
    containers::Array<Operation*>       maps;  // parallel_for / map_reduce ops, dense-map order
    explicit Built(memory::IAllocator* a) : m(nullptr), cells(a), maps(a) {}
};

// ── shared builder helpers ──
inline Operation* konst(Context& ctx, const Kit& k, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
inline Operation* bin(Context& ctx, OpId op, Value* a, Value* b2, Block* b)
{
    Value* ops[2] = {a, b2};
    Operation* const o = ctx.create_operation(op, containers::ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(o);
    return o;
}
inline Operation* mk_state(Context& ctx, const Kit& k, Value* init, Value* next, Block* b)
{
    Value* ops[2] = {init, next};
    Operation* const s = ctx.create_operation(k.state, containers::ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(s);
    return s;
}
inline Operation* call_fn(Context& ctx, containers::StringView name, Value* arg, Block* b)
{
    Value* a[1] = {arg};
    Operation* const c = func::create_call(ctx, name, containers::ConstSpan<Value*>(a, 1U), 1U, ctx.type_i32());
    b->append(c);
    return c;
}
inline void ret1(Context& ctx, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(func::create_return(ctx, containers::ConstSpan<Value*>(a, 1U)));
}
inline void yield1(Context& ctx, const Kit& k, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(ctx.create_operation(k.yield, containers::ConstSpan<Value*>(a, 1U), 0U));
}
inline Operation* mkfunc(Context& ctx, Module& m, containers::StringView name, crd::u32 nparams, Block*& body_out)
{
    Operation* const f = func::create_func(ctx, m, name, Visibility::Public, nparams, ctx.type_i32());
    Block*           mb = m.body()->first_block();
    if (mb == nullptr)
    {
        mb = ctx.create_block(0U);
        m.body()->append(mb);
    }
    mb->append(f);
    body_out = func::func_body_block(f);
    return f;
}
// a `<dialect>.<name> { yield const v }` token op (launch/spawn/...): result(0) is the token handle.
inline Operation* mk_tok(Context& ctx, const Kit& k, Block* b, OpId kind, i64 v)
{
    Operation* const l  = ctx.create_operation(kind, {}, 1U, ctx.type_i32(), 1U);
    Block* const     lb = ctx.create_block(0U);
    l->region(0)->append(lb);
    yield1(ctx, k, lb, konst(ctx, k, lb, v)->result(0U));
    b->append(l);
    return l;
}
inline Operation* mk_await(Context& ctx, const Kit& k, Block* b, OpId kind, Value* tok)
{
    (void)k; // symmetry with mk_tok; the await kind is passed explicitly
    Value* a[1] = {tok};
    Operation* const w = ctx.create_operation(kind, containers::ConstSpan<Value*>(a, 1U), 1U, ctx.type_i32());
    b->append(w);
    return w;
}

// ── P1 (5z): the pinned BAND-5 gate program (loop + match + value-producing if + calls + a §20 accumulator across calls)
//    — REUSED EXACTLY by test_band5_gate.cpp. @main(6) -> 118; the in-loop cell = 15; @acc's cross-call cell = 18. ──
inline Built build_5z(Context& ctx, const Kit& o)
{
    Built out(ctx.allocator());
    Module* const m = ctx.create_module();
    out.m           = m;

    Block*           ba   = nullptr; // @acc(%delta): a running total across calls
    (void)mkfunc(ctx, *m, "acc", 1U, ba);
    Operation* const z    = konst(ctx, o, ba, 0);
    Operation* const cell = mk_state(ctx, o, z->result(0U), z->result(0U), ba); // next wired below
    Operation* const nsum = bin(ctx, o.addi, cell->result(0U), ba->arg(0U), ba);
    cell->set_operand(1U, nsum->result(0U));
    ret1(ctx, ba, nsum->result(0U));

    Block*           bm = nullptr; // @main(%n)
    (void)mkfunc(ctx, *m, "main", 1U, bm);
    Operation* const half = konst(ctx, o, bm, 3);
    Operation* const two  = konst(ctx, o, bm, 2);
    Operation* const zero = konst(ctx, o, bm, 0);
    Operation* const one  = konst(ctx, o, bm, 1);

    Value* lohilst[3] = {zero->result(0U), bm->arg(0U), one->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, containers::ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    bm->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_i32()); // %iv
    forop->region(0)->append(body);

    Operation* const sel = bin(ctx, o.cmpi, body->arg(0U), half->result(0U), body); // %sel = iv < half
    ctx.set_attr(sel, "predicate", ctx.attr_string("slt"));
    Value* selarr[1] = {sel->result(0U)};
    Operation* const mt = ctx.create_operation(o.cmatch, containers::ConstSpan<Value*>(selarr, 1U), 0U, {}, 2U);
    body->append(mt);
    Block* const arm0 = ctx.create_block(0U); // sel==0 (iv>=half): acc(iv)
    mt->region(0)->append(arm0);
    (void)call_fn(ctx, "acc", body->arg(0U), arm0);
    Block* const arm1 = ctx.create_block(0U); // sel==1 (iv<half): acc(2*iv)
    mt->region(1)->append(arm1);
    Operation* const d = bin(ctx, o.muli, body->arg(0U), two->result(0U), arm1);
    (void)call_fn(ctx, "acc", d->result(0U), arm1);

    // the in-loop feedback cell: %a2 = state(0, %n2); %n2 = addi(%a2, %iv) — dense cell 0 (compiled inside main's body)
    Operation* const cell2 = mk_state(ctx, o, zero->result(0U), zero->result(0U), body);
    Operation* const n2    = bin(ctx, o.addi, cell2->result(0U), body->arg(0U), body);
    cell2->set_operand(1U, n2->result(0U));

    Operation* const final    = call_fn(ctx, "acc", zero->result(0U), bm);
    Operation* const eighteen = konst(ctx, o, bm, 18);
    Operation* const okc      = bin(ctx, o.cmpi, final->result(0U), eighteen->result(0U), bm); // final == 18
    ctx.set_attr(okc, "predicate", ctx.attr_string("eq"));
    Value* condarr[1] = {okc->result(0U)};
    Operation* const ifop = ctx.create_operation(o.cif, containers::ConstSpan<Value*>(condarr, 1U), 1U, ctx.type_i32(), 2U);
    bm->append(ifop);
    Block* const thenb = ctx.create_block(0U);
    ifop->region(0)->append(thenb);
    yield1(ctx, o, thenb, konst(ctx, o, thenb, 100)->result(0U));
    Block* const elseb = ctx.create_block(0U);
    ifop->region(1)->append(elseb);
    yield1(ctx, o, elseb, konst(ctx, o, elseb, 200)->result(0U));
    ret1(ctx, bm, bin(ctx, o.addi, final->result(0U), ifop->result(0U), bm)->result(0U));

    out.cells.push_back(cell2); // dense cell 0 (in main's body, compiled first)
    out.cells.push_back(cell);  // dense cell 1 (@acc compiled off the worklist after main)
    return out;
}

// ── P2 (6z): the non-associative map_reduce (map iv*iv; combine acc*31 + elem — index-ordered fold). @main() -> the
//    reduced scalar; the map = [0,1,4,9,16] for n=5. ──
inline Built build_6z(Context& ctx, const Kit& o)
{
    Built out(ctx.allocator());
    Module* const m = ctx.create_module();
    out.m           = m;
    Block*           bm = nullptr;
    (void)mkfunc(ctx, *m, "main", 0U, bm);
    Value* r4[4] = {konst(ctx, o, bm, 0)->result(0U), konst(ctx, o, bm, 5)->result(0U),
                    konst(ctx, o, bm, 1)->result(0U), konst(ctx, o, bm, 0)->result(0U)}; // lo,hi,step,init
    Operation* const mr = ctx.create_operation(o.mreduce, containers::ConstSpan<Value*>(r4, 4U), 1U, ctx.type_i32(), 2U);
    bm->append(mr);
    Block* const mapb = ctx.create_block(1U, ctx.type_i32());
    mr->region(0)->append(mapb);
    yield1(ctx, o, mapb, bin(ctx, o.muli, mapb->arg(0U), mapb->arg(0U), mapb)->result(0U)); // iv*iv
    Block* const cb = ctx.create_block(2U, ctx.type_i32());
    mr->region(1)->append(cb);
    Value* const acc31 = bin(ctx, o.muli, cb->arg(0U), konst(ctx, o, cb, 31)->result(0U), cb)->result(0U); // acc*31
    yield1(ctx, o, cb, bin(ctx, o.addi, acc31, cb->arg(1U), cb)->result(0U));                               // + elem
    ret1(ctx, bm, mr->result(0U));
    out.maps.push_back(mr); // dense map 0
    return out;
}

// ── P3 (async): launch/await, join, continuation — @main() -> (7) + (3+4) + (10+5) = 29. ──
inline Built build_async(Context& ctx, const Kit& o)
{
    Built out(ctx.allocator());
    Module* const m = ctx.create_module();
    out.m           = m;
    Block*           bm = nullptr;
    (void)mkfunc(ctx, *m, "main", 0U, bm);
    Operation* const t  = mk_tok(ctx, o, bm, o.launch, 7);
    Operation* const aw = mk_await(ctx, o, bm, o.awaitk, t->result(0U)); // 7
    Operation* const la = mk_tok(ctx, o, bm, o.launch, 3);
    Operation* const lb = mk_tok(ctx, o, bm, o.launch, 4);
    Value* jt[2] = {la->result(0U), lb->result(0U)};
    Operation* const j = ctx.create_operation(o.join, containers::ConstSpan<Value*>(jt, 2U), 1U, ctx.type_i32());
    bm->append(j);
    Value* jat[1] = {j->result(0U)};
    Operation* const jaw = ctx.create_operation(o.awaitk, containers::ConstSpan<Value*>(jat, 1U), 2U, ctx.type_i32());
    bm->append(jaw); // await join -> [3,4]
    Value* const jsum = bin(ctx, o.addi, jaw->result(0U), jaw->result(1U), bm)->result(0U); // 3+4 = 7
    // continuation: launch{10} -> continuation{(x): x + 5} -> await -> 15
    Operation* const lc = mk_tok(ctx, o, bm, o.launch, 10);
    Value* ct[1] = {lc->result(0U)};
    Operation* const cn = ctx.create_operation(o.cont, containers::ConstSpan<Value*>(ct, 1U), 1U, ctx.type_i32(), 1U);
    Block* const cnb = ctx.create_block(1U, ctx.type_i32());
    cn->region(0)->append(cnb);
    yield1(ctx, o, cnb, bin(ctx, o.addi, cnb->arg(0U), konst(ctx, o, cnb, 5)->result(0U), cnb)->result(0U));
    bm->append(cn);
    Operation* const caw = mk_await(ctx, o, bm, o.awaitk, cn->result(0U)); // 15
    Value* const s1 = bin(ctx, o.addi, aw->result(0U), jsum, bm)->result(0U);   // 7 + 7 = 14
    ret1(ctx, bm, bin(ctx, o.addi, s1, caw->result(0U), bm)->result(0U));       // 14 + 15 = 29
    return out;
}

// ── P4 (the six task ops): spawn/main_thread/worker (launch-likes) + group (scope) + fiber_wait (await) +
//    continuation. @main() -> 1 + 2 + 3 + 40 (continuation of a spawn) = 46. ──
inline Built build_tasks(Context& ctx, const Kit& o)
{
    Built out(ctx.allocator());
    Module* const m = ctx.create_module();
    out.m           = m;
    Block*           bm = nullptr;
    (void)mkfunc(ctx, *m, "main", 0U, bm);
    Operation* const s1 = mk_tok(ctx, o, bm, o.spawn, 1);
    Operation* const w1 = mk_await(ctx, o, bm, o.fiber, s1->result(0U)); // spawn -> fiber_wait -> 1
    Operation* const s2 = mk_tok(ctx, o, bm, o.mainth, 2);
    Operation* const w2 = mk_await(ctx, o, bm, o.fiber, s2->result(0U)); // main_thread -> 2
    Operation* const s3 = mk_tok(ctx, o, bm, o.worker, 3);
    Operation* const w3 = mk_await(ctx, o, bm, o.fiber, s3->result(0U)); // worker -> 3
    // task.group { yield 40 } (a scope boundary that forwards its yield)
    Operation* const g = ctx.create_operation(o.group, {}, 1U, ctx.type_i32(), 1U);
    Block* const gb = ctx.create_block(0U);
    g->region(0)->append(gb);
    yield1(ctx, o, gb, konst(ctx, o, gb, 40)->result(0U));
    bm->append(g); // group -> 40
    Value* const a = bin(ctx, o.addi, w1->result(0U), w2->result(0U), bm)->result(0U); // 1 + 2
    Value* const b = bin(ctx, o.addi, w3->result(0U), g->result(0U), bm)->result(0U);  // 3 + 40
    ret1(ctx, bm, bin(ctx, o.addi, a, b, bm)->result(0U));                             // 3 + 43 = 46
    return out;
}

// ── P5 (composing): arith + calls + async + task + TWO §20 cells + a bounded for + a parallel_for, all in one program.
//    @main() -> a_read(0) + b_read(5) + call inc(5)=6 + await(7) + fiber_wait(100) = 118. Cells latch to [10, 20];
//    the parallel_for map = [0,1,4]. The cross-dialect DoD program of the differential (the 11a-composing intent). ──
inline Built build_composing(Context& ctx, const Kit& o)
{
    Built out(ctx.allocator());
    Module* const m = ctx.create_module();
    out.m           = m;
    Block*           bi = nullptr; // @inc(%x){ return x + 1 }
    (void)mkfunc(ctx, *m, "inc", 1U, bi);
    ret1(ctx, bi, bin(ctx, o.addi, bi->arg(0U), konst(ctx, o, bi, 1)->result(0U), bi)->result(0U));

    Block*           bm = nullptr; // @main()
    (void)mkfunc(ctx, *m, "main", 0U, bm);
    Operation* const ca = mk_state(ctx, o, konst(ctx, o, bm, 0)->result(0U), konst(ctx, o, bm, 10)->result(0U), bm); // cell 0
    Operation* const cb = mk_state(ctx, o, konst(ctx, o, bm, 5)->result(0U), konst(ctx, o, bm, 20)->result(0U), bm); // cell 1
    Operation* const c  = call_fn(ctx, "inc", konst(ctx, o, bm, 5)->result(0U), bm);                                 // 6
    Operation* const t  = mk_tok(ctx, o, bm, o.launch, 7);
    Operation* const aw = mk_await(ctx, o, bm, o.awaitk, t->result(0U)); // 7
    Operation* const sp = mk_tok(ctx, o, bm, o.spawn, 100);
    Operation* const fw = mk_await(ctx, o, bm, o.fiber, sp->result(0U)); // 100
    // a bounded control-flow for (no state/yield: a pure statement exercising the loop)
    Value* lohilst[3] = {konst(ctx, o, bm, 0)->result(0U), konst(ctx, o, bm, 4)->result(0U),
                         konst(ctx, o, bm, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, containers::ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    bm->append(forop);
    Block* const fbody = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(fbody);
    (void)bin(ctx, o.muli, fbody->arg(0U), fbody->arg(0U), fbody); // iv*iv (dead — the loop is a pure statement)
    // a parallel_for map (iv*iv over [0,3)) -> map [0,1,4]
    Value* pr[3] = {konst(ctx, o, bm, 0)->result(0U), konst(ctx, o, bm, 3)->result(0U),
                    konst(ctx, o, bm, 1)->result(0U)};
    Operation* const pf = ctx.create_operation(o.pfor, containers::ConstSpan<Value*>(pr, 3U), 0U, {}, 1U);
    bm->append(pf);
    Block* const pbody = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(pbody);
    yield1(ctx, o, pbody, bin(ctx, o.muli, pbody->arg(0U), pbody->arg(0U), pbody)->result(0U)); // iv*iv
    // return a_read(0) + b_read(5) + c(6) + aw(7) + fw(100) = 118
    Value* const p1 = bin(ctx, o.addi, ca->result(0U), cb->result(0U), bm)->result(0U); // 0 + 5
    Value* const p2 = bin(ctx, o.addi, c->result(0U), aw->result(0U), bm)->result(0U);  // 6 + 7
    Value* const p3 = bin(ctx, o.addi, p1, p2, bm)->result(0U);                          // 5 + 13 = 18
    ret1(ctx, bm, bin(ctx, o.addi, p3, fw->result(0U), bm)->result(0U));                 // 18 + 100 = 118

    out.cells.push_back(ca); // dense cell 0
    out.cells.push_back(cb); // dense cell 1
    out.maps.push_back(pf);  // dense map 0
    return out;
}
} // namespace crd::ceir::corpus
