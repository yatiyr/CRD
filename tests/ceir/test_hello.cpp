// CEIR-1z - the BAND-1 GATE (Section 167): a typed hello-world (func + const + call + return) round-trips text <=>
// binary <=> builder BYTE-IDENTICALLY, and its symbol resolves after EVERY form. "const" is a generic op kind carrying
// a `value` attribute (arith.const {value = N}) - no dedicated dialect. This is the concrete "IR core" acceptance
// test the band contract asks for; the fuzz corpus (test_fuzz) + the guard greps (crd-ceir-invariants ctest, I3/I5/I6)
// complete the gate. Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/builder.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::u8;
using crd::usize;

namespace
{
[[nodiscard]] bool text_equal(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
[[nodiscard]] bool blob_equal(const Array<u8>& a, const Array<u8>& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] ConstSpan<u8> span(const Array<u8>& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }

// module { ^bb0:
//   func.func() {sym_name = "add1"} { ^bb0(%0 : !t1):
//     %1 = arith.const() {value = 1} : !t1
//     %2 = math.add(%0, %1) : !t1
//     func.return(%2) }
//   %3 = arith.const() {value = 41} : !t1
//   %4 = func.call(%3) {callee = @add1} : !t1 }
Module* build_hello_hand(Context& ctx)
{
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);

    Operation* const fn = func::create_func(ctx, *m, "add1", Visibility::Public, 1U, TypeId{1U});
    top->append(fn);
    Block* const fb = func::func_body_block(fn);

    Operation* const c1 = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, TypeId{1U});
    ctx.set_attr(c1, "value", ctx.attr_int(1));
    fb->append(c1);

    Value*           add_ops[2] = {fb->arg(0U), c1->result(0U)};
    Operation* const add = ctx.create_operation(ctx.intern_op("math", "add"), ConstSpan<Value*>(add_ops, 2U), 1U, TypeId{1U});
    fb->append(add);

    Value* ret_ops[1] = {add->result(0U)};
    fb->append(func::create_return(ctx, ConstSpan<Value*>(ret_ops, 1U)));

    Operation* const c41 = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, TypeId{1U});
    ctx.set_attr(c41, "value", ctx.attr_int(41));
    top->append(c41);

    Value*           call_args[1] = {c41->result(0U)};
    Operation* const call = func::create_call(ctx, "add1", ConstSpan<Value*>(call_args, 1U), 1U, TypeId{1U});
    top->append(call);
    return m;
}

Module* build_hello_builder(Context& ctx)
{
    ModuleBuilder mb(ctx);
    (void)mb.add_block(0U);

    Operation* const fn = mb.func("add1", Visibility::Public, 1U, TypeId{1U});
    {
        InsertionGuard g(mb);
        Block* const   fb = func::func_body_block(fn);
        mb.set_insertion(fb);
        Value* const one = mb.op("arith", "const").result(TypeId{1U}).attr("value", ctx.attr_int(1)).build_result();
        Value* const sum = mb.op("math", "add").operand(fb->arg(0U)).operand(one).result(TypeId{1U}).build_result();
        Value* ret_ops[1] = {sum};
        mb.ret(ConstSpan<Value*>(ret_ops, 1U));
    }
    Value* const c41 = mb.op("arith", "const").result(TypeId{1U}).attr("value", ctx.attr_int(41)).build_result();
    Value* call_args[1] = {c41};
    mb.call("add1", ConstSpan<Value*>(call_args, 1U), 1U, TypeId{1U});
    return mb.module();
}

// The `func.call` (last op of the top block) resolves the `func.func` @add1 through the module's symbol table.
void check_symbol_resolves(Context& ctx, Module& m)
{
    const SymbolEntry* const e = m.symbols()->lookup("add1");
    REQUIRE(e != nullptr);
    Operation* const call = m.body()->first_block()->last_op();
    REQUIRE(call != nullptr);
    CHECK(call->kind() == func::call_kind(ctx));
    CHECK(func::resolve_call(ctx, call, *m.symbols()) == e->op);
}
} // namespace

TEST_CASE("ceir hello: the builder and the hand path produce byte-identical text", "[ceir][hello][band1]")
{
    crd::memory::MallocAllocator root;
    Context                      ch(&root);
    Context                      cb(&root);
    CHECK(text_equal(print(ch, *build_hello_hand(ch), &root), print(cb, *build_hello_builder(cb), &root)));
}

TEST_CASE("ceir hello: round-trips text <=> binary <=> builder byte-identically", "[ceir][hello][band1]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module* const                m    = build_hello_hand(ctx);
    const String                 text = print(ctx, *m, &root);

    // text round-trip
    Context           cp(&root);
    const ParseResult pr = parse(cp, StringView(text.data(), text.size()));
    REQUIRE(pr.ok);
    CHECK(text_equal(text, print(cp, *pr.module, &root)));

    // binary round-trip + agreement with the text form
    const Array<u8>   blob = serialize(ctx, *m, &root);
    Context           cd(&root);
    const ParseResult pb = deserialize(cd, span(blob));
    REQUIRE(pb.ok);
    CHECK(blob_equal(blob, serialize(cd, *pb.module, &root)));
    CHECK(text_equal(text, print(cd, *pb.module, &root)));

    // and the builder's native form matches too
    Context cbu(&root);
    CHECK(text_equal(text, print(cbu, *build_hello_builder(cbu), &root)));
}

TEST_CASE("ceir hello: the callee symbol resolves after text parse and binary load", "[ceir][hello][band1][symbol]")
{
    crd::memory::MallocAllocator root;

    // builder-native
    Context cbu(&root);
    check_symbol_resolves(cbu, *build_hello_builder(cbu));

    // after a text round-trip
    Context      ct(&root);
    const String text = print(ct, *build_hello_hand(ct), &root);
    Context      ctp(&root);
    const auto   pr = parse(ctp, StringView(text.data(), text.size()));
    REQUIRE(pr.ok);
    check_symbol_resolves(ctp, *pr.module);

    // after a binary load
    Context         cb(&root);
    const Array<u8> blob = serialize(cb, *build_hello_hand(cb), &root);
    Context         cbl(&root);
    const auto      pb = deserialize(cbl, span(blob));
    REQUIRE(pb.ok);
    check_symbol_resolves(cbl, *pb.module);
}
