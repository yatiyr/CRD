// CEIR-7a - the program-as-asset metadata (sec 105-107): the INTERFACE HASH (sec 107) split from the content hash so an
// implementation-only edit hot-swaps without invalidating callers, the DEPENDENCY RECORD (sec 106), and the strict
// cook-time registration check. The headline is the FOUR-EDIT MATRIX pinning sec 107: (a) a body-constant edit changes
// the CONTENT hash but NOT the interface hash; (b) a signature edit changes BOTH; (c) a function REORDER changes NEITHER
// (canonical by-name projection); (d) a state-schema edit changes the interface hash. Plus cross-Context stability.
// Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp> // stable_hash (the content hash)
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/test_ops.hpp> // the `test.dummy` op declares [op.native] -> the intrinsic dep-record vehicle
#include <crd/ceir/program_asset.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u32;
using crd::u64;

namespace
{
struct Reg
{
    OpId cst, muli, state, dummy;
    explicit Reg(Context& c)
        : cst(c.intern_op("arith", "const")), muli(c.intern_op("arith", "muli")), state(c.intern_op("core", "state")),
          dummy(c.intern_op("test", "dummy"))
    {
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
        (void)test::register_test_ops(c);
        (void)func::register_dialect(c);
    }
};
Operation* konst(Context& c, const Reg& r, Block* b, i64 v, TypeId ty)
{
    Operation* const op = c.create_operation(r.cst, {}, 1U, ty);
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}
Operation* mul(Context& c, const Reg& r, Block* b, crd::ceir::Value* x, crd::ceir::Value* y)
{
    crd::ceir::Value* ops[2] = {x, y};
    Operation* const  op     = c.create_operation(r.muli, ConstSpan<crd::ceir::Value*>(ops, 2U), 1U, c.type_i32());
    b->append(op);
    return op;
}
void ret(Context& c, Block* b, crd::ceir::Value* v)
{
    crd::ceir::Value* ops[1] = {v};
    b->append(func::create_return(c, ConstSpan<crd::ceir::Value*>(ops, 1U)));
}
// @name(x: param_ty) -> i32 { return x*x }  (a Public func with a body that squares its arg).
Operation* mkfunc_sq(Context& c, const Reg& r, Module& m, StringView name, TypeId param_ty)
{
    Operation* const f = func::create_func(c, m, name, Visibility::Public, 1U, param_ty);
    m.body()->first_block()->append(f);
    Block* const b = func::func_body_block(f);
    ret(c, b, mul(c, r, b, b->arg(0U), b->arg(0U))->result(0U));
    return f;
}
Block* module_block(Context& c, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = c.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
} // namespace

// ─────────────────────────── the sec 107 four-edit matrix ───────────────────────────

TEST_CASE("ceir 7a: a body-only edit changes the content hash but NOT the interface hash", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    // @f() -> i32 { return K }  --  two modules differing ONLY in the returned constant (an implementation edit).
    Context   a(&root);
    const Reg ra(a);
    Module*   ma = a.create_module();
    (void)module_block(a, *ma);
    Operation* const fa = func::create_func(a, *ma, "f", Visibility::Public, 0U, a.type_i32());
    ma->body()->first_block()->append(fa);
    ret(a, func::func_body_block(fa), konst(a, ra, func::func_body_block(fa), 5, a.type_i32())->result(0U));

    Context   b(&root);
    const Reg rb(b);
    Module*   mb = b.create_module();
    (void)module_block(b, *mb);
    Operation* const fb = func::create_func(b, *mb, "f", Visibility::Public, 0U, b.type_i32());
    mb->body()->first_block()->append(fb);
    ret(b, func::func_body_block(fb), konst(b, rb, func::func_body_block(fb), 6, b.type_i32())->result(0U)); // 6, not 5

    CHECK(interface_hash(a, *ma, &root) == interface_hash(b, *mb, &root)); // sec 107: impl-only edit -> SAME interface
    CHECK(stable_hash(a, *ma, &root) != stable_hash(b, *mb, &root));       // ...but a DIFFERENT content hash
}

TEST_CASE("ceir 7a: a signature edit changes BOTH the interface and content hash", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    Context   a(&root);
    const Reg ra(a);
    Module*   ma = a.create_module();
    (void)module_block(a, *ma);
    (void)mkfunc_sq(a, ra, *ma, "f", a.type_i32());

    Context   b(&root);
    const Reg rb(b);
    Module*   mb = b.create_module();
    (void)module_block(b, *mb);
    (void)mkfunc_sq(b, rb, *mb, "f", b.type_i64()); // param i64, not i32 -- a signature change

    CHECK(interface_hash(a, *ma, &root) != interface_hash(b, *mb, &root));
    CHECK(stable_hash(a, *ma, &root) != stable_hash(b, *mb, &root));
}

TEST_CASE("ceir 7a: reordering function definitions changes NEITHER hash's interface (canonical by name)", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    Context   a(&root);
    const Reg ra(a);
    Module*   ma = a.create_module();
    (void)module_block(a, *ma);
    (void)mkfunc_sq(a, ra, *ma, "alpha", a.type_i32());
    (void)mkfunc_sq(a, ra, *ma, "beta", a.type_i32());

    Context   b(&root);
    const Reg rb(b);
    Module*   mb = b.create_module();
    (void)module_block(b, *mb);
    (void)mkfunc_sq(b, rb, *mb, "beta", b.type_i32()); // reversed definition order
    (void)mkfunc_sq(b, rb, *mb, "alpha", b.type_i32());

    CHECK(interface_hash(a, *ma, &root) == interface_hash(b, *mb, &root)); // canonical sort by name -> reorder-stable
}

TEST_CASE("ceir 7a: adding a state cell changes the interface hash (the migration schema)", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    // @f() -> i32 { return 0 }  vs  the same PLUS a core.state cell in the body.
    Context   a(&root);
    const Reg ra(a);
    Module*   ma = a.create_module();
    (void)module_block(a, *ma);
    Operation* const fa = func::create_func(a, *ma, "f", Visibility::Public, 0U, a.type_i32());
    ma->body()->first_block()->append(fa);
    Block* const ba = func::func_body_block(fa);
    ret(a, ba, konst(a, ra, ba, 0, a.type_i32())->result(0U));

    Context   b(&root);
    const Reg rb(b);
    Module*   mb = b.create_module();
    (void)module_block(b, *mb);
    Operation* const fb = func::create_func(b, *mb, "f", Visibility::Public, 0U, b.type_i32());
    mb->body()->first_block()->append(fb);
    Block* const bb = func::func_body_block(fb);
    crd::ceir::Value* seed[2] = {konst(b, rb, bb, 0, b.type_i32())->result(0U), konst(b, rb, bb, 0, b.type_i32())->result(0U)};
    Operation* const cell = b.create_operation(rb.state, ConstSpan<crd::ceir::Value*>(seed, 2U), 1U, b.type_i32());
    bb->append(cell);
    ret(b, bb, cell->result(0U));

    CHECK(interface_hash(a, *ma, &root) != interface_hash(b, *mb, &root)); // sec 20 state schema is part of the interface
}

TEST_CASE("ceir 7a: the interface and content hashes are cross-Context stable", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    // The SAME module built in two independent Contexts (different intern history) must hash EQUAL -- the projection is
    // structural (no TypeId ints leak), exactly like the 1f binary blob (the dirty-Context purity precedent).
    Context   a(&root);
    const Reg ra(a);
    Module*   ma = a.create_module();
    (void)module_block(a, *ma);
    (void)mkfunc_sq(a, ra, *ma, "f", a.type_i32());
    // pollute b's intern tables first, so its TypeId/OpId ints differ from a's.
    Context b(&root);
    (void)b.intern_op("junk", "op0");
    (void)b.type_f64();
    (void)b.type_vector(b.type_f32(), 3U);
    const Reg rb(b);
    Module*   mb = b.create_module();
    (void)module_block(b, *mb);
    (void)mkfunc_sq(b, rb, *mb, "f", b.type_i32());

    CHECK(interface_hash(a, *ma, &root) == interface_hash(b, *mb, &root));
    CHECK(stable_hash(a, *ma, &root) == stable_hash(b, *mb, &root));
}

// ─────────────────────────── sec 106 dependency records ───────────────────────────

TEST_CASE("ceir 7a: dependency records list EXTERNAL calls, intrinsics, and providers", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    (void)module_block(c, *m);
    // @local() -> i32 { return 0 }  (an INTERNAL callee -- must NOT appear as a dependency)
    Operation* const fl = func::create_func(c, *m, "local", Visibility::Public, 0U, c.type_i32());
    m->body()->first_block()->append(fl);
    ret(c, func::func_body_block(fl), konst(c, r, func::func_body_block(fl), 0, c.type_i32())->result(0U));
    // @main() { call local(); call external(); test.dummy; return }
    Operation* const fm = func::create_func(c, *m, "main", Visibility::Public, 0U, c.type_i32());
    m->body()->first_block()->append(fm);
    Block* const mb = func::func_body_block(fm);
    mb->append(func::create_call(c, "local", {}, 0U, {}));    // internal
    mb->append(func::create_call(c, "external", {}, 0U, {})); // an unresolved IMPORT -> a dependency
    mb->append(c.create_operation(r.dummy, {}, 0U, {}, 0U));  // the intrinsic op (native_provider = "host")
    mb->append(func::create_return(c, {}));

    const DependencyRecord dep = collect_dependencies(c, *m, &root);
    REQUIRE(dep.called_funcs.size() == 1U);
    CHECK(dep.called_funcs[0] == StringView("external")); // ONLY the external call, never `local`
    REQUIRE(dep.intrinsics.size() == 1U);
    CHECK(dep.intrinsics[0] == StringView("test.dummy"));
    REQUIRE(dep.providers.size() == 1U);
    CHECK(dep.providers[0] == StringView("host"));
}

// ─────────────────────────── the strict cook-time registration check ───────────────────────────

TEST_CASE("ceir 7a: find_unregistered_op catches an op of an UNregistered dialect (EMPTY != UNKNOWN)", "[ceir][program-asset]")
{
    crd::memory::MallocAllocator root;
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    Block* const top = module_block(c, *m);

    SECTION("all registered -> nullptr")
    {
        Operation* const f = func::create_func(c, *m, "f", Visibility::Public, 0U, c.type_i32());
        top->append(f);
        ret(c, func::func_body_block(f), konst(c, r, func::func_body_block(f), 0, c.type_i32())->result(0U));
        CHECK(find_unregistered_op(c, *m) == nullptr);
    }
    SECTION("an unregistered-dialect op is returned")
    {
        Operation* const ghost = c.create_operation(c.intern_op("ghost", "op"), {}, 0U, {}, 0U); // never registered
        top->append(ghost);
        CHECK(find_unregistered_op(c, *m) == ghost);
    }
}
