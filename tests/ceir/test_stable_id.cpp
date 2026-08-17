// CEIR-8d (ADR-0114) — content-independent STABLE SEMANTIC IDENTITY. The matrix: a fresh module gets pre-order ids
// 1..N; assign_stable_ids is idempotent; ids ride the op through serialize/deserialize (not position); a hostile STID
// chunk (a 0 id, a duplicate id) is a GRACEFUL reject; the content hash (stable_hash) is id-INDEPENDENT (STID-skip) so
// content-identical modules with different ids hash equal and a pre-8d blob's content hash is unchanged; the blob is a
// pure function of content, not Context history; the §20 state schema is reorder-invariant yet delete/re-add-sensitive
// (id VALUES in the hash); kBinaryVersion is unchanged; and a fresh module's text round-trip reproduces its ids.
// Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>        // serialize / deserialize / stable_hash / kBinaryVersion
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/program_asset.hpp> // interface_hash

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using ByteArray = Array<u8>;

namespace
{
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// A module whose body is `n` bare `test.op`s (0 results) in creation/body order; fills `out[0..n)` with the ops.
Module* mod_with_ops(Context& ctx, u32 n, Operation** out)
{
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    for (u32 i = 0; i < n; ++i)
    {
        Operation* const op = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
        top->append(op);
        out[i] = op;
    }
    return m;
}
// gather ops in body pre-order (the STID / assignment order).
void gather(Region* r, Array<Operation*>& out)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.push_back(op);
            for (u32 i = 0; i < op->num_regions(); ++i) { gather(op->region(i), out); }
        }
    }
}
} // namespace

TEST_CASE("ceir 8d: a fresh module gets pre-order stable ids 1..N; assign is idempotent", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);

    CHECK(ops[0]->stable_id().valid() == false); // unassigned until a persist/consumer needs them
    ctx.assign_stable_ids(*m);
    CHECK(ops[0]->stable_id() == StableId{1U});
    CHECK(ops[1]->stable_id() == StableId{2U});
    CHECK(ops[2]->stable_id() == StableId{3U});
    ctx.assign_stable_ids(*m); // idempotent — a second call is a no-op
    CHECK(ops[0]->stable_id() == StableId{1U});
    CHECK(ops[2]->stable_id() == StableId{3U});

    // an op appended AFTER assignment gets max+1 (never a reused/pre-order-shifted id).
    Operation* const late = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
    m->body()->first_block()->append(late);
    ctx.assign_stable_ids(*m);
    CHECK(late->stable_id() == StableId{4U});
    CHECK(ops[0]->stable_id() == StableId{1U}); // existing ids untouched
}

TEST_CASE("ceir 8d: stable ids ride the op through serialize/deserialize (not position)", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);
    const ByteArray              blob   = serialize(ctx, *m, &root); // assigns 1,2,3 + emits STID

    Context           ctx2(&root);
    const ParseResult pr = deserialize(ctx2, span(blob));
    REQUIRE(pr.ok);
    Array<Operation*> got(&root);
    gather(pr.module->body(), got);
    REQUIRE(got.size() == 3U);
    CHECK(got[0]->stable_id() == StableId{1U});
    CHECK(got[1]->stable_id() == StableId{2U});
    CHECK(got[2]->stable_id() == StableId{3U});
    // a re-serialize is byte-exact (ids preserved, not reassigned).
    CHECK(blob_eq(blob, serialize(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir 8d: a hostile STID chunk (zero id / duplicate id) is a graceful reject", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[2] = {};
    Module* const                m      = mod_with_ops(ctx, 2U, ops);
    const ByteArray              blob   = serialize(ctx, *m, &root); // STID is the LAST chunk; the final 8 bytes = op1's id

    REQUIRE(blob.size() >= 8U);
    // ZERO id: clobber the final u64 (op1's stable id) to 0 → the loader rejects an invalid id (not an assert).
    {
        ByteArray b(&root);
        for (usize i = 0; i < blob.size(); ++i) { b.push_back(blob[i]); }
        for (usize i = 0; i < 8U; ++i) { b[b.size() - 8U + i] = 0U; }
        Context c(&root);
        CHECK_FALSE(deserialize(c, span(b)).ok);
    }
    // DUPLICATE id: set op1's id equal to op0's id (1). The STID payload is [count=2][id0=1][id1=2]; the final u64 is id1.
    {
        ByteArray b(&root);
        for (usize i = 0; i < blob.size(); ++i) { b.push_back(blob[i]); }
        b[b.size() - 8U] = 1U; // low byte of id1 -> 1 (id0 is 1); the rest are already 0
        for (usize i = 1; i < 8U; ++i) { b[b.size() - 8U + i] = 0U; }
        Context c(&root);
        CHECK_FALSE(deserialize(c, span(b)).ok);
    }
}

TEST_CASE("ceir 8d: the content hash is id-INDEPENDENT (stable_hash skips STID)", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   a_ops[2] = {};
    Module* const                a        = mod_with_ops(ctx, 2U, a_ops);
    ctx.assign_stable_ids(*a); // 1,2

    Operation*    b_ops[2] = {};
    Module* const b        = mod_with_ops(ctx, 2U, b_ops); // identical content
    ctx.set_stable_id(b_ops[0], StableId{100U});           // but DIFFERENT ids (simulating a different edit history)
    ctx.set_stable_id(b_ops[1], StableId{200U});

    CHECK(stable_hash(ctx, *a, &root) == stable_hash(ctx, *b, &root));   // content hash ignores ids
    CHECK_FALSE(blob_eq(serialize(ctx, *a, &root), serialize(ctx, *b, &root))); // but the full blobs differ (STID)
}

TEST_CASE("ceir 8d: a pre-8d blob's content hash is unchanged, and the blob is a pure function of content", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    // "pre-8d" = a blob with no STID: it is exactly what stable_hash hashes (with_stid=false). Prove the content hash is
    // unchanged after a decode->serialize round-trip (the STID-skip fixpoint), and that the same graph in a clean vs a
    // pre-polluted Context serializes byte-equal (ids are content-pure — the gate a Context counter would have broken).
    Context       polluted(&root);
    Operation*    junk[5] = {};
    (void)mod_with_ops(polluted, 5U, junk); // pollute the Context with unrelated ops FIRST

    Context    clean(&root);
    Operation* a_ops[3] = {};
    Operation* b_ops[3] = {};
    Module* const a = mod_with_ops(clean, 3U, a_ops);
    Module* const b = mod_with_ops(polluted, 3U, b_ops);
    CHECK(blob_eq(serialize(clean, *a, &root), serialize(polluted, *b, &root))); // Context history does not leak into ids
    CHECK(stable_hash(clean, *a, &root) == stable_hash(polluted, *b, &root));
}

TEST_CASE("ceir 8d: the state schema is reorder-invariant yet delete/re-add-sensitive (id values in the hash)", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d    = ctx.register_dialect("st");
    const OpId                   cell = d->register_op("cell", {.traits = flags_of(OpTrait::StateEdge)});

    // Two cells {id=1:f32, id=2:i32} in body order A, and the SAME two REVERSED in body order B (ids preserved by a
    // reorder). Sorting by stable id makes A and B hash EQUAL — the reorder false-incompatible is fixed.
    auto build = [&](bool reversed, u64 id_first, u64 id_second) -> Module* {
        Module* const m   = ctx.create_module();
        Block* const  top = ctx.create_block(0U);
        m->body()->append(top);
        Operation* const c_f32 = ctx.create_operation(cell, {}, 1U, ctx.type_f32());
        Operation* const c_i32 = ctx.create_operation(cell, {}, 1U, ctx.type_i32());
        if (!reversed) { top->append(c_f32); top->append(c_i32); }
        else           { top->append(c_i32); top->append(c_f32); }
        ctx.set_stable_id(c_f32, StableId{id_first});
        ctx.set_stable_id(c_i32, StableId{id_second});
        return m;
    };
    Module* const a = build(/*reversed*/ false, 1U, 2U); // body [f32#1, i32#2]
    Module* const b = build(/*reversed*/ true, 1U, 2U);  // body [i32#2, f32#1] — same cells, reordered, ids preserved
    CHECK(interface_hash(ctx, *a, &root) == interface_hash(ctx, *b, &root)); // reorder-invariant (the fix)

    // delete id=1 + add id=2 (same TYPE) must be INCOMPATIBLE — the id VALUE is in the hash, not just the order.
    Module* const c = ctx.create_module();
    Block* const  ct = ctx.create_block(0U);
    c->body()->append(ct);
    Operation* const c1 = ctx.create_operation(cell, {}, 1U, ctx.type_f32());
    ct->append(c1);
    ctx.set_stable_id(c1, StableId{1U});
    Module* const dmod = ctx.create_module();
    Block* const  dt   = ctx.create_block(0U);
    dmod->body()->append(dt);
    Operation* const d1 = ctx.create_operation(cell, {}, 1U, ctx.type_f32());
    dt->append(d1);
    ctx.set_stable_id(d1, StableId{2U});
    CHECK(interface_hash(ctx, *c, &root) != interface_hash(ctx, *dmod, &root)); // {id1:f32} != {id2:f32}
}

TEST_CASE("ceir 8d: an id freed by erase is NEVER reused (the watermark guards identity)", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);
    ctx.assign_stable_ids(*m); // ids 1,2,3; watermark 3
    CHECK(m->stable_id_watermark() == 3U);

    ops[2]->erase(); // erase the max-id op (0 results ⇒ legal); it is tombstoned + unlinked ⇒ invisible to the scan
    Operation* const late = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
    m->body()->first_block()->append(late);
    ctx.assign_stable_ids(*m);
    CHECK(late->stable_id() == StableId{4U}); // ⛔ NOT 3 (the erased id) — the watermark prevents reuse
}

TEST_CASE("ceir 8d: the id watermark survives serialize/load, preventing reuse across a round-trip", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);
    ctx.assign_stable_ids(*m); // watermark 3
    ops[2]->erase();           // now only 2 live ops (ids 1,2), but the watermark is 3
    const ByteArray blob = serialize(ctx, *m, &root); // STID: count=2, watermark=3, ids [1,2]

    Context           ctx2(&root);
    const ParseResult pr = deserialize(ctx2, span(blob));
    REQUIRE(pr.ok);
    CHECK(pr.module->stable_id_watermark() == 3U); // the watermark was restored
    Operation* const late = ctx2.create_operation(ctx2.intern_op("test", "op"), {}, 0U);
    pr.module->body()->first_block()->append(late);
    ctx2.assign_stable_ids(*pr.module);
    CHECK(late->stable_id() == StableId{4U}); // ⛔ the freed id 3 is not reused even after a serialize/load cycle
}

TEST_CASE("ceir 8d: a genuine pre-8d blob (no STID chunk) decodes and re-serializes to a fixpoint", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);
    const ByteArray              full   = serialize(ctx, *m, &root); // 6 chunks incl. STID; ids 1,2,3, watermark 3

    // Strip the trailing STID chunk to forge a genuine pre-8d blob: STID = fourcc(4)+len(4)+payload[count(4)+
    // watermark(8)+3*id(24)] = 8 + 36 = 44 bytes. Then patch chunk_count (the 3rd u32, offset 8) from 6 to 5.
    REQUIRE(full.size() > 44U);
    ByteArray pre(&root);
    for (usize i = 0; i < full.size() - 44U; ++i) { pre.push_back(full[i]); }
    pre[8] = 5U; // chunk_count 6 -> 5 (little-endian low byte)

    Context           ctx2(&root);
    const ParseResult pr = deserialize(ctx2, span(pre));
    REQUIRE(pr.ok); // ⛔ a pre-8d blob decodes fine (the decode_stid !m_has_stid path — the whole backward-compat story)
    Array<Operation*> got(&root);
    gather(pr.module->body(), got);
    REQUIRE(got.size() == 3U);
    for (usize i = 0; i < got.size(); ++i) { CHECK(got[i]->stable_id().value == 0U); } // no STID ⇒ ids unassigned

    // re-serialize: fresh ids re-seed pre-order 1,2,3 = exactly what `full` carried ⇒ BYTE-EXACT (the fixpoint) + same
    // content hash (stronger than hash-equal alone).
    CHECK(blob_eq(full, serialize(ctx2, *pr.module, &root)));
    CHECK(stable_hash(ctx2, *pr.module, &root) == stable_hash(ctx, *m, &root));
}

TEST_CASE("ceir 8d: the binary MODULE format is unchanged (no kBinaryVersion bump)", "[ceir][stable-id]")
{
    CHECK(kBinaryVersion == 2U); // ⛔ STID is additive/forward-skippable; the content hash skips it — no format bump
}

TEST_CASE("ceir 8d: a fresh module's text round-trip reproduces identical stable ids", "[ceir][stable-id]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);
    const String                 t      = print(ctx, *m, &root);

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, StringView(t.data(), t.size()));
    REQUIRE(pr.ok);
    // text carries no ids (the id-free content projection), so BOTH sides re-seed pre-order — a fresh module reproduces
    // identical ids (only post-EDIT id history is lost through text; that rides the binary form).
    ctx.assign_stable_ids(*m);
    ctx2.assign_stable_ids(*pr.module);
    Array<Operation*> a(&root);
    Array<Operation*> b(&root);
    gather(m->body(), a);
    gather(pr.module->body(), b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) { CHECK(a[i]->stable_id() == b[i]->stable_id()); }
}

TEST_CASE("ceir 8d: single-byte corruption of a stable-id blob never crashes a loader", "[ceir][stable-id]")
{
    // The hostile-input guard extended over the STID decode arm (count, per-id read, dup/zero checks). ASan/UBSan is the
    // memory-safety proof; ok/!ok both acceptable, the ONLY invariant is no crash.
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Operation*                   ops[3] = {};
    Module* const                m      = mod_with_ops(ctx, 3U, ops);
    const ByteArray              blob   = serialize(ctx, *m, &root);

    for (usize i = 0; i < blob.size(); ++i)
    {
        ByteArray b(&root);
        for (usize j = 0; j < blob.size(); ++j) { b.push_back(blob[j]); }
        b[i] = static_cast<u8>(b[i] ^ 0xFFU);
        Context           c(&root);
        const ParseResult pr = deserialize(c, span(b));
        (void)pr; // must not crash
    }
    CHECK(true);
}
