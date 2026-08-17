// CEIR-8z BAND-8 GATE (Foundation Closure). ⛔ A COMPOSING gate, not a re-run of slice tests: ONE curated program
// carries EVERY foundation axis at once -- an open-world Extern TYPE (8a) in an exported func signature, an Extern +
// aggregate ATTR (8b) on a body op, a capability-bearing op with explicit determinism/domain axes (8c/8f -- the 4z
// Unspecified-default trap), settled stable ids (8d) -- and the foundation guarantees are then composed IN SEQUENCE on
// that same module: byte-identical text<->binary; decode into an UNREGISTERED Context preserves the plugin content and
// re-serializes byte-exact (U-56 x STID x caps x extern, composed in ONE blob -- no slice test did this); re-register
// UNIFIES the preserved plugin type and the interface hash is CROSS-CONTEXT PURE (caps + extern composed); a
// TRANSACTION (8i) edits the preserved plugin content in the unregistered Context -- commit succeeds (an unknown op
// verifies true), a second edit rolls back BYTE-IDENTICALLY (the U-52 agent-edits-plugin-content story end-to-end);
// and a single-byte corruption sweep over the COMPOSED blob never crashes a loader (cross-chunk: ATTR pool x STID x
// extern class strings). The band's exit criterion "the foundation is universal by open-world semantics, not a second
// architecture" in assertions. Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>          // umbrella: context/ir/transaction/diagnostic/binary/parse/print
#include <crd/ceir/binary.hpp>        // serialize / deserialize / stable_hash
#include <crd/ceir/effect.hpp>        // EffectRecord / EffectFamily
#include <crd/ceir/func.hpp>          // create_func / func_body_block
#include <crd/ceir/program_asset.hpp> // interface_hash
#include <crd/ceir/semantics.hpp>     // DeterminismClass / EvalDomain

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
namespace fn = crd::ceir::func;
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
// The "gate" plugin dialect: a type-class `gate.shape<elem>` (exactly one member), an attr-class `gate.meta`
// (permissive), and a capability-bearing op `gate.process` with EXPLICIT determinism + domain axes (⛔ the 4z
// Unspecified-default trap: a gate op that carries a mode/domain axis sets it, so the composed program is realistic).
bool verify_shape(const Context&, const Type& t) noexcept { return t.members.size() == 1U; }
bool verify_meta(const Context&, const AttrValue&) noexcept { return true; }

struct GateReg
{
    TypeClassId shape;
    AttrClassId meta;
    OpId        process;
};
GateReg register_gate(Context& ctx)
{
    Dialect* const     d       = ctx.register_dialect("gate");
    const StringView   caps[1] = {StringView{"gpu.compute"}};  // register_op interns + arena-copies these...
    const EffectRecord eff[1]  = {EffectRecord{EffectFamily::MemoryRead}}; // ...and arena-copies the effect span
    GateReg            g{};
    g.shape   = d->register_type_class("shape", TypeClassSpec{&verify_shape, 1U});
    g.meta    = d->register_attr_class("meta", AttrClassSpec{&verify_meta, 1U});
    g.process = d->register_op("process", OpSpec{.effects      = ConstSpan<EffectRecord>(eff, 1U),
                                                 .determinism  = DeterminismClass::BitExact,
                                                 .domain       = EvalDomain::HostFrameTime,
                                                 .capabilities = ConstSpan<StringView>(caps, 1U)});
    (void)fn::register_dialect(ctx); // the func dialect is core substrate (present even where the plugin is not)
    return g;
}
TypeId shape_of(Context& ctx, TypeClassId cls, TypeId elem)
{
    Type         p;
    const TypeId m[1] = {elem};
    p.members         = ConstSpan<TypeId>(m, 1U);
    return ctx.type_extern(cls, p);
}
// The ONE composed module: func @f(%x: gate.shape<f32>) { gate.process [cfg=#extern<gate.meta,7>, list=[1,2]] }.
Module* build_gate_module(Context& ctx, const GateReg& g)
{
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const f = fn::create_func(ctx, *m, "f", Visibility::Public, 1U, shape_of(ctx, g.shape, ctx.type_f32()));
    top->append(f);
    Block* const     body = fn::func_body_block(f);
    Operation* const p    = ctx.create_operation(g.process, {}, 0U);
    body->append(p);
    ctx.set_attr(p, "cfg", ctx.attr_extern(g.meta, ctx.attr_int(7)));      // a custom Extern attr (8b) -> content hash
    const AttrId elems[2] = {ctx.attr_int(1), ctx.attr_int(2)};
    ctx.set_attr(p, "list", ctx.attr_array(ConstSpan<AttrId>(elems, 2U))); // an aggregate attr (8b)
    return m;
}
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
[[nodiscard]] bool          blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] bool text_eq(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
} // namespace

TEST_CASE("ceir 8z: the composed foundation program round-trips text and binary byte-identically", "[ceir][gate8]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const GateReg                g = register_gate(ctx);
    Module* const                m = build_gate_module(ctx, g);

    // binary: serialize (settles stable ids + emits STID) -> deserialize -> re-serialize BYTE-EXACT.
    const ByteArray b1 = serialize(ctx, *m, &root);
    Context         ctx_b(&root);
    (void)register_gate(ctx_b);
    const ParseResult pr = deserialize(ctx_b, span(b1));
    REQUIRE(pr.ok);
    CHECK(blob_eq(b1, serialize(ctx_b, *pr.module, &root)));

    // text: print -> parse (gate registered) -> re-print BYTE-EXACT (the generic Extern forms, U-56 round-trip).
    const String      t1 = print(ctx, *m, &root);
    Context           ctx_t(&root);
    (void)register_gate(ctx_t);
    const ParseResult tp = parse(ctx_t, StringView(t1.data(), t1.size()));
    REQUIRE(tp.ok);
    CHECK(text_eq(t1, print(ctx_t, *tp.module, &root)));
}

TEST_CASE("ceir 8z: the composed blob preserves through an unregistered Context and re-serializes byte-exact", "[ceir][gate8]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const GateReg                g = register_gate(ctx);
    Module* const                m = build_gate_module(ctx, g);
    const ByteArray              b1 = serialize(ctx, *m, &root);

    // Decode into a host that has the CORE substrate (func) but NOT the `gate` plugin -> the Extern type, the Extern +
    // aggregate attrs, and the unknown `gate.process` op are all PRESERVED opaquely, and the blob re-serializes byte-exact
    // (U-56 composed with the STID chunk + the ATTR pool + the extern class strings, in ONE blob).
    Context unreg(&root);
    (void)fn::register_dialect(unreg); // core substrate present; the plugin is not
    const ParseResult pr = deserialize(unreg, span(b1));
    REQUIRE(pr.ok);
    CHECK(blob_eq(b1, serialize(unreg, *pr.module, &root)));

    // The program's required-capability set treats the unknown plugin op conservatively (external.process, EMPTY!=UNKNOWN).
    Array<CapabilityId> prog(&root);
    unreg.program_capabilities(*pr.module, prog);
    bool has_external = false;
    for (usize i = 0; i < prog.size(); ++i)
    {
        if (prog[i] == unreg.intern_capability("external.process")) { has_external = true; }
    }
    CHECK(has_external);
}

TEST_CASE("ceir 8z: re-registering unifies the preserved plugin type and the interface hash is cross-Context pure", "[ceir][gate8]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const GateReg                g  = register_gate(ctx);
    Module* const                m  = build_gate_module(ctx, g);
    const u64                    ih = interface_hash(ctx, *m, &root);
    const ByteArray              b1 = serialize(ctx, *m, &root);

    // A SECOND Context with the gate plugin RE-registered: decode -> the preserved Extern type on the func param UNIFIES
    // with a factory build of the class (same TypeId), and the interface hash is IDENTICAL across the two Contexts
    // (cross-Context purity with caps + extern composed -- 7a/7b purity, now over the open-world surfaces).
    Context       ctx2(&root);
    const GateReg g2 = register_gate(ctx2);
    const ParseResult pr = deserialize(ctx2, span(b1));
    REQUIRE(pr.ok);
    const TypeId param_ty = fn::func_body_block(pr.module->body()->first_block()->first_op())->arg(0U)->type();
    CHECK(param_ty == shape_of(ctx2, g2.shape, ctx2.type_f32())); // the preserved plugin type unifies after re-register
    CHECK(interface_hash(ctx2, *pr.module, &root) == ih);          // cross-Context interface-hash purity (caps + extern)
}

TEST_CASE("ceir 8z: a transaction edits preserved plugin content in an unregistered Context", "[ceir][gate8]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const GateReg                g  = register_gate(ctx);
    Module* const                m  = build_gate_module(ctx, g);
    const ByteArray              b1 = serialize(ctx, *m, &root);

    // COMMIT: in a host lacking the plugin, an agent edit (inserting an op) commits -- an unknown op verifies true, so
    // the transaction machinery works over preserved plugin content (the U-52 agent-edits-plugin story).
    {
        Context unreg(&root);
        (void)fn::register_dialect(unreg);
        const ParseResult pr = deserialize(unreg, span(b1));
        REQUIRE(pr.ok);
        Block* const     bodyblk = pr.module->body()->first_block();
        DiagnosticEngine diag(unreg, &root);
        Transaction      tx(unreg, *pr.module, diag, &root);
        Operation* const note = tx.insert(unreg.intern_op("plugin", "note"), {}, 0U, bodyblk);
        REQUIRE(note != nullptr);
        REQUIRE(tx.commit());
        CHECK(note->stable_id().valid());
        bool touched = false;
        for (usize i = 0; i < tx.touched().size(); ++i)
        {
            if (tx.touched()[i] == note->stable_id()) { touched = true; }
        }
        CHECK(touched);
    }
    // ROLLBACK: a fresh decode; an edit that rolls back leaves the preserved plugin module BYTE-IDENTICAL.
    {
        Context unreg(&root);
        (void)fn::register_dialect(unreg);
        const ParseResult pr = deserialize(unreg, span(b1));
        REQUIRE(pr.ok);
        const ByteArray  before  = serialize(unreg, *pr.module, &root);
        Block* const     bodyblk = pr.module->body()->first_block();
        DiagnosticEngine diag(unreg, &root);
        {
            Transaction tx(unreg, *pr.module, diag, &root);
            REQUIRE(tx.insert(unreg.intern_op("plugin", "note"), {}, 0U, bodyblk) != nullptr);
            tx.rollback();
        }
        CHECK(blob_eq(before, serialize(unreg, *pr.module, &root)));
    }
}

TEST_CASE("ceir 8z: single-byte corruption of the composed foundation blob never crashes a loader", "[ceir][gate8]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const GateReg                g  = register_gate(ctx);
    Module* const                m  = build_gate_module(ctx, g);
    const ByteArray              b1 = serialize(ctx, *m, &root);

    // Corrupt EACH byte once and decode with the plugin registered (so the Extern verify hooks run on decode -- the
    // registered-invalid reject leg) -> the loader either rejects gracefully or preserves; ASan/UBSan is the proof it
    // never reads out of bounds. The composed blob exercises cross-chunk interactions (ATTR pool x STID x class strings)
    // no single-feature per-slice sweep reached.
    for (usize i = 0; i < b1.size(); ++i)
    {
        ByteArray b(&root);
        for (usize k = 0; k < b1.size(); ++k) { b.push_back(b1[k]); }
        b[i] = static_cast<u8>(b[i] ^ 0xFFU);
        Context c(&root);
        (void)register_gate(c);
        (void)deserialize(c, span(b)); // no crash is the assertion (ASan)
    }
    SUCCEED();
}
