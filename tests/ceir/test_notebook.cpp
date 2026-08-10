// CEIR-9a (UNIVERSALITY VALIDATION, U1 Notebook/incremental, U-§82/U-§119). ⛔ A PROOF, not a feature: a
// notebook/reactive-recompute workload runs on the CEIR foundation with ZERO new machinery — the 8h
// `crd::containers::IncrementalDag` IS the dirty engine, 8d stable ids ARE cell identity, 8i transactions ARE the edit
// path. The cells are REAL CEIR arith ops (const/addi/muli); deps are SSA operands; each cell's CONTENT revision = a
// hash of its FORMULA (op kind + operand DEP stable-ids + attrs — reorder/id-INDEPENDENT), its INTERFACE revision = a
// hash of its computed VALUE. That mapping makes the §107 rule the notebook EARLY-CUTOFF: a formula edit that preserves
// the value hot-swaps its dependents (they never recompute). The mock evaluator (a const/addi/muli interpreter +
// per-cell recompute COUNTER) is the notebook's compute; the dependency machinery (topo order, deps, revisions, the
// §107 propagation rule) is ALL the engine — the consumer is evaluate+compare+propagate glue, NOT a second engine.
// Every edit rides a Transaction (the touched-set seeds the incremental eval; a no-op edit's content hash yields zero
// recomputes — the content-addressed memo). Exact recompute/cache-hit COUNTS asserted. Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>          // umbrella: context/ir/transaction/diagnostic
#include <crd/ceir/gen/arith_ops.hpp> // arith::register_arith_ops (const/addi/muli)
#include <crd/containers/incremental_dag.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include "incremental_helpers.hpp" // fnv_mix / content_hash / value_hash (CEIR-9x shared)

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::test; // NOLINT(google-build-using-namespace) — the shared incremental-proof helpers
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::IncrementalDag;
using crd::containers::StringView;
using crd::memory::IAllocator;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

namespace
{
// A mock notebook over REAL CEIR arith cells + the 8h engine. Cells are indexed by (stable_id - 1) = 0..N-1 (fresh
// module ⇒ pre-order ids 1..N). `dependents[i]` = the DIRECT dependents of cell i (derived from the engine's dep edges).
struct Notebook
{
    Context&    ctx;
    IAllocator* alloc;
    Module*             m = nullptr;
    IncrementalDag      dag;
    OpId                cst, addi, muli;
    Array<Operation*>   cell;       // stable_id-1 -> op
    Array<i64>          value;      // cached value
    Array<u32>          recomputes; // recompute counter
    Array<Array<u64>>   dependents; // direct dependents (stable ids)

    Notebook(Context& c, IAllocator* a)
        : ctx(c), alloc(a), dag(a), cst(c.intern_op("arith", "const")), addi(c.intern_op("arith", "addi")),
          muli(c.intern_op("arith", "muli")), cell(a), value(a), recomputes(a), dependents(a)
    {
        (void)arith::register_arith_ops(c);
    }

    [[nodiscard]] Operation* konst(Block* b, i64 v)
    {
        Operation* const o = ctx.create_operation(cst, {}, 1U, ctx.type_i32());
        ctx.set_attr(o, "value", ctx.attr_int(v));
        b->append(o);
        return o;
    }
    [[nodiscard]] Operation* bin(Block* b, OpId k, Operation* x, Operation* y)
    {
        Value* const     ops[2] = {x->result(0), y->result(0)};
        Operation* const o      = ctx.create_operation(k, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
        b->append(o);
        return o;
    }
    [[nodiscard]] i64 eval(const Operation& op) const
    {
        if (op.kind() == cst) { return ctx.attr_value(op.attr("value")).i; }
        const i64 a = value[op.operand(0)->defining_op()->stable_id().value - 1U];
        const i64 b = value[op.operand(1)->defining_op()->stable_id().value - 1U];
        return op.kind() == addi ? a + b : a * b;
    }
    [[nodiscard]] u32 count(const Operation* op) const { return recomputes[op->stable_id().value - 1U]; }

    // Build the fixture: A=2, B=3, Z=0; C=A+B; C2=A*Z (value 0, STABLE under A-edits ⇒ the chained-cutoff node);
    // E=C+A; F=C2+B. Deps: C→{A,B}, C2→{A,Z}, E→{C,A}, F→{C2,B}. assign_stable_ids ⇒ ids 1..7 in build order.
    void build(Array<Operation*>& out)
    {
        m                = ctx.create_module();
        Block* const top = ctx.create_block(0U);
        m->body()->append(top);
        Operation* const a  = konst(top, 2);
        Operation* const b  = konst(top, 3);
        Operation* const z  = konst(top, 0);
        Operation* const c  = bin(top, addi, a, b);
        Operation* const c2 = bin(top, muli, a, z);
        Operation* const e  = bin(top, addi, c, a);
        Operation* const f  = bin(top, addi, c2, b);
        ctx.assign_stable_ids(*m);
        Operation* const ops[7] = {a, b, z, c, c2, e, f};
        for (u32 i = 0; i < 7U; ++i)
        {
            cell.push_back(ops[i]);
            value.push_back(0);
            recomputes.push_back(0U);
            dependents.push_back(Array<u64>(alloc));
            out.push_back(ops[i]);
        }
        // build the engine graph + the derived direct-dependents map (from the engine's dep edges).
        for (u32 i = 0; i < 7U; ++i)
        {
            const u64 id = cell[i]->stable_id().value;
            dag.add_node(id);
            for (u32 k = 0; k < cell[i]->num_operands(); ++k)
            {
                const Operation* const d = cell[i]->operand(k)->defining_op();
                if (d == nullptr) { continue; }
                const u64 did = d->stable_id().value;
                dag.add_edge(id, did);
                bool seen = false;
                for (usize j = 0; j < dependents[did - 1U].size(); ++j)
                {
                    if (dependents[did - 1U][j] == id) { seen = true; }
                }
                if (!seen) { dependents[did - 1U].push_back(id); }
            }
        }
    }
    // Initial full evaluation: every cell computed once, in the engine's topo order (deps first).
    void full_eval()
    {
        Array<u64> order(alloc);
        REQUIRE(dag.topo_order(order));
        for (usize i = 0; i < order.size(); ++i)
        {
            const u64        id = order[i];
            const Operation& op = *cell[id - 1U];
            const i64        v  = eval(op);
            value[id - 1U]      = v;
            recomputes[id - 1U] += 1U;
            dag.set_revision(id, content_hash(op, ctx), value_hash(v));
        }
    }
    // Incremental re-evaluation seeded by an edit's touched-set. Processes the ENGINE's topo order (deps first); a
    // SEEDED cell recomputes IFF its formula (content hash) actually changed (the content-addressed memo — a no-op edit
    // skips); a non-seeded cell recomputes IFF a dep's value changed; after a recompute, propagation to DIRECT dependents
    // happens IFF the cell's INTERFACE (value) changed (the §107 early-cutoff). All structure/revisions are the engine's.
    void incremental_eval(ConstSpan<StableId> seed)
    {
        Array<u8> seeded(alloc);
        Array<u8> depdirty(alloc);
        for (u32 i = 0; i < 7U; ++i)
        {
            seeded.push_back(0U);
            depdirty.push_back(0U);
        }
        for (usize i = 0; i < seed.size(); ++i) { seeded[seed[i].value - 1U] = 1U; }
        Array<u64> order(alloc);
        REQUIRE(dag.topo_order(order));
        for (usize i = 0; i < order.size(); ++i)
        {
            const u64        id          = order[i];
            const Operation& op          = *cell[id - 1U];
            const u64        new_content = content_hash(op, ctx);
            bool             must        = false;
            if (seeded[id - 1U] != 0U && new_content != dag.content_of(id)) { must = true; } // formula changed (not a no-op)
            if (depdirty[id - 1U] != 0U) { must = true; }                                    // a dependency's value changed
            if (!must) { continue; }                                                         // memo / no-op hit
            const u64 old_interface = dag.interface_of(id);
            const i64 v             = eval(op);
            value[id - 1U]          = v;
            recomputes[id - 1U] += 1U;
            const u64 new_interface = value_hash(v);
            dag.set_revision(id, new_content, new_interface);
            if (new_interface != old_interface) // §107: propagate ONLY on a value change (early-cutoff otherwise)
            {
                for (usize k = 0; k < dependents[id - 1U].size(); ++k) { depdirty[dependents[id - 1U][k] - 1U] = 1U; }
            }
        }
    }
    // An authored edit that rides a TRANSACTION (8i): change a const's value; commit; return the touched stable-ids.
    [[nodiscard]] Array<StableId> edit_const(Operation* c, i64 newval)
    {
        DiagnosticEngine diag(ctx, alloc);
        Transaction      tx(ctx, *m, diag, alloc);
        REQUIRE(tx.set_attr(c, "value", ctx.attr_int(newval)));
        REQUIRE(tx.commit());
        Array<StableId> t(alloc);
        for (usize i = 0; i < tx.touched().size(); ++i) { t.push_back(tx.touched()[i]); }
        return t;
    }
    // A value-PRESERVING formula edit: swap a commutative binary op's operands (content changes, value does not).
    [[nodiscard]] Array<StableId> edit_swap(Operation* c)
    {
        DiagnosticEngine diag(ctx, alloc);
        Transaction      tx(ctx, *m, diag, alloc);
        Value* const     op0 = c->operand(0);
        Value* const     op1 = c->operand(1);
        REQUIRE(tx.set_operand(c, 0U, op1)); // transient (op1, op1) mid-tx is fine — verify runs at commit
        REQUIRE(tx.set_operand(c, 1U, op0));
        REQUIRE(tx.commit());
        Array<StableId> t(alloc);
        for (usize i = 0; i < tx.touched().size(); ++i) { t.push_back(tx.touched()[i]); }
        return t;
    }
};
} // namespace

TEST_CASE("ceir 9a: initial evaluation computes every cell exactly once", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    nb.full_eval();
    for (u32 i = 0; i < 7U; ++i) { CHECK(nb.recomputes[i] == 1U); }
    // the fixture values: A=2 B=3 Z=0 C=5 C2=0 E=7 F=3.
    CHECK(nb.value[3] == 5); // C = A+B
    CHECK(nb.value[4] == 0); // C2 = A*Z
    CHECK(nb.value[5] == 7); // E = C+A
    CHECK(nb.value[6] == 3); // F = C2+B
}

TEST_CASE("ceir 9a: editing an input recomputes exactly its value-dependents; a stable node cuts propagation", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    nb.full_eval();
    Operation* const a = c[0];

    const Array<StableId> touched = nb.edit_const(a, 10); // A: 2 -> 10 (a real value change)
    REQUIRE(touched.size() == 1U);
    CHECK(touched[0] == a->stable_id());
    nb.incremental_eval(ConstSpan<StableId>(touched.data(), touched.size()));

    // A's VALUE changed -> A, C, C2, E recompute; but C2 = A*0 is VALUE-STABLE (0), so F (its only dependent) is CUT.
    CHECK(nb.recomputes[0] == 2U); // A recomputed
    CHECK(nb.recomputes[3] == 2U); // C recomputed
    CHECK(nb.recomputes[4] == 2U); // C2 recomputed (its value is 0, unchanged)
    CHECK(nb.recomputes[5] == 2U); // E recomputed
    CHECK(nb.recomputes[1] == 1U); // B cache-hit (not a dependent of A)
    CHECK(nb.recomputes[2] == 1U); // Z cache-hit
    CHECK(nb.recomputes[6] == 1U); // F CACHE-HIT — the chained early-cutoff through the stable C2
    CHECK(nb.value[0] == 10);
    CHECK(nb.value[3] == 13); // C = A+B = 10+3
    CHECK(nb.value[5] == 23); // E = C+A = 13+10
}

TEST_CASE("ceir 9a: the engine dependent-set is a conservative superset; the evaluator prunes the cut node", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    nb.full_eval();
    Operation* const a = c[0];
    Operation* const f = c[6];

    // The ENGINE's structural dependent set of A (8h affected_by) is CONSERVATIVE — it includes F (structurally
    // downstream via C2). The evaluator's PRECISE set (value-driven) excludes F (C2 is value-stable). The difference is
    // exactly the cut node. This documents conservative-structure (engine) vs precise-value (consumer) — the proof.
    Array<u64> aff(&root);
    REQUIRE(nb.dag.affected_by(a->stable_id().value, aff));
    bool engine_has_f = false;
    for (usize i = 0; i < aff.size(); ++i)
    {
        if (aff[i] == f->stable_id().value) { engine_has_f = true; }
    }
    CHECK(engine_has_f); // F is in the engine's structural dependents...

    const Array<StableId> touched = nb.edit_const(a, 10);
    nb.incremental_eval(ConstSpan<StableId>(touched.data(), touched.size()));
    CHECK(nb.count(f) == 1U); // ...but the evaluator did NOT recompute it (value-stable C2 cut it) -> the named cut node
}

TEST_CASE("ceir 9a: editing a different input recomputes a precise partial set", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    nb.full_eval();
    Operation* const b = c[1];

    const Array<StableId> touched = nb.edit_const(b, 30); // B: 3 -> 30
    nb.incremental_eval(ConstSpan<StableId>(touched.data(), touched.size()));

    // B's dependents: C (A+B), F (C2+B), and C->E. So {B, C, E, F} recompute; A, Z, and C2 (=A*Z, no B dep) cache-hit.
    CHECK(nb.recomputes[1] == 2U); // B
    CHECK(nb.recomputes[3] == 2U); // C
    CHECK(nb.recomputes[5] == 2U); // E
    CHECK(nb.recomputes[6] == 2U); // F
    CHECK(nb.recomputes[0] == 1U); // A cache-hit
    CHECK(nb.recomputes[2] == 1U); // Z cache-hit
    CHECK(nb.recomputes[4] == 1U); // C2 cache-hit — NOT a dependent of B (the precise-set proof)

    // engine dependent set of B == the evaluator's recomputed dependents (every B-dependent's value genuinely changed).
    Array<u64> aff(&root);
    REQUIRE(nb.dag.affected_by(b->stable_id().value, aff));
    CHECK(aff.size() == 3U); // {C, E, F}
    for (usize i = 0; i < aff.size(); ++i) { CHECK(nb.recomputes[aff[i] - 1U] == 2U); }
}

TEST_CASE("ceir 9a: a value-preserving formula edit hot-swaps its dependents (the section-107 cutoff)", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    nb.full_eval();
    Operation* const cc = c[3]; // C = addi(A, B)

    const u64 old_content   = nb.dag.content_of(cc->stable_id().value);
    const u64 old_interface = nb.dag.interface_of(cc->stable_id().value);

    const Array<StableId> touched = nb.edit_swap(cc); // addi(A,B) -> addi(B,A): content changes, value 5 unchanged
    REQUIRE(touched.size() == 1U);
    CHECK(touched[0] == cc->stable_id());
    // the CONTENT (formula) hash changed but the VALUE hash did not — the early-cutoff precondition.
    CHECK(content_hash(*cc, ctx) != old_content);
    CHECK(value_hash(nb.eval(*cc)) == old_interface);

    nb.incremental_eval(ConstSpan<StableId>(touched.data(), touched.size()));
    CHECK(nb.recomputes[3] == 2U); // C recomputed (its formula changed)
    CHECK(nb.recomputes[5] == 1U); // E CACHE-HIT — C's value was unchanged, so its dependent hot-swaps (section 107)
    for (u32 i = 0; i < 7U; ++i)
    {
        if (i != 3U) { CHECK(nb.recomputes[i] == 1U); } // nothing but C recomputed
    }
}

TEST_CASE("ceir 9a: a no-op edit recomputes nothing (content-addressed memo through the transaction seam)", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    nb.full_eval();
    Operation* const a = c[0];

    // A transaction sets A's value to its CURRENT value (2) -> the tx reports A as touched (an edit happened), but A's
    // CONTENT hash is unchanged, so the content-addressed memo recomputes NOTHING. The tx says WHAT was edited; the
    // hash decides WHETHER work happens — the 8i -> 8h division of labour.
    const Array<StableId> touched = nb.edit_const(a, 2);
    REQUIRE(touched.size() == 1U);
    CHECK(touched[0] == a->stable_id()); // the tx DID report the edit
    nb.incremental_eval(ConstSpan<StableId>(touched.data(), touched.size()));
    for (u32 i = 0; i < 7U; ++i) { CHECK(nb.recomputes[i] == 1U); } // ...but zero recomputes
}

TEST_CASE("ceir 9a: identical formulas hash equal regardless of cell identity or position", "[ceir][notebook]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Notebook                     nb(ctx, &root);
    Array<Operation*>            c(&root);
    nb.build(c);
    // A second addi(A,B) built LATER (a different op, a later stable id, a later position) has the SAME formula over the
    // same deps -> the SAME content hash (the hash keys on dep IDENTITY, not creation order) -> a reorder is a cache HIT.
    Operation* const a = c[0];
    Operation* const b = c[1];
    Value* const     ops[2] = {a->result(0), b->result(0)};
    Operation* const g      = ctx.create_operation(nb.addi, ConstSpan<Value*>(ops, 2U), 1U, ctx.type_i32());
    nb.m->body()->first_block()->append(g);
    ctx.assign_stable_ids(*nb.m);
    CHECK(g->stable_id().value != c[3]->stable_id().value);          // different cells...
    CHECK(content_hash(*g, ctx) == content_hash(*c[3], ctx));         // ...identical formula hash (position-independent)
}
