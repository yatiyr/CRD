#pragma once

// Collision filtering + contact / trigger event surface for eylem.
// Phase 3.1 v1c-sensor + v1d-filter-{a,b,c} + v1d-callback-{a,b,c}
// (planned). API frozen here at v1l (ADR-0062 §15).
//
// Architecture: ADR-0068 (eylem body types + collision filtering + callback
// architecture). Determinism contract: ADR-0063. Industry survey + tier
// rationale: docs/research/cerid-eylem-collision-filtering.md.
//
// Five filtering tiers, evaluated cheapest-first; a pair survives only if
// every tier passes it:
//
//   1. Bit-mask layers (mutual consent, 64-bit)              ~3 cycles
//   2. Group index (Box2D-style override, signed)            ~1 cycle
//   3. Explicit excluded pairs (URDF / IsaacSim path)        ~10-20 cycles
//   4. Articulation / joint implicit auto-filter             ~5 cycles
//   5. ECS-native predicate with closed read set             ~50-500 cycles
//
// (Numbering matches ADR-0068 §10.4. The articulation auto-filter is
// declared in `joint.hpp` / articulation surface, not here, because it's
// a property of the articulation/joint structure, not a per-pair filter
// knob.)
//
// IMPL STATUS — every type below is the public surface only. `EylemSystem`
// (impl in crd-eylem-rigid3d) wires the filter pipeline + event dispatch
// across v1c-sensor → v1d-filter-a → v1d-filter-b → v1d-filter-c →
// v1d-callback-a → v1d-callback-b → v1d-callback-c. ContactModify
// (`IContactModifyCallback`) is the v1g+ surface preview — included here
// for API freeze even though impl waits.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::scene
{
class World;
}

namespace crd::eylem
{
// ---------------------------------------------------------------------------
// Tier 1 — bit-mask layers (mutual consent)
// ---------------------------------------------------------------------------
//
// Per-collider 64-bit category + mask. Larger than the typical 32-bit
// ceiling because robotics + cinematic workflows exhaust 32 channels
// quickly (per-robot sensor categories × multiple robots in a cell × env
// categories). Doubling to 64 bits costs nothing and removes the future
// renumbering pain.
//
// Mutual-consent collide rule:
//     collide ⟺ (A.belongs_to & B.collides_with) != 0
//             && (B.belongs_to & A.collides_with) != 0
//
// Designer-authored layer-name table (Unity-style: layer 0 = "Default",
// layer 1 = "Player", ...) lives in the project's TOML config; the
// engine knows only the bits.
struct CollisionLayer
{
    crd::u64 belongs_to    = 0x0000'0000'0000'0001ULL; // default = layer 0 only
    crd::u64 collides_with = 0xFFFF'FFFF'FFFF'FFFFULL; // default = collide with all
};

static_assert(sizeof(CollisionLayer) == 16, "CollisionLayer must pack to 16 bytes");

// ---------------------------------------------------------------------------
// Tier 2 — group index (Box2D-style override)
// ---------------------------------------------------------------------------
//
// Per-collider signed i16. Cheapest tier (~1 cycle) and the cleanest
// solution to ragdoll self-collision policy.
//
//   group_index > 0  + same value  → ALWAYS collide  (override Tier 1 false)
//   group_index < 0  + same value  → NEVER  collide  (override Tier 1 true)
//   group_index == 0                → fall through to Tier 1
//   different non-zero values       → fall through to Tier 1
//
// Use `kCollisionGroupNone` (0) for "no group" colliders. Negative groups
// for "this set of colliders should never self-collide" (ragdoll limbs).
// Positive groups for "this set should always collide regardless of mask"
// (rare; debug visualisation, gameplay scripted collisions).
inline constexpr crd::i16 kCollisionGroupNone = 0;

// ---------------------------------------------------------------------------
// Tier 3 — explicit excluded pairs
// ---------------------------------------------------------------------------
//
// `IPhysicsScene::exclude_pair(a, b)` / `include_pair(a, b)` /
// `is_pair_excluded(a, b)` (declared in physics_scene.hpp). Internal
// storage = hash set of `(min(a.raw), max(a.raw))` tuples; O(1) test per
// pair. Round-trips with URDF / SDF / MJCF importers in the v4
// articulation slice — robotics scenes routinely declare 100+ explicit
// self-collision exclusions per articulated chain.
//
// (No types declared here — surface lives on IPhysicsScene; this comment
// is the doc anchor for ADR-0068 §10.4 Tier 3.)

// ---------------------------------------------------------------------------
// Tier 4 — ECS-native predicate hook (closed read set)
// ---------------------------------------------------------------------------
//
// PhysX-filter-shader expressiveness without the determinism trap. A
// scene registers ONE predicate; the predicate is a pure function of ECS
// component state at substep boundary; the read set is declared at
// registration and PHYSICALLY enforced by the API surface (the
// `EntityComponentRefs` view exposes only the declared types).
//
// Bevy Rapier's `BevyPhysicsHooks` formalises this via `SystemParam`;
// Cerid's version is one step stricter — the read set is *declared*, not
// inferred, baking ADR-0063 compliance into the API rather than leaving
// it as a documentation constraint.

// Forward declaration; concrete type lives in crd-scene at the impl-
// module layer (eylem-rigid3d) since the read set type-set is
// scene-bound.
class PredicateInputView;

class ICollisionPredicate
{
public:
    virtual ~ICollisionPredicate() = default;

    // Pure function of substep-start ECS state. Reads forbidden: World
    // handle, RNG, time, file system, network. Cerid lints these out at
    // the API surface (the view exposes only declared component types;
    // the function signature does not provide handles to mutable state).
    [[nodiscard]] virtual bool should_collide(const PredicateInputView& a,
                                              const PredicateInputView& b) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Tier 5 — articulation / joint implicit auto-filter
// ---------------------------------------------------------------------------
//
// Bodies connected by a joint don't collide by default
// (`Joint::collide_connected = false`); articulations carry a
// `self_collision_enabled = false` default plus an explicit allowlist of
// (link_a, link_b) pairs that *do* collide. Standard URDF/SDF/MJCF
// pattern; importers consume it directly.
//
// Surface lives on `Joint` / `ArticulationLinkComponent` (joints +
// articulations); not declared here. This comment is the doc anchor for
// ADR-0068 §10.4 Tier 5.

// ===========================================================================
// Contact + trigger events — deferred ECS event-stream dispatch
// ===========================================================================
//
// Per ADR-0068 §10.5 + ADR-0063 §6: callbacks are NOT synchronous virtual
// functions (incompatible with fiber-fan-out determinism — PhysX's
// mistake). Eylem writes events into ECS buffers in `PostPhysics` phase;
// user systems iterate in the next phase.
//
// Events are sorted by `(min(body_a, body_b), max(body_a, body_b), kind)`
// before user delivery; identical event sets produce identical iteration
// order across machines regardless of which fibre generated which contact.
//
// `Begin` and `End` are first-class for both contact + trigger events.
// `Persist` / `Stay` are OPT-IN per pair via `ContactPairFlags::report_persist`
// (default OFF) — destruction scenes routinely generate 10K contacts/frame,
// 30K events at 3-events-per-pair would be unrecoverable. Designers opt
// IN for specific pairs that need it (e.g., gun barrel touching enemy for
// damage-over-time).

struct ContactEvent
{
    enum class Kind : crd::u8
    {
        Begin   = 0, // first substep contact established
        Persist = 1, // contact persisted (opt-in via ContactPairFlags::report_persist)
        End     = 2, // first substep contact lost
    };

    Kind             kind;
    crd::u8          _pad0[3];
    BodyId           body_a;
    BodyId           body_b;
    ColliderId       collider_a;
    ColliderId       collider_b;
    crd::math::Vec3f contact_point_world{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f normal_world       {0.0F, 1.0F, 0.0F}; // points from a → b
    crd::f32         penetration_depth = 0.0F;
    crd::f32         normal_impulse    = 0.0F; // valid only on End / after-solve Persist; 0 on Begin
};

// 1 (kind) + 3 (pad) + 4×4 (BodyIds + ColliderIds) + 12×2 (Vec3fs) + 4×2 (f32s) = 52 bytes.
static_assert(sizeof(ContactEvent) == 52, "ContactEvent must pack to 52 bytes");

struct TriggerEvent
{
    enum class Kind : crd::u8
    {
        Enter = 0,
        Stay  = 1, // opt-in via ContactPairFlags::report_persist (same flag as Contact Persist)
        Exit  = 2,
    };

    Kind       kind;
    crd::u8    _pad0[3];
    BodyId     body_a;
    BodyId     body_b;
    ColliderId collider_a;
    ColliderId collider_b;
};

static_assert(sizeof(TriggerEvent) == 20, "TriggerEvent must pack to 20 bytes");

// Per-pair event reporting flags. Set by the predicate (Tier 4) when it
// returns true for a pair; cached by the engine until the pair separates.
// Default = ReportBeginEnd (cheapest universal lifecycle); designers opt
// in to Persist + ContactDetail for specific pairs that need them.
struct ContactPairFlags
{
    crd::u8 report_begin_end  : 1; // default ON  — Begin + End events
    crd::u8 report_persist    : 1; // default OFF — Persist / Stay events every substep
    crd::u8 report_contact_pt : 1; // default OFF — populate contact_point + normal_impulse
                                   //                (Begin + End include zero-cost basics regardless)
    crd::u8 _reserved         : 5;
};

static_assert(sizeof(ContactPairFlags) == 1, "ContactPairFlags must pack to 1 byte");

inline constexpr ContactPairFlags kDefaultContactPairFlags{
    /*report_begin_end =*/ 1,
    /*report_persist   =*/ 0,
    /*report_contact_pt=*/ 0,
    /*_reserved        =*/ 0
};

// ===========================================================================
// ContactModify — synchronous mid-step pure-function hook (v1g+)
// ===========================================================================
//
// Per ADR-0068 §10.6: lets user code MUTATE contact points BEFORE the
// solver runs them. Use cases: one-way platforms (zero contacts where
// dot(normal, jump_dir) > 0), pickup-through-walls (zero contacts when
// either body has a "phasing" tag), conveyor friction (override contact
// surface velocity), attenuated soft contact (scale impulse by material
// softness).
//
// API-enforced purity: signature provides BodyId + Span<ContactPoint>
// only — NO World handle, NO RNG, NO time, NO external state. Cerid sorts
// the post-modify contact arrays by stable feature id before the solver
// consumes them, recovering determinism even though the callback fires
// in fibre-arrival order.
//
// Ships with v1g (after v1d basic dispatch is stable). Reserved as API
// surface here so v1l freeze covers it.

struct ContactPoint
{
    crd::math::Vec3f point_world{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f normal_world{0.0F, 1.0F, 0.0F};
    crd::f32         penetration_depth = 0.0F;
    crd::f32         friction_override = -1.0F; // < 0 = use material friction; ≥ 0 = override
    crd::f32         restitution_override = -1.0F; // same convention
    crd::math::Vec3f surface_velocity{0.0F, 0.0F, 0.0F}; // for conveyor/wheel materials
    crd::u32         feature_id = 0; // stable hash of contact-feature pair (for warm-start cache)
    bool             enabled    = true; // set false to disable this contact point
};

static_assert(sizeof(ContactPoint) == 56, "ContactPoint must pack to 56 bytes");

class IContactModifyCallback
{
public:
    virtual ~IContactModifyCallback() = default;

    // Pure function of contact data + body ids. NO World handle, NO RNG,
    // NO time, NO external state. The argument types do not expose them.
    // Called from the narrow-phase fibre that produced the contact;
    // Cerid sorts post-modify contact arrays by stable feature id before
    // the solver consumes them, recovering determinism.
    virtual void modify_contacts(BodyId                              a,
                                 BodyId                              b,
                                 crd::containers::Span<ContactPoint> contacts) noexcept = 0;
};

} // namespace crd::eylem
