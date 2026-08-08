// CEIR-1b — the SymbolTable + ceir.func gate: define/lookup, visibility, duplicate rejection, a func.func with a body
// (params -> func.return), func.call resolution, and CROSS-MODULE symbol resolution (resolve a call against another
// module's table). Host-only, device-free.

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
namespace fn = crd::ceir::func;

TEST_CASE("ceir func: define, lookup, and visibility are stored per symbol", "[ceir][func][symbol]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = ctx.create_module();
    REQUIRE(m != nullptr);
    REQUIRE(m->symbols() != nullptr);
    REQUIRE(m->symbols()->size() == 0U);

    Operation* pub = fn::create_func(ctx, *m, "exported", Visibility::Public, 0U);
    Operation* prv = fn::create_func(ctx, *m, "internal", Visibility::Private, 0U);
    REQUIRE(pub != nullptr);
    REQUIRE(prv != nullptr);
    CHECK(m->symbols()->size() == 2U);
    CHECK(pub->kind() == fn::func_kind(ctx));
    CHECK(pub->num_regions() == 1U); // the body region

    const SymbolEntry* a = m->symbols()->lookup("exported");
    const SymbolEntry* b = m->symbols()->lookup("internal");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->op == pub);
    CHECK(a->visibility == Visibility::Public);
    CHECK(b->op == prv);
    CHECK(b->visibility == Visibility::Private);
    CHECK(m->symbols()->lookup("nope") == nullptr);
}

TEST_CASE("ceir func: a duplicate symbol is rejected, never a silent overwrite", "[ceir][func][symbol]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = ctx.create_module();

    Operation* first = fn::create_func(ctx, *m, "dup", Visibility::Public, 0U);
    REQUIRE(first != nullptr);
    Operation* second = fn::create_func(ctx, *m, "dup", Visibility::Private, 0U); // same name, different visibility
    CHECK(second == nullptr);
    CHECK(m->symbols()->size() == 1U);
    // the ORIGINAL entry is untouched (still the first op, still Public)
    const SymbolEntry* e = m->symbols()->lookup("dup");
    REQUIRE(e != nullptr);
    CHECK(e->op == first);
    CHECK(e->visibility == Visibility::Public);

    // an empty name is not a function
    CHECK(fn::create_func(ctx, *m, "", Visibility::Public, 0U) == nullptr);
}

TEST_CASE("ceir func: func.func body carries params; func.return terminates over them", "[ceir][func]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = ctx.create_module();

    Operation* add = fn::create_func(ctx, *m, "add", Visibility::Public, 2U); // two params
    REQUIRE(add != nullptr);
    Block* body = fn::func_body_block(add);
    REQUIRE(body != nullptr);
    REQUIRE(body->num_args() == 2U);
    CHECK(body->empty()); // no ops yet

    Value*     rets[2] = {body->arg(0), body->arg(1)};
    Operation* ret     = fn::create_return(ctx, crd::containers::ConstSpan<Value*>(rets, 2U));
    body->append(ret);

    CHECK(ret->kind() == fn::return_kind(ctx));
    CHECK(body->num_ops() == 1U);
    CHECK(body->first_op() == ret);
    REQUIRE(ret->num_operands() == 2U);
    CHECK(ret->operand(0) == body->arg(0));
    CHECK(ret->operand(1) == body->arg(1));
    // the terminator consumes the block args → they now have uses
    CHECK(body->arg(0)->has_uses());
    CHECK(body->arg(1)->has_uses());
}

TEST_CASE("ceir func: func.call resolves its callee within the module", "[ceir][func][symbol]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = ctx.create_module();

    Operation* callee = fn::create_func(ctx, *m, "callee", Visibility::Public, 0U);
    REQUIRE(callee != nullptr);
    Operation* call = fn::create_call(ctx, "callee", crd::containers::ConstSpan<Value*>{}, 1U);
    REQUIRE(call != nullptr);
    CHECK(call->kind() == fn::call_kind(ctx));
    CHECK(call->num_results() == 1U);
    CHECK(fn::resolve_call(ctx, call, *m->symbols()) == callee);

    // an unresolved callee → nullptr, never a wrong op
    Operation* dangling = fn::create_call(ctx, "missing", crd::containers::ConstSpan<Value*>{}, 0U);
    CHECK(fn::resolve_call(ctx, dangling, *m->symbols()) == nullptr);
}

TEST_CASE("ceir func: cross-module symbol resolution is by name, not by which module the call lives in",
          "[ceir][func][symbol]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      a = ctx.create_module();
    Module*                      b = ctx.create_module();

    Operation* shared = fn::create_func(ctx, *a, "shared", Visibility::Public, 0U); // defined in A
    REQUIRE(shared != nullptr);
    Operation* call = fn::create_call(ctx, "shared", crd::containers::ConstSpan<Value*>{}, 0U); // conceptually in B

    // resolves against A's table (the module that OWNS the definition) — cross-module
    CHECK(fn::resolve_call(ctx, call, *a->symbols()) == shared);
    // B's own table has no such symbol → unresolved
    CHECK(fn::resolve_call(ctx, call, *b->symbols()) == nullptr);
    CHECK(b->symbols()->contains("shared") == false);
    CHECK(a->symbols()->contains("shared"));
}
