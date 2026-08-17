// CEIR-8c (ADR-0113) — effect FAMILY widening (u32->u64) + the OPEN effect-LOCATION model. The matrix: a >=bit-32
// (U-§19) family survives the interface-hash projection (the truncation catcher — a u32 mask would alias bit 34 onto
// bit 2 = MemoryReadWrite); the u64 mask carries a >=bit-32 family through effective_effects without truncation; a
// dialect-registered effect-location class places an effect in its DECLARED ResourceClass; an UNREGISTERED Extern
// location conflicts with EVERYTHING (Universe, EMPTY!=UNKNOWN); two DIFFERENT registered location classes do not
// hazard; the location verify hook rejects a registered-invalid record; and a module blob round-trips byte-exact with
// NO binary-format bump (effects are registration metadata, not serialized). Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>       // serialize / deserialize / kBinaryVersion
#include <crd/ceir/func.hpp>
#include <crd/ceir/program_asset.hpp> // interface_hash

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
namespace fn = crd::ceir::func;
using crd::containers::ConstSpan;
using crd::u8;
using crd::usize;
using ByteArray = crd::containers::Array<u8>;

namespace
{
[[nodiscard]] ConstSpan<EffectRecord> e1(const EffectRecord* a) { return ConstSpan<EffectRecord>(a, 1U); }
[[nodiscard]] ConstSpan<u8>           span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// A func @f() whose single body op is `body_kind` — so interface_hash projects that op's effective family mask.
Module* func_with_body_op(Context& ctx, OpId body_kind)
{
    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const f = fn::create_func(ctx, *m, "f", Visibility::Public, 0U);
    top->append(f);
    fn::func_body_block(f)->append(ctx.create_operation(body_kind, {}, 0U));
    return m;
}
// An ambient (whole-op, 0-operand) instance of `kind`.
Operation* ambient(Context& ctx, OpId kind, Block* b)
{
    Operation* const o = ctx.create_operation(kind, {}, 0U);
    b->append(o);
    return o;
}
// A location-class verify hook: `testfs.strict` accepts only a whole-resource (range_mask == 0) record.
bool verify_no_range(const Context&, const EffectRecord& e) noexcept { return e.range_mask == 0U; }
} // namespace

TEST_CASE("ceir 8c: a >=bit-32 U-19 family survives the interface hash (the u32-truncation catcher)", "[ceir][effect-open]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("efx");
    // AgentAction is ordinal 34 (bit 34); MemoryReadWrite is ordinal 2 (bit 2). A u32 family mask would truncate
    // 1<<34 to 1<<(34 mod 32) = 1<<2 == MemoryReadWrite's bit — so the two funcs would COLLIDE to one interface hash.
    const EffectRecord agent[1] = {{EffectFamily::AgentAction, EffectTarget::None, 0U, 0U}};
    const EffectRecord mrw[1]   = {{EffectFamily::MemoryReadWrite, EffectTarget::None, 0U, 0U}};
    const OpId a = d->register_op("agent", {.effects = e1(agent)});
    const OpId m = d->register_op("mrw", {.effects = e1(mrw)});

    const auto ha = interface_hash(ctx, *func_with_body_op(ctx, a), &root);
    const auto hm = interface_hash(ctx, *func_with_body_op(ctx, m), &root);
    CHECK(ha != hm); // ⛔ equal here = a low-32 truncation somewhere in the family-mask pipeline
}

TEST_CASE("ceir 8c: the u64 mask carries a >=bit-32 family through effective_effects without truncation", "[ceir][effect-open]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("efx");
    const EffectRecord           agent[1] = {{EffectFamily::AgentAction, EffectTarget::None, 0U, 0U}};
    const OpId                   a        = d->register_op("agent", {.effects = e1(agent)});

    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const op = ambient(ctx, a, top);

    crd::containers::Array<EffectRecord> eff(&root);
    ctx.effective_effects(*op, *m->symbols(), eff);
    REQUIRE(eff.size() == 1U);
    CHECK(eff[0].family == EffectFamily::AgentAction); // ⛔ MemoryReadWrite here = bit 34 truncated to bit 2
}

TEST_CASE("ceir 8c: a >=bit-32 callee family lifts transitively into the caller's interface hash", "[ceir][effect-open]")
{
    // The caller's projected effective mask is TRANSITIVE (CEIR-5c) — a func.call folds the callee's families in via the
    // EffectsFn hook + collect_region_effective_mask recursion, both now u64. A PRIVATE callee is not projected itself,
    // so the two modules' interface hashes differ ONLY by whether the caller transitively sees the bit-34 AgentAction.
    auto build = [](Context& ctx, bool callee_has_agent) -> Module* {
        fn::register_dialect(ctx); // ⛔ func.call needs its EffectsFn hook registered to resolve the callee transitively
        Dialect* const     d        = ctx.register_dialect("efx");
        const EffectRecord agent[1] = {{EffectFamily::AgentAction, EffectTarget::None, 0U, 0U}};
        const OpId         a        = d->register_op("agent", {.effects = e1(agent)});
        Module* const      m        = ctx.create_module();
        Block* const       top      = ctx.create_block(0U);
        m->body()->append(top);
        Operation* const callee = fn::create_func(ctx, *m, "callee", Visibility::Private, 0U); // NOT exported
        top->append(callee);
        if (callee_has_agent) { fn::func_body_block(callee)->append(ctx.create_operation(a, {}, 0U)); }
        Operation* const caller = fn::create_func(ctx, *m, "caller", Visibility::Public, 0U);
        top->append(caller);
        fn::func_body_block(caller)->append(fn::create_call(ctx, "callee", ConstSpan<Value*>{}, 0U));
        return m;
    };
    crd::memory::GrowableTlsfAllocator root;
    Context                      ca(&root);
    Context                      cb(&root);
    const auto ha = interface_hash(ca, *build(ca, /*callee_has_agent*/ true), &root);
    const auto hb = interface_hash(cb, *build(cb, /*callee_has_agent*/ false), &root);
    CHECK(ha != hb); // ⛔ equal = the bit-34 family was lost (truncated) on the transitive func.call path
}

TEST_CASE("ceir 8c: an Extern location resolves to its registered class; UNREGISTERED is Universe (EMPTY!=UNKNOWN)", "[ceir][effect-open]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               fs = ctx.register_dialect("testfs");
    const LocationClassId io = fs->register_location_class("file_handle", {nullptr, ResourceClass::Io, 1U});
    const LocationClassId un = ctx.intern_location_class("plugin", "widget"); // interned but NOT registered as a class

    const EffectRecord reg{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 0U, io};
    const EffectRecord unreg{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 0U, un};
    CHECK(ctx.effect_resource_class(reg) == ResourceClass::Io);        // registered ⇒ its declared class
    CHECK(ctx.effect_resource_class(unreg) == ResourceClass::Universe); // unregistered ⇒ maximally conflicting

    // a non-Extern record uses the FAMILY's class (the location machinery does not perturb the existing path).
    const EffectRecord plain{EffectFamily::SceneWrite, EffectTarget::None, 0U, 0U, {}};
    CHECK(ctx.effect_resource_class(plain) == ResourceClass::Scene);
}

TEST_CASE("ceir 8c: an UNREGISTERED Extern location refuses to reorder against everything; distinct classes do not hazard", "[ceir][effect-open]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               efx = ctx.register_dialect("efx");
    Dialect* const               fs  = ctx.register_dialect("testfs");
    Dialect* const               sc  = ctx.register_dialect("testscene");
    const LocationClassId io_loc = fs->register_location_class("file_handle", {nullptr, ResourceClass::Io, 1U});
    const LocationClassId sc_loc = sc->register_location_class("node", {nullptr, ResourceClass::Scene, 1U});
    const LocationClassId un_loc = ctx.intern_location_class("plugin", "widget"); // NOT registered as a location class

    const EffectRecord io_w[1] = {{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 0U, io_loc}};
    const EffectRecord sc_w[1] = {{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 0U, sc_loc}};
    const EffectRecord un_w[1] = {{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 0U, un_loc}};
    const OpId io_write = efx->register_op("io_write", {.effects = e1(io_w)});
    const OpId sc_write = efx->register_op("sc_write", {.effects = e1(sc_w)});
    const OpId un_write = efx->register_op("un_write", {.effects = e1(un_w)});

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const io1 = ambient(ctx, io_write, top);
    Operation* const io2 = ambient(ctx, io_write, top);
    Operation* const sc1 = ambient(ctx, sc_write, top);
    Operation* const un1 = ambient(ctx, un_write, top);
    Operation* const un2 = ambient(ctx, un_write, top);

    CHECK(ctx.ops_hazard(*io1, *io2) == HazardKind::Waw);  // same registered class (Io), both whole-class ⇒ conflict
    CHECK(ctx.ops_hazard(*io1, *sc1) == HazardKind::None);  // Io vs Scene: DIFFERENT registered classes ⇒ no hazard
    CHECK(ctx.ops_hazard(*un1, *io1) == HazardKind::Waw);  // ⛔ unregistered = Universe ⇒ conflicts with everything
    CHECK(ctx.ops_hazard(*un1, *un2) == HazardKind::Waw);
}

TEST_CASE("ceir 8c: a registered location class's verify hook rejects an invalid record (the factory leg)", "[ceir][effect-open]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               fs = ctx.register_dialect("testfs");
    const LocationClassId strict = fs->register_location_class("strict", {&verify_no_range, ResourceClass::Io, 1U});

    const EffectRecord ok{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 0U, strict};        // whole resource
    const EffectRecord bad{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 4U, strict};       // a range narrowing
    CHECK(ctx.effect_location_valid(ok));
    CHECK_FALSE(ctx.effect_location_valid(bad)); // the hook rejects; register_op ASSERTS on this (the factory leg)
    // an UNREGISTERED Extern location has no hook ⇒ preserved (validity cannot be judged — U-§56 discipline).
    const EffectRecord unreg{EffectFamily::DocumentWrite, EffectTarget::Extern, 0U, 4U, ctx.intern_location_class("x", "y")};
    CHECK(ctx.effect_location_valid(unreg));
}

TEST_CASE("ceir 8c: AgentAction/TransactionBoundary are forbidden in an audio/device region; domain writes are legal", "[ceir][effect-open]")
{
    // The SECOND family consumer (§32 effect_legal_in_region): the audio-RT denylist. CEIR-8c's deliberate classification
    // forbids the two unbounded/blocking families (AgentAction ~ ExternalCall, TransactionBoundary ~ Synchronization) and
    // leaves the bounded domain read/writes legal (like SceneWrite/EcsWrite, already legal in an audio-RT region).
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("efx");
    const EffectRecord           agent[1] = {{EffectFamily::AgentAction, EffectTarget::None, 0U, 0U}};
    const EffectRecord           txn[1]   = {{EffectFamily::TransactionBoundary, EffectTarget::None, 0U, 0U}};
    const EffectRecord           docw[1]  = {{EffectFamily::DocumentWrite, EffectTarget::None, 0U, 0U}};
    const OpId agent_op = d->register_op("agent", {.effects = e1(agent)});
    const OpId txn_op   = d->register_op("txn", {.effects = e1(txn)});
    const OpId doc_op   = d->register_op("docw", {.effects = e1(docw)});
    const OpId hold     = d->register_op("hold", {}); // effect-free region holder

    auto scan = [&](OpId body) -> DomainViolation {
        Module* const    m   = ctx.create_module();
        Block* const     top = ctx.create_block(0U);
        m->body()->append(top);
        Operation* const owner = ctx.create_operation(hold, {}, 0U, {}, 1U);
        top->append(owner);
        ctx.set_region_exec(owner, RegionExec{EvalDomain::DeviceTime, RealtimeClass::AudioRealTime});
        Block* const inner = ctx.create_block(0U);
        owner->region(0)->append(inner);
        inner->append(ctx.create_operation(body, {}, 0U));
        return ctx.find_domain_violation(*m);
    };

    const DomainViolation va = scan(agent_op);
    CHECK(va.op != nullptr);
    CHECK(va.effect == EffectFamily::AgentAction); // ⛔ forbidden (unbounded external-ish work in a real-time region)
    const DomainViolation vt = scan(txn_op);
    CHECK(vt.op != nullptr);
    CHECK(vt.effect == EffectFamily::TransactionBoundary); // ⛔ forbidden (a commit can block, like Synchronization)
    const DomainViolation vd = scan(doc_op);
    CHECK(vd.op == nullptr); // DocumentWrite is a bounded mutation ⇒ LEGAL (consistent with SceneWrite/EcsWrite)
}

TEST_CASE("ceir 8c: effects are registration metadata - a module blob round-trips byte-exact with NO format bump", "[ceir][effect-open]")
{
    CHECK(kBinaryVersion == 2U); // ⛔ the module binary format is UNCHANGED by 8c (effects aren't serialized in it)

    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("efx");
    const EffectRecord           agent[1] = {{EffectFamily::AgentAction, EffectTarget::None, 0U, 0U}};
    const OpId                   a        = d->register_op("agent", {.effects = e1(agent)});

    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    (void)ambient(ctx, a, top);

    const ByteArray blob1 = serialize(ctx, *m, &root);
    Context         ctx2(&root);
    Dialect* const  d2 = ctx2.register_dialect("efx"); // the op-kind re-registers; effects re-derive from registration
    (void)d2->register_op("agent", {.effects = e1(agent)});
    const ParseResult pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    CHECK(blob_eq(blob1, serialize(ctx2, *pr.module, &root)));
}
