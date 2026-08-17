// CEIR-9d (UNIVERSALITY VALIDATION, U4 CAD parametric + TRANSACTION, U-§85/U-§31/U-§120). ⛔ A PROOF, not a feature: a
// parametric-CAD feature graph runs on the foundation with ZERO new machinery — the 8i TRANSACTION is the HEADLINE
// (commit/rollback over 8d stable ids), the 8h `affected_by` is the depsgraph (from 9c), and a CAD CONSTRAINT is just
// the transaction's existing commit-VERIFY (a sketch verifier) — no new constraint system. The mock `cad` dialect is
// inline-registered (zero central edits): sketch(width,height,constraint) -> extrude(depth); a second
// sketch->extrude; assembly(join) -> fillet(radius) -> body. A "geometry" is a mock scalar. The four transaction proofs:
// (1) a single dimension edit re-evaluates its dependent subtree (cite 9c); (2) MULTI-OP edits in ONE transaction seed
// the eval from the WHOLE touched-set (the 9a multi-cell named-forward, now the centerpiece); (3) a no-op edit inside a
// multi-op transaction recomputes only the real edit's subtree (multi-op UNION x content-addressed memo); (4) a
// transactional edit ROLLS BACK BYTE-IDENTICALLY (the 8i money-test on a feature graph); (5) a CONSTRAINT-violating
// commit is REJECTED (verify-fail -> rollback, byte-identical) while a satisfying commit succeeds. Host-only. ASCII names.

#include <crd/ceir/ceir.hpp>   // umbrella: context/ir/dialect/transaction/diagnostic/binary
#include <crd/ceir/binary.hpp> // serialize
#include <crd/ceir/effect.hpp> // EffectRecord / EffectFamily::ConstraintRead
#include <crd/containers/incremental_dag.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include "incremental_helpers.hpp" // content_hash / value_hash (CEIR-9x shared)

#include <cstring> // std::memcmp

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
using ByteArray = Array<u8>;

namespace
{
// The CAD CONSTRAINT is the sketch's VERIFIER — the transaction's existing commit-verify enforces it, no new mechanism.
// constraint attr: 0 = none, 1 = "width == height" (an equal-dimensions constraint).
bool verify_sketch(const Context& ctx, const Operation& op) noexcept
{
    const AttrId c = op.attr("constraint");
    if (!c.valid() || ctx.attr_value(c).i == 0) { return true; }
    return ctx.attr_value(op.attr("width")).i == ctx.attr_value(op.attr("height")).i;
}
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

struct Edit
{
    Operation*  op;
    StringView  param;
    i64         val;
};

// A mock parametric-CAD feature graph over the 8h engine. Two features (sketch1->extrude1, sketch2->extrude2) join at
// an assembly, then fillet -> body. `deps[i]` = a feature's UPSTREAM inputs (stable ids).
struct CadGraph
{
    Context&          ctx;
    IAllocator*       alloc;
    Module*           m = nullptr;
    IncrementalDag    dag;
    OpId              sketch, extrude, assembly, fillet, body;
    Array<Operation*> cell;
    Array<i64>        value;
    Array<u32>        recomputes;
    Array<Array<u64>> deps;

    CadGraph(Context& c, IAllocator* a)
        : ctx(c), alloc(a), dag(a), sketch(c.intern_op("cad", "sketch")), extrude(c.intern_op("cad", "extrude")),
          assembly(c.intern_op("cad", "assembly")), fillet(c.intern_op("cad", "fillet")),
          body(c.intern_op("cad", "body")), cell(a), value(a), recomputes(a), deps(a)
    {
        Dialect* const     d       = c.register_dialect("cad");
        const EffectRecord cread[1] = {EffectRecord{EffectFamily::ConstraintRead}};
        (void)d->register_op("sketch", OpSpec{.verify       = &verify_sketch, // the CONSTRAINT enforcement (commit-verify)
                                              .effects      = ConstSpan<EffectRecord>(cread, 1U), // 8c classification
                                              .determinism  = DeterminismClass::BitExact,
                                              .domain       = EvalDomain::CookTime});
        (void)d->register_op("extrude", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("assembly", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("fillet", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("body", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
    }

    [[nodiscard]] Operation* sketch_op(Block* b, i64 w, i64 h, i64 constraint)
    {
        Operation* const o = ctx.create_operation(sketch, {}, 1U, ctx.type_i64());
        ctx.set_attr(o, "width", ctx.attr_int(w));
        ctx.set_attr(o, "height", ctx.attr_int(h));
        ctx.set_attr(o, "constraint", ctx.attr_int(constraint));
        b->append(o);
        return o;
    }
    [[nodiscard]] Operation* feature(Block* b, OpId k, ConstSpan<Value*> inputs, StringView param, i64 pval)
    {
        Operation* const o = ctx.create_operation(k, inputs, 1U, ctx.type_i64());
        if (!param.empty()) { ctx.set_attr(o, param, ctx.attr_int(pval)); }
        b->append(o);
        return o;
    }
    [[nodiscard]] i64 eval(const Operation& op) const
    {
        if (op.kind() == sketch) { return ctx.attr_value(op.attr("width")).i * ctx.attr_value(op.attr("height")).i; }
        const i64 in0 = value[op.operand(0)->defining_op()->stable_id().value - 1U];
        if (op.kind() == extrude) { return in0 * ctx.attr_value(op.attr("depth")).i; }
        if (op.kind() == fillet) { return in0 + ctx.attr_value(op.attr("radius")).i; }
        if (op.kind() == assembly) { return in0 + value[op.operand(1)->defining_op()->stable_id().value - 1U]; }
        return in0; // body
    }
    [[nodiscard]] u32 count(const Operation* op) const { return recomputes[op->stable_id().value - 1U]; }

    // sketch1(4,4,constraint=equal)->extrude1(2); sketch2(3,5,none)->extrude2(2); assembly(e1,e2)->fillet(1)->body.
    // ids 1..7 in creation order: sketch1, extrude1, sketch2, extrude2, assembly, fillet, body.
    void build(Array<Operation*>& out)
    {
        m                = ctx.create_module();
        Block* const top = ctx.create_block(0U);
        m->body()->append(top);
        Operation* const s1     = sketch_op(top, 4, 4, 0); // unconstrained by default; the constraint test activates it
        Value* const     e1in[1] = {s1->result(0)};
        Operation* const e1     = feature(top, extrude, ConstSpan<Value*>(e1in, 1U), StringView("depth"), 2);
        Operation* const s2     = sketch_op(top, 3, 5, 0);
        Value* const     e2in[1] = {s2->result(0)};
        Operation* const e2     = feature(top, extrude, ConstSpan<Value*>(e2in, 1U), StringView("depth"), 2);
        Value* const     asin[2] = {e1->result(0), e2->result(0)};
        Operation* const as     = feature(top, assembly, ConstSpan<Value*>(asin, 2U), StringView(), 0);
        Value* const     fin[1] = {as->result(0)};
        Operation* const fl     = feature(top, fillet, ConstSpan<Value*>(fin, 1U), StringView("radius"), 1);
        Value* const     bin[1] = {fl->result(0)};
        Operation* const bd     = feature(top, body, ConstSpan<Value*>(bin, 1U), StringView(), 0);
        ctx.assign_stable_ids(*m);
        Operation* const ops[7] = {s1, e1, s2, e2, as, fl, bd};
        for (u32 i = 0; i < 7U; ++i)
        {
            cell.push_back(ops[i]);
            value.push_back(0);
            recomputes.push_back(0U);
            deps.push_back(Array<u64>(alloc));
            out.push_back(ops[i]);
        }
        for (u32 i = 0; i < 7U; ++i)
        {
            const u64 id = cell[i]->stable_id().value;
            dag.add_node(id);
            for (u32 k = 0; k < cell[i]->num_operands(); ++k)
            {
                const Operation* const dop = cell[i]->operand(k)->defining_op();
                if (dop == nullptr) { continue; }
                dag.add_edge(id, dop->stable_id().value);
                deps[i].push_back(dop->stable_id().value);
            }
        }
    }
    void full_eval()
    {
        Array<u64> order(alloc);
        REQUIRE(dag.topo_order(order));
        for (usize i = 0; i < order.size(); ++i)
        {
            const u64 id   = order[i];
            const i64 v    = eval(*cell[id - 1U]);
            value[id - 1U] = v;
            recomputes[id - 1U] += 1U;
            dag.set_revision(id, content_hash(*cell[id - 1U], ctx), value_hash(v));
        }
    }
    // The Blender/CAD depsgraph re-eval seeded by a MULTI-op touched-set. TAG = the UNION over seeds of {seed} +
    // affected_by(seed). EVAL walk: a tagged node re-evaluates iff (it is a SEED whose FORMULA changed — checked for
    // EVERY seed, so a no-op seed contributes nothing) OR (an upstream input's value changed); else it early-outs.
    void depsgraph_reeval(ConstSpan<StableId> seeds, Array<u64>& tag, Array<u64>& evaluated)
    {
        tag.clear();
        evaluated.clear();
        Array<u8> is_seed(alloc);
        Array<u8> tagged(alloc);
        Array<u8> changed(alloc);
        for (u32 i = 0; i < 7U; ++i)
        {
            is_seed.push_back(0U);
            tagged.push_back(0U);
            changed.push_back(0U);
        }
        for (usize i = 0; i < seeds.size(); ++i)
        {
            is_seed[seeds[i].value - 1U] = 1U;
            tagged[seeds[i].value - 1U]  = 1U;
            Array<u64> aff(alloc);
            REQUIRE(dag.affected_by(seeds[i].value, aff));
            for (usize j = 0; j < aff.size(); ++j) { tagged[aff[j] - 1U] = 1U; }
        }
        for (u32 id = 1; id <= 7U; ++id)
        {
            if (tagged[id - 1U] != 0U) { tag.push_back(id); }
        }
        Array<u64> order(alloc);
        REQUIRE(dag.topo_order(order));
        for (usize i = 0; i < order.size(); ++i)
        {
            const u64 id = order[i];
            if (tagged[id - 1U] == 0U) { continue; }
            const u64 new_content = content_hash(*cell[id - 1U], ctx);
            bool      must        = false;
            if (is_seed[id - 1U] != 0U && new_content != dag.content_of(id)) { must = true; } // EVERY seed checked (no-op skips)
            for (usize k = 0; k < deps[id - 1U].size(); ++k)
            {
                if (changed[deps[id - 1U][k] - 1U] != 0U) { must = true; }
            }
            if (!must) { continue; }
            const u64 old_interface = dag.interface_of(id);
            const i64 v             = eval(*cell[id - 1U]);
            value[id - 1U]          = v;
            recomputes[id - 1U] += 1U;
            const u64 new_interface = value_hash(v);
            dag.set_revision(id, new_content, new_interface);
            evaluated.push_back(id);
            if (new_interface != old_interface) { changed[id - 1U] = 1U; }
        }
    }
    // Apply `edits` atomically in ONE transaction and COMMIT; return the touched stable-ids (⛔ order not guaranteed).
    [[nodiscard]] Array<StableId> commit_edits(ConstSpan<Edit> edits)
    {
        DiagnosticEngine diag(ctx, alloc);
        Transaction      tx(ctx, *m, diag, alloc);
        for (usize i = 0; i < edits.size(); ++i) { REQUIRE(tx.set_attr(edits[i].op, edits[i].param, ctx.attr_int(edits[i].val))); }
        REQUIRE(tx.commit());
        Array<StableId> t(alloc);
        for (usize i = 0; i < tx.touched().size(); ++i) { t.push_back(tx.touched()[i]); }
        return t;
    }
};
} // namespace

TEST_CASE("ceir 9d: a single dimension edit re-evaluates only its dependent subtree", "[ceir][cad]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    CadGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const fillet = c[5];

    // editing the fillet radius re-evaluates {fillet, body} only (the upstream features cache-hit) -- the 9c partial
    // re-eval, cited not re-proven.
    const Edit            edits[1] = {Edit{fillet, StringView("radius"), 9}};
    const Array<StableId> touched  = g.commit_edits(ConstSpan<Edit>(edits, 1U));
    Array<u64>            tag(&root);
    Array<u64>            evaluated(&root);
    g.depsgraph_reeval(ConstSpan<StableId>(touched.data(), touched.size()), tag, evaluated);
    CHECK(g.recomputes[5] == 2U); // fillet
    CHECK(g.recomputes[6] == 2U); // body
    for (u32 i = 0; i < 5U; ++i) { CHECK(g.recomputes[i] == 1U); } // sketch1/extrude1/sketch2/extrude2/assembly cache-hit
}

TEST_CASE("ceir 9d: multi-op edits in one transaction seed the eval from the whole touched-set", "[ceir][cad]")
{
    crd::memory::GrowableTlsfAllocator root;
    // MULTI-OP: one transaction edits BOTH sketches -> both feature branches re-evaluate.
    {
        Context           ctx(&root);
        CadGraph          g(ctx, &root);
        Array<Operation*> c(&root);
        g.build(c);
        g.full_eval();
        const Edit            edits[2] = {Edit{c[0], StringView("width"), 8}, Edit{c[2], StringView("width"), 9}};
        const Array<StableId> touched  = g.commit_edits(ConstSpan<Edit>(edits, 2U));
        CHECK(touched.size() == 2U); // the tx reports BOTH edited sketches (order not asserted)
        Array<u64> tag(&root);
        Array<u64> evaluated(&root);
        g.depsgraph_reeval(ConstSpan<StableId>(touched.data(), touched.size()), tag, evaluated);
        CHECK(g.recomputes[1] == 2U); // extrude1 (sketch1 branch)
        CHECK(g.recomputes[3] == 2U); // ⛔ extrude2 (sketch2 branch) ALSO re-evals — the union of both subtrees
        CHECK(g.recomputes[4] == 2U); // assembly
        CHECK(g.recomputes[6] == 2U); // body
    }
    // CONTROL (a FRESH graph): editing ONLY sketch1 leaves the sketch2 branch cached.
    {
        Context           ctx(&root);
        CadGraph          g(ctx, &root);
        Array<Operation*> c(&root);
        g.build(c);
        g.full_eval();
        const Edit            edits[1] = {Edit{c[0], StringView("width"), 8}};
        const Array<StableId> touched  = g.commit_edits(ConstSpan<Edit>(edits, 1U));
        Array<u64> tag(&root);
        Array<u64> evaluated(&root);
        g.depsgraph_reeval(ConstSpan<StableId>(touched.data(), touched.size()), tag, evaluated);
        CHECK(g.recomputes[1] == 2U); // extrude1 re-evals
        CHECK(g.recomputes[3] == 1U); // ⛔ extrude2 CACHE-HIT (sketch2 was NOT edited) -- the control
    }
}

TEST_CASE("ceir 9d: a no-op edit inside a multi-op transaction recomputes only the real edit subtree", "[ceir][cad]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    CadGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    // one transaction: a REAL edit to sketch1.width + a NO-OP edit to sketch2.width (its current value 3). Both are in
    // the touched-set, but the content-addressed memo recomputes ONLY sketch1's subtree (the sketch2 branch is cached).
    const Edit            edits[2] = {Edit{c[0], StringView("width"), 8}, Edit{c[2], StringView("width"), 3}};
    const Array<StableId> touched  = g.commit_edits(ConstSpan<Edit>(edits, 2U));
    CHECK(touched.size() == 2U);
    Array<u64> tag(&root);
    Array<u64> evaluated(&root);
    g.depsgraph_reeval(ConstSpan<StableId>(touched.data(), touched.size()), tag, evaluated);
    CHECK(g.recomputes[0] == 2U); // sketch1 (real edit)
    CHECK(g.recomputes[1] == 2U); // extrude1
    CHECK(g.recomputes[2] == 1U); // ⛔ sketch2 CACHE-HIT (no-op edit, content unchanged)
    CHECK(g.recomputes[3] == 1U); // extrude2 cache-hit
    CHECK(g.recomputes[4] == 2U); // assembly re-evals (extrude1 changed)
    CHECK(g.recomputes[6] == 2U); // body
}

TEST_CASE("ceir 9d: a transactional edit rolls back the feature graph byte-identically", "[ceir][cad]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    CadGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const s1 = c[0];
    Operation* const e1 = c[1];

    const ByteArray before = serialize(ctx, *g.m, &root);
    {
        DiagnosticEngine diag(ctx, &root);
        Transaction      tx(ctx, *g.m, diag, &root);
        REQUIRE(tx.set_attr(s1, "width", ctx.attr_int(99)));
        REQUIRE(tx.set_attr(e1, "depth", ctx.attr_int(7)));
        tx.rollback(); // the CAD "cancel" -- discard the whole multi-op edit
    }
    const ByteArray after = serialize(ctx, *g.m, &root);
    CHECK(blob_eq(before, after)); // the feature graph is byte-identical
    CHECK(s1->attr("width") == ctx.attr_int(4));  // the dimensions read back as the originals (the 8i attr-restore)
    CHECK(e1->attr("depth") == ctx.attr_int(2));
}

TEST_CASE("ceir 9d: a constraint-violating commit is rejected and rolls back; a satisfying commit succeeds", "[ceir][cad]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    CadGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const s1 = c[0]; // sketch1: width==height==4
    ctx.set_attr(s1, "constraint", ctx.attr_int(1)); // ACTIVATE the equal-dimensions constraint (setup, pre-transaction)

    const ByteArray before = serialize(ctx, *g.m, &root);
    // VIOLATE: set width to 5 (height stays 4) -> the sketch verifier fails at commit -> the transaction rolls back.
    {
        DiagnosticEngine diag(ctx, &root);
        Transaction      tx(ctx, *g.m, diag, &root);
        REQUIRE(tx.set_attr(s1, "width", ctx.attr_int(5)));
        CHECK_FALSE(tx.commit()); // the CONSTRAINT (the sketch verifier) rejects via the tx commit-verify
        bool verify_failed = false;
        for (usize i = 0; i < diag.count(); ++i)
        {
            if (diag.at(i).code == make_diagnostic_code("ceir.transaction.verify_failed")) { verify_failed = true; }
        }
        CHECK(verify_failed);
    }
    const ByteArray after = serialize(ctx, *g.m, &root);
    CHECK(blob_eq(before, after));               // byte-identical after the rejected commit
    CHECK(s1->attr("width") == ctx.attr_int(4));  // width restored to the constraint-satisfying original

    // SATISFY: set width AND height both to 6 (keeps width==height) -> the commit succeeds.
    DiagnosticEngine diag2(ctx, &root);
    Transaction      tx2(ctx, *g.m, diag2, &root);
    REQUIRE(tx2.set_attr(s1, "width", ctx.attr_int(6)));
    REQUIRE(tx2.set_attr(s1, "height", ctx.attr_int(6)));
    CHECK(tx2.commit());
}
