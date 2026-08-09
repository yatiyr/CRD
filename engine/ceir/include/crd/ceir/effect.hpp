#pragma once

// crd-ceir — the EFFECT vocabulary (CEIR-4a, §26). Every effectful op-kind DECLARES its semantic effects; the core
// carries them on `OpInfo` (beside traits + verifier — dialect.hpp) so the compiler can reason about reordering
// legality, hazard detection, scheduling, replay, determinism, sandboxing, caching, and incremental execution WITHOUT a
// central switch on op.kind (§7). Declaration is KIND-LEVEL: an effect optionally names an operand/result POSITION; the
// per-instance SSA resource it resolves to is the CEIR-4d hazard analysis. Effects are DECLARED via the CEIR-2a schema
// (the `.ceirop.toml` `effects` field → the generator emits typed `EffectRecord` arrays); hand-registered ops pass them
// to `register_op`.

#include <crd/core/types.hpp>

namespace crd::ceir
{
// The §26 core effect families, in §26 order. ⛔ APPEND AT END — the ordinal is stable vocabulary (mirrored by the
// CEIR-2 generator, the committed `.ops.json`, and — when it exists — a serialized form). ⛔ NO subsetting or
// editorializing (the NO-FOLLOW mandate): `MemoryReadWrite` stays its own family even though it reads as composable,
// because §26 lists it. `u8` — 27 families, far from 256.
// NOLINTNEXTLINE(performance-enum-size)
enum class EffectFamily : u8
{
    MemoryRead = 0,
    MemoryWrite,
    MemoryReadWrite,
    Allocate,
    Deallocate,
    ResourceResidency,
    GPUCommand,
    HostStateRead,
    HostStateWrite,
    SceneRead,
    SceneWrite,
    EcsRead,
    EcsWrite,
    PhysicsRead,
    PhysicsWrite,
    AudioRead,
    AudioWrite,
    FileIO,
    NetworkIO,
    DeviceIO,
    ExternalCall,
    TimeRead,
    RandomRead,
    Nondeterministic,
    Synchronization,
    Logging,
    Debug,
};
// The last family — the vocabulary bound a declaration site validates against (a family code past this is invalid).
inline constexpr EffectFamily kLastEffectFamily = EffectFamily::Debug;
// ⛔ Cross-language lockstep: §26 lists 27 families (ordinals 0..26). If this fires, effect.hpp and the generator's
// EFFECT_FAMILIES (tools/ceir_opgen/ceir_opgen.py) have diverged — an append to one side only. Keep both in sync.
static_assert(static_cast<u8>(kLastEffectFamily) == 26U, "EffectFamily must have 27 entries (§26); sync with ceir_opgen");

// The bit for family `f` in a `u32` EffectFamily bitmask (27 families < 32 bits). The compact set representation the
// CEIR-5c callee-derived-effects walk (`Context::collect_effective_mask`) accumulates — a union is one `|=`, dedup falls
// out for free (no allocation), and the ambient `EffectRecord` list is one record per set bit.
[[nodiscard]] constexpr u32 effect_family_bit(EffectFamily f) noexcept { return u32{1} << static_cast<u32>(f); }

// How an effect identifies WHAT it touches (§26 "effects may carry resource/range identity"). `None` = an ambient /
// whole-op effect with no resource identity (e.g. `TimeRead`, `Logging`, `Synchronization`); `Operand`/`Result` name a
// POSITION on the op-kind, resolved to a concrete SSA resource by the CEIR-4d hazard analysis.
// NOLINTNEXTLINE(performance-enum-size)
enum class EffectTarget : u8
{
    None = 0,
    Operand,
    Result,
};

// One declared effect of an op-kind: a family, optionally targeting an operand/result position, optionally narrowed to a
// range. `range_mask` reuses the CEIR-3c `ViewRange` bitmask vocabulary (byte/element/mip/layer/aspect); 0 = the whole
// resource. A trivially-copyable POD (no `StringView`) so generated `constexpr` arrays AND the `register_op` arena copy
// are both cheap — the compiler reads this on every reorder/hazard query.
struct EffectRecord
{
    EffectFamily family;                          // which §26 effect
    EffectTarget target     = EffectTarget::None; // whose resource identity (if any)
    u32          index      = 0U;                 // operand/result index when target != None
    u32          range_mask = 0U;                 // a ViewRange mask (0 = whole resource)
};
} // namespace crd::ceir
