#pragma once

// crd-ceir — the EFFECT-DERIVED HAZARD vocabulary (CEIR-4d, §26/§116). Composes the CEIR-4a EffectRecords of two ops
// into an ordering constraint: two ops that touch the SAME resource (+ overlapping range) with at least one WRITE have a
// RAW / WAR / WAW hazard the scheduler (CEIR-12d) must preserve. This is the frame-graph read/write/lifetime discipline
// (the WAR-needs-lifetime, RMW-not-RWM scars) promoted to first-class IR. This header is the pure vocabulary; the query
// (`Context::ops_hazard`) + the all-pairs collector (`Context::collect_block_hazards`) live on Context.

#include <crd/ceir/effect.hpp> // EffectFamily
#include <crd/core/types.hpp>

namespace crd::ceir
{
// The ordering relationship between an EARLIER op and a LATER op over a shared resource. `None` ⇒ freely reorderable.
// NOLINTNEXTLINE(performance-enum-size)
enum class HazardKind : u8
{
    None = 0,
    War, // read-then-write   (the earlier op reads, the later writes — the frame-graph WAR-lifetime scar)
    Raw, // write-then-read   (the later op depends on the earlier op's write)
    Waw, // write-then-write  (both write; order is observable)
};
// Precedence for aggregating multiple conflicting effect-pairs into one op-pair verdict: WAW > RAW > WAR > None.
[[nodiscard]] constexpr u8 hazard_rank(HazardKind k) noexcept
{
    switch (k)
    {
    case HazardKind::None: return 0U;
    case HazardKind::War: return 1U;
    case HazardKind::Raw: return 2U;
    case HazardKind::Waw: return 3U;
    }
    return 0U;
}

// The disjoint RESOURCE CLASSES an effect touches. Two effects can only conflict within the same class — EXCEPT
// `Universe` (ExternalCall / Synchronization), which overlaps every class (a full barrier). `None` = an inert effect
// (touches nothing — Nondeterministic; the real determinism signal is CEIR-4b's DeterminismClass).
// NOLINTNEXTLINE(performance-enum-size)
enum class ResourceClass : u8
{
    None = 0,
    Memory, // linear memory + resource lifecycle (alloc/dealloc/residency live here so use-after-free is visible)
    HostState,
    Scene,
    Ecs,
    Physics,
    Audio,
    Gpu,  // GPU command submission order (§116 "GPU unordered hazards")
    Io,   // file / network / device — one class: cross-channel reordering conservatively forbidden until a channel model
    Time, // read-only (no writer exists) ⇒ hazard-inert; replay-ordering of clock reads is the recorder's concern (5+)
    Random, // ⛔ a PRNG draw ADVANCES the stream, so RandomRead WRITES this class — RNG draws must NOT reorder (replay)
    Log,
    Debug,
    // ── CEIR-8c (ADR-0113) U-§19 domain classes — the conflict classes for the new families + built-in/Extern
    // locations. A dialect-defined Extern location declares its own ResourceClass (LocationClassSpec::resource_class);
    // an UNREGISTERED one degrades to Universe. ──
    Document,   // DCC/CAD/EDA/notebook document-object graph
    Constraint, // CAD parametric constraints + EDA design rules
    Ui,         // reactive-UI signals + events
    Agent,      // agent-driven edits (specific resource identity rides the open LOCATION)
    Universe,   // overlaps everything (an opaque call / a synchronization fence / a transaction boundary)
};

// How a §26 effect family accesses its class: does it read, does it write, and which class. ⛔ RandomRead writes (it
// mutates the stream); TimeRead is a pure read. `MemoryReadWrite` and the I/O / opaque families read AND write.
struct EffectAccess
{
    bool          reads;
    bool          writes;
    ResourceClass klass;
};

// The CLASSIFIER — a total switch over all §26 families. ⛔ NO default case: appending a 28th family without classifying
// it is a `-Werror=switch` COMPILE ERROR (the append-at-end guard, free). The conservative-correct judgment calls
// (Allocate/Dealloc/Residency in Memory so use-after-free shows; GPUCommand a write; I/O one rw class; ExternalCall +
// Synchronization = Universe; RandomRead a WRITE; TimeRead inert-read; Nondeterministic inert) are documented in §4d.
[[nodiscard]] constexpr EffectAccess effect_access(EffectFamily f) noexcept
{
    switch (f)
    {
    case EffectFamily::MemoryRead: return {true, false, ResourceClass::Memory};
    case EffectFamily::MemoryWrite: return {false, true, ResourceClass::Memory};
    case EffectFamily::MemoryReadWrite: return {true, true, ResourceClass::Memory};
    // lifecycle in the Memory class so Deallocate(R)-then-MemoryRead(R) use-after-free is visible (identical access)
    case EffectFamily::Allocate:
    case EffectFamily::Deallocate:
    case EffectFamily::ResourceResidency: return {false, true, ResourceClass::Memory};
    case EffectFamily::GPUCommand: return {false, true, ResourceClass::Gpu};
    case EffectFamily::HostStateRead: return {true, false, ResourceClass::HostState};
    case EffectFamily::HostStateWrite: return {false, true, ResourceClass::HostState};
    case EffectFamily::SceneRead: return {true, false, ResourceClass::Scene};
    case EffectFamily::SceneWrite: return {false, true, ResourceClass::Scene};
    case EffectFamily::EcsRead: return {true, false, ResourceClass::Ecs};
    case EffectFamily::EcsWrite: return {false, true, ResourceClass::Ecs};
    case EffectFamily::PhysicsRead: return {true, false, ResourceClass::Physics};
    case EffectFamily::PhysicsWrite: return {false, true, ResourceClass::Physics};
    case EffectFamily::AudioRead: return {true, false, ResourceClass::Audio};
    case EffectFamily::AudioWrite: return {false, true, ResourceClass::Audio};
    // one shared Io class — cross-channel reordering conservatively forbidden until a channel model exists
    case EffectFamily::FileIO:
    case EffectFamily::NetworkIO:
    case EffectFamily::DeviceIO: return {true, true, ResourceClass::Io};
    case EffectFamily::ExternalCall: return {true, true, ResourceClass::Universe};
    case EffectFamily::TimeRead: return {true, false, ResourceClass::Time};
    case EffectFamily::RandomRead: return {true, true, ResourceClass::Random}; // ⛔ advances the stream
    case EffectFamily::Nondeterministic: return {false, false, ResourceClass::None};
    case EffectFamily::Synchronization: return {true, true, ResourceClass::Universe};
    case EffectFamily::Logging: return {false, true, ResourceClass::Log};
    case EffectFamily::Debug: return {false, true, ResourceClass::Debug};
    // CEIR-8c (ADR-0113) U-§19 families. Read/write follows the family name; the class is the domain. ⛔ documented
    // judgment: TransactionBoundary is a Universe BARRIER (a commit/rollback orders every effect across it, like
    // Synchronization); AgentAction reads AND writes (an edit observes-then-mutates) — its specific resource rides the
    // open LOCATION, not the family, so the family class is the coarse Agent domain.
    case EffectFamily::DocumentRead: return {true, false, ResourceClass::Document};
    case EffectFamily::DocumentWrite: return {false, true, ResourceClass::Document};
    case EffectFamily::ConstraintRead: return {true, false, ResourceClass::Constraint};
    case EffectFamily::ConstraintWrite: return {false, true, ResourceClass::Constraint};
    case EffectFamily::TransactionBoundary: return {true, true, ResourceClass::Universe};
    case EffectFamily::UIRead: return {true, false, ResourceClass::Ui};
    case EffectFamily::UIWrite: return {false, true, ResourceClass::Ui};
    case EffectFamily::AgentAction: return {true, true, ResourceClass::Agent};
    }
    return {true, true, ResourceClass::Universe}; // unreachable (total switch); conservative if somehow hit
}

// Two ViewRange masks overlap iff either is 0 (the WHOLE resource) or they share a bit.
[[nodiscard]] constexpr bool range_overlap(u32 m1, u32 m2) noexcept
{
    return m1 == 0U || m2 == 0U || (m1 & m2) != 0U;
}

// ── CEIR-8f (ADR-0116 §2.2) the SAFETY axes (U-§23) — orthogonal to WHERE (EvalDomain). A PROJECTION of the effect
// families, NOT new declared data (extend-not-fork). `realtime_safe` is DERIVED (allocation-, block-, and IO-free). ──
struct SafetyBits
{
    bool may_allocate;
    bool may_block;
    bool may_io;
    [[nodiscard]] constexpr bool realtime_safe() const noexcept { return !(may_allocate || may_block || may_io); }
    [[nodiscard]] constexpr SafetyBits merged(SafetyBits o) const noexcept
    {
        return {may_allocate || o.may_allocate, may_block || o.may_block, may_io || o.may_io};
    }
};
// The classifier — a TOTAL switch over all 35 families (⛔ NO default: appending a family without classifying its
// safety is a `-Werror=switch` COMPILE ERROR — the append-at-end guard, exactly like effect_access; a mask/denylist
// predicate would be a THIRD family consumer INVISIBLE to that guard, the 8c hole). Documented judgment: the lifecycle
// families are may-allocate; File/Network/Device are may-IO; ExternalCall/Synchronization/TransactionBoundary/
// AgentAction AND GPUCommand (a submit can stall) are may-block; reads/writes/time/random/log/debug are none.
// ⛔ realtime_safe is STRICTER than effect_legal_in_region (semantics.hpp): that HARD gate permits Allocate in an
// audio-RT region (a soft cost, not a priority-inversion deadlock), so `realtime_safe ⟹ legal_in_RT` but not the
// converse — two questions over ONE vocabulary (§2.2), never two drifting oracles.
[[nodiscard]] constexpr SafetyBits effect_safety(EffectFamily f) noexcept
{
    switch (f)
    {
    case EffectFamily::Allocate:
    case EffectFamily::Deallocate:
    case EffectFamily::ResourceResidency: return {true, false, false};
    case EffectFamily::FileIO:
    case EffectFamily::NetworkIO:
    case EffectFamily::DeviceIO: return {false, false, true};
    case EffectFamily::GPUCommand:        // a command submission can stall on a full queue
    case EffectFamily::ExternalCall:
    case EffectFamily::Synchronization:
    case EffectFamily::TransactionBoundary:
    case EffectFamily::AgentAction: return {false, true, false};
    case EffectFamily::MemoryRead:
    case EffectFamily::MemoryWrite:
    case EffectFamily::MemoryReadWrite:
    case EffectFamily::HostStateRead:
    case EffectFamily::HostStateWrite:
    case EffectFamily::SceneRead:
    case EffectFamily::SceneWrite:
    case EffectFamily::EcsRead:
    case EffectFamily::EcsWrite:
    case EffectFamily::PhysicsRead:
    case EffectFamily::PhysicsWrite:
    case EffectFamily::AudioRead:
    case EffectFamily::AudioWrite:
    case EffectFamily::TimeRead:
    case EffectFamily::RandomRead:
    case EffectFamily::Nondeterministic:
    case EffectFamily::Logging:
    case EffectFamily::Debug:
    case EffectFamily::DocumentRead:
    case EffectFamily::DocumentWrite:
    case EffectFamily::ConstraintRead:
    case EffectFamily::ConstraintWrite:
    case EffectFamily::UIRead:
    case EffectFamily::UIWrite: return {false, false, false};
    }
    return {true, true, true}; // unreachable (total switch); maximally unsafe if somehow hit
}
} // namespace crd::ceir
