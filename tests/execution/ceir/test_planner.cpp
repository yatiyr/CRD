// CEIR-12d §78/§162 — the memory PLANNER: Context::plan_block_memory colors 12c's live ranges into physical slots (the
// frame graph's greedy interval-coloring, ported). Proves: the REN-1 "aliasing saves memory" claim (transient_physical <
// transient_logical) IR-edition; the plan<->predicate consistency invariant (every co-slotted pair satisfies
// resources_may_alias); the §78 Memory-vs-Latency profiles; and the §162 inspectability (per-assignment SlotReason). ASCII.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/effect.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

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
    OpId decl, exp, read;
    explicit Kit(Context& ctx)
        : decl(ctx.intern_op("resource", "declare")), exp(ctx.intern_op("resource", "export")),
          read(ctx.intern_op("u", "read"))
    {
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
// the assignment for the resource produced by declare `d` (assignments are in declaration order; matched by SSA value).
const SlotAssignment* assign_of(const MemoryPlan& p, const Operation* d)
{
    for (usize i = 0; i < p.assignments.size(); ++i)
    {
        if (p.assignments[i].resource == d->result(0U)) { return &p.assignments[i]; }
    }
    return nullptr;
}
} // namespace

TEST_CASE("ceir 12d: aliasing saves memory -- transient_physical < transient_logical (the REN-1 proof, IR edition)",
          "[ceir][planner]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    // %a=[0,1], %b=[2,3]: two disjoint transients in the same bucket -> ONE physical slot for TWO logical resources.
    Operation* const a = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
    read_into(ctx, k, bm, a->result(0U));                           // pos 1
    Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf); // pos 2
    read_into(ctx, k, bm, b->result(0U));                           // pos 3
    bm->append(func::create_return(ctx, {}));

    MemoryPlan plan(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, plan);
    CHECK(plan.transient_logical == 2U);
    CHECK(plan.transient_physical == 1U);                 // ⭐ aliasing collapsed 2 logical -> 1 physical
    CHECK(plan.transient_physical < plan.transient_logical);
    const SlotAssignment* aa = assign_of(plan, a);
    const SlotAssignment* ab = assign_of(plan, b);
    REQUIRE(aa != nullptr);
    REQUIRE(ab != nullptr);
    CHECK(aa->reason == SlotReason::NewPoolSlot); // %a opened the slot
    CHECK(ab->reason == SlotReason::Pooled);      // %b reused it
    CHECK(ab->prior == a->result(0U));            // §162: %b shares %a's slot
    CHECK(aa->slot == ab->slot);
}

TEST_CASE("ceir 12d: interval-coloring is minimal and the plan is consistent with resources_may_alias", "[ceir][planner]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    // ⛔ 15d-3b (first-use memory-liveness): a transient's slot is FREE until its FIRST access, so a resource only-read-LATE
    // would not span. %a is therefore touched EARLY (@1) and LATE (@6) so it genuinely SPANS -> %a=[1,6], %b=[3,3], %c=[5,5].
    // %a overlaps both (2 slots minimum); %c reuses %b's slot (disjoint) -> 2 slots. (This mirrors a real write-then-read
    // resource, and matches the render-graph aliaser's first-touch `first_use` model, frame_graph.cpp.)
    Operation* const a = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0
    read_into(ctx, k, bm, a->result(0U));                           // pos 1 -> %a first-touched @1
    Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf); // pos 2
    read_into(ctx, k, bm, b->result(0U));                           // pos 3 -> %b=[3,3]
    Operation* const c = decl_res(ctx, k, bm, "transient", 1, buf); // pos 4
    read_into(ctx, k, bm, c->result(0U));                           // pos 5 -> %c=[5,5]
    read_into(ctx, k, bm, a->result(0U));                           // pos 6 -> %a=[1,6] spans everything
    bm->append(func::create_return(ctx, {}));

    MemoryPlan plan(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, plan);
    CHECK(plan.transient_logical == 3U);
    CHECK(plan.transient_physical == 2U);            // ⭐ exactly 2 -- the minimal coloring (max concurrent = 2)
    const SlotAssignment* ac = assign_of(plan, c);
    REQUIRE(ac != nullptr);
    CHECK(ac->reason == SlotReason::Pooled);
    CHECK(ac->prior == b->result(0U));               // %c reused %b's slot, not %a's (%a is still live)
    CHECK(assign_of(plan, a)->slot != assign_of(plan, b)->slot); // %a and %b overlap -> distinct slots

    // consistency invariant: any two resources sharing a slot must satisfy resources_may_alias.
    Array<ResourceLifetime> lts(&root);
    ctx.compute_block_lifetimes(*bm, lts);
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
}

// ⛔ CEIR-15d-3b: memory-liveness is [FIRST-use, last-use], NOT [declare, last-use]. A transient DECLARED early but
// first-USED late is free until then, so it pools with a transient that finished before it — the EXACT frame-graph shape
// (the converter emits every resource.declare up-front). Under the old declare-pos `first`, both ranges started at ~0 and
// overlapped -> zero pooling. This is parity with the render-graph aliaser's `first_use` model (frame_graph.cpp L1200).
TEST_CASE("ceir 15d-3b: a declared-early first-used-late transient pools with an early-and-done one", "[ceir][planner]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    // Both declared UP-FRONT (positions 0,1 — the frame-converter shape). %late is first-USED @3; %early is done @2.
    Operation* const late  = decl_res(ctx, k, bm, "transient", 1, buf); // pos 0 (declared FIRST, used LAST)
    Operation* const early = decl_res(ctx, k, bm, "transient", 1, buf); // pos 1
    read_into(ctx, k, bm, early->result(0U));                           // pos 2 -> %early=[2,2]
    read_into(ctx, k, bm, late->result(0U));                            // pos 3 -> %late=[3,3] (NOT [0,3])
    bm->append(func::create_return(ctx, {}));

    MemoryPlan plan(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, plan);
    CHECK(plan.transient_logical == 2U);
    CHECK(plan.transient_physical == 1U); // ⛔ THE 15d-3b GAIN: 2 logical -> 1 physical (declare-pos `first`=0 would tie both to ~0 -> 2)
    const SlotAssignment* al = assign_of(plan, late);
    const SlotAssignment* ae = assign_of(plan, early);
    REQUIRE(al != nullptr);
    REQUIRE(ae != nullptr);
    CHECK(al->slot == ae->slot);          // pooled into the SAME physical slot

    // the lifetimes prove WHY: %late's memory-live range starts at its first ACCESS @3, disjoint from %early=[2,2].
    Array<ResourceLifetime> lts(&root);
    ctx.compute_block_lifetimes(*bm, lts);
    const ResourceLifetime* ll = nullptr;
    const ResourceLifetime* le = nullptr;
    for (usize t = 0; t < lts.size(); ++t)
    {
        if (lts[t].resource == late->result(0U)) { ll = &lts[t]; }
        if (lts[t].resource == early->result(0U)) { le = &lts[t]; }
    }
    REQUIRE(ll != nullptr);
    REQUIRE(le != nullptr);
    CHECK(ll->first == 3U);               // ⛔ first-USE @3, NOT the declare position (0) — the whole point of 15d-3b
    CHECK(le->last == 2U);
    CHECK_FALSE(Context::resources_interfere(*ll, *le)); // [3,3] disjoint from [2,2]

    // the DEGENERATE: a transient DECLARED but NEVER used collapses to a well-formed point interval [declare, declare].
    Module* const           m2  = ctx.create_module();
    Block* const            bm2 = mkmain(ctx, *m2);
    Operation* const        unused = decl_res(ctx, k, bm2, "transient", 1, buf); // pos 0, never touched
    (void)unused;
    bm2->append(func::create_return(ctx, {}));                                   // pos 1
    Array<ResourceLifetime> lts2(&root);
    ctx.compute_block_lifetimes(*bm2, lts2);
    REQUIRE(lts2.size() == 1U);
    CHECK(lts2[0].first == 0U);           // ⛔ 15d-3b degenerate: unused -> `first` falls back to the declare position (0)
    CHECK(lts2[0].last == 0U);
}

TEST_CASE("ceir 12d: the Latency profile disables pooling (physical == logical == the Memory plan's logical)",
          "[ceir][planner]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    Operation* const a = decl_res(ctx, k, bm, "transient", 1, buf); // %a=[1,1] (first-use @1)
    read_into(ctx, k, bm, a->result(0U));
    Operation* const b = decl_res(ctx, k, bm, "transient", 1, buf); // %b=[3,3] (first-use @3; disjoint -> would pool under Memory)
    read_into(ctx, k, bm, b->result(0U));
    bm->append(func::create_return(ctx, {}));

    MemoryPlan mem(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, mem);
    MemoryPlan lat(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Latency, lat);
    CHECK(lat.transient_physical == lat.transient_logical);   // ⭐ Latency never pools
    CHECK(lat.transient_physical == mem.transient_logical);   // ...so it equals the un-aliased baseline
    CHECK(lat.transient_physical == 2U);
    CHECK(assign_of(lat, a)->reason == SlotReason::DedicatedProfile);
    CHECK(assign_of(lat, b)->reason == SlotReason::DedicatedProfile);
    CHECK(assign_of(lat, a)->slot != assign_of(lat, b)->slot); // distinct dedicated slots
}

TEST_CASE("ceir 12d: every dedicated SlotReason and the history ring depth are recorded (sec 162)", "[ceir][planner]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    // one resource per DEDICATED reason (Memory profile -- so a plain transient would POOL, isolating the negatives).
    Operation* const pers = decl_res(ctx, k, bm, "persistent", 1, buf); // -> DedicatedLifetime
    read_into(ctx, k, bm, pers->result(0U));
    Operation* const hist = decl_res(ctx, k, bm, "history", 1, buf);    // -> DedicatedLifetime + ring depth
    ctx.set_attr(hist, "history_length", ctx.attr_int(2));
    read_into(ctx, k, bm, hist->result(0U));
    Operation* const hist1 = decl_res(ctx, k, bm, "history", 1, buf);   // history with NO history_length -> defaults to 1
    read_into(ctx, k, bm, hist1->result(0U));
    Operation* const exp = decl_res(ctx, k, bm, "transient", 1, buf);   // exported -> DedicatedExported
    Value* eo[1] = {exp->result(0U)};
    bm->append(ctx.create_operation(k.exp, ConstSpan<Value*>(eo, 1U), 0U));
    Operation* const unsz = decl_res(ctx, k, bm, "transient", 0, buf);  // size_class 0 -> DedicatedUnsized
    read_into(ctx, k, bm, unsz->result(0U));
    bm->append(func::create_return(ctx, {}));

    MemoryPlan plan(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, plan);
    CHECK(assign_of(plan, pers)->reason == SlotReason::DedicatedLifetime);
    CHECK(assign_of(plan, hist)->reason == SlotReason::DedicatedLifetime);
    CHECK(assign_of(plan, exp)->reason == SlotReason::DedicatedExported);
    CHECK(assign_of(plan, unsz)->reason == SlotReason::DedicatedUnsized);
    // §162: the history<T> ring depth is recorded on its slot (a depth-2 ring is 2x the memory).
    const SlotAssignment* ah = assign_of(plan, hist);
    REQUIRE(ah != nullptr);
    CHECK(plan.slots[ah->slot].history_length == 2);
    CHECK(plan.slots[ah->slot].dedicated);
    const SlotAssignment* ah1 = assign_of(plan, hist1);
    REQUIRE(ah1 != nullptr);
    CHECK(plan.slots[ah1->slot].history_length == 1); // ⭐ 12b: a history with no explicit depth defaults to 1 (TAA)
    // none of these are poolable-eligible, so the transient counters stay zero.
    CHECK(plan.transient_logical == 0U);
    CHECK(plan.transient_physical == 0U);
}

TEST_CASE("ceir 12d: different size_class buckets never share; the plan is deterministic across runs", "[ceir][planner]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    // disjoint ranges but DIFFERENT size_class -> two buckets -> two slots (the slot-SIZE scar, planner edition).
    Operation* const a = decl_res(ctx, k, bm, "transient", 1, buf); // %a=[0,1] bucket 1
    read_into(ctx, k, bm, a->result(0U));
    Operation* const b = decl_res(ctx, k, bm, "transient", 2, buf); // %b=[2,3] bucket 2
    read_into(ctx, k, bm, b->result(0U));
    bm->append(func::create_return(ctx, {}));

    MemoryPlan p1(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, p1);
    CHECK(p1.transient_logical == 2U);
    CHECK(p1.transient_physical == 2U);                             // ⭐ different buckets -> no pooling
    CHECK(assign_of(p1, a)->slot != assign_of(p1, b)->slot);

    // determinism: planning the same block again yields byte-identical assignments (pins the Deterministic profile too).
    MemoryPlan p2(&root);
    ctx.plan_block_memory(*bm, PlanProfile::Memory, p2);
    REQUIRE(p1.assignments.size() == p2.assignments.size());
    for (usize i = 0; i < p1.assignments.size(); ++i)
    {
        CHECK(p1.assignments[i].resource == p2.assignments[i].resource);
        CHECK(p1.assignments[i].slot == p2.assignments[i].slot);
        CHECK(p1.assignments[i].reason == p2.assignments[i].reason);
    }
    CHECK(plan_profile_name(PlanProfile::Memory) == containers::StringView("memory"));
    CHECK(slot_reason_name(SlotReason::Pooled) == containers::StringView("pooled"));

    // ADR-0124: Balanced + Deterministic RIDE Memory this slice -> byte-identical assignments (pins the "ride" claim).
    const PlanProfile riders[] = {PlanProfile::Balanced, PlanProfile::Deterministic};
    for (const PlanProfile pr : riders)
    {
        MemoryPlan pp(&root);
        ctx.plan_block_memory(*bm, pr, pp);
        REQUIRE(pp.assignments.size() == p1.assignments.size());
        for (usize i = 0; i < p1.assignments.size(); ++i)
        {
            CHECK(pp.assignments[i].slot == p1.assignments[i].slot);
            CHECK(pp.assignments[i].reason == p1.assignments[i].reason);
        }
    }
}
