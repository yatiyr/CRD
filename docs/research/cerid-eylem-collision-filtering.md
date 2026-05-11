# Cerid Eylem — Body Types, Collision Filtering, and Contact Callbacks

> **Companion to** [`cerid-eylem.md`](cerid-eylem.md) and
> [`cerid-eylem-fields.md`](cerid-eylem-fields.md). Those backed
> ADR-0062 (architecture) + ADR-0063 (determinism contract) +
> ADR-0067 (force fields). This file backs **ADR-0068 (eylem body
> types, collision filtering, and contact callbacks)** and pins the
> per-collider sensor model, the multi-tier filtering surface, the
> deferred ECS event-stream callback model, and the optional
> mid-step `ContactModify` hook.
>
> **Date:** 2026-05-11. **Audience:** anyone working on
> `crd-eylem`'s collision substrate who needs the *why* behind an
> architecture choice. The *what* is in ADR-0068 and the phase plan.
>
> **Scope:** body-type catalogue across domains (games + robotics +
> aerospace + cinematic + animation), the broadphase + narrow-phase
> filtering surface, sensor / trigger semantics, contact event
> dispatch, contact modification, and the determinism implications of
> all four. NOT joint topology, articulation algebra, or the solver
> itself — those have their own ADRs (joints v1f, articulations v4).

---

## 1. Why this is a substrate decision, not three independent ones

Body type, collision filtering, and contact callbacks are the three
faces of one underlying question: **how does the engine decide which
pairs of geometry interact, and what does the consumer learn about
those interactions?** Every shipped engine answers this question with
a tightly-coupled triple — change one face and the others have to
follow. PhysX's `PxSimulationFilterShader` makes sense only because
PhysX's deferred-event model lets the engine batch across cores;
Bullet's per-broadphase-pair `gContactProcessedCallback` makes sense
only because Bullet runs single-threaded; Jolt's `ContactListener`
sorts bodies by id specifically so the dispatch order is deterministic
even though the narrow phase fans out to fibres.

Cerid's mandate is broader than any single shipped engine. The same
substrate must support:

- **Games** (PhysX/Jolt-class workloads): tens of thousands of dynamic
  rigids, character controllers, vehicles, ragdolls, destruction.
- **Robotics + aerospace** (MuJoCo/Drake/Chrono/Isaac-class): kinematic
  trees with O(n) Featherstone solvers, force/torque sensors at joint
  frames, contact-rich manipulation, deterministic verification (FAA /
  ESA / NASA scientific code review).
- **Cinematic + animation** (Houdini/Maya/Chaos-class): mass-scale
  destruction, animator-driven keyframed bodies with implicit velocity,
  cloth/grain/soft coupled to rigids, sub-frame motion-blur correctness.
- **Industrial / heavy machinery** (AGX/Vortex Studio-class): granular
  dynamics, heavy contact stacking, operator-training fidelity.
- **DAW / scientific visualisation**: arbitrary couplings — physics
  driving audio parameters, audio-modulated forces, MRI-derived
  magnetic fields.

If Cerid ships three independent decisions for these three faces, each
domain's needs will pull a face in a direction that breaks the others.
The dossier below answers the three questions together because the
right answers are constraints on each other.

What this dossier *will not* re-litigate:

- The five-collider catalogue (`Sphere`/`Box`/`Capsule`/`Convex`/`Plane`/
  `TriangleMesh`/`Heightfield`/`Sdf`) is locked by ADR-0062 §4.5.
- The deterministic FP contract (no FMA, deterministic libm, sort, hash)
  is locked by ADR-0063.
- The öbek prefab serialisation pattern, content-addressed ids, and
  reuse of the dynamic AABB tree are locked by ADR-0058 + ADR-0067.

What this dossier locks for ADR-0068:

- The body-type catalogue (`Static` / `Kinematic` / `Dynamic`, with
  `Kinematic` carrying implicit velocity inference from pose deltas to
  cover Maya's "Animated Rigid Body" and MuJoCo's mocap body).
- The sensor model (per-collider flag, not per-body — Jolt is the
  documented exception and we explain why we don't follow it).
- The five-tier collision-filtering surface (bit-mask layers + group
  index + explicit excluded pairs + ECS predicate hook +
  articulation-/joint-implicit auto-filter).
- The deferred ECS event-stream callback model with deterministic
  ordering by `(min(id_a,id_b), max(id_a,id_b))`.
- The optional synchronous mid-step `ContactModify` pure-function hook
  (v1g+, separate API).
- Specialised actor types as ECS *components* composed atop the three
  motion types — never as an enum sibling.

---

## 2. The body-type catalogue across domains

### 2.1 Games-style engines: the universal 3×3

PhysX 5, Bullet 3, Jolt, Box2D v3, Unity (classic + DOTS), Unreal Chaos,
and Godot 4 all ship the same fundamental three motion types, with
identical interaction rules. The Jolt
[`EMotionType`](https://jrouwe.github.io/JoltPhysics/) enum is the
canonical statement: `Static`, `Kinematic`, `Dynamic`. Box2D v3's
[`b2BodyType`](https://box2d.org/documentation/md_simulation.html) is
the same triple by name. PhysX uses `PxRigidStatic` and
`PxRigidDynamic` classes with the dynamic flag `eKINEMATIC` for the
middle case; the semantic split is identical. Unreal's
`Mobility::Static` / `Stationary` / `Movable` is the same triple under
a renderer-flavoured name.

The interaction matrix every one of those engines ships is:

| | Static | Kinematic | Dynamic |
|---|---|---|---|
| **Static**    | no contact     | one-way push (kin pushes static = ignored, static is immovable) | one-way push (dyn collides + bounces off static) |
| **Kinematic** | (same)         | **no contact** (both infinite-mass; no impulse possible) | one-way push (kinematic moves dynamic; dynamic exerts no force on kinematic) |
| **Dynamic**   | (same)         | (same)                  | full two-way contact + impulse exchange |

The diagonals + symmetry are universal. The single non-trivial cell is
"Kinematic-vs-Kinematic = no contact" — every shipped engine gives this
answer because there is no defined impulse for two infinite-mass bodies.
Jolt, PhysX, Bullet, Box2D, Unity, Unreal, Godot, ODE, Project Chrono
all agree. This is the deepest empirical regularity in the entire
physics-engine industry.

### 2.2 The "animated rigid body" question

Maya nDynamics ships an "Animated Rigid Body" — a body whose pose is
keyframed by the animator and whose velocity is *inferred* from the
delta between consecutive keys, so that other bodies receive the
correct collision impulse when the animated body strikes them
([Autodesk docs on nDynamics overview](https://knowledge.autodesk.com/support/maya/learn-explore/caas/CloudHelp/cloudhelp/2022/ENU/Maya-SimulationEffects/files/GUID-E1498F66-BD9D-4DB9-9BB7-EA123ABEB9E7-htm.html)).
The MuJoCo "mocap body" is the same idea, exposed slightly
differently: a body that is "treated as fixed from the viewpoint of
physics, yet the user is expected to move them programmatically at
each simulation step"
([MuJoCo modeling](https://mujoco.readthedocs.io/en/latest/modeling.html)).
The Unity / Unreal / Godot solution is identical to Maya's: when the
designer sets a Kinematic body's pose, the engine computes a one-frame
finite-difference velocity and feeds that into the contact solver so
that struck dynamic bodies see the correct impulse.

The clarifying question is therefore: **is "animated rigid body" a
fourth body type, or is it Kinematic with implicit velocity
inference?** Every shipped game engine says the latter. PhysX
[`setKinematicTarget`](https://documentation.help/NVIDIA-PhysX-SDK-Guide/Articulations.html)
does exactly this: you set a target pose, PhysX computes the implied
velocity from the delta, and the kinematic body striking a dynamic
body delivers the correct impulse. Maya makes it visible as a separate
"animated" type only because Maya's user is an animator who thinks in
keyframes, not in motion types — in the underlying Nucleus solver,
animated rigid bodies are kinematic bodies whose pose stream comes
from an animation curve.

This is the pivotal answer for Cerid's body-type catalogue: **3 types,
not 4. Animated rigid body is Kinematic-with-implicit-velocity, and
the engine infers velocity from the per-step pose delta.** The ADR
recommendation in §10 returns to this.

### 2.3 Robotics: bodies become links in kinematic trees

MuJoCo, Drake, Project Chrono, and Isaac Sim share a structurally
*different* abstraction. In all four, **every body is a link in a
kinematic tree**, and motion is parameterised by joint coordinates,
not by free-body position/velocity. MuJoCo's primer is explicit:
"the system state is represented in joint coordinates and the bodies
are explicitly organized into kinematic trees... bodies may have at
most one parent but any number of child bodies, and a body may only
define joints between itself and its parent body"
([MuJoCo overview](https://mujoco.readthedocs.io/)).

A "free body" in MuJoCo is a body attached to the world by a 6-DOF
free joint (`mjJNT_FREE`). A "static body" is a body without any
joints relative to the world. There is no separate dynamic-body
abstraction; dynamic-ness is a property of the joint topology.

Drake's [`MultibodyPlant`](https://drake.mit.edu/doxygen_cxx/classdrake_1_1multibody_1_1_multibody_plant.html)
is identical in spirit — bodies are nodes in a tree, joints are edges,
and the solver works in generalised coordinates. Project Chrono's
[`ChBody`](https://api.projectchrono.org/classchrono_1_1_ch_body.html)
exposes a free-body abstraction but couples it to the same tree-with-
joints model for articulations.

This sounds like a deal-breaker for Cerid's games-style 3-type model,
but it isn't. PhysX 5 and Jolt both support full robotics workloads
(IsaacSim's PhysX backend powers tens of thousands of robot policies
in parallel) by **layering articulations on top of the 3-type body
model**. Each articulation link is a regular dynamic body; the
articulation's joint topology is metadata that the solver consumes for
the Featherstone path. Adjacent links auto-filter their pairwise
contacts, which is how PhysX ships robotics ergonomics without giving
up the games-engine substrate
([PhysX articulations docs](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/docs/Articulations.html)).

For Cerid, this means: **ship the 3-type body model. Recover robotics
abstractions through the articulation substrate (v4 maximal-coords;
v6/v7 reduced-coords).** Joint topology atop a 3-type body model is
exactly what PhysX 5 and Jolt do.

### 2.4 Cinematic + animation: actor types as composition, not enum

Maya nDynamics' Nucleus solver
([Autodesk nDynamics overview](https://knowledge.autodesk.com/support/maya/learn-explore/caas/CloudHelp/cloudhelp/2022/ENU/Maya-SimulationEffects/files/GUID-E1498F66-BD9D-4DB9-9BB7-EA123ABEB9E7-htm.html))
ships a different vocabulary: **active vs passive objects.** nCloth,
nParticle, nHair, and (Maya-specific) nRigid are *active* — they're
simulated. *Passive* objects are static colliders (or animator-driven
keyframed colliders — Maya's animated-passive case maps to Cerid's
Kinematic). The interaction model is "by default all nParticle, nCloth
and passive collision objects that belong to the same Nucleus solver
collide with each other" — opt-out via the per-shape Collide attribute.

This collapses cleanly to Cerid's surface: **nCloth is a `SoftBodyComponent`
on a Kinematic-or-Dynamic body. nRigid is a Dynamic rigid. Passive is
Static or Kinematic. Constraints are joints.** Maya's vocabulary is a
function of its target user (animator, not engine programmer); the
underlying physics is the games-style model.

Houdini Vellum is structurally similar
([SideFX Vellum docs](https://www.sidefx.com/docs/houdini/nodes/dop/vellumconstraints.html)).
Vellum models cloth, grain, hair, soft-body, balloon, and rigid-shell
as **constraint sets on point clouds**. The "body type" question
collapses to "what constraint set is active" — Distance for cloth,
Stitch for connected pieces, Pin to attach to other geometry, Glue for
rigid-shell. This is closer to Cerid's compositional model than to
Maya's enum: a `VellumComponent` would carry a constraint-set descriptor,
not a body-type enum value.

Unreal Chaos Destruction extends the games-engine model with **geometry
collections** — fractured meshes that decompose into many child bodies
on impact
([UE5 Chaos Destruction docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-destruction-in-unreal-engine)).
A geometry collection is a tree of (originally welded) dynamic bodies
that activate as the parent is fractured. The body-type model is
unchanged; what's new is a *spawn pattern* that emits dynamic bodies on
break events. For Cerid this is a `GeometryCollectionComponent` (post-
v1) plus the existing dynamic body type — no new enum value needed.

### 2.5 Specialised actors across the survey

Every shipped engine adds specialised "actor" types beyond the three
motion types:

| Engine | Specialised actors |
|---|---|
| PhysX 5 | `PxRigidStatic`, `PxRigidDynamic`, `PxArticulationLink`, `PxArticulationReducedCoordinate`, `PxParticleSystem`, `PxSoftBody`, `PxFEMCloth`, character controller (`PxCapsuleController`) |
| Jolt | `Body` (with `EMotionType`), `CharacterVirtual` / `Character` (kinematic), `VehicleConstraint`, `SoftBodyCreationSettings` |
| Box2D v3 | `b2Body` (with `b2BodyType`); no specialised actors — character/vehicle are gameplay code on top |
| Unity DOTS | `PhysicsCollider`, `PhysicsVelocity`, `PhysicsMass`, `PhysicsDamping`, `Character` (Kinematic Character Controller package) |
| Unreal Chaos | `UPrimitiveComponent` family, `UCharacterMovementComponent`, `UWheeledVehicleMovementComponent`, `USkeletalMeshComponent` (ragdoll), `UGeometryCollectionComponent` |
| Godot 4 | `RigidBody3D`, `StaticBody3D`, `CharacterBody3D`, `AnimatableBody3D`, `Area3D`, `VehicleBody3D`, `SoftBody3D` |
| MuJoCo | links in kinematic trees + `mjJNT_FREE` for free bodies + mocap bodies (kinematic with implicit velocity) |
| Drake | bodies + joints + `ForceElement` + actuators + frames; no separate "character" type |
| Houdini Vellum | constraint sets (Cloth/Grain/Hair/Soft/Glue/Stitch/Pin/Attach) on point clouds |

Two patterns are visible:

1. **Most engines treat character / vehicle / softbody / particle as
   composition atop a base body, not as a sibling enum value.** Jolt's
   `Character` wraps a kinematic body; Unity DOTS adds a
   `KinematicCharacterController` component to a regular entity; Unreal's
   `UCharacterMovementComponent` is a component on a Pawn that itself
   owns a capsule collider. Godot is the only mainstream engine that
   gives each specialised actor a top-level type — and even there the
   types share `CollisionObject3D` machinery underneath
   ([Godot CharacterBody3D](https://docs.godotengine.org/en/stable/classes/class_characterbody3d.html)).
2. **Articulation links are dynamic bodies with extra metadata, not a
   separate enum value.** PhysX `PxArticulationLink` derives from
   `PxRigidBody` — same body type, extra constraint topology.

For Cerid the takeaway is unambiguous: **specialised actors are ECS
components composed atop one of the three motion types, never enum
siblings of the motion type.** This matches Cerid's ECS-native design
(ADR-0062 §6) and avoids the Godot trap of a body-type explosion when
someone wants a Vehicle that's also a CharacterController.

### 2.6 Industrial / heavy machinery

AGX Dynamics
([Algoryx AGX docs](https://www.algoryx.se/agx-dynamics/)) ships rigid
bodies plus a `GranularBodySystem` for particle-scale dynamics with
6-DOF spherical particles, used for mining/marine/heavy-machinery
training. The granular system is structurally a soft-body / particle
substrate, not a new rigid-body type — same pattern as PhysX
particles or NVIDIA FleX. Vortex Studio
([CM Labs Vortex docs](https://cm-labs.com/en/vortex-studio/))
similarly extends a base rigid-body model with cable / earthmoving
(soil) / vehicle modules — composition, not enum extension. Neither
engine breaks ranks on the Static/Kinematic/Dynamic three-tuple.

### 2.7 Aerospace + variable-mass bodies

Variable-mass bodies (rockets burning fuel, satellites venting
propellant) are the one workload where a games-style 3-type model
genuinely struggles. The standard answer across surveyed engines is
**no native support** — even MuJoCo and Drake assume mass is constant
over a sim step. Aerospace simulators (NASA's GMAT, ESA's Orekit) wrap
this manually: at each step boundary, the simulation updates the
body's `inv_mass` from a fuel-flow model. This is API-trivial in
Cerid (`set_body_state` accepts a fresh mass field) and does not
justify a fourth body type. The recommendation in §10 keeps it as a
runtime mass-mutation pattern, with a documented note that the solver
sees a stair-step mass curve between substeps.

CCD for hypersonic and multi-body coupling for staged rockets are
v6+/v4+ feature concerns, not body-type concerns. They land on top of
the three motion types unchanged.

---

## 3. Sensor / trigger handling — per-collider, with Jolt as the
documented exception

A *sensor* (trigger) is a body or shape that detects overlap with
other bodies but does not produce contact response — no impulse, no
solver participation. Used for damage zones, pickup volumes,
proximity detectors, robotics LIDAR cones, gameplay triggers.

The empirical question: **is sensor a property of the body or of the
collider?** The survey:

| Engine | Sensor at | API |
|---|---|---|
| PhysX 5 | per-shape | [`PxShapeFlag::eTRIGGER_SHAPE`](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/RigidBodyCollision.html) |
| Box2D v3 | per-shape | [`b2ShapeDef::isSensor`](https://box2d.org/documentation/md_simulation.html) |
| Unity (classic + DOTS) | per-collider | `Collider.isTrigger` |
| Godot 4 | per-`Area3D`-node (effectively per-collider; `Area3D` is sibling to `CollisionShape3D`) | [`Area3D`](https://docs.godotengine.org/en/stable/classes/class_area3d.html) |
| Unreal Chaos | per-collision-response (channel × `Overlap`) | [`ECollisionResponse::ECR_Overlap`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/ECollisionChannel) |
| Bullet | per-`btCollisionObject` (`CF_NO_CONTACT_RESPONSE` flag) | between body-level and per-shape — Bullet's collision-object is the unit of mounting shapes |
| ODE | per-geom (closer to per-collider) | category/collide bits with no-response flag |
| MuJoCo | per-geom (`contype=0 conaffinity=0` for visual-only; sensor is reported via the `<sensor>` element separately) | [MuJoCo XML reference](https://mujoco.readthedocs.io/en/stable/XMLreference.html) |
| **Jolt** | **per-body** | [`BodyCreationSettings::mIsSensor`](https://jrouwe.github.io/JoltPhysicsDocs/5.1.0/class_contact_listener.html) |

**Jolt is the exception, not the rule.** Eight of the nine surveyed
engines mount sensor at the shape/collider level. Jolt's per-body
choice is documented but unjustified in the public docs — best guess
from the codebase: Jolt was built for AAA games (Horizon Forbidden
West, Death Stranding 2) where multi-collider entities mixing solid
and trigger shapes are rare, and per-body fits Jolt's tight body-pool
layout better. In that workload it pays for itself.

For Cerid the workload is broader. Two scenarios immediately demand
per-collider:

1. **Character with proximity aura.** A character has a foot capsule
   (solid contact for ground), a chest capsule (solid contact for
   shoves), and an aura sphere around the chest (sensor for "enemy in
   melee range"). Per-body forces three separate bodies joined by
   joints — strictly worse ergonomics, worse cache footprint, worse
   determinism (more joints = more solver state).
2. **Robotics end-effector.** A gripper's finger has a solid contact
   pad (for grasp force) and a proximity sensor (for "object detected
   approaching grip frame"). Per-body splits the gripper across two
   entities with a joint; per-collider keeps them as one entity with
   two shapes.

The cinematic case (an animator-keyframed dragon with breath-zone
sensors as separate aura colliders) and the medical case (a surgical
tool with cutting blade as solid + ablation zone as sensor) both
demand per-collider for the same reason.

**Cerid lands on per-collider sensor.** The `Collider` struct gains a
single `is_sensor : 1` bit in its existing `flags` slot (no size growth
— `Collider` already has padding bytes). This matches PhysX, Box2D v3,
Unity, Godot, Unreal, Bullet, ODE, MuJoCo. Jolt's per-body model is
documented as the modern AAA-games exception.

---

## 4. The body-type interaction matrix — empirical universality
verified

Section 2.1 claimed the games-engine 3×3 matrix is universal. The
literature search confirms this for nine engines (PhysX, Bullet, Jolt,
Box2D, Unity classic + DOTS, Unreal Chaos, Godot, ODE) plus MuJoCo's
"static body = body without joints" reduces to Static, mocap = Kinematic,
free body = Dynamic semantically. Drake collapses similarly. Project
Chrono's `ChBody` carries a `SetBodyFixed(bool)` flag that selects
Static-vs-Dynamic; Kinematic is achieved by setting the body fixed and
prescribing motion via constraints.

The single corner where engines differ in *behaviour* (not in the
matrix shape) is **kinematic-vs-static overlap reporting**:

| Engine | Reports kinematic-static overlap event? |
|---|---|
| PhysX 5 | No (default); opt-in via `eNOTIFY_TOUCH_FOUND` on the pair via filter shader |
| Bullet | No (filtered out at broadphase by `StaticFilter` not in `KinematicFilter` mask by default) |
| Jolt | Yes if `ContactListener` registered and not filtered |
| Box2D v3 | Yes — kinematic-static overlap fires `b2ContactBeginTouchEvent` |
| Unity DOTS | Configurable via `CollisionResponsePolicy` |
| Godot 4 | Yes — `Area3D` reports overlap with both Static and Kinematic by default |
| Unreal Chaos | Configurable per channel × per body |

This divergence matters for Cerid's design: if the API never reports
kinematic-static overlap, gameplay code that needs "moving platform
arrived at end-of-track sensor" must poll. If the API does report it,
the broadphase pays a per-pair tracking cost for an interaction that
typically means nothing. Cerid's call: **make it configurable per
filter, default to NOT reporting**, matching the more performant PhysX
default. Designers who want kinematic-static overlap opt in via the
filter (§5).

The rare deviation that *does* break the 3×3 shape is **kinematic-vs-
kinematic contact response** for some animation workflows where two
animator-keyframed bodies need to push each other. No surveyed engine
ships this — Maya animators script around it via constraints, and
Houdini animators use a Vellum solver where everything is dynamic
under the hood. Cerid does NOT support kinematic-vs-kinematic contact
response. The matrix stays universal.

---

## 5. Collision filtering — the full survey

Collision filtering decides which pairs of geometry the broadphase
allows through to the narrow phase. The survey is wide because every
engine landed somewhere different on the expressiveness/performance/
authoring axes.

### 5.1 Bit-mask layers (the universal foundation)

Every engine ships some form of bit-mask layers. The common shape is:
each collider carries two bit fields, "what I am" (`category`) and
"what I want to collide with" (`mask`); two colliders interact iff
`(A.category & B.mask) != 0 && (B.category & A.mask) != 0` (the
*mutual consent* rule). This is the
[Box2D v3 collision filter formula](https://box2d.org/documentation/md_simulation.html)
in its purest form.

Variations:

- **Box2D v3** (`b2Filter`): `categoryBits` (16-bit) + `maskBits`
  (16-bit) + `groupIndex` (signed 16-bit). The mutual-consent rule
  applies; `groupIndex` overrides (§5.2).
- **PhysX 5** ([`PxFilterData`](https://physics-playground.github.io/PhysX5/physx/5.3.1/_api_build/struct_px_filter_data.html)):
  four 32-bit words of opaque user data (`word0`..`word3`). The user
  filter shader interprets these freely — PhysX itself imposes no
  category/mask convention. Most users build (category, mask) on top.
- **Bullet** (`btBroadphaseProxy`): 16-bit `m_collisionFilterGroup` +
  16-bit `m_collisionFilterMask`. Default groups: `DefaultFilter=1`,
  `StaticFilter=2`, `KinematicFilter=4`, `DebrisFilter=8`,
  `SensorTrigger=16`, `CharacterFilter=32`, `AllFilter=-1`
  ([forum](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=8752)).
- **Unity DOTS Physics**
  ([`CollisionFilter`](https://docs.unity3d.com/Packages/com.unity.physics@1.0/api/Unity.Physics.CollisionFilter.html)):
  `BelongsTo` (32-bit) + `CollidesWith` (32-bit) + `GroupIndex`
  (32-bit) — mutual-consent rule + group override.
- **Unity classic** ([Layer Matrix](https://docs.unity3d.com/Manual/LayerBasedCollision.html)):
  32 named layers, designer ticks pairs in a UI matrix. Compiles to
  the same bit-mask test internally.
- **Godot 4** (`collision_layer` + `collision_mask`): 32-bit each;
  identical model to Unity DOTS.
- **Rapier** (`InteractionGroups`): 16 groups, two 16-bit fields
  (memberships + filter); mutual-consent rule.
- **ODE** (`dGeomSetCategoryBits` / `dGeomSetCollideBits`): 32-bit
  category + 32-bit collide; mutual-consent rule.
- **AGX Dynamics**: named collision groups; pairwise enable/disable
  table.
- **Project Chrono**: family-bits with up to 15 families
  ([Chrono ChBody](https://api.projectchrono.org/classchrono_1_1_ch_body.html)).
- **Unreal Chaos** ([`ECollisionChannel`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/ECollisionChannel)):
  32 named channels × per-channel response (`Block` / `Overlap` /
  `Ignore`). This is *richer* than bit-mask layers — each pair has
  three possible responses, not two. Cell of the channel × channel
  matrix per body. Designed for "this projectile blocks Pawns,
  overlaps Triggers, ignores Decals" out of the box.

**Performance:** bit-mask filtering is two `AND`s and a comparison —
~3 cycles per pair tested at the broadphase. Production engines reject
80–95% of broadphase candidate pairs at this stage when layers are set
up properly
([PhysX RigidBodyCollision docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/RigidBodyCollision.html)).

**Failure modes:**

- **Layer rename pain.** Unity's "Layer 3" is a global namespace; rename
  it from "Enemies" to "EnemyAI" mid-project and the integer value
  stays but every authored asset that referenced "Enemies" now reads
  the wrong layer. The Unity community has a long history of this
  ([Unity Discussions](https://discussions.unity.com/t/layer-collision-matrix/892121)).
  Mitigation across engines: store layers by content-addressed hash,
  not by index. Box2D / Rapier / Godot avoid this by exposing layers
  as raw bits with no editor-side string mapping.
- **Channel exhaustion.** 16 bits = 16 layers; 32 bits = 32 layers.
  Larger projects exhaust this and start aliasing semantically distinct
  groups onto the same bit. PhysX's `PxFilterData` (128 bits) sidesteps
  this by giving the user space to encode arbitrary policy. Rapier's
  16-group cap is the most cramped; Unreal's 32 channels is the
  generous end of conventional design.

### 5.2 Group index (the cheap escape hatch)

Box2D, Unity DOTS, Rapier, and Bullet ship a *signed integer group
index* on top of bit-mask layers. The semantics
([Box2D collision filtering](https://www.iforce2d.net/b2dtut/collision-filtering)):

- `groupIndex == 0`: ignore — fall through to the bit-mask test.
- `groupIndex > 0`: if both colliders share a positive group index,
  always collide regardless of bit masks.
- `groupIndex < 0`: if both colliders share a negative group index,
  never collide regardless of bit masks.

This is a one-integer override that handles "ragdoll never collides
with itself" (negative group), "vehicle parts always collide with each
other" (positive group), and the rare case where a designer needs to
override the layer matrix without editing it.

Implementation cost is one extra integer comparison per filter test (~1
cycle). Authoring cost is one extra integer per body (8–32 bits). The
expressiveness gain is large enough that four major engines ship it;
the only ones that don't (PhysX, Unreal, Godot) push the same pattern
through their richer filter mechanisms (PhysX's filter shader; Unreal's
per-channel response matrix; Godot's per-`CollisionObject3D` masks).

### 5.3 PhysX-style filter shader (most powerful, most dangerous)

PhysX's `PxSimulationFilterShader` is a user-supplied C function that
runs per pair at broadphase output and returns a `PxFilterFlags`
([PxFilterFlag docs](https://nvidia-omniverse.github.io/PhysX/physx/5.2.1/_build/physx/latest/struct_px_filter_flag.html))
plus a `PxPairFlags`. The four `PxFilterFlag` values are:

- `eKILL` — ignore the pair forever; never re-test until filter data
  changes.
- `eSUPPRESS` — ignore as long as bounding volumes overlap.
- `eCALLBACK` — invoke `PxSimulationFilterCallback::pairFound()` for
  this pair (the C++ callback can read scene state the C function
  cannot).
- `eNOTIFY` — track the pair, fire `pairLost()` when the AABBs separate.

The filter shader is enormously powerful — it can implement any
filtering policy expressible as a pure function of two `PxFilterData`
values. It is also the most error-prone API in any major physics
engine, for three reasons:

1. **Non-determinism risk.** The shader runs per pair, on whatever
   thread the broadphase happens to produce the pair on. PhysX itself
   is documented as not cross-platform deterministic at all (O3DE
   explicitly disables PhysX determinism in its integration). If the
   shader reads any external state — gameplay flags, time, RNG — it
   compounds the problem.
2. **Reentrancy traps.** The shader runs while the broadphase is
   still iterating. Any side effect (logging, allocations, locking)
   can deadlock or corrupt.
3. **Misuse for non-filter logic.** Game programmers reach for the
   filter shader to "fix" gameplay bugs that should be solved
   elsewhere. Shipped game post-mortems include "we used the filter
   shader to disable damage between team-mates and broke replay because
   the team assignment was a runtime read."

For Cerid, this API shape is **rejected**. The expressiveness it buys
is recoverable from the ECS-predicate hook (§5.4) without breaking
determinism. The lesson stands: a filter mechanism that can read
external state at filter-evaluation time is incompatible with ADR-0063.

### 5.4 ECS-native predicate filtering

Bevy Rapier's `BevyPhysicsHooks` trait
([Rapier advanced collision detection docs](https://rapier.rs/docs/user_guides/bevy_plugin/advanced_collision_detection/))
is the modern ECS-native answer to the filter-shader problem. The user
implements a hook that's a `SystemParam` — it can `Query` ECS components
attached to entities — and returns whether the contact pair should be
processed. Example: `SameUserDataFilter` retrieves a `CustomFilterTag`
component from each body and rejects pairs whose tags don't match.

The expressive power matches PhysX's filter shader: any predicate over
component data is expressible. The determinism story is *cleaner*: the
hook is a pure function of ECS state at the start of the substep, so
the filter result is deterministic if the schedule is deterministic
(which Cerid's already is — ADR-0052). The hook does not race with the
solver because Rapier evaluates it at a fixed phase boundary.

Unity DOTS Physics uses a similar pattern via custom `IBodyPairsJob`
implementations that filter the candidate pair stream
([Unity Physics collision queries](https://docs.unity3d.com/Packages/com.unity.physics@1.0/manual/collision-queries.html)).

For Cerid the ECS predicate is the right answer for the
"PhysX filter shader" use case. Detail in §10.

### 5.5 Drake / Isaac-style explicit excluded pairs

Drake's `CollisionFilterDeclaration::ExcludeFromProximity()`
([Drake CollisionFilterDeclaration](https://drake.mit.edu/doxygen_cxx/classdrake_1_1geometry_1_1_collision_filter_declaration.html))
and Isaac Sim's "Filtered Pairs"
([Isaac Sim Tutorial 4](https://docs.isaacsim.omniverse.nvidia.com/6.0.0/openusd_tuning_tutorials/tutorial_04_collider_pairs.html))
ship a fundamentally different mechanism: an **explicit set of
excluded pairs**. The robotics workflow is "I imported a URDF; the
self-collision matrix from the URDF says links 3 and 7 must never
collide; let me declare that pair as excluded once at scene setup."

This is the canonical robotics requirement. Bit-mask layers can express
it by placing each link on a unique layer and setting up the matrix,
but for a 30-link humanoid that's 30 layers (Unity exhausts at 32) and
a 30×30 matrix with 435 distinct cells — an authoring disaster. The
explicit-pair set is `O(excluded pairs)` storage and `O(1)` test in
narrow-phase rejection, and it round-trips cleanly with URDF/SDF.

MuJoCo ships the same pattern via the `<exclude>` element in the XML
schema
([MuJoCo XML reference](https://mujoco.readthedocs.io/en/stable/XMLreference.html))
and via `contype` / `conaffinity` bit masks for the in-line case.
Project Chrono provides similar "collision family" exclusion. AGX
Dynamics manages it via group enable/disable tables.

For Cerid the lesson is: **bit-mask layers do not subsume explicit-pair
exclusion.** The URDF import path needs the explicit pair set as a
first-class API, separately from the layer mask. ADR recommendation
in §10.

### 5.6 Two-tier (Jolt's broadphase + object layer split)

Jolt is the only engine that ships *two layers of layer*. Each body
carries an `ObjectLayer` (16-bit; runtime mask) AND a `BroadPhaseLayer`
(8-bit; compile-time small set). The broadphase maintains a separate
BVH per `BroadPhaseLayer`; queries reject entire trees when the
broadphase-layer test fails before testing object layers
([Jolt Architecture docs](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)).

Rationale: each broadphase layer = one BVH. Coarse rejection at the
broadphase-layer stage is *much* faster than per-pair object-layer
testing because it skips the entire BVH descent. Cost: per-broadphase-
layer memory (one tree's nodes); careful design needed to balance "few
broadphase layers (small memory)" against "few cross-layer queries
(low pair-test count)." Jolt's documentation recommends starting with
2 broadphase layers — typically Static vs Moving.

This is a **performance optimisation, not a different filter model.**
The semantic answer is still bit-mask layers; broadphase-layer is just
"which BVH does this body live in." Cerid's recommendation in §10 is
to ship the simpler one-tier model first and revisit the broadphase-
layer split if profiling shows broadphase pair generation is hot. For
v1 workloads (~10k bodies), the simpler model wins on code clarity.

### 5.7 Articulation- and joint-induced auto-filtering

Two implicit filter rules ship in nearly every engine:

**Articulation self-collision.** Adjacent links in an articulation
auto-filter their pairwise contacts. PhysX explicitly documents this
([PhysX Articulations](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/docs/Articulations.html)):
"when articulation self collision is enabled, PhysX automatically
filters collisions between parent and child links." Non-adjacent links
in the same articulation may or may not collide depending on a
per-articulation flag (Isaac Sim's `enabledSelfCollisions`). The
robotics use case (humanoid arm should not be able to drive its own
hand into its own forearm) is the most tuning-intensive part of any
shipped robot: you almost always start with self-collision off, then
selectively enable it per-link-pair to handle "the fingers of the
hand should still collide with each other so we can grasp."

**Joint-induced auto-filtering.** Two bodies connected by a joint
typically default to non-colliding. PhysX:
[`PxConstraintFlag::eCOLLISION_ENABLED`](https://github.com/NVIDIAGameWorks/PhysX/issues/326)
is OFF by default — jointed pairs do not collide unless the joint
declares the flag explicitly. Box2D v3:
`b2RevoluteJointDef::collideConnected` defaults to false. Bullet:
`btTypedConstraint::m_disableCollisionsBetweenLinkedBodies` defaults
to true. Same pattern across the board: when the designer connects
two bodies with a joint, the engine assumes they're meant to be locked
into a relative pose and contact between them would just produce
solver fight.

These are *implementation details of the articulation / joint system*,
not designer-visible filter tiers. The lesson for Cerid: ship the
implicit auto-filter, expose a per-articulation `self_collision_enabled`
flag, and a per-joint `collide_connected` flag. No new top-level
filter machinery needed.

### 5.8 Robotics-specific filtering — sensors and end-effectors

Robotics workloads add specialised "filter-like" requirements that the
above tiers must compose with cleanly:

- **Proximity sensors** (LIDAR cones, ultrasonic probes) are sensor
  colliders attached to a robot link. They need to overlap-test against
  *every* dynamic object in their cone but must not impede motion. This
  is the per-collider sensor model (§3) plus a layer that covers
  "everything in the world."
- **End-effector grasp volumes.** A gripper's two fingers should
  collide with the payload but must not collide with each other.
  Standard pattern: each finger on its own layer + a per-pair exclusion
  for the finger-finger cell. Or an explicit excluded pair (§5.5).
- **Force/torque sensors at joint frames.** Not strictly a filter —
  this is a sensor *output*, not a filter input. Isaac Sim's
  [Effort Sensor](https://docs.isaacsim.omniverse.nvidia.com/5.1.0/sensors/isaacsim_sensors_physics_effort.html)
  reads the constraint impulse at a joint frame; Drake exposes the
  same via `MultibodyPlant::get_reaction_forces_output_port()`. For
  Cerid this lives in the joints substrate (v1f), not in the filter
  surface. Mentioned here because robotics users will ask.

---

## 6. Contact callback systems

Once the filter has decided pair (A, B) interacts, the engine generates
contact points and runs the solver. The callback system is how the
engine reports those interactions back to the application.

### 6.1 Synchronous mid-step vs deferred batched

This is the central design axis. The two extremes:

- **Synchronous mid-step** (Bullet `gContactProcessedCallback`): the
  callback fires while the narrow phase is still iterating. Pros:
  zero latency for "modify the contact before the solver runs"; lowest
  per-event cost. Cons: callback runs in whatever thread the narrow
  phase happens to be on, in arrival order; **non-deterministic by
  construction.** Bullet's own forum literature is clear: "callbacks
  shouldn't have any game logic and are meant for modifying contacts...
  it's considered better to look through all the contact manifolds
  after each simulation step"
  ([libGDX Bullet wrapper](https://github.com/libgdx/libgdx/wiki/Bullet-Wrapper---Contact-callbacks)).
- **Deferred batched** (PhysX `PxSimulationEventCallback`): all
  contact events are queued during `simulate()` and delivered in a
  single batched call after `fetchResults()`. Pros: deterministic
  ordering possible (PhysX sorts internally before delivery); single
  cache-friendly traversal of the event buffer; gameplay code runs in
  a known phase, not under solver locks. Cons: latency — the callback
  cannot influence this step's solve.

PhysX
([PhysX Simulation docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/Simulation.html)):
"For all events except `PxSimulationEventCallback::onAdvance()`, read
operations will return the state of the actors at the end of the
simulation step rather than the state the actors had when the event
was first detected." This commitment — that callbacks see end-of-step
state, batched — is what makes the deferred model usable in concurrent
gameplay code.

Jolt sits in the middle ([Jolt ContactListener](https://jrouwe.github.io/JoltPhysicsDocs/5.1.0/class_contact_listener.html)):
the `ContactListener` callbacks are called *during* the narrow phase
(synchronous mid-step in placement) but with **all bodies locked and
sorted such that body 1 ID < body 2 ID** — explicit determinism by
construction. The callback gets pre-sort guarantees the synchronous
model normally lacks.

Box2D v3 uses event streams: `b2World_GetContactEvents()` returns
arrays of begin/end-touch events drained between steps. Pure deferred,
deterministic with the rest of Box2D v3's cross-platform contract
([Box2D determinism](https://box2d.org/posts/2024/08/determinism/)).

Unity DOTS Physics uses the same pattern via DynamicBuffer event
streams the user iterates from a system that runs after physics.

For Cerid the choice is forced by ADR-0063: **deferred event streams
are the only callback model compatible with cross-platform deterministic
replay.** Synchronous mid-step is incompatible the moment the narrow
phase fans out to fibres. The Jolt sort-by-id pattern is a partial fix;
deferred is the cleaner answer because it composes with the existing
ECS schedule (events are written by `EylemPostPhysicsSystem`, drained
by user systems in the next phase).

### 6.2 ContactBegin / Stay / End — the lifecycle that doesn't agree

Engines differ on the contact lifecycle:

| Engine | Begin / Stay / End semantics |
|---|---|
| PhysX 5 | `eNOTIFY_TOUCH_FOUND` (begin), `eNOTIFY_TOUCH_PERSISTS` (stay — fires every step the contact is detected), `eNOTIFY_TOUCH_LOST` (end). Opt-in via filter shader per pair. |
| Jolt | `OnContactAdded` (begin), `OnContactPersisted` (stay — fires every time contact is *re-detected*, i.e., every narrow-phase pass), `OnContactRemoved` (end). |
| Box2D v3 | `b2ContactBeginTouchEvent`, `b2ContactEndTouchEvent`. **No stay event.** Gameplay derives stay state by tracking begins minus ends. |
| Bullet | `gContactStartedCallback` (begin), `gContactProcessedCallback` (per-contact-point, every frame), `gContactEndedCallback` (end). |
| Unity DOTS | `CollisionEvent` stream per step; user derives Begin/Stay/End by diffing pairs across steps. |

The "stay" event is the divergence: PhysX, Jolt, and Bullet ship it;
Box2D v3 and Unity DOTS make the user derive it. The pro-stay
argument: gameplay code that needs "this body is currently touching
that one" is common (apply continuous damage, play looping audio). The
anti-stay argument: stay events are by far the largest event volume in
any scene, and 90% of gameplay code that listens to them only needs
the begin/end transitions — emitting stay every frame is waste.

For Cerid: **ship Begin and End as first-class. Make Stay opt-in per
pair.** The default is Box2D's: derive stay from begin/end deltas in
gameplay code. Pair flags allow opting in to `Persist` events when the
gameplay genuinely needs every-frame contact updates (one-way damage
ticks, contact-rate audio modulation). This minimises the event-volume
cost while keeping the gameplay-friendly model available.

### 6.3 Trigger Enter / Stay / Exit — the simpler twin

Trigger lifecycle is the same shape as contact, with the same
divergence on Stay. Same recommendation: Begin (Enter) + End (Exit)
first-class; Stay opt-in per pair. Same dispatch model as contacts —
deferred event stream, sorted by `(min(id_a, id_b), max(id_a, id_b))`.

### 6.4 ContactModify — the synchronous mid-step pure-function hook

PhysX `PxContactModifyCallback`
([PhysX Simulation docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/Simulation.html))
and Bullet `gContactProcessedCallback` are both APIs that let user
code mutate contact points after the narrow phase generates them but
before the solver consumes them. Use cases — collected from PhysX docs,
Bullet forum threads, and shipped games:

- **One-way platforms** (jump-up-through-from-below): cancel the
  contact normal if the dynamic body's velocity is upward.
- **Pickup-through-walls**: cancel contacts between the player and
  decorative geometry while a "rush through wall" power is active.
- **Conveyor friction**: replace the contact's tangent velocity with
  the conveyor surface velocity, producing a "moves-you-along" effect
  without modifying the floor's body velocity.
- **Attenuated soft contact**: scale the impulse limit on contact
  normals for soft-material objects without authoring a separate
  material per shape pair.

PhysX documents that the callback "should be thread safe and reentrant,
and the SDK may call `onContactModify()` from any thread and it may be
called concurrently." This is exactly the determinism trap discussed
in §5.3. PhysX accepts the trap because contact modification by its
nature has to fire mid-step before the solver runs.

The clean solution is to constrain `ContactModify` to be a **pure
function of contact data** — the API gives the callback access to the
contact array and the two body ids, but does not give it access to a
World handle, RNG, time, or external state. With that constraint, the
callback is reproducible regardless of thread arrival order. Cerid's
recommendation in §10 ships this hook as a separate v1g+ API with the
purity constraint enforced at the API surface.

### 6.5 Contact persistence and warm-starting

The contact cache that the solver warm-starts from is keyed by
`(body_a_id, body_b_id, feature)` — the feature is which face/edge/
vertex of each shape is producing this contact point. Across all
surveyed engines the warm-start cache is content-addressed by stable
ids, never by pointer. ADR-0063 §1 already locks this for Cerid; no
new constraint here. The implication for the callback API: when the
warm-start cache hits, the engine fires `OnContactPersisted` (Jolt /
PhysX naming) with the cached contact data. This is what Jolt's
"called every time a contact is *detected*" wording means — the
detection includes the warm-start cache hit.

### 6.6 Performance — the callback storm problem

A destruction scene generates 10–100k contact pairs in a single step.
At 3 events per pair (Begin / Persist / End across substeps) that's
30k–300k user-code invocations per step. Surveyed mitigation patterns:

- **Filtered callback registration** (PhysX `eNOTIFY_TOUCH_FOUND` /
  `eNOTIFY_CONTACT_POINTS`): only pairs that opted-in via the filter
  shader fire callbacks. Default: no callbacks. Cost only on opted-in
  pairs.
- **Batched events** (Box2D v3, Unity DOTS): the callback is a single
  invocation that delivers an array of events. The user's per-event
  handling is a tight inner loop over the array, not an indirect call
  per event. Saves the function-call overhead.
- **Coarse-grained event** (Unreal Chaos `OnComponentHit` fires once
  per actor-actor pair per frame, not per contact point). Loses point-
  level detail; saves orders of magnitude in event count.
- **GPU contact event filtering** (Niagara GPU collision events): the
  GPU emits events to a buffer; the CPU reads only summary statistics.

For Cerid the right combination is: **batched events (Box2D pattern) +
opt-in registration (PhysX pattern) + ECS event-component drain
(Unity DOTS pattern).** The default is "no callback unless opted in";
opted-in pairs write to a per-frame ECS event buffer (`ContactEvent`
component on a transient event entity); user systems iterate the
buffer in a clean cache-friendly inner loop next phase. This composes
with the ECS schedule and avoids the callback-storm collapse.

PhysX's documented limit is informative for budget design: the SDK
caps interactions at 65,535 per actor; beyond that, an error is
emitted and extra interactions are silently ignored
([PhysX 3.4 release notes](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/release_notes.html)).
The constraint is a 16-bit counter overflow. Cerid's API takes the
opposite stance — `BodyId` is 32-bit and there's no per-actor cap —
but the budget message is the same: pre-allocate the event buffer
generously, document the cap, fail visibly when exceeded.

### 6.7 Determinism in callback ordering

Jolt's pattern (sort by `(min(id_a, id_b), max(id_a, id_b))` before
delivering) is the canonical deterministic-ordering answer. Box2D v3
ships the same: the contact event arrays are sorted by stable id before
the user drains them. PhysX is the documented bad case — `PxSimulationFilterShader`
runs in parallel and is not deterministic across cores by default
([Rapier determinism docs](https://rapier.rs/docs/user_guides/rust/determinism/)
contrast this with Rapier's `enhanced-determinism`, which pins exactly
this).

For Cerid: **all event buffers are sorted by `(min(id_a, id_b),
max(id_a, id_b))` before user delivery.** Hard requirement; no opt-out.
The cost is one sort over the event array at end-of-step, which is
~`O(n log n)` on event count and dominated by the actual event count in
practice.

---

## 7. Determinism (cross-cutting)

ADR-0063 commits to bit-exact world-snapshot hashes across MSVC /
clang / gcc × x64 / ARM × Windows / Linux. The collision-substrate
implications, by mechanism:

- **Filter evaluation order.** Bit-mask filters are commutative and
  associative — the result of the filter test does not depend on the
  pair-evaluation order. Group-index and explicit-pair-set tests are
  the same. ECS predicates are pure functions of ECS state, so the
  result is deterministic if the schedule is deterministic. None of
  the filtering tiers in §5 introduce non-determinism *if* they are
  pure functions; the PhysX filter-shader trap is precisely that the
  shader can read external state.
- **Multi-threaded narrow-phase merge.** Box2D v3 fans out the narrow
  phase to fibres; the merge is deterministic because each fibre
  writes to a pre-reserved range and the merge walks the ranges in
  stable id order
  ([Catto 2024 determinism](https://box2d.org/posts/2024/08/determinism/)).
  Cerid follows ADR-0063 §4: same pattern — fixed-position writes per
  fibre, no atomic counters for "next free index."
- **Callback dispatch ordering.** Per §6.7, hard sort by
  `(min(id_a, id_b), max(id_a, id_b))` before delivery.
- **Sensor enter/exit edge cases when bodies sleep.** This is the
  subtle one. If body A sleeps inside sensor S, the standard engine
  practice is to *not* fire OnExit (the body is still inside; it just
  stopped moving). When A wakes, no OnEnter fires (A is still inside).
  When S moves and A is no longer inside, OnExit fires. PhysX, Jolt,
  Box2D all agree on this convention. The determinism risk: if A's
  wake decision depends on some accumulated quantity (energy history),
  the wake step could shift across runs. ADR-0063 §1 locks sleep
  thresholds to absolute energy, not history-window averages — this
  rules out the wake-step-shift risk by construction.
- **Filter shader running in parallel.** Cerid does not ship a
  PhysX-style filter shader for exactly this reason (§5.3).
- **Contact warm-start cache.** Keyed by `(body_a_id, body_b_id,
  feature)` per ADR-0063 §1; no pointer-based keys. Standard.

The Box2D v3 cross-platform-determinism post is the single best
empirical reference: Catto enumerates exactly the failure modes Cerid
must avoid, and the v3 codebase is the proof that the bake-in approach
works at production scale. Rapier's `enhanced-determinism` flag is
the alternative reference — same outcome via slightly different
mechanism (Rapier disables SIMD / parallel features when the flag is
on; Cerid keeps SIMD via the deterministic SIMD substrate from
Phase 3.1 v0).

---

## 8. Performance characteristics

Hard published numbers for collision filtering and callback dispatch
are sparse — most engines benchmark at the level of "k bodies in N ms"
without isolating the filter / callback cost. What's documented or
inferable:

| Stage | Cost (typical) | Source |
|---|---|---|
| Bit-mask layer test | ~3 cycles per pair | ubiquitous; AND + AND + compare |
| Group-index test | ~1 cycle per pair | additional integer compare |
| Excluded-pair set lookup | ~10–20 cycles per pair (hash) | rare path; small set |
| ECS predicate evaluation | ~50–500 cycles per pair | depends on component-data fetch + predicate body |
| PhysX filter shader call | ~30–100 ns per pair | indirect call + user code |
| PhysX trigger interaction cap | 65,535 per actor before silent overflow | [PhysX 3.4 release notes](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/release_notes.html) |
| Contact event dispatch (deferred) | ~20–50 ns per event (single batched call) | inferred from Box2D v3 / Unity DOTS architectural notes |
| Contact event dispatch (sync per-callback) | ~50–500 ns per event (Bullet) | function pointer + user code cost |
| Broadphase rejection ratio (well-set-up layers) | 80–95% of pair candidates | PhysX RigidBodyCollision docs |
| Niagara GPU collision (5 systems × 4.5k particles) | 6.2 ms tick | [Epic Niagara perf tutorial](https://dev.epicgames.com/community/learning/tutorials/0qPO/unreal-engine-optimizing-niagara-measuring-performance) |
| Isaac Sim PhysX-on-GPU robotics throughput | tens of thousands of robot policies in parallel (no per-policy ms number published) | [IsaacLab paper](https://arxiv.org/html/2511.04831v1) |

The hierarchy is unambiguous: **reject as early as possible.** A pair
killed at the bit-mask stage is 30–100× cheaper than a pair killed at
the ECS-predicate stage, and 1000× cheaper than a pair that produces
contacts that are then filtered by `ContactModify`. Cerid's filtering
tiers in §10 are ordered to enforce this discipline.

Concrete Cerid budgets for v1 (10k bodies; values from
analogous engine measurements):

- Broadphase pair generation + bit-mask reject: ≤ 0.3 ms.
- ECS predicate evaluation (when active, typically << 1% of pairs): ≤ 0.1 ms.
- Contact event dispatch (1k events/step typical, 10k peak): ≤ 0.05 ms typical, ≤ 0.5 ms peak.
- ContactModify (when active, typically << 1% of pairs): ≤ 0.05 ms.

These become CI assertions in `tests/eylem-rigid3d/bench_filter.cpp`
shipped with v1d, mirroring the pattern from `bench_fields.cpp` in
v1f-fields-i.

---

## 9. Failure modes documented in production

Each item is a constraint on Cerid's design.

1. **Layer rename pain (Unity).** Renaming a layer changes the editor
   string but the integer bit value silently rebinds — every authored
   asset that referenced the old name now references the new layer.
   Mitigation: Cerid's layers are content-addressed by hash of name +
   öbek-source path, not by index. Renaming is a rebake operation
   that touches every consumer asset.
2. **Channel exhaustion (Rapier 16-bit, Unity 32-bit).** Larger
   projects exhaust the bit budget and start aliasing semantically
   distinct groups onto the same bit. Mitigation: Cerid's primary
   layer field is 64-bit (`u64 belongs_to`, `u64 collides_with`) — 64
   layers, with the extra capacity reserved for robotics / cinematic
   workloads that need more than 32 named channels.
3. **Callback storms in destruction (Chaos / Unity).** 100k contact
   pairs at 3 events each = 300k callbacks per frame. Mitigation:
   opt-in callback registration per pair; default no events; batched
   delivery via ECS event buffer.
4. **Sensor overlap missed when both bodies sleep (PhysX gotcha).**
   Two bodies asleep inside an overlap region — if the broadphase
   doesn't wake the pair on a filter-relevant change, the OnEnter
   never fires. Mitigation: Cerid's broadphase wake-on-filter-change
   policy is documented and tested explicitly; sleep state never
   suppresses the *first* enter event.
5. **Filter shader breaking deterministic replay (PhysX in production
   networked games).** A team-mate-damage filter shader read the
   "current team assignment" from external state; replay diverged when
   the team assignment was applied in a different order on the
   replaying machine. Mitigation: Cerid does not ship a PhysX-style
   filter shader. The ECS-predicate hook is constrained to read only
   ECS component state at the substep boundary, which is by definition
   reproducible.
6. **Joint-ignore relations missed for runtime-added joints (Bullet).**
   When a joint is added at runtime, the engine's existing pair set may
   already have an active contact between the now-jointed bodies; the
   joint flag is honoured for new pairs but existing pairs continue to
   solve until they separate. PhysX has the same gotcha
   ([PhysX issue #326](https://github.com/NVIDIAGameWorks/PhysX/issues/326)):
   "removing `PxConstraintFlag::eCOLLISION_ENABLED` does not stop
   collision." Mitigation: Cerid's joint-add path explicitly purges
   the contact cache for the affected pair on the same step, with a
   documented invariant.
7. **Articulation self-collision tuning rabbit hole.** Every shipped
   robotics arm does the same dance: start with self-collision off,
   selectively enable per-link-pair to handle "fingers should still
   collide for grasping," then iterate for weeks chasing edge cases.
   Mitigation: Cerid ships an explicit per-link-pair allow list at
   articulation construction, documented as the canonical robotics
   pattern.
8. **Robotics RL training non-determinism from callback ordering.**
   IsaacSim / IsaacLab training runs depend on contact-event order to
   reproduce a policy's trajectory. PhysX's parallel callback is a
   live problem here that the Isaac team works around with
   single-threaded contact processing. Mitigation: Cerid's
   sort-by-id-pair callback delivery is unconditional.
9. **Trigger interaction cap (PhysX 65,535 per actor).** Large field
   volumes with many bodies inside silently overflow. Mitigation:
   Cerid's event buffer is sized at scene-config time, with a runtime
   error on overflow rather than silent drop.

---

## 10. Recommended Cerid architecture (for ADR-0068)

Lock the choices below.

### 10.1 Body type catalogue: 3 motion types, sensor as per-collider flag

```cpp
enum class RigidBodyType : crd::u8
{
    Static    = 0, // never moves; infinite mass; collides with Kinematic + Dynamic
    Kinematic = 1, // user-driven; engine infers velocity from per-step pose delta
                   // (covers Maya "Animated Rigid Body" + MuJoCo mocap body)
    Dynamic   = 2, // fully simulated; participates in solver, gravity, sleep
};
```

This is the games-engine-universal triple, confirmed across 9+ surveyed
engines. Robotics engines (MuJoCo / Drake / Chrono) collapse into this
shape via the articulation substrate (joints atop bodies, not joints
*as* bodies). Maya's "Animated Rigid Body" and MuJoCo's mocap body
collapse into Kinematic with implicit velocity inference. **No fourth
type.** This locks the API surface against the body-type-explosion
trap (Godot's lesson).

The `Kinematic` velocity-inference contract: when `set_body_state` is
called on a Kinematic body, the eylem implementation computes
`(new_pose − old_pose) / dt` and writes it into the body's
`linear_velocity` / `angular_velocity` fields. Dynamic bodies struck
by the kinematic body in the next substep see the correct impulse.
This is the PhysX `setKinematicTarget` contract.

**Sensor is a per-collider flag**, not a body type. Add to `Collider`:

```cpp
struct ColliderFlags
{
    crd::u8 is_sensor : 1; // overlap-only; no contact response
    crd::u8 _reserved : 7;
};
```

This matches PhysX, Box2D v3, Unity, Godot, Unreal, Bullet, ODE, MuJoCo.
Jolt's per-body sensor model is documented as the modern-AAA-games
exception. The decisive case for per-collider: a single character
entity carrying both a solid foot capsule and a proximity aura sphere
without splitting into two bodies.

### 10.2 Specialised actor types: ECS components atop the 3 motion types

```cpp
// All composed atop one of the 3 motion types via ECS:
struct CharacterControllerComponent { /* capsule, slope limit, step height */ };
struct VehicleBodyComponent          { /* axle config, tyre model, gearbox */ };
struct ArticulationLinkComponent     { /* parent link id, joint topology metadata */ };
struct SoftBodyComponent             { /* XPBD substrate, mesh ref */ };
struct GpuParticleComponent          { /* GPU particle pool ref */ };
struct GeometryCollectionComponent   { /* fractured mesh tree, post-v1 */ };
```

Each is an ECS component composed with a `RigidBodyComponent`. The
character is a Kinematic body + `CharacterControllerComponent`. The
vehicle is a Dynamic body + `VehicleBodyComponent`. An articulation
link is a Dynamic body + `ArticulationLinkComponent`. This matches
PhysX (`PxArticulationLink` derives from `PxRigidBody`) and the modern
ECS-physics lineage (Unity DOTS, Bevy Rapier).

No specialised body-type enum value for any of these. Composition is
the universal pattern, and ECS-native composition is what Cerid was
built to do.

### 10.3 Body-interaction matrix: the universal 3×3 with one
configurable corner

The matrix from §2.1 is locked. The single configurable corner is
**kinematic-vs-static overlap event delivery**, default OFF (PhysX
behaviour). Designers who want "moving platform arrived at end-of-track
sensor" opt in via the filter (§10.4 tier 1 layer setup). The matrix
itself does not branch — Kinematic-vs-Kinematic remains "no contact"
because no sane impulse is defined.

### 10.4 Filtering tiers — five tiers, zero filter shader

```cpp
// Tier 1 — bit-mask layers (universal foundation)
struct CollisionLayer
{
    crd::u64 belongs_to;     // "I am a..."
    crd::u64 collides_with;  // "I want to collide with..."
};
// Mutual-consent rule:
//   collide ⟺ (A.belongs_to & B.collides_with) != 0
//          && (B.belongs_to & A.collides_with) != 0

// Tier 2 — group index (Box2D-style override)
//   field: i16 group_index on each Collider
//   group_index > 0 + same value → always collide (ragdoll-with-itself opt-in)
//   group_index < 0 + same value → never collide (ragdoll-no-self-collide)
//   group_index == 0 → fall through to tier 1

// Tier 3 — explicit excluded pairs (Drake / IsaacSim / MuJoCo robotics path)
class IPhysicsScene
{
    virtual void exclude_pair(BodyId a, BodyId b) noexcept = 0;
    virtual void include_pair(BodyId a, BodyId b) noexcept = 0;
    [[nodiscard]] virtual bool is_pair_excluded(BodyId a, BodyId b) const noexcept = 0;
};
// Storage: hash set of (min_id, max_id) tuples. O(1) test per pair.
// Round-trips with URDF / SDF importers in v4 (articulation slice).

// Tier 4 — ECS-native predicate hook (Bevy Rapier pattern; PhysX filter
//   shader expressiveness without the determinism trap)
struct ICollisionPredicate
{
    // Pure function of ECS component state at substep boundary.
    // Reads forbidden: World handle, RNG, time, file system, network.
    // Cerid lints these out at API surface (predicate signature does not
    // expose them).
    virtual bool should_collide(
        const PredicateInput& a,
        const PredicateInput& b) const noexcept = 0;
};
// Registered per-scene; one predicate per scene; users compose if needed.

// Tier 5 — articulation- and joint-implicit auto-filter
//   ArticulationComponent carries: bool self_collision_enabled (default false)
//                                  + ExcludedPairs allowlist for the chain
//   JointComponent carries:        bool collide_connected (default false)
//   These are not designer-visible filter knobs in the per-pair sense; they
//   are properties of the articulation / joint structures.
```

Tier ordering for evaluation (cheapest first):

1. Bit-mask layers (~3 cycles).
2. Group index (~1 cycle).
3. Excluded pairs (~10–20 cycles).
4. Articulation / joint auto-filter (~5 cycles, reads adjacency bit).
5. ECS predicate (~50–500 cycles).

A pair survives only if every tier passes it. Ordering reflects cost;
expressing the same policy at a cheaper tier is always preferable.

**No PhysX-style filter shader.** The expressiveness it buys is
recovered by tier 4; the determinism cost it pays is unacceptable
under ADR-0063.

**No two-tier broadphase-layer split (Jolt model)** in v1. The model
is a perf optimisation, not a different filter; revisit only if v1
profiling shows broadphase pair generation is hot.

### 10.5 Callback dispatch model — deferred ECS event streams

```cpp
// Written by EylemPostPhysicsSystem; drained by user systems next phase.
struct ContactEvent
{
    enum class Kind : crd::u8 { Begin = 0, Persist = 1, End = 2 };
    Kind                       kind;
    BodyId                     body_a;
    BodyId                     body_b;
    ColliderId                 collider_a;
    ColliderId                 collider_b;
    crd::math::Vec3f           contact_point_world;
    crd::math::Vec3f           normal_world;          // points from a to b
    crd::f32                   penetration_depth;
    crd::f32                   normal_impulse;        // valid only after solve
};

struct TriggerEvent
{
    enum class Kind : crd::u8 { Enter = 0, Stay = 1, Exit = 2 };
    Kind                       kind;
    BodyId                     body_a;
    BodyId                     body_b;
    ColliderId                 collider_a;
    ColliderId                 collider_b;
};
```

Both are written into ECS event buffers per step; user systems iterate
in the next ECS phase. Sort key for deterministic delivery:
`(min(body_a, body_b), max(body_a, body_b), kind)`.

**Persist / Stay events are opt-in per pair flag**, default OFF. The
default lifecycle is Begin / End only — gameplay derives "currently
touching" by accumulating begins minus ends. This is the Box2D v3
default and minimises event volume in destruction scenes.

The dispatch model composes with ADR-0052's schedule (events written
in `PostPhysics` phase, drained in the user phase that follows). It
composes with ADR-0063's determinism contract (sorted by stable id;
no thread-arrival-order dependency). It composes with ADR-0067's
field substrate (field events route through the same buffer for
`OnEnter` / `OnExit` triggers).

### 10.6 ContactModify hook — synchronous mid-step pure function, v1g+

Ship as a separate API in v1g (after the basic contact dispatch is
stable in v1d):

```cpp
struct IContactModifyCallback
{
    // Pure function of contact data + body ids. NO World handle, NO RNG,
    // NO time, NO external state. The argument types do not expose them.
    // Called from the narrow-phase fibre that produced the contact;
    // Cerid sorts the post-modify contact arrays by stable feature id
    // before the solver consumes them, recovering determinism even
    // though the callback fires in arrival order.
    virtual void modify_contacts(
        BodyId a, BodyId b,
        crd::containers::Span<ContactPoint> contacts) noexcept = 0;
};
```

Register per-scene; one callback per scene. The pure-function constraint
is enforced at the API surface — the callback signature does not
provide handles to mutable state.

Use cases (from §6.4): one-way platforms, pickup-through-walls,
conveyor friction, attenuated soft contact. All four expressible as
pure functions of contact data + the two body ids.

### 10.7 Robotics-specific extensions

- **Force/torque sensors at joint frames.** First-class component
  `JointForceSensorComponent` on the joint entity, populated each
  step from the constraint impulse (Isaac Sim / Drake pattern). Lives
  in the joints substrate (v1f), not the filter substrate; mentioned
  here for cross-reference.
- **Self-collision per-link-pair toggle.** `ArticulationComponent`
  carries an explicit allowlist of (link_a, link_b) pairs that *do*
  collide despite the articulation's default-off self-collision.
  Standard robotics pattern; URDF / SDF importers consume it directly.
- **Contact-area integrators.** Out of scope for v1; revisit when FEM
  / hydroelastic contact lands in v7 (the use case is medical
  simulation needing pressure-distributed contact, not point contact).

### 10.8 Cinematic-specific extensions

- **Geometry collections / mass-scale destruction.** Post-v1, as
  `GeometryCollectionComponent` (composition pattern) atop dynamic
  bodies. The substrate is unchanged; the new component is a spawn
  pattern that emits dynamic bodies on break events. Reuses the
  existing dispatch model (Begin event on parent, OnDestroy on
  parent + OnSpawn on children).
- **Sub-frame contact for motion-blur correctness.** Out of scope for
  v1. The use case is film cinematic motion blur where physics steps
  at 60 Hz but the camera samples at 120 Hz with shutter open across
  half the frame. The clean solution interpolates contact points
  between substeps (the variable-rate presentation step in ADR-0063
  §1 — physics fixed-step + presentation interpolated). Revisit if
  cinematic studios using Cerid surface a concrete need.

### 10.9 ECS-native filtering surface — concrete recommendation

The tier-4 ECS predicate hook reads ECS component state at the substep
boundary. The signature surface:

```cpp
struct PredicateInput
{
    BodyId      body;
    ColliderId  collider;
    // Component handle slot — populated by the eylem system from the
    // owning entity's components, reading whatever the predicate
    // declares as needed. The set is fixed at registration time; the
    // determinism contract demands the read set is closed.
    crd::scene::EntityComponentRefs components;
};
```

The predicate registers a *closed list of component types it reads*
at registration time. The eylem system enforces that the predicate
does not read any other component (by API construction — the
`EntityComponentRefs` view exposes only declared types). Closed read
set + pure function = reproducible across hosts.

This is the discipline Bevy Rapier's `BevyPhysicsHooks` formalises
via `SystemParam`. Cerid's version is one step stricter: the read set
is *declared* at registration, not inferred from the closure; the API
surface forbids reads outside the declaration. This bakes ADR-0063
compliance into the API rather than leaving it as a documentation
constraint.

### 10.10 Determinism constraints baked in from day one

- All event buffers sorted by `(min(body_a, body_b), max(body_a, body_b),
  kind)` before user delivery. Hard requirement; no opt-out.
- All filter tiers are pure functions of substep-start state. No
  filter tier reads runtime-mutable external state.
- Multi-threaded narrow-phase merge follows ADR-0063 §4: pre-reserved
  per-fibre slots, merge in stable id order, no atomic counters.
- Contact warm-start cache keyed by `(body_a_id, body_b_id, feature)` —
  never by pointer.
- Sleep thresholds use absolute energy, not history-window averages
  (ADR-0063 §1).
- ContactModify callback is a pure function of contact data + body
  ids; the API surface does not expose external state.

These are CI-asserted via the v1j replay-hash test (10 seconds of
contact-rich scene → snapshot hash matches across MSVC / clang / gcc ×
x64 / ARM × Windows / Linux).

### 10.11 Performance budgets

CI assertions in `tests/eylem-rigid3d/bench_filter.cpp` shipped with
v1d, mirroring `bench_fields.cpp` from v1f-fields-i:

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

### 10.12 Suggested slice integration

Slot into Phase 3.1 v1 between **v1c (ECS components — adds the sensor
flag)** and **v1d (filter system + contact dispatch)**, with the
ContactModify hook deferred to **v1g (advanced contact features)**:

| Sub-slice | Scope | LOC | Tests |
|---|---|---|---|
| **v1c-sensor** | Add `ColliderFlags::is_sensor` to the existing `Collider` struct + Cerid-scene component round-trip. | ~50 | ~3 |
| **v1d-filter-a** | Tier 1 (bit-mask layers) + Tier 2 (group index) + filter eval pipeline. Bench: layer reject rate. | ~250 | ~6 |
| **v1d-filter-b** | Tier 3 (explicit excluded pairs) + tier-3 storage (hash set). Bench: 30-link humanoid self-collision. | ~150 | ~3 |
| **v1d-filter-c** | Tier 4 (ECS predicate hook) + closed-read-set enforcement at API surface. Bench: 1% pair predicate cost. | ~200 | ~4 |
| **v1d-callback-a** | Deferred event streams: `ContactEvent` + `TriggerEvent` ECS buffers; `EylemPostPhysicsSystem` writes; sort by id-pair. | ~300 | ~6 |
| **v1d-callback-b** | Per-pair flags: opt-in for `Persist` / `Stay`; opt-in for contact-point detail; default Begin / End only. | ~100 | ~3 |
| **v1d-callback-c** | Bench suite: callback storm (10k events / step destruction) + budget assertions. | ~50 | bench |
| **v1f-articulation-filter** | Tier 5 articulation auto-filter (lands with the joint slice that introduces articulations). Per-link-pair allowlist. | ~150 | ~3 |
| **v1g-contactmodify** | `IContactModifyCallback` API + pure-function constraint + post-modify sort to recover determinism. Use cases: one-way platform sandbox demo. | ~250 | ~5 |

**Total ~1500 LOC + ~33 tests + 2 benches** for the body-types +
filtering + callback substrate. Proportional to the value (a substrate
every Cerid solver and every gameplay system consumes).

The blocking dependencies:

- v1d-filter-a, v1d-filter-b, v1d-callback-a, v1d-callback-b,
  v1d-callback-c are all unblocked at v1d entry.
- v1d-filter-c depends on the ECS query system (already shipped, ADR-0052).
- v1f-articulation-filter ships with v1f (joints), naturally.
- v1g-contactmodify ships once v1d is stable; not a v1 close
  requirement.

API surface freezes at **v1l** (v1 close). The v1g `ContactModify`
hook is a separate API surface — adding it post-freeze is a *new*
surface, not a modification of frozen surface. Same discipline as
ADR-0067's blocked sub-slices.

---

## 11. References

**Companion ADRs / dossiers:**
- [ADR-0062](../decisions/0062-eylem-physics-architecture.md) — eylem
  architecture (defines body / collider / scene contract).
- [ADR-0063](../decisions/0063-eylem-determinism-contract.md) —
  determinism contract that this dossier's tier 4 + callback model
  must comply with.
- [ADR-0067](../decisions/0067-eylem-force-field-architecture.md) —
  force-field substrate (parent of the closed-enum + ECS-native
  + content-addressed-id pattern this dossier follows).
- [`cerid-eylem.md`](cerid-eylem.md) — primary research backing
  ADR-0062 / ADR-0063.
- [`cerid-eylem-fields.md`](cerid-eylem-fields.md) — pattern reference
  for tone, structure, citation style.

**Primary engine documentation cited:**
- PhysX 5: [Rigid Body Collision](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/RigidBodyCollision.html),
  [Simulation](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/Simulation.html),
  [PxFilterData](https://physics-playground.github.io/PhysX5/physx/5.3.1/_api_build/struct_px_filter_data.html),
  [PxFilterFlag](https://nvidia-omniverse.github.io/PhysX/physx/5.2.1/_build/physx/latest/struct_px_filter_flag.html),
  [Articulations](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/docs/Articulations.html),
  [Character Controllers](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/CharacterControllers.html),
  [Joints (issue #326)](https://github.com/NVIDIAGameWorks/PhysX/issues/326),
  [3.4 release notes](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/release_notes.html).
- Jolt: [Jolt Physics docs root](https://jrouwe.github.io/JoltPhysics/),
  [ContactListener](https://jrouwe.github.io/JoltPhysicsDocs/5.1.0/class_contact_listener.html),
  [Architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md).
- Box2D v3: [Simulation](https://box2d.org/documentation/md_simulation.html),
  [Determinism (Catto 2024)](https://box2d.org/posts/2024/08/determinism/),
  [iforce2d Collision filtering](https://www.iforce2d.net/b2dtut/collision-filtering).
- Bullet: [libGDX wrapper contact callbacks](https://github.com/libgdx/libgdx/wiki/Bullet-Wrapper---Contact-callbacks),
  [Filter group/mask forum](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=8752).
- Unity DOTS Physics:
  [CollisionFilter](https://docs.unity3d.com/Packages/com.unity.physics@1.0/api/Unity.Physics.CollisionFilter.html),
  [Layer-based collision](https://docs.unity3d.com/Manual/LayerBasedCollision.html),
  [Layer matrix discussions](https://discussions.unity.com/t/layer-collision-matrix/892121).
- Unreal Chaos: [ECollisionChannel](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/ECollisionChannel),
  [Chaos Destruction](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-destruction-in-unreal-engine),
  [Chaos Destruction Optimization](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-destruction-optimization).
- Godot 4: [Area3D](https://docs.godotengine.org/en/stable/classes/class_area3d.html),
  [CharacterBody3D](https://docs.godotengine.org/en/stable/classes/class_characterbody3d.html).
- Rapier: [Determinism docs](https://rapier.rs/docs/user_guides/rust/determinism/),
  [Bevy advanced collision detection](https://rapier.rs/docs/user_guides/bevy_plugin/advanced_collision_detection/).
- MuJoCo: [Overview](https://mujoco.readthedocs.io/),
  [Modeling](https://mujoco.readthedocs.io/en/latest/modeling.html),
  [XML reference](https://mujoco.readthedocs.io/en/stable/XMLreference.html),
  [Computation](https://mujoco.readthedocs.io/en/stable/computation/index.html).
- Drake: [MultibodyPlant](https://drake.mit.edu/doxygen_cxx/classdrake_1_1multibody_1_1_multibody_plant.html),
  [CollisionFilterDeclaration](https://drake.mit.edu/doxygen_cxx/classdrake_1_1geometry_1_1_collision_filter_declaration.html).
- NVIDIA Isaac Sim / Lab: [Tutorial 4: Collider Pairs](https://docs.isaacsim.omniverse.nvidia.com/6.0.0/openusd_tuning_tutorials/tutorial_04_collider_pairs.html),
  [Effort Sensor](https://docs.isaacsim.omniverse.nvidia.com/5.1.0/sensors/isaacsim_sensors_physics_effort.html),
  [Articulation Joint Sensors](https://docs.isaacsim.omniverse.nvidia.com/5.1.0/sensors/isaacsim_sensors_physics_articulation_force.html),
  [IsaacLab paper (arXiv 2511.04831)](https://arxiv.org/html/2511.04831v1).
- Project Chrono: [ChBody](https://api.projectchrono.org/classchrono_1_1_ch_body.html),
  [Rigid Bodies](https://api.projectchrono.org/rigid_bodies.html),
  [Collision shapes](https://api.projectchrono.org/8.0.0/collision_shapes.html).
- AGX Dynamics / Algoryx: [AGX Dynamics product page](https://www.algoryx.se/agx-dynamics/),
  [AGX Granular](https://www.algoryx.se/documentation/complete/agx/tags/latest/doc/UserManual/source/granular_body_system.html).
- CM Labs Vortex Studio: [Vortex Studio product page](https://cm-labs.com/en/vortex-studio/).
- Maya nDynamics: [nDynamics Overview](https://knowledge.autodesk.com/support/maya/learn-explore/caas/CloudHelp/cloudhelp/2022/ENU/Maya-SimulationEffects/files/GUID-E1498F66-BD9D-4DB9-9BB7-EA123ABEB9E7-htm.html),
  [nDynamic Collisions](https://download.autodesk.com/global/docs/maya2012/en_us/files/guid-babbd1ce-c82c-4edb-bbf6-4c0ebc854da-3545.htm).
- Houdini Vellum: [Vellum Constraints](https://www.sidefx.com/docs/houdini/nodes/dop/vellumconstraints.html),
  [HoudiniVellum cgwiki](https://www.tokeru.com/cgwiki/HoudiniVellum.html).
- Niagara: [GPU Raytracing Collisions](https://dev.epicgames.com/documentation/en-us/unreal-engine/gpu-raytracing-collisions-in-niagara-for-unreal-engine),
  [Niagara perf tutorial](https://dev.epicgames.com/community/learning/tutorials/0qPO/unreal-engine-optimizing-niagara-measuring-performance).
- ODE: [ODE Manual](https://ode.org/wiki/index.php/Manual),
  [User Guide PDF](https://ode.org/ode-latest-userguide.pdf).
