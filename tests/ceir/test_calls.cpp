// CEIR-5c: calls + recursion policy (§14/§34). Two features under test: (1) the EffectsFn hook — a func.call's effective
// effects are its CALLEE's, resolved through the SymbolTable and unioned transitively (with a recursion cycle guard),
// degrading to an ExternalCall barrier on any unresolved/unregistered reach; its two consumers (hazards + domain
// legality, the 4c "ExternalCall stays legal-for-now" gap now closed); (2) the recursion-policy verifier over the
// resolved call graph (declared promises only).

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/hazard.hpp>
#include <crd/ceir/semantics.hpp>
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;        // NOLINT(google-build-using-namespace)
using namespace crd::ceir;  // NOLINT(google-build-using-namespace)
namespace fn = crd::ceir::func;
using containers::ConstSpan;

namespace
{
// A fixture dialect set: func (registered — the hook + resolve_call), `mem.write` (a MemoryWrite effect), `io.read` (a
// FileIO effect), `nb.box` (an effect-free 1-region container to test nested-region traversal), and an UNREGISTERED
// opaque kind (interned only — maximally effectful).
struct Ops
{
    OpId memw;
    OpId ioread;
    OpId box;
    OpId opaque;
    OpId src;      // an effect-free op that PRODUCES one Value (a resource handle)
    OpId writeref; // a MemoryWrite targeting OPERAND 0 (a per-Value effect — precision the table path must preserve)
    explicit Ops(Context& ctx)
    {
        fn::register_dialect(ctx);
        Dialect* const d = ctx.register_dialect("mem");
        memw             = d->register_op("write", {.effects = ConstSpan<EffectRecord>(&kWrite, 1U)});
        writeref         = d->register_op("writeref", {.effects = ConstSpan<EffectRecord>(&kWriteOp0, 1U)});
        Dialect* const io = ctx.register_dialect("io");
        ioread            = io->register_op("read", {.effects = ConstSpan<EffectRecord>(&kFile, 1U)});
        box               = ctx.register_dialect("nb")->register_op("box", {}); // effect-free region holder
        src               = ctx.register_dialect("res")->register_op("make", {}); // value producer
        opaque            = ctx.intern_op("opaque", "thing");                     // NOT registered
    }
    static constexpr EffectRecord kWrite{EffectFamily::MemoryWrite, EffectTarget::None, 0U, 0U};
    static constexpr EffectRecord kFile{EffectFamily::FileIO, EffectTarget::None, 0U, 0U};
    static constexpr EffectRecord kWriteOp0{EffectFamily::MemoryWrite, EffectTarget::Operand, 0U, 0U};
};

// The module body's first block (created on demand) — the module body Region holds BLOCKS; ops live inside them.
Block* body_block(Context& ctx, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = ctx.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
// Create a public func @name, LINK it into the module body (so the call-graph / domain walks find it), return the op.
Operation* make_func(Context& ctx, Module& m, containers::StringView name)
{
    Operation* const f = fn::create_func(ctx, m, name, Visibility::Public, 0U);
    body_block(ctx, m)->append(f);
    return f;
}
// Append `op` to func `f`'s entry block.
Operation* in_body(Context& ctx, Operation* f, Operation* op)
{
    (void)ctx;
    fn::func_body_block(f)->append(op);
    return op;
}
// The single effective effect family of `op` (asserts exactly one). Helper for the "exact family set" checks.
EffectFamily sole_effect(Context& ctx, const Operation& op, const SymbolTable& table)
{
    containers::Array<EffectRecord> eff(ctx.allocator());
    ctx.effective_effects(op, table, eff);
    REQUIRE(eff.size() == 1U);
    CHECK(eff[0].target == EffectTarget::None); // lifted effects are AMBIENT (whole-class)
    return eff[0].family;
}
bool has_family(Context& ctx, const Operation& op, const SymbolTable& table, EffectFamily f)
{
    containers::Array<EffectRecord> eff(ctx.allocator());
    ctx.effective_effects(op, table, eff);
    for (u32 i = 0; i < static_cast<u32>(eff.size()); ++i)
    {
        if (eff[i].family == f) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("ceir calls: effective effects - static passthrough and a resolved call's callee set", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);
    Module* const           m = ctx.create_module();

    // a plain mem.write op: effective effects == its static {MemoryWrite}.
    Operation* const w = ctx.create_operation(o.memw, {}, 0U, {}, 0U);
    CHECK(sole_effect(ctx, *w, *m->symbols()) == EffectFamily::MemoryWrite);

    // a func @writer that writes; a call to it resolves to {MemoryWrite}, NOT the conservative ExternalCall barrier.
    Operation* const writer = make_func(ctx, *m, "writer");
    in_body(ctx, writer, ctx.create_operation(o.memw, {}, 0U, {}, 0U));
    Operation* const call = fn::create_call(ctx, "writer", {}, 0U);
    CHECK(sole_effect(ctx, *call, *m->symbols()) == EffectFamily::MemoryWrite);
    CHECK_FALSE(has_family(ctx, *call, *m->symbols(), EffectFamily::ExternalCall)); // refined, not opaque
}

TEST_CASE("ceir calls: effects are unioned TRANSITIVELY and through nested regions", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);
    Module* const           m = ctx.create_module();

    // c writes; b calls c; a calls b  ⇒  a call to @a is {MemoryWrite} (A->B->C transitive).
    Operation* const fc = make_func(ctx, *m, "c");
    in_body(ctx, fc, ctx.create_operation(o.memw, {}, 0U, {}, 0U));
    Operation* const fb = make_func(ctx, *m, "b");
    in_body(ctx, fb, fn::create_call(ctx, "c", {}, 0U));
    Operation* const fa = make_func(ctx, *m, "a");
    in_body(ctx, fa, fn::create_call(ctx, "b", {}, 0U));
    Operation* const call_a = fn::create_call(ctx, "a", {}, 0U);
    CHECK(sole_effect(ctx, *call_a, *m->symbols()) == EffectFamily::MemoryWrite);

    // @n's body holds a box whose NESTED region writes ⇒ a call to @n still sees {MemoryWrite}.
    Operation* const fn2 = make_func(ctx, *m, "n");
    Operation* const bx  = ctx.create_operation(o.box, {}, 0U, {}, 1U);
    Block* const     rb  = ctx.create_block(0U);
    bx->region(0)->append(rb);
    rb->append(ctx.create_operation(o.memw, {}, 0U, {}, 0U));
    in_body(ctx, fn2, bx);
    Operation* const call_n = fn::create_call(ctx, "n", {}, 0U);
    CHECK(sole_effect(ctx, *call_n, *m->symbols()) == EffectFamily::MemoryWrite);
}

TEST_CASE("ceir calls: recursion in the callee graph TERMINATES (cycle guard)", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);
    Module* const           m = ctx.create_module();

    // self-recursion: @s calls only @s ⇒ effect-free AND the walk terminates (empty set).
    Operation* const fs = make_func(ctx, *m, "s");
    in_body(ctx, fs, fn::create_call(ctx, "s", {}, 0U));
    containers::Array<EffectRecord> eff(ctx.allocator());
    ctx.effective_effects(*fn::create_call(ctx, "s", {}, 0U), *m->symbols(), eff);
    CHECK(eff.size() == 0U);

    // mutual recursion: @p calls @q; @q calls @p AND writes ⇒ {MemoryWrite}, and the walk terminates.
    Operation* const fq = make_func(ctx, *m, "q");
    in_body(ctx, fq, fn::create_call(ctx, "p", {}, 0U));
    in_body(ctx, fq, ctx.create_operation(o.memw, {}, 0U, {}, 0U));
    Operation* const fp = make_func(ctx, *m, "p");
    in_body(ctx, fp, fn::create_call(ctx, "q", {}, 0U));
    CHECK(sole_effect(ctx, *fn::create_call(ctx, "p", {}, 0U), *m->symbols()) == EffectFamily::MemoryWrite);
}

TEST_CASE("ceir calls: effects DEGRADE to an ExternalCall barrier on any unmodeled reach", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);
    Module* const           m = ctx.create_module();

    // a top-level call to a symbol that does not exist ⇒ the ExternalCall barrier.
    CHECK(sole_effect(ctx, *fn::create_call(ctx, "ghost", {}, 0U), *m->symbols()) == EffectFamily::ExternalCall);

    // a RESOLVED call whose callee makes an UNRESOLVED nested call ⇒ still degrades (ExternalCall present).
    Operation* const fg = make_func(ctx, *m, "g");
    in_body(ctx, fg, fn::create_call(ctx, "phantom", {}, 0U));
    CHECK(has_family(ctx, *fn::create_call(ctx, "g", {}, 0U), *m->symbols(), EffectFamily::ExternalCall));

    // ⛔ the interprocedural landmine: an UNREGISTERED op inside the callee ⇒ degrade (a registered-empty must not read
    // as provably-effect-free through a call).
    Operation* const fu = make_func(ctx, *m, "u");
    in_body(ctx, fu, ctx.create_operation(o.opaque, {}, 0U, {}, 0U));
    CHECK(sole_effect(ctx, *fn::create_call(ctx, "u", {}, 0U), *m->symbols()) == EffectFamily::ExternalCall);
}

TEST_CASE("ceir calls: hazard A/B - a call is a barrier without a table, callee-precise with one", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);
    Module* const           m = ctx.create_module();

    make_func(ctx, *m, "pure"); // empty body ⇒ effect-free
    Operation* const fw = make_func(ctx, *m, "w");
    in_body(ctx, fw, ctx.create_operation(o.memw, {}, 0U, {}, 0U));

    Operation* const cp1 = fn::create_call(ctx, "pure", {}, 0U);
    Operation* const cp2 = fn::create_call(ctx, "pure", {}, 0U);
    Operation* const cw1 = fn::create_call(ctx, "w", {}, 0U);
    Operation* const cw2 = fn::create_call(ctx, "w", {}, 0U);

    // NO table: every call is the conservative ExternalCall/Universe barrier ⇒ WAW even between two PURE calls.
    CHECK(ctx.ops_hazard(*cp1, *cp2) == HazardKind::Waw);
    // WITH the table: two pure calls carry no effects ⇒ freely reorderable; two memory-writers WAW.
    CHECK(ctx.ops_hazard(*cp1, *cp2, *m->symbols()) == HazardKind::None);
    CHECK(ctx.ops_hazard(*cw1, *cw2, *m->symbols()) == HazardKind::Waw);
    // a non-call op keeps its PRECISE static access even on the table path (a pure call does not hazard a distinct write).
    CHECK(ctx.ops_hazard(*cp1, *cw1, *m->symbols()) == HazardKind::None);

    // ⛔ the table path must NOT lose per-Value precision for NON-call ops: two operand-targeted writes over DISTINCT
    // resources are non-aliasing ⇒ None (a WAW only over the SAME Value). Pins that the static branch survives the table.
    Block* const     bb = body_block(ctx, *m);
    Operation* const r1 = ctx.create_operation(o.src, {}, 1U, ctx.type_i32());
    Operation* const r2 = ctx.create_operation(o.src, {}, 1U, ctx.type_i32());
    bb->append(r1);
    bb->append(r2);
    Value* const     v1  = r1->result(0);
    Value* const     v2  = r2->result(0);
    Value*           a1[]= {v1};
    Value*           a2[]= {v2};
    Operation* const w1  = ctx.create_operation(o.writeref, ConstSpan<Value*>(a1, 1U), 0U);
    Operation* const w1b = ctx.create_operation(o.writeref, ConstSpan<Value*>(a1, 1U), 0U);
    Operation* const w2  = ctx.create_operation(o.writeref, ConstSpan<Value*>(a2, 1U), 0U);
    CHECK(ctx.ops_hazard(*w1, *w2, *m->symbols()) == HazardKind::None); // distinct Values, non-aliasing
    CHECK(ctx.ops_hazard(*w1, *w1b, *m->symbols()) == HazardKind::Waw); // same Value ⇒ still a WAW
}

TEST_CASE("ceir calls: domain legality - a call resolves to its callee's families (the 4c gap closed)", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);
    Module* const           m = ctx.create_module();

    // callees: @pure (effect-free), @reader (FileIO).
    make_func(ctx, *m, "pure");
    Operation* const reader = make_func(ctx, *m, "reader");
    in_body(ctx, reader, ctx.create_operation(o.ioread, {}, 0U, {}, 0U));

    // an AudioRealTime region holding ONE call.
    const auto build_audio_call = [&](containers::StringView callee) -> Operation* {
        Operation* const owner = ctx.create_operation(o.box, {}, 0U, {}, 1U);
        ctx.set_region_exec(owner, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
        Block* const rb = ctx.create_block(0U);
        owner->region(0)->append(rb);
        rb->append(fn::create_call(ctx, callee, {}, 0U));
        return owner;
    };

    SECTION("a call to a PURE func is legal in an audio region")
    {
        body_block(ctx, *m)->append(build_audio_call("pure"));
        CHECK(ctx.find_domain_violation(*m).op == nullptr);
    }
    SECTION("a call to a FileIO func is flagged")
    {
        Operation* const owner  = build_audio_call("reader");
        body_block(ctx, *m)->append(owner);
        const DomainViolation v = ctx.find_domain_violation(*m);
        REQUIRE(v.op != nullptr);
        CHECK(v.effect == EffectFamily::FileIO); // the callee's family, surfaced at the call site
    }
    SECTION("an UNRESOLVED call is flagged - ExternalCall is now forbidden in audio (the flip)")
    {
        body_block(ctx, *m)->append(build_audio_call("does_not_exist"));
        const DomainViolation v = ctx.find_domain_violation(*m);
        REQUIRE(v.op != nullptr);
        CHECK(v.effect == EffectFamily::ExternalCall);
    }
}

TEST_CASE("ceir calls: recursion policy - declared promises are verified against the call graph", "[ceir][calls]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Ops               o(ctx);

    SECTION("a None-declared ACYCLIC func passes")
    {
        Module* const    a = ctx.create_module();
        Operation* const f = make_func(ctx, *a, "leaf");
        fn::set_recursion_policy(ctx, f, fn::RecursionPolicy::None);
        make_func(ctx, *a, "other"); // no calls at all
        CHECK(fn::find_recursion_violation(ctx, *a, *a->symbols()).kind == fn::RecursionViolationKind::None);
    }
    SECTION("a None-declared SELF-recursive func is flagged, pointing at itself")
    {
        Module* const    a = ctx.create_module();
        Operation* const s = make_func(ctx, *a, "s");
        in_body(ctx, s, fn::create_call(ctx, "s", {}, 0U));
        fn::set_recursion_policy(ctx, s, fn::RecursionPolicy::None);
        const fn::RecursionViolation v = fn::find_recursion_violation(ctx, *a, *a->symbols());
        CHECK(v.kind == fn::RecursionViolationKind::DeclaredNoneRecurses);
        CHECK(v.func_op == s);
    }
    SECTION("recursion THROUGH a nested region is caught (collect_callees descends regions)")
    {
        Module* const    a  = ctx.create_module();
        Operation* const g  = make_func(ctx, *a, "g");
        Operation* const bx = ctx.create_operation(o.box, {}, 0U, {}, 1U); // g's self-call hides inside a box region
        Block* const     rb = ctx.create_block(0U);
        bx->region(0)->append(rb);
        rb->append(fn::create_call(ctx, "g", {}, 0U));
        in_body(ctx, g, bx);
        fn::set_recursion_policy(ctx, g, fn::RecursionPolicy::None);
        const fn::RecursionViolation v = fn::find_recursion_violation(ctx, *a, *a->symbols());
        CHECK(v.kind == fn::RecursionViolationKind::DeclaredNoneRecurses);
        CHECK(v.func_op == g);
    }
    SECTION("a None func in a MUTUAL cycle is flagged; the undeclared partner is NOT (verify-where-declared)")
    {
        Module* const    a  = ctx.create_module();
        Operation* const fa = make_func(ctx, *a, "a");
        Operation* const fb = make_func(ctx, *a, "b");
        in_body(ctx, fa, fn::create_call(ctx, "b", {}, 0U));
        in_body(ctx, fb, fn::create_call(ctx, "a", {}, 0U)); // b is Unspecified — a legal (unacknowledged) cycle member
        fn::set_recursion_policy(ctx, fa, fn::RecursionPolicy::None);
        const fn::RecursionViolation v = fn::find_recursion_violation(ctx, *a, *a->symbols());
        CHECK(v.kind == fn::RecursionViolationKind::DeclaredNoneRecurses);
        CHECK(v.func_op == fa); // the DECLARED offender, not b
    }
    SECTION("a Bounded func WITH a positive depth may recurse")
    {
        Module* const    a = ctx.create_module();
        Operation* const g = make_func(ctx, *a, "g");
        in_body(ctx, g, fn::create_call(ctx, "g", {}, 0U)); // self-recursive but ACKNOWLEDGED
        fn::set_recursion_policy(ctx, g, fn::RecursionPolicy::Bounded, 4U);
        CHECK(fn::find_recursion_violation(ctx, *a, *a->symbols()).kind == fn::RecursionViolationKind::None);
    }
    SECTION("a Bounded func WITHOUT a depth is flagged (declared words must be validated)")
    {
        Module* const    b = ctx.create_module();
        Operation* const h = make_func(ctx, *b, "h");
        in_body(ctx, h, fn::create_call(ctx, "h", {}, 0U));
        fn::set_recursion_policy(ctx, h, fn::RecursionPolicy::Bounded); // depth omitted
        const fn::RecursionViolation v = fn::find_recursion_violation(ctx, *b, *b->symbols());
        CHECK(v.kind == fn::RecursionViolationKind::BoundedMissingDepth);
        CHECK(v.func_op == h);
    }
    SECTION("a corrupt recursion attr is flagged")
    {
        Module* const    a = ctx.create_module();
        Operation* const f = make_func(ctx, *a, "f");
        ctx.set_attr(f, "recursion", ctx.attr_int(99)); // out of the RecursionPolicy vocabulary
        CHECK(fn::find_recursion_violation(ctx, *a, *a->symbols()).kind == fn::RecursionViolationKind::InvalidPolicyAttr);
    }
}
