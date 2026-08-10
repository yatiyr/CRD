// CEIR-9g (UNIVERSALITY VALIDATION, U7 Agent transaction, U-§94/U-§50–53/U-§117). ⛔ A PROOF, not a feature, and the
// CULMINATION of band 9: the FULL agent loop runs on the foundation with ZERO new machinery. A mock agent DISCOVERS ops
// via the machine-readable `OpSchema` registry (the surface built for "CLI/MCP/agent discovery" — `.ops.json` is its
// OUT-of-process serialization; the in-process `<dialect>_op_schemas()` tables are the same registry, no parser, and no
// no-std JSON reader exists) — ⛔ with ZERO hard-coded op knowledge (it SELECTS by property: arity + result count, and
// interns via the discovered `schema.dialect`/`name`). It then opens an 8i TRANSACTION, authors + modifies a program,
// runs the verifiers at TWO granularities (the 9e op-local commit-verify BACKSTOP + a module-wide `find_structure_error`
// SWEEP mid-tx), reads structured 8g DIAGNOSTICS by CODE, and commits or rolls back byte-identically. ⛔ The generated
// verifiers are STRUCTURAL (arity + required attrs); semantic TYPE validity is a further verification layer
// (named-forward — CEIR-3/4 / a type-constraint language). Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>          // umbrella: context/ir/transaction/diagnostic/binary
#include <crd/ceir/binary.hpp>        // serialize
#include <crd/ceir/gen/arith_ops.hpp> // register_arith_ops + arith_op_schemas (the real generated registry)
#include <crd/ceir/op_schema.hpp>     // OpSchema

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u8;
using crd::usize;
using ByteArray = Array<u8>;

namespace
{
// The agent's own diagnostic code for a module-wide sweep failure (8g stable code, read by CODE not string).
[[nodiscard]] DiagnosticCode sweep_defect_code() noexcept { return make_diagnostic_code("agent.sweep.structure_defect"); }

// ── the mock AGENT's discovery over the OpSchema registry (SELECTS by property, never by a hard-coded name) ──
// The first schema with the given operand + result arity; `pure_only` skips ops with required attributes (a "plain"
// producer/binary). nullptr if none.
[[nodiscard]] const OpSchema* discover(ConstSpan<OpSchema> schemas, usize nops, usize nres, bool pure_only) noexcept
{
    for (usize i = 0; i < schemas.size(); ++i)
    {
        const OpSchema& s = schemas[i];
        if (s.operands.size() != nops || s.results.size() != nres) { continue; }
        if (pure_only)
        {
            bool has_required = false;
            for (usize k = 0; k < s.attributes.size(); ++k)
            {
                if (s.attributes[k].required) { has_required = true; }
            }
            if (has_required) { continue; }
        }
        return &s;
    }
    return nullptr;
}
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
Module* single_block(Context& ctx, Block*& top)
{
    Module* const m = ctx.create_module();
    top             = ctx.create_block(0U);
    m->body()->append(top);
    return m;
}
} // namespace

TEST_CASE("ceir 9g: an agent discovers ops by property, authors and modifies a program, and commits", "[ceir][agent]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    Block*        top = nullptr;
    Module* const m   = single_block(ctx, top);

    // DISCOVER (zero hard-coded op knowledge): a source = 0 operands + 1 result; a binary = 2 operands + 1 result, no
    // required attrs. The agent knows NOTHING about "arith.const"/"addi" at compile time — it selects by property.
    const ConstSpan<OpSchema> schemas = arith::arith_op_schemas();
    const OpSchema* const     src     = discover(schemas, 0U, 1U, false);
    const OpSchema* const     bin     = discover(schemas, 2U, 1U, true);
    REQUIRE(src != nullptr);
    REQUIRE(bin != nullptr);
    const OpId src_kind = ctx.intern_op(src->dialect, src->name); // interned from the DISCOVERED name
    const OpId bin_kind = ctx.intern_op(bin->dialect, bin->name);

    DiagnosticEngine diag(ctx, &root);
    Operation*       s1 = nullptr;
    {
        Transaction tx(ctx, *m, diag, &root);
        s1                = tx.insert(src_kind, {}, 1U, top, nullptr, ctx.type_i32());
        Operation* const s2 = tx.insert(src_kind, {}, 1U, top, nullptr, ctx.type_i32());
        REQUIRE(s1 != nullptr);
        REQUIRE(s2 != nullptr);
        // set every REQUIRED int attr the discovered schema declares (the agent reads the AttrInfo contract).
        for (usize k = 0; k < src->attributes.size(); ++k)
        {
            if (src->attributes[k].required && src->attributes[k].kind == AttrKind::Int)
            {
                REQUIRE(tx.set_attr(s1, src->attributes[k].name, ctx.attr_int(3)));
                REQUIRE(tx.set_attr(s2, src->attributes[k].name, ctx.attr_int(4)));
            }
        }
        Value* const     ops[2] = {s1->result(0), s2->result(0)};
        Operation* const sum    = tx.insert(bin_kind, ConstSpan<Value*>(ops, 2U), 1U, top);
        REQUIRE(sum != nullptr);
        // the agent's PRE-COMMIT module-wide SWEEP (find_structure_error) -> clean -> commit.
        REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
        REQUIRE(tx.commit());
    }
    // the loop closed: the authored op's kind resolves to the DISCOVERED schema's qualified name (no hard-coded literal).
    CHECK(ctx.op_name(s1->kind()) == src->qualified);

    // MODIFY: a second transaction edits the discovered source's required attr; the sweep stays clean; commit.
    if (!src->attributes.empty() && src->attributes[0].required && src->attributes[0].kind == AttrKind::Int)
    {
        Transaction tx2(ctx, *m, diag, &root);
        REQUIRE(tx2.set_attr(s1, src->attributes[0].name, ctx.attr_int(30)));
        REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
        REQUIRE(tx2.commit());
        CHECK(s1->attr(src->attributes[0].name) == ctx.attr_int(30));
    }
}

TEST_CASE("ceir 9g: the agent module-wide sweep catches a defect and rolls back byte-identically", "[ceir][agent]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    Block*        top = nullptr;
    Module* const m   = single_block(ctx, top);

    const ConstSpan<OpSchema> schemas = arith::arith_op_schemas();
    const OpSchema* const     src     = discover(schemas, 0U, 1U, false);
    const OpSchema* const     bin     = discover(schemas, 2U, 1U, true);
    REQUIRE(src != nullptr);
    REQUIRE(bin != nullptr);
    const OpId src_kind = ctx.intern_op(src->dialect, src->name);
    const OpId bin_kind = ctx.intern_op(bin->dialect, bin->name);

    const ByteArray before = serialize(ctx, *m, &root);
    DiagnosticEngine diag(ctx, &root);
    {
        Transaction tx(ctx, *m, diag, &root);
        Operation* const s1 = tx.insert(src_kind, {}, 1U, top, nullptr, ctx.type_i32());
        Operation* const s2 = tx.insert(src_kind, {}, 1U, top, nullptr, ctx.type_i32());
        REQUIRE(s1 != nullptr);
        REQUIRE(s2 != nullptr);
        // ⛔ author a SAME-BLOCK FORWARD REFERENCE: insert the binary BEFORE s2 (via the anchor) while referencing s2's
        // result -> a module-wide structural defect NO op-local commit-verify can see (only the sweep catches it).
        Value* const     ops[2] = {s1->result(0), s2->result(0)};
        Operation* const sum    = tx.insert(bin_kind, ConstSpan<Value*>(ops, 2U), 1U, top, /*before=*/s2);
        REQUIRE(sum != nullptr);

        const StructureError e = ctx.find_structure_error(*m); // the agent's pre-commit sweep
        // ⛔ a same-block forward reference in a GRAPH region is a FEEDBACK edge (§20), not UseBeforeDef — so the sweep
        // reports FeedbackWithoutState (a combinational cycle not through a StateEdge op). Either way it is a module-wide
        // defect NO op-local commit-verify can see.
        CHECK(e.kind == StructureErrorKind::FeedbackWithoutState);
        // the agent emits its OWN structured diagnostic (by CODE), carrying the offending op's provenance, then rolls back.
        diag.emit(Severity::Error, sweep_defect_code(), StringView("agent.sweep.structure_defect"),
                  e.op != nullptr ? e.op->loc() : SourceLoc{}, StringView("authored module failed the structure sweep"));
        tx.rollback();
    }
    // the agent read its decision by CODE, and the rollback left the feature graph byte-identical (nothing authored).
    bool swept = false;
    for (usize i = 0; i < diag.count(); ++i)
    {
        if (diag.at(i).code == sweep_defect_code()) { swept = true; }
    }
    CHECK(swept);
    CHECK(blob_eq(before, serialize(ctx, *m, &root)));
    CHECK(top->empty()); // nothing was committed
}

TEST_CASE("ceir 9g: the op-local commit-verify backstop catches a lazy agent that skips the sweep", "[ceir][agent]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    Block*        top = nullptr;
    Module* const m   = single_block(ctx, top);

    const ConstSpan<OpSchema> schemas = arith::arith_op_schemas();
    const OpSchema* const     src     = discover(schemas, 0U, 1U, false);
    REQUIRE(src != nullptr);
    REQUIRE_FALSE(src->attributes.empty()); // the discovered source declares a REQUIRED attr (arity alone is insufficient)
    const OpId src_kind = ctx.intern_op(src->dialect, src->name);

    const ByteArray before = serialize(ctx, *m, &root);
    DiagnosticEngine diag(ctx, &root);
    {
        // a LAZY agent discovers only ARITY and skips both the schema's required-attr contract AND the module-wide sweep:
        // it authors the source WITHOUT its required attr and commits directly. The op-LOCAL commit-verify BACKSTOP
        // (the discovered op's registered verifier) rejects -> commit returns false -> the transaction auto-rolls-back.
        Transaction tx(ctx, *m, diag, &root);
        REQUIRE(tx.insert(src_kind, {}, 1U, top, nullptr, ctx.type_i32()) != nullptr); // no required attr set
        CHECK_FALSE(tx.commit());
    }
    bool verify_failed = false;
    for (usize i = 0; i < diag.count(); ++i)
    {
        if (diag.at(i).code == make_diagnostic_code("ceir.transaction.verify_failed")) { verify_failed = true; }
    }
    CHECK(verify_failed); // the agent reads the op-local backstop's diagnostic by CODE
    CHECK(blob_eq(before, serialize(ctx, *m, &root)));
    CHECK(top->empty());
}
