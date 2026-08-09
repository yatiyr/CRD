// CEIR-1e (part 1) - the deterministic PRINTER gate: the same semantic graph prints byte-identical text, attributes
// print in name order (canonical, position-independent), and an UNREGISTERED-dialect op prints opaquely by its
// interned "dialect.op" name. The parser + full round-trip land in test_roundtrip.cpp. Host-only. ASCII-only names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::strstr / std::strcmp (test-only content assertions)

using namespace crd::ceir;

namespace
{
// Build: module { ^bb0(%0 : !i32): %1 = test.add(%0, %0) {tag = 7, zebra = "z"} : !i32 }
Module* build_sample(Context& ctx)
{
    Module* m     = ctx.create_module();
    Block*  entry = ctx.create_block(1U, ctx.type_i32());
    m->body()->append(entry);
    Value*     a0        = entry->arg(0U);
    Value*     operands[2] = {a0, a0};
    Operation* add = ctx.create_operation(ctx.intern_op("test", "add"),
                                          crd::containers::ConstSpan<Value*>(operands, 2U), 1U, ctx.type_i32());
    // set attrs OUT of name order to prove the printer canonicalizes (zebra before tag by insertion)
    ctx.set_attr(add, "zebra", ctx.attr_string("z"));
    ctx.set_attr(add, "tag", ctx.attr_int(7));
    entry->append(add);
    return m;
}
} // namespace

TEST_CASE("ceir print: the same graph prints byte-identical text (deterministic)", "[ceir][print]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = build_sample(ctx);

    const crd::containers::String a = print(ctx, *m, &root);
    const crd::containers::String b = print(ctx, *m, &root);
    REQUIRE(a.size() == b.size());
    CHECK(std::strcmp(a.c_str(), b.c_str()) == 0);
}

TEST_CASE("ceir print: canonical shape - value ids, sorted attrs, op name, types", "[ceir][print]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = build_sample(ctx);
    const crd::containers::String s = print(ctx, *m, &root);
    const char* const            t = s.c_str();

    CHECK(std::strstr(t, "module {") != nullptr);
    CHECK(std::strstr(t, "^bb0(%0 : !i32):") != nullptr);       // block arg with its type
    CHECK(std::strstr(t, "%1 = test.add(%0, %0)") != nullptr);  // result id, op name, operand refs
    CHECK(std::strstr(t, "{tag = 7, zebra = \"z\"}") != nullptr); // attrs SORTED by name (tag < zebra), int + string
    CHECK(std::strstr(t, ": !i32") != nullptr);                 // result type
}

TEST_CASE("ceir print: an unregistered-dialect op prints opaquely by its interned name", "[ceir][print]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m     = ctx.create_module();
    Block*                       entry = ctx.create_block(0U);
    m->body()->append(entry);
    // no dialect registered — a plugin op still prints by its "plugin.widget" name
    entry->append(ctx.create_operation(ctx.intern_op("plugin", "widget"), {}, 0U));
    const crd::containers::String s = print(ctx, *m, &root);
    CHECK(std::strstr(s.c_str(), "plugin.widget()") != nullptr);
}

TEST_CASE("ceir print: float attributes are round-trippable and typed distinctly from ints", "[ceir][print]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m     = ctx.create_module();
    Block*                       entry = ctx.create_block(0U);
    m->body()->append(entry);
    Operation* op = ctx.create_operation(ctx.intern_op("test", "c"), {}, 0U);
    ctx.set_attr(op, "whole", ctx.attr_float(4.0)); // must print "4.0" not "4" (else it re-reads as an int)
    ctx.set_attr(op, "frac", ctx.attr_float(4.5));
    entry->append(op);
    const crd::containers::String s = print(ctx, *m, &root);
    CHECK(std::strstr(s.c_str(), "frac = 4.5") != nullptr);
    CHECK(std::strstr(s.c_str(), "whole = 4.0") != nullptr);
}
