// CEIR-8i (ADR-0119) — TRANSACTIONS: the atomic authored-mutation surface. The matrix: commit applies the edits and
// reports the touched/removed 8h node-sets; a mid-transaction FAILURE (a poisoned edit OR a commit-verify failure such
// as a duplicate symbol) rolls back to a BYTE-IDENTICAL module (the A/B proof); reverse-replay restores exact block
// order under interleaved erase/insert (both orders); the 8d delete/re-add discriminator survives a committed
// transaction (a replacement never reuses a freed id); the watermark is restored under rollback even after a
// mid-transaction serialize; a rejected edit poisons rather than asserts; a rolled-back attr snapshot outlives the
// Transaction (the Context-arena UAF guard); and the touched set drives the 8h IncrementalDag §107 rule.
// Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>          // umbrella: Context/ir/transaction/diagnostic/binary
#include <crd/ceir/binary.hpp>        // serialize / stable_hash
#include <crd/containers/incremental_dag.hpp> // the 8h seam consumer

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::IncrementalDag;
using crd::containers::StringView;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using ByteArray = Array<u8>;

namespace
{
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// A module with one empty top block (returned via `top`).
Module* single_block(Context& ctx, Block*& top)
{
    Module* const m = ctx.create_module();
    top             = ctx.create_block(0U);
    m->body()->append(top);
    return m;
}
// Create + append a bare op (no operands) with `num_results` results of type `rt`.
Operation* add_op(Context& ctx, Block* b, const char* dialect, const char* name, u32 num_results = 0U, TypeId rt = {})
{
    Operation* const op = ctx.create_operation(ctx.intern_op(dialect, name), {}, num_results, rt);
    b->append(op);
    return op;
}
// Snapshot a block's op order (pointers) for exact-order assertions.
void block_ops(Block* b, Array<Operation*>& out)
{
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { out.push_back(op); }
}
[[nodiscard]] bool id_in(ConstSpan<StableId> s, StableId id) noexcept
{
    for (usize i = 0; i < s.size(); ++i)
    {
        if (s[i] == id) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("ceir 8i: commit applies the edits and reports touched/removed", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             a   = add_op(ctx, top, "test", "a");

    DiagnosticEngine diag(ctx, &root);
    Transaction      tx(ctx, *m, diag, &root);
    Operation* const b = tx.insert(ctx.intern_op("test", "b"), {}, 1U, top, nullptr, ctx.type_i32());
    REQUIRE(b != nullptr);
    CHECK(tx.set_attr(a, "x", ctx.attr_int(1)));
    REQUIRE(tx.commit());
    CHECK_FALSE(tx.is_poisoned());

    // the edits stuck: the block is [a, b]; a carries x=1.
    Array<Operation*> ops(&root);
    block_ops(top, ops);
    REQUIRE(ops.size() == 2U);
    CHECK(ops[0] == a);
    CHECK(ops[1] == b);
    CHECK(a->attr("x") == ctx.attr_int(1));

    // touched = the inserted (b) + the modified (a); removed empty; both ids resolvable after commit.
    CHECK(id_in(tx.touched(), a->stable_id()));
    CHECK(id_in(tx.touched(), b->stable_id()));
    CHECK(tx.removed().size() == 0U);
    CHECK(tx.find(a->stable_id()) == a);
    CHECK(diag.count() == 0U);
}

TEST_CASE("ceir 8i: a poisoned edit rolls back to a byte-identical module (the A/B proof)", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             a   = add_op(ctx, top, "test", "producer", 1U, ctx.type_i32());
    Value*                       av  = a->result(0);
    Operation* const             c   = ctx.create_operation(ctx.intern_op("test", "consumer"),
                                                            ConstSpan<Value*>(&av, 1U), 0U); // consumes a's result
    top->append(c);

    const ByteArray before = serialize(ctx, *m, &root); // A

    DiagnosticEngine diag(ctx, &root);
    {
        Transaction tx(ctx, *m, diag, &root);
        // a valid edit first (must also be reverted), then an ILLEGAL erase (a's result is used by c) → poison.
        Operation* const x = tx.insert(ctx.intern_op("test", "scratch"), {}, 0U, top);
        REQUIRE(x != nullptr);
        CHECK(tx.set_attr(a, "k", ctx.attr_int(5)));
        CHECK_FALSE(tx.erase(a)); // rejected — result still used
        CHECK(tx.is_poisoned());
        CHECK_FALSE(tx.commit()); // auto-rolls-back
    }

    const ByteArray after = serialize(ctx, *m, &root); // B
    CHECK(blob_eq(before, after));
    CHECK(a->has_attr("k") == false); // the valid pre-poison edit was reverted too
    bool rejected = false;
    for (usize i = 0; i < diag.count(); ++i)
    {
        if (diag.at(i).code == make_diagnostic_code("ceir.transaction.rejected")) { rejected = true; }
    }
    CHECK(rejected);
}

TEST_CASE("ceir 8i: a duplicate-symbol commit fails and rolls back byte-identically", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             a   = add_op(ctx, top, "test", "sym");
    ctx.set_attr(a, "sym_name", ctx.attr_string("dup")); // a pre-existing symbol "dup"

    const ByteArray before = serialize(ctx, *m, &root);

    DiagnosticEngine diag(ctx, &root);
    {
        Transaction      tx(ctx, *m, diag, &root);
        Operation* const b = tx.insert(ctx.intern_op("test", "sym2"), {}, 0U, top);
        REQUIRE(b != nullptr);
        CHECK(tx.set_attr(b, "sym_name", ctx.attr_string("dup"))); // a SECOND "dup" — a commit-verify failure
        CHECK_FALSE(tx.commit());                                   // resync detects the duplicate → rollback
    }

    const ByteArray after = serialize(ctx, *m, &root);
    CHECK(blob_eq(before, after));
    bool dup = false;
    for (usize i = 0; i < diag.count(); ++i)
    {
        if (diag.at(i).code == make_diagnostic_code("ceir.transaction.duplicate_symbol")) { dup = true; }
    }
    CHECK(dup);
}

TEST_CASE("ceir 8i: reverse-order replay restores exact block order (both interleavings)", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;

    // Variant 1: erase b, THEN insert x before c.
    {
        Context          ctx(&root);
        Block*           top = nullptr;
        Module* const    m   = single_block(ctx, top);
        Operation* const a   = add_op(ctx, top, "test", "a");
        Operation* const b   = add_op(ctx, top, "test", "b");
        Operation* const c   = add_op(ctx, top, "test", "c");
        DiagnosticEngine diag(ctx, &root);
        {
            Transaction tx(ctx, *m, diag, &root);
            CHECK(tx.erase(b));
            CHECK(tx.insert(ctx.intern_op("test", "x"), {}, 0U, top, c) != nullptr);
            tx.rollback();
        }
        Array<Operation*> ops(&root);
        block_ops(top, ops);
        REQUIRE(ops.size() == 3U);
        CHECK(ops[0] == a);
        CHECK(ops[1] == b);
        CHECK(ops[2] == c);
    }
    // Variant 2: insert x before c, THEN erase b (b's recorded anchor is now x).
    {
        Context          ctx(&root);
        Block*           top = nullptr;
        Module* const    m   = single_block(ctx, top);
        Operation* const a   = add_op(ctx, top, "test", "a");
        Operation* const b   = add_op(ctx, top, "test", "b");
        Operation* const c   = add_op(ctx, top, "test", "c");
        DiagnosticEngine diag(ctx, &root);
        {
            Transaction tx(ctx, *m, diag, &root);
            CHECK(tx.insert(ctx.intern_op("test", "x"), {}, 0U, top, c) != nullptr);
            CHECK(tx.erase(b));
            tx.rollback();
        }
        Array<Operation*> ops(&root);
        block_ops(top, ops);
        REQUIRE(ops.size() == 3U);
        CHECK(ops[0] == a);
        CHECK(ops[1] == b);
        CHECK(ops[2] == c);
    }
}

TEST_CASE("ceir 8i: a replacement never reuses a freed id across a committed transaction", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             p   = add_op(ctx, top, "test", "p");
    Operation* const             k   = add_op(ctx, top, "test", "k");
    (void)serialize(ctx, *m, &root); // settle ids: p=1, k=2, watermark=2
    REQUIRE(p->stable_id() == StableId{1U});
    REQUIRE(k->stable_id() == StableId{2U});

    DiagnosticEngine diag(ctx, &root);
    Transaction      tx(ctx, *m, diag, &root);
    CHECK(tx.erase(k));
    Operation* const kk = tx.insert(ctx.intern_op("test", "kk"), {}, 0U, top);
    REQUIRE(kk != nullptr);
    REQUIRE(tx.commit());

    // kk draws from the watermark (2) → id 3, NOT k's freed id 2 (the 8d delete/re-add discriminator holds).
    CHECK(kk->stable_id() == StableId{3U});
    CHECK_FALSE(kk->stable_id() == StableId{2U});
    CHECK(id_in(tx.touched(), StableId{3U}));  // kk inserted
    CHECK(id_in(tx.removed(), StableId{2U}));  // k removed
    CHECK_FALSE(id_in(tx.touched(), StableId{2U}));
}

TEST_CASE("ceir 8i: rollback restores the watermark even after a mid-transaction serialize", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             a   = add_op(ctx, top, "test", "a");

    const ByteArray before = serialize(ctx, *m, &root); // settles a=1, watermark=1
    REQUIRE(a->stable_id() == StableId{1U});
    REQUIRE(m->stable_id_watermark() == 1U);

    DiagnosticEngine diag(ctx, &root);
    {
        Transaction tx(ctx, *m, diag, &root);
        Operation* const b = tx.insert(ctx.intern_op("test", "b"), {}, 0U, top);
        REQUIRE(b != nullptr);
        (void)serialize(ctx, *m, &root);          // mid-transaction persist → b gets id 2, watermark → 2
        CHECK(m->stable_id_watermark() == 2U);
        tx.rollback();
    }
    CHECK(m->stable_id_watermark() == 1U);        // restored to the begin value
    const ByteArray after = serialize(ctx, *m, &root);
    CHECK(blob_eq(before, after));                // byte-identical despite the mid-transaction id churn
}

TEST_CASE("ceir 8i: an erase with a live result is rejected, not asserted", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             a   = add_op(ctx, top, "test", "producer", 1U, ctx.type_i32());
    Value*                       av  = a->result(0);
    Operation* const             c   = ctx.create_operation(ctx.intern_op("test", "consumer"), ConstSpan<Value*>(&av, 1U), 0U);
    top->append(c);

    DiagnosticEngine diag(ctx, &root);
    Transaction      tx(ctx, *m, diag, &root);
    CHECK_FALSE(tx.erase(a)); // graceful reject (result used) — no assert
    CHECK(tx.is_poisoned());
    CHECK(a->is_erased() == false); // no mutation happened
    REQUIRE(diag.count() >= 1U);
    CHECK(diag.at(0).code == make_diagnostic_code("ceir.transaction.rejected"));
}

TEST_CASE("ceir 8i: a rolled-back attr snapshot outlives the Transaction", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             a   = add_op(ctx, top, "test", "a");
    ctx.set_attr(a, "keep", ctx.attr_int(1)); // a pre-existing attr

    const ByteArray before = serialize(ctx, *m, &root);

    DiagnosticEngine diag(ctx, &root);
    {
        Transaction tx(ctx, *m, diag, &root);
        CHECK(tx.set_attr(a, "keep", ctx.attr_int(99))); // overwrite-in-place branch
        CHECK(tx.set_attr(a, "added", ctx.attr_int(7))); // grow-by-rebuild branch
        tx.rollback();
    } // ⛔ the Transaction is DESTROYED here — the restored dict must NOT be tx-owned (linux-asan UAF guard)

    CHECK(a->attr("keep") == ctx.attr_int(1)); // restored value, read AFTER the tx died
    CHECK(a->has_attr("added") == false);       // the added attr is gone
    const ByteArray after = serialize(ctx, *m, &root);
    CHECK(blob_eq(before, after));
}

TEST_CASE("ceir 8i: the touched set drives the 8h IncrementalDag rule", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             p   = add_op(ctx, top, "test", "p", 1U, ctx.type_i32());
    Value*                       pv  = p->result(0);
    Operation* const             q   = ctx.create_operation(ctx.intern_op("test", "q"), ConstSpan<Value*>(&pv, 1U), 0U);
    top->append(q);
    (void)serialize(ctx, *m, &root); // settle ids: p=1, q=2

    // A consumer's IncrementalDag: q depends on p (q consumes p's result).
    IncrementalDag dag(&root);
    dag.set_revision(p->stable_id().value, 10U, 100U);
    dag.set_revision(q->stable_id().value, 20U, 200U);
    dag.add_edge(q->stable_id().value, p->stable_id().value);

    // Edit p through a transaction; the tx reports p as touched (q is a DERIVED dependent, not reported).
    DiagnosticEngine diag(ctx, &root);
    Transaction      tx(ctx, *m, diag, &root);
    CHECK(tx.set_attr(p, "x", ctx.attr_int(1)));
    REQUIRE(tx.commit());
    REQUIRE(id_in(tx.touched(), p->stable_id()));
    CHECK_FALSE(id_in(tx.touched(), q->stable_id())); // q is derived by the engine, never reported by the tx

    // Feed the touched id to the engine. A CONTENT-ONLY change recomputes only p (q hot-swaps).
    Array<u64> out(&root);
    REQUIRE(dag.recompute_after_change(p->stable_id().value, 11U, 100U, out)); // content changed, interface same
    REQUIRE(out.size() == 1U);
    CHECK(out[0] == p->stable_id().value);

    // An INTERFACE change propagates to the dependent q (the §107 rule).
    Array<u64> out2(&root);
    REQUIRE(dag.recompute_after_change(p->stable_id().value, 12U, 101U, out2)); // interface changed
    REQUIRE(out2.size() == 2U);
    CHECK(out2[0] == p->stable_id().value);
    CHECK(out2[1] == q->stable_id().value);
}

// ── erase of a REGION-BEARING op: both subtree-boundary directions are rejected; a CLOSED subtree is accepted ──
// A region op with one region holding a nested block (returns the op; the caller fills the block).
Operation* add_region_op(Context& ctx, Block* parent, const char* name, Block*& inner)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("test", name), {}, 0U, {}, 1U);
    parent->append(op);
    inner = ctx.create_block(0U);
    op->region(0)->append(inner);
    return op;
}

TEST_CASE("ceir 8i: erasing a region-bearing op with a nested IN-edge is rejected", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Operation* const             ext = add_op(ctx, top, "test", "ext", 1U, ctx.type_i32());
    Block*                       inner = nullptr;
    Operation* const             r   = add_region_op(ctx, top, "region", inner);
    Value*                       extv = ext->result(0);
    Operation* const             n = ctx.create_operation(ctx.intern_op("test", "inner"), ConstSpan<Value*>(&extv, 1U), 0U);
    inner->append(n); // n consumes ext, defined OUTSIDE r's subtree — an in-edge

    DiagnosticEngine diag(ctx, &root);
    Transaction      tx(ctx, *m, diag, &root);
    CHECK_FALSE(tx.erase(r)); // the subtree walk catches the crossing operand
    CHECK(tx.is_poisoned());
    CHECK(r->is_erased() == false);
}

TEST_CASE("ceir 8i: erasing a region-bearing op with a nested OUT-edge is rejected", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Block*                       top = nullptr;
    Module* const                m   = single_block(ctx, top);
    Block*                       inner = nullptr;
    Operation* const             r   = add_region_op(ctx, top, "region", inner);
    Operation* const             n   = ctx.create_operation(ctx.intern_op("test", "inner"), {}, 1U, ctx.type_i32());
    inner->append(n);
    Value*           nv = n->result(0);
    Operation* const o  = ctx.create_operation(ctx.intern_op("test", "outer"), ConstSpan<Value*>(&nv, 1U), 0U);
    top->append(o); // o (outside r) consumes n's result (nested) — an out-edge

    DiagnosticEngine diag(ctx, &root);
    Transaction      tx(ctx, *m, diag, &root);
    CHECK_FALSE(tx.erase(r)); // the subtree walk catches the escaping nested result
    CHECK(tx.is_poisoned());
    CHECK(r->is_erased() == false);
}

TEST_CASE("ceir 8i: erasing a CLOSED region-bearing subtree commits, and rolls back byte-identically", "[ceir][transaction]")
{
    crd::memory::GrowableTlsfAllocator root;

    // Part A: a CLOSED subtree (nested edges all internal) commits — the whole subtree is gone.
    {
        Context          ctx(&root);
        Block*           top   = nullptr;
        Module* const    m     = single_block(ctx, top);
        Block*           inner = nullptr;
        Operation* const r     = add_region_op(ctx, top, "region", inner);
        Operation* const n1    = ctx.create_operation(ctx.intern_op("test", "n1"), {}, 1U, ctx.type_i32());
        inner->append(n1);
        Value*           n1v = n1->result(0);
        Operation* const n2  = ctx.create_operation(ctx.intern_op("test", "n2"), ConstSpan<Value*>(&n1v, 1U), 0U);
        inner->append(n2); // closed: n2 uses n1 (inside), n1's only use is inside, n2's result is unused

        DiagnosticEngine diag(ctx, &root);
        Transaction      tx(ctx, *m, diag, &root);
        CHECK(tx.erase(r)); // accepted — no crossing edge
        REQUIRE(tx.commit());
        CHECK(top->empty());                          // r and its whole subtree are gone
        CHECK(id_in(tx.removed(), r->stable_id()));   // r reported removed (id settled at begin)
    }
    // Part B: erasing the SAME closed subtree then rolling back is byte-identical.
    {
        Context          ctx(&root);
        Block*           top   = nullptr;
        Module* const    m     = single_block(ctx, top);
        Block*           inner = nullptr;
        Operation* const r     = add_region_op(ctx, top, "region", inner);
        Operation* const n1    = ctx.create_operation(ctx.intern_op("test", "n1"), {}, 1U, ctx.type_i32());
        inner->append(n1);
        Value*           n1v = n1->result(0);
        Operation* const n2  = ctx.create_operation(ctx.intern_op("test", "n2"), ConstSpan<Value*>(&n1v, 1U), 0U);
        inner->append(n2);

        const ByteArray  before = serialize(ctx, *m, &root);
        DiagnosticEngine diag(ctx, &root);
        {
            Transaction tx(ctx, *m, diag, &root);
            CHECK(tx.erase(r));
            tx.rollback(); // reinsert_erased_op restores r whole (erase never recursed into the subtree)
        }
        const ByteArray after = serialize(ctx, *m, &root);
        CHECK(blob_eq(before, after));
        CHECK_FALSE(top->empty()); // r is back
    }
}
