// CEIR-2a+2b - the GENERATED-dialect gate: the `arith` dialect is DEFINED in engine/ceir/ops/arith.ceirop.toml and
// GENERATED (crd/ceir/gen/arith_ops.hpp/.cpp) by ceir_opgen.py. This exercises the generated op-kind interners, typed
// wrappers, builders, and the generated verifiers dispatched through the CEIR-1d Context::verify - proving a real op
// dialect exists with ZERO central-enum/switch edits (section 7). Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gen/arith_ops.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::ConstSpan;

TEST_CASE("ceir arith gen: the dialect self-registers with traits + verifier, no central edit", "[ceir][gen][arith]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = arith::register_arith_ops(ctx);
    REQUIRE(d != nullptr);
    CHECK(d->name() == crd::containers::StringView("arith"));
    // every op is a first-class registered kind carrying its schema trait (Pure), reachable through the open-world registry
    CHECK(ctx.has_trait(arith::const_kind(ctx), OpTrait::Pure));
    CHECK(ctx.has_trait(arith::addi_kind(ctx), OpTrait::Pure));
    CHECK(ctx.has_trait(arith::cmpi_kind(ctx), OpTrait::Pure));
    CHECK(ctx.dialect_of(arith::muli_kind(ctx)) == d);
}

TEST_CASE("ceir arith gen: builders + typed wrappers + verifier round-trip on well-formed ops", "[ceir][gen][arith]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);

    Operation* const c1  = arith::build_const(ctx, ctx.attr_int(40), ctx.type_i32());
    Operation* const c2  = arith::build_const(ctx, ctx.attr_int(2), ctx.type_i32());
    Operation* const sum = arith::build_addi(ctx, arith::ConstOp(c1).result(), arith::ConstOp(c2).result(), ctx.type_i32());

    // typed wrappers read the structure (no stringly-typed op->attr / operand(i) at the call site)
    CHECK(arith::ConstOp(c1).value() == ctx.attr_int(40));
    CHECK(arith::AddiOp(sum).lhs() == arith::ConstOp(c1).result());
    CHECK(arith::AddiOp(sum).rhs() == arith::ConstOp(c2).result());
    CHECK(arith::AddiOp(sum).result() != nullptr);

    // the GENERATED verifiers pass on well-formed ops, dispatched through the real Context::verify (no bypass)
    CHECK(ctx.verify(*c1));
    CHECK(ctx.verify(*sum));

    Operation* const cmp = arith::build_cmpi(ctx, arith::ConstOp(c1).result(), arith::ConstOp(c2).result(),
                                             ctx.attr_string("slt"), ctx.type_i32());
    CHECK(arith::CmpiOp(cmp).predicate() == ctx.attr_string("slt"));
    CHECK(ctx.verify(*cmp));
}

TEST_CASE("ceir arith gen: the generated verifier rejects malformed ops", "[ceir][gen][arith]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);

    // arith.const with NO value attr -> the generated required-attr check fails it
    Operation* const bad_const = ctx.create_operation(arith::const_kind(ctx), {}, 1U, ctx.type_i32());
    CHECK_FALSE(ctx.verify(*bad_const));

    // arith.addi with the wrong operand count (0, not 2) -> the generated arity check fails it
    Operation* const bad_addi = ctx.create_operation(arith::addi_kind(ctx), {}, 1U, ctx.type_i32());
    CHECK_FALSE(ctx.verify(*bad_addi));

    // arith.cmpi missing its required `predicate` string attr -> rejected
    Value* ops2[2] = {arith::build_const(ctx, ctx.attr_int(1), ctx.type_i32())->result(0U),
                      arith::build_const(ctx, ctx.attr_int(2), ctx.type_i32())->result(0U)};
    Operation* const bad_cmp = ctx.create_operation(arith::cmpi_kind(ctx), ConstSpan<Value*>(ops2, 2U), 1U, ctx.type_i32());
    CHECK_FALSE(ctx.verify(*bad_cmp));
}

TEST_CASE("ceir arith gen: the reflection record is coherent with the generated ops (no parallel truth)",
          "[ceir][gen][arith]")
{
    const ConstSpan<OpSchema> schemas = arith::arith_op_schemas();
    REQUIRE(schemas.size() == 4U); // const/addi/muli/cmpi

    // reflection is emitted sorted by name (a cheap structural invariant that catches an emit-order regression)
    crd::containers::StringView prev;
    for (const OpSchema& s : schemas)
    {
        if (!prev.empty()) { CHECK(prev < s.name); }
        prev = s.name;
    }

    // find arith.const by its qualified name; its reflected schema matches the codegen exactly
    const OpSchema* c = nullptr;
    for (const OpSchema& s : schemas)
    {
        if (s.qualified == crd::containers::StringView("arith.const")) { c = &s; }
    }
    REQUIRE(c != nullptr);
    CHECK(c->version == 1U);
    CHECK(c->operands.size() == 0U);
    CHECK(c->results.size() == 1U);
    REQUIRE(c->attributes.size() == 1U);
    CHECK(c->attributes[0].name == crd::containers::StringView("value"));
    CHECK(c->attributes[0].kind == AttrKind::Int);
    CHECK(c->attributes[0].required);
    CHECK((c->traits & flags_of(OpTrait::Pure)) != 0U);
    CHECK(c->num_regions == 0U);
    CHECK_FALSE(c->intrinsic);

    // reflection <-> codegen coherence: an op built via the generated builder has EXACTLY the reflected shape
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    Operation* const built = arith::build_const(ctx, ctx.attr_int(7), ctx.type_i32());
    CHECK(built->num_operands() == c->operands.size());
    CHECK(built->num_results() == c->results.size());
    CHECK(built->num_attrs() == c->attributes.size());
}
