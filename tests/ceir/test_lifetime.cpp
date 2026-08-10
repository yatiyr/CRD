// CEIR-12c §78/§26 — the resource ALIAS/LIFETIME analysis: Context::compute_block_lifetimes computes per-resource live
// ranges over a block's op order (following resource.view chains + nested-region uses + the "over 4d effects" ambient
// rule + the exported pin), and resources_interfere / resources_may_alias are the predicates the CEIR-12d planner colors.
// The two frame-graph scars become IR-level ctests here: the WAR-lifetime scar (a live range extends to the LAST use, not
// declaration order) and the slot-SIZE scar (two transients pool only within one (size_class, kind) bucket). ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/effect.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{
// hand-registered effectful ops (the test_hazard.cpp pattern): `read` reads its operand (a resource USE); `gwrite` is an
// AMBIENT memory write (EffectTarget::None -> resolves to a null resource, the whole Memory class); `scope` is a plain
// region-bearing op with NO effects (so it never triggers the ambient rule -- only its NESTED uses matter).
constexpr EffectRecord kReadOp0[] = {{EffectFamily::MemoryRead, EffectTarget::Operand, 0U, 0U}};
constexpr EffectRecord kGWrite[]  = {{EffectFamily::MemoryWrite, EffectTarget::None, 0U, 0U}};

struct Kit
{
    OpId cst, decl, view, imp, exp, read, gwrite, scope;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), decl(ctx.intern_op("resource", "declare")),
          view(ctx.intern_op("resource", "view")), imp(ctx.intern_op("resource", "import")),
          exp(ctx.intern_op("resource", "export")), read(ctx.intern_op("u", "read")),
          gwrite(ctx.intern_op("u", "gwrite")), scope(ctx.intern_op("u", "scope"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        Dialect* const d = ctx.register_dialect("u");
        (void)d->register_op("read", {.effects = ConstSpan<EffectRecord>(kReadOp0, 1U)});
        (void)d->register_op("gwrite", {.effects = ConstSpan<EffectRecord>(kGWrite, 1U)});
        (void)d->register_op("scope", {}); // registered + effect-free (NOT unregistered -> no synthetic Universe hazard)
    }
};
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m.body()->append(top);
    }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
// a graph-owned resource of `ty` with an optional §20 lifetime class + §78 size_class (0 = leave the attr absent).
Operation* decl_res(Context& ctx, const Kit& k, Block* b, const char* lifetime, i64 size_class, TypeId ty)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ty);
    if (lifetime != nullptr) { ctx.set_attr(d, "lifetime", ctx.attr_string(containers::StringView(lifetime))); }
    if (size_class != 0) { ctx.set_attr(d, "size_class", ctx.attr_int(size_class)); }
    b->append(d);
    return d;
}
Operation* read_into(Context& ctx, const Kit& k, Block* b, Value* v)
{
    Value* ops[1] = {v};
    Operation* const r = ctx.create_operation(k.read, ConstSpan<Value*>(ops, 1U), 0U);
    b->append(r);
    return r;
}
// find the live range of the resource declared by op `d` in `lts` (declaration order; nullptr if not present).
const ResourceLifetime* find_lt(const Array<ResourceLifetime>& lts, const Operation* d)
{
    for (usize i = 0; i < lts.size(); ++i)
    {
        if (lts[i].declare == d) { return &lts[i]; }
    }
    return nullptr;
}
} // namespace

TEST_CASE("ceir 12c: transient live ranges alias when disjoint and the WAR scar blocks overlap", "[ceir][lifetime]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    // @main(){ %a=declare[transient,sz1]; read(%a); %b=declare[transient,sz1]; read(%b); read(%a) [LATE]; return }
    // %a's range extends to the LATE read (the WAR-lifetime scar: last-USE, not declaration order); %b sits inside it.
    Operation* const a = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
    read_into(ctx, k, bm, a->result(0U));                           // pos 1
    Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf); // pos 2
    read_into(ctx, k, bm, b->result(0U));                           // pos 3
    read_into(ctx, k, bm, a->result(0U));                           // pos 4  <- extends %a to [0,4]
    bm->append(func::create_return(ctx, {}));

    Array<ResourceLifetime> lts(&root);
    ctx.compute_block_lifetimes(*bm, lts);
    REQUIRE(lts.size() == 2U);
    const ResourceLifetime* la = find_lt(lts, a);
    const ResourceLifetime* lb = find_lt(lts, b);
    REQUIRE(la != nullptr);
    REQUIRE(lb != nullptr);
    CHECK(la->first == 0U);
    CHECK(la->last == 4U);                                   // ⭐ WAR scar: last-USE, not decl order (would be 0)
    CHECK(lb->first == 2U);
    CHECK(lb->last == 3U);
    CHECK(Context::resources_interfere(*la, *lb));           // [0,4] overlaps [2,3]
    CHECK_FALSE(Context::resources_may_alias(*la, *lb));     // ⭐ so they must NOT share a slot

    // positive control: a THIRD transient declared+used AFTER %a's range ends would alias %a -- build it fresh to keep
    // positions clean. %a=[0,1], %c=[2,3] disjoint, same (kind, size_class) -> may_alias TRUE (12z needs a true somewhere).
    Module* const m2 = ctx.create_module();
    Block* const  b2 = mkmain(ctx, *m2);
    Operation* const a2 = decl_res(ctx, k, b2, "transient", 1, buf); // pos 0
    read_into(ctx, k, b2, a2->result(0U));                          // pos 1
    Operation* const c2 = decl_res(ctx, k, b2, "transient", 1, buf); // pos 2
    read_into(ctx, k, b2, c2->result(0U));                          // pos 3
    b2->append(func::create_return(ctx, {}));
    Array<ResourceLifetime> lts2(&root);
    ctx.compute_block_lifetimes(*b2, lts2);
    REQUIRE(lts2.size() == 2U);
    CHECK_FALSE(Context::resources_interfere(lts2[0], lts2[1])); // [0,1] vs [2,3]
    CHECK(Context::resources_may_alias(lts2[0], lts2[1]));       // ⭐ the aliasable positive control
}

TEST_CASE("ceir 12c: lifetime class gates aliasing (persistent, history, unspecified never pool)", "[ceir][lifetime]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());

    struct Case
    {
        const char* la;
        const char* lb;
        bool        alias;
    };
    // disjoint ranges + same (kind, size_class); only transient+transient may pool.
    const Case cases[] = {{"transient", "transient", true}, {"persistent", "transient", false},
                          {"history", "transient", false}, {nullptr, "transient", false}, // unspecified (no lifetime attr)
                          {"transient", "persistent", false}};
    for (const Case& c : cases)
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, c.la, 1, buf); // pos 0
        read_into(ctx, k, bm, a->result(0U));                     // pos 1
        Operation* const b = decl_res(ctx, k, bm, c.lb, 1, buf);  // pos 2
        read_into(ctx, k, bm, b->result(0U));                     // pos 3
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        CHECK_FALSE(Context::resources_interfere(lts[0], lts[1]));    // disjoint always
        CHECK(Context::resources_may_alias(lts[0], lts[1]) == c.alias);
    }
}

TEST_CASE("ceir 12c: slot-size, exported, and the over-4d-effects ambient rule", "[ceir][lifetime]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    const TypeId                 img = ctx.type_image(ImageDim::Dim2D, ctx.type_f32());

    // (1) slot-SIZE scar: two transients, disjoint ranges, DIFFERENT size_class -> may NOT alias; SAME size_class -> may.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf);
        read_into(ctx, k, bm, a->result(0U));
        Operation* const b = decl_res(ctx, k, bm, "transient", 2, buf); // size_class 2 != 1
        read_into(ctx, k, bm, b->result(0U));
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        CHECK_FALSE(Context::resources_interfere(lts[0], lts[1]));
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1])); // ⭐ slot-size scar: different bucket
    }
    // (1b) a DIFFERENT KIND (buffer vs image) never pools, even disjoint + same size_class.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf);
        read_into(ctx, k, bm, a->result(0U));
        Operation* const b = decl_res(ctx, k, bm, "transient", 1, img); // an image, not a buffer
        read_into(ctx, k, bm, b->result(0U));
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1]));
    }
    // (2) EXPORTED resource: never aliasable + its range pins to block-END (external code may touch it past any position).
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
        Value* eops[1] = {a->result(0U)};
        bm->append(ctx.create_operation(k.exp, ConstSpan<Value*>(eops, 1U), 0U)); // pos 1: export(%a)
        Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf);           // pos 2
        read_into(ctx, k, bm, b->result(0U));                                     // pos 3
        Operation* const ret = func::create_return(ctx, {});                      // pos 4 (block end)
        bm->append(ret);
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        const ResourceLifetime* la = find_lt(lts, a);
        REQUIRE(la != nullptr);
        CHECK(la->exported);
        CHECK(la->last == 4U);                                    // ⭐ pinned to block-end
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1])); // an exported resource never pools
    }
    // (3) the OVER-4D-EFFECTS ambient rule: a gwrite (ambient Memory write) between two otherwise-disjoint transients
    //     extends the earlier one's range across it, forcing interference. Without the rule they would falsely alias.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
        read_into(ctx, k, bm, a->result(0U));                           // pos 1  (real last-use of %a)
        Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf);  // pos 2
        bm->append(ctx.create_operation(k.gwrite, {}, 0U));             // pos 3: AMBIENT memory write
        read_into(ctx, k, bm, b->result(0U));                           // pos 4
        bm->append(func::create_return(ctx, {}));                       // pos 5
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        const ResourceLifetime* la = find_lt(lts, a);
        const ResourceLifetime* lb = find_lt(lts, b);
        REQUIRE(la != nullptr);
        REQUIRE(lb != nullptr);
        CHECK(la->last == 3U);                                // ⭐ the ambient gwrite extended %a from 1 to 3
        CHECK(Context::resources_interfere(*la, *lb));        // [0,3] now overlaps [2,4]
        CHECK_FALSE(Context::resources_may_alias(*la, *lb));  // ⭐ the ambient touch blocks the alias
    }
    // (3b) an UNREGISTERED op is maximally effectful (synthetic Universe rw) -> it triggers the ambient rule too.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
        read_into(ctx, k, bm, a->result(0U));                           // pos 1
        Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf);  // pos 2
        bm->append(ctx.create_operation(ctx.intern_op("z", "unreg"), {}, 0U)); // pos 3: interned, NEVER registered
        read_into(ctx, k, bm, b->result(0U));                           // pos 4
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        CHECK(find_lt(lts, a)->last == 3U);                          // ⭐ the unregistered op extended %a from 1 to 3
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1]));
    }
    // (4) UNSPECIFIED size_class (0) never pools -- same reasoning as unspecified lifetime (a wrong alias is a bug).
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 0, buf); // size_class attr ABSENT
        read_into(ctx, k, bm, a->result(0U));
        Operation* const b = decl_res(ctx, k, bm, "transient", 0, buf);
        read_into(ctx, k, bm, b->result(0U));
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        CHECK(lts[0].size_class == 0);
        CHECK_FALSE(Context::resources_interfere(lts[0], lts[1]));   // disjoint...
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1]));   // ⭐ ...but unspecified size refuses to pool
    }
}

TEST_CASE("ceir 12c: nested-region and view-chain uses extend the root resource's live range", "[ceir][lifetime]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());

    // (1) NESTED-region use: %a is used ONLY inside a `scope` op's region; its range must extend to the CONTAINING op's
    //     position (the "pass"), not stay stuck at the declaration.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
        Operation* const sc = ctx.create_operation(k.scope, {}, 0U, {}, 1U); // pos 1: a region-bearing op
        Block* const     inner = ctx.create_block(0U);
        sc->region(0U)->append(inner);
        read_into(ctx, k, inner, a->result(0U)); // the ONLY use of %a -- inside the nested region
        bm->append(sc);
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 1U);
        CHECK(lts[0].first == 0U);
        CHECK(lts[0].last == 1U); // ⭐ extended to the scope op's position (would be 0 if nested uses were missed)
    }
    // (1b) NESTED export: a resource.export INSIDE a region must still mark the root exported (wrapping never weakens).
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf);     // pos 0
        Operation* const sc = ctx.create_operation(k.scope, {}, 0U, {}, 1U); // pos 1
        Block* const     inner = ctx.create_block(0U);
        sc->region(0U)->append(inner);
        Value* eops[1] = {a->result(0U)};
        inner->append(ctx.create_operation(k.exp, ConstSpan<Value*>(eops, 1U), 0U)); // export(%a) INSIDE the region
        bm->append(sc);
        Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf); // pos 2
        read_into(ctx, k, bm, b->result(0U));                           // pos 3
        bm->append(func::create_return(ctx, {}));                       // pos 4
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        const ResourceLifetime* la = find_lt(lts, a);
        REQUIRE(la != nullptr);
        CHECK(la->exported);                                       // ⭐ the NESTED export was seen
        CHECK(la->last == 4U);                                     // pinned to block-end
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1])); // an exported resource never pools
    }
    // (1c) NESTED ambient: a gwrite INSIDE a region triggers the ambient rule at the containing op's position.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
        read_into(ctx, k, bm, a->result(0U));                           // pos 1 (real last-use)
        Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf);  // pos 2
        Operation* const sc = ctx.create_operation(k.scope, {}, 0U, {}, 1U); // pos 3
        Block* const     inner = ctx.create_block(0U);
        sc->region(0U)->append(inner);
        inner->append(ctx.create_operation(k.gwrite, {}, 0U)); // an AMBIENT write INSIDE the region
        bm->append(sc);
        read_into(ctx, k, bm, b->result(0U));                  // pos 4
        bm->append(func::create_return(ctx, {}));              // pos 5
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        CHECK(find_lt(lts, a)->last == 3U);                        // ⭐ the NESTED gwrite extended %a from 1 to 3
        CHECK_FALSE(Context::resources_may_alias(lts[0], lts[1])); // -> interference -> no alias
    }
    // (2) VIEW-CHAIN use: a use of view(%a) is a use of %a -- the root's range follows the view.
    {
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const a   = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
        Operation* const off = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
        ctx.set_attr(off, "value", ctx.attr_int(0));
        bm->append(off); // pos 1
        Operation* const sz = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
        ctx.set_attr(sz, "value", ctx.attr_int(16));
        bm->append(sz); // pos 2
        Value* vops[3] = {a->result(0U), off->result(0U), sz->result(0U)};
        const TypeId     vty = ctx.type_view(buf, static_cast<u32>(ViewRange::Byte));
        Operation* const v   = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 3U), 1U, vty);
        bm->append(v);                        // pos 3: %v = view(%a)
        read_into(ctx, k, bm, v->result(0U)); // pos 4: read(%v) -- a use of %a through the view
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 1U);
        CHECK(lts[0].first == 0U);
        CHECK(lts[0].last == 4U); // ⭐ extended by the read of the VIEW (pos 4), via the view->root map
    }
    // (3) resource.import is EXCLUDED from the analysis (it is never planned).
    {
        Module* const m  = ctx.create_module();
        Block* const  bm = mkmain(ctx, *m);
        bm->append(ctx.create_operation(k.imp, {}, 1U, ctx.type_external_resource()));
        bm->append(func::create_return(ctx, {}));
        Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        CHECK(lts.empty()); // an import produced no planned resource
    }
}
