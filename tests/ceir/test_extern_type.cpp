// CEIR-8a (ADR-0111) — the OPEN-WORLD type model: dialect-defined type-classes behind TypeKind::Extern, with ZERO
// central-enum edits per class. The matrix: intern dedup + the different-class/same-params discriminator (the
// operator== landmine); text + binary round-trip byte-exact (the generic form); THE U-§56 headline (serialize
// registered -> decode UNREGISTERED -> re-serialize byte-exact -> late-register -> usable); the verify triple
// (factory asserts / decoder rejects / parser rejects); interface-hash class discrimination; the version range check;
// substitute-through-Extern; and a pre-8a (no-Extern) module still round-trips. Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/program_asset.hpp> // interface_hash

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using ByteArray = crd::containers::Array<u8>;

namespace
{
// A test type-class verify hook: a `testcad.toposhape` wraps EXACTLY ONE element type (a param). 0 or >1 members reject.
bool verify_toposhape(const Context&, const Type& t) noexcept { return t.members.size() == 1U; }

// Register the test type-class on `ctx` at `version`; returns its id. dialect "testcad", class "toposhape".
TypeClassId register_toposhape(Context& ctx, u32 version = 1U)
{
    Dialect* const d = ctx.register_dialect("testcad");
    return d->register_type_class("toposhape", TypeClassSpec{&verify_toposhape, version});
}
// A `testcad.toposhape<elem>` custom type (one member = the element).
TypeId toposhape_of(Context& ctx, TypeClassId cls, TypeId elem)
{
    Type p;
    const TypeId m[1] = {elem};
    p.members         = ConstSpan<TypeId>(m, 1U);
    return ctx.type_extern(cls, p);
}
// A module whose entry block carries ONE arg of type `ty` — so serialization/printing exercises the TYPE record.
Module* module_with_typed_arg(Context& ctx, TypeId ty)
{
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(1U, ty);
    m->body()->append(b);
    return m;
}
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] bool text_eq(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
} // namespace

TEST_CASE("ceir 8a: a custom type interns/dedups, and a DIFFERENT class with identical params is a DIFFERENT type", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    // a SECOND class with the exact same param shape (one member) — the operator== landmine test.
    Dialect* const    d2  = ctx.register_dialect("testeda");
    const TypeClassId net = d2->register_type_class("net", TypeClassSpec{&verify_toposhape, 1U});

    const TypeId a1 = toposhape_of(ctx, topo, ctx.type_f32());
    const TypeId a2 = toposhape_of(ctx, topo, ctx.type_f32());
    const TypeId b1 = toposhape_of(ctx, net, ctx.type_f32());
    CHECK(a1 == a2);   // same class + same params -> interned to ONE id (dedup)
    CHECK(a1 != b1);   // ⛔ DIFFERENT class, identical params -> DIFFERENT type (the type_class landmine)
    const TypeId a3 = toposhape_of(ctx, topo, ctx.type_i32()); // same class, different param
    CHECK(a1 != a3);
}

TEST_CASE("ceir 8a: the type-class fields are Extern-only, and an Extern must name a class (canonicality)", "[ceir][extern-type]")
{
    // junk type_class on a NON-Extern kind would print as the plain type but intern DISTINCTLY — non-canonical, rejected.
    Type junk_bool;
    junk_bool.kind       = TypeKind::Bool;
    junk_bool.type_class = TypeClassId{123U};
    CHECK_FALSE(type_is_canonical(junk_bool));
    Type junk_ver;
    junk_ver.kind               = TypeKind::Int;
    junk_ver.count              = 32U;
    junk_ver.is_signed          = true;
    junk_ver.type_class_version = 4U;
    CHECK_FALSE(type_is_canonical(junk_ver));
    // an Extern with class 0 is nonsense (the factory could be handed {}) — rejected.
    Type classless;
    classless.kind = TypeKind::Extern;
    CHECK_FALSE(type_is_canonical(classless));
}

TEST_CASE("ceir 8a: a custom type round-trips text AND binary byte-exact (the generic form)", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    Module* const                m    = module_with_typed_arg(ctx, toposhape_of(ctx, topo, ctx.type_f32()));

    // binary: serialize -> deserialize (fresh ctx, class registered) -> re-serialize == byte-exact.
    const ByteArray blob1 = serialize(ctx, *m, &root);
    Context         ctx2(&root);
    (void)register_toposhape(ctx2);
    const ParseResult pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const ByteArray blob2 = serialize(ctx2, *pr.module, &root);
    CHECK(blob_eq(blob1, blob2));

    // text: print -> parse (fresh ctx) -> re-print == byte-exact.
    const String t1 = print(ctx, *m, &root);
    Context      ctx3(&root);
    (void)register_toposhape(ctx3);
    const ParseResult tp = parse(ctx3, StringView(t1.data(), t1.size()));
    REQUIRE(tp.ok);
    REQUIRE(tp.module != nullptr);
    const String t3 = print(ctx3, *tp.module, &root);
    CHECK(text_eq(t1, t3));
}

TEST_CASE("ceir 8a: THE U-56 headline - an unknown plugin type round-trips through an UNREGISTERED Context", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    Module* const                m    = module_with_typed_arg(ctx, toposhape_of(ctx, topo, ctx.type_f64()));
    const ByteArray              blob = serialize(ctx, *m, &root);

    // BINARY: decode into a Context WITHOUT the class registered -> preserved opaquely (no reject).
    Context           unreg(&root);
    const ParseResult pr = deserialize(unreg, span(blob));
    REQUIRE(pr.ok); // ⛔ an unknown plugin type is PRESERVED, not rejected (U-56)
    REQUIRE(pr.module != nullptr);
    // re-serialize from the UNREGISTERED context -> BYTE-EXACT (the class string survived; no data lost).
    const ByteArray blob_u = serialize(unreg, *pr.module, &root);
    CHECK(blob_eq(blob, blob_u));

    // TEXT (the other half of ADR-0111 §5 item 4): print registered -> parse into an UNREGISTERED ctx -> re-print exact.
    const String      txt = print(ctx, *m, &root);
    Context           text_unreg(&root);
    const ParseResult tp = parse(text_unreg, StringView(txt.data(), txt.size()));
    REQUIRE(tp.ok); // parse_extern preserves an unregistered class
    REQUIRE(tp.module != nullptr);
    const String txt2 = print(text_unreg, *tp.module, &root);
    CHECK(text_eq(txt, txt2));

    // late registration makes the SAME preserved type FULLY USABLE: a factory build of the class unifies (same TypeId)
    // with the blob-preserved arg type (both version 1 — the content hash is identical -> they intern to one id).
    const TypeId      preserved = pr.module->body()->first_block()->arg(0U)->type();
    const TypeClassId late      = register_toposhape(unreg); // version 1 (matches the encoder's)
    CHECK(late.valid());
    CHECK(toposhape_of(unreg, late, unreg.type_f64()) == preserved);
}

TEST_CASE("ceir 8a: the verify hook rejects at the decoder + parser (registered-invalid), preserves when unregistered", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    // Build a HAND-CRAFTED invalid instance (TWO members — the hook requires exactly one) by going around the factory:
    // an Extern Type with 2 members, serialized. (The factory type_extern would ASSERT; the decoder must REJECT.)
    Type inv;
    const TypeId two[2] = {ctx.type_f32(), ctx.type_i32()};
    inv.kind            = TypeKind::Extern;
    inv.type_class      = topo;
    inv.members         = ConstSpan<TypeId>(two, 2U);
    // intern_type asserts only canonicality (Extern accepts structurally), so this interns; the VALIDITY is the hook's.
    CHECK_FALSE(ctx.verify_extern(inv)); // the hook rejects 2 members directly

    // A blob carrying the invalid record: build it in a ctx where the class is registered so the ENCODER emits it, then
    // decode where it is ALSO registered -> the decoder runs the hook -> REJECT.
    Module* const   m    = module_with_typed_arg(ctx, ctx.intern_type(inv)); // interns the invalid Extern (canonical=true)
    const ByteArray blob = serialize(ctx, *m, &root);
    Context         ctx2(&root);
    (void)register_toposhape(ctx2);
    const ParseResult pr = deserialize(ctx2, span(blob));
    CHECK_FALSE(pr.ok); // ⛔ the decoder ran the registered hook and rejected the 2-member record
    // but decoding the SAME blob where the class is UNREGISTERED preserves it (U-56 — cannot verify the unknown).
    Context           unreg(&root);
    const ParseResult pu = deserialize(unreg, span(blob));
    CHECK(pu.ok);

    // PARSER leg (ADR-0111 §5 item 5): print the invalid module -> parse WITH the class registered -> parse_extern's
    // verify_extern gate rejects into a ParseResult error (the third construction boundary).
    const String      inv_txt = print(ctx, *m, &root);
    Context           pctx(&root);
    (void)register_toposhape(pctx);
    const ParseResult ptp = parse(pctx, StringView(inv_txt.data(), inv_txt.size()));
    CHECK_FALSE(ptp.ok);
}

TEST_CASE("ceir 8a: the interface hash discriminates custom types by class", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    Dialect* const               d2   = ctx.register_dialect("testeda");
    const TypeClassId            net  = d2->register_type_class("net", TypeClassSpec{&verify_toposhape, 1U});
    // @f(x: testcad.toposhape<f32>) and @f(x: testeda.net<f32>) -- identical but for the param's type-class.
    Module* const    fa  = ctx.create_module();
    Block* const     fab = ctx.create_block(0U);
    fa->body()->append(fab);
    fab->append(func::create_func(ctx, *fa, "f", Visibility::Public, 1U, toposhape_of(ctx, topo, ctx.type_f32())));

    Module* const gb  = ctx.create_module();
    Block* const  gbb = ctx.create_block(0U);
    gb->body()->append(gbb);
    gbb->append(func::create_func(ctx, *gb, "f", Visibility::Public, 1U, toposhape_of(ctx, net, ctx.type_f32())));

    CHECK(interface_hash(ctx, *fa, &root) != interface_hash(ctx, *gb, &root)); // class is part of the caller-visible type
}

TEST_CASE("ceir 8a: a blob written by a NEWER class schema version is rejected by an older loader", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      enc(&root);
    const TypeClassId            topo5 = register_toposhape(enc, /*version*/ 5U); // encoder knows schema v5
    Module* const   m    = module_with_typed_arg(enc, toposhape_of(enc, topo5, enc.type_f32()));
    const ByteArray blob = serialize(enc, *m, &root);

    Context dec(&root);
    (void)register_toposhape(dec, /*version*/ 1U); // loader only knows schema v1 -> a v5 record is the future -> reject
    const ParseResult pr = deserialize(dec, span(blob));
    CHECK_FALSE(pr.ok);

    // INVERSE: an OLDER record (v1) loaded by a NEWER loader (v5) is ACCEPTED (the loader knows a superset; a migration
    // transform is named-forward to its future consumer — no reject on backward-compatible schema).
    Context           enc1(&root);
    const TypeClassId c1 = register_toposhape(enc1, /*version*/ 1U);
    const ByteArray   v1blob =
        serialize(enc1, *module_with_typed_arg(enc1, toposhape_of(enc1, c1, enc1.type_f32())), &root);
    Context dec5(&root);
    (void)register_toposhape(dec5, /*version*/ 5U);
    CHECK(deserialize(dec5, span(v1blob)).ok);
}

TEST_CASE("ceir 8a: substitution rebuilds a generic custom type, preserving its class", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    const TypeId                 tp   = ctx.type_param("T", {});             // a generic param
    const TypeId                 gen  = toposhape_of(ctx, topo, tp);         // testcad.toposhape<!param<T>>
    CHECK(ctx.type_has_params(gen));                                          // the param is seen through Extern (generic walk)
    const TypeBinding  bind[1] = {{tp, ctx.type_i32()}};
    const SubstResult  r       = ctx.substitute(gen, ConstSpan<TypeBinding>(bind, 1U));
    REQUIRE(r.ok);
    CHECK(r.type == toposhape_of(ctx, topo, ctx.type_i32())); // rebuilt with the substituted member, SAME class
}

TEST_CASE("ceir 8a: a pre-8a module (no custom types) round-trips byte-exact (no format churn)", "[ceir][extern-type]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m     = module_with_typed_arg(ctx, ctx.type_vector(ctx.type_f32(), 4U));
    const ByteArray              blob1 = serialize(ctx, *m, &root);
    Context                      ctx2(&root);
    const ParseResult            pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    const ByteArray blob2 = serialize(ctx2, *pr.module, &root);
    CHECK(blob_eq(blob1, blob2)); // an ordinary type blob is unchanged by the Extern addition (conditional trailing only)
}

TEST_CASE("ceir 8a: single-byte corruption of a custom-type blob never crashes a loader", "[ceir][extern-type]")
{
    // The 1h hostile-input guard, EXTENDED over the Extern decode path (class-STRP index, version, verify) — the fuzz
    // that lands WITH the feature. ASan/UBSan is the memory-safety proof; ok/!ok are both acceptable, the ONLY invariant
    // is no crash. Swept BOTH registered (exercises the hook + version reject) and unregistered (the preserve path).
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const TypeClassId            topo = register_toposhape(ctx);
    Module* const                m    = module_with_typed_arg(ctx, toposhape_of(ctx, topo, ctx.type_f32()));
    const ByteArray              blob = serialize(ctx, *m, &root);

    for (usize i = 0; i < blob.size(); ++i)
    {
        ByteArray b(&root);
        for (usize j = 0; j < blob.size(); ++j) { b.push_back(blob[j]); }
        b[i] = static_cast<u8>(b[i] ^ 0xFFU);
        {
            Context           c(&root);
            (void)register_toposhape(c); // registered: the hook + version-range decode arms run under corruption
            const ParseResult pr = deserialize(c, span(b));
            (void)pr; // must not crash
        }
        {
            Context           u(&root); // unregistered: the preserve arm runs under corruption
            const ParseResult pu = deserialize(u, span(b));
            (void)pu;
        }
    }
    CHECK(true); // reached only if every corruption was handled without a crash
}
