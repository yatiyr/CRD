// CEIR-10z — the BAND-10 GATE (§172 matrix, host subset): the asset lifecycle (10a hot-reload) and the execution-plan
// cache (10b) COMPOSED and proven LIVE together, in ONE program graph, with hit-count-delta assertions across the three
// edit classes. Unlike the 9z doc-synthesis gate, band 10's guarantees only meet HERE — the PlanCache validates against a
// resolver backed by the LIVE ReloadSet, so a reload decision and a cache verdict are checked in lockstep:
//   (1) body-edit of callee B  → HotSwap installs live  AND caller A's plan stays a HIT (§107 at the plan layer, LIVE);
//   (2) signature-edit of B     → ContractChange REJECT + last-good  AND A's plan still HIT (reject keeps the cache coherent);
//   (3) dep-edit (cold-reload B) → B's interface changes → A's plan StaleDeps; recompile set = affected(B) ∪ {B}.
// C (independent) stays a HIT throughout. Host subset: cross-program calls do NOT execute — decisions + cache verdicts only.
// ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/plan_cache.hpp>
#include <crd/ceir/cook/program_cook.hpp> // cook_program
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility> // std::move

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u64;
using crd::u8;

namespace
{
struct Reg
{
    OpId cst;
    explicit Reg(Context& c) : cst(c.intern_op("arith", "const"))
    {
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
        (void)func::register_dialect(c);
    }
};
void registrar(Context& c, void* /*user*/)
{
    (void)arith::register_arith_ops(c);
    (void)core::register_core_ops(c);
    (void)func::register_dialect(c);
}
Operation* konst(Context& c, const Reg& r, Block* b, i64 v, TypeId ty)
{
    Operation* const op = c.create_operation(r.cst, {}, 1U, ty);
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}
void ret(Context& c, Block* b, Value* v)
{
    Value* ops[1] = {v};
    b->append(func::create_return(c, ConstSpan<Value*>(ops, 1U)));
}
void module_block(Context& c, Module& m)
{
    if (m.body()->first_block() == nullptr) { m.body()->append(c.create_block(0U)); }
}
ConstSpan<u8> span_of(const Array<u8>& b) { return ConstSpan<u8>(b.data(), b.size()); }

// a leaf @fname() -> (i32|i64) { return k } — `k` toggles the BODY, `i64_result` toggles the SIGNATURE.
Array<u8> cook_leaf(crd::memory::MallocAllocator& root, u64 asset_id, StringView fname, i64 k, bool i64_result)
{
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    module_block(c, *m);
    const TypeId ty = i64_result ? c.type_i64() : c.type_i32();
    Operation* const f = func::create_func(c, *m, fname, Visibility::Public, 0U, ty);
    m->body()->first_block()->append(f);
    ret(c, func::func_body_block(f), konst(c, r, func::func_body_block(f), k, ty)->result(0U));
    CookResult cr = cook_program(c, *m, asset_id, &root, &root);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}
// a caller @fname() -> i32 { call callee(); return 0 } — imports `callee` (a dependency edge).
Array<u8> cook_caller(crd::memory::MallocAllocator& root, u64 asset_id, StringView fname, StringView callee)
{
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    module_block(c, *m);
    Operation* const f = func::create_func(c, *m, fname, Visibility::Public, 0U, c.type_i32());
    m->body()->first_block()->append(f);
    Block* const b = func::func_body_block(f);
    b->append(func::create_call(c, callee, {}, 0U, {}));
    ret(c, b, konst(c, r, b, 0, c.type_i32())->result(0U));
    CookResult cr = cook_program(c, *m, asset_id, &root, &root);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}
// the PlanCache resolver = the LIVE ReloadSet's current interface hash (0 if absent — EMPTY≠UNKNOWN).
u64 live_resolve(AssetId a, void* user)
{
    const RuntimeProgram* const p = static_cast<ReloadSet*>(user)->program(a);
    return p != nullptr ? p->interface_hash : 0U;
}
// cache `owner`'s plan (opaque 1-byte artifact) at its current content, recording `deps`.
void cache_plan(PlanCache& cache, ReloadSet& set, u64 target, AssetId owner, ConstSpan<PlanDep> deps)
{
    const u8      art[1] = {0xABU};
    const PlanKey key{set.program(owner)->content_hash, target, 1U};
    cache.put(owner, key, ConstSpan<u8>(art, 1U), deps);
}
// look up `owner`'s plan at its CURRENT content (a self-change would miss by construction — a new key).
PlanStatus plan_of(PlanCache& cache, ReloadSet& set, u64 target, AssetId owner)
{
    return cache.get(PlanKey{set.program(owner)->content_hash, target, 1U}).status;
}
} // namespace

TEST_CASE("ceir 10z: BAND-10 GATE - lifecycle + plan cache compose live across the three edit classes", "[ceir][gate10]")
{
    crd::memory::MallocAllocator root;
    ReloadSet                    set(&root, &registrar, nullptr);
    const AssetId                id_a{1U};   // A — caller: imports "bar"
    const AssetId                id_b{2U};   // B — callee: exports bar()
    const AssetId                id_c{3U};   // C — independent
    REQUIRE(set.add(id_a, span_of(cook_caller(root, 1U, "usea", "bar"))).ok());
    REQUIRE(set.add(id_b, span_of(cook_leaf(root, 2U, "bar", 0, false))).ok());
    REQUIRE(set.add(id_c, span_of(cook_leaf(root, 3U, "cee", 0, false))).ok());

    // the recompile PREDICTION: a change to B affects exactly {A}.
    Array<AssetId> aff(&root);
    REQUIRE(set.affected(id_b, aff));
    REQUIRE(aff.size() == 1U);
    CHECK(aff[0] == id_a);

    PlanCache cache(&root, &live_resolve, &set);
    const u64 tgt = plan_target(StringView("host"));
    // cache all three plans; A records a dep on B at B's CURRENT interface.
    const PlanDep dep_a[1] = {{id_b, set.program(id_b)->interface_hash}};
    cache_plan(cache, set, tgt, id_a, ConstSpan<PlanDep>(dep_a, 1U));
    cache_plan(cache, set, tgt, id_b, ConstSpan<PlanDep>());
    cache_plan(cache, set, tgt, id_c, ConstSpan<PlanDep>());
    REQUIRE(plan_of(cache, set, tgt, id_a) == PlanStatus::Hit);
    REQUIRE(plan_of(cache, set, tgt, id_b) == PlanStatus::Hit);
    REQUIRE(plan_of(cache, set, tgt, id_c) == PlanStatus::Hit);
    // ⛔ EXACT counters (the 9a "not ≥" discipline): the 3 baseline gets are all hits.
    CHECK(cache.hits() == 3U);
    CHECK(cache.misses() == 0U);

    // ── (1) BODY-edit B → HotSwap installs live; B's interface is UNCHANGED → A's plan STILL HITS (§107, live) ──
    const ReloadResult r1 = set.reload(id_b, span_of(cook_leaf(root, 2U, "bar", 42, false))); // body const 0→42
    CHECK(r1.decision == ReloadDecision::HotSwap);
    CHECK(r1.installed);
    CHECK(plan_of(cache, set, tgt, id_a) == PlanStatus::Hit); // A's recorded dep on B still validates (B iface unchanged)
    CHECK(plan_of(cache, set, tgt, id_c) == PlanStatus::Hit); // C untouched
    // B's OWN plan is now a MISS BY CONSTRUCTION (B's new body = new content = a new key); B recompiles + re-caches.
    CHECK(plan_of(cache, set, tgt, id_b) == PlanStatus::Miss);
    cache_plan(cache, set, tgt, id_b, ConstSpan<PlanDep>());
    CHECK(plan_of(cache, set, tgt, id_b) == PlanStatus::Hit);
    CHECK(cache.hits() == 6U);   // +A, +C, +B(re-cached) = 3 live-validated hits
    CHECK(cache.misses() == 1U); // +B self-miss (new content = new key)

    // ── (2) SIGNATURE-edit B → ContractChange REJECT + last-good; B's LIVE interface UNCHANGED → A's plan STILL HITS ──
    const crd::u64 b_iface = set.program(id_b)->interface_hash;
    const ReloadResult r2 = set.reload(id_b, span_of(cook_leaf(root, 2U, "bar", 0, /*i64_result=*/true))); // bar()->i64
    CHECK(r2.decision == ReloadDecision::ContractChange);
    CHECK_FALSE(r2.installed);
    CHECK(set.program(id_b)->interface_hash == b_iface);       // last-good: B's live interface is unchanged
    CHECK(plan_of(cache, set, tgt, id_a) == PlanStatus::Hit);  // ...so A's plan stays valid
    CHECK(plan_of(cache, set, tgt, id_b) == PlanStatus::Hit);
    CHECK(cache.hits() == 8U);   // +A, +B — a REJECT leaves the cache fully valid (nothing drifted)
    CHECK(cache.misses() == 1U); // unchanged — a rejected reload invalidates nothing

    // ── (3) DEP-edit: COLD-reload B with a new signature → B's interface CHANGES → A's plan StaleDeps (recompile A) ──
    set.remove(id_b);
    REQUIRE(set.add(id_b, span_of(cook_leaf(root, 2U, "bar", 0, /*i64_result=*/true))).ok()); // bar()->i64 : a new interface
    CHECK(plan_of(cache, set, tgt, id_a) == PlanStatus::StaleDeps); // A ∈ affected(B) — its recorded dep drifted
    CHECK(plan_of(cache, set, tgt, id_b) == PlanStatus::Miss);      // ∪ {B}: B's new content = new key = a self-miss
    CHECK(plan_of(cache, set, tgt, id_c) == PlanStatus::Hit);       // C ∉ affected(B) — untouched throughout
    // ⛔ EXACT final counters: the recompile set is A (StaleDeps) ∪ {B} (Miss); C stayed a HIT. The cache never trusted
    // itself — every hit was re-validated against the LIVE resolver, so the deltas ARE the §172 "cache-hit counts".
    CHECK(cache.hits() == 9U);   // +C only (A stale, B miss)
    CHECK(cache.misses() == 3U); // 1 (B body self-miss) + A StaleDeps + B cold-reload self-miss
}
