// CEIR-3a - the interned TYPE gate (section 16). Types are structural values interned in the Context: identical types
// share one TypeId (equality is a u32 compare), the canonical `!`-sigil grammar prints + parses back byte-exact, and
// every scalar + aggregate kind round-trips (incl. nesting). Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::strstr / std::memcmp

using namespace crd::ceir;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
// A module with one `test.val() : <type>` op per given type — the smallest carrier that prints a result type.
Module* build_typed(Context& ctx, ConstSpan<TypeId> types)
{
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    for (crd::usize i = 0; i < types.size(); ++i)
    {
        b->append(ctx.create_operation(ctx.intern_op("test", "val"), {}, 1U, types[i]));
    }
    return m;
}

[[nodiscard]] bool has(const String& s, const char* needle) noexcept { return std::strstr(s.c_str(), needle) != nullptr; }
} // namespace

TEST_CASE("ceir type: identical types intern to one id; distinct types differ", "[ceir][type]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    CHECK(ctx.type_i32() == ctx.type_int(32U, true));  // dedup: same structure -> same id
    CHECK(ctx.type_i32() != ctx.type_int(32U, false)); // signedness distinguishes
    CHECK(ctx.type_i32() != ctx.type_int(64U, true));  // width distinguishes
    CHECK(ctx.type_f32() != ctx.type_float(FloatKind::F64));
    CHECK(ctx.type_vector(ctx.type_f32(), 4U) == ctx.type_vector(ctx.type_f32(), 4U));
    CHECK(ctx.type_vector(ctx.type_f32(), 4U) != ctx.type_vector(ctx.type_f32(), 3U));
    CHECK(ctx.type_option(ctx.type_i32()) != ctx.type_i32()); // an option is not its element

    // a struct's NAME participates in structural equality (nominal-ish): same fields, different name -> different type
    const TypeId     i32    = ctx.type_i32();
    const TypeId     ts[1]  = {i32};
    const StringView fns[1] = {StringView("x")};
    CHECK(ctx.type_struct("A", ConstSpan<TypeId>(ts, 1U), ConstSpan<StringView>(fns, 1U))
          != ctx.type_struct("B", ConstSpan<TypeId>(ts, 1U), ConstSpan<StringView>(fns, 1U)));
}

TEST_CASE("ceir type: type_of reads back the interned structure", "[ceir][type]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const Type u16 = ctx.type_of(ctx.type_int(16U, false));
    CHECK(u16.kind == TypeKind::Int);
    CHECK(u16.count == 16U);
    CHECK_FALSE(u16.is_signed);

    const Type v = ctx.type_of(ctx.type_vector(ctx.type_f32(), 4U));
    CHECK(v.kind == TypeKind::Vector);
    CHECK(v.count == 4U);
    REQUIRE(v.members.size() == 1U);
    CHECK(ctx.type_of(v.members[0]).kind == TypeKind::Float);

    const Type s = ctx.type_of(ctx.type_i32());
    CHECK(s.kind == TypeKind::Int);
    CHECK(s.is_signed);
}

TEST_CASE("ceir type: every scalar + aggregate kind prints canonically and round-trips byte-exact", "[ceir][type]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const TypeId     f32     = ctx.type_f32();
    const TypeId     i32     = ctx.type_i32();
    const TypeId     u32t    = ctx.type_int(32U, false);
    const TypeId     ftys[2] = {i32, f32};
    const StringView fns[2]  = {StringView("x"), StringView("y")};
    const StringView cns[2]  = {StringView("red"), StringView("green")};
    const TypeId     seq[2]  = {i32, f32};

    const TypeId ts[] = {
        ctx.type_bool(), ctx.type_index(), i32, ctx.type_int(16U, false), f32, ctx.type_float(FloatKind::BF16),
        ctx.type_float(FloatKind::F8E4M3), ctx.type_vector(f32, 4U), ctx.type_matrix(f32, 4U, 4U),
        ctx.type_complex(f32), ctx.type_quaternion(f32), ctx.type_array(i32, 8U),
        ctx.type_tuple(ConstSpan<TypeId>(seq, 2U)),
        ctx.type_struct("Point", ConstSpan<TypeId>(ftys, 2U), ConstSpan<StringView>(fns, 2U)),
        ctx.type_enum("Color", ConstSpan<StringView>(cns, 2U)), ctx.type_variant(ConstSpan<TypeId>(seq, 2U)),
        ctx.type_option(i32), ctx.type_result(i32, u32t),
    };
    Module* const m  = build_typed(ctx, ConstSpan<TypeId>(ts, static_cast<crd::usize>(sizeof(ts) / sizeof(ts[0]))));
    const String  t1 = print(ctx, *m, &root);

    CHECK(has(t1, ": !bool"));
    CHECK(has(t1, ": !index"));
    CHECK(has(t1, ": !i32"));
    CHECK(has(t1, ": !u16"));
    CHECK(has(t1, ": !f32"));
    CHECK(has(t1, ": !bf16"));
    CHECK(has(t1, ": !f8e4m3"));
    CHECK(has(t1, ": !vec<4x!f32>"));
    CHECK(has(t1, ": !mat<4x4x!f32>"));
    CHECK(has(t1, ": !complex<!f32>"));
    CHECK(has(t1, ": !quat<!f32>"));
    CHECK(has(t1, ": !array<8x!i32>"));
    CHECK(has(t1, ": !tuple<!i32,!f32>"));
    CHECK(has(t1, ": !struct<Point,x:!i32,y:!f32>"));
    CHECK(has(t1, ": !enum<Color,red,green>"));
    CHECK(has(t1, ": !variant<!i32,!f32>"));
    CHECK(has(t1, ": !option<!i32>"));
    CHECK(has(t1, ": !result<!i32,!u32>"));

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}

TEST_CASE("ceir type: generics (param/trait/callable) intern, dedup, and round-trip byte-exact", "[ceir][type][generics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const TypeId ord = ctx.type_trait("Ord", {});
    // param structural identity: two params with the same (name, constraints) intern to ONE id
    const TypeId c1[1] = {ord};
    const TypeId t_a   = ctx.type_param("T", ConstSpan<TypeId>(c1, 1U));
    const TypeId t_b   = ctx.type_param("T", ConstSpan<TypeId>(c1, 1U));
    CHECK(t_a == t_b);
    CHECK(ctx.type_param("U", ConstSpan<TypeId>(c1, 1U)) != t_a); // different name -> different param
    CHECK(ctx.type_param("T", {}) != t_a);                       // different constraints -> different param
    CHECK(ctx.type_has_params(t_a));
    CHECK_FALSE(ctx.type_has_params(ctx.type_i32()));
    CHECK(ctx.type_has_params(ctx.type_vector(t_a, 4U))); // a param nested in an aggregate is still generic

    const TypeId i32 = ctx.type_i32();
    const TypeId f32 = ctx.type_f32();
    const TypeId ps[1]  = {t_a};
    const TypeId rs[1]  = {ctx.type_option(i32)};
    const TypeId fn     = ctx.type_callable(ConstSpan<TypeId>(ps, 1U), ConstSpan<TypeId>(rs, 1U));
    const TypeId eq     = ctx.type_trait("Eq", {});
    const TypeId sup[1] = {eq};
    const TypeId ord2   = ctx.type_trait("Ord2", ConstSpan<TypeId>(sup, 1U)); // trait with a supertrait
    const TypeId all[] = {t_a, ctx.type_trait("Eq", {}), ord2, fn, ctx.type_callable({}, {})};

    Module* const m  = build_typed(ctx, ConstSpan<TypeId>(all, static_cast<crd::usize>(sizeof(all) / sizeof(all[0]))));
    const String  t1 = print(ctx, *m, &root);
    CHECK(has(t1, ": !param<T,!trait<Ord>>"));
    CHECK(has(t1, ": !trait<Eq>"));
    CHECK(has(t1, ": !trait<Ord2,!trait<Eq>>"));
    CHECK(has(t1, ": !fn<(!param<T,!trait<Ord>>)->(!option<!i32>)>"));
    CHECK(has(t1, ": !fn<()->()>")); // empty callable
    (void)f32;

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}

TEST_CASE("ceir type: trait conformance is transitive over supertraits", "[ceir][type][generics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const TypeId eq     = ctx.type_trait("Eq", {});
    const TypeId sup[1] = {eq};
    const TypeId ord    = ctx.type_trait("Ord", ConstSpan<TypeId>(sup, 1U)); // Ord : Eq
    const TypeId i32    = ctx.type_i32();
    const TypeId f32    = ctx.type_f32();

    ctx.register_conformance(i32, ord); // i32 conforms to Ord
    CHECK(ctx.satisfies(i32, ord));
    CHECK(ctx.satisfies(i32, eq));       // ...hence to Eq, transitively via the supertrait
    CHECK_FALSE(ctx.satisfies(f32, ord)); // f32 was never registered
    CHECK_FALSE(ctx.satisfies(f32, eq));
}

TEST_CASE("ceir type: substitution binds params, checks constraints, keeps unbound generic", "[ceir][type][generics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const TypeId ord   = ctx.type_trait("Ord", {});
    const TypeId con[1] = {ord};
    const TypeId t     = ctx.type_param("T", ConstSpan<TypeId>(con, 1U)); // T : Ord
    const TypeId i32   = ctx.type_i32();
    const TypeId f32   = ctx.type_f32();
    ctx.register_conformance(i32, ord); // only i32 satisfies Ord

    // ground identity: substituting into a param-free type is a no-op that returns the SAME id
    const SubstResult g = ctx.substitute(ctx.type_vector(i32, 4U), {});
    CHECK(g.ok);
    CHECK(g.type == ctx.type_vector(i32, 4U));

    // unbound param remains -> the result stays generic
    const SubstResult u = ctx.substitute(t, {});
    CHECK(u.ok);
    CHECK(u.type == t);
    CHECK(ctx.type_has_params(u.type));

    // bind T=i32 inside vec<2 x struct<S,f:T>> : the constraint holds, params vanish
    const TypeId     ftys[1] = {t};
    const StringView fns[1]  = {StringView("f")};
    const TypeId     st      = ctx.type_struct("S", ConstSpan<TypeId>(ftys, 1U), ConstSpan<StringView>(fns, 1U));
    const TypeId     gen     = ctx.type_vector(st, 2U);
    const TypeBinding bind_ok[1] = {{t, i32}};
    const SubstResult ok        = ctx.substitute(gen, ConstSpan<TypeBinding>(bind_ok, 1U));
    REQUIRE(ok.ok);
    CHECK_FALSE(ctx.type_has_params(ok.type));
    const StringView fns2[1] = {StringView("f")};
    const TypeId     itys[1] = {i32};
    CHECK(ok.type == ctx.type_vector(ctx.type_struct("S", ConstSpan<TypeId>(itys, 1U), ConstSpan<StringView>(fns2, 1U)), 2U));

    // param on BOTH sides of a callable, bound in one shot
    const TypeId      ps[1]      = {t};
    const TypeId      rs[1]      = {t};
    const TypeId      fn         = ctx.type_callable(ConstSpan<TypeId>(ps, 1U), ConstSpan<TypeId>(rs, 1U));
    const SubstResult fok        = ctx.substitute(fn, ConstSpan<TypeBinding>(bind_ok, 1U));
    REQUIRE(fok.ok);
    const TypeId      ip[1]      = {i32};
    const TypeId      ir[1]      = {i32};
    CHECK(fok.type == ctx.type_callable(ConstSpan<TypeId>(ip, 1U), ConstSpan<TypeId>(ir, 1U)));

    // CONSTRAINT VIOLATION: T=f32 fails Ord -> ok=false with the offending (param, trait) reported
    const TypeBinding bind_bad[1] = {{t, f32}};
    const SubstResult bad         = ctx.substitute(gen, ConstSpan<TypeBinding>(bind_bad, 1U));
    CHECK_FALSE(bad.ok);
    CHECK(bad.failed_param == t);
    CHECK(bad.failed_trait == ord);

    // TRANSITIVE accept THROUGH substitute: T2:Eq, and i32 conforms to OrdX (OrdX:Eq) only — i32 satisfies Eq via the
    // supertrait, so binding T2=i32 is accepted (the accept side of the 3z gate, proven end-to-end not just in satisfies).
    const TypeId      eq       = ctx.type_trait("Eq", {});
    const TypeId      eqsup[1] = {eq};
    const TypeId      ordx     = ctx.type_trait("OrdX", ConstSpan<TypeId>(eqsup, 1U)); // OrdX : Eq
    const TypeId      eqc[1]   = {eq};
    const TypeId      t2       = ctx.type_param("T2", ConstSpan<TypeId>(eqc, 1U));      // T2 : Eq
    ctx.register_conformance(i32, ordx);                                               // i32 -> OrdX (not Eq directly)
    const TypeBinding bt2[1]   = {{t2, i32}};
    CHECK(ctx.substitute(t2, ConstSpan<TypeBinding>(bt2, 1U)).ok); // accepted via the transitive supertrait
    // negative stays sharp: a param needing a trait i32 does NOT conform to still violates
    const TypeId      unmet[1] = {ctx.type_trait("Unmet", {})};
    const TypeId      t3       = ctx.type_param("T3", ConstSpan<TypeId>(unmet, 1U));
    const TypeBinding bt3[1]   = {{t3, i32}};
    CHECK_FALSE(ctx.substitute(t3, ConstSpan<TypeBinding>(bt3, 1U)).ok);
}

TEST_CASE("ceir type: resource + view kinds intern, dedup, and round-trip byte-exact", "[ceir][type][resource]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32   = ctx.type_f32();
    const TypeId                 vec4f = ctx.type_vector(f32, 4U);
    const TypeId                 buf   = ctx.type_buffer(BufferMode::Plain, f32);
    const TypeId                 img   = ctx.type_image(ImageDim::Dim2D, vec4f);

    CHECK(ctx.type_buffer(BufferMode::Plain, f32) == buf);
    CHECK(ctx.type_buffer(BufferMode::Structured, f32) != buf); // mode distinguishes
    CHECK(ctx.type_buffer(BufferMode::Raw) != buf);             // raw carries no element
    CHECK(ctx.type_image(ImageDim::Dim3D, vec4f) != img);       // dim distinguishes
    CHECK(ctx.type_sampler(true) != ctx.type_sampler(false));   // comparison distinguishes

    const TypeId ts[] = {
        ctx.type_buffer(BufferMode::Raw),          buf,
        ctx.type_buffer(BufferMode::Structured, f32), ctx.type_buffer(BufferMode::Typed, f32),
        img,                                       ctx.type_image(ImageDim::Dim1D, f32),
        ctx.type_image(ImageDim::Cube, vec4f),     ctx.type_sampler(false),
        ctx.type_sampler(true),                    ctx.type_resource_table(f32),
        ctx.type_accel_struct(),                   ctx.type_video_frame(),
        ctx.type_audio_buffer(),                   ctx.type_external_resource(),
        ctx.type_view(buf, ViewRange::Byte | ViewRange::Element),
        ctx.type_view(img, ViewRange::Mip | ViewRange::Layer | ViewRange::Aspect),
    };
    Module* const m  = build_typed(ctx, ConstSpan<TypeId>(ts, static_cast<crd::usize>(sizeof(ts) / sizeof(ts[0]))));
    const String  t1 = print(ctx, *m, &root);
    CHECK(has(t1, ": !buffer<raw>"));
    CHECK(has(t1, ": !buffer<plain,!f32>"));
    CHECK(has(t1, ": !buffer<structured,!f32>"));
    CHECK(has(t1, ": !buffer<typed,!f32>"));
    CHECK(has(t1, ": !image<d2,!vec<4x!f32>>"));
    CHECK(has(t1, ": !image<d1,!f32>"));
    CHECK(has(t1, ": !image<cube,!vec<4x!f32>>"));
    CHECK(has(t1, ": !sampler<plain>"));
    CHECK(has(t1, ": !sampler<cmp>"));
    CHECK(has(t1, ": !restable<!f32>"));
    CHECK(has(t1, ": !accel"));
    CHECK(has(t1, ": !video"));
    CHECK(has(t1, ": !audio"));
    CHECK(has(t1, ": !external"));
    CHECK(has(t1, ": !view<!buffer<plain,!f32>,byte,element>"));
    CHECK(has(t1, ": !view<!image<d2,!vec<4x!f32>>,mip,layer,aspect>"));

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}

TEST_CASE("ceir type: view combinations are validated (legal accepted, illegal rejected)", "[ceir][type][resource]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, f32);
    const TypeId                 img = ctx.type_image(ImageDim::Dim2D, f32);

    CHECK(ctx.view_combination_valid(buf, ViewRange::Byte | ViewRange::Element));
    CHECK(ctx.view_combination_valid(img, ViewRange::Mip | ViewRange::Layer | ViewRange::Aspect));
    CHECK(ctx.view_combination_valid(buf, 0U)); // an empty (whole-resource) view is legal
    CHECK_FALSE(ctx.view_combination_valid(buf, static_cast<crd::u32>(ViewRange::Mip)));  // mip on a buffer
    CHECK_FALSE(ctx.view_combination_valid(img, static_cast<crd::u32>(ViewRange::Byte))); // byte on an image
    CHECK_FALSE(ctx.view_combination_valid(f32, static_cast<crd::u32>(ViewRange::Byte))); // f32 is not viewable
    CHECK_FALSE(ctx.view_combination_valid(buf, 1U << 20U));                              // an undefined range bit
    const TypeId imgview = ctx.type_view(img, static_cast<crd::u32>(ViewRange::Mip));     // a valid image view...
    CHECK_FALSE(ctx.view_combination_valid(imgview, static_cast<crd::u32>(ViewRange::Mip))); // ...is itself not viewable

    // a parser-level illegal combination is a pointing diagnostic, never an abort
    const auto rejects = [&root](const char* src) {
        Context    c(&root);
        const auto pr = parse(c, StringView(src));
        return !pr.ok && pr.module == nullptr;
    };
    CHECK(rejects("module { ^bb0: t.x() : !view<!buffer<plain,!f32>,mip> }")); // mip range on a buffer
    CHECK(rejects("module { ^bb0: t.x() : !view<!i32,byte> }"));               // view of a non-resource
}

TEST_CASE("ceir type: dims, shapes, tensors intern, dedup, and round-trip byte-exact", "[ceir][type][shape]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();
    const TypeId                 d4  = ctx.type_dim_static(4U);
    const TypeId                 dn  = ctx.type_dim_symbolic("N");
    const TypeId                 dyn = ctx.type_dim_dynamic();

    CHECK(ctx.type_dim_static(4U) == d4);
    CHECK(ctx.type_dim_static(5U) != d4);       // extent distinguishes
    CHECK(ctx.type_dim_symbolic("N") == dn);
    CHECK(ctx.type_dim_symbolic("M") != dn);    // symbolic identity is by NAME
    CHECK(ctx.type_dim_dynamic() == dyn);
    CHECK(d4 != dn);
    CHECK(dn != dyn);

    const TypeId dims[3]     = {d4, dn, dyn};
    const TypeId shp         = ctx.type_shape(ConstSpan<TypeId>(dims, 3U));
    const TypeId scalar_shape = ctx.type_shape({}); // rank-0
    const TypeId tns         = ctx.type_tensor(f32, shp);
    const TypeId stns        = ctx.type_sparse_tensor(f32, scalar_shape);
    CHECK(ctx.type_shape(ConstSpan<TypeId>(dims, 3U)) == shp); // shape dedup

    const TypeId ts[] = {d4, dn, dyn, shp, scalar_shape, tns, stns};
    Module* const m   = build_typed(ctx, ConstSpan<TypeId>(ts, static_cast<crd::usize>(sizeof(ts) / sizeof(ts[0]))));
    const String  t1  = print(ctx, *m, &root);
    CHECK(has(t1, ": !dim<4>"));
    CHECK(has(t1, ": !dim<N>"));
    CHECK(has(t1, ": !dim<dyn>"));
    CHECK(has(t1, ": !shape<!dim<4>,!dim<N>,!dim<dyn>>"));
    CHECK(has(t1, ": !shape<>")); // rank-0
    CHECK(has(t1, ": !tensor<!f32,!shape<!dim<4>,!dim<N>,!dim<dyn>>>"));
    CHECK(has(t1, ": !stensor<!f32,!shape<>>"));

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}

TEST_CASE("ceir type: shape broadcast + reshape compatibility is tri-state", "[ceir][type][shape]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const auto sh1 = [&](TypeId a) {
        const TypeId d[1] = {a};
        return ctx.type_shape(ConstSpan<TypeId>(d, 1U));
    };
    const auto sh2 = [&](TypeId a, TypeId b) {
        const TypeId d[2] = {a, b};
        return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
    };
    const TypeId d0 = ctx.type_dim_static(0U);
    const TypeId d1 = ctx.type_dim_static(1U);
    const TypeId d2 = ctx.type_dim_static(2U);
    const TypeId d3 = ctx.type_dim_static(3U);
    const TypeId d4 = ctx.type_dim_static(4U);
    const TypeId d5 = ctx.type_dim_static(5U);
    const TypeId d6 = ctx.type_dim_static(6U);
    const TypeId dn = ctx.type_dim_symbolic("N");
    const TypeId dy = ctx.type_dim_dynamic();

    // broadcast
    CHECK(ctx.shapes_broadcast(sh2(d4, d3), sh2(d1, d3)).compat == ShapeCompat::Compatible); // one is 1
    CHECK(ctx.shapes_broadcast(sh2(d4, d3), sh1(d3)).compat == ShapeCompat::Compatible);     // rank padding
    const BroadcastResult bad = ctx.shapes_broadcast(sh2(d4, d3), sh2(d5, d3));
    CHECK(bad.compat == ShapeCompat::Incompatible);
    CHECK(bad.position == 1U); // 4 vs 5 at right-aligned position 1 (position 0 = the matching innermost 3s)
    CHECK(ctx.shapes_broadcast(sh1(d0), sh1(d1)).compat == ShapeCompat::Compatible);   // 0 vs 1 -> broadcasts
    CHECK(ctx.shapes_broadcast(sh1(d0), sh1(d5)).compat == ShapeCompat::Incompatible); // 0 vs 5 -> clash
    CHECK(ctx.shapes_broadcast(sh1(dn), sh1(dn)).compat == ShapeCompat::Compatible);   // symbolic same name
    CHECK(ctx.shapes_broadcast(sh1(dn), sh1(d3)).compat == ShapeCompat::Unknown);      // symbolic vs static -> unknown
    CHECK(ctx.shapes_broadcast(sh1(dy), sh1(d3)).compat == ShapeCompat::Unknown);      // dynamic -> unknown

    // reshape
    CHECK(ctx.shapes_reshape(sh2(d2, d3), sh1(d6)) == ShapeCompat::Compatible); // 2*3 == 6
    CHECK(ctx.shapes_reshape(sh2(d4, d3), sh1(d5)) == ShapeCompat::Incompatible);           // 12 != 5
    CHECK(ctx.shapes_reshape(sh1(dn), sh1(dn)) == ShapeCompat::Compatible);                 // identical
    CHECK(ctx.shapes_reshape(sh1(dn), sh1(d4)) == ShapeCompat::Unknown);                    // symbolic -> unknown
    // overflow: three huge static dims -> the product overflows u64 -> Unknown (never a false Compatible)
    const TypeId big     = ctx.type_dim_static(0xFFFFFFFFU);
    const TypeId bigs[3] = {big, big, big};
    CHECK(ctx.shapes_reshape(ctx.type_shape(ConstSpan<TypeId>(bigs, 3U)), sh1(d1)) == ShapeCompat::Unknown);

    // composition predicate units (direct, not only through the parser arm)
    const TypeId i32     = ctx.type_i32();
    const TypeId one[1]  = {d4};
    const TypeId noti[1] = {i32};
    CHECK(ctx.shape_members_valid(ConstSpan<TypeId>(one, 1U)));
    CHECK_FALSE(ctx.shape_members_valid(ConstSpan<TypeId>(noti, 1U))); // a non-dim member
    const TypeId a_shape = sh1(d4);
    CHECK(ctx.tensor_composition_valid(i32, a_shape));
    CHECK_FALSE(ctx.tensor_composition_valid(i32, i32));         // shape arg is not a Shape
    CHECK_FALSE(ctx.tensor_composition_valid(a_shape, a_shape)); // element is a Shape
}

TEST_CASE("ceir type: substitution flows a param through a tensor shape (3b x 3d seam)", "[ceir][type][shape][generics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 ord  = ctx.type_trait("Ord", {});
    const TypeId                 con[1] = {ord};
    const TypeId                 t    = ctx.type_param("T", ConstSpan<TypeId>(con, 1U));
    const TypeId                 i32  = ctx.type_i32();
    ctx.register_conformance(i32, ord);

    const TypeId dims[1] = {ctx.type_dim_symbolic("N")};
    const TypeId shp     = ctx.type_shape(ConstSpan<TypeId>(dims, 1U));
    const TypeId genten  = ctx.type_tensor(t, shp); // tensor<param T:Ord, shape<N>>

    const TypeBinding bind[1] = {{t, i32}};
    const SubstResult r       = ctx.substitute(genten, ConstSpan<TypeBinding>(bind, 1U));
    REQUIRE(r.ok);
    CHECK(r.type == ctx.type_tensor(i32, shp)); // T -> i32, shape unchanged
    CHECK_FALSE(ctx.type_has_params(r.type));
}

TEST_CASE("ceir type: quantity dimension pack/unpack round-trips signed exponents at every base", "[ceir][type][quantity]")
{
    const auto e8 = [](int i, int v) {
        QuantityDim d;
        d.exp[i] = static_cast<crd::i8>(v);
        return d;
    };
    // a negative exponent in the TOP byte of each word (I=-2 -> count byte 3, A=-3 -> cols byte 3) — the sign-extension trap
    QuantityDim mixed;
    mixed.exp[0] = static_cast<crd::i8>(1);
    mixed.exp[3] = static_cast<crd::i8>(-2);
    mixed.exp[4] = static_cast<crd::i8>(5);
    mixed.exp[7] = static_cast<crd::i8>(-3);
    CHECK(unpack_dim(pack_dim_count(mixed), pack_dim_cols(mixed)) == mixed);
    for (int i = 0; i < 8; ++i) // every base, at the sign-boundary values
    {
        for (int v : {-1, -128, 127, 3})
        {
            const QuantityDim d = e8(i, v);
            CHECK(unpack_dim(pack_dim_count(d), pack_dim_cols(d)) == d);
        }
    }
}

TEST_CASE("ceir type: quantities intern, dedup, and round-trip byte-exact", "[ceir][type][quantity]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();

    QuantityDim len;
    len.exp[0] = static_cast<crd::i8>(1); // Length
    QuantityDim accel;
    accel.exp[0] = static_cast<crd::i8>(1);
    accel.exp[2] = static_cast<crd::i8>(-2); // L1 T-2 = acceleration
    QuantityDim mass;
    mass.exp[1] = static_cast<crd::i8>(1);
    const QuantityDim none; // dimensionless (all zero)

    const TypeId qlen = ctx.type_quantity(f32, len);
    CHECK(ctx.type_quantity(f32, len) == qlen);              // dedup
    CHECK(ctx.type_quantity(f32, mass) != qlen);             // different dimension
    CHECK(ctx.type_quantity(ctx.type_i32(), len) != qlen);   // different underlying
    CHECK(ctx.type_quantity(f32, none) != f32);            // a dimensionless quantity is DISTINCT from its raw underlying

    const TypeId     vec3f  = ctx.type_vector(f32, 3U);
    const TypeId     ord    = ctx.type_trait("Ord", {});
    const TypeId     con[1] = {ord};
    const TypeId     tparam = ctx.type_param("T", ConstSpan<TypeId>(con, 1U));
    const TypeId     ts[]   = {
        qlen, ctx.type_quantity(f32, accel), ctx.type_quantity(f32, none), ctx.type_quantity(vec3f, len),
        ctx.type_quantity(tparam, mass),
    };
    Module* const m  = build_typed(ctx, ConstSpan<TypeId>(ts, static_cast<crd::usize>(sizeof(ts) / sizeof(ts[0]))));
    const String  t1 = print(ctx, *m, &root);
    CHECK(has(t1, ": !qty<!f32,L1>"));
    CHECK(has(t1, ": !qty<!f32,L1T-2>"));
    CHECK(has(t1, ": !qty<!f32,1>")); // dimensionless
    CHECK(has(t1, ": !qty<!vec<3x!f32>,L1>"));
    CHECK(has(t1, ": !qty<!param<T,!trait<Ord>>,M1>")); // a generic quantity

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}

TEST_CASE("ceir type: quantity dimension relations + composition", "[ceir][type][quantity]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();
    QuantityDim                  len;
    len.exp[0] = static_cast<crd::i8>(1);
    QuantityDim tim;
    tim.exp[2] = static_cast<crd::i8>(1);
    const TypeId qlen = ctx.type_quantity(f32, len);
    const TypeId qtim = ctx.type_quantity(f32, tim);

    CHECK(ctx.quantity_dimensions_equal(qlen, ctx.type_quantity(f32, len)).equal);
    const DimMismatch mm = ctx.quantity_dimensions_equal(qlen, qtim); // Length vs Time
    CHECK_FALSE(mm.equal);
    CHECK(mm.first_differing_base == 0U); // Length is base 0

    const DimArith mul = quantity_dim_mul(len, tim);
    REQUIRE(mul.ok);
    CHECK(mul.dim.exp[0] == 1);
    CHECK(mul.dim.exp[2] == 1);
    const DimArith div = quantity_dim_div(len, tim); // L1 T-1 = velocity
    REQUIRE(div.ok);
    CHECK(div.dim.exp[0] == 1);
    CHECK(div.dim.exp[2] == -1);
    QuantityDim big; // exponent-arithmetic overflow -> failure, never wraparound
    big.exp[0] = static_cast<crd::i8>(100);
    CHECK_FALSE(quantity_dim_mul(big, big).ok); // 200 > 127

    // composition: numeric underlying accepted; quantity/dim/shape/struct rejected
    CHECK(ctx.quantity_composition_valid(f32));
    CHECK(ctx.quantity_composition_valid(ctx.type_vector(f32, 3U)));
    CHECK(ctx.quantity_composition_valid(ctx.type_i32()));
    CHECK_FALSE(ctx.quantity_composition_valid(qlen));                    // no quantity-of-quantity
    CHECK_FALSE(ctx.quantity_composition_valid(ctx.type_dim_static(4U))); // not a value type
    const StringView fns[1]  = {StringView("x")};
    const TypeId     ftys[1] = {f32};
    CHECK_FALSE(ctx.quantity_composition_valid(
        ctx.type_struct("S", ConstSpan<TypeId>(ftys, 1U), ConstSpan<StringView>(fns, 1U)))); // an aggregate
}

TEST_CASE("ceir type: ownership qualifiers intern, dedup, and round-trip byte-exact (all nine kinds)",
          "[ceir][type][qualifier]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();

    // dedup: same (kind, underlying) -> one id; a different kind OR underlying -> a distinct id.
    const TypeId imm_f32 = ctx.type_qualified(OwnershipKind::ImmutableValue, f32);
    CHECK(ctx.type_qualified(OwnershipKind::ImmutableValue, f32) == imm_f32);            // dedup
    CHECK(ctx.type_qualified(OwnershipKind::MutableValue, f32) != imm_f32);              // different ownership kind
    CHECK(ctx.type_qualified(OwnershipKind::ImmutableValue, ctx.type_i32()) != imm_f32); // different underlying
    CHECK(imm_f32 != f32); // a qualified type is DISTINCT from its bare underlying (!qual<imm,!f32> != !f32 — intentional)

    // ALL NINE keywords must print + parse back — the printer/parser keyword tables carry NO -Werror=switch guard, so a
    // dropped or transposed entry only surfaces as a broken round-trip here.
    const OwnershipKind kinds[9] = {
        OwnershipKind::ImmutableValue, OwnershipKind::MutableValue,   OwnershipKind::BorrowedView,
        OwnershipKind::OwnedResource,  OwnershipKind::SharedHandle,   OwnershipKind::WeakHandle,
        OwnershipKind::StateSlot,      OwnershipKind::ExternalHandle,  OwnershipKind::TransientArena,
    };
    // 9 keyword carriers over f32 + 2 nested (a resource underlying and an aggregate) to exercise recursion.
    TypeId ts[11];
    for (int i = 0; i < 9; ++i) { ts[i] = ctx.type_qualified(kinds[i], f32); }
    ts[9]                 = ctx.type_qualified(OwnershipKind::BorrowedView, ctx.type_buffer(BufferMode::Plain, f32));
    const TypeId tup2[2]  = {f32, ctx.type_i32()};
    ts[10]                = ctx.type_qualified(OwnershipKind::OwnedResource, ctx.type_tuple(ConstSpan<TypeId>(tup2, 2U)));

    Module* const m  = build_typed(ctx, ConstSpan<TypeId>(ts, 11U));
    const String  t1 = print(ctx, *m, &root);
    CHECK(has(t1, ": !qual<imm,!f32>"));
    CHECK(has(t1, ": !qual<mut,!f32>"));
    CHECK(has(t1, ": !qual<borrow,!f32>"));
    CHECK(has(t1, ": !qual<own,!f32>"));
    CHECK(has(t1, ": !qual<shared,!f32>"));
    CHECK(has(t1, ": !qual<weak,!f32>"));
    CHECK(has(t1, ": !qual<state,!f32>"));
    CHECK(has(t1, ": !qual<ext,!f32>"));
    CHECK(has(t1, ": !qual<transient,!f32>"));
    CHECK(has(t1, ": !qual<borrow,!buffer<plain,!f32>>"));    // a borrowed RESOURCE
    CHECK(has(t1, ": !qual<own,!tuple<!f32,!i32>>"));         // an owned AGGREGATE

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}

TEST_CASE("ceir type: ownership-qualifier composition accepts value+resource types, rejects meta types",
          "[ceir][type][qualifier]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 f32 = ctx.type_f32();

    // ACCEPTED: numerics, aggregates, generics, AND resource handles (a borrowed buffer/image is the whole point of §19).
    CHECK(ctx.qualified_composition_valid(f32));
    CHECK(ctx.qualified_composition_valid(ctx.type_vector(f32, 3U)));
    CHECK(ctx.qualified_composition_valid(ctx.type_buffer(BufferMode::Plain, f32)));
    CHECK(ctx.qualified_composition_valid(ctx.type_image(ImageDim::Dim2D, f32)));
    const TypeId ord    = ctx.type_trait("Ord", {});
    const TypeId con[1] = {ord};
    CHECK(ctx.qualified_composition_valid(ctx.type_param("T", ConstSpan<TypeId>(con, 1U)))); // a generic can be qualified

    // REJECTED: meta-types that are not runtime values — a qualifier over a qualifier / dim / shape / trait is nonsense.
    CHECK_FALSE(ctx.qualified_composition_valid(ctx.type_qualified(OwnershipKind::ImmutableValue, f32))); // no qual-of-qual
    CHECK_FALSE(ctx.qualified_composition_valid(ctx.type_dim_static(4U)));
    const TypeId dims[1] = {ctx.type_dim_symbolic("N")};
    CHECK_FALSE(ctx.qualified_composition_valid(ctx.type_shape(ConstSpan<TypeId>(dims, 1U))));
    CHECK_FALSE(ctx.qualified_composition_valid(ord)); // a trait is a constraint, not a value
}

TEST_CASE("ceir type: value_escapes_region tracks uses across the region tree (3f escape predicate)",
          "[ceir][type][qualifier]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 i32 = ctx.type_i32();

    Module* const m   = ctx.create_module();
    Region* const r0  = m->body();
    Block* const  top = ctx.create_block(0U);
    r0->append(top);

    // an op carrying a nested region R1; R1's block has an arg A -> A is DEFINED inside R1.
    Operation* const rop = ctx.create_operation(ctx.intern_op("scf", "region"), {}, 0U, {}, 1U);
    top->append(rop);
    Region* const r1    = rop->region(0);
    Block* const  inner = ctx.create_block(1U, i32);
    r1->append(inner);
    Value* const a = inner->arg(0U);

    CHECK_FALSE(ctx.value_escapes_region(a, r1)); // no uses yet -> nothing escapes

    Value* const     use_in[1] = {a};
    Operation* const inner_use = ctx.create_operation(ctx.intern_op("test", "use"), ConstSpan<Value*>(use_in, 1U), 0U);
    inner->append(inner_use);
    CHECK_FALSE(ctx.value_escapes_region(a, r1)); // the sole use is INSIDE R1 -> contained, no escape

    Operation* const outer_use = ctx.create_operation(ctx.intern_op("test", "use"), ConstSpan<Value*>(use_in, 1U), 0U);
    top->append(outer_use);
    CHECK(ctx.value_escapes_region(a, r1)); // a use now sits in R0 (not contained by R1) -> A escapes R1

    // containment is DIRECTIONAL: relative to the OUTER region R0 both uses live within R0's subtree (R1 is a child of
    // R0), so nothing escapes R0 — a borrow captured into a child scope is fine; only leaking OUT of the defining scope is not.
    CHECK_FALSE(ctx.value_escapes_region(a, r0));
}

TEST_CASE("ceir type: substitution flows a param through an ownership qualifier (3b x 3f seam)",
          "[ceir][type][qualifier][generics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 ord    = ctx.type_trait("Ord", {});
    const TypeId                 con[1] = {ord};
    const TypeId                 t      = ctx.type_param("T", ConstSpan<TypeId>(con, 1U));
    const TypeId                 i32    = ctx.type_i32();
    ctx.register_conformance(i32, ord);

    const TypeId genq = ctx.type_qualified(OwnershipKind::BorrowedView, t); // qual<borrow, param T:Ord>
    CHECK(ctx.type_has_params(genq));

    const TypeBinding bind[1] = {{t, i32}};
    const SubstResult r       = ctx.substitute(genq, ConstSpan<TypeBinding>(bind, 1U));
    REQUIRE(r.ok);
    CHECK(r.type == ctx.type_qualified(OwnershipKind::BorrowedView, i32)); // T -> i32, the ownership kind is PRESERVED
    CHECK_FALSE(ctx.type_has_params(r.type));
}

TEST_CASE("ceir type: substitution rejects a binding that breaks composition (a fourth construction path)",
          "[ceir][type][qualifier][generics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 t   = ctx.type_param("T", {}); // no constraints -> any concrete binds
    const TypeId                 f32 = ctx.type_f32();

    // qual<borrow,T> with T -> qual<imm,f32> would intern a qualifier-OVER-a-qualifier: structurally canonical, so the
    // intern assert would pass, yet it prints-but-wont-reparse (the decoder re-checks composition). substitute must reject.
    const TypeId      genq   = ctx.type_qualified(OwnershipKind::BorrowedView, t);
    const TypeId      badq   = ctx.type_qualified(OwnershipKind::ImmutableValue, f32);
    const TypeBinding bq[1]  = {{t, badq}};
    const SubstResult rq     = ctx.substitute(genq, ConstSpan<TypeBinding>(bq, 1U));
    CHECK_FALSE(rq.ok);
    CHECK(rq.failed_compose.valid()); // a pointing diagnostic, not a bare failure

    // qty<T,L1> with T -> qty<f32,T1> would make a quantity-OF-a-quantity. (Retroactively closes the same 3e/3b hole that
    // this 3f composition audit uncovered — the fix lives in subst_rec, shared by all four kinds.)
    QuantityDim len;
    len.exp[0] = static_cast<crd::i8>(1);
    QuantityDim tim;
    tim.exp[2]               = static_cast<crd::i8>(1);
    const TypeId      genqty = ctx.type_quantity(t, len);
    const TypeId      badqty = ctx.type_quantity(f32, tim);
    const TypeBinding bt[1]  = {{t, badqty}};
    const SubstResult rt     = ctx.substitute(genqty, ConstSpan<TypeBinding>(bt, 1U));
    CHECK_FALSE(rt.ok);
    CHECK(rt.failed_compose.valid());

    // a VALID binding through the same generic shapes still succeeds — no false rejection.
    const TypeBinding ok[1] = {{t, f32}};
    CHECK(ctx.substitute(genq, ConstSpan<TypeBinding>(ok, 1U)).ok);
    CHECK(ctx.substitute(genqty, ConstSpan<TypeBinding>(ok, 1U)).ok);
}

TEST_CASE("ceir type: value_escapes_region walks a multi-level region nest (3f escape depth)",
          "[ceir][type][qualifier]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 i32 = ctx.type_i32();

    // R0 (module body) > op0.R1 > op1.R2 ; a value A is defined DEEP in R2 and used one level up in R1.
    Module* const m  = ctx.create_module();
    Region* const r0 = m->body();
    Block* const  b0 = ctx.create_block(0U);
    r0->append(b0);

    Operation* const op0 = ctx.create_operation(ctx.intern_op("scf", "region"), {}, 0U, {}, 1U);
    b0->append(op0);
    Region* const r1 = op0->region(0);
    Block* const  b1 = ctx.create_block(0U);
    r1->append(b1);

    Operation* const op1 = ctx.create_operation(ctx.intern_op("scf", "region"), {}, 0U, {}, 1U);
    b1->append(op1);
    Region* const r2 = op1->region(0);
    Block* const  b2 = ctx.create_block(1U, i32);
    r2->append(b2);
    Value* const a = b2->arg(0U);

    Value* const     use[1] = {a};
    Operation* const u1     = ctx.create_operation(ctx.intern_op("test", "use"), ConstSpan<Value*>(use, 1U), 0U);
    b1->append(u1); // A used in R1 — one level ABOVE its defining R2

    CHECK(ctx.value_escapes_region(a, r2));       // used above R2 -> escapes its defining region
    CHECK_FALSE(ctx.value_escapes_region(a, r1)); // R1 directly contains the use -> no escape
    CHECK_FALSE(ctx.value_escapes_region(a, r0)); // R0 contains the whole nest (2-hop walk) -> no escape
}

TEST_CASE("ceir type: well-formedness rejects structurally-invalid records", "[ceir][type]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const TypeId                 i32   = ctx.type_i32();
    const TypeId                 m1[1] = {i32};
    const TypeId                 m2[2] = {i32, i32};

    CHECK(type_is_well_formed(ctx.type_of(i32)));
    CHECK(type_is_well_formed(ctx.type_of(ctx.type_vector(i32, 4U))));

    Type bad_vec = Type::scalar(TypeKind::Vector); // needs exactly 1 member, has 0
    CHECK_FALSE(type_is_well_formed(bad_vec));

    Type bad_result = Type::scalar(TypeKind::Result); // needs 2 members
    bad_result.members = ConstSpan<TypeId>(m1, 1U);
    CHECK_FALSE(type_is_well_formed(bad_result));

    Type bad_struct = Type::scalar(TypeKind::Struct); // 1 member, 0 labels -> label/member parity broken
    bad_struct.members = ConstSpan<TypeId>(m1, 1U);
    CHECK_FALSE(type_is_well_formed(bad_struct));

    Type bad_callable  = Type::scalar(TypeKind::Callable); // count exceeds members -> would index OOB
    bad_callable.count = 3U;
    bad_callable.members = ConstSpan<TypeId>(m2, 2U);
    CHECK_FALSE(type_is_well_formed(bad_callable));

    Type ok_callable  = Type::scalar(TypeKind::Callable); // 1 param + 1 result, count 1 <= 2 members
    ok_callable.count = 1U;
    ok_callable.members = ConstSpan<TypeId>(m2, 2U);
    CHECK(type_is_well_formed(ok_callable));

    Type bad_view = Type::scalar(TypeKind::View); // a view needs exactly 1 member (the underlying resource)
    CHECK_FALSE(type_is_well_formed(bad_view));

    Type bad_raw = Type::scalar(TypeKind::Buffer); // count defaults to 0 = Raw; a raw buffer must have NO element
    bad_raw.members = ConstSpan<TypeId>(m1, 1U);
    CHECK_FALSE(type_is_well_formed(bad_raw));

    CHECK(type_is_well_formed(Type::scalar(TypeKind::AccelStruct))); // niladic resource kinds are well-formed empty
    CHECK(type_is_well_formed(Type::scalar(TypeKind::Buffer)));      // a raw buffer (empty) is well-formed

    Type bad_tensor = Type::scalar(TypeKind::Tensor); // a tensor needs exactly 2 members (element + shape)
    bad_tensor.members = ConstSpan<TypeId>(m1, 1U);
    CHECK_FALSE(type_is_well_formed(bad_tensor));
    CHECK(type_is_well_formed(Type::scalar(TypeKind::Dim)));  // a dim carries no member types
    CHECK(type_is_well_formed(Type::scalar(TypeKind::Shape))); // a rank-0 shape is well-formed
}

TEST_CASE("ceir type: deeply nested aggregates round-trip byte-exact", "[ceir][type]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    // option<result<vec<2x!f32>, array<3x!i32>>> - nesting through several kinds
    const TypeId inner  = ctx.type_result(ctx.type_vector(ctx.type_f32(), 2U), ctx.type_array(ctx.type_i32(), 3U));
    const TypeId nested = ctx.type_option(inner);
    const TypeId one[1] = {nested};

    Module* const m  = build_typed(ctx, ConstSpan<TypeId>(one, 1U));
    const String  t1 = print(ctx, *m, &root);
    CHECK(has(t1, ": !option<!result<!vec<2x!f32>,!array<3x!i32>>>"));

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const String t2 = print(ctx2, *pr.module, &root);
    REQUIRE(t1.size() == t2.size());
    CHECK(std::memcmp(t1.data(), t2.data(), t1.size()) == 0);
}
