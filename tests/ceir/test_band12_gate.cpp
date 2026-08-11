// CEIR-12z BAND-12 GATE (Resource/memory subsystem). ⛔ A COMPOSING gate, not a re-run of slice tests: ONE curated
// frame-graph-shaped module carries EVERY band axis at once -- resource.declare/view/import/export (12a) with the §20/§24/§25
// planning-intent attrs (12b), a WAR-lifetime resource used LATE through a VIEW + differently-sized transients (the 12c
// scars), mixed lifetime classes (transient/persistent/history) + an exported resource -- and the band's guarantees are
// then composed IN SEQUENCE on that same module: the type verifier (12a) + the intent verifier (12b) are clean; the
// live-range analysis (12c) extends the WAR resource to its LAST use (through the view chain); the planner (12d) pools the
// three disjoint same-bucket transients into ONE slot (transient_physical < transient_logical -- the REN-1 "aliasing saves
// memory" proof, IR edition) while the WAR resource, the differently-sized transient, and the persistent/history/exported
// resources each keep their own slot; the Latency profile proves aliasing is what saved the memory (physical == logical);
// every co-slotted pair is provably resources_may_alias; and a serialize->deserialize twin re-plans to the SAME shape (the
// intent attrs + View-typed results survive, the plan is reproducible cross-context). The band exit criterion in
// assertions. Host-only. ASCII test names.

#include <crd/ceir/binary.hpp>
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
constexpr EffectRecord kReadOp0[] = {{EffectFamily::MemoryRead, EffectTarget::Operand, 0U, 0U}};

struct Kit
{
    OpId cst, decl, view, imp, exp, read;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), decl(ctx.intern_op("resource", "declare")),
          view(ctx.intern_op("resource", "view")), imp(ctx.intern_op("resource", "import")),
          exp(ctx.intern_op("resource", "export")), read(ctx.intern_op("u", "read"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        Dialect* const d = ctx.register_dialect("u");
        (void)d->register_op("read", {.effects = ConstSpan<EffectRecord>(kReadOp0, 1U)});
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
Operation* decl_res(Context& ctx, const Kit& k, Block* b, const char* lifetime, i64 size_class, TypeId ty)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ty);
    if (lifetime != nullptr) { ctx.set_attr(d, "lifetime", ctx.attr_string(containers::StringView(lifetime))); }
    if (size_class != 0) { ctx.set_attr(d, "size_class", ctx.attr_int(size_class)); }
    b->append(d);
    return d;
}
void read_into(Context& ctx, const Kit& k, Block* b, Value* v)
{
    Value* ops[1] = {v};
    b->append(ctx.create_operation(k.read, ConstSpan<Value*>(ops, 1U), 0U));
}
Operation* konst(Context& ctx, const Kit& k, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
const SlotAssignment* assign_of(const MemoryPlan& p, const Operation* d)
{
    for (usize i = 0; i < p.assignments.size(); ++i)
    {
        if (p.assignments[i].resource == d->result(0U)) { return &p.assignments[i]; }
    }
    return nullptr;
}
const ResourceLifetime* lt_of(const Array<ResourceLifetime>& lts, const Operation* d)
{
    for (usize i = 0; i < lts.size(); ++i)
    {
        if (lts[i].resource == d->result(0U)) { return &lts[i]; }
    }
    return nullptr;
}
// Build the composed band-12 module into `bm`; return the declare ops by out-param for the assertions.
struct Res
{
    Operation *war, *a, *b, *c, *big, *hist, *persist, *out;
};
Res build_band12(Context& ctx, const Kit& k, Block* bm, TypeId buf1, TypeId buf9)
{
    // pos  op                                     lifetime effect
    // ----------------------------------------------------------------------------------------------------------------
    //  0   %war  = declare[transient, sz1]         war: [17,19] (FIRST-use is the view op @17; last @19 -- 15d-3b first-use + view-chain-lifetime scar)
    //  1   %a    = declare[transient, sz1]
    //  2   read(%a)                                a:   [1,2]
    //  3   %b    = declare[transient, sz1]
    //  4   read(%b)                                b:   [3,4]
    //  5   %c    = declare[transient, sz1]
    //  6   read(%c)                                c:   [5,6]   (%a,%b,%c disjoint, same sz1 bucket -> ONE slot)
    //  7   %big  = declare[transient, sz9]
    //  8   read(%big)                              big: [7,8]   (different bucket -> its own slot -- slot-SIZE scar)
    //  9   %hist = declare[history, sz1] (hl=2)
    // 10   read(%hist)                             hist:[9,10]  (dedicated ring, depth 2)
    // 11   %persist = declare[persistent, sz1]
    // 12   read(%persist)                          persist:[11,12] (dedicated)
    // 13   %out  = declare[transient, sz1]
    // 14   export(%out)                            out: exported -> pinned to block-end (dedicated)
    // 15   %off  = arith.const 0 : index
    // 16   %sz   = arith.const 16 : index
    // 17   %v    = view(%war, %off, %sz) : view(buf, Byte)
    // 18   read(%v)                                war extended to 18 (through the view->root chain)
    // 19   read(%war)                              war extended to 19 (direct)
    // 20   %ext = import : external_resource        EXTERNALLY-owned -> the planner must NEVER plan it (12a contract)
    // 21   return
    Res r{};
    r.war = decl_res(ctx, k, bm, "transient", 1, buf1);
    r.a   = decl_res(ctx, k, bm, "transient", 1, buf1);
    read_into(ctx, k, bm, r.a->result(0U));
    r.b = decl_res(ctx, k, bm, "transient", 1, buf1);
    read_into(ctx, k, bm, r.b->result(0U));
    r.c = decl_res(ctx, k, bm, "transient", 1, buf1);
    read_into(ctx, k, bm, r.c->result(0U));
    r.big = decl_res(ctx, k, bm, "transient", 9, buf9);
    read_into(ctx, k, bm, r.big->result(0U));
    r.hist = decl_res(ctx, k, bm, "history", 1, buf1);
    ctx.set_attr(r.hist, "history_length", ctx.attr_int(2));
    read_into(ctx, k, bm, r.hist->result(0U));
    r.persist = decl_res(ctx, k, bm, "persistent", 1, buf1);
    read_into(ctx, k, bm, r.persist->result(0U));
    r.out = decl_res(ctx, k, bm, "transient", 1, buf1);
    Value* eo[1] = {r.out->result(0U)};
    bm->append(ctx.create_operation(k.exp, ConstSpan<Value*>(eo, 1U), 0U));
    Operation* const off = konst(ctx, k, bm, 0);
    Operation* const sz  = konst(ctx, k, bm, 16);
    Value*           vo[3] = {r.war->result(0U), off->result(0U), sz->result(0U)};
    const TypeId     vty   = ctx.type_view(buf1, static_cast<u32>(ViewRange::Byte));
    Operation* const v     = ctx.create_operation(k.view, ConstSpan<Value*>(vo, 3U), 1U, vty);
    bm->append(v);
    read_into(ctx, k, bm, v->result(0U));       // use %war THROUGH the view
    read_into(ctx, k, bm, r.war->result(0U));   // ...and directly
    bm->append(ctx.create_operation(k.imp, {}, 1U, ctx.type_external_resource())); // externally-owned: NEVER planned
    bm->append(func::create_return(ctx, {}));
    return r;
}
} // namespace

TEST_CASE("ceir 12z band gate: the resource->memory pipeline composes and aliasing provably saves memory", "[ceir][band12]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf1 = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    const TypeId                 buf9 = ctx.type_buffer(BufferMode::Structured, ctx.type_f32()); // a DIFFERENT buffer type
    Module* const                m    = ctx.create_module();
    Block* const                 bm   = mkmain(ctx, *m);
    const Res                    r    = build_band12(ctx, k, bm, buf1, buf9);

    // 12a + 12b: the type contract and the planning-intent vocabulary are both clean on the composed module.
    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    REQUIRE(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::None);
    REQUIRE(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::None);

    // 12c: the WAR resource's live range is [FIRST-use 17, LAST-use 19] — `last` reached through the VIEW chain + a direct
    // read (NOT collapsed to the declaration); `first` is its first ACCESS (the view op @17), NOT the declare position (0).
    // ⛔ 15d-3b: memory-liveness = [first-use, last-use] (the render-graph aliaser's model, frame_graph.cpp), so %war is
    // DISJOINT in time from the early short transients and pools with them — the memory gain the old declare-pos model lost.
    Array<ResourceLifetime> lts(&root);
    ctx.compute_block_lifetimes(*bm, lts);
    REQUIRE(lts.size() == 8U);
    CHECK(lt_of(lts, r.war)->first == 17U);  // ⛔ 15d-3b: FIRST-use (the view op @17), not the declare position (0)
    CHECK(lt_of(lts, r.war)->last == 19U);   // ⭐ view-chain-lifetime scar (would be 0 on declaration order)
    CHECK(lt_of(lts, r.a)->last == 2U);
    CHECK(lt_of(lts, r.c)->last == 6U);

    // 12d: the planner pools the FOUR disjoint sz1 transients {%a,%b,%c,%war} into ONE slot (15d-3b: %war's memory-live
    // range [17,19] is disjoint from the early [2..6] ones); %big (different bucket) + the dedicated resources keep their own.
    MemoryPlan plan(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, plan);
    CHECK(plan.assignments.size() == 8U);    // ⭐ the resource.import got NO assignment (the planner never plans imports)
    CHECK(plan.transient_logical == 5U);     // %war,%a,%b,%c,%big are poolable-eligible
    CHECK(plan.transient_physical == 2U);    // ⛔ 15d-3b: 5 logical -> 2 physical ({%a,%b,%c,%war} collapse + %big); was 3 under the over-conservative declare-pos `first`
    CHECK(plan.transient_physical < plan.transient_logical);
    // the {%a,%b,%c} pool + their §162 reasons.
    CHECK(assign_of(plan, r.a)->slot == assign_of(plan, r.b)->slot);
    CHECK(assign_of(plan, r.b)->slot == assign_of(plan, r.c)->slot);
    CHECK(assign_of(plan, r.a)->reason == SlotReason::NewPoolSlot);
    CHECK(assign_of(plan, r.b)->reason == SlotReason::Pooled);
    CHECK(assign_of(plan, r.b)->prior == r.a->result(0U));
    CHECK(assign_of(plan, r.c)->reason == SlotReason::Pooled);
    CHECK(assign_of(plan, r.c)->prior == r.b->result(0U));
    // 15d-3b: %war's memory-live range [17,19] is DISJOINT from the early transients, so it CORRECTLY pools into their slot
    // (the first-use gain; the old declare-pos `first`=0 forced war=[0,19] and blocked this). The differently-SIZED transient
    // still never shares (the slot-SIZE scar is intact).
    CHECK(assign_of(plan, r.war)->slot == assign_of(plan, r.a)->slot); // ⛔ 15d-3b: war [17,19] disjoint from a [2,2] -> POOLED (was != under declare-pos)
    CHECK(assign_of(plan, r.big)->slot != assign_of(plan, r.a)->slot); // ⭐ slot-SIZE: different bucket -> own slot
    // the dedicated resources + the §162 ring depth.
    CHECK(assign_of(plan, r.hist)->reason == SlotReason::DedicatedLifetime);
    CHECK(plan.slots[assign_of(plan, r.hist)->slot].history_length == 2);
    CHECK(plan.slots[assign_of(plan, r.hist)->slot].dedicated);
    CHECK(assign_of(plan, r.persist)->reason == SlotReason::DedicatedLifetime);
    CHECK(assign_of(plan, r.out)->reason == SlotReason::DedicatedExported);
    CHECK(assign_of(plan, r.out)->slot != assign_of(plan, r.war)->slot);

    // the consistency invariant: every co-slotted pair is provably aliasable.
    for (usize i = 0; i < plan.assignments.size(); ++i)
    {
        for (usize j = i + 1; j < plan.assignments.size(); ++j)
        {
            if (plan.assignments[i].slot != plan.assignments[j].slot) { continue; }
            const ResourceLifetime* li = nullptr;
            const ResourceLifetime* lj = nullptr;
            for (usize t = 0; t < lts.size(); ++t)
            {
                if (lts[t].resource == plan.assignments[i].resource) { li = &lts[t]; }
                if (lts[t].resource == plan.assignments[j].resource) { lj = &lts[t]; }
            }
            REQUIRE(li != nullptr);
            REQUIRE(lj != nullptr);
            CHECK(Context::resources_may_alias(*li, *lj)); // ⭐ co-slotted => provably aliasable
        }
    }

    // the Latency cross-check: with pooling DISABLED, physical == logical -- aliasing is what saved the memory.
    MemoryPlan lat(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Latency, lat);
    CHECK(lat.transient_logical == 5U);
    CHECK(lat.transient_physical == 5U); // ⭐ no aliasing -> the un-collapsed baseline

    // the round-trip twin: the intent attrs + View-typed results survive, and the plan is REPRODUCIBLE cross-context.
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                     ctx2(&root);
    const Kit                   k2(ctx2);
    (void)k2;
    const ParseResult dr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    CHECK(ctx2.find_resource_misuse(*dr.module).kind == ResourceMisuseKind::None);
    CHECK(ctx2.find_resource_intent_misuse(*dr.module).kind == ResourceIntentMisuseKind::None);
    Block* const twin_body = func::func_body_block(dr.module->body()->first_block()->first_op());
    MemoryPlan   tplan(&root);
    ctx2.plan_block_memory(*twin_body, PlanProfile::Memory, tplan);
    CHECK(tplan.transient_logical == 5U);  // ⭐ lifetime + size_class attrs survived (else these would collapse to 0)
    CHECK(tplan.transient_physical == 2U); // ⛔ 15d-3b: the same aliasing shape (2 physical) reproduced in a fresh Context
    // the full plan SHAPE reproduces (stronger than the counts): the twin is declaration-ordered too, so compare by INDEX.
    // ⛔ `prior` is a Value* into ctx2 -- compare only its NULL-ness (Pooled vs not), never the cross-context pointer.
    REQUIRE(tplan.assignments.size() == plan.assignments.size());
    for (usize i = 0; i < plan.assignments.size(); ++i)
    {
        CHECK(tplan.assignments[i].slot == plan.assignments[i].slot);
        CHECK(tplan.assignments[i].reason == plan.assignments[i].reason);
        CHECK((tplan.assignments[i].prior == nullptr) == (plan.assignments[i].prior == nullptr));
    }
}
