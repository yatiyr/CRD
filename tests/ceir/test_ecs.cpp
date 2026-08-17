// CEIR-9f (UNIVERSALITY VALIDATION, U6 Game/ECS effects, U-§88/U-§41). ⛔ A PROOF, not a feature: ECS system
// parallelism runs on the foundation with ZERO new machinery — the 4d hazard analysis (`ops_hazard` /
// `collect_block_hazards`) over 4a `EcsRead`/`EcsWrite` effects IS the parallelism oracle. ⭐ Per-component identity is
// the SSA VALUE (the canonical 4d resource model): each component is a distinct Value (a block arg = a component
// storage), a system's effect targets an OPERAND, so two systems touching DISJOINT components do not conflict (parallel)
// while a shared component with >=1 write is ordered — the compiler infers legal parallelism from DECLARED EFFECTS
// ALONE. A STRUCTURAL mutation (spawn) is a whole-store write (a null resource) that conflicts with everything — the
// barrier, by the existing nullptr conflict rule, no special mechanism. ⛔ The proof exercises the EcsRead/EcsWrite
// FAMILIES + Value identity; the 8c `EcsComponent` location KIND resolves whole-class today (per-instance location
// identity is named-forward IN the 4d source) — that coarse fallback is safe-but-pessimal (MORE hazards, never fewer),
// and is pinned as the fifth check. The mock `ecs` dialect is INLINE-registered (zero central edits). Host-only. ASCII.

#include <crd/ceir/ceir.hpp>   // umbrella: context/ir/dialect
#include <crd/ceir/effect.hpp> // EffectRecord / EffectFamily / EffectTarget
#include <crd/ceir/hazard.hpp> // HazardKind
#include <crd/ceir/semantics.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::u32;
using crd::usize;

namespace
{
// A system's effect targets a specific OPERAND (a component storage Value) -> per-component resource identity (the 4d
// model). `spawn` writes target=None (the whole ECS store, a null resource) -> the structural barrier. `coarse` targets
// the 8c `EcsComponent` location KIND, which resolves whole-class today -> the conservative fallback.
struct Systems
{
    OpId movement, render, physics, input, spawn, coarse;
};
OpId reg(Dialect* d, const char* name, ConstSpan<EffectRecord> effects)
{
    return d->register_op(name, OpSpec{.effects     = effects,
                                       .determinism = DeterminismClass::BitExact,
                                       .domain      = EvalDomain::HostSimulationTime});
}
Systems register_ecs(Context& ctx)
{
    Dialect* const     d = ctx.register_dialect("ecs");
    // component slots by operand index: 0 = Position, 1 = Velocity, 2 = Health, 3 = Sprite (per system's operand order).
    const EffectRecord mv[2] = {EffectRecord{EffectFamily::EcsWrite, EffectTarget::Operand, 0U, 0U},   // writes Position
                                EffectRecord{EffectFamily::EcsRead, EffectTarget::Operand, 1U, 0U}};    // reads Velocity
    const EffectRecord rn[2] = {EffectRecord{EffectFamily::EcsRead, EffectTarget::Operand, 0U, 0U},     // reads Position
                                EffectRecord{EffectFamily::EcsRead, EffectTarget::Operand, 1U, 0U}};    // reads Sprite
    const EffectRecord ph[1] = {EffectRecord{EffectFamily::EcsWrite, EffectTarget::Operand, 0U, 0U}};   // writes Velocity
    const EffectRecord in[1] = {EffectRecord{EffectFamily::EcsWrite, EffectTarget::Operand, 0U, 0U}};   // writes Health
    const EffectRecord sp[1] = {EffectRecord{EffectFamily::EcsWrite, EffectTarget::None, 0U, 0U}};       // whole store
    const EffectRecord co[1] = {EffectRecord{EffectFamily::EcsWrite, EffectTarget::EcsComponent, 0U, 0U}}; // whole-class fallback
    Systems s{};
    s.movement = reg(d, "movement", ConstSpan<EffectRecord>(mv, 2U));
    s.render   = reg(d, "render", ConstSpan<EffectRecord>(rn, 2U));
    s.physics  = reg(d, "physics", ConstSpan<EffectRecord>(ph, 1U));
    s.input    = reg(d, "input", ConstSpan<EffectRecord>(in, 1U));
    s.spawn    = reg(d, "spawn", ConstSpan<EffectRecord>(sp, 1U));
    s.coarse   = reg(d, "coarse", ConstSpan<EffectRecord>(co, 1U));
    return s;
}

// A "world" block whose 4 args are the component storages (Position=arg0, Velocity=arg1, Health=arg2, Sprite=arg3), plus
// the system ops wired to the components they touch. Returns the block; fills `ops` with {movement, render, physics,
// input, spawn} in that ORDER (the collect_block_hazards list order).
Block* build_world(Context& ctx, const Systems& s, Operation** ops)
{
    Block* const b = ctx.create_block(4U, ctx.type_i64());
    Value* const pos = b->arg(0U);
    Value* const vel = b->arg(1U);
    Value* const hp  = b->arg(2U);
    Value* const spr = b->arg(3U);
    Value* const mv_in[2] = {pos, vel};
    Value* const rn_in[2] = {pos, spr};
    Value* const ph_in[1] = {vel};
    Value* const in_in[1] = {hp};
    ops[0] = ctx.create_operation(s.movement, ConstSpan<Value*>(mv_in, 2U), 0U);
    ops[1] = ctx.create_operation(s.render, ConstSpan<Value*>(rn_in, 2U), 0U);
    ops[2] = ctx.create_operation(s.physics, ConstSpan<Value*>(ph_in, 1U), 0U);
    ops[3] = ctx.create_operation(s.input, ConstSpan<Value*>(in_in, 1U), 0U);
    ops[4] = ctx.create_operation(s.spawn, {}, 0U);
    for (u32 i = 0; i < 5U; ++i) { b->append(ops[i]); }
    return b;
}
[[nodiscard]] bool edge_present(const Array<Hazard>& h, const Operation* a, const Operation* b, HazardKind k) noexcept
{
    for (usize i = 0; i < h.size(); ++i)
    {
        if (h[i].before == a && h[i].after == b && h[i].kind == k) { return true; }
    }
    return false;
}
[[nodiscard]] bool any_edge(const Array<Hazard>& h, const Operation* a, const Operation* b) noexcept
{
    for (usize i = 0; i < h.size(); ++i)
    {
        if ((h[i].before == a && h[i].after == b) || (h[i].before == b && h[i].after == a)) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("ceir 9f: disjoint systems run in parallel; a shared component with a write is ordered", "[ceir][ecs]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Systems                s = register_ecs(ctx);
    Operation*                   op[5] = {};
    (void)build_world(ctx, s, op);
    Operation* const movement = op[0];
    Operation* const render   = op[1];
    Operation* const physics  = op[2];
    Operation* const input    = op[3];

    // movement WRITES Position, render READS Position -> shared resource + a write -> RAW -> ORDERED.
    CHECK(ctx.ops_hazard(*movement, *render) == HazardKind::Raw);
    // movement READS Velocity, physics WRITES Velocity, movement BEFORE physics -> WAR -> ORDERED (exact kind).
    CHECK(ctx.ops_hazard(*movement, *physics) == HazardKind::War);
    // ⭐ movement WRITES Position, input WRITES Health -> DISJOINT components -> NO hazard -> PARALLEL.
    CHECK(ctx.ops_hazard(*movement, *input) == HazardKind::None);
    // render (Position, Sprite reads) vs input (Health write) -> disjoint -> parallel.
    CHECK(ctx.ops_hazard(*render, *input) == HazardKind::None);
    // physics (Velocity write) vs input (Health write) -> disjoint -> parallel.
    CHECK(ctx.ops_hazard(*physics, *input) == HazardKind::None);
}

TEST_CASE("ceir 9f: a structural mutation is a whole-store barrier against every system", "[ceir][ecs]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Systems                s = register_ecs(ctx);
    Operation*                   op[5] = {};
    (void)build_world(ctx, s, op);
    Operation* const spawn = op[4];

    // spawn WRITES the whole ECS store (a null resource) -> conflicts with EVERY component access (the nullptr rule) ->
    // a barrier, by the EXISTING 4d conflict predicate, no special structural-mutation mechanism.
    for (u32 i = 0; i < 4U; ++i) { CHECK(ctx.ops_hazard(*spawn, *op[i]) != HazardKind::None); }
    CHECK(ctx.ops_hazard(*spawn, *op[0]) == HazardKind::Waw); // whole write vs Position write -> WAW
    CHECK(ctx.ops_hazard(*spawn, *op[1]) == HazardKind::Raw); // whole write vs render (reads) -> RAW
}

TEST_CASE("ceir 9f: the compiler infers the full ordering set from declared effects alone", "[ceir][ecs]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Systems                s = register_ecs(ctx);
    Operation*                   op[5] = {};
    Block* const                 b = build_world(ctx, s, op);
    Operation* const             movement = op[0];
    Operation* const             render   = op[1];
    Operation* const             physics  = op[2];
    Operation* const             input    = op[3];
    Operation* const             spawn    = op[4];

    Array<Hazard> h(&root);
    ctx.collect_block_hazards(*b, h);

    // exactly the six ordering edges the DECLARED effects imply (list order [movement,render,physics,input,spawn]).
    CHECK(h.size() == 6U);
    CHECK(edge_present(h, movement, render, HazardKind::Raw));  // Position
    CHECK(edge_present(h, movement, physics, HazardKind::War)); // Velocity
    CHECK(edge_present(h, movement, spawn, HazardKind::Waw));   // whole store
    CHECK(edge_present(h, render, spawn, HazardKind::War));     // render READS before spawn WRITES -> WAR
    CHECK(edge_present(h, physics, spawn, HazardKind::Waw));
    CHECK(edge_present(h, input, spawn, HazardKind::Waw));
    // ...and the disjoint system pairs have NO edge (they may run in parallel) -- the parallelism inference.
    CHECK_FALSE(any_edge(h, movement, input)); // Position write vs Health write
    CHECK_FALSE(any_edge(h, render, physics)); // Position/Sprite reads vs Velocity write
    CHECK_FALSE(any_edge(h, render, input));
    CHECK_FALSE(any_edge(h, physics, input));
}

TEST_CASE("ceir 9f: an EcsComponent-located effect resolves whole-class (the conservative fallback)", "[ceir][ecs]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Systems                s = register_ecs(ctx);
    Operation*                   op[5] = {};
    (void)build_world(ctx, s, op);
    Operation* const movement = op[0]; // writes Position
    Operation* const input    = op[3]; // writes Health

    // `coarse` declares an EcsWrite located on the 8c EcsComponent KIND (not an operand). That location kind carries no
    // per-instance identity yet (named-forward IN the 4d source), so it resolves WHOLE-CLASS -> it conflicts with EVERY
    // Ecs access, including two systems that are mutually parallel. That is the EMPTY!=UNKNOWN direction for location
    // identity: the coarse fallback is SAFE (more hazards) but PESSIMAL; precision is opt-in via the operand (above).
    Operation* const coarse = ctx.create_operation(s.coarse, {}, 0U);
    CHECK(ctx.ops_hazard(*coarse, *movement) != HazardKind::None); // conflicts with Position...
    CHECK(ctx.ops_hazard(*coarse, *input) != HazardKind::None);    // ...AND with the disjoint Health system
    CHECK(ctx.ops_hazard(*movement, *input) == HazardKind::None);  // whereas the two operand-precise systems stay parallel
}
