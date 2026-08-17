// CEIR-1g - the ModuleBuilder gate. A module built through the fluent builder must be INDISTINGUISHABLE from the
// hand-built equivalent (prints byte-identical) and must serialize/round-trip through the binary form. The builder
// emits ORDINARY canonical IR - it routes op creation through Context::create_operation and verification through the
// real Context::verify with NO privileged bypass: a verifier-failing op is caught, and a duplicate sym_name is
// rejected (build() -> nullptr, no silent overwrite). Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/builder.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::u8;

namespace
{
[[nodiscard]] bool text_equal(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// The focused fixture, built two ways. Both must produce byte-identical IR:
//   module { ^bb0(%0 : !t1):
//     %1 = test.add(%0, %0) {k = 7} : !t2
//     scf.region(%1) { ^bb0(%2 : !t1): test.use(%1, %2) }
//     func.func() {sym_name = "helper"} { ^bb0(%3 : !t1): func.return(%3) }
//     %4 = func.call(%0) {callee = @helper} : !t1 }
Module* build_via_builder(Context& ctx)
{
    ModuleBuilder mb(ctx);
    Block* const  top = mb.add_block(1U, ctx.type_i32());
    Value* const  a0  = top->arg(0U);

    Value* const r =
        mb.op("test", "add").operand(a0).operand(a0).result(ctx.type_f32()).attr("k", ctx.attr_int(7)).build_result();

    Operation* const rop = mb.op("scf", "region").operand(r).regions(1U).build();
    {
        InsertionGuard g(mb);
        Block* const   inner      = mb.add_block(1U, ctx.type_i32(), rop->region(0));
        Value*         use_ops[2] = {r, inner->arg(0U)};
        mb.op("test", "use").operands(ConstSpan<Value*>(use_ops, 2U)).build();
    }

    Operation* const fn = mb.func("helper", Visibility::Public, 1U, ctx.type_i32());
    {
        InsertionGuard g(mb);
        Block* const   fb         = func::func_body_block(fn);
        mb.set_insertion(fb);
        Value* ret_ops[1] = {fb->arg(0U)};
        mb.ret(ConstSpan<Value*>(ret_ops, 1U));
    }

    Value* call_args[1] = {a0};
    mb.call("helper", ConstSpan<Value*>(call_args, 1U), 1U, ctx.type_i32());
    return mb.module();
}

Module* build_via_hand(Context& ctx)
{
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(1U, ctx.type_i32());
    m->body()->append(top);
    Value* const a0 = top->arg(0U);

    Value*           add_ops[2] = {a0, a0};
    Operation* const add =
        ctx.create_operation(ctx.intern_op("test", "add"), ConstSpan<Value*>(add_ops, 2U), 1U, ctx.type_f32());
    ctx.set_attr(add, "k", ctx.attr_int(7));
    top->append(add);
    Value* const r = add->result(0U);

    Value*           reg_ops[1] = {r};
    Operation* const rop =
        ctx.create_operation(ctx.intern_op("scf", "region"), ConstSpan<Value*>(reg_ops, 1U), 0U, {}, 1U);
    top->append(rop);
    Block* const inner = ctx.create_block(1U, ctx.type_i32());
    rop->region(0)->append(inner);
    Value* use_ops[2] = {r, inner->arg(0U)};
    inner->append(ctx.create_operation(ctx.intern_op("test", "use"), ConstSpan<Value*>(use_ops, 2U), 0U));

    Operation* const fn = func::create_func(ctx, *m, "helper", Visibility::Public, 1U, ctx.type_i32());
    top->append(fn);
    Block* const fb         = func::func_body_block(fn);
    Value*       ret_ops[1] = {fb->arg(0U)};
    fb->append(func::create_return(ctx, ConstSpan<Value*>(ret_ops, 1U)));

    Value*           call_args[1] = {a0};
    Operation* const call = func::create_call(ctx, "helper", ConstSpan<Value*>(call_args, 1U), 1U, ctx.type_i32());
    top->append(call);
    return m;
}

// A verifier that fails a BUILDABLE condition (>= 1 operand) — so we can construct a well-formed-but-invalid op via
// the builder and prove verify() actually dispatches it (func.return can't fail: it passes inside any block).
[[nodiscard]] bool verify_needs_operand(const Context& /*ctx*/, const Operation& op) noexcept
{
    return op.num_operands() >= 1U;
}
} // namespace

TEST_CASE("ceir builder: a builder-made module is byte-identical to the hand-built one", "[ceir][builder]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      cb(&root);
    Context                      ch(&root);
    const String                 pb = print(cb, *build_via_builder(cb), &root);
    const String                 ph = print(ch, *build_via_hand(ch), &root);
    CHECK(text_equal(pb, ph));
}

TEST_CASE("ceir builder: a builder-made module round-trips through the binary form", "[ceir][builder]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m  = build_via_builder(ctx);
    const String                 p1 = print(ctx, *m, &root);

    const crd::containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                          ctx2(&root);
    const ParseResult                pr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(pr.ok);
    CHECK(text_equal(p1, print(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir builder: verify() dispatches the real per-kind verifier (no bypass)", "[ceir][builder]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("vtest");
    d->register_op("needsop", {.verify = &verify_needs_operand});

    ModuleBuilder mb(ctx);
    Block* const  top = mb.add_block(1U, ctx.type_i32());
    Value* const  a0  = top->arg(0U);

    (void)mb.op("vtest", "needsop").operand(a0).build(); // 1 operand -> passes
    CHECK(mb.verify());

    Operation* const bad = mb.op("vtest", "needsop").build(); // 0 operands -> fails its verifier
    const Operation* failing = nullptr;
    CHECK_FALSE(mb.verify(&failing));
    CHECK(failing == bad); // and it points at exactly the offending op
}

TEST_CASE("ceir builder: a duplicate sym_name is rejected, build() returns nullptr", "[ceir][builder][symbol]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    ModuleBuilder                mb(ctx);
    (void)mb.add_block(0U);

    Operation* const f1 = mb.func("dup", Visibility::Public, 0U);
    REQUIRE(f1 != nullptr);

    // a second symbol-defining op with the SAME sym_name, via the generic path -> no silent overwrite
    Operation* const f2 = mb.op("func", "func").regions(1U).attr("sym_name", ctx.attr_string("dup")).build();
    CHECK(f2 == nullptr);
    CHECK(mb.module()->symbols()->size() == 1U); // the original definition is untouched
}
