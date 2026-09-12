// CEIR-26a — the Dead-Code Elimination pass gate (device-free). Proves the deadness rule is `registered AND Pure AND
// no-result-uses`: a Pure leaf with no uses is removed; a chain collapses to FIXPOINT in ONE run; a second run is a no-op
// (idempotence — the PassManager "did nothing => invalidate nothing" contract); and DCE KEEPS an effectful op, an
// UNREGISTERED op (EMPTY!=UNKNOWN), and a used Pure op. This is the reference-independent unit gate; the ceir-gpu consumer
// gate (the vjp_mlp 11/5/4 -> 9/4/3 plan shift) + the 25c-2 device-numeric re-run land at 26a-2.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/passes/dce.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;

namespace
{
// A controlled test dialect: `dcetest.pure` (Pure => DCE-removable) + `dcetest.eff` (no traits => effectful => kept). A
// third kind `dcetest.unknown` is INTERNED but never REGISTERED (op_info == nullptr => maximally effectful => kept).
struct DceFixture
{
    crd::memory::GrowableTlsfAllocator root;
    Context                           ctx{&root};
    OpId                              k_pure{};
    OpId                              k_eff{};
    OpId                              k_unknown{};
    DceFixture()
    {
        Dialect* const d = ctx.register_dialect("dcetest");
        d->register_op("pure", OpSpec{.traits = flags_of(OpTrait::Pure)});
        d->register_op("eff", OpSpec{}); // no traits, no effects => NOT Pure => never a DCE target
        k_pure    = ctx.intern_op("dcetest", "pure");
        k_eff     = ctx.intern_op("dcetest", "eff");
        k_unknown = ctx.intern_op("dcetest", "unknown"); // NEVER registered
    }
};
} // namespace

TEST_CASE("ceir 26a: DCE removes a dead Pure chain to fixpoint in ONE run + is idempotent", "[ceir][dce]")
{
    DceFixture f;
    Module*    m     = f.ctx.create_module();
    Block*     block = f.ctx.create_block();
    m->body()->append(block);

    // %a = dcetest.pure ; %b = dcetest.pure(%a) -- %b unused; %a used only by %b (the dx -> W1t dead-chain shape).
    Operation* const a = f.ctx.create_operation(f.k_pure, {}, 1U);
    block->append(a);
    Value* const     ain[1] = {a->result(0)};
    Operation* const b      = f.ctx.create_operation(f.k_pure, ain, 1U);
    block->append(b);
    REQUIRE(block->num_ops() == 2U);
    REQUIRE(a->result(0)->has_uses()); // ⛔ %a is NOT dead at t=0 (used by %b) — so its death is the FIXPOINT proof (round 2)

    DiagnosticEngine diag(f.ctx, &f.root);
    // ONE run collapses the WHOLE chain: round 1 kills %b, round 2 kills the now-dead %a (the transpose only dies once dx is gone).
    const bool changed = dce_run(f.ctx, *m, diag);
    CHECK(changed);
    CHECK(block->num_ops() == 0U);
    CHECK(a->is_erased()); // ⛔ identity (not just count) — %a died ONLY after round 2 freed it; robust when Release disables erase()'s assert
    CHECK(b->is_erased());
    CHECK_FALSE(diag.has_fatal());

    // Idempotence: a second run changes nothing (a pass that spuriously flips `changed` would evict analyses every run).
    const bool changed2 = dce_run(f.ctx, *m, diag);
    CHECK_FALSE(changed2);
}

TEST_CASE("ceir 26a: DCE KEEPS effectful / unregistered / used ops (deadness = registered AND Pure AND no-uses)", "[ceir][dce]")
{
    DceFixture f;
    Module*    m     = f.ctx.create_module();
    Block*     block = f.ctx.create_block();
    m->body()->append(block);

    // (3) a Pure op whose result IS used -> KEPT (has_uses); (1) the effectful consumer, itself unused -> KEPT (not Pure);
    // (2) an unregistered op, unused -> KEPT (op_info == nullptr, EMPTY!=UNKNOWN -- the write-through-declare guard's shape).
    Operation* const pure_used = f.ctx.create_operation(f.k_pure, {}, 1U);
    block->append(pure_used);
    Value* const     ein[1] = {pure_used->result(0)};
    Operation* const eff = f.ctx.create_operation(f.k_eff, ein, 1U); // consumes pure_used; itself unused
    block->append(eff);
    Operation* const unk = f.ctx.create_operation(f.k_unknown, {}, 1U); // unregistered, unused
    block->append(unk);
    REQUIRE(block->num_ops() == 3U);

    DiagnosticEngine diag(f.ctx, &f.root);
    const bool       changed = dce_run(f.ctx, *m, diag);
    CHECK_FALSE(changed);          // nothing was dead
    CHECK(block->num_ops() == 3U); // all three kept
}

TEST_CASE("ceir 26a: DCE recurses into nested regions + never removes a region-bearing op", "[ceir][dce]")
{
    DceFixture f;
    // A container op with ONE region. ⛔ Pure-labelled ON PURPOSE: it is STILL kept, because num_regions != 0 (a leaf-only rule).
    Dialect* const d = f.ctx.dialect("dcetest");
    d->register_op("container", OpSpec{.traits = flags_of(OpTrait::Pure)});
    const OpId k_container = f.ctx.intern_op("dcetest", "container");

    Module* const m   = f.ctx.create_module();
    Block* const  top = f.ctx.create_block();
    m->body()->append(top);
    Operation* const container = f.ctx.create_operation(k_container, {}, 0U, {}, /*num_regions*/ 1U);
    top->append(container);
    Block* const inner = f.ctx.create_block();
    container->region(0)->append(inner);
    Operation* const dead_inner = f.ctx.create_operation(f.k_pure, {}, 1U); // dead Pure op INSIDE the region
    inner->append(dead_inner);
    REQUIRE(top->num_ops() == 1U);
    REQUIRE(inner->num_ops() == 1U);

    DiagnosticEngine diag(f.ctx, &f.root);
    const bool       changed = dce_run(f.ctx, *m, diag);
    CHECK(changed);
    CHECK(top->num_ops() == 1U);            // the container is KEPT (region-bearing, even though Pure + result-less)
    CHECK_FALSE(container->is_erased());    // identity: the container survived
    CHECK(inner->num_ops() == 0U);          // the dead Pure op inside was removed (recursion)
    CHECK(dead_inner->is_erased());         // identity: THAT op is the one that died
}
