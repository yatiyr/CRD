// CEIR-8b (ADR-0112) - the OPEN-WORLD ATTRIBUTE model: the deferred aggregate kinds (Array, Dict) + two wrappers
// (TypedConst, Extern) over one vocabulary, with ZERO central-enum edits per dialect attribute-class. The matrix:
// intern dedup + the wrapper-class discriminator (the operator== landmine - a different class / wrapped-type with the
// same payload is a DIFFERENT attr); canonicality (the new fields are aggregate/wrapper-only; junk on a scalar / an
// unsorted dict / a classless Extern rejects); text + binary round-trip byte-exact (the generic form) for every new
// kind incl. NESTED (the child-first ATTR pool); THE U-56 headline (serialize registered -> decode UNREGISTERED ->
// re-serialize byte-exact -> late-register -> unify); the verify triple (factory / decoder / parser) + the wrapper-
// composition rule (no wrapper-on-wrapper); attrs feed the CONTENT hash but NOT the interface hash; the version range
// check; and a pre-8b (scalar-attr) module round-trips byte-exact (no format churn). Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>       // serialize / deserialize / stable_hash
#include <crd/ceir/func.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/program_asset.hpp> // interface_hash

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
namespace fn = crd::ceir::func;
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
// A test attribute-class verify hook: a `testunit.dimension` wraps EXACTLY an Int payload (a dimension exponent code).
// A non-Int payload rejects. (verify_attr_extern runs this AFTER the wrapper-composition gate, so a wrapper payload is
// already excluded before the hook sees it.)
bool verify_dimension(const Context& ctx, const AttrValue& v) noexcept
{
    return ctx.attr_value(v.payload).kind == AttrKind::Int;
}
// Register `testunit.dimension` at `version`; returns its id.
AttrClassId register_dimension(Context& ctx, u32 version = 1U)
{
    Dialect* const d = ctx.register_dialect("testunit");
    return d->register_attr_class("dimension", AttrClassSpec{&verify_dimension, version});
}
// A module whose body is a single `test.op` carrying ONE named attribute `a = v` - so serialize/print exercise ATTR.
Module* module_with_attr(Context& ctx, AttrId v)
{
    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const op = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
    ctx.set_attr(op, "a", v);
    top->append(op);
    return m;
}
// A func @f() whose body op carries `a = v` - the func SIGNATURE is identical regardless of `v`, so interface_hash is
// insensitive to the attr while stable_hash (content) is not.
Module* func_module_with_body_attr(Context& ctx, AttrId v)
{
    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const f = fn::create_func(ctx, *m, "f", Visibility::Public, 0U);
    top->append(f); // create_func does NOT auto-append the op to the module body (8a precedent) - attach it here
    Operation* const op = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
    ctx.set_attr(op, "a", v);
    fn::func_body_block(f)->append(op);
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

// intern an Array attr from a scratch list of ids.
AttrId arr(Context& ctx, ConstSpan<AttrId> es) { return ctx.intern_attr(AttrValue::of_array(es)); }

// Replace the FIRST occurrence of `from` in `s` with `to` (a test-only munger for building hostile text from a printed
// canonical form). Returns `s` unchanged if `from` is absent.
String replace_first(const String& s, StringView from, StringView to, crd::memory::IAllocator* a)
{
    String        out(a);
    const char*   d = s.data();
    usize         i = 0;
    while (i < s.size())
    {
        if (i + from.size() <= s.size() && std::memcmp(d + i, from.data(), from.size()) == 0)
        {
            for (usize j = 0; j < to.size(); ++j) { out.push_back(to[j]); }
            for (usize j = i + from.size(); j < s.size(); ++j) { out.push_back(d[j]); }
            return out;
        }
        out.push_back(d[i]);
        ++i;
    }
    return out;
}

// Assert that an attr value round-trips byte-exact through both serial forms (binary + text), in a fresh Context.
void check_attr_roundtrips(Context& ctx, AttrId v, crd::memory::GrowableTlsfAllocator& root)
{
    Module* const   m  = module_with_attr(ctx, v);
    const ByteArray b1 = serialize(ctx, *m, &root);
    Context         c2(&root);
    const ParseResult pr = deserialize(c2, span(b1));
    REQUIRE(pr.ok);
    CHECK(blob_eq(b1, serialize(c2, *pr.module, &root)));
    const String      t1 = print(ctx, *m, &root);
    Context           c3(&root);
    const ParseResult tp = parse(c3, StringView(t1.data(), t1.size()));
    REQUIRE(tp.ok);
    CHECK(text_eq(t1, print(c3, *tp.module, &root)));
}
} // namespace

TEST_CASE("ceir 8b: aggregates + wrappers intern/dedup; a DIFFERENT class/type with identical payload is DIFFERENT", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);

    // Array: order is semantic; length matters; dedup by element sequence.
    const AttrId e12[2] = {ctx.attr_int(1), ctx.attr_int(2)};
    const AttrId e21[2] = {ctx.attr_int(2), ctx.attr_int(1)};
    CHECK(arr(ctx, ConstSpan<AttrId>(e12, 2U)) == arr(ctx, ConstSpan<AttrId>(e12, 2U))); // dedup
    CHECK(arr(ctx, ConstSpan<AttrId>(e12, 2U)) != arr(ctx, ConstSpan<AttrId>(e21, 2U))); // order matters
    const AttrId e123[3] = {ctx.attr_int(1), ctx.attr_int(2), ctx.attr_int(3)};
    CHECK(arr(ctx, ConstSpan<AttrId>(e12, 2U)) != arr(ctx, ConstSpan<AttrId>(e123, 3U))); // length matters

    // Dict: key order is NOT semantic (canonicalized) - {a,b} and {b,a} intern to one; a value change is a new attr.
    const StringView kab[2] = {StringView{"a"}, StringView{"b"}};
    const StringView kba[2] = {StringView{"b"}, StringView{"a"}};
    const AttrId     v12[2] = {ctx.attr_int(1), ctx.attr_int(2)};
    const AttrId     v21[2] = {ctx.attr_int(2), ctx.attr_int(1)};
    const AttrId     d_ab = ctx.attr_dict(ConstSpan<StringView>(kab, 2U), ConstSpan<AttrId>(v12, 2U));
    const AttrId     d_ba = ctx.attr_dict(ConstSpan<StringView>(kba, 2U), ConstSpan<AttrId>(v21, 2U)); // b:2,a:1 == a:1,b:2
    CHECK(d_ab == d_ba);
    const AttrId d_ab3 = ctx.attr_dict(ConstSpan<StringView>(kab, 2U), ConstSpan<AttrId>(v21, 2U)); // a:2,b:1
    CHECK(d_ab != d_ab3);

    // TypedConst: the wrapped TYPE is part of identity (the landmine - same payload, different type = different attr).
    CHECK(ctx.attr_typed(ctx.type_f32(), ctx.attr_int(1)) == ctx.attr_typed(ctx.type_f32(), ctx.attr_int(1)));
    CHECK(ctx.attr_typed(ctx.type_f32(), ctx.attr_int(1)) != ctx.attr_typed(ctx.type_i32(), ctx.attr_int(1)));

    // Extern: the CLASS is part of identity (the attr_class landmine - same payload, different class = different attr).
    const AttrClassId dim = register_dimension(ctx);
    Dialect* const    d2  = ctx.register_dialect("testother");
    const AttrClassId oth = d2->register_attr_class("dimension", AttrClassSpec{&verify_dimension, 1U});
    CHECK(ctx.attr_extern(dim, ctx.attr_int(2)) == ctx.attr_extern(dim, ctx.attr_int(2)));
    CHECK(ctx.attr_extern(dim, ctx.attr_int(2)) != ctx.attr_extern(oth, ctx.attr_int(2))); // different class
}

TEST_CASE("ceir 8b: the aggregate/wrapper fields are kind-scoped, and Extern must name a class (canonicality)", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);

    // junk `elems` on a scalar Int would print as `42` but intern DISTINCTLY - non-canonical, rejected.
    AttrValue junk_int = AttrValue::of_int(42);
    const AttrId one[1] = {ctx.attr_int(1)};
    junk_int.elems      = ConstSpan<AttrId>(one, 1U);
    CHECK_FALSE(attr_is_canonical(junk_int));

    // junk `attr_class` on a String - non-canonical. (attr_is_canonical is a pure function of the fields, so an
    // un-interned StringView is fine here - only s.empty() is inspected.)
    AttrValue junk_str  = AttrValue::of_string(StringView{"x"});
    junk_str.attr_class = AttrClassId{123U};
    CHECK_FALSE(attr_is_canonical(junk_str));

    // an Extern with class 0 / no payload is nonsense - rejected.
    AttrValue classless;
    classless.kind = AttrKind::Extern;
    CHECK_FALSE(attr_is_canonical(classless));

    // a Dict whose keys are NOT strictly byte-order sorted (or duplicated) - non-canonical (the on-disk order is fixed).
    AttrValue unsorted;
    unsorted.kind             = AttrKind::Dict;
    const StringView kbad[2]  = {StringView{"b"}, StringView{"a"}}; // descending
    const AttrId     vbad[2]  = {ctx.attr_int(1), ctx.attr_int(2)};
    unsorted.keys             = ConstSpan<StringView>(kbad, 2U);
    unsorted.elems            = ConstSpan<AttrId>(vbad, 2U);
    CHECK_FALSE(attr_is_canonical(unsorted));
    const StringView kdup[2]  = {StringView{"a"}, StringView{"a"}}; // duplicate
    unsorted.keys             = ConstSpan<StringView>(kdup, 2U);
    CHECK_FALSE(attr_is_canonical(unsorted));
}

TEST_CASE("ceir 8b: every new attr kind round-trips text AND binary byte-exact, incl. NESTED (child-first pool)", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const AttrClassId            dim = register_dimension(ctx);

    // a NESTED value that exercises all four kinds at once: a dict whose values are an array (of a typed-const and an
    // extern) and a scalar - the child-first ATTR pool must emit the leaves before the aggregates that reference them.
    const AttrId tc     = ctx.attr_typed(ctx.type_f32(), ctx.attr_float(2.5));
    const AttrId ex     = ctx.attr_extern(dim, ctx.attr_int(-1));
    const AttrId inner[2] = {tc, ex};
    const AttrId a      = arr(ctx, ConstSpan<AttrId>(inner, 2U));
    const StringView keys[2] = {StringView{"list"}, StringView{"n"}};
    const AttrId     vals[2] = {a, ctx.attr_int(7)};
    const AttrId     top  = ctx.attr_dict(ConstSpan<StringView>(keys, 2U), ConstSpan<AttrId>(vals, 2U));
    Module* const    m    = module_with_attr(ctx, top);

    // binary: serialize -> deserialize (fresh ctx, class registered) -> re-serialize == byte-exact.
    const ByteArray blob1 = serialize(ctx, *m, &root);
    Context         ctx2(&root);
    (void)register_dimension(ctx2);
    const ParseResult pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const ByteArray blob2 = serialize(ctx2, *pr.module, &root);
    CHECK(blob_eq(blob1, blob2));

    // text: print -> parse (fresh ctx) -> re-print == byte-exact.
    const String t1 = print(ctx, *m, &root);
    Context      ctx3(&root);
    (void)register_dimension(ctx3);
    const ParseResult tp = parse(ctx3, StringView(t1.data(), t1.size()));
    REQUIRE(tp.ok);
    REQUIRE(tp.module != nullptr);
    const String t3 = print(ctx3, *tp.module, &root);
    CHECK(text_eq(t1, t3));
}

TEST_CASE("ceir 8b: THE U-56 headline - an unknown-plugin Extern attr round-trips through an UNREGISTERED Context", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const AttrClassId            dim = register_dimension(ctx);
    Module* const                m   = module_with_attr(ctx, ctx.attr_extern(dim, ctx.attr_int(3)));
    const ByteArray              blob = serialize(ctx, *m, &root);

    // BINARY: decode WITHOUT the class registered -> preserved opaquely -> re-serialize BYTE-EXACT (class string survived).
    Context           unreg(&root);
    const ParseResult pr = deserialize(unreg, span(blob));
    REQUIRE(pr.ok); // an unknown plugin attr is PRESERVED, not rejected
    REQUIRE(pr.module != nullptr);
    const ByteArray blob_u = serialize(unreg, *pr.module, &root);
    CHECK(blob_eq(blob, blob_u));

    // TEXT: print registered -> parse into an UNREGISTERED ctx -> re-print exact.
    const String      txt = print(ctx, *m, &root);
    Context           text_unreg(&root);
    const ParseResult tp = parse(text_unreg, StringView(txt.data(), txt.size()));
    REQUIRE(tp.ok);
    REQUIRE(tp.module != nullptr);
    const String txt2 = print(text_unreg, *tp.module, &root);
    CHECK(text_eq(txt, txt2));

    // late registration makes the preserved attr FULLY USABLE: a factory build unifies with the blob-preserved value.
    const AttrId      preserved = pr.module->body()->first_block()->first_op()->attr("a");
    const AttrClassId late      = register_dimension(unreg); // version 1 (matches the encoder's)
    CHECK(late.valid());
    CHECK(unreg.attr_extern(late, unreg.attr_int(3)) == preserved);
}

TEST_CASE("ceir 8b: the class verify hook rejects at the decoder + parser (registered-invalid), preserves unregistered", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const AttrClassId            dim = register_dimension(ctx);
    // Build a HAND-CRAFTED invalid Extern (a String payload - the hook requires Int) by going around the factory: the
    // factory attr_extern would ASSERT (it runs the hook); intern_attr asserts only canonicality, so this interns.
    const AttrValue inv = AttrValue::of_extern(dim, 1U, ctx.attr_string("nope"));
    CHECK_FALSE(ctx.verify_attr_extern(inv)); // the hook rejects a non-Int payload directly
    Module* const   m    = module_with_attr(ctx, ctx.intern_attr(inv));
    const ByteArray blob = serialize(ctx, *m, &root);

    Context           ctx2(&root);
    (void)register_dimension(ctx2);
    const ParseResult pr = deserialize(ctx2, span(blob));
    CHECK_FALSE(pr.ok); // the decoder ran the registered hook and rejected the record

    Context           unreg(&root); // the SAME blob where the class is UNREGISTERED preserves it (cannot verify unknown)
    const ParseResult pu = deserialize(unreg, span(blob));
    CHECK(pu.ok);

    // PARSER leg: print the invalid module -> parse WITH the class registered -> parse_wrapper_attr's verify gate rejects.
    const String      inv_txt = print(ctx, *m, &root);
    Context           pctx(&root);
    (void)register_dimension(pctx);
    const ParseResult ptp = parse(pctx, StringView(inv_txt.data(), inv_txt.size()));
    CHECK_FALSE(ptp.ok);
}

TEST_CASE("ceir 8b: the wrapper-composition rule - a wrapper payload is rejected (no wrapper-on-wrapper)", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const AttrClassId            dim = register_dimension(ctx);
    const AttrId                 tc  = ctx.attr_typed(ctx.type_f32(), ctx.attr_int(1)); // a wrapper

    // verify_attr_extern is the composition gate for BOTH wrappers: a payload that is itself a wrapper is rejected.
    CHECK_FALSE(ctx.verify_attr_extern(AttrValue::of_extern(dim, 1U, tc)));
    CHECK_FALSE(ctx.verify_attr_extern(AttrValue::of_typed_const(ctx.type_i32(), tc)));
    // a NON-wrapper payload (a scalar or an aggregate) is allowed through the gate.
    const AttrId e1[1] = {ctx.attr_int(1)};
    CHECK(ctx.verify_attr_extern(AttrValue::of_typed_const(ctx.type_i32(), arr(ctx, ConstSpan<AttrId>(e1, 1U)))));
}

TEST_CASE("ceir 8b: an op attribute feeds the CONTENT hash but NOT the interface hash", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const AttrClassId            dim = register_dimension(ctx);
    Module* const a = func_module_with_body_attr(ctx, ctx.attr_extern(dim, ctx.attr_int(1)));
    Module* const b = func_module_with_body_attr(ctx, ctx.attr_extern(dim, ctx.attr_int(2))); // same signature, diff attr

    CHECK(interface_hash(ctx, *a, &root) == interface_hash(ctx, *b, &root)); // signatures identical -> same iface hash
    CHECK(stable_hash(ctx, *a, &root) != stable_hash(ctx, *b, &root));       // attr is content -> different content hash
}

TEST_CASE("ceir 8b: a blob written by a NEWER attr-class schema version is rejected by an older loader", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      enc(&root);
    const AttrClassId            dim5 = register_dimension(enc, /*version*/ 5U);
    Module* const   m    = module_with_attr(enc, enc.attr_extern(dim5, enc.attr_int(1)));
    const ByteArray blob = serialize(enc, *m, &root);

    Context dec(&root);
    (void)register_dimension(dec, /*version*/ 1U); // loader knows only v1 -> a v5 record is the future -> reject
    CHECK_FALSE(deserialize(dec, span(blob)).ok);

    // INVERSE: an OLDER record (v1) loaded by a NEWER loader (v5) is ACCEPTED (the loader knows a superset).
    Context           enc1(&root);
    const AttrClassId c1     = register_dimension(enc1, /*version*/ 1U);
    const ByteArray   v1blob = serialize(enc1, *module_with_attr(enc1, enc1.attr_extern(c1, enc1.attr_int(1))), &root);
    Context           dec5(&root);
    (void)register_dimension(dec5, /*version*/ 5U);
    CHECK(deserialize(dec5, span(v1blob)).ok);

    // TEXT leg (SYMMETRIC with binary - the 8b parser retrofit): a printed v5 record is rejected by a v1 text loader,
    // and a v1 record is accepted by a v5 text loader.
    const String v5txt = print(enc, *m, &root);
    Context      tdec(&root);
    (void)register_dimension(tdec, /*version*/ 1U);
    CHECK_FALSE(parse(tdec, StringView(v5txt.data(), v5txt.size())).ok);
    const String v1txt = print(enc1, *module_with_attr(enc1, enc1.attr_extern(c1, enc1.attr_int(1))), &root);
    Context      tdec5(&root);
    (void)register_dimension(tdec5, /*version*/ 5U);
    CHECK(parse(tdec5, StringView(v1txt.data(), v1txt.size())).ok);
}

TEST_CASE("ceir 8b: a pre-8b module (scalar attrs only) round-trips byte-exact (no format churn)", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m     = module_with_attr(ctx, ctx.attr_int(1234));
    const ByteArray              blob1 = serialize(ctx, *m, &root);
    Context                      ctx2(&root);
    const ParseResult            pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    const ByteArray blob2 = serialize(ctx2, *pr.module, &root);
    CHECK(blob_eq(blob1, blob2)); // a scalar-attr blob is unchanged by the aggregate/wrapper addition (conditional only)
}

TEST_CASE("ceir 8b: unsorted or duplicate dict keys in TEXT are rejected gracefully (never assert)", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const StringView             keys[2] = {StringView{"a"}, StringView{"b"}};
    const AttrId                 vals[2] = {ctx.attr_int(1), ctx.attr_int(2)};
    Module* const m = module_with_attr(ctx, ctx.attr_dict(ConstSpan<StringView>(keys, 2U), ConstSpan<AttrId>(vals, 2U)));
    const String  t = print(ctx, *m, &root);
    const StringView canon{R"({"a":1,"b":2})"}; // the canonical printed dict (sorted + unique)

    { // sanity: the canonical form round-trips.
        Context           c(&root);
        const ParseResult pr = parse(c, StringView(t.data(), t.size()));
        CHECK(pr.ok);
    }
    { // UNSORTED keys - rejected (the parser requires the on-disk canonical order, symmetric with the binary decoder).
        const String      bad = replace_first(t, canon, StringView{R"({"b":2,"a":1})"}, &root);
        Context           c(&root);
        const ParseResult pr = parse(c, StringView(bad.data(), bad.size()));
        CHECK_FALSE(pr.ok);
    }
    { // DUPLICATE key - rejected GRACEFULLY (this is the assert-on-duplicate landmine: attr_dict sorts but does not
      // dedup, so routing this through intern_attr's canonical assert would CRASH; the parser must fail() instead).
        const String      bad = replace_first(t, canon, StringView{R"({"a":1,"a":2})"}, &root);
        Context           c(&root);
        const ParseResult pr = parse(c, StringView(bad.data(), bad.size()));
        CHECK_FALSE(pr.ok);
    }
}

TEST_CASE("ceir 8b: empty aggregates round-trip text and binary", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    check_attr_roundtrips(ctx, ctx.intern_attr(AttrValue::of_array(ConstSpan<AttrId>())), root);          // []
    check_attr_roundtrips(ctx, ctx.attr_dict(ConstSpan<StringView>(), ConstSpan<AttrId>()), root);        // {}
}

TEST_CASE("ceir 8b: a pathologically nested attribute is rejected by the parser depth guard (no crash)", "[ceir][extern-attr]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    AttrId                       v = ctx.attr_int(1);
    for (u32 i = 0; i < 80U; ++i) // 80-deep nest (> kMaxTypeDepth=64); the factory + printer have no depth cap
    {
        const AttrId e[1] = {v};
        v                 = arr(ctx, ConstSpan<AttrId>(e, 1U));
    }
    Module* const     m = module_with_attr(ctx, v);
    const String      t = print(ctx, *m, &root);
    Context           c(&root);
    const ParseResult pr = parse(c, StringView(t.data(), t.size()));
    CHECK_FALSE(pr.ok); // the recursive-descent parser's depth guard rejects gracefully (no stack overflow)
}

TEST_CASE("ceir 8b: single-byte corruption of an aggregate-attr blob never crashes a loader", "[ceir][extern-attr]")
{
    // The hostile-input guard, EXTENDED over the aggregate/wrapper decode arms (element counts, child ATTR refs, the
    // class STRP index, version, the verify hook). ASan/UBSan is the memory-safety proof; ok/!ok both acceptable, the
    // ONLY invariant is no crash. Swept BOTH registered (the hook + version arms) and unregistered (the preserve arm).
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const AttrClassId            dim = register_dimension(ctx);
    const AttrId     e2[2] = {ctx.attr_typed(ctx.type_f32(), ctx.attr_int(1)), ctx.attr_extern(dim, ctx.attr_int(2))};
    const StringView keys[1] = {StringView{"k"}};
    const AttrId     vals[1] = {arr(ctx, ConstSpan<AttrId>(e2, 2U))};
    Module* const    m    = module_with_attr(ctx, ctx.attr_dict(ConstSpan<StringView>(keys, 1U), ConstSpan<AttrId>(vals, 1U)));
    const ByteArray  blob = serialize(ctx, *m, &root);

    for (usize i = 0; i < blob.size(); ++i)
    {
        ByteArray b(&root);
        for (usize j = 0; j < blob.size(); ++j) { b.push_back(blob[j]); }
        b[i] = static_cast<u8>(b[i] ^ 0xFFU);
        {
            Context           c(&root);
            (void)register_dimension(c);
            const ParseResult pr = deserialize(c, span(b));
            (void)pr; // must not crash
        }
        {
            Context           u(&root);
            const ParseResult pu = deserialize(u, span(b));
            (void)pu;
        }
    }
    CHECK(true); // reached only if every corruption was handled without a crash
}
