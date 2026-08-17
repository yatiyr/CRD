// CEIR-3z — the BAND-3 GATE. Band 3 built the type/shape/unit/lifetime foundation as a family of interned types plus
// tri-split predicates; this gate proves the four §16/§17/§19/§21 error checks fire as POINTING diagnostics — each names
// the exact offender, not a bare bool — and that the pointing field DISCRIMINATES (a second case moves it, a control
// case does not fire). These are the predicates CEIR-4's typed-operand op verifiers compose; here they are proven at the
// type level (op-level wiring has no producer op until CEIR-4, the 3e "predicate-now" precedent). Host-only, ASCII names.
//
//   (1) Length+Time            — quantity_dimensions_equal -> DimMismatch.first_differing_base   (§17)
//   (2) rank-mismatched cast   — shapes_broadcast          -> BroadcastResult.position           (§21)
//   (3) borrowed-view escape   — find_borrowed_escape      -> BorrowEscape{value, escaping_use}   (§19)
//   (4) generic-constraint     — substitute               -> SubstResult{failed_param,trait}     (§16)

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::ConstSpan;

TEST_CASE("ceir gate3: a dimensional mismatch points at the first clashing base (Length+Time)", "[ceir][gate3]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();

    QuantityDim len; // L
    len.exp[0] = static_cast<crd::i8>(1);
    QuantityDim tim; // T
    tim.exp[2] = static_cast<crd::i8>(1);
    QuantityDim len_tim; // L*T (agrees with `len` at base 0, differs at base 2)
    len_tim.exp[0] = static_cast<crd::i8>(1);
    len_tim.exp[2] = static_cast<crd::i8>(1);

    const TypeId qlen     = ctx.type_quantity(f32, len);
    const TypeId qtim     = ctx.type_quantity(f32, tim);
    const TypeId qlen_tim = ctx.type_quantity(f32, len_tim);

    // Length vs Time -> not equal, first clash at base 0 (Length).
    const DimMismatch lt = ctx.quantity_dimensions_equal(qlen, qtim);
    CHECK_FALSE(lt.equal);
    CHECK(lt.first_differing_base == 0U);

    // Length vs Length*Time -> agree at base 0, first clash at base 2 (Time). Discriminates a real field from a hard 0.
    const DimMismatch t2 = ctx.quantity_dimensions_equal(qlen, qlen_tim);
    CHECK_FALSE(t2.equal);
    CHECK(t2.first_differing_base == 2U);

    // control: identical dimensions do NOT fire.
    CHECK(ctx.quantity_dimensions_equal(qlen, ctx.type_quantity(f32, len)).equal);
}

TEST_CASE("ceir gate3: an un-broadcastable shape pair points at the right-aligned bad dim", "[ceir][gate3]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const auto                   sh1 = [&](TypeId a) {
        const TypeId d[1] = {a};
        return ctx.type_shape(ConstSpan<TypeId>(d, 1U));
    };
    const auto sh2 = [&](TypeId a, TypeId b) {
        const TypeId d[2] = {a, b};
        return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
    };
    const TypeId d3 = ctx.type_dim_static(3U);
    const TypeId d4 = ctx.type_dim_static(4U);
    const TypeId d5 = ctx.type_dim_static(5U);

    // [4,3] vs [5,3]: innermost 3s match (position 0), 4 vs 5 clash at right-aligned position 1 -> the diagnostic names it.
    const BroadcastResult bad = ctx.shapes_broadcast(sh2(d4, d3), sh2(d5, d3));
    CHECK(bad.compat == ShapeCompat::Incompatible);
    CHECK(bad.position == 1U);

    // control: a broadcastable pair (rank padding) does NOT fire.
    CHECK(ctx.shapes_broadcast(sh2(d4, d3), sh1(d3)).compat == ShapeCompat::Compatible);
}

namespace
{
// A module carrier for the escape check. `scf.region` holds a nested region; `test.def` produces a typed value; a
// `test.use` op consumes one. Opaque ops (no registered verifier) — the escape check is a pure IR analysis over uses.
struct EscapeOps
{
    OpId region, def, use;
    explicit EscapeOps(Context& ctx)
        : region(ctx.intern_op("scf", "region")), def(ctx.intern_op("test", "def")), use(ctx.intern_op("test", "use"))
    {
    }
};
} // namespace

TEST_CASE("ceir gate3: a borrowed value that escapes its region is flagged with the exact escaping use", "[ceir][gate3]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const EscapeOps              ops(ctx);
    const TypeId                 f32     = ctx.type_f32();
    const TypeId                 qborrow = ctx.type_qualified(OwnershipKind::BorrowedView, f32);
    const TypeId                 qown    = ctx.type_qualified(OwnershipKind::OwnedResource, f32);

    // A module: R0(body) > op_region.R1 . In R1: a borrowed block arg used only INSIDE (must NOT flag), a borrowed op
    // RESULT used in R0 (escapes -> flagged), and an OWNED op result used in R0 (escapes but not borrowed -> not flagged).
    SECTION("op-result borrow escapes, chosen over a non-escaping borrow and an escaping non-borrow")
    {
        Module* const m  = ctx.create_module();
        Region* const r0 = m->body();
        Block* const  b0 = ctx.create_block(0U);
        r0->append(b0);

        Operation* const op_region = ctx.create_operation(ops.region, {}, 0U, {}, 1U);
        b0->append(op_region);
        Region* const r1 = op_region->region(0);
        Block* const  b1 = ctx.create_block(1U, qborrow); // borrowed BLOCK ARG a_in
        r1->append(b1);
        Value* const a_in = b1->arg(0U);

        Operation* const op_borrow = ctx.create_operation(ops.def, {}, 1U, qborrow); // borrowed OP RESULT r_esc
        b1->append(op_borrow);
        Value* const r_esc = op_borrow->result(0U);
        Operation* const op_own = ctx.create_operation(ops.def, {}, 1U, qown); // OWNED result own_esc (non-borrow)
        b1->append(op_own);
        Value* const     own_esc   = op_own->result(0U);
        Value* const     inuse[1]  = {a_in};
        Operation* const inner_use = ctx.create_operation(ops.use, ConstSpan<Value*>(inuse, 1U), 0U);
        b1->append(inner_use); // a_in used INSIDE R1 -> a_in does not escape

        Value* const     ruse[1]  = {r_esc};
        Operation* const use_esc  = ctx.create_operation(ops.use, ConstSpan<Value*>(ruse, 1U), 0U);
        b0->append(use_esc); // r_esc used in R0 -> escapes; THIS op is the pointing target
        Value* const     ouse[1]  = {own_esc};
        Operation* const use_own  = ctx.create_operation(ops.use, ConstSpan<Value*>(ouse, 1U), 0U);
        b0->append(use_own); // own_esc escapes too, but it is OwnedResource -> must be ignored

        const BorrowEscape e = ctx.find_borrowed_escape(*m);
        REQUIRE(e.value == r_esc);          // the borrowed escaper, NOT the non-escaping borrowed arg a_in
        CHECK(e.escaping_use == use_esc);   // points at the exact outside use, not the owned-value use
    }

    // A borrowed BLOCK ARG (the other value kind) escaping is flagged the same way.
    SECTION("block-arg borrow escapes")
    {
        Module* const m  = ctx.create_module();
        Region* const r0 = m->body();
        Block* const  b0 = ctx.create_block(0U);
        r0->append(b0);
        Operation* const op_region = ctx.create_operation(ops.region, {}, 0U, {}, 1U);
        b0->append(op_region);
        Region* const r1 = op_region->region(0);
        Block* const  b1 = ctx.create_block(1U, qborrow);
        r1->append(b1);
        Value* const     a_esc    = b1->arg(0U);
        Value* const     ause[1]  = {a_esc};
        Operation* const use_esc  = ctx.create_operation(ops.use, ConstSpan<Value*>(ause, 1U), 0U);
        b0->append(use_esc); // the inner block arg used in the OUTER region -> escapes

        const BorrowEscape e = ctx.find_borrowed_escape(*m);
        REQUIRE(e.value == a_esc);
        CHECK(e.escaping_use == use_esc);
    }

    // The type-directed contract: an OWNED value escaping and a BORROWED value used only inside both yield NO diagnostic.
    SECTION("no false fire: owned escapes and borrow stays inside")
    {
        Module* const m  = ctx.create_module();
        Region* const r0 = m->body();
        Block* const  b0 = ctx.create_block(0U);
        r0->append(b0);
        Operation* const op_region = ctx.create_operation(ops.region, {}, 0U, {}, 1U);
        b0->append(op_region);
        Region* const r1 = op_region->region(0);
        Block* const  b1 = ctx.create_block(1U, qborrow); // a borrowed arg...
        r1->append(b1);
        Value* const     a_in     = b1->arg(0U);
        Operation* const op_own   = ctx.create_operation(ops.def, {}, 1U, qown);
        b1->append(op_own);
        Value* const     own_esc  = op_own->result(0U);
        Value* const     inuse[1] = {a_in};
        Operation* const inner    = ctx.create_operation(ops.use, ConstSpan<Value*>(inuse, 1U), 0U);
        b1->append(inner); // ...used only inside -> does not escape
        Value* const     ouse[1]  = {own_esc};
        Operation* const outer    = ctx.create_operation(ops.use, ConstSpan<Value*>(ouse, 1U), 0U);
        b0->append(outer); // an OwnedResource escapes -> not a borrow -> not flagged

        const BorrowEscape e = ctx.find_borrowed_escape(*m);
        CHECK(e.value == nullptr);
        CHECK(e.escaping_use == nullptr);
    }

    // With MORE THAN ONE escaping use, the check latches AN escaping use (first-offender in use-list order) — it does not
    // collect all, and it never returns a non-escaping use. (Use-list is prepend-at-head, so order is impl-defined here;
    // the contract consumers rely on is "non-null iff escape, points at an escaping use".)
    SECTION("two escaping uses: latches one of them")
    {
        Module* const m  = ctx.create_module();
        Region* const r0 = m->body();
        Block* const  b0 = ctx.create_block(0U);
        r0->append(b0);
        Operation* const op_region = ctx.create_operation(ops.region, {}, 0U, {}, 1U);
        b0->append(op_region);
        Region* const r1 = op_region->region(0);
        Block* const  b1 = ctx.create_block(1U, qborrow);
        r1->append(b1);
        Value* const     a_esc   = b1->arg(0U);
        Value* const     use1[1] = {a_esc};
        Operation* const outer1  = ctx.create_operation(ops.use, ConstSpan<Value*>(use1, 1U), 0U);
        b0->append(outer1);
        Value* const     use2[1] = {a_esc};
        Operation* const outer2  = ctx.create_operation(ops.use, ConstSpan<Value*>(use2, 1U), 0U);
        b0->append(outer2); // a second escaping use

        const BorrowEscape e = ctx.find_borrowed_escape(*m);
        REQUIRE(e.value == a_esc);
        CHECK((e.escaping_use == outer1 || e.escaping_use == outer2)); // an escaping use, never a phantom
    }
}

TEST_CASE("ceir gate3: a generic-constraint violation points at the failing (param, trait)", "[ceir][gate3]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeId                 ord    = ctx.type_trait("Ord", {});
    const TypeId                 con[1] = {ord};
    const TypeId                 t      = ctx.type_param("T", ConstSpan<TypeId>(con, 1U)); // T : Ord
    const TypeId                 i32    = ctx.type_i32();

    // i32 does not (yet) conform to Ord -> binding T=i32 is rejected, naming the exact (param, trait).
    const TypeBinding bad[1] = {{t, i32}};
    const SubstResult v      = ctx.substitute(t, ConstSpan<TypeBinding>(bad, 1U));
    CHECK_FALSE(v.ok);
    CHECK(v.failed_param == t);
    CHECK(v.failed_trait == ord);

    // control: once i32 conforms, the SAME binding is accepted -> no false rejection.
    ctx.register_conformance(i32, ord);
    CHECK(ctx.substitute(t, ConstSpan<TypeBinding>(bad, 1U)).ok);
}
