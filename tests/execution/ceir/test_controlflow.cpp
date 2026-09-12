// CEIR-5a — structured control-flow region ops (sec 13/14). The ceir.core dialect (scope / if / for / foreach + the
// yield terminator) is defined through the CEIR-2 generator with a REGION SIGNATURE (entry-block arg count) it now
// supports; the constant-condition `if` fold (Context::fold_constant_if) is the partial-eval seed. This proves the ops
// build + verify (incl. the region-arg contract), the fold folds to the taken branch, the fold BAILS on every unsound
// case (incl. a CEIR-4c region_exec-tagged if), and a typed-region-arg op round-trips through text. Host-only, ASCII.
//
// CHECKPOINT (CEIR-5a is IN PROGRESS): while / switch / match + value-producing variants (results = yields) are the
// remaining 5a work — see the tracker row.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace crd::ceir;
using crd::containers::ConstSpan;

namespace
{
struct Ops
{
    OpId scope, cif, cfor, cforeach, yield, cst, addi;
    explicit Ops(Context& ctx)
        : scope(ctx.intern_op("core", "scope")), cif(ctx.intern_op("core", "if")), cfor(ctx.intern_op("core", "for")),
          cforeach(ctx.intern_op("core", "foreach")), yield(ctx.intern_op("core", "yield")),
          cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi"))
    {
        (void)core::register_core_ops(ctx);
        (void)arith::register_arith_ops(ctx);
    }
};
Operation* konst(Context& ctx, const Ops& o, Block* b, crd::i64 v)
{
    Operation* const c = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
} // namespace

TEST_CASE("ceir core: the structured ops build and verify; the region-arg contract is checked", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    // a for-op with a correctly-shaped body (^body(%iv) — one entry-block arg) verifies.
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const l = konst(ctx, o, b, 0);
    Operation* const h = konst(ctx, o, b, 10);
    Operation* const s = konst(ctx, o, b, 1);
    Value*           forops[3] = {l->result(0U), h->result(0U), s->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(forops, 3U), 0U, {}, 1U);
    b->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_index()); // ^body(%iv : index) — the induction variable
    forop->region(0)->append(body);
    body->append(ctx.create_operation(o.yield, {}, 0U));
    CHECK(ctx.verify(*forop)); // 3 operands, 0 results, 1 region, body has 1 arg — the region signature holds

    // a WRONG-arity body (0 args where the signature declares 1) fails the generated verifier.
    Operation* const bad = ctx.create_operation(o.cfor, ConstSpan<Value*>(forops, 3U), 0U, {}, 1U);
    b->append(bad);
    Block* const bad_body = ctx.create_block(0U); // no induction arg
    bad->region(0)->append(bad_body);
    bad_body->append(ctx.create_operation(o.yield, {}, 0U));
    CHECK_FALSE(ctx.verify(*bad));

    // a freshly-BUILT skeleton (empty regions) still verifies — the arg check is skipped when no entry block exists yet.
    Operation* const skel = core::build_scope(ctx);
    CHECK(ctx.verify(*skel));
}

TEST_CASE("ceir core: switch/match carry a variadic number of case regions", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   cswitch = ctx.intern_op("core", "switch");
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const sel[1] = {konst(ctx, o, b, 0)->result(0U)};

    // a switch with THREE case regions (each a 0-arg block ending in yield) verifies — the variadic-region signature.
    Operation* const sw = ctx.create_operation(cswitch, ConstSpan<Value*>(sel, 1U), 0U, {}, 3U);
    b->append(sw);
    for (crd::u32 i = 0; i < 3U; ++i)
    {
        Block* const arm = ctx.create_block(0U);
        sw->region(i)->append(arm);
        arm->append(ctx.create_operation(o.yield, {}, 0U));
    }
    CHECK(ctx.verify(*sw));

    // a switch with ZERO regions fails (the variadic region requires at least one case).
    Operation* const empty = ctx.create_operation(cswitch, ConstSpan<Value*>(sel, 1U), 0U, {}, 0U);
    b->append(empty);
    CHECK_FALSE(ctx.verify(*empty));
}

namespace
{
// build `%c = arith.const{value} ; core.if(%c) { ^then: <thenbody> core.yield } { ^else: core.yield }` in block `b`;
// returns the if op. `then_has_yield` / `then_multiblock` perturb the THEN region for the bail tests.
Operation* build_if(Context& ctx, const Ops& o, Block* b, crd::i64 cond_value, Operation** marker_out,
                    bool then_has_yield = true, bool then_multiblock = false)
{
    Value* const     c     = konst(ctx, o, b, cond_value)->result(0U);
    Value*           cin[1] = {c};
    Operation* const ifop  = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 0U, {}, 2U);
    b->append(ifop);
    Block* const thenb = ctx.create_block(0U);
    ifop->region(0)->append(thenb);
    Value*           cc[2]  = {c, c};
    Operation* const marker = ctx.create_operation(o.addi, ConstSpan<Value*>(cc, 2U), 1U, ctx.type_i32()); // well-formed splice witness
    thenb->append(marker);
    if (marker_out != nullptr) { *marker_out = marker; }
    if (then_multiblock) { ifop->region(0)->append(ctx.create_block(0U)); } // a second block -> fold must bail
    if (then_has_yield) { thenb->append(ctx.create_operation(o.yield, {}, 0U)); }
    Block* const elseb = ctx.create_block(0U);
    ifop->region(1)->append(elseb);
    elseb->append(ctx.create_operation(o.yield, {}, 0U));
    return ifop;
}
} // namespace

TEST_CASE("ceir core: fold_constant_if inlines the taken branch and erases the if", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // cond = 1 (nonzero) -> THEN taken: the marker splices into the parent before the if; the if is erased.
    Operation* marker = nullptr;
    Operation* const ifop = build_if(ctx, o, b, /*cond*/ 1, &marker);
    REQUIRE(ctx.fold_constant_if(ifop));
    CHECK(marker->parent_block() == b); // the then-body op is now a sibling in the parent block
    CHECK(b->last_op() == marker);      // ...and the if is gone (the marker is the last op; const precedes it)

    // cond = 0 -> ELSE taken (which is just a yield): nothing splices, the if is erased (the now-dead cond const stays —
    // constant-folding the dead const is a SEPARATE canonicalization, not this one). b2 = [cst2, cond_const]; the if gone.
    Block* const b2 = ctx.create_block(0U);
    m->body()->append(b2);
    (void)konst(ctx, o, b2, 5); // a pre-existing op
    Operation* const if2 = build_if(ctx, o, b2, /*cond*/ 0, nullptr); // appends its own cond const, then the if
    REQUIRE(b2->num_ops() == 3U);                                     // cst2, cond_const, if2
    REQUIRE(ctx.fold_constant_if(if2));
    CHECK(b2->num_ops() == 2U);                       // the if is gone; nothing from the (yield-only) else was spliced
    CHECK(b2->last_op()->kind() == o.cst);            // the last op is the dead cond const, not the erased if
}

TEST_CASE("ceir core: fold_constant_if bails (no mutation) on every unsound case", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // 1. a NON-constant condition (a block arg, no defining arith.const) -> bail.
    Block* const argb = ctx.create_block(1U, ctx.type_i32());
    m->body()->append(argb);
    Value*           cin[1] = {argb->arg(0U)};
    Operation* const nc     = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 0U, {}, 2U);
    argb->append(nc);
    argb->append(ctx.create_operation(o.yield, {}, 0U)); // keep the block terminated (not relevant to the bail)
    CHECK_FALSE(ctx.fold_constant_if(nc));

    // 2. a MULTI-BLOCK taken region -> bail.
    CHECK_FALSE(ctx.fold_constant_if(build_if(ctx, o, b, 1, nullptr, /*yield*/ true, /*multiblock*/ true)));
    // 3. a taken region MISSING its yield terminator -> bail.
    CHECK_FALSE(ctx.fold_constant_if(build_if(ctx, o, b, 1, nullptr, /*yield*/ false)));
    // 4. ⛔ a region_exec-TAGGED if (CEIR-4c) -> bail: inlining would silently delete the if's own domain/realtime
    //    constraint. (The un-tagged fold succeeding is the previous test — this one only asserts the bail.)
    Operation* const tagged = build_if(ctx, o, b, 1, nullptr);
    ctx.set_region_exec(tagged, RegionExec{EvalDomain::DeviceTime, RealtimeClass::Unspecified});
    CHECK_FALSE(ctx.fold_constant_if(tagged));

    // 5. ⛔ an INNER op carrying a region_exec tag inside the taken block -> bail: the fold must not MOVE an
    //    execution-context boundary out of its region (the 2nd band-4-metadata-breaks-a-band-5-rewrite case).
    Value* const     c5      = konst(ctx, o, b, 1)->result(0U);
    Value*           c5in[1] = {c5};
    Operation* const if5     = ctx.create_operation(o.cif, ConstSpan<Value*>(c5in, 1U), 0U, {}, 2U);
    b->append(if5);
    Block* const then5 = ctx.create_block(0U);
    if5->region(0)->append(then5);
    Operation* const inner = ctx.create_operation(o.scope, {}, 0U, {}, 1U); // a region-owner...
    then5->append(inner);
    ctx.set_region_exec(inner, RegionExec{EvalDomain::DeviceTime, RealtimeClass::Unspecified}); // ...that is TAGGED
    then5->append(ctx.create_operation(o.yield, {}, 0U));
    Block* const else5 = ctx.create_block(0U);
    if5->region(1)->append(else5);
    else5->append(ctx.create_operation(o.yield, {}, 0U));
    CHECK_FALSE(ctx.fold_constant_if(if5));

    // a non-core.if op is not folded.
    CHECK_FALSE(ctx.fold_constant_if(konst(ctx, o, b, 7)));
}

TEST_CASE("ceir core: fold_constant_if forwards a value-producing if's results (RAUW); count mismatch bails", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // %r = core.if(1) -> (1 result) { then: %t = addi(%c,%c); yield %t } { else: %e = const 0; yield %e } ; use %r twice.
    Value* const     c      = konst(ctx, o, b, 1)->result(0U);
    Value*           cin[1] = {c};
    Operation* const ifop   = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 1U, ctx.type_i32(), 2U); // ONE result
    b->append(ifop);
    Value* const thenc[2] = {c, c};
    Block* const thenb    = ctx.create_block(0U);
    ifop->region(0)->append(thenb);
    Operation* const t = ctx.create_operation(o.addi, ConstSpan<Value*>(thenc, 2U), 1U, ctx.type_i32());
    thenb->append(t);
    Value* const tyield[1] = {t->result(0U)};
    thenb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(tyield, 1U), 0U)); // yields %t (1 operand = 1 result)
    Block* const elseb = ctx.create_block(0U);
    ifop->region(1)->append(elseb);
    Value* const eyield[1] = {konst(ctx, o, elseb, 0)->result(0U)};
    elseb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(eyield, 1U), 0U));
    Value* const     users[2] = {ifop->result(0U), ifop->result(0U)};
    Operation* const user     = ctx.create_operation(o.addi, ConstSpan<Value*>(users, 2U), 1U, ctx.type_i32());
    b->append(user);

    REQUIRE(ctx.fold_constant_if(ifop));                 // const 1 -> THEN taken; %r RAUW'd to %t
    CHECK(user->operand(0U) == t->result(0U));           // the user now consumes the then's yielded value directly
    CHECK(user->operand(1U) == t->result(0U));

    // ⛔ count mismatch: an if with 2 results whose taken yield has only 1 operand -> bail (no OOB RAUW).
    Value* const     c2      = konst(ctx, o, b, 1)->result(0U);
    Value*           c2in[1] = {c2};
    Operation* const if2     = ctx.create_operation(o.cif, ConstSpan<Value*>(c2in, 1U), 2U, ctx.type_i32(), 2U); // TWO results
    b->append(if2);
    Block* const t2 = ctx.create_block(0U);
    if2->region(0)->append(t2);
    Value* const one[1] = {c2};
    t2->append(ctx.create_operation(o.yield, ConstSpan<Value*>(one, 1U), 0U)); // 1 operand != 2 results
    Block* const e2 = ctx.create_block(0U);
    if2->region(1)->append(e2);
    e2->append(ctx.create_operation(o.yield, ConstSpan<Value*>(one, 1U), 0U));
    CHECK_FALSE(ctx.fold_constant_if(if2));
}

TEST_CASE("ceir core: fold_constant_if(const-0) folds the ELSE contents and RAUWs from the else yield", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // %r = core.if(0) -> (1) { then: yield %c } { else: %e = addi(%c,%c); yield %e } ; use %r. The FALSE branch must
    // splice the ELSE marker and RAUW from the ELSE yield — a region-swap bug (folding to THEN on false) fails here.
    Value* const     c      = konst(ctx, o, b, 0)->result(0U); // cond = 0 -> ELSE taken
    Value*           cin[1] = {c};
    Operation* const ifop   = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 1U, ctx.type_i32(), 2U);
    b->append(ifop);
    Block* const thenb = ctx.create_block(0U); // then just yields %c (not taken)
    ifop->region(0)->append(thenb);
    thenb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(cin, 1U), 0U));
    Block* const elseb = ctx.create_block(0U);
    ifop->region(1)->append(elseb);
    Value* const     ecc[2] = {c, c};
    Operation* const e      = ctx.create_operation(o.addi, ConstSpan<Value*>(ecc, 2U), 1U, ctx.type_i32());
    elseb->append(e); // the ELSE marker — must land in the parent
    Value* const eyield[1] = {e->result(0U)};
    elseb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(eyield, 1U), 0U));
    Value* const     users[1] = {ifop->result(0U)};
    Operation* const user     = ctx.create_operation(o.addi, ConstSpan<Value*>(users, 1U), 0U);
    b->append(user);

    REQUIRE(ctx.fold_constant_if(ifop));
    CHECK(e->parent_block() == b);            // the ELSE marker spliced into the parent (not the then's)
    CHECK(user->operand(0U) == e->result(0U)); // %r RAUW'd from the ELSE's yielded value
}

TEST_CASE("ceir core: while carries a condition region and a body region (structural)", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   cwhile = ctx.intern_op("core", "while");
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // core.while { cond: yield %bool } { body: yield } -- 0 operands, 0 results, 2 regions.
    Operation* const w = ctx.create_operation(cwhile, {}, 0U, {}, 2U);
    b->append(w);
    Block* const cond = ctx.create_block(0U);
    w->region(0)->append(cond);
    Value* const bv[1] = {konst(ctx, o, cond, 1)->result(0U)};
    cond->append(ctx.create_operation(o.yield, ConstSpan<Value*>(bv, 1U), 0U)); // the cond region yields the loop test
    Block* const body = ctx.create_block(0U);
    w->region(1)->append(body);
    body->append(ctx.create_operation(o.yield, {}, 0U));
    CHECK(ctx.verify(*w)); // structural (the cond-yields-exactly-1-bool contract is CEIR-5b)
}

TEST_CASE("ceir core: a for with a typed entry-block arg round-trips through text", "[ceir][core]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const l = konst(ctx, o, b, 0);
    Operation* const h = konst(ctx, o, b, 4);
    Operation* const s = konst(ctx, o, b, 1);
    Value*           forops[3] = {l->result(0U), h->result(0U), s->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(forops, 3U), 0U, {}, 1U);
    b->append(forop);
    Block* const body = ctx.create_block(1U, ctx.type_index());
    forop->region(0)->append(body);
    body->append(ctx.create_operation(o.yield, {}, 0U));

    const crd::containers::String t1 = print(ctx, *m, &root);
    Context                       ctx2(&root);
    const Ops                     o2(ctx2); // re-register so the parsed core ops rebind
    const ParseResult             pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const crd::containers::String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0); // the region-arg-carrying for survives text byte-exact
}
