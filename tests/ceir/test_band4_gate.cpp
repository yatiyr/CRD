// CEIR-4z — the BAND-4 GATE (sec 26 / sec 168). Band 4 built three orthogonal axes over the effect model: determinism +
// numerics vs the compiler MODE (4b), effect legality vs a region's DOMAIN tag (4c), and effect-derived ordering HAZARDS
// (4d). This gate composes all three over ONE curated module and proves the band contract — "the compiler distinguishes
// reorderable vs ordered ops correctly" — with the frame-graph WAR-needs-lifetime scar as the named centerpiece. A
// TEST-ONLY gate: it reuses find_mode_violation / find_domain_violation / ops_hazard / collect_block_hazards; a unified
// verify entry stays unbuilt until a consumer exists (CEIR-6 / 12d). Host-only, ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{
constexpr EffectRecord kWriteOp0[] = {{EffectFamily::MemoryWrite, EffectTarget::Operand, 0U, 0U}};
constexpr EffectRecord kReadOp0[]  = {{EffectFamily::MemoryRead, EffectTarget::Operand, 0U, 0U}};
constexpr EffectRecord kRng[]      = {{EffectFamily::RandomRead, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kLog[]      = {{EffectFamily::Logging, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kFile[]     = {{EffectFamily::FileIO, EffectTarget::None, 0U, 0U}};

[[nodiscard]] ConstSpan<EffectRecord> e1(const EffectRecord* a) { return ConstSpan<EffectRecord>(a, 1U); }

// The gate dialect. ⛔ EVERY op is BitExact so the Certified MODE axis is CLEAN in the baseline — a SEEDED PRNG is
// bit-exact, so even `rng` is BitExact; only the deliberately-`Unspecified` `undecl` op (and unknown ops, kept OUT of the
// main block) trip the mode walk. `buf` is a Pure zero-effect producer of a buffer Value.
struct G4
{
    OpId buf, write, read, rng, log, undecl, region, fileio;
    explicit G4(Context& ctx)
    {
        Dialect* const d = ctx.register_dialect("g4");
        buf    = d->register_op("buf", {.traits = flags_of(OpTrait::Pure), .determinism = DeterminismClass::BitExact});
        write  = d->register_op("write", {.effects = e1(kWriteOp0), .determinism = DeterminismClass::BitExact});
        read   = d->register_op("read", {.effects = e1(kReadOp0), .determinism = DeterminismClass::BitExact});
        rng    = d->register_op("rng", {.effects = e1(kRng), .determinism = DeterminismClass::BitExact});
        log    = d->register_op("log", {.effects = e1(kLog), .determinism = DeterminismClass::BitExact});
        undecl = d->register_op("undecl", {}); // zero effects, NO determinism claim (Unspecified) — the mode perturbation
        region = d->register_op("region", {.determinism = DeterminismClass::BitExact}); // a region holder
        fileio = d->register_op("fileio", {.effects = e1(kFile), .determinism = DeterminismClass::BitExact});
    }
};

Value* mk_buf(Context& ctx, const G4& g, Block* b)
{
    Operation* const o = ctx.create_operation(g.buf, {}, 1U, ctx.type_i32());
    b->append(o);
    return o->result(0U);
}
Operation* use1(Context& ctx, OpId k, Value* v, Block* b)
{
    Value* ops[1] = {v};
    Operation* const o = ctx.create_operation(k, ConstSpan<Value*>(ops, 1U), 0U);
    b->append(o);
    return o;
}
Operation* ambient(Context& ctx, OpId k, Block* b)
{
    Operation* const o = ctx.create_operation(k, {}, 0U);
    b->append(o);
    return o;
}
// exact list IDENTITY (not just count) — an orthogonality perturbation must leave every edge's endpoints AND kind intact.
void check_same_edges(const Array<Hazard>& a, const Array<Hazard>& base)
{
    REQUIRE(a.size() == base.size());
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        CHECK((a[i].before == base[i].before && a[i].after == base[i].after && a[i].kind == base[i].kind));
    }
}
} // namespace

TEST_CASE("ceir gate4: the WAR-needs-lifetime scar - a read then a write over ONE buffer is ordered", "[ceir][gate4]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const G4                     g(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // read(R) THEN write(R): WAR purely from the effects + SSA resource identity — there is NO declaration-order metadata
    // anywhere; the ordering falls out of "both reference the same buffer Value, one writes". This IS the scar.
    Value* const     r  = mk_buf(ctx, g, b);
    Operation* const rd = use1(ctx, g.read, r, b);
    Operation* const wr = use1(ctx, g.write, r, b);
    CHECK(ctx.ops_hazard(*rd, *wr) == HazardKind::War);

    // the SAME two op kinds on a DIFFERENT buffer do NOT order — the scar is the SHARED resource, not textual adjacency.
    Value* const r2 = mk_buf(ctx, g, b);
    CHECK(ctx.ops_hazard(*use1(ctx, g.read, r, b), *use1(ctx, g.write, r2, b)) == HazardKind::None);
}

TEST_CASE("ceir gate4: the curated module classifies every pair reorderable vs ordered", "[ceir][gate4]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const G4                     g(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // baseline: buf(R) ; write(R) ; read(R) ; write2(R) ; rng ; rng2 ; log  — mode-clean (all BitExact), domain-clean
    // (untagged), hazard-rich.
    Value* const     r   = mk_buf(ctx, g, b);
    Operation* const w1  = use1(ctx, g.write, r, b);
    Operation* const r1  = use1(ctx, g.read, r, b);
    Operation* const w2  = use1(ctx, g.write, r, b);
    Operation* const rg1 = ambient(ctx, g.rng, b);
    Operation* const rg2 = ambient(ctx, g.rng, b);
    (void)ambient(ctx, g.log, b);

    Array<Hazard> hz(&root);
    ctx.collect_block_hazards(*b, hz);
    REQUIRE(hz.size() == 4U); // the EXACT forward matrix — every OTHER pair is None, pinned by absence + the count
    CHECK((hz[0].before == w1 && hz[0].after == r1 && hz[0].kind == HazardKind::Raw));  // W->R
    CHECK((hz[1].before == w1 && hz[1].after == w2 && hz[1].kind == HazardKind::Waw));  // W->W
    CHECK((hz[2].before == r1 && hz[2].after == w2 && hz[2].kind == HazardKind::War));  // R->W (the buffer WAR)
    CHECK((hz[3].before == rg1 && hz[3].after == rg2 && hz[3].kind == HazardKind::Waw)); // two PRNG draws ORDER

    // reverse-direction sweep on the direction-sensitive pairs: RAW<->WAR flips; WAW is symmetric.
    CHECK(ctx.ops_hazard(*w1, *r1) == HazardKind::Raw);
    CHECK(ctx.ops_hazard(*r1, *w1) == HazardKind::War);
    CHECK(ctx.ops_hazard(*w2, *w1) == HazardKind::Waw);

    // the OTHER two axes are clean on this module: no determinism/numerics violation (even under Certified), no domain one.
    CHECK(ctx.find_mode_violation(*m) == nullptr);
    ctx.set_compiler_mode(CompilerMode::CertifiedDeterministic);
    CHECK(ctx.find_mode_violation(*m) == nullptr); // all ops BitExact, no fast-math numerics
    CHECK(ctx.find_domain_violation(*m).op == nullptr);
}

TEST_CASE("ceir gate4: the three band-4 axes are orthogonal - mode/domain moves leave the hazard list unchanged", "[ceir][gate4]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const G4                     g(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const     r  = mk_buf(ctx, g, b);
    Operation* const w1 = use1(ctx, g.write, r, b);
    (void)use1(ctx, g.read, r, b);
    (void)use1(ctx, g.write, r, b);
    Array<Hazard> base(&root);
    ctx.collect_block_hazards(*b, base);
    REQUIRE(base.size() == 3U); // W->R, W->W, R->W

    // MODE axis A (per-instance NUMERICS): fast_math on a BitExact op trips Certified but is hazard-INERT.
    NumericalSemantics ns;
    ns.fast_math = Toggle::On;
    ctx.set_numerics(w1, ns);
    ctx.set_compiler_mode(CompilerMode::CertifiedDeterministic);
    CHECK(ctx.find_mode_violation(*m) == w1);
    Array<Hazard> after_num(&root);
    ctx.collect_block_hazards(*b, after_num);
    check_same_edges(after_num, base); // hazards are CONTENT; the session mode + per-instance numerics don't touch them

    // MODE axis B (op-kind DETERMINISM): restore w1 numerics to clean, then append an Unspecified op — the OTHER half of
    // find_mode_violation (a class that fails Certified), still hazard-inert (zero effects).
    ctx.set_numerics(w1, NumericalSemantics{});
    Operation* const undecl = ambient(ctx, g.undecl, b);
    CHECK(ctx.find_mode_violation(*m) == undecl);
    Array<Hazard> after_det(&root);
    ctx.collect_block_hazards(*b, after_det);
    check_same_edges(after_det, base);

    // DOMAIN axis: a side region tagged audio-RT with a FileIO op fires the domain walk; the MAIN block's hazards are
    // untouched (the region-holder is zero-effect, the FileIO lives in a DIFFERENT block).
    Operation* const rholder = ctx.create_operation(g.region, {}, 0U, {}, 1U);
    b->append(rholder);
    Block* const rbody = ctx.create_block(0U);
    rholder->region(0)->append(rbody);
    (void)ambient(ctx, g.fileio, rbody);
    CHECK(ctx.find_domain_violation(*m).op == nullptr); // untagged region: legal
    ctx.set_region_exec(rholder, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
    CHECK(ctx.find_domain_violation(*m).effect == EffectFamily::FileIO); // now FileIO-in-audio fires
    Array<Hazard> after_dom(&root);
    ctx.collect_block_hazards(*b, after_dom);
    check_same_edges(after_dom, base); // still identical — a domain move is content-orthogonal to the block's hazards
}

TEST_CASE("ceir gate4: unknown-op is a barrier, a Pure op is reorderable (side block)", "[ceir][gate4]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const G4                     g(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const     r    = mk_buf(ctx, g, b);
    Operation* const wr   = use1(ctx, g.write, r, b);
    Operation* const pure = ctx.create_operation(g.buf, {}, 1U, ctx.type_i32()); // Pure, zero effects
    Operation* const unk  = ctx.create_operation(ctx.intern_op("plugin", "opaque"), {}, 0U); // unregistered

    CHECK(ctx.ops_hazard(*unk, *wr) == HazardKind::Waw);   // unknown = Universe rw ⇒ a barrier vs anything effectful
    CHECK(ctx.ops_hazard(*unk, *pure) == HazardKind::None); // ...but Pure touches nothing ⇒ still reorderable
}

TEST_CASE("ceir gate4: the hazard classification survives serialize + deserialize", "[ceir][gate4]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const G4                     g(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const r = mk_buf(ctx, g, b);
    (void)use1(ctx, g.write, r, b);
    (void)use1(ctx, g.read, r, b);
    (void)use1(ctx, g.write, r, b);
    Array<Hazard> before(&root);
    ctx.collect_block_hazards(*b, before);
    REQUIRE(before.size() == 3U);

    // round-trip, then re-collect: the shared-operand Value identity must reconstitute through the parser's operand
    // FIXUP pass (write/read/write2 must re-bind to the SAME buffer Value), or the hazards vanish.
    const Array<crd::u8> blob = serialize(ctx, *m, &root);
    Context              ctx2(&root);
    const G4             g2(ctx2); // re-register g4 so the deserialized ops rebind their effects
    const ParseResult    dr = deserialize(ctx2, ConstSpan<crd::u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    Block* const  rb = dr.module->body()->first_block();
    Array<Hazard> after(&root);
    ctx2.collect_block_hazards(*rb, after);
    REQUIRE(after.size() == before.size()); // same edge count post-round-trip
    for (crd::usize i = 0; i < after.size(); ++i) { CHECK(after[i].kind == before[i].kind); }
}
