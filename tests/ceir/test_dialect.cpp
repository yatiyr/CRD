// CEIR-1d - the dialect registry + traits/interfaces gate. Register a dialect + ops with traits; dispatch via a TRAIT
// (not a switch); an UNKNOWN-dialect op survives opaquely; a verifier hook runs; and an analysis dispatches through an
// op INTERFACE (the open-world alternative to switch(op.kind)). Host-only, device-free. ASCII-only test names.

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
namespace fn = crd::ceir::func;

namespace
{
// An op-interface: "what symbol does this call-like op call?" An analysis dispatches through THIS, never a switch on
// op.kind — the whole point of §7. func.call implements it below.
struct CallInterface
{
    crd::containers::StringView (*callee)(const Context&, const Operation&);
};
crd::containers::StringView func_call_callee(const Context& ctx, const Operation& op)
{
    const AttrId id = op.attr("callee");
    return id.valid() ? ctx.attr_value(id).s : crd::containers::StringView{};
}
const CallInterface kFuncCallInterface{&func_call_callee};
} // namespace

TEST_CASE("ceir dialect: register a dialect + ops; traits and dialect_of resolve", "[ceir][dialect]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);

    Dialect* d = fn::register_dialect(ctx);
    REQUIRE(d != nullptr);
    CHECK(d->name() == crd::containers::StringView{"func"});
    CHECK(ctx.dialect("func") == d);
    CHECK(fn::register_dialect(ctx) == d); // idempotent

    const OpId func_k = fn::func_kind(ctx);
    const OpId ret_k  = fn::return_kind(ctx);
    const OpId call_k = fn::call_kind(ctx);

    CHECK(ctx.dialect_of(func_k) == d);
    CHECK(ctx.has_trait(func_k, OpTrait::Symbol));
    CHECK(ctx.has_trait(ret_k, OpTrait::Terminator));
    CHECK(ctx.has_trait(call_k, OpTrait::Terminator) == false);
    REQUIRE(ctx.op_info(func_k) != nullptr);
    CHECK(ctx.op_info(func_k)->name == crd::containers::StringView{"func.func"});
}

TEST_CASE("ceir dialect: an unregistered-dialect op is still a valid Operation (opaque preservation)", "[ceir][dialect]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);

    // No register_dialect("plugin") — the dialect is UNKNOWN, yet its op is a first-class Operation.
    const OpId widget = ctx.intern_op("plugin", "widget");
    Operation* op     = ctx.create_operation(widget, {}, 1U);
    REQUIRE(op != nullptr);
    CHECK(op->kind() == widget);
    CHECK(op->num_results() == 1U);
    // the registry answers gracefully for the unknown op — no crash, no central switch
    CHECK(ctx.dialect_of(widget) == nullptr);
    CHECK(ctx.op_info(widget) == nullptr);
    CHECK(ctx.has_trait(widget, OpTrait::Terminator) == false);
    CHECK(ctx.verify(*op)); // opaque (no verifier) => valid
}

TEST_CASE("ceir dialect: a verifier hook runs on its op-kind", "[ceir][dialect][verify]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    fn::register_dialect(ctx);
    Module*    m = ctx.create_module();
    Operation* f = fn::create_func(ctx, *m, "f", Visibility::Public, 0U);
    Block*     body = fn::func_body_block(f);
    REQUIRE(body != nullptr);

    Operation* ret = fn::create_return(ctx, crd::containers::ConstSpan<Value*>{});
    CHECK(ctx.verify(*ret) == false); // not in a block yet → the func.return verifier fails
    body->append(ret);
    CHECK(ctx.verify(*ret));          // now a block terminator → passes
}

TEST_CASE("ceir dialect: an analysis dispatches through an op interface, not a switch on kind",
          "[ceir][dialect][interface]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    fn::register_dialect(ctx);
    Module* m = ctx.create_module();
    (void)fn::create_func(ctx, *m, "target", Visibility::Public, 0U);
    Operation* call = fn::create_call(ctx, "target", crd::containers::ConstSpan<Value*>{}, 0U);

    const InterfaceId call_iface = ctx.intern_interface("ceir.CallInterface");
    ctx.register_interface(fn::call_kind(ctx), call_iface, &kFuncCallInterface);

    // the ANALYSIS knows nothing about func.call — it asks the op-kind for the interface and dispatches through it
    const auto* impl = static_cast<const CallInterface*>(ctx.get_interface(call->kind(), call_iface));
    REQUIRE(impl != nullptr);
    CHECK(impl->callee(ctx, *call) == crd::containers::StringView{"target"});

    // an op that does NOT implement the interface → nullptr (skipped, no switch)
    Operation* ret = fn::create_return(ctx, crd::containers::ConstSpan<Value*>{});
    CHECK(ctx.get_interface(ret->kind(), call_iface) == nullptr);
    // a different interface on a known op → nullptr
    CHECK(ctx.get_interface(call->kind(), ctx.intern_interface("ceir.OtherInterface")) == nullptr);
}
