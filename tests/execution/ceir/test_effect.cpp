// CEIR-4a — the §26 EFFECT-system gate. Every effectful op declares typed effect records (a family + optional resource/
// range identity) via the CEIR-2a schema; the core carries them on `OpInfo` so the compiler can query `op_effects`
// WITHOUT a switch on op.kind (§7). This proves the TOML→generator→register_op→op_effects round-trip (BOTH declaration
// forms — a bare family and an identity-bearing table), the arena-copy lifetime (a caller's span need not outlive the
// call), and the EMPTY≠UNKNOWN contract (an unregistered kind is maximally effectful, not effect-free). Host-only, ASCII.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>        // the hand-registered func dialect (func.call's conservative barrier)
#include <crd/ceir/gen/test_ops.hpp> // the generated full-surface `test` dialect (register_test_ops)

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::ConstSpan;

TEST_CASE("ceir effect: op_effects round-trips both declaration forms from the 2a schema", "[ceir][effect]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)test::register_test_ops(ctx);

    // test.dummy declares (test.ceirop.toml): "GPUCommand" (bare/ambient), {MemoryRead, operand 0, range=element},
    // {MemoryWrite, result 0} — the generator emitted typed EffectRecords and passed them to register_op.
    const ConstSpan<EffectRecord> eff = ctx.op_effects(ctx.intern_op("test", "dummy"));
    REQUIRE(eff.size() == 3U);

    CHECK(eff[0].family == EffectFamily::GPUCommand); // bare family: no resource identity
    CHECK(eff[0].target == EffectTarget::None);
    CHECK(eff[0].range_mask == 0U);

    CHECK(eff[1].family == EffectFamily::MemoryRead); // table form: operand identity + a ViewRange narrowing
    CHECK(eff[1].target == EffectTarget::Operand);
    CHECK(eff[1].index == 0U);
    CHECK(eff[1].range_mask == static_cast<crd::u32>(ViewRange::Element));

    CHECK(eff[2].family == EffectFamily::MemoryWrite); // table form: result identity, whole resource (range 0)
    CHECK(eff[2].target == EffectTarget::Result);
    CHECK(eff[2].index == 0U);
    CHECK(eff[2].range_mask == 0U);

    // test.kinds is Pure ⇒ declares zero effects (the Pure-coherence path — enforced at cook time + register_op).
    CHECK(ctx.op_effects(ctx.intern_op("test", "kinds")).size() == 0U);
}

TEST_CASE("ceir effect: EMPTY != UNKNOWN - an unregistered kind is maximally effectful", "[ceir][effect]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)test::register_test_ops(ctx);

    // a registered effect-free op: empty span AND op_info != nullptr (genuinely declared no effects).
    const OpId kinds = ctx.intern_op("test", "kinds");
    CHECK(ctx.op_effects(kinds).size() == 0U);
    REQUIRE(ctx.op_info(kinds) != nullptr);

    // an UNREGISTERED (unknown-dialect) op: empty span too — BUT op_info == nullptr, so an analysis MUST treat it as
    // maximally effectful (never reorderable), never effect-free. The two are distinguished ONLY by op_info.
    const OpId unknown = ctx.intern_op("plugin", "widget");
    CHECK(ctx.op_effects(unknown).size() == 0U);
    CHECK(ctx.op_info(unknown) == nullptr);
}

TEST_CASE("ceir effect: a hand-registered op carries effects, arena-copied to outlive the caller's span", "[ceir][effect]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("hw");

    OpId store;
    {
        // a SCOPE-LOCAL effects array — register_op MUST copy it into the arena (the source array dies at the brace).
        // Under ASan this is a real use-after-scope probe: a non-copying register_op would dangle here.
        const EffectRecord local[2] = {
            {EffectFamily::MemoryWrite, EffectTarget::Operand, 1U, static_cast<crd::u32>(ViewRange::Element)},
            {EffectFamily::Synchronization, EffectTarget::None, 0U, 0U},
        };
        store = d->register_op("store", {.effects = ConstSpan<EffectRecord>(local, 2U)});
    } // `local` is destroyed here

    const ConstSpan<EffectRecord> eff = ctx.op_effects(store); // read AFTER the source array died
    REQUIRE(eff.size() == 2U);
    CHECK(eff[0].family == EffectFamily::MemoryWrite);
    CHECK(eff[0].target == EffectTarget::Operand);
    CHECK(eff[0].index == 1U);
    CHECK(eff[0].range_mask == static_cast<crd::u32>(ViewRange::Element));
    CHECK(eff[1].family == EffectFamily::Synchronization);
    CHECK(eff[1].target == EffectTarget::None);

    // ⛔ Pure ⇒ zero effects is asserted in register_op (and rejected at cook time by test_opgen.py); an assert aborts
    // the process, so that direction is proven at those two live arms, not with a death-test here.
}

TEST_CASE("ceir effect: func.call declares a conservative ExternalCall barrier, not effect-free", "[ceir][effect]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    func::register_dialect(ctx);

    // ⛔ a REGISTERED call with an empty span would read as "provably effect-free" (empty≠unknown) and 4d would reorder/
    // DCE it — so func.call declares ExternalCall until CEIR-5 derives effects from the callee.
    const ConstSpan<EffectRecord> call = ctx.op_effects(ctx.intern_op("func", "call"));
    REQUIRE(call.size() >= 1U);
    CHECK(call[0].family == EffectFamily::ExternalCall);
    // func.return (a control-flow terminator) and func.func (a definition) stay effect-free at the op level.
    CHECK(ctx.op_effects(ctx.intern_op("func", "return")).size() == 0U);
}
