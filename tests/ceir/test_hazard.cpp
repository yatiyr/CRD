// CEIR-4d — the effect-derived HAZARD gate (sec 26 / sec 116). Two ops that touch the same resource (+ overlapping
// range) with at least one WRITE have a RAW/WAR/WAW ordering constraint a scheduler must preserve — the frame-graph
// read/write/lifetime discipline promoted to IR. Proves the family classifier (incl. the RandomRead-writes trap), the
// W->R/R->W/W->W/R->R quartet on a shared operand, range/alias/ambient rules, the barrier + unknown + Pure cases, the
// WAW>RAW>WAR precedence, and the collect_block_hazards edge list. Host-only, ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp> // func.call as a barrier op

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{
// effect-record tables (operand-0 identity unless ambient); ViewRange bits: Element=2, Mip=4.
constexpr EffectRecord kReadOp0[]   = {{EffectFamily::MemoryRead, EffectTarget::Operand, 0U, 0U}};
constexpr EffectRecord kWriteOp0[]  = {{EffectFamily::MemoryWrite, EffectTarget::Operand, 0U, 0U}};
constexpr EffectRecord kReadElem[]  = {{EffectFamily::MemoryRead, EffectTarget::Operand, 0U, 2U}};
constexpr EffectRecord kWriteMip[]  = {{EffectFamily::MemoryWrite, EffectTarget::Operand, 0U, 4U}};
constexpr EffectRecord kWriteElem[] = {{EffectFamily::MemoryWrite, EffectTarget::Operand, 0U, 2U}};
constexpr EffectRecord kGWrite[]    = {{EffectFamily::MemoryWrite, EffectTarget::None, 0U, 0U}};       // ambient memory
constexpr EffectRecord kRng[]       = {{EffectFamily::RandomRead, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kLog[]       = {{EffectFamily::Logging, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kNow[]       = {{EffectFamily::TimeRead, EffectTarget::None, 0U, 0U}};
constexpr EffectRecord kRW2[] = {{EffectFamily::MemoryRead, EffectTarget::Operand, 0U, 0U},            // TWO effects
                                 {EffectFamily::MemoryWrite, EffectTarget::Operand, 0U, 0U}};
constexpr EffectRecord kWriteRes0[] = {{EffectFamily::MemoryWrite, EffectTarget::Result, 0U, 0U}};     // writes its RESULT
constexpr EffectRecord kFreeOp0[]   = {{EffectFamily::Deallocate, EffectTarget::Operand, 0U, 0U}};     // Deallocate operand 0

[[nodiscard]] ConstSpan<EffectRecord> e1(const EffectRecord* a) { return ConstSpan<EffectRecord>(a, 1U); }

struct HzOps
{
    OpId src, read, write, read_elem, write_mip, write_elem, gwrite, rng, log, now, rw2, produce, dealloc;
    explicit HzOps(Context& ctx)
    {
        Dialect* const d = ctx.register_dialect("hz");
        src        = d->register_op("src", {.traits = flags_of(OpTrait::Pure)}); // 1 result, ZERO effects (a resource producer)
        produce    = d->register_op("produce", {.effects = e1(kWriteRes0)}); // writes its RESULT (alloc-and-init shape)
        dealloc      = d->register_op("free", {.effects = e1(kFreeOp0)});
        read       = d->register_op("read", {.effects = e1(kReadOp0)});
        write      = d->register_op("write", {.effects = e1(kWriteOp0)});
        read_elem  = d->register_op("read_elem", {.effects = e1(kReadElem)});
        write_mip  = d->register_op("write_mip", {.effects = e1(kWriteMip)});
        write_elem = d->register_op("write_elem", {.effects = e1(kWriteElem)});
        gwrite     = d->register_op("gwrite", {.effects = e1(kGWrite)});
        rng        = d->register_op("rng", {.effects = e1(kRng)});
        log        = d->register_op("log", {.effects = e1(kLog)});
        now        = d->register_op("now", {.effects = e1(kNow)});
        rw2        = d->register_op("rw2", {.effects = ConstSpan<EffectRecord>(kRW2, 2U)});
    }
};

Value* mk_resource(Context& ctx, const HzOps& hz, Block* b)
{
    Operation* const o = ctx.create_operation(hz.src, {}, 1U, ctx.type_i32());
    b->append(o);
    return o->result(0U);
}
Operation* use_res(Context& ctx, OpId kind, Value* v, Block* b)
{
    Value* ops[1] = {v};
    Operation* const o = ctx.create_operation(kind, ConstSpan<Value*>(ops, 1U), 0U);
    b->append(o);
    return o;
}
} // namespace

TEST_CASE("ceir hazard: effect_access classifies every family (incl the RandomRead-writes trap)", "[ceir][hazard]")
{
    CHECK(effect_access(EffectFamily::MemoryRead).reads);
    CHECK_FALSE(effect_access(EffectFamily::MemoryRead).writes);
    CHECK(effect_access(EffectFamily::MemoryReadWrite).reads);
    CHECK(effect_access(EffectFamily::MemoryReadWrite).writes);
    CHECK(effect_access(EffectFamily::Allocate).writes); // lifecycle lives in the Memory class (use-after-free visible)
    CHECK(effect_access(EffectFamily::Allocate).klass == ResourceClass::Memory);
    CHECK(effect_access(EffectFamily::GPUCommand).klass == ResourceClass::Gpu);
    CHECK(effect_access(EffectFamily::FileIO).klass == ResourceClass::Io);
    CHECK((effect_access(EffectFamily::FileIO).reads && effect_access(EffectFamily::FileIO).writes));
    CHECK(effect_access(EffectFamily::ExternalCall).klass == ResourceClass::Universe);
    CHECK(effect_access(EffectFamily::Synchronization).klass == ResourceClass::Universe);
    // ⛔ the trap: a PRNG draw ADVANCES the stream, so RandomRead WRITES; TimeRead is a pure (inert) read.
    CHECK(effect_access(EffectFamily::RandomRead).writes);
    CHECK(effect_access(EffectFamily::TimeRead).reads);
    CHECK_FALSE(effect_access(EffectFamily::TimeRead).writes);
    CHECK_FALSE(effect_access(EffectFamily::Nondeterministic).reads); // inert (DeterminismClass is the real signal)
    CHECK_FALSE(effect_access(EffectFamily::Nondeterministic).writes);
    // range overlap: 0 is the whole resource (overlaps all); shared bits overlap; disjoint bits don't.
    CHECK(range_overlap(0U, 4U));
    CHECK(range_overlap(2U, 2U));
    CHECK_FALSE(range_overlap(2U, 4U));
}

TEST_CASE("ceir hazard: the W->R / R->W / W->W / R->R quartet on a shared operand (sec 116)", "[ceir][hazard]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const HzOps                  hz(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const     v  = mk_resource(ctx, hz, b);
    Operation* const rd = use_res(ctx, hz.read, v, b);
    Operation* const wr = use_res(ctx, hz.write, v, b);
    Operation* const rd2 = use_res(ctx, hz.read, v, b);
    Operation* const wr2 = use_res(ctx, hz.write, v, b);

    CHECK(ctx.ops_hazard(*rd, *wr) == HazardKind::War); // read-then-write over the same buffer (the WAR-lifetime scar)
    CHECK(ctx.ops_hazard(*wr, *rd) == HazardKind::Raw); // write-then-read
    CHECK(ctx.ops_hazard(*wr, *wr2) == HazardKind::Waw); // write-then-write
    CHECK(ctx.ops_hazard(*rd, *rd2) == HazardKind::None); // read-read: freely reorderable
}

TEST_CASE("ceir hazard: range masks + distinct-Value non-aliasing gate a conflict", "[ceir][hazard]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const HzOps                  hz(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const v = mk_resource(ctx, hz, b);

    // disjoint ranges (element vs mip) over the SAME buffer -> no hazard; overlapping (element vs element) -> hazard.
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.read_elem, v, b), *use_res(ctx, hz.write_mip, v, b)) == HazardKind::None);
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.read_elem, v, b), *use_res(ctx, hz.write_elem, v, b)) == HazardKind::War);
    // a whole-resource write (mask 0) overlaps a mip write.
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.write, v, b), *use_res(ctx, hz.write_mip, v, b)) == HazardKind::Waw);

    // ⛔ distinct SSA Values are assumed NON-ALIASING: writes to two different buffers do not hazard.
    Value* const v2 = mk_resource(ctx, hz, b);
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.write, v, b), *use_res(ctx, hz.write, v2, b)) == HazardKind::None);
}

TEST_CASE("ceir hazard: ambient effects, the RandomRead/Logging/TimeRead families", "[ceir][hazard]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const HzOps                  hz(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const v = mk_resource(ctx, hz, b);

    // an AMBIENT memory write (whole class) conflicts with a SPECIFIC memory write on any buffer.
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.gwrite, v, b), *use_res(ctx, hz.write, v, b)) == HazardKind::Waw);
    auto ambient = [&](OpId k) { return ctx.create_operation(k, {}, 0U); };
    CHECK(ctx.ops_hazard(*ambient(hz.gwrite), *ambient(hz.gwrite)) == HazardKind::Waw);
    // ⛔ two PRNG draws must ORDER (each advances the stream) — the replay-determinism trap.
    CHECK(ctx.ops_hazard(*ambient(hz.rng), *ambient(hz.rng)) == HazardKind::Waw);
    // logging orders among itself (stable interleave) but does NOT conflict with compute; time reads never order.
    CHECK(ctx.ops_hazard(*ambient(hz.log), *ambient(hz.log)) == HazardKind::Waw);
    CHECK(ctx.ops_hazard(*ambient(hz.log), *ambient(hz.gwrite)) == HazardKind::None); // Log vs Memory: different classes
    CHECK(ctx.ops_hazard(*ambient(hz.now), *ambient(hz.now)) == HazardKind::None);    // TimeRead is inert (read-read)
}

TEST_CASE("ceir hazard: barrier (func.call), unknown-op, Pure-op, and WAW>RAW>WAR precedence", "[ceir][hazard]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const HzOps                  hz(ctx);
    func::register_dialect(ctx);
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const v = mk_resource(ctx, hz, b);

    auto op0 = [&](OpId k) { return ctx.create_operation(k, {}, 0U); };
    // func.call = ExternalCall = Universe rw: a full barrier — it hazards anything effectful, in both directions.
    Operation* const call = op0(ctx.intern_op("func", "call"));
    CHECK(ctx.ops_hazard(*call, *use_res(ctx, hz.write, v, b)) == HazardKind::Waw);
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.read, v, b), *call) == HazardKind::War);
    // an UNREGISTERED op is maximally effectful (Universe rw) -> hazards; but a Pure (zero-effect) op touches nothing.
    Operation* const unknown = op0(ctx.intern_op("plugin", "mystery"));
    CHECK(ctx.ops_hazard(*unknown, *use_res(ctx, hz.write, v, b)) == HazardKind::Waw);
    Operation* const pure = ctx.create_operation(hz.src, {}, 1U, ctx.type_i32());
    CHECK(ctx.ops_hazard(*unknown, *pure) == HazardKind::None); // unknown vs Pure = None (Pure touches nothing)
    CHECK(ctx.ops_hazard(*call, *pure) == HazardKind::None);

    // precedence: rw2 (a read AND a write on v) BEFORE a write on v yields both a WAR and a WAW pair -> WAW wins.
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.rw2, v, b), *use_res(ctx, hz.write, v, b)) == HazardKind::Waw);
}

TEST_CASE("ceir hazard: Result-targeted effects report RAW; lifecycle ordering is the WAR/UAF scar (sec 116)", "[ceir][hazard]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const HzOps                  hz(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);

    // a producer WRITES its RESULT; a reader of that result -> RAW is REPORTED (an effect-derived edge that coincides with
    // the SSA def-use edge is NOT filtered — the collector reports everything, the scheduler dedups). Exercises the
    // EffectTarget::Result resolution branch.
    Operation* const p = ctx.create_operation(hz.produce, {}, 1U, ctx.type_i32());
    b->append(p);
    CHECK(ctx.ops_hazard(*p, *use_res(ctx, hz.read, p->result(0U), b)) == HazardKind::Raw);

    // ⛔ lifecycle in the Memory class: a Deallocate hazards a read of the same buffer BOTH ways — the frame-graph
    // WAR-needs-lifetime scar (a free must not move before a pending reader) and the visible use-after-free.
    Value* const v = mk_resource(ctx, hz, b);
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.read, v, b), *use_res(ctx, hz.dealloc, v, b)) == HazardKind::War);
    CHECK(ctx.ops_hazard(*use_res(ctx, hz.dealloc, v, b), *use_res(ctx, hz.read, v, b)) == HazardKind::Raw);
}

TEST_CASE("ceir hazard: collect_block_hazards yields the exact edge list in list order", "[ceir][hazard]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const HzOps                  hz(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = ctx.create_block(0U);
    m->body()->append(b);
    // src(pure) ; write(v) ; read(v) ; write2(v)  -> edges: write->read RAW, write->write2 WAW, read->write2 WAR.
    Value* const     v   = mk_resource(ctx, hz, b);
    Operation* const wr  = use_res(ctx, hz.write, v, b);
    Operation* const rd  = use_res(ctx, hz.read, v, b);
    Operation* const wr2 = use_res(ctx, hz.write, v, b);

    Array<Hazard> out(&root);
    ctx.collect_block_hazards(*b, out);
    REQUIRE(out.size() == 3U); // src is Pure -> contributes no edges
    CHECK((out[0].before == wr && out[0].after == rd && out[0].kind == HazardKind::Raw));
    CHECK((out[1].before == wr && out[1].after == wr2 && out[1].kind == HazardKind::Waw));
    CHECK((out[2].before == rd && out[2].after == wr2 && out[2].kind == HazardKind::War));
}
