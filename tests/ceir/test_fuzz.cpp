// CEIR-1h - the round-trip FUZZ + stable-hash harness (permanent, per §119/§167). Random-but-VALID modules are
// generated THROUGH ModuleBuilder (dogfoods 1g; operands only reference already-defined SSA values, so every module is
// well-formed) and must round-trip byte-exact through BOTH serial forms: text print->parse->print and binary
// serialize->deserialize->serialize. The stable content hash (FNV-1a over the content-pure binary blob) is
// deterministic, Context-history-independent, and stable under a binary round-trip. ⛔ FIXED HARDCODED SEEDS ONLY (no
// <random>, never time-seeded). Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/builder.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

#include "rich_graph.hpp" // crd::ceir::test::build_rich

using namespace crd::ceir;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

namespace
{
// A tiny deterministic xorshift64 PRNG (nonzero state never sticks; time-independent). NO <random>.
struct Rng
{
    u64 s;
    explicit Rng(u64 seed) noexcept : s(seed != 0U ? seed : 0x9E3779B97F4A7C15ULL) {}
    u64 next() noexcept
    {
        u64 x = s;
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        s = x;
        return x;
    }
    [[nodiscard]] u32  u32v() noexcept { return static_cast<u32>(next() >> 32U); }
    [[nodiscard]] u32  range(u32 n) noexcept { return n == 0U ? 0U : u32v() % n; } // range(0) guarded
    [[nodiscard]] bool chance(u32 pct) noexcept { return range(100U) < pct; }
};

[[nodiscard]] bool text_equal(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
[[nodiscard]] bool blob_equal(const Array<u8>& a, const Array<u8>& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] ConstSpan<u8> span(const Array<u8>& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }

// A random INTERNED type (CEIR-3a) — bounded depth (aggregates recurse to a scalar leaf by depth 3, so a fuzz module
// can never build an unbounded type), covering every scalar + aggregate kind incl. named struct/enum. Feeds the same
// text + binary round-trip harness, so the TYPE chunk (child-first pool, per-result/arg refs) is fuzzed end-to-end.
[[nodiscard]] TypeId random_type(Context& ctx, Rng& rng, u32 depth)
{
    if (depth >= 3U || rng.chance(50U)) // a scalar leaf
    {
        switch (rng.range(6U))
        {
        case 0U: return ctx.type_bool();
        case 1U: return ctx.type_index();
        case 2U: return ctx.type_int(rng.chance(50U) ? 32U : 64U, rng.chance(50U));
        case 3U: return ctx.type_float(FloatKind::F32);
        case 4U: return ctx.type_float(FloatKind::F64);
        default: return ctx.type_float(FloatKind::BF16);
        }
    }
    const TypeId e = random_type(ctx, rng, depth + 1U);
    switch (rng.range(19U))
    {
    case 0U: return ctx.type_vector(e, 1U + rng.range(8U));
    case 1U: return ctx.type_matrix(e, 1U + rng.range(4U), 1U + rng.range(4U));
    case 2U: return ctx.type_array(e, rng.range(16U));
    case 3U: return ctx.type_complex(e);
    case 4U: return ctx.type_quaternion(e);
    case 5U: return ctx.type_option(e);
    case 6U: return ctx.type_result(e, random_type(ctx, rng, depth + 1U));
    case 7U:
    {
        const TypeId elems[2] = {e, random_type(ctx, rng, depth + 1U)};
        return ctx.type_variant(ConstSpan<TypeId>(elems, 2U));
    }
    case 8U:
    {
        const StringView cases[2] = {StringView("lo"), StringView("hi")};
        return ctx.type_enum(StringView("E"), ConstSpan<StringView>(cases, 2U));
    }
    case 9U: return ctx.type_trait(StringView("Tr"), {}); // a leaf trait (CEIR-3b)
    case 10U:
    {
        const TypeId con[1] = {ctx.type_trait(StringView("C"), {})}; // a type param constrained by a trait
        return ctx.type_param(StringView("P"), rng.chance(50U) ? ConstSpan<TypeId>{} : ConstSpan<TypeId>(con, 1U));
    }
    case 11U:
    {
        const TypeId p[1] = {e}; // a callable: 1 random param, 1 random result
        const TypeId r[1] = {random_type(ctx, rng, depth + 1U)};
        return ctx.type_callable(ConstSpan<TypeId>(p, 1U), ConstSpan<TypeId>(r, 1U));
    }
    case 12U: return ctx.type_buffer(rng.chance(50U) ? BufferMode::Plain : BufferMode::Structured, e); // CEIR-3c
    case 13U: return ctx.type_image(rng.chance(50U) ? ImageDim::Dim2D : ImageDim::Dim3D, e);
    case 14U: return ctx.type_sampler(rng.chance(50U));
    case 15U:
    {
        // a VALID view: pick a resource then a mask legal for it (an illegal combo would trip the factory assert)
        if (rng.chance(50U))
        {
            const TypeId b = ctx.type_buffer(BufferMode::Plain, e);
            return ctx.type_view(b, rng.chance(50U) ? (ViewRange::Byte | ViewRange::Element)
                                                    : static_cast<crd::u32>(ViewRange::Byte));
        }
        const TypeId im = ctx.type_image(ImageDim::Dim2D, e);
        return ctx.type_view(im, rng.chance(50U) ? (ViewRange::Mip | ViewRange::Layer)
                                                 : static_cast<crd::u32>(ViewRange::Aspect));
    }
    case 16U:
    {
        // CEIR-3d: a tensor whose ELEMENT is `e` (never a bare Dim/Shape — those only appear inside a shape here, so the
        // composition is always valid) and whose SHAPE is a small mix of static/symbolic/dynamic dims.
        TypeId    dm   = ctx.type_dim_dynamic();
        const u32 pick = rng.range(3U);
        if (pick == 0U) { dm = ctx.type_dim_static(rng.range(8U)); }
        else if (pick == 1U) { dm = ctx.type_dim_symbolic(StringView("N")); }
        const TypeId dm2[2] = {dm, ctx.type_dim_static(1U + rng.range(4U))};
        const TypeId shp    = ctx.type_shape(ConstSpan<TypeId>(dm2, rng.chance(50U) ? 2U : 1U));
        return rng.chance(50U) ? ctx.type_tensor(e, shp) : ctx.type_sparse_tensor(e, shp);
    }
    case 17U:
    {
        // CEIR-3e: a quantity over a NUMERIC underlying (built fresh so composition is always valid) + a random dim
        const TypeId num = rng.chance(50U) ? ctx.type_f32() : ctx.type_vector(ctx.type_f32(), 1U + rng.range(4U));
        QuantityDim  d;
        d.exp[rng.range(8U)] = static_cast<crd::i8>(static_cast<int>(rng.range(5U)) - 2); // a small signed exponent
        return ctx.type_quantity(num, d);
    }
    default:
    {
        const TypeId     ftys[2]   = {e, random_type(ctx, rng, depth + 1U)};
        const StringView fnames[2] = {StringView("f0"), StringView("f1")};
        return ctx.type_struct(StringView("S"), ConstSpan<TypeId>(ftys, 2U), ConstSpan<StringView>(fnames, 2U));
    }
    }
}

// A result/block-arg type: sometimes NONE (untyped), else a random interned type.
[[nodiscard]] TypeId random_value_type(Context& ctx, Rng& rng) { return rng.chance(30U) ? TypeId{} : random_type(ctx, rng, 0U); }

// A random attribute of a random kind — floats are FINITE (NaN's text form is payload-lossy by design), symbols and
// attr-names are identifiers (the printer emits @name raw), strings include quote/backslash/brace (the 1e escaping).
[[nodiscard]] AttrId random_attr(Context& ctx, Rng& rng)
{
    const u32 pick = rng.range(6U);
    if (pick == 0U) { return ctx.attr_int(static_cast<i64>(rng.next())); } // full-range incl negative
    if (pick == 1U)
    {
        const f64 vals[] = {0.0, -0.0, 1.0, -1.0, 4.5, 4.0, 0.5, -3.25, 1.0e20, 1.0e-20};
        return ctx.attr_float(vals[rng.range(10U)]);
    }
    if (pick == 2U) { return ctx.attr_bool(rng.chance(50U)); }
    if (pick == 3U)
    {
        const char* const strs[] = {"plain", "with space", R"(q"uote)", R"(back\slash)", "brace{}", ""};
        return ctx.attr_string(StringView(strs[rng.range(6U)]));
    }
    if (pick == 4U)
    {
        const char* const syms[] = {"foo", "bar", "baz", "qux"};
        return ctx.attr_symbol(StringView(syms[rng.range(4U)]));
    }
    return ctx.attr_type(random_type(ctx, rng, 0U)); // pick == 5 — a real interned type (possibly a nested aggregate)
}

const char* const kDialects[] = {"test", "math", "cf", "scf", "arith"};
const char* const kOpNames[]  = {"a", "b", "add", "mul", "loop", "br", "sel", "use", "gen", "sink"};
const char* const kAttrKeys[] = {"a", "b", "c", "x", "y", "tag", "k", "flag"};

// Emit `count` random ops at the builder's insertion point, appending each op's results (and any nested-region block
// args) to `live`. Operands are drawn ONLY from `live` (already-defined) so the module is always well-formed.
void gen_ops(ModuleBuilder& mb, Rng& rng, Array<Value*>& live, u32 count, u32 depth)
{
    Context& ctx = mb.context();
    for (u32 k = 0; k < count; ++k)
    {
        OpBuilder ob = mb.op(StringView(kDialects[rng.range(5U)]), StringView(kOpNames[rng.range(10U)]));

        const u32 nops = live.size() == 0U ? 0U : rng.range(4U);
        for (u32 i = 0; i < nops; ++i) { ob.operand(live[rng.range(static_cast<u32>(live.size()))]); }

        const u32 nres = rng.range(3U);
        ob.results(nres, random_value_type(ctx, rng)); // uniform result type (sometimes none)

        const u32 nattrs = rng.range(4U);
        for (u32 i = 0; i < nattrs; ++i) { ob.attr(StringView(kAttrKeys[rng.range(8U)]), random_attr(ctx, rng)); }

        // 0, 1, OR 2 regions — a 2-region op (scf.if/scf.for in CEIR-5) exercises the multi-region paths:
        // count_trailing_regions counting several groups, the parser's per-i region parse, the binary region loops.
        u32 nregions = 0U;
        if (depth < 2U && rng.chance(30U)) { nregions = 1U + rng.range(2U); }
        ob.regions(nregions);

        Operation* const op = ob.build();
        for (u32 i = 0; i < nres; ++i) { live.push_back(op->result(i)); }

        for (u32 ri = 0; ri < nregions; ++ri)
        {
            InsertionGuard g(mb); // each region fills independently, restoring the parent insertion point after
            const u32      rnargs = rng.range(3U);
            Block* const   rb     = mb.add_block(rnargs, random_value_type(ctx, rng), op->region(ri));
            for (u32 i = 0; i < rnargs; ++i) { live.push_back(rb->arg(i)); }
            gen_ops(mb, rng, live, rng.range(4U), depth + 1U); // may be 0 ops — empty-block round-trip coverage
        }
    }
}

[[nodiscard]] Module* gen_random(Context& ctx, Rng& rng)
{
    ModuleBuilder mb(ctx, rng.chance(50U) ? RegionKind::Graph : RegionKind::SsaCfg);
    Array<Value*> live(ctx.allocator());
    const u32     nblocks = 1U + rng.range(3U);
    for (u32 bi = 0; bi < nblocks; ++bi)
    {
        const u32    nargs = rng.range(3U);
        Block* const blk   = mb.add_block(nargs, random_value_type(ctx, rng));
        for (u32 i = 0; i < nargs; ++i) { live.push_back(blk->arg(i)); }
        gen_ops(mb, rng, live, rng.range(5U), 0U); // may be 0 ops (zero-op block round-trip coverage)
    }
    return mb.module();
}

// Assert both serial forms round-trip byte-exact for `m` (built in `ctx`).
void check_roundtrips(Context& ctx, Module& m, crd::memory::IAllocator* root)
{
    const String t1 = print(ctx, m, root);
    Context      ctx_text(root);
    const auto   pr = parse(ctx_text, StringView(t1.data(), t1.size()));
    REQUIRE(pr.ok); // a generated module is always valid text
    CHECK(text_equal(t1, print(ctx_text, *pr.module, root)));

    const Array<u8> b1 = serialize(ctx, m, root);
    Context         ctx_bin(root);
    const auto      pb = deserialize(ctx_bin, span(b1));
    REQUIRE(pb.ok);
    CHECK(blob_equal(b1, serialize(ctx_bin, *pb.module, root)));
}
} // namespace

TEST_CASE("ceir fuzz: random valid modules round-trip byte-exact through text and binary", "[ceir][fuzz]")
{
    crd::memory::MallocAllocator root;
    const u64 seeds[] = {1U, 2U, 3U, 7U, 42U, 0x1234U, 0xABCDEFU, 0xDEADBEEFU, 0xC0FFEEU, 99991U,
                         0x5A5A5A5AU, 123456789U, 0xFACEU, 0x8BADF00DU, 271828U, 314159U};
    for (u64 seed : seeds)
    {
        Context ctx(&root);
        Rng     rng(seed);
        check_roundtrips(ctx, *gen_random(ctx, rng), &root);
    }
    // and the dense hand fixture (func.func/call/return coverage the generator omits)
    Context ctx(&root);
    check_roundtrips(ctx, *test::build_rich(ctx), &root);
}

TEST_CASE("ceir fuzz: the stable content hash is deterministic and content-derived", "[ceir][fuzz][hash]")
{
    crd::memory::MallocAllocator root;

    // determinism: the same module hashes to the same value every time
    Context   ctx(&root);
    Module&   m  = *test::build_rich(ctx);
    const u64 h1 = stable_hash(ctx, m, &root);
    CHECK(h1 == stable_hash(ctx, m, &root));

    // content purity: the same graph in a clean vs a pre-polluted Context hashes EQUAL (no history leakage)
    Context clean(&root);
    Context dirty(&root);
    (void)dirty.register_file("noise.txt");
    (void)dirty.intern_op("noise", "op");
    (void)dirty.attr_int(999999);
    (void)dirty.type_f64(); // ...including the TYPE table (v2): the hash is over the content-pure blob, type-history-free
    (void)dirty.type_vector(dirty.type_i64(), 8U);
    CHECK(stable_hash(clean, *test::build_rich(clean), &root) == stable_hash(dirty, *test::build_rich(dirty), &root));

    // discrimination: two clearly-different modules hash differently
    Context      cs(&root);
    Module* const trivial = cs.create_module();
    trivial->body()->append(cs.create_block(0U));
    trivial->body()->first_block()->append(cs.create_operation(cs.intern_op("t", "x"), {}, 0U));
    Context cr(&root);
    CHECK(stable_hash(cs, *trivial, &root) != stable_hash(cr, *test::build_rich(cr), &root));

    // stability under a BINARY round-trip (never across the text path — region kind / NaN are text-invisible)
    Context         c1(&root);
    Module&         src  = *test::build_rich(c1);
    const Array<u8> blob = serialize(c1, src, &root);
    Context         c2(&root);
    const auto      pr = deserialize(c2, span(blob));
    REQUIRE(pr.ok);
    CHECK(stable_hash(c2, *pr.module, &root) == stable_hash(c1, src, &root));
}
