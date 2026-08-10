// CEIR-10a - the hot-reload STATE-MIGRATION core surfaces (stage 1, crd-ceir core): `collect_state_schema` (the
// module-wide (stable_id, type, depth) cells the migration keys on), `contract_hash` (the sec 107 projection MINUS the
// state schema, so a signature change [Reject] is distinguished from a state-schema-only change [Migrate]), and the
// Interpreter `snapshot_state_by_id`/`restore_state_by_id` value-move (a cell carries across a generation swap by 8d
// stable id; an absent id or a depth mismatch is SKIPPED -> the new generation init-fills). Host-only. ASCII test names.
//
// The DECISION TABLE this validates (used by the 10a supervisor):
//   interface_hash equal                         -> CompatibleReuse
//   interface_hash differ + contract_hash differ -> Reject (callers break; a migration fn cannot save them)
//   interface_hash differ + contract_hash equal  -> Migrate (only the state schema changed; a migration fn may cover it)

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/program_asset.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility> // std::move

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::exec; // NOLINT(google-build-using-namespace)  -- Interpreter/ExecResult/StateSnapshot/install_*
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u32;
using crd::u64;

namespace
{
struct Reg
{
    OpId cst, state;
    explicit Reg(Context& c) : cst(c.intern_op("arith", "const")), state(c.intern_op("core", "state"))
    {
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
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
void ret(Context& c, Block* b, Value* v)
{
    Value* ops[1] = {v};
    b->append(func::create_return(c, ConstSpan<Value*>(ops, 1U)));
}
// %s = core.state(init, next[, depth]); returns the state op (result(0) = the cell value).
Operation* state_cell(Context& c, const Reg& r, Block* b, Value* init, Value* next, TypeId ty, u32 depth = 1U)
{
    Value* seed[2] = {init, next};
    Operation* const op = c.create_operation(r.state, ConstSpan<Value*>(seed, 2U), 1U, ty);
    if (depth != 1U) { c.set_attr(op, "depth", c.attr_int(static_cast<i64>(depth))); }
    b->append(op);
    return op;
}
// @name() -> i32 { %i = 0; %n = nextval; %s = state(%i, %n[, depth]); return %s }  (a stateful "next-latches-to-nextval"
// counter: invoke 1 returns 0 then latches to nextval; invoke 2 returns nextval; ...).
Operation* mkfunc_counter(Context& c, const Reg& r, Module& m, StringView name, i64 nextval, u32 depth = 1U)
{
    Operation* const f = func::create_func(c, m, name, Visibility::Public, 0U, c.type_i32());
    m.body()->first_block()->append(f);
    Block* const b = func::func_body_block(f);
    Value* const init = konst(c, r, b, 0, c.type_i32())->result(0U);
    Value* const nxt  = konst(c, r, b, nextval, c.type_i32())->result(0U);
    Operation* const s = state_cell(c, r, b, init, nxt, c.type_i32(), depth);
    ret(c, b, s->result(0U));
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
i64 run_once(Interpreter& in, Module& m)
{
    const ExecResult r = in.invoke(m, StringView("f"), ConstSpan<i64>());
    REQUIRE(r.ok());
    REQUIRE(r.values.size() == 1U);
    return r.values[0];
}
} // namespace

// ─────────────────────────── collect_state_schema ───────────────────────────

TEST_CASE("ceir 10a: collect_state_schema lists every cell sorted by stable id with type and depth", "[ceir][reload]")
{
    crd::memory::MallocAllocator root;
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    (void)module_block(c, *m);
    (void)mkfunc_counter(c, r, *m, "f", 7, /*depth=*/1U);   // one i32 cell, depth 1
    (void)mkfunc_counter(c, r, *m, "g", 9, /*depth=*/3U);   // one i32 cell, depth 3 (g's body)

    const auto schema = collect_state_schema(c, *m, &root);
    REQUIRE(schema.size() == 2U);
    CHECK(schema[0].id < schema[1].id);              // sorted by stable id
    CHECK(schema[0].type == c.type_i32());
    CHECK(schema[1].type == c.type_i32());
    // depths: the two funcs carry depth 1 and depth 3 (order-by-id, both present as a set).
    const bool depths_ok = (schema[0].depth == 1U && schema[1].depth == 3U) ||
                           (schema[0].depth == 3U && schema[1].depth == 1U);
    CHECK(depths_ok);
}

// ─────────────────────────── contract_hash — the decision-table split ───────────────────────────

TEST_CASE("ceir 10a: a body-only edit leaves BOTH interface_hash and contract_hash equal", "[ceir][reload]")
{
    crd::memory::MallocAllocator root;
    Context a(&root); const Reg ra(a); Module* ma = a.create_module(); (void)module_block(a, *ma);
    (void)mkfunc_counter(a, ra, *ma, "f", 7);
    Context b(&root); const Reg rb(b); Module* mb = b.create_module(); (void)module_block(b, *mb);
    (void)mkfunc_counter(b, rb, *mb, "f", 8); // the latched constant (a body value) differs -- an impl-only edit

    CHECK(interface_hash(a, *ma, &root) == interface_hash(b, *mb, &root));
    CHECK(contract_hash(a, *ma, &root) == contract_hash(b, *mb, &root));
}

TEST_CASE("ceir 10a: a signature edit changes BOTH interface_hash and contract_hash (Reject)", "[ceir][reload]")
{
    crd::memory::MallocAllocator root;
    // @f() -> i32 vs @f() -> i64 : the caller-visible result type changed -- callers break, a migration fn cannot help.
    Context a(&root); const Reg ra(a); Module* ma = a.create_module(); (void)module_block(a, *ma);
    Operation* const fa = func::create_func(a, *ma, "f", Visibility::Public, 0U, a.type_i32());
    ma->body()->first_block()->append(fa);
    ret(a, func::func_body_block(fa), konst(a, ra, func::func_body_block(fa), 0, a.type_i32())->result(0U));

    Context b(&root); const Reg rb(b); Module* mb = b.create_module(); (void)module_block(b, *mb);
    Operation* const fb = func::create_func(b, *mb, "f", Visibility::Public, 0U, b.type_i64());
    mb->body()->first_block()->append(fb);
    ret(b, func::func_body_block(fb), konst(b, rb, func::func_body_block(fb), 0, b.type_i64())->result(0U));

    CHECK(interface_hash(a, *ma, &root) != interface_hash(b, *mb, &root));
    CHECK(contract_hash(a, *ma, &root) != contract_hash(b, *mb, &root));
}

TEST_CASE("ceir 10a: a state-schema-only edit changes interface_hash but NOT contract_hash (Migrate)", "[ceir][reload]")
{
    crd::memory::MallocAllocator root;
    // The SAME signature/effects/body, differing ONLY in the state cell's depth (1 vs 2) -- the one delta a migration fn
    // may cover. contract_hash (signature + effects + caps, NO state schema) is IDENTICAL; interface_hash differs.
    Context a(&root); const Reg ra(a); Module* ma = a.create_module(); (void)module_block(a, *ma);
    (void)mkfunc_counter(a, ra, *ma, "f", 7, /*depth=*/1U);
    Context b(&root); const Reg rb(b); Module* mb = b.create_module(); (void)module_block(b, *mb);
    (void)mkfunc_counter(b, rb, *mb, "f", 7, /*depth=*/2U); // ONLY the ring depth differs

    CHECK(interface_hash(a, *ma, &root) != interface_hash(b, *mb, &root)); // the state schema IS part of the interface
    CHECK(contract_hash(a, *ma, &root) == contract_hash(b, *mb, &root));   // ...but the caller contract is UNCHANGED
}

// ─────────────────────────── Interpreter snapshot / restore (the value move) ───────────────────────────

TEST_CASE("ceir 10a: a state cell VALUE migrates across a generation swap by stable id", "[ceir][reload]")
{
    crd::memory::MallocAllocator root;
    // OLD generation: run once -> returns 0 (init), the cell latches to 7.
    Context old_c(&root); const Reg old_r(old_c); Module* old_m = old_c.create_module(); (void)module_block(old_c, *old_m);
    (void)mkfunc_counter(old_c, old_r, *old_m, "f", 7);
    (void)collect_state_schema(old_c, *old_m, &root); // assign stable ids on the old module (the migration key)
    Interpreter old_in(old_c);
    install_builtin_semantics(old_in);
    CHECK(run_once(old_in, *old_m) == 0);  // init
    // after one latch the cell holds 7.
    crd::containers::Array<StateSnapshot> snap(&root);
    old_in.snapshot_state_by_id(snap, &root);
    REQUIRE(snap.size() == 1U);

    // NEW generation, identical structure. WITHOUT restore, a fresh run re-inits to 0.
    Context new_c(&root); const Reg new_r(new_c); Module* new_m = new_c.create_module(); (void)module_block(new_c, *new_m);
    (void)mkfunc_counter(new_c, new_r, *new_m, "f", 7);
    {
        Interpreter fresh(new_c);
        install_builtin_semantics(fresh);
        CHECK(run_once(fresh, *new_m) == 0); // control: no migration -> init 0
    }

    // WITH restore, the migrated cell continues from 7.
    Interpreter migrated(new_c);
    install_builtin_semantics(migrated);
    const u32 n = migrated.restore_state_by_id(*new_m, ConstSpan<StateSnapshot>(snap.data(), snap.size()));
    CHECK(n == 1U);
    CHECK(run_once(migrated, *new_m) == 7); // the OLD generation's latched value survived the swap
}

TEST_CASE("ceir 10a: an absent id and a depth mismatch are SKIPPED -> the new generation init-fills", "[ceir][reload]")
{
    crd::memory::MallocAllocator root;
    Context old_c(&root); const Reg old_r(old_c); Module* old_m = old_c.create_module(); (void)module_block(old_c, *old_m);
    (void)mkfunc_counter(old_c, old_r, *old_m, "f", 7, /*depth=*/1U);
    (void)collect_state_schema(old_c, *old_m, &root);
    Interpreter old_in(old_c);
    install_builtin_semantics(old_in);
    (void)run_once(old_in, *old_m); // cell latches to 7
    crd::containers::Array<StateSnapshot> snap(&root);
    old_in.snapshot_state_by_id(snap, &root);
    REQUIRE(snap.size() == 1U);

    SECTION("a DEPTH MISMATCH (depth 1 snapshot into a depth-2 new cell) is skipped")
    {
        Context new_c(&root); const Reg new_r(new_c); Module* new_m = new_c.create_module(); (void)module_block(new_c, *new_m);
        (void)mkfunc_counter(new_c, new_r, *new_m, "f", 7, /*depth=*/2U); // ring depth 2 != the snapshot's 1
        Interpreter in(new_c);
        install_builtin_semantics(in);
        const u32 n = in.restore_state_by_id(*new_m, ConstSpan<StateSnapshot>(snap.data(), snap.size()));
        CHECK(n == 0U);                    // skipped
        CHECK(run_once(in, *new_m) == 0);  // init-filled, not corrupted
    }
    SECTION("an ABSENT id (a bogus snapshot) restores nothing")
    {
        Context new_c(&root); const Reg new_r(new_c); Module* new_m = new_c.create_module(); (void)module_block(new_c, *new_m);
        (void)mkfunc_counter(new_c, new_r, *new_m, "f", 7);
        crd::containers::Array<StateSnapshot> bogus(&root);
        StateSnapshot s(&root);
        s.id = ~static_cast<u64>(0); s.pos = 0U; s.ring.push_back(999); // an id no op carries
        bogus.push_back(std::move(s));
        Interpreter in(new_c);
        install_builtin_semantics(in);
        const u32 n = in.restore_state_by_id(*new_m, ConstSpan<StateSnapshot>(bogus.data(), bogus.size()));
        CHECK(n == 0U);
        CHECK(run_once(in, *new_m) == 0);
    }
}
