// CEIR-8f (ADR-0116, U-§23) — the domain/safety split. The SAFETY axes (may-allocate/may-block/may-IO + derived
// realtime_safe) are a PROJECTION of the effect families (a total switch, no new declared data). op_safety folds them
// over an op's effects (unregistered = maximally unsafe, EMPTY!=UNKNOWN). The two RT oracles (the effect_legal_in_region
// HARD gate vs the realtime_safe advisory axis) are a documented SUBSET, not two drifting oracles. Host-only. ASCII.

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

TEST_CASE("ceir 8f: effect_safety classifies families; op_safety folds over an op's declared effects", "[ceir][safety]")
{
    CHECK(effect_safety(EffectFamily::Allocate).may_allocate);
    CHECK_FALSE(effect_safety(EffectFamily::Allocate).realtime_safe());
    CHECK(effect_safety(EffectFamily::FileIO).may_io);
    CHECK(effect_safety(EffectFamily::Synchronization).may_block);
    CHECK(effect_safety(EffectFamily::GPUCommand).may_block); // a submit can stall (documented judgment)
    CHECK(effect_safety(EffectFamily::MemoryWrite).realtime_safe());
    CHECK(effect_safety(EffectFamily::TimeRead).realtime_safe());
    CHECK(effect_safety(EffectFamily::DocumentWrite).realtime_safe()); // a bounded domain write is RT-safe

    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d      = ctx.register_dialect("test");
    const EffectRecord           fio[1] = {{EffectFamily::FileIO, EffectTarget::None, 0U, 0U}};
    const OpId                   io     = d->register_op("io", {.effects = ConstSpan<EffectRecord>(fio, 1U)});
    CHECK(ctx.op_safety(io).may_io);
    CHECK_FALSE(ctx.op_safety(io).realtime_safe());
    const OpId pure = d->register_op("pure", {.traits = flags_of(OpTrait::Pure)}); // registered, ZERO effects
    CHECK(ctx.op_safety(pure).realtime_safe());                                    // genuinely declared none

    const SafetyBits u = ctx.op_safety(ctx.intern_op("plugin", "widget")); // ⛔ EMPTY!=UNKNOWN
    CHECK(u.may_allocate);
    CHECK(u.may_block);
    CHECK(u.may_io);
    CHECK_FALSE(u.realtime_safe());
}

TEST_CASE("ceir 8f: the two RT oracles are a documented SUBSET, not drift", "[ceir][safety]")
{
    const RegionExec audio_rt{EvalDomain::DeviceTime, RealtimeClass::AudioRealTime};
    // ⛔ THE reconciliation, pinned side by side: the HARD gate PERMITS Allocate in an audio-RT region (a soft cost,
    // not a priority-inversion deadlock)...
    CHECK(effect_legal_in_region(EffectFamily::Allocate, audio_rt));
    // ...but the STRICTER advisory axis flags it (an RT-safe op allocates nothing). realtime_safe => legal, never converse.
    CHECK_FALSE(effect_safety(EffectFamily::Allocate).realtime_safe());
    // where they AGREE: a blocking wait is BOTH illegal in an RT region AND not realtime_safe.
    CHECK_FALSE(effect_legal_in_region(EffectFamily::Synchronization, audio_rt));
    CHECK_FALSE(effect_safety(EffectFamily::Synchronization).realtime_safe());
}
