#pragma once

// crd-ceir — the EFFECT vocabulary (CEIR-4a, §26). Every effectful op-kind DECLARES its semantic effects; the core
// carries them on `OpInfo` (beside traits + verifier — dialect.hpp) so the compiler can reason about reordering
// legality, hazard detection, scheduling, replay, determinism, sandboxing, caching, and incremental execution WITHOUT a
// central switch on op.kind (§7). Declaration is KIND-LEVEL: an effect optionally names an operand/result POSITION; the
// per-instance SSA resource it resolves to is the CEIR-4d hazard analysis. Effects are DECLARED via the CEIR-2a schema
// (the `.ceirop.toml` `effects` field → the generator emits typed `EffectRecord` arrays); hand-registered ops pass them
// to `register_op`.

#include <crd/ceir/id.hpp> // LocationClassId (CEIR-8c)
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
    // ── CEIR-8c (ADR-0113) U-§19 domain families — APPEND AT END (ordinals 27..34). Derived from the U-§19 domains
    // (document/CAD-EDA-constraint/transaction/UI/agent) following the established <Domain>Read/<Domain>Write
    // convention (§26 stops enumerating at Debug). Each is CLASSIFIED in hazard.hpp::effect_access (the -Werror=switch
    // total-switch guard forces it). Crossing bit 31 is exactly why the family bitmask widened u32→u64. ──
    DocumentRead,        // DCC/CAD/EDA/notebook document-object graph
    DocumentWrite,
    ConstraintRead,      // CAD parametric constraints + EDA design rules
    ConstraintWrite,
    TransactionBoundary, // a begin/commit/rollback — a full barrier (Universe), like Synchronization
    UIRead,              // reactive-UI signal reads
    UIWrite,             // UI event/state mutation
    AgentAction,         // an agent edit (observes-then-mutates); its specific resource rides the open LOCATION
};
// The last family — the vocabulary bound a declaration site validates against (a family code past this is invalid).
inline constexpr EffectFamily kLastEffectFamily = EffectFamily::AgentAction;
// ⛔ Cross-language lockstep: §26 + the CEIR-8c U-§19 families = 35 families (ordinals 0..34). If this fires,
// effect.hpp and the generator's EFFECT_FAMILIES (tools/ceir_opgen/ceir_opgen.py) have diverged — an append to one
// side only. Keep both in sync.
static_assert(static_cast<u8>(kLastEffectFamily) == 34U, "EffectFamily must have 35 entries (§26 + U-§19); sync with ceir_opgen");

// The bit for family `f` in a `u64` EffectFamily bitmask (35 families < 64 bits — CEIR-8c widened it from u32; the
// U-§19 families cross bit 31). The compact set representation the CEIR-5c callee-derived-effects walk
// (`Context::collect_effective_mask`) accumulates — a union is one `|=`, dedup falls out for free (no allocation),
// and the ambient `EffectRecord` list is one record per set bit.
[[nodiscard]] constexpr u64 effect_family_bit(EffectFamily f) noexcept { return u64{1} << static_cast<u32>(f); }

// How an effect identifies WHAT it touches (§26 "effects may carry resource/range identity"). `None` = an ambient /
// whole-op effect with no resource identity (e.g. `TimeRead`, `Logging`, `Synchronization`); `Operand`/`Result` name a
// POSITION on the op-kind, resolved to a concrete SSA resource by the CEIR-4d hazard analysis. CEIR-8c (ADR-0113,
// U-§20) opens this: the built-in fast path gains named location KINDS (buffer range · image subresource · tensor
// slice · ECS component · document object · state slot · file · net) + ONE `Extern` door for dialect-defined location
// classes. ⛔ APPEND AT END — the position kinds keep ordinals 0..2 so every generated EffectRecord array is unchanged.
// The built-in kinds are the named vocabulary; their per-instance identity resolution is named-forward to CEIR-8d
// (today they resolve conservatively to whole-class). `Extern` names a `LocationClassId` on the EffectRecord.
// NOLINTNEXTLINE(performance-enum-size)
enum class EffectTarget : u8
{
    None = 0,
    Operand,
    Result,
    // ── CEIR-8c open location vocabulary (append after Result) ──
    BufferRange,
    ImageSubresource,
    TensorSlice,
    EcsComponent,
    DocObject,
    StateSlot,
    File,
    Net,
    Extern, // the ONE open-world door — `EffectRecord::location_class` names the dialect-defined location class
};
// The last location kind — the bound a declaration site validates against (a kind past this is invalid).
inline constexpr EffectTarget kLastLocationKind = EffectTarget::Extern;
// CEIR-8c: the widened concept's name. `EffectTarget` stays the field type (generated arrays + hand registrations name
// it) so this is a pure alias, not a rename.
using LocationKind = EffectTarget;

// One declared effect of an op-kind: a family, optionally targeting an operand/result position, optionally narrowed to a
// range. `range_mask` reuses the CEIR-3c `ViewRange` bitmask vocabulary (byte/element/mip/layer/aspect); 0 = the whole
// resource. A trivially-copyable POD (no `StringView`) so generated `constexpr` arrays AND the `register_op` arena copy
// are both cheap — the compiler reads this on every reorder/hazard query.
struct EffectRecord
{
    EffectFamily family;                          // which §26 effect
    EffectTarget target     = EffectTarget::None; // whose resource identity (if any) — a LocationKind (CEIR-8c)
    u32          index      = 0U;                 // operand/result index when target != None
    u32          range_mask = 0U;                 // a ViewRange mask (0 = whole resource)
    // CEIR-8c (ADR-0113): the dialect-defined location class when `target == Extern` (0 otherwise — the junk-field
    // guard is asserted at register_op). A `u64` id keeps EffectRecord a trivially-copyable POD, so the generated
    // `constexpr` arrays and the register_op arena copy are both unchanged (the field default-inits when omitted).
    LocationClassId location_class = {};
};
} // namespace crd::ceir
