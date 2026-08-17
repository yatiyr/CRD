// CEIR-5b — the sec 115 STRUCTURE-layer verifier. `Context::find_structure_error` is one pre-order walk returning the
// FIRST structural defect: SSA dominance (single-block def-before-use), capture visibility across nested regions (blocked
// by IsolatedFromAbove), terminator rules by RegionKind (an SsaCfg block must end with a Terminator), and the general
// yield<->owner count contract (the 3f parent_op back-link's payoff). Structure layer ONLY -- types are CEIR-6. ASCII.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::ConstSpan;

namespace
{
struct Ops
{
    OpId cst, addi, cif, cwhile, yield, box; // `box` is a local IsolatedFromAbove region holder
    explicit Ops(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")), cif(ctx.intern_op("core", "if")),
          cwhile(ctx.intern_op("core", "while")), yield(ctx.intern_op("core", "yield")), box(ctx.intern_op("iso", "box"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        ctx.register_dialect("iso")->register_op("box", {.traits = flags_of(OpTrait::IsolatedFromAbove)});
    }
};
Operation* konst(Context& ctx, const Ops& o, Block* b, crd::i64 v)
{
    Operation* const c = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
Operation* use2(Context& ctx, OpId k, Value* a, Value* b2, Block* b)
{
    Value* ops[2] = {a, b2};
    Operation* const o = ctx.create_operation(k, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(o);
    return o;
}
} // namespace

TEST_CASE("ceir structure: a well-formed module has no structure error", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const c = konst(ctx, o, b, 3);
    (void)use2(ctx, o.addi, c->result(0U), c->result(0U), b); // %r = addi(%c, %c) -- def precedes use
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
}

TEST_CASE("ceir structure: a rich nested valid module (captures + value-producing if + block args) is sound", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(1U, ctx.type_i32()); // ^bb0(%arg)
    m->body()->append(b);
    Value* const     v      = konst(ctx, o, b, 5)->result(0U);
    Value*           cin[1] = {v};
    Operation* const ifop   = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 1U, ctx.type_i32(), 2U); // 1 result
    b->append(ifop);
    // both branches CAPTURE %v and the block arg (legal, non-isolated) and yield exactly 1 value (matches the 1 result).
    for (crd::u32 ri = 0; ri < 2U; ++ri)
    {
        Block* const rb = ctx.create_block(0U);
        ifop->region(ri)->append(rb);
        Operation* const t     = use2(ctx, o.addi, v, b->arg(0U), rb); // captures %v AND the enclosing block arg
        Value*           yv[1] = {t->result(0U)};
        rb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(yv, 1U), 0U));
    }
    (void)use2(ctx, o.addi, ifop->result(0U), ifop->result(0U), b); // uses the if's result AFTER it -- def precedes use
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
}

TEST_CASE("ceir structure: use-before-def (forward reference and self-reference)", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    // FORWARD ref: %a = addi(%b, %b) placed BEFORE %b -> %b not yet visible.
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const later = konst(ctx, o, b, 1);       // defines %b, appended first...
    Value*           fwd[2] = {later->result(0U), later->result(0U)};
    Operation* const a      = ctx.create_operation(o.addi, ConstSpan<Value*>(fwd, 2U), 1U, ctx.type_i32());
    b->insert_before(a, later);                          // ...but %a is inserted BEFORE it -> forward ref
    const StructureError e = ctx.find_structure_error(*m);
    // ⚠ RE-CLASSIFIED at CEIR-5d: a SAME-BLOCK forward ref is a FEEDBACK edge, not a plain dominance error. `addi` is not
    // a StateEdge op, so this combinational back-edge is FeedbackWithoutState (strictly more informative than UseBeforeDef).
    CHECK(e.kind == StructureErrorKind::FeedbackWithoutState);
    CHECK(e.op == a);
    CHECK(e.value == later->result(0U));

    // SELF ref: %s = addi(%s, ...) via set_operand -- a same-block self-feedback on a NON-state op ⇒ FeedbackWithoutState.
    Module* const m2 = ctx.create_module();
    Block* const  b2 = ctx.create_block(0U);
    m2->body()->append(b2);
    Operation* const s = use2(ctx, o.addi, konst(ctx, o, b2, 0)->result(0U), konst(ctx, o, b2, 0)->result(0U), b2);
    s->set_operand(0U, s->result(0U));                   // %s uses its OWN result
    CHECK(ctx.find_structure_error(*m2).kind == StructureErrorKind::FeedbackWithoutState);
}

TEST_CASE("ceir structure: capture into a non-isolated region is legal; through isolation is not", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    // LEGAL: capture %v into a core.if (NOT IsolatedFromAbove) region.
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const     v      = konst(ctx, o, b, 7)->result(0U);
    Value*           cin[1] = {v};
    Operation* const ifop   = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 0U, {}, 2U);
    b->append(ifop);
    for (crd::u32 ri = 0; ri < 2U; ++ri)
    {
        Block* const rb = ctx.create_block(0U);
        ifop->region(ri)->append(rb);
        if (ri == 0U) { (void)use2(ctx, o.addi, v, v, rb); } // the THEN captures %v -- legal (no isolation)
        rb->append(ctx.create_operation(o.yield, {}, 0U));
    }
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // ILLEGAL: the SAME capture through an IsolatedFromAbove `iso.box` region.
    Module* const m2 = ctx.create_module();
    Block* const  b2 = ctx.create_block(0U);
    m2->body()->append(b2);
    Value* const     v2  = konst(ctx, o, b2, 7)->result(0U);
    Operation* const box = ctx.create_operation(o.box, {}, 0U, {}, 1U);
    b2->append(box);
    Block* const inner = ctx.create_block(0U);
    box->region(0)->append(inner);
    Operation* const captured = use2(ctx, o.addi, v2, v2, inner); // captures %v2 THROUGH isolation
    const StructureError ce   = ctx.find_structure_error(*m2);
    CHECK(ce.kind == StructureErrorKind::CaptureThroughIsolation);
    CHECK(ce.op == captured);
}

TEST_CASE("ceir structure: capturing a value defined AFTER the region owner is use-before-def", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // core.if BEFORE %w ; the THEN captures %w -- which is defined AFTER the if-owner, so it does NOT dominate the region.
    Operation* const ifop = ctx.create_operation(o.cif, {}, 0U, {}, 2U); // 0 operands (cond irrelevant here)
    b->append(ifop);
    Operation* const w = konst(ctx, o, b, 9); // %w defined AFTER the if
    Block* const then0 = ctx.create_block(0U);
    ifop->region(0)->append(then0);
    Operation* const cap = use2(ctx, o.addi, w->result(0U), w->result(0U), then0);
    then0->append(ctx.create_operation(o.yield, {}, 0U));
    Block* const else0 = ctx.create_block(0U);
    ifop->region(1)->append(else0);
    else0->append(ctx.create_operation(o.yield, {}, 0U));

    const StructureError e = ctx.find_structure_error(*m);
    CHECK(e.kind == StructureErrorKind::UseBeforeDef);
    CHECK(e.op == cap);
}

TEST_CASE("ceir structure: terminator rules by RegionKind", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    // an SsaCfg region whose block does NOT end with a Terminator -> MissingTerminator; the SAME as a Graph region is fine.
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const holder = ctx.create_operation(o.box, {}, 0U, {}, 1U); // any region owner
    b->append(holder);
    Block* const rb = ctx.create_block(0U);
    holder->region(0)->append(rb);
    (void)konst(ctx, o, rb, 0); // a non-terminator op as the block's last op
    ctx.set_region_kind(holder->region(0), RegionKind::SsaCfg);
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::MissingTerminator);
    ctx.set_region_kind(holder->region(0), RegionKind::Graph); // as a Graph region, no terminator is required
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // an EMPTY SsaCfg block also fails -- and the diagnostic must POINT at something: since the block has no op to
    // blame, it points at the region's OWNER via the 3f parent_op back-link (this is where that back-link earns its keep).
    Operation* const empty_holder = ctx.create_operation(o.box, {}, 0U, {}, 1U);
    b->append(empty_holder);
    holder->region(0)->append(ctx.create_block(0U)); // keep `holder` well-formed (a yield terminator)
    holder->region(0)->last_block()->append(ctx.create_operation(o.yield, {}, 0U));
    ctx.set_region_kind(empty_holder->region(0), RegionKind::SsaCfg); // region 0 exists but has NO block at all...
    empty_holder->region(0)->append(ctx.create_block(0U));            // ...now one EMPTY block
    const StructureError te = ctx.find_structure_error(*m);
    CHECK(te.kind == StructureErrorKind::MissingTerminator);
    CHECK(te.op == empty_holder); // pointed via parent_op, not nullptr

    // a Terminator op that is NOT the last op in its block -> TerminatorNotLast.
    Module* const m2 = ctx.create_module();
    Block* const  b2 = ctx.create_block(0U);
    m2->body()->append(b2);
    b2->append(ctx.create_operation(o.yield, {}, 0U)); // core.yield (a Terminator)...
    (void)konst(ctx, o, b2, 0);                        // ...followed by another op
    CHECK(ctx.find_structure_error(*m2).kind == StructureErrorKind::TerminatorNotLast);
}

TEST_CASE("ceir structure: the yield<->owner count contract", "[ceir][structure]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    // a value-producing if declaring 1 result whose taken yield has 0 operands -> YieldCountMismatch.
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Value*           cin[1] = {konst(ctx, o, b, 1)->result(0U)};
    Operation* const ifop   = ctx.create_operation(o.cif, ConstSpan<Value*>(cin, 1U), 1U, ctx.type_i32(), 2U); // 1 RESULT
    b->append(ifop);
    for (crd::u32 ri = 0; ri < 2U; ++ri)
    {
        Block* const rb = ctx.create_block(0U);
        ifop->region(ri)->append(rb);
        rb->append(ctx.create_operation(o.yield, {}, 0U)); // yields 0 operands != 1 result
    }
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::YieldCountMismatch);

    // core.while: the cond region (region 0) must yield EXACTLY 1 (the loop test) -- 2 is a mismatch, 1 is sound.
    const auto build_while = [&](Context& c, const Ops& op, crd::u32 cond_yield_count) -> Module* {
        Module* const wm = c.create_module();
        Block* const  wb = c.create_block(0U);
        wm->body()->append(wb);
        Operation* const w = c.create_operation(op.cwhile, {}, 0U, {}, 2U);
        wb->append(w);
        Block* const cond = c.create_block(0U);
        w->region(0)->append(cond);
        Operation* const k0 = konst(c, op, cond, 1);
        Operation* const k1 = konst(c, op, cond, 1);
        Value*           two[2] = {k0->result(0U), k1->result(0U)};
        cond->append(c.create_operation(op.yield, ConstSpan<Value*>(two, cond_yield_count), 0U));
        Block* const body = c.create_block(0U);
        w->region(1)->append(body);
        body->append(c.create_operation(op.yield, {}, 0U));
        return wm;
    };
    CHECK(ctx.find_structure_error(*build_while(ctx, o, 2U)).kind == StructureErrorKind::YieldCountMismatch);
    CHECK(ctx.find_structure_error(*build_while(ctx, o, 1U)).kind == StructureErrorKind::None);
}
