// CEIR-5d (sec 20 STATE IS EXPLICIT): the explicit-state ops (core.state/delay/history) + the "graph cycles must pass
// through explicit state/delay semantics" rule, folded into the CEIR-5b structure verifier. A same-block back-edge (an
// operand defined LATER in the same block) is legal ONLY as a StateEdge op's LAST operand (its `next`/feedback); any other
// back-edge is FeedbackWithoutState (a combinational feedback cycle). The exemption keys on the OpTrait::StateEdge trait
// (open-world), not an op name. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;
using ByteArray = crd::containers::Array<crd::u8>;

namespace
{
struct Ops
{
    OpId cst, addi, state, delay, history;
    explicit Ops(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          state(ctx.intern_op("core", "state")), delay(ctx.intern_op("core", "delay")),
          history(ctx.intern_op("core", "history"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
    }
};
Operation* konst(Context& ctx, const Ops& o, Block* b, crd::i64 v)
{
    Operation* const c = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
// A 2-operand, 1-result op (init/next -> current for a state op; lhs/rhs -> result for addi), appended to `b`.
Operation* mk2(Context& ctx, OpId k, Value* a, Value* b2, Block* b)
{
    Value* ops[2] = {a, b2};
    Operation* const o = ctx.create_operation(k, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    b->append(o);
    return o;
}
// Build, in a fresh module, a legal feedback loop through a state op of kind `sk`:
//   %init = const 0 ; %x = const 1 ; %cur = <sk>(%init, %next) ; %next = addi(%cur, %x)   [%next wired as the feedback]
Module* legal_cycle(Context& ctx, const Ops& o, OpId sk)
{
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const init = konst(ctx, o, b, 0);
    Operation* const x    = konst(ctx, o, b, 1);
    Operation* const st   = mk2(ctx, sk, init->result(0U), init->result(0U), b); // next = placeholder for now
    Operation* const add  = mk2(ctx, o.addi, st->result(0U), x->result(0U), b);  // %next = %cur + %x
    st->set_operand(1U, add->result(0U));                                        // wire the FEEDBACK (last operand)
    return m;
}
} // namespace

TEST_CASE("ceir state: a feedback cycle through a StateEdge op is legal (state, delay, history)", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    for (const OpId sk : {o.state, o.delay, o.history})
    {
        CHECK(ctx.find_structure_error(*legal_cycle(ctx, o, sk)).kind == StructureErrorKind::None);
    }
}

TEST_CASE("ceir state: self-feedback (next = own result) is legal", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const init = konst(ctx, o, b, 0);
    Operation* const st   = mk2(ctx, o.state, init->result(0U), init->result(0U), b);
    st->set_operand(1U, st->result(0U)); // %cur = state(%init, %cur) -- the feedback is its own result
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
}

TEST_CASE("ceir state: a normally-visible feedback operand (a pass-through register) is legal", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const init = konst(ctx, o, b, 0);
    Operation* const x    = konst(ctx, o, b, 5);
    mk2(ctx, o.state, init->result(0U), x->result(0U), b); // next = %x, defined BEFORE -> no back-edge at all
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
}

TEST_CASE("ceir state: a combinational cycle with no state op is FeedbackWithoutState", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const z  = konst(ctx, o, b, 1);
    Operation* const a  = mk2(ctx, o.addi, z->result(0U), z->result(0U), b); // placeholder operands
    Operation* const bb = mk2(ctx, o.addi, a->result(0U), z->result(0U), b);
    a->set_operand(0U, bb->result(0U)); // %a uses %b (defined LATER, same block) -- addi is NOT a StateEdge op
    const StructureError e = ctx.find_structure_error(*m);
    CHECK(e.kind == StructureErrorKind::FeedbackWithoutState);
    CHECK(e.op == a);
    CHECK(e.value == bb->result(0U));
}

TEST_CASE("ceir state: a forward ref on the INIT (non-last) operand is FeedbackWithoutState", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const seed  = konst(ctx, o, b, 0);
    Operation* const st    = mk2(ctx, o.state, seed->result(0U), seed->result(0U), b); // valid next, placeholder init
    Operation* const later = konst(ctx, o, b, 7);                                      // defined AFTER st
    st->set_operand(0U, later->result(0U)); // INIT (operand 0, NOT the last) forward-refs -> not the feedback edge
    const StructureError e = ctx.find_structure_error(*m);
    CHECK(e.kind == StructureErrorKind::FeedbackWithoutState); // pins the LAST-operand convention
    CHECK(e.op == st);
}

TEST_CASE("ceir state: a feedback operand defined in a DIFFERENT block is UseBeforeDef", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Block* const                 b1 = ctx.create_block(0U);
    Block* const                 b2 = ctx.create_block(0U);
    m->body()->append(b1);
    m->body()->append(b2);
    Operation* const init  = konst(ctx, o, b1, 0);
    Operation* const st    = mk2(ctx, o.state, init->result(0U), init->result(0U), b1);
    Operation* const other = konst(ctx, o, b2, 9); // in the OTHER block
    st->set_operand(1U, other->result(0U));        // cross-block feedback -> not a same-block back-edge
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::UseBeforeDef);
}

TEST_CASE("ceir state: the history depth attr must be an Int >= 1", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = legal_cycle(ctx, o, o.history);
    // the history op is the 3rd op in the single block (const, const, history, addi).
    Block* const     b = m->body()->first_block();
    Operation* const h = b->first_op()->next_in_block()->next_in_block();
    REQUIRE(ctx.op_name(h->kind()) == crd::containers::StringView("core.history"));

    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None); // depth ABSENT -> defaults to 1
    ctx.set_attr(h, "depth", ctx.attr_int(3));
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None); // depth = 3
    ctx.set_attr(h, "depth", ctx.attr_int(0));
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::StateDepthInvalid); // depth 0 < 1
    ctx.set_attr(h, "depth", ctx.attr_int(-2));
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::StateDepthInvalid); // negative
    ctx.set_attr(h, "depth", ctx.attr_string("nope"));
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::StateDepthInvalid); // wrong kind
}

TEST_CASE("ceir state: a legal feedback cycle round-trips through text and binary", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = legal_cycle(ctx, o, o.state);
    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // ⛔ A feedback cycle FORWARD-REFERENCES the value it feeds back. This confirms both the 1e parser and the 1f binary
    // decoder resolve a forward value reference (they were built against the fuzz corpus, which never forward-refs). The
    // 5z gate needs the pinned program (a state<T> accumulator) in TEXT-PARSED form — this proves that path is open.
    // NOTE: the StateEdge trait is REGISTRY state (registration), NOT serialized module content (the open-world design) —
    // so a consumer that VERIFIES a round-tripped module must register its dialects (as any real consumer does); an
    // unregistered state op has no trait and its feedback would (conservatively) read as FeedbackWithoutState.
    const String      t1 = print(ctx, *m, &root);
    Context           ctx2(&root);
    (void)arith::register_arith_ops(ctx2);
    (void)core::register_core_ops(ctx2);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok); // the parser accepts the forward value reference
    REQUIRE(pr.module != nullptr);
    CHECK(ctx2.find_structure_error(*pr.module).kind == StructureErrorKind::None);

    const ByteArray   blob = serialize(ctx, *m, &root);
    Context           ctx3(&root);
    (void)arith::register_arith_ops(ctx3);
    (void)core::register_core_ops(ctx3);
    const ParseResult dr = deserialize(ctx3, ConstSpan<crd::u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok); // the decoder resolves the forward operand index
    CHECK(ctx3.find_structure_error(*dr.module).kind == StructureErrorKind::None);
}

TEST_CASE("ceir state: the feedback exemption keys on the trait, not the op name (open-world)", "[ceir][state]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // a HAND-registered non-core op carrying StateEdge -- the verifier must exempt its last operand just like core.state.
    const OpId reg = ctx.register_dialect("hw")->register_op("reg", {.traits = flags_of(OpTrait::StateEdge)});

    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const init = konst(ctx, o, b, 0);
    Operation* const x    = konst(ctx, o, b, 1);
    Operation* const rg   = mk2(ctx, reg, init->result(0U), init->result(0U), b);
    Operation* const add  = mk2(ctx, o.addi, rg->result(0U), x->result(0U), b);
    rg->set_operand(1U, add->result(0U)); // feedback through the hand op
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // contrast: the SAME cyclic shape through a non-StateEdge hand op is a combinational cycle.
    const OpId  plain = ctx.register_dialect("hw")->register_op("plain", {});
    Module* const m2  = ctx.create_module();
    Block* const  b2  = ctx.create_block(0U);
    m2->body()->append(b2);
    Operation* const i2 = konst(ctx, o, b2, 0);
    Operation* const p  = mk2(ctx, plain, i2->result(0U), i2->result(0U), b2);
    Operation* const a2 = mk2(ctx, o.addi, p->result(0U), i2->result(0U), b2);
    p->set_operand(1U, a2->result(0U)); // last operand, but `plain` is not StateEdge
    CHECK(ctx.find_structure_error(*m2).kind == StructureErrorKind::FeedbackWithoutState);
}
