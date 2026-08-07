# ADR-0068 — Eylem body types + collision filtering + contact callbacks

> Status: **Accepted** (2026-05-11)
> Companions: [ADR-0062](0062-eylem-physics-architecture.md) (eylem
> architecture), [ADR-0063](0063-eylem-determinism-contract.md) (determinism
> contract), [ADR-0066](0066-draw-substrate-architecture.md) (`crd-draw`),
> [ADR-0067](0067-eylem-force-field-architecture.md) (force-field substrate).
> Research dossier: [`docs/research/cerid-eylem-collision-filtering.md`](../research/cerid-eylem-collision-filtering.md).

## Context

Three concerns sit on the same architectural plane and must be designed
together: **body types** (what kinds of dynamic objects participate),
**collision filtering** (which pairs the broadphase allows through), and
**contact callbacks** (how user code observes contact events). Surveyed
engines that designed these independently — PhysX in the 2000s, Bullet
in the 2000s, Unity classic — paid for it later: PhysX has three
generations of sensor handling, Bullet's filter API requires user-written
filter shaders for routine cases, Unity's layer matrix permutes between
projects when artists rename layers.

Cerid's mandate is broader than any shipped engine: **games + scientific
computing + robotics + aerospace + cinematic**. Each domain has different
filtering needs (games: layers; robotics: URDF self-collision matrices;
cinematic: per-shot overrides) and different callback patterns (games:
event-driven gameplay; robotics: deterministic RL training; cinematic:
mass-scale destruction without callback storms). The substrate must
serve all five without compromise.

The research dossier (`cerid-eylem-collision-filtering.md`, 10,370
words) surveys 9 game engines + MuJoCo + Drake + IsaacSim + Project
Chrono + AGX + Houdini Vellum + Maya nDynamics + Unreal Chaos
Destruction + Niagara across 5 mandate domains. This ADR locks the
recommendations from §10 "Recommended Cerid architecture".

The user-facing requirements that drove this ADR:

1. Three motion types — Static / Kinematic / Dynamic — with the universal
   interaction matrix
2. Sensor (overlap-only collider) as a first-class concept
3. Other body roles (character controllers, vehicles, articulation links,
   particles, geometry collections) compose via ECS, NOT via a body-type
   enum explosion
4. Collision filtering at multiple tiers — designer-authorable for the
   common case, expressive for the long tail, deterministic always
5. Per-pair "ignore this entity" semantics — the user's specific ask
6. Contact callbacks — Begin / Stay / End for both contacts + triggers,
   without callback storms in destruction scenes
7. ContactModify hook for one-way platforms / pickup-through-walls /
   conveyor friction / soft-contact attenuation
8. Determinism — bit-exact replay across MSVC/clang/gcc × x64/ARM per
   ADR-0063, including callback firing order
9. Performance budgets pinned in CI from day one
10. ECS-native everything — bodies, colliders, sensors, callbacks,
    filters all live as components / relations / event streams; existing
    scene tooling (öbek prefabs, query DSL, change detection) reuses
    transparently

## Decision

### 1. Body type catalogue: 3 motion types, no fourth

```cpp
enum class RigidBodyType : crd::u8
{
    Static    = 0, // never moves; infinite mass; collides with Kinematic + Dynamic
    Kinematic = 1, // user-driven; engine infers velocity from per-step pose delta
                   // (covers Maya "Animated Rigid Body" + MuJoCo mocap body)
    Dynamic   = 2, // fully simulated; participates in solver, gravity, sleep
};
```

Confirmed across 9+ surveyed engines (PhysX / Bullet / Jolt / Box2D v3
/ Unity classic + DOTS / Unreal Chaos / Godot / ODE / Project Chrono).
Robotics engines (MuJoCo / Drake / Chrono) collapse into this triple
via the articulation substrate (joints atop bodies, not joints AS
bodies). Maya's "Animated Rigid Body" and MuJoCo's mocap body collapse
into Kinematic with implicit velocity inference (when `set_body_state`
is called on a Kinematic body, the impl computes
`(new_pose − old_pose) / dt` and writes `linear_velocity` /
`angular_velocity`). Dynamic bodies struck by the kinematic body in the
next substep see the correct impulse.

**No fourth type.** The Godot taxonomy explosion (PhysicsBody3D /
RigidBody3D / StaticBody3D / CharacterBody3D / AnimatableBody3D) is
documented as friction-generating; we lock against that pattern.

### 2. Sensor as a per-collider flag

```cpp
struct ColliderFlags
{
    crd::u8 is_sensor : 1; // overlap-only; no contact response
    crd::u8 _reserved : 7;
};
```

Per-collider, NOT per-body. Confirmed across 8 of 9 surveyed engines
(PhysX, Box2D v3, Unity, Godot, Unreal, Bullet, ODE, MuJoCo). **Jolt is
the documented exception** — its per-body sensor model is the
modern-AAA-games tight-body-pool optimisation. Cerid follows the
8-engine majority because the multi-domain mandate demands the case
where one body has both solid colliders (a character's foot capsule)
AND sensor colliders (a proximity aura sphere) without splitting into
two bodies.

### 3. Specialised actor types via ECS composition

```cpp
// All composed atop one of the 3 motion types via ECS:
struct CharacterControllerComponent { /* capsule, slope limit, step height */ };
struct VehicleBodyComponent          { /* axle config, tyre model, gearbox */ };
struct ArticulationLinkComponent     { /* parent link id, joint topology */ };
struct SoftBodyComponent             { /* XPBD substrate, mesh ref */ };
struct GpuParticleComponent          { /* GPU particle pool ref */ };
struct GeometryCollectionComponent   { /* fractured mesh tree, post-v1 */ };
```

The character is a Kinematic body + `CharacterControllerComponent`. The
vehicle is a Dynamic body + `VehicleBodyComponent`. An articulation
link is a Dynamic body + `ArticulationLinkComponent`. This matches
PhysX (`PxArticulationLink` derives from `PxRigidBody`) and the modern
ECS-physics lineage (Unity DOTS, Bevy Rapier).

No specialised body-type enum value for any of these. ECS-native
composition is what Cerid was built to do.

### 4. Body-interaction matrix — universal 3×3 with one configurable corner

| | Static | Kinematic | Dynamic |
|---|---|---|---|
| **Static** | no contact | no contact | dynamic responds, static doesn't |
| **Kinematic** | no contact | no contact | dynamic responds, kinematic doesn't |
| **Dynamic** | dynamic responds | dynamic responds | both respond (full two-way) |

Sensors layer ORTHOGONAL: any sensor↔anything pair → overlap event, no
force.

**The single configurable corner is kinematic-vs-static overlap event
delivery, default OFF.** PhysX behaviour. Designers who want "moving
platform arrived at end-of-track sensor" opt in via the filter
(§5 Tier 1 layer setup). The matrix itself does not branch —
Kinematic-vs-Kinematic remains "no contact" because no sane impulse is
defined.

### 5. Collision filtering — five tiers, no PhysX-style filter shader

A pair survives only if every tier passes it. Ordering reflects cost;
expressing the same policy at a cheaper tier is always preferable.

#### Tier 1 — bit-mask layers (mutual consent, 64-bit) — ~3 cycles

```cpp
struct CollisionLayer
{
    crd::u64 belongs_to    = 0x0000'0000'0000'0001ULL; // default = layer 0 only
    crd::u64 collides_with = 0xFFFF'FFFF'FFFF'FFFFULL; // default = collide with all
};
// Mutual-consent rule:
//   collide ⟺ (A.belongs_to & B.collides_with) != 0
//          && (B.belongs_to & A.collides_with) != 0
```

**64-bit, not 32-bit.** Robotics + cinematic workflows exhaust 32
channels quickly (per-robot sensor categories × multiple robots × env
categories). Doubling to 64 costs nothing and removes future
renumbering pain.

Designer-authored layer-name table (Unity-style: layer 0 = "Default",
layer 1 = "Player", ...) lives in the project's TOML config; the
engine knows only the bits.

#### Tier 2 — group index (Box2D-style override) — ~1 cycle

Per-collider signed `i16`. Cheapest tier and the cleanest solution to
ragdoll self-collision policy.

```
group_index > 0  + same value  → ALWAYS collide (override Tier 1 false)
group_index < 0  + same value  → NEVER  collide (override Tier 1 true)
group_index == 0                → fall through to Tier 1
different non-zero values       → fall through to Tier 1
```

Use negative groups for "this set of colliders should never
self-collide" (ragdoll limbs). Positive groups for "this set should
always collide regardless of mask" (rare; debug visualisation,
gameplay-scripted interactions).

#### Tier 3 — explicit excluded pairs — ~10–20 cycles

```cpp
class IPhysicsScene
{
    virtual void exclude_pair(BodyId a, BodyId b) noexcept = 0;
    virtual void include_pair(BodyId a, BodyId b) noexcept = 0;
    [[nodiscard]] virtual bool is_pair_excluded(BodyId a, BodyId b) const noexcept = 0;
};
```

Internal storage = hash set of `(min(a.raw), max(b.raw))` tuples; O(1)
test per pair. Round-trips with URDF / SDF / MJCF importers in v4
(articulation slice). Robotics scenes routinely declare 100+ explicit
self-collision exclusions per articulated chain.

#### Tier 4 — ECS-native predicate with closed read set — ~50–500 cycles

```cpp
class ICollisionPredicate
{
public:
    virtual bool should_collide(const PredicateInputView& a,
                                const PredicateInputView& b) const noexcept = 0;
};
```

PhysX-filter-shader expressiveness without the determinism trap. **One
predicate per scene**; pure function of substep-start state; **read set
declared at registration time**, PHYSICALLY enforced at the API surface
(the `PredicateInputView` exposes only the declared component types).

Bevy Rapier's `BevyPhysicsHooks` formalises this via `SystemParam`;
Cerid's version is one step stricter — the read set is *declared*, not
inferred from the closure; the API surface forbids reads outside the
declaration. This bakes ADR-0063 compliance into the API rather than
leaving it as a documentation constraint.

#### Tier 5 — articulation / joint implicit auto-filter — ~5 cycles

`Joint::collide_connected = false` (default) auto-disables contact
between joint endpoints. `ArticulationLinkComponent`'s
`self_collision_enabled = false` (default) auto-disables contact across
the entire chain, with an explicit allowlist of (link_a, link_b) pairs
that *do* collide despite the chain default. Standard URDF/SDF/MJCF
pattern; importers consume directly.

Surface lives on `Joint` / `ArticulationLinkComponent`, not in
`collision_filter.hpp` per se — these are properties of the
articulation structure, not per-pair filter knobs.

#### What this REJECTS

- **PhysX-style `PxFilterData` filter shader.** Expressiveness recovered
  by Tier 4. Determinism cost unacceptable under ADR-0063 (PhysX is
  documented as not cross-platform deterministic; the filter shader is
  the proximate cause).
- **Jolt's two-tier broadphase-layer split (`EObjectLayer` ×
  `EBroadPhaseLayer`).** Perf optimisation, not a different filter.
  Revisit only if v1 profiling shows broadphase pair generation is hot.

### 6. Contact callback dispatch — deferred ECS event-stream model

```cpp
struct ContactEvent
{
    enum class Kind : crd::u8 { Begin = 0, Persist = 1, End = 2 };
    Kind             kind;
    BodyId           body_a;
    BodyId           body_b;
    ColliderId       collider_a;
    ColliderId       collider_b;
    crd::math::Vec3f contact_point_world;
    crd::math::Vec3f normal_world;          // points from a to b
    crd::f32         penetration_depth;
    crd::f32         normal_impulse;        // valid only after solve
};

struct TriggerEvent
{
    enum class Kind : crd::u8 { Enter = 0, Stay = 1, Exit = 2 };
    Kind       kind;
    BodyId     body_a;
    BodyId     body_b;
    ColliderId collider_a;
    ColliderId collider_b;
};
```

**NOT synchronous virtual callbacks** (PhysX's
`PxSimulationEventCallback` mistake — incompatible with ADR-0063 the
moment narrow phase fans to fibres; callback firing order would depend
on thread arrival order).

Both events are written into ECS event buffers in `PostPhysics` phase;
user systems iterate in the next phase. Sort key for deterministic
delivery: `(min(body_a, body_b), max(body_a, body_b), kind)` —
identical event sets produce identical iteration order across machines
regardless of which fibre generated which contact.

#### Persist / Stay opt-in per pair

`Begin` and `End` are first-class for both contact + trigger events.
**`Persist` / `Stay` are OPT-IN per pair via
`ContactPairFlags::report_persist`, default OFF.** This is the Box2D v3
default and prevents destruction-scene event storms (10K contacts/frame
× 3 events each = 30K user-code invocations is unrecoverable).

Default lifecycle is **Begin / End only**; gameplay derives "currently
touching" by accumulating begins minus ends. Designers opt INTO
Persist for specific pairs that need it (e.g., gun barrel touching
enemy for damage-over-time, conveyor friction integration).

#### Drain semantics

```cpp
class IPhysicsScene
{
    [[nodiscard]] virtual crd::containers::ConstSpan<ContactEvent>
        drain_contact_events() noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<TriggerEvent>
        drain_trigger_events() noexcept = 0;
};
```

Returned span is valid until the next `step()` call (events live in
scene-managed scratch from `PhysicsConfig::solver_scratch`). Caller may
iterate but must not retain pointers past the next step. ECS-native
event-system integration in the user system that follows `PostPhysics`.

### 7. ContactModify hook — separate v1g+ API surface

```cpp
struct ContactPoint
{
    crd::math::Vec3f point_world;
    crd::math::Vec3f normal_world;
    crd::f32         penetration_depth;
    crd::f32         friction_override;    // < 0 = use material; ≥ 0 = override
    crd::f32         restitution_override;
    crd::math::Vec3f surface_velocity;     // for conveyor / wheel materials
    crd::u32         feature_id;           // stable hash for warm-start cache
    bool             enabled;              // set false to disable this contact
};

class IContactModifyCallback
{
public:
    virtual void modify_contacts(BodyId a, BodyId b,
                                 crd::containers::Span<ContactPoint> contacts) noexcept = 0;
};
```

**API-enforced purity.** The signature provides `BodyId` +
`Span<ContactPoint>` only — NO World handle, NO RNG, NO time, NO
external state. Pure function of contact data + body ids.

Cerid sorts the post-modify contact arrays by stable feature id before
the solver consumes them, recovering determinism even though the
callback fires in fibre-arrival order.

Use cases (from research dossier §6.4): one-way platforms (zero
contacts where `dot(normal, jump_dir) > 0`), pickup-through-walls (zero
contacts when either body has a "phasing" tag), conveyor friction
(override `surface_velocity`), attenuated soft contact (scale impulse
by material softness via `friction_override` / `restitution_override`).

**Ships with v1g (after v1d basic dispatch is stable).** API surface
reserved here so v1l API freeze covers it.

### 8. Determinism contract — baked in from day one

Per ADR-0063 the substrate participates in cross-platform replay-hash
CI. Six constraints are pinned at the API surface level:

1. **All event buffers sorted by `(min(body_a, body_b), max(body_a,
   body_b), kind)`** before user delivery. Hard requirement; no
   opt-out.
2. **All filter tiers are pure functions of substep-start state.** No
   filter tier reads runtime-mutable external state. Tier 4 enforces
   this via the closed-read-set declaration.
3. **Multi-threaded narrow-phase merge follows ADR-0063 §4** —
   pre-reserved per-fibre slots, merge in stable id order, no atomic
   counters.
4. **Contact warm-start cache keyed by `(body_a_id, body_b_id,
   feature_id)`** — never by pointer. The `feature_id` field on
   `ContactPoint` is a stable hash of the contact-feature pair (e.g.,
   "vertex 3 of A on face 7 of B").
5. **Sleep thresholds use absolute energy**, not history-window
   averages (ADR-0063 §1).
6. **ContactModify callback is a pure function** of contact data + body
   ids; the API surface does not expose external state.

CI assertions: the v1j replay-hash test (10 seconds of contact-rich
scene → snapshot hash matches across MSVC / clang / gcc × x64 / ARM
× Windows / Linux). Regression fails the build.

### 9. Robotics-specific extensions

- **Force/torque sensors at joint frames.** First-class component
  `JointForceSensorComponent` on the joint entity, populated each step
  from the constraint impulse (Isaac Sim / Drake pattern). Lives in the
  joints substrate (v1f), not the filter substrate; cross-referenced
  here.
- **Self-collision per-link-pair toggle.** `ArticulationLinkComponent`
  carries an explicit allowlist of (link_a, link_b) pairs that *do*
  collide despite the articulation's default-off self-collision.
  Standard robotics pattern; URDF / SDF importers consume directly.
- **Contact-area integrators.** Out of scope for v1; revisit when FEM /
  hydroelastic contact lands in v7. The use case is medical simulation
  needing pressure-distributed contact.
- **Cross-reference (added 2026-05-11):** the broader sensor substrate
  (IMU / LIDAR / proximity / threshold-event sensors / diagnostic
  sensors) is locked in [ADR-0072](0072-eylem-sensor-substrate.md).
  ADR-0072 consumes this ADR's contact-event stream + adds the
  raycast / threshold sensor catalog.

### 10. Cinematic-specific extensions

- **Geometry collections / mass-scale destruction.** Post-v1, as
  `GeometryCollectionComponent` (composition pattern) atop dynamic
  bodies. The substrate is unchanged; the new component is a spawn
  pattern that emits dynamic bodies on break events. Reuses the
  existing dispatch model (Begin event on parent, OnDestroy on parent
  + OnSpawn on children).
- **Sub-frame contact for motion-blur correctness.** Out of scope for
  v1. The use case is film cinematic motion blur where physics steps
  at 60 Hz but the camera samples at 120 Hz with shutter open across
  half the frame. The clean solution interpolates contact points
  between substeps (the variable-rate presentation step in ADR-0063
  §1 — physics fixed-step + presentation interpolated). Revisit if
  cinematic studios using Cerid surface a concrete need.
- **Cross-reference (added 2026-05-11):** the broader cinematic /
  animation-physics bridge (animation curves driving kinematic bodies,
  pre-roll simulation, per-shot physics overrides, slow-motion
  substepping) is locked in [ADR-0074](0074-eylem-cinematic-bridge.md).
  ADR-0074 consumes this ADR's filter + callback substrate from the
  cinematic side; the Kinematic body's velocity-inference contract
  (§1) is what makes animation-curve-driven kinematic bodies push
  dynamic bodies correctly.

### 11. Performance budget targets

Per the research dossier §6, calibrated against published Niagara,
PhysX, and Box2D v3 numbers. CI assertions in
`tests/eylem-rigid3d/bench_filter.cpp` shipped with v1d.

| Workload | Budget |
|---|---|
| Broadphase pair generation + bit-mask reject (10k bodies, well-set-up layers) | ≤ 0.3 ms |
| ECS predicate evaluation (1% of pairs) | ≤ 0.1 ms |
| Excluded-pair set test (URDF self-collision matrix on 30-link humanoid) | ≤ 0.05 ms |
| Articulation auto-filter (30-link humanoid) | ≤ 0.02 ms |
| Contact event dispatch (1k events typical) | ≤ 0.05 ms |
| Contact event dispatch (10k events peak — destruction) | ≤ 0.5 ms |
| ContactModify (when active, 1% of pairs) | ≤ 0.05 ms |

Regressions fail the build, same model the Phase 2.5 jobs benchmarks
use. Measured on Zen 4 / Raptor Lake; NEON 4-lane M-series scales
to ~2× the budget.

### 12. Slice plan (locked)

Slot into Phase 3.1 v1 across the existing v1c → v1d → v1f → v1g
slices. Nine sub-slices, ~1500 LOC + ~33 tests + 2 benches:

| Sub-slice | Scope | LOC | Tests |
|---|---|---|---|
| **v1c-sensor** | `ColliderFlags::is_sensor` (✅ shipped 2026-05-11 in `collider.hpp`) + scene-component round-trip. | ~50 | ~3 |
| **v1d-filter-a** | Tier 1 (bit-mask layers, 64-bit) + Tier 2 (group index) + filter eval pipeline. Bench: layer reject rate. | ~250 | ~6 |
| **v1d-filter-b** | Tier 3 (explicit excluded pairs) + storage (hash set). Bench: 30-link humanoid self-collision. | ~150 | ~3 |
| **v1d-filter-c** | Tier 4 (ECS predicate) + closed-read-set enforcement at API surface. Bench: 1% pair predicate cost. | ~200 | ~4 |
| **v1d-callback-a** | Deferred event streams: `ContactEvent` + `TriggerEvent` ECS buffers; `EylemPostPhysicsSystem` writes; sort by id-pair. | ~300 | ~6 |
| **v1d-callback-b** | Per-pair `ContactPairFlags`: opt-in for `Persist` / `Stay`; opt-in for contact-point detail; default Begin / End only. | ~100 | ~3 |
| **v1d-callback-c** | Bench suite: callback storm (10k events / step destruction) + budget assertions. | ~50 | bench |
| **v1f-articulation-filter** | Tier 5 articulation auto-filter (lands with the joint slice that introduces articulations). Per-link-pair allowlist. | ~150 | ~3 |
| **v1g-contactmodify** | `IContactModifyCallback` API + pure-function constraint + post-modify sort to recover determinism. Use cases: one-way platform sandbox demo. | ~250 | ~5 |

**Dependencies:**
- `v1d-filter-a/b`, `v1d-callback-a/b/c` unblocked at v1d entry.
- `v1d-filter-c` depends on the ECS query system (already shipped, ADR-0052).
- `v1f-articulation-filter` ships with v1f (joints), naturally.
- `v1g-contactmodify` ships once v1d is stable; not a v1 close
  requirement.

API surface freezes at **v1l** (v1 close). The v1g `ContactModify`
hook is a separate API surface — adding it post-freeze is a *new*
surface, not a modification of frozen surface (same discipline as
ADR-0067's blocked sub-slices).

## Rationale

### Why no fourth body type

Every surveyed engine collapses Maya "Animated Rigid Body" + MuJoCo
mocap body into Kinematic-with-velocity-inference. The Godot taxonomy
explosion (RigidBody3D / StaticBody3D / CharacterBody3D /
AnimatableBody3D / PhysicsBody3D base) is documented as
friction-generating — designers learn five types when three would do.
Locking against this pattern locks the API surface against
body-type-explosion drift.

### Why per-collider sensor flag, not per-body sensor type

8 of 9 surveyed engines do per-collider. The decisive case for Cerid:
a single character entity carrying both a solid foot capsule and a
proximity aura sphere without splitting into two bodies. Robotics:
end-effector with proximity sensor next to solid finger geometry. The
multi-domain mandate breaks Jolt's per-body model.

### Why 64-bit layer mask, not 32-bit

Robotics + cinematic workflows exhaust 32 channels quickly.
Per-robot sensor categories (proximity, force-torque, vision) ×
multiple robots in a cell × environment categories (floor, wall,
ceiling, dynamic obstacles, static obstacles) routinely passes 32 in
non-trivial scenes. Doubling to 64 costs nothing — same memory
footprint pre-padding given typical struct alignment — and removes the
future renumbering pain.

### Why five filtering tiers, not three

The dossier survey turned up five distinct filtering needs each engine
addresses differently. Compressing them into three tiers forces every
case through a more-expensive path:

- Tier 2 (Box2D-style group index) handles ragdoll self-collision in
  ONE cycle. Without it, every limb pair burns Tier 4 (~50–500 cycles)
  on every broadphase pair.
- Tier 5 (articulation auto-filter) is structural metadata — joint
  endpoints don't collide by default. Expressing this through Tier 1
  layers requires designers to allocate layer bits per articulation;
  expressing through Tier 4 burns ECS query cost. Reading the
  adjacency bit on the articulation graph is ~5 cycles.

Each tier earns its existence by either expressiveness (Tier 4) or
performance (Tiers 1, 2, 5).

### Why deferred ECS event streams, not synchronous callbacks

Synchronous virtual-function callbacks (PhysX
`PxSimulationEventCallback`) break determinism the moment narrow phase
fans to fibres — callback firing order depends on thread arrival
order. Cerid commits to fibre-jobified narrow phase from day one
(ADR-0062 §3). The deferred event-stream model is the only model
compatible with this commitment.

The bonus: ECS event streams compose naturally with crd-scene's
existing query system (ADR-0052). User systems iterate events with
the same Query DSL they use for entity queries. Zero new authoring
surface.

### Why Persist/Stay opt-in per pair

Mass-scale destruction scenes routinely generate 10K contact pairs
per frame. With Persist/Stay events firing on every pair every frame,
that's 30K+ user-code invocations per step — unrecoverable in the
typical sub-millisecond budget. Box2D v3's default-off Persist
prevents this. Designers opt IN per pair only where Persist matters
(damage-over-time, conveyor integration).

The Begin/End delta is ~2-4 events per pair total over its contact
lifetime — bounded regardless of duration. Default lifecycle is the
right answer for the 95% case.

### Why ContactModify is a separate v1g+ slice

The basic Begin/End/Persist dispatch in v1d is the complete core
contract. ContactModify is a power-user feature with a sharper API
boundary (synchronous mid-step, pure-function-constrained). Shipping
it separately:

- Lets v1d ship without committing to the ContactModify surface
  (which depends on the contact-feature warm-start cache layout).
- Forces designers to use Begin/End/Persist for the 90% case — power
  features get used last, by design.
- Cleanly handles the post-API-freeze path for the v1g shipment (new
  surface added to a frozen surface set, same as ADR-0067's blocked
  sub-slices).

### Why no PhysX-style filter shader

The expressiveness PhysX's filter shader buys is recovered by Tier 4
(ECS predicate). The cost it pays — non-deterministic execution
because the shader can read shared state and may run in any order — is
unacceptable under ADR-0063. Cerid's mandate explicitly demands
cross-platform deterministic replay; PhysX explicitly does not (O3DE
disables PhysX determinism in production).

## Consequences

**Positive:**
- One substrate covers all five mandate domains. No "but the cinematic
  pipeline doesn't have ragdoll self-collision matrices."
- Designer-authorable for the 90% case via layer matrix + group index +
  Begin/End events.
- Robotics workflows unblocked by Tier 3 + Tier 5 + per-collider
  sensors.
- Cinematic destruction scenes don't drown in callback storms.
- Determinism pre-baked at the architecture level (id-stable
  ordering, content-addressed events, closed-read-set predicates).
- ECS-native event delivery composes with the existing scene query DSL.
- Performance budgets pinned in CI from day one.

**Negative:**
- Five-tier filter pipeline is more code than two-tier (Bullet) or
  three-tier (Unity). Mitigation: each tier is small (~150–250 LOC) and
  independently testable.
- Closed-read-set enforcement at the Tier 4 API surface requires a
  custom `PredicateInputView` type that reflects the registration's
  declared component type-set. Implementation complexity in
  crd-eylem-rigid3d.
- ContactModify pure-function constraint is API-enforced (no World
  handle in the signature) but the typical use case (gameplay-driven
  one-way platforms) sometimes wants to read a small amount of state
  (e.g., the player's jump-direction flag). Mitigation: state is
  passed via the body's `ContactPairFlags` or via a side-channel
  component the predicate already has access to (Tier 4).

**Neutral:**
- Substrate adds ~1500 LOC + ~33 tests + 2 benches to v1. Material
  but proportional to the value (a substrate every Cerid solver and
  every gameplay system consumes).
- v1g `ContactModify` ships post-v1l-freeze — same discipline as
  ADR-0067's blocked sub-slices (Gradient, Script).

## References

**Research:** [`docs/research/cerid-eylem-collision-filtering.md`](../research/cerid-eylem-collision-filtering.md)
— full industry survey across PhysX, Bullet, Havok, Jolt, Box2D v3,
Unity DOTS Physics + classic, Unreal Chaos, Godot, MuJoCo, Drake,
NVIDIA IsaacSim/Lab, Project Chrono, AGX Dynamics, Houdini Vellum,
Maya nDynamics, Niagara, Chaos Destruction; algorithm references;
determinism failure modes; performance benchmarks.

**Companion ADRs:**
- [ADR-0050](0050-scene-storage-backends.md) — sparse-set storage rationale
- [ADR-0052](0052-scene-query-system-schedule.md) — ECS query DSL (consumed by Tier 4)
- [ADR-0058](0058-obek-system.md) — öbek prefab system (round-trips ColliderFlags + filter state)
- [ADR-0062](0062-eylem-physics-architecture.md) §3 (broadphase reuse), §4.5 (collider model), §6 (ECS integration)
- [ADR-0063](0063-eylem-determinism-contract.md) — determinism contract
- [ADR-0066](0066-draw-substrate-architecture.md) §11–12 — VisualizerRegistry pattern (used by eylem-viz to render contact normals + force arrows)
- [ADR-0067](0067-eylem-force-field-architecture.md) — force-field substrate (composes with sensor + trigger semantics)
