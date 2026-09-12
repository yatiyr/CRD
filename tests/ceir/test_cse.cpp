// CEIR-26c — the Common-Subexpression-Elimination gate (device-free). Proves cse.hpp on a synthetic `cs.*` dialect (the
// planner/device legs are 26c-2): (1) a duplicate eligible op is eliminated and its consumers REWIRED to the surviving twin;
// (2) THREE identical ops collapse to the FIRST in one pass — a third dup finds the survivor, never the just-eliminated
// tombstone (correction #1 / the monotone-chain scar in CSE form); (3) a CASCADE — eliminating a dup makes two consumers
// identical, and they collapse too (the RAUW propagates before the later consumer is scanned); (4) STRUCTURAL negatives — ops
// differing by operand (POINTER, not deep equality), attr, or result type are KEPT; (5) ELIGIBILITY negatives — an effectful
// (non-Pure) twin and a Pure-but-POSITIVELY-Nondeterministic twin are KEPT (the §27 determinism gate). Host-only. ASCII names.
//
// The `cs.*` dialect is REGISTERED (CSE reads op_info + Pure + §27): src/mul/add are Pure candidates; sink/eff are NOT Pure
// (effectful — kept, and sink keeps a candidate's result live so the RAUW is observable); nd is Pure but Nondeterministic (the
// determinism disqualifier — no real op-kind carries it yet, so a synthetic one gates the clause). Match-free: CSE checks
// traits/§27, not op ids, but the ids are pinned to intern_op so the registration and the structural compares agree.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gen/resource_ops.hpp> // CEIR-26c: resource.declare is an external SOURCE (Allocate effect, NOT Pure) -> not CSE'd
#include <crd/ceir/passes/cse.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::u32;
using crd::usize;

namespace
{
constexpr OpId kSrc{fnv1a_ct("cs.src")};
constexpr OpId kMul{fnv1a_ct("cs.mul")};
constexpr OpId kAdd{fnv1a_ct("cs.add")};
constexpr OpId kSink{fnv1a_ct("cs.sink")};
constexpr OpId kEff{fnv1a_ct("cs.eff")};
constexpr OpId kNd{fnv1a_ct("cs.nd")};

// Register the cs dialect with the traits/§27 classes CSE reads (Pure candidates, effectful barriers, a nondeterministic op).
void register_cs(Context& ctx)
{
    Dialect* const d = ctx.register_dialect("cs");
    d->register_op("src", OpSpec{.traits = flags_of(OpTrait::Pure)});
    d->register_op("mul", OpSpec{.traits = flags_of(OpTrait::Pure)});
    d->register_op("add", OpSpec{.traits = flags_of(OpTrait::Pure)});
    d->register_op("sink", OpSpec{});                                          // NOT Pure — effectful, keeps operands live
    d->register_op("eff", OpSpec{});                                           // NOT Pure — an effectful CSE-shaped op
    d->register_op("nd", OpSpec{.traits = flags_of(OpTrait::Pure), .determinism = DeterminismClass::Nondeterministic});
    REQUIRE(ctx.intern_op("cs", "src") == kSrc);  // pin the capture-free ids to intern_op
    REQUIRE(ctx.intern_op("cs", "mul") == kMul);
    REQUIRE(ctx.intern_op("cs", "add") == kAdd);
    REQUIRE(ctx.intern_op("cs", "sink") == kSink);
    REQUIRE(ctx.intern_op("cs", "eff") == kEff);
    REQUIRE(ctx.intern_op("cs", "nd") == kNd);
}

// A 1-operand op of `kind`, result type `t`, appended to `block`, with an optional single int attr (name `attr_name`, value
// `v`). Distinct attr names/values keep otherwise-identical ops apart so a negative test isolates ONE difference axis.
Operation* mk(Context& ctx, Block* block, OpId kind, Value* in, TypeId t, const char* attr_name = nullptr, crd::i64 v = 0)
{
    Value* const     ops[1] = {in};
    Operation* const op     = ctx.create_operation(kind, ConstSpan<Value*>(ops, 1U), 1U, t);
    if (attr_name != nullptr) { ctx.set_attr(op, StringView(attr_name), ctx.attr_int(v)); }
    block->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 26c: CSE eliminates a duplicate op and rewires its consumers to the survivor", "[ceir][cse]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    register_cs(ctx);
    const TypeId i32 = ctx.type_i32();

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    // %x = cs.src ; %m1 = cs.mul(%x){k=7} ; %m2 = cs.mul(%x){k=7} ; %s1 = cs.sink(%m1) ; %s2 = cs.sink(%m2)
    Operation* const x  = ctx.create_operation(kSrc, {}, 1U, i32);
    block->append(x);
    Operation* const m1 = mk(ctx, block, kMul, x->result(0), i32, "k", 7);
    Operation* const m2 = mk(ctx, block, kMul, x->result(0), i32, "k", 7); // structurally identical to m1
    Operation* const s1 = mk(ctx, block, kSink, m1->result(0), i32);
    Operation* const s2 = mk(ctx, block, kSink, m2->result(0), i32);
    REQUIRE(block->num_ops() == 5U);

    DiagnosticEngine diag(ctx, &root);
    CHECK(cse_run(ctx, *m, diag));
    CHECK(m2->is_erased());          // the later twin dies ...
    CHECK_FALSE(m1->is_erased());    // ... the earlier one survives (first-in-block dominates)
    CHECK(s1->operand(0) == m1->result(0));
    CHECK(s2->operand(0) == m1->result(0)); // ⛔ s2 was rewired off %m2 onto the SURVIVOR %m1 (the RAUW)
    CHECK(block->num_ops() == 4U);
    CHECK_FALSE(cse_run(ctx, *m, diag)); // fixpoint — nothing left to eliminate
}

TEST_CASE("ceir 26c: three identical ops collapse to the FIRST in one pass (no tombstone chain)", "[ceir][cse]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    register_cs(ctx);
    const TypeId i32 = ctx.type_i32();

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    Operation* const x  = ctx.create_operation(kSrc, {}, 1U, i32);
    block->append(x);
    Operation* const m1 = mk(ctx, block, kMul, x->result(0), i32); // three identical twins
    Operation* const m2 = mk(ctx, block, kMul, x->result(0), i32);
    Operation* const m3 = mk(ctx, block, kMul, x->result(0), i32);
    Operation* const s1 = mk(ctx, block, kSink, m1->result(0), i32);
    Operation* const s2 = mk(ctx, block, kSink, m2->result(0), i32);
    Operation* const s3 = mk(ctx, block, kSink, m3->result(0), i32);

    DiagnosticEngine diag(ctx, &root);
    CHECK(cse_run(ctx, *m, diag));
    CHECK_FALSE(m1->is_erased());
    CHECK(m2->is_erased());
    CHECK(m3->is_erased());
    // ⛔ the load-bearing assertion: s3 reads the SURVIVOR %m1, NOT the tombstoned %m2. If a RAUW'd dup were added to `seen`,
    //    %m3 would match %m2 and s3 would point at an erased op's result — this catches the tombstone-chain regression.
    CHECK(s1->operand(0) == m1->result(0));
    CHECK(s2->operand(0) == m1->result(0));
    CHECK(s3->operand(0) == m1->result(0));
}

TEST_CASE("ceir 26c: CSE cascades - a survivor's consumers become identical and collapse too", "[ceir][cse]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    register_cs(ctx);
    const TypeId i32 = ctx.type_i32();

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    // %x = cs.src ; %a1,%a2 = cs.mul(%x) [twins] ; %b1 = cs.add(%a1), %b2 = cs.add(%a2) [twins ONCE a2->a1] ; sinks keep them live.
    Operation* const x  = ctx.create_operation(kSrc, {}, 1U, i32);
    block->append(x);
    Operation* const a1 = mk(ctx, block, kMul, x->result(0), i32);
    Operation* const a2 = mk(ctx, block, kMul, x->result(0), i32);
    Operation* const b1 = mk(ctx, block, kAdd, a1->result(0), i32);
    Operation* const b2 = mk(ctx, block, kAdd, a2->result(0), i32);
    Operation* const s1 = mk(ctx, block, kSink, b1->result(0), i32);
    Operation* const s2 = mk(ctx, block, kSink, b2->result(0), i32);
    REQUIRE(block->num_ops() == 7U);

    DiagnosticEngine diag(ctx, &root);
    CHECK(cse_run(ctx, *m, diag));
    CHECK(a2->is_erased()); // the mul twin
    CHECK(b2->is_erased()); // ⛔ the CASCADE: b2 (was add(%a2)) only equals b1 (add(%a1)) AFTER a2->a1 rewired b2's operand
    CHECK_FALSE(a1->is_erased());
    CHECK_FALSE(b1->is_erased());
    CHECK(b1->operand(0) == a1->result(0)); // the surviving add reads the surviving mul
    CHECK(s1->operand(0) == b1->result(0));
    CHECK(s2->operand(0) == b1->result(0)); // s2 rewired through the whole cascade onto b1
    CHECK(block->num_ops() == 5U);          // x, a1, b1, s1, s2
    CHECK_FALSE(cse_run(ctx, *m, diag));    // fixpoint
}

TEST_CASE("ceir 26c: CSE keeps ops that differ by operand, attr, or result type", "[ceir][cse]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    register_cs(ctx);
    const TypeId i32 = ctx.type_i32();
    const TypeId f32 = ctx.type_f32();

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    // two DISTINCT sources (different result types ⇒ not twins themselves), so muls over them differ by OPERAND pointer.
    Operation* const x = ctx.create_operation(kSrc, {}, 1U, i32);
    block->append(x);
    Operation* const y = ctx.create_operation(kSrc, {}, 1U, f32);
    block->append(y);
    // Each pair carries a UNIQUE tag so the pairs never collide with each other — only the intended axis (operand/attr/type)
    // differs WITHIN a pair. ⛔ tag=10 pins the operand pair; deep operand equality would fuse mul(%x) with mul(%y) — POINTER
    // equality keeps them apart.
    Operation* const by_operand_a = mk(ctx, block, kMul, x->result(0), i32, "tag", 10); // mul(%x){tag=10}
    Operation* const by_operand_b = mk(ctx, block, kMul, y->result(0), i32, "tag", 10); // mul(%y){tag=10} — operand differs
    Operation* const by_attr_a    = mk(ctx, block, kMul, x->result(0), i32, "k", 1);    // mul(%x){k=1}
    Operation* const by_attr_b    = mk(ctx, block, kMul, x->result(0), i32, "k", 2);    // mul(%x){k=2} — attr value differs
    Operation* const by_type_a    = mk(ctx, block, kMul, x->result(0), i32, "typ", 20); // mul(%x){typ=20}:i32
    Operation* const by_type_b    = mk(ctx, block, kMul, x->result(0), f32, "typ", 20); // mul(%x){typ=20}:f32 — type differs
    const usize      n_before     = block->num_ops();

    DiagnosticEngine diag(ctx, &root);
    CHECK_FALSE(cse_run(ctx, *m, diag)); // nothing is a twin ⇒ no change
    CHECK(block->num_ops() == n_before);
    CHECK_FALSE(by_operand_a->is_erased());
    CHECK_FALSE(by_operand_b->is_erased());
    CHECK_FALSE(by_attr_a->is_erased());
    CHECK_FALSE(by_attr_b->is_erased());
    CHECK_FALSE(by_type_a->is_erased());
    CHECK_FALSE(by_type_b->is_erased());
}

// ⛔ 26c-2b + 26z REGRESSION (the device leg found the declare half; 26z closed the import half): two structurally-identical
// resource EXTERNAL SOURCES are DISTINCT values (each names/binds its own resource, seeded independently), NOT one — so BOTH
// `resource.declare` (graph-owned) AND `resource.import` (externally-owned) carry an ALLOCATE effect (NOT Pure) and CSE must KEEP
// every one. A Pure declare was silently merged, collapsing inputs A/B/C into ONE buffer (a gemm computed A@A) — 26c fixed declare.
// ⛔ 26z: import was STILL Pure, so two ANONYMOUS imports (no `name` ⇒ 0 attrs, structurally identical) were CSE-equal ⇒ MERGED ⇒
// two distinct externals collapsed (the declare-A@A shape, on the import path — the scar's "anon import flagged NOT fixed"). Import
// is now Allocate too: named AND anonymous imports are all KEPT. ⛔ the SAME-NAMED merge that 26c asserted (i2 erased) was a Pure-
// ARTIFACT, never a specified optimization — CSE RAUWs the result VALUE (not the name), which a Value-keyed provider binding would
// notice; a named import is referentially-transparent by name but keeping both is sound (redundant bind, not a miscompile). The
// CSE-WORKS positive control is the file's FIRST test (the cs.* Pure ops DO merge) — so this all-kept module is not a vacuous no-op.
TEST_CASE("ceir 26c/26z: CSE keeps every resource external source -- declares AND imports (named or anonymous) are Allocate, never merged", "[ceir][cse]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    (void)resource::register_resource_ops(ctx);
    const OpId   decl = ctx.intern_op("resource", "declare");
    const OpId   imp  = ctx.intern_op("resource", "import");
    const TypeId rt   = ctx.type_i32(); // any consistent type — CSE compares result TYPES, not resource-kindedness

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    // two identical declares — external SOURCES, must NOT merge (each a distinct graph-owned buffer).
    Operation* const d1 = ctx.create_operation(decl, {}, 1U, rt);
    block->append(d1);
    Operation* const d2 = ctx.create_operation(decl, {}, 1U, rt);
    block->append(d2);
    // two identical SAME-NAMED imports — 26c asserted these MERGED (import was Pure); 26z: import is Allocate ⇒ KEPT (the merge was
    // a Pure-artifact, not a specified optimization — see the comment above).
    Operation* const i1 = ctx.create_operation(imp, {}, 1U, rt);
    ctx.set_attr(i1, StringView("name"), ctx.attr_string(StringView("ext")));
    block->append(i1);
    Operation* const i2 = ctx.create_operation(imp, {}, 1U, rt);
    ctx.set_attr(i2, StringView("name"), ctx.attr_string(StringView("ext")));
    block->append(i2);
    // ⛔ 26z THE FIX: two ANONYMOUS imports (no `name` ⇒ 0 attrs, structurally identical) — DISTINCT external sources with NO shared
    // identity, so they must NOT merge. A Pure import would collapse them into one (the declare-A@A shape on the import path).
    Operation* const a1 = ctx.create_operation(imp, {}, 1U, rt);
    block->append(a1);
    Operation* const a2 = ctx.create_operation(imp, {}, 1U, rt);
    block->append(a2);

    DiagnosticEngine diag(ctx, &root);
    CHECK_FALSE(cse_run(ctx, *m, diag)); // every op is an external source (Allocate ⇒ non-candidate) ⇒ CSE finds nothing to merge
    CHECK_FALSE(d1->is_erased());        // both declares KEPT (26c)
    CHECK_FALSE(d2->is_erased());
    CHECK_FALSE(i1->is_erased());        // both SAME-NAMED imports KEPT (26z — import is now Allocate, the 26c Pure-artifact merge retired)
    CHECK_FALSE(i2->is_erased());
    CHECK_FALSE(a1->is_erased());        // ⛔ both ANONYMOUS imports KEPT — THE 26z FIX (a Pure import would have merged these into one)
    CHECK_FALSE(a2->is_erased());
}

TEST_CASE("ceir 26c: CSE keeps effectful and positively-nondeterministic twins", "[ceir][cse]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    register_cs(ctx);
    const TypeId i32 = ctx.type_i32();

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    Operation* const x = ctx.create_operation(kSrc, {}, 1U, i32);
    block->append(x);
    // Two identical EFFECTFUL twins — CSE'ing them would drop a side effect (the effect-narrowing scar in CSE form) ⇒ KEEP both.
    Operation* const e1 = mk(ctx, block, kEff, x->result(0), i32);
    Operation* const e2 = mk(ctx, block, kEff, x->result(0), i32);
    // Two identical Pure-but-NONDETERMINISTIC twins — same inputs need not give the same bits ⇒ KEEP both (§27 disqualifier).
    Operation* const n1 = mk(ctx, block, kNd, x->result(0), i32);
    Operation* const n2 = mk(ctx, block, kNd, x->result(0), i32);
    const usize      n_before = block->num_ops();

    DiagnosticEngine diag(ctx, &root);
    CHECK_FALSE(cse_run(ctx, *m, diag)); // neither pair is CSE-eligible ⇒ no change
    CHECK(block->num_ops() == n_before);
    CHECK_FALSE(e1->is_erased());
    CHECK_FALSE(e2->is_erased());
    CHECK_FALSE(n1->is_erased());
    CHECK_FALSE(n2->is_erased());
}
