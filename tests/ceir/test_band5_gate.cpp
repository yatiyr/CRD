// CEIR-5z BAND-5 GATE (sec 118): a pinned NONTRIVIAL program -- a bounded core.for loop, a core.match selecting the
// accumulation per iteration, a value-producing core.if, ceir.func calls, and a sec 20 state<T> accumulator that persists
// ACROSS calls -- executes in
// the reference executor to a byte-pinned result, IDENTICAL from the builder-built form and the text-parsed form. This is
// the whole band: the executor + the round-trip + state/control-flow semantics, in one assertion. ASCII names.
//
// The program exercises for + match + a value-producing if + calls + a state<T> accumulator (across calls):
//   func.func @acc(%delta): %c = state(%zero, %n); %n = addi(%c, %delta); return %n   -- a running total across CALLS
//   func.func @main(%n):
//     for iv in [0,%n): %sel = cmpi slt(%iv,%half); match(%sel){ acc(%iv) }{ %d=muli(%iv,2); acc(%d) };
//                       %a2 = state(0,%n2); %n2 = addi(%a2,%iv)               -- a second, in-loop feedback cell
//     %final = call @acc(0); %ok = cmpi eq(%final, 18);
//     %bonus = if(%ok) { yield 100 } { yield 200 };   -- a value-producing core.if (yield -> results)
//     return addi(%final, %bonus)
//   @main(6) with half=3  ->  acc total = sum(iv<3 ? 2*iv : iv) = (0+2+4)+(3+4+5) = 18 ; ok=1 -> bonus=100 -> 18+100 = 118.
//   The in-loop cell = sum iv = 15.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;

namespace
{
struct Ops
{
    OpId cst, addi, muli, cmpi, cfor, cmatch, cif, state;
    explicit Ops(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          muli(ctx.intern_op("arith", "muli")), cmpi(ctx.intern_op("arith", "cmpi")), cfor(ctx.intern_op("core", "for")),
          cmatch(ctx.intern_op("core", "match")), cif(ctx.intern_op("core", "if")), state(ctx.intern_op("core", "state"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)func::register_dialect(ctx);
    }
};
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
Operation* mk_state(Context& ctx, const Ops& o, Value* init, Value* next, Block* b)
{
    Value* ops[2] = {init, next};
    Operation* const s = ctx.create_operation(o.state, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(s);
    return s;
}
Operation* callacc(Context& ctx, Value* arg, Block* b)
{
    Value* a[1] = {arg};
    Operation* const c = func::create_call(ctx, "acc", ConstSpan<Value*>(a, 1U), 1U, ctx.type_i32());
    b->append(c);
    return c;
}
void ret1(Context& ctx, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(func::create_return(ctx, ConstSpan<Value*>(a, 1U)));
}
void yield1(Context& ctx, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(ctx.create_operation(ctx.intern_op("core", "yield"), ConstSpan<Value*>(a, 1U), 0U));
}
Operation* mkfunc(Context& ctx, Module& m, containers::StringView name, crd::u32 nparams, Block*& body_out)
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

// Build the pinned program into a fresh module; return the module + the in-loop 2nd cell op (for cell_value inspection).
struct Built
{
    Module*    m;
    Operation* cell2;
};
Built build_gate(Context& ctx, const Ops& o)
{
    Module* const m = ctx.create_module();

    // @acc(%delta): a running total across calls.
    Block*           ba   = nullptr;
    Operation* const facc = mkfunc(ctx, *m, "acc", 1U, ba);
    (void)facc;
    Operation* const z    = konst(ctx, o, ba, 0);
    Operation* const cell = mk_state(ctx, o, z->result(0U), z->result(0U), ba); // next placeholder -> wired below
    Operation* const nsum = bin(ctx, o.addi, cell->result(0U), ba->arg(0U), ba);
    cell->set_operand(1U, nsum->result(0U));
    ret1(ctx, ba, nsum->result(0U));

    // @main(%n)
    Block*           bm    = nullptr;
    Operation* const fmain = mkfunc(ctx, *m, "main", 1U, bm);
    (void)fmain;
    Operation* const half = konst(ctx, o, bm, 3);
    Operation* const two  = konst(ctx, o, bm, 2);
    Operation* const zero = konst(ctx, o, bm, 0);
    Operation* const one  = konst(ctx, o, bm, 1);

    Value* lohilst[3] = {zero->result(0U), bm->arg(0U), one->result(0U)};
    Operation* const  forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    bm->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_i32()); // %iv
    forop->region(0)->append(body);

    Operation* const sel = bin(ctx, o.cmpi, body->arg(0U), half->result(0U), body); // %sel = iv < half
    ctx.set_attr(sel, "predicate", ctx.attr_string("slt"));

    Value* selarr[1] = {sel->result(0U)};
    Operation* const mt = ctx.create_operation(o.cmatch, ConstSpan<Value*>(selarr, 1U), 0U, {}, 2U); // match (multi-way)
    body->append(mt);
    Block* const arm0 = ctx.create_block(0U); // sel==0 (iv>=half): acc(iv)
    mt->region(0)->append(arm0);
    (void)callacc(ctx, body->arg(0U), arm0);
    Block* const arm1 = ctx.create_block(0U); // sel==1 (iv<half): acc(2*iv)
    mt->region(1)->append(arm1);
    Operation* const d = bin(ctx, o.muli, body->arg(0U), two->result(0U), arm1);
    (void)callacc(ctx, d->result(0U), arm1);

    // the second, in-loop feedback cell: %a2 = state(0, %n2); %n2 = addi(%a2, %iv)
    Operation* const cell2 = mk_state(ctx, o, zero->result(0U), zero->result(0U), body);
    Operation* const n2    = bin(ctx, o.addi, cell2->result(0U), body->arg(0U), body);
    cell2->set_operand(1U, n2->result(0U));

    // after the loop: %final = call @acc(0) (returns the total, unchanged).
    Operation* const final    = callacc(ctx, zero->result(0U), bm);
    Operation* const eighteen = konst(ctx, o, bm, 18);
    Operation* const okc      = bin(ctx, o.cmpi, final->result(0U), eighteen->result(0U), bm); // %ok = final == 18
    ctx.set_attr(okc, "predicate", ctx.attr_string("eq"));
    // a VALUE-PRODUCING core.if: %bonus = ok ? 100 : 200 (the yield->results mechanism)
    Value* condarr[1] = {okc->result(0U)};
    Operation* const ifop = ctx.create_operation(o.cif, ConstSpan<Value*>(condarr, 1U), 1U, ctx.type_i32(), 2U);
    bm->append(ifop);
    Block* const thenb = ctx.create_block(0U);
    ifop->region(0)->append(thenb);
    yield1(ctx, thenb, konst(ctx, o, thenb, 100)->result(0U));
    Block* const elseb = ctx.create_block(0U);
    ifop->region(1)->append(elseb);
    yield1(ctx, elseb, konst(ctx, o, elseb, 200)->result(0U));
    // return %final + %bonus
    ret1(ctx, bm, bin(ctx, o.addi, final->result(0U), ifop->result(0U), bm)->result(0U));

    return {m, cell2};
}
} // namespace

TEST_CASE("ceir band5 gate: a pinned program executes identically builder-built and text-parsed", "[ceir][gate5]")
{
    crd::memory::MallocAllocator root;

    // ---- BUILDER form ----
    Context   ctx_a(&root);
    const Ops o_a(ctx_a);
    const Built a = build_gate(ctx_a, o_a);
    REQUIRE(ctx_a.find_structure_error(*a.m).kind == StructureErrorKind::None); // the program is structurally sound

    exec::Interpreter in_a(ctx_a);
    exec::install_builtin_semantics(in_a);
    i64                    n6[1] = {6};
    const exec::ExecResult r_a = in_a.invoke(*a.m, "main", ConstSpan<i64>(n6, 1U));
    REQUIRE(r_a.ok());
    REQUIRE(r_a.values.size() == 1U);
    CHECK(r_a.values[0] == 118); // acc total (0+2+4)+(3+4+5)=18; ok -> bonus 100; 18+100
    i64 cell2v = -1;
    REQUIRE(in_a.cell_value(a.cell2, cell2v)); // the in-loop cell, builder form only (pointers don't survive round-trip)
    CHECK(cell2v == 15);                       // sum 0..5

    // ---- TEXT-PARSED form (dialects registered in the parse context -- the CEIR-5d trait-is-registry-state finding) ----
    const String text = print(ctx_a, *a.m, &root);
    Context      ctx_b(&root);
    const Ops    o_b(ctx_b);
    (void)o_b;
    const ParseResult pr = parse(ctx_b, text);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    REQUIRE(ctx_b.find_structure_error(*pr.module).kind == StructureErrorKind::None);

    exec::Interpreter in_b(ctx_b);
    exec::install_builtin_semantics(in_b);
    const exec::ExecResult r_b = in_b.invoke(*pr.module, "main", ConstSpan<i64>(n6, 1U));
    REQUIRE(r_b.ok());
    REQUIRE(r_b.values.size() == 1U);
    CHECK(r_b.values[0] == 118);

    // ---- BYTE-IDENTICAL output (the sec 118 byte-pin) ----
    const containers::Array<u8> pin_a = exec::pin_values(ConstSpan<i64>(r_a.values.data(), r_a.values.size()), &root);
    const containers::Array<u8> pin_b = exec::pin_values(ConstSpan<i64>(r_b.values.data(), r_b.values.size()), &root);
    REQUIRE(pin_a.size() == pin_b.size());
    REQUIRE(pin_a.size() == 8U); // one i64
    CHECK(std::memcmp(pin_a.data(), pin_b.data(), pin_a.size()) == 0);

    // ---- and the whole program survives a BINARY round-trip too ----
    const containers::Array<u8> blob = serialize(ctx_a, *a.m, &root);
    Context                     ctx_c(&root);
    const Ops                   o_c(ctx_c);
    (void)o_c;
    const ParseResult dr = deserialize(ctx_c, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    exec::Interpreter in_c(ctx_c);
    exec::install_builtin_semantics(in_c);
    const exec::ExecResult r_c = in_c.invoke(*dr.module, "main", ConstSpan<i64>(n6, 1U));
    REQUIRE(r_c.ok());
    CHECK(r_c.values[0] == 118);
}
