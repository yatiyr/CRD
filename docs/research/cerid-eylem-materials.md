# Cerid Eylem — Materials Substrate Research

> **Companion to** [`cerid-eylem.md`](cerid-eylem.md),
> [`cerid-eylem-fields.md`](cerid-eylem-fields.md), and
> [`cerid-eylem-collision-filtering.md`](cerid-eylem-collision-filtering.md).
> Those backed ADR-0062 (architecture) + ADR-0063 (determinism contract)
> + ADR-0067 (fields) + ADR-0068 (body types / filtering / callbacks).
> This file backs **ADR-0069 (eylem materials substrate)** and pins the
> friction / restitution / surface-velocity / density catalogue, the
> combine-mode contract, the per-collider material-override storage
> shape, and the LuGre per-contact bristle-state allocation strategy.
>
> **Date:** 2026-05-11. **Audience:** anyone working on `crd-eylem`'s
> material substrate who needs the *why* behind an architecture choice.
> The *what* is in ADR-0069 and the v1a-material slice plan.
>
> **Scope:** the per-shape material contract that every contact, every
> solver, and every integrator reads — friction, restitution,
> density-vs-mass authoring, surface velocity, combine modes, per-pair
> override semantics, damage/fracture parameter reservation. NOT the
> contact-point dispatch model (ADR-0068 §6), NOT the constraint solver
> itself (v1e), NOT the joint/articulation algebra. Materials are the
> *parameter* substrate the solver consumes — not the solver.

---

## 1. Why materials are a substrate, not a per-collider hash

Every contact in every eylem solver — rigid SI v1e, XPBD soft v3,
articulation maximal-coords v4, vehicles v5, FEM v7 — reads the same
five questions about the surfaces that just touched: *how slippery is
this pair? how bouncy? does the surface itself drag along velocity
(conveyor / tire)? what's its density (so the integrator knows the
body's mass)? when this material's properties differ from the other
collider's, how do they combine?* The answers must come from one
place. Otherwise:

- **PhysX 5** ships `PxMaterial` for rigid-body contact, a separate
  `PxVehicleTireData` for the vehicle substrate, a separate
  `PxParticleMaterial` for particle simulation, a separate
  `PxFEMSoftBodyMaterial` for FEM ([PhysX 5.1
  `PxMaterial`](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/_build/physx/latest/class_px_material.html);
  PhysX 5.4 deprecated `eCOMPLIANT_CONTACT` and folded compliant
  contact into the same struct via signed restitution). The four
  schemas drift; tire friction does not compose with rigid-body
  friction; a designer's "wood" material is wood for boxes but not for
  the vehicle wheels rolling over the box. The split is the failure
  mode.
- **Bullet 3** stores friction and restitution per
  `btCollisionObject` (`m_friction`, `m_restitution`) and
  *anisotropic* friction via a separate
  `setAnisotropicFriction(btVector3, mode)` API on the same object
  ([Bullet
  source](https://github.com/bulletphysics/bullet3/blob/master/src/BulletCollision/CollisionDispatch/btCollisionObject.h)).
  Vehicle tires and constraint joints have their own friction
  constants in third places. The combine rule is hard-coded to
  multiplication and not configurable per-pair.
- **Unity classic** mounted material on the Collider component
  (`Collider.material` → `PhysicMaterial` asset) but the DOTS Physics
  rewrite moved it to the
  [`Material`](https://docs.unity3d.com/Packages/com.unity.physics@1.2/api/Unity.Physics.Material.html)
  struct on the collider's `MaterialBlob`. Designers using both
  systems in the same project — common during the long Unity ECS
  migration — get two unrelated material concepts.

Cerid's mandate is broader than any of those engines: rigid 3D + rigid
2D + soft + cloth + rope + articulations + vehicles + FEM + GPU
particles + acoustic occlusion, sharing one ECS world (ADR-0062). Five
parallel material schemas would dominate the bug budget. **Eylem ships
ONE `Material` struct that every solver reads identically.** The
public shape locks at v1a interface freeze; vehicles (v5) cannot grow
its own; FEM (v7) cannot grow its own; cloth (v3) cannot grow its
own. If a future solver needs a parameter that doesn't exist (e.g.,
hyperelastic Mooney-Rivlin coefficients for FEM), the path is *grow
this struct in a major-version bump*, not *fork a peer struct*.

The substrate also gates the v1a public API freeze. Material struct
shape determines:

- The cooker handler (`.physics-material.toml` → CRDR `'EMAT'`,
  v1k).
- The öbek serialisation trait (Material is a component-trait
  attached to colliders).
- The replay-snapshot layout (Material parameters round-trip through
  the `'EYLM'` snapshot artifact per ADR-0063 §7).
- The downstream solver code that reads it. Every solver hot path
  threads a `const Material*` through narrow phase + integrator;
  changing the struct shape post-freeze cascades to every consumer.

This dossier locks the shape so v1a closes cleanly.

---

## 2. Industry survey

Sixteen engines and tools, ranked roughly by influence on Cerid's
substrate. Each entry pins: where Material lives (per-body /
per-shape / per-pair), what fields, what combine rules, what defaults,
what's unique.

### 2.1 PhysX 5 — `PxMaterial`, with three siblings

Per-shape, attached via `PxShape::setMaterials(material**, count)`.
[Documented
fields](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/_build/physx/latest/class_px_material.html)
on `PxMaterial`:

- `setStaticFriction(PxReal)` — Coulomb static μ.
- `setDynamicFriction(PxReal)` — Coulomb dynamic μ.
- `setRestitution(PxReal)` — coefficient of restitution. **Negative
  values activate compliant contact** (PhysX 5.4 deprecated
  `PxMaterialFlag::eCOMPLIANT_CONTACT`; the negative-restitution
  encoding now triggers compliant mode).
- `setDamping(PxReal)` — companion to compliant restitution; spring
  damping of the contact.
- `setFrictionCombineMode(PxCombineMode)` /
  `setRestitutionCombineMode(PxCombineMode)` — `eAVERAGE`, `eMIN`,
  `eMULTIPLY`, `eMAX`. Average is default for both.
- `setFlags(PxMaterialFlags)` — `eDISABLE_FRICTION`,
  `eDISABLE_STRONG_FRICTION`, `eIMPROVED_PATCH_FRICTION`.

**What's NOT on `PxMaterial`:**

- **Surface velocity** is NOT a material field. PhysX exposes it via
  two paths: `PxRigidDynamic::setKinematicSurfaceVelocity()` (only on
  kinematic bodies — used for static conveyors and rotating
  surfaces) and `PxContactSet::setTargetVelocity()` inside a
  `PxContactModifyCallback` (per-contact override; the canonical
  path for animated surface velocity)
  ([discussion](https://github.com/NVIDIA-Omniverse/PhysX/discussions/280)).
  The ContactModify path is documented as CPU-only in Isaac Sim's
  GPU pipeline — surface velocity does not survive the GPU
  dispatch. **This is a documented limitation of the
  not-on-material design, and Cerid corrects it.**
- **Anisotropic friction** is per-shape, NOT per-material in PhysX 5
  — a `PxShape` carries `setFrictionAnisotropy()` distinct from its
  material. The shape vs material split makes "rubber tire on ice"
  harder than it should be: the anisotropy direction is on the
  shape, the magnitude on the material, the combine mode on the
  material — three places to author one effect.
- **Density** is not on `PxMaterial`. Mass is computed by
  `PxRigidBodyExt::updateMassAndInertia(actor, density)` taking a
  uniform density at body construction; per-shape densities are
  passed as a parallel array
  ([PhysX 5.1
  manual](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/docs/RigidBodyDynamics.html)).
  The `PxMassProperties` helper assumes density = 1 at construction
  and scales linearly.

The three PhysX siblings — `PxMaterial`, `PxVehicleTireData`,
`PxParticleMaterial`, `PxFEMSoftBodyMaterial` — duplicate the
friction/restitution surface across substrates. **The vehicle tire
material's μ and the rigid-body material's μ are not the same number
and do not compose; a tire on a wood box does not see the wood
material.** This is the principal lesson Cerid lifts from PhysX:
unify or fragment.

### 2.2 Bullet 3 — per-`btCollisionObject` scalars + special anisotropic call

Per-body (Bullet's `btCollisionObject` is the unit that mounts shapes;
shapes themselves are stateless geometry). [`btCollisionObject`
header](https://github.com/bulletphysics/bullet3/blob/master/src/BulletCollision/CollisionDispatch/btCollisionObject.h)
fields:

- `m_friction` (PxReal) — Coulomb friction.
- `m_rollingFriction` — separate from sliding friction; for
  curling/rolling spheres.
- `m_spinningFriction` — separate again; for billiard-cue spin.
- `m_restitution` — coefficient of restitution.
- `m_contactStiffness` / `m_contactDamping` — compliant contact
  parameters.
- `m_anisotropicFriction` (`btVector3`) + `m_collisionFlags &
  CF_ANISOTROPIC_FRICTION` (or `CF_ANISOTROPIC_ROLLING_FRICTION`).

`setAnisotropicFriction(btVector3(0, 1, 1),
CF_ANISOTROPIC_FRICTION)` zeroes friction along the body's local X
([forum discussion](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=12500)).
Direction is **the body's local frame** — there is no separate "friction
direction" vector. To rotate the friction axis without rotating the
body, you build a child rigid with the desired orientation and joint
it. This is the API shape Cerid wants to avoid: anisotropy direction
should be authored alongside the magnitude.

Combine modes are NOT configurable: the contact solver always uses
`mFriction = body0.friction * body1.friction` (multiply) and similar
for restitution. Authoring a "this should always feel slippery
regardless of the other surface" needs gameplay code to set both
sides.

Density lives on the `btRigidBody` constructor as part of
`btRigidBodyConstructionInfo` — density is a property of the body, not
the material; the `calculatePrincipalAxisTransform` helper computes
the inertia tensor from the shape and the constructor's mass.

### 2.3 Jolt — per-body friction/restitution + sub-shape `PhysicsMaterial`

[`Body`](https://jrouwe.github.io/JoltPhysics/class_body.html) carries
friction and restitution as scalars (`SetFriction(float)`,
`SetRestitution(float)`). For compound shapes (multiple sub-shapes per
body) Jolt provides per-sub-shape material via `PhysicsMaterial`
referenced in `ShapeSettings::mMaterial` or
`CompoundShapeSettings::SubShapeSettings::mMaterial`
([discussion](https://github.com/jrouwe/JoltPhysics/discussions/1311)).

The combine functions are user-supplied
(`ContactConstraintManager::CombineFunction` callbacks set on the
`PhysicsSystem`):

```cpp
inSystem.SetCombineFriction([](const Body &b1, const SubShapeID &s1,
                              const Body &b2, const SubShapeID &s2)
{ return std::sqrt(b1.GetFriction() * b2.GetFriction()); });
```

Defaults: `mFriction = 0.2f`, `mRestitution = 0.0f`,
default friction combine = geometric-mean, restitution combine =
max. **This is the clearest "users own combine" API in any major
engine** — the engine doesn't hard-code the formula but provides
sensible defaults. Cerid lifts the discipline: combine modes are
material fields, not engine constants, but defaults match Jolt's.

Surface velocity: not a `PhysicsMaterial` field. Jolt's documentation
example for conveyors implements it via a `ContactListener::OnContactAdded`
override that sets `IslandBuilder::mRelativeLinearSurfaceVelocity` —
a per-contact override, similar to PhysX's `ContactModify`.

Density: `ShapeSettings::SetDensity(float)` per shape; Jolt's
`Body::GetMassProperties()` aggregates over compound shapes weighted
by density. This is the right shape — density on the shape
description, mass computed from geometry × density, override on the
body if needed.

### 2.4 Box2D v3 — `b2ShapeDef` packs material into shape construction

[`b2ShapeDef`](https://box2d.org/documentation/group__shape.html):

- `friction` (default `0.6f` per the Box2D v3 source) — Coulomb μ.
- `restitution` (default `0.0f`) — coefficient of restitution.
- `density` (default `1.0f` kg/m²) — per-shape density; mass derived
  via `b2Body_ComputeMassFromShapes()`.
- `material` (`int32_t`) — opaque integer tag; user-defined
  semantics. Used by some custom mixing callbacks.
- `customColor` — visual debug only.

The combine rule, **per Erin Catto's stated implementation in Box2D
v3**, is geometric mean:

```cpp
float mixedFriction = sqrtf(b2Shape_GetFriction(a) * b2Shape_GetFriction(b));
float mixedRestitution = b2MaxFloat(b2Shape_GetRestitution(a),
                                    b2Shape_GetRestitution(b));
```

The user can override via custom `b2FrictionCallback` /
`b2RestitutionCallback`. **Geometric mean for friction is the
mathematically defensible choice** — it preserves the property that
`combine(x, x) == x` (idempotency) and `combine(0, x) == 0`
(zero-friction surface forces zero-friction contact regardless of the
other surface). Average has neither property cleanly. Box2D v3's
choice is the modernised one.

### 2.5 Unity classic + DOTS Physics

**Classic**: `PhysicMaterial` asset attached via `Collider.material`
or `Collider.sharedMaterial`. Fields: `dynamicFriction`,
`staticFriction`, `bounciness`, `frictionCombine`, `bounceCombine`
(`Average`, `Min`, `Max`, `Multiply`).

**DOTS Physics** — the modern path —
[`Material`](https://docs.unity3d.com/Packages/com.unity.physics@1.2/api/Unity.Physics.Material.html)
struct on the collider blob:

```csharp
public struct Material
{
    public CombinePolicy FrictionCombinePolicy;
    public CombinePolicy RestitutionCombinePolicy;
    public CollisionResponsePolicy CollisionResponse;
    public float Friction;
    public float Restitution;
    public byte CustomTags;
}
```

`CombinePolicy`: `GeometricMean`, `Maximum`, `Minimum`,
`ArithmeticMean`. Note that DOTS Physics ships `GeometricMean` as a
first-class enum value alongside `ArithmeticMean` — Cerid follows.
`CustomTags` is a designer-controllable byte attached to the
material, used for gameplay queries ("did the player land on a wood
material?") via `MaterialQuery`.

Density lives elsewhere — on `PhysicsMassProperties`, computed at
authoring time via `BoxColliderUtility.CalculateMass(volume, density)`.

### 2.6 Unreal Chaos — `UPhysicalMaterial` + `UPhysicalMaterialMask`

[`UPhysicalMaterial`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/PhysicsCore/UPhysicalMaterial)
fields:

- `Friction` (float, default 0.7) — combined static + dynamic μ; UE
  collapses the two.
- `Restitution` (float, default 0.3) — coefficient of restitution.
- `Density` (float, default 1.0 g/cm³) — *measured in g/cm³*, not SI
  kg/m³. Designer-friendly but wire-format-incompatible with every
  other engine.
- `RaiseMassToPower` (float, default 0.75) — for arcade vehicles;
  `effective_mass = pow(true_mass, RaiseMassToPower)`. Specific to
  Chaos vehicle handling; cinematic / robotics rejects it.
- `FrictionCombineMode` / `RestitutionCombineMode` enum: `Average`,
  `Min`, `Multiply`, `Max`. Default Average.
- `SurfaceType` — designer-named enum (32 values) used for gameplay
  ("hit a wood surface, play wood thud").
- `DestructibleDamageThresholdScale` — scales the destruction
  break-strain threshold for any geometry collection that uses this
  material. **First-class destruction parameter on the material.**

`UPhysicalMaterialMask` is a 2D-texture-driven per-pixel material
selector for static meshes — the same triangle mesh can read a
"grass / asphalt / mud" material per UV based on a paint-mask. Out
of scope for v1 eylem (Phase 7 editor concern) but the struct's
existence informs the design: the material schema must be
serializable independently of the colliders that reference it (so
many colliders can share one material asset), and a future per-pixel
override path is plausible without a struct change.

### 2.7 Godot 4 — `PhysicsMaterial` resource (minimal)

[`PhysicsMaterial`](https://docs.godotengine.org/en/stable/classes/class_physicsmaterial.html)
resource fields:

- `friction` (float, default 1.0) — combined μ.
- `bounce` (float, default 0.0) — restitution.
- `rough` (bool, default false) — combine policy: if EITHER side is
  rough, use the larger friction; if BOTH rough, use the largest;
  if NEITHER, use the smaller. (A boolean encoding 4 of the 9
  possible combine outcomes — this is the documented Godot
  shape.)
- `absorbent` (bool, default false) — combine policy for restitution:
  if true, *subtract* this body's bounce from the other's instead of
  adding/maxing.

**This is the most cramped material schema in any mainstream
engine**, and the Godot community has open proposals to expand it
([proposal
#11715](https://github.com/godotengine/godot-proposals/issues/11715)).
The lesson is the cost of underdesign — Godot 4 cannot express
anisotropic friction, surface velocity, or non-Coulomb friction at
all without engine modification. Cerid pays the cost upfront.

### 2.8 MuJoCo — friction triple + `solref`/`solimp` parameter vectors

MuJoCo doesn't ship a "Material" struct; instead, friction and
contact parameters live on each `<geom>` element of the MJCF model
([XML
reference](https://mujoco.readthedocs.io/en/stable/XMLreference.html)):

- `friction` — **a triple of (sliding, torsional, rolling) friction
  values**, default `(1, 0.005, 0.0001)`. Sliding is the standard
  Coulomb μ; torsional resists spin around the contact normal;
  rolling resists rolling on the contact patch. Three friction
  modes per material is a robotics-and-billiards refinement
  Cerid will need post-v1 (rolling friction lands when v1f vehicles
  ship, alongside Bullet-style `m_rollingFriction`).
- `solref` — 2-vector `(timeconst, dampratio)` defining the contact
  reference acceleration; together with `solimp` it fully
  parameterises MuJoCo's soft-contact spring-damper. The contact
  acts as a virtual spring with first-order time constant
  `timeconst` and damping ratio `dampratio`.
- `solimp` — 5-vector `(d0, d1, width, midpoint, power)` defining
  the impedance ramp from soft (`d0`) to stiff (`d1`) over a
  penetration depth of `width`. This is MuJoCo's Hunt-Crossley
  alternative — equivalent expressive power, different
  parameterisation.
- `priority` (int) — when two contacting geoms have different
  parameters, the higher-priority one wins (the lower priority's
  `solref`/`solimp`/`friction` are ignored). This is MuJoCo's
  combine rule: "max-priority-wins" rather than commutative
  formulas. Robotics-friendly because the manipulator's gripper
  always wins over the conveyor's belt regardless of authoring
  order.

MuJoCo's friction triple + `solref`/`solimp` is the most
domain-specific material schema in the survey. Eylem absorbs the
*friction triple* concept (sliding + torsional + rolling) for the v5
vehicle slice; the `solref`/`solimp` parameterisation maps onto Cerid's
Hunt-Crossley + signed-restitution-compliant-contact path with a
documented translation.

### 2.9 Drake — `PointContactParameters` + `HydroelasticContactParameters`

Drake's [compliant
contact](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)
splits into two material concepts:

**Point contact** parameters per-geometry:

- `("material", "point_contact_stiffness")` — spring stiffness
  `k` (N/m). Combined for a contact pair as
  `k_combined = (k1·k2) / (k1+k2)` (springs in series).
- `("material", "hunt_crossley_dissipation")` — dissipation
  parameter `d` (s/m). Combined as
  `d = (k2/(k1+k2))·d1 + (k1/(k1+k2))·d2`. Typical range
  `[0, 100]` s/m, default 20 s/m. **Bounce velocity after impact
  is bounded by `1/d`** — a quick physical intuition for
  authoring.
- Friction (Coulomb μ_static, μ_dynamic) per-geometry; combined as
  geometric mean.

**Hydroelastic contact** (Drake's compliant FEM-like contact for
robotics manipulation) adds:

- `hydroelastic_modulus` — bulk Young's modulus E (Pa).
- `resolution_hint` — mesh resolution for the pressure field
  computation (m).
- `slab_thickness` — for half-space hydroelastic.
- `compliance_type` — `kRigid`, `kCompliant`, `kHybrid`.

Drake's split between point and hydroelastic is **the cleanest "two
contact models share one material asset" architecture in the survey**.
Eylem reserves room in the Material struct for the hydroelastic
parameters (FEM lands in v7) without forcing v1 users to author them.

The Hunt-Crossley contact force law itself ([Hunt & Crossley
1975](https://www.sciencedirect.com/science/article/pii/S0094114X2500312X)):

```
F_normal = k·δ^n · (1 + (3/2)·d·δ̇)
```

where `δ` is the penetration depth, `δ̇` is the penetration rate, `n`
is the Hertz exponent (usually 1 for flat-flat or 1.5 for
sphere-sphere Hertzian), `k` is contact stiffness, `d` is the
dissipation parameter. The form ensures the contact force is zero at
zero penetration AND zero at zero penetration rate — the key
improvement over Kelvin-Voigt (which can be non-zero at separation
and produce sticky contacts). 50 years of citation
([compendium](https://www.sciencedirect.com/science/article/abs/pii/S0094114X21002573)).

### 2.10 NVIDIA IsaacSim + IsaacLab — PhysX wrap with robotics extensions

IsaacSim wraps PhysX 5's `PxMaterial` and adds the
[`RigidBodyMaterialCfg`](https://isaac-sim.github.io/IsaacLab/main/source/api/lab/isaaclab.sim.schemas.html)
API for per-actor / per-geometry assignment in IsaacLab. Important
robotics extension: `randomize_rigid_body_material` — randomizes
friction (static + dynamic) and restitution across a population of
actors for sim-to-real domain randomization. This is the API shape
robotics RL training expects: per-actor material override, batched
randomization, deterministic seeded.

PhysX has a documented hard limit of **64,000 unique physics
materials per scene**; exceeding crashes IsaacLab. Cerid's
`MaterialPool` is bounded by `crd::u32` index (~4 billion), but any
implementation should warn or error before reaching tens of
thousands of distinct materials — the cache footprint of a 100k
material pool exceeds modern L2.

### 2.11 Project Chrono — `ChMaterialSurface{NSC,SMC}` solver-paired

[`ChMaterialSurfaceNSC`](https://api.projectchrono.org/classchrono_1_1_ch_material_surface_n_s_c.html)
(non-smooth contact, complementarity-based) and
[`ChMaterialSurfaceSMC`](https://api.projectchrono.org/classchrono_1_1_ch_material_surface_s_m_c.html)
(smooth/penalty-based contact) are *solver variants of the material
itself*. The same physical wood with the same friction needs two
materials if the scene uses both solvers in the same simulation.

NSC fields: `static_friction`, `sliding_friction`, `restitution`,
`compliance` (m/N — tangential), `compliance_T`, `damping_F`,
`rolling_friction`, `spinning_friction`. SMC fields: `young_modulus`,
`poisson_ratio`, `static_friction`, `sliding_friction`, `restitution`,
`adhesion`, `kn` (normal stiffness), `kt` (tangential stiffness),
`gn` (normal damping), `gt` (tangential damping).

**This is the second principal failure mode Cerid avoids: do not pair
the material schema to the solver.** Cerid's Material is one struct;
the solver consumes it. Future solvers (TGS v2, XPBD v3, FEM v7) will
read the same Material — they may use parameters the SI v1e doesn't,
but the parameter space is shared.

### 2.12 AGX Dynamics — `agx::Material` + `agx::ContactMaterial` per-pair

The most expressive material substrate in the industrial-simulation
space. [`agx::Material`](https://www.algoryx.se/documentation/complete/agx/tags/latest/doc/html/classagx_1_1Material.html)
is per-shape and carries `density`, `viscosity` (used for restitution
combine), `youngs_modulus`, `friction_coefficient`, `roughness` (used
for friction combine), `surface_viscosity`, `adhesion_force`,
`adhesion_overlap`. **`agx::ContactMaterial`** is a *per-pair* material
authored explicitly between two `agx::Material` instances, overriding
any combine formula:

- Implicit (no `ContactMaterial` declared): friction =
  `sqrt(roughness1 · roughness2)`, restitution =
  `sqrt((1-viscosity1)·(1-viscosity2))` — geometric-mean for both,
  with restitution computed from the bounded-bounciness
  `(1 − viscosity)` factor.
- Explicit (`ContactMaterial(matA, matB)` in scene config): the
  authored values override; arbitrary `friction_coefficient`,
  `restitution`, `youngs_modulus`, `adhesion_force` etc. specific
  to the pair.

**Per-pair material is the principal AGX feature Cerid considered and
rejected.** §6 §12.5 explain. Storage cost is `O(N²)` in the number of
distinct materials when fully populated; even a sparse representation
(only authored pairs) is unbounded by O(1) memory budgets at v1
scale. A tractable v9+ revisit when the FEM / hydroelastic substrate
needs richer cross-material physics; not for v1.

### 2.13 Maya nDynamics — Nucleus solver attribute set

Maya's Nucleus solver (`nCloth`, `nParticle`, `nHair`) treats material
properties as *per-object* attribute sets attached to the Nucleus
node, not as a separate Material asset:

- Friction (sliding only).
- Stickiness — Maya's adhesion equivalent.
- Bounce — restitution.
- Mass — explicit; no density-derived path.
- For nCloth: stretchResistance, bendResistance, compressionResistance,
  damping — soft-body-specific.

Maya's animator-facing UI exposes everything as per-object attributes
because the solver is unified across cloth/particle/rigid; this is
the 1990s Nucleus discipline. Eylem absorbs the lesson: a single
Material struct must scale from "rigid body friction" to
"soft-body damping" without forcing two schemas.

### 2.14 Houdini Vellum — per-constraint, not per-material

[Vellum
constraints](https://www.sidefx.com/docs/houdini/nodes/dop/vellumconstraints.html)
carry friction and stiffness as per-constraint attributes, modifiable
via `Vellum Constraint Properties` SOPs. There is no "material"
abstraction; each constraint primitive has its own `friction`,
`stiffness`, `dampingratio`, `compressionstiffness`, `bendstiffness`
attributes. A piece of cloth is N constraints, each individually
parameterised.

This is the *opposite* discipline from a shared material: maximum
expressivity, zero reuse. Houdini's TD audience accepts the cost
because the workflow is "build attributes via Wrangle, share by
copying." Cerid is not Houdini; Material as a shared asset is the
correct shape for a games + robotics + cinematic engine. But: the
Houdini approach justifies *post-v1 per-shape attribute overrides on
top of Material* (a future
`MaterialOverrideComponent::friction_scale` per ECS entity), not
replacing Material with per-instance attributes.

### 2.15 Vortex Studio (CM Labs) — high-fidelity industrial materials

CM Labs' Vortex Studio targets operator-training simulators (mining,
maritime, construction). The product line is closed-source but
documented features include cable / earthmoving (soil) / vehicle
modules with high-fidelity material parameters
([CM Labs](https://cm-labs.com/en/vortex-studio/)). The earthmoving
material includes soil cohesion, internal friction angle, swell
factor, compaction parameters — domain-specific extensions that
Cerid's Material struct does not need to reserve at v1 but should be
expressible as a *post-v1 sub-component* (`SoilMaterialComponent`
attached to a heightfield collider). The architectural pattern is
"core Material + domain-specific sub-component", not "one giant
material struct that includes soil cohesion."

### 2.16 Blender — per-object collision panel

Blender's Rigid Body and Soft Body modifier panels carry friction,
bounce, damping, and "collision margin" as per-object properties.
There is no shared material asset; designers either author per
object or use Python scripts to copy values. Mentioned for
completeness; the discipline does not inform Cerid's design beyond
"unified material asset is correct."

---

## 3. Friction model catalogue

Five friction models are credibly demanded by Cerid's mandate.
Cerid ships all five; the `FrictionModel` enum gates which one a
material activates.

### 3.1 Coulomb (constant μ) — default, universal

The textbook static + dynamic friction model. At a contact:

```
|F_t| ≤ μ_s · |F_n|       (static, when not sliding)
F_t  = -μ_d · |F_n| · v̂_t (dynamic, when sliding; v̂_t = unit tangent of slip)
```

Two parameters: `μ_static`, `μ_dynamic`. Cerid's v1e Sequential
Impulses solver consumes these directly; the friction-cone
linearisation (4-direction pyramid) Catto's [GDC
2005](https://box2d.org/files/ErinCatto_ModelingAndSolvingConstraints_GDC2009.pdf)
material describes uses the dynamic value once slipping is detected.
Determinism: trivial — pure FP add/multiply with deterministic
clamping.

Coverage: 99% of game contacts; the default of every engine in §2.

### 3.2 Stribeck (velocity-dependent low-speed dip)

Real surfaces show *more* friction at very low slip velocities
(static) than at moderate speeds (dynamic), with a continuous
transition through the Stribeck velocity `v_s`:

```
μ(v) = μ_d + (μ_s − μ_d) · exp(-(|v|/v_s)²) + α · |v|
```

Three parameters added to Coulomb: `v_s` (Stribeck velocity, m/s,
typical 0.001–0.1), `α` (viscous coefficient, typical 0–0.1). The
exponential dip below `v_s` captures the breakaway behaviour that
makes vehicle tires "stick" at low speed and the smooth transition
to viscous regime at high speed
([compendium](https://www.sciencedirect.com/topics/engineering/stribeck-curve)).

When to use: vehicle tire slip-curve (slip ratio < 5%); robotics
finger-on-object manipulation; brake squeal modeling; conveyor belt
startup. **Stribeck is the v5 vehicle slice's contact model**; ships
in v1a-material as the enum value, gets its solver treatment in v5.

Determinism: requires `exp` from `crd::math::deterministic` (the
Cephes-style polynomial; per ADR-0063 §2). The `|v|/v_s` division
must round-to-nearest deterministically — IEEE-754 mandates this
across compilers, so safe by construction.

### 3.3 LuGre (state-variable; bristle dynamics)

The model that captures pre-sliding displacement, frictional lag,
stiction, and the Stribeck effect within one ODE
([Canudas-de-Wit et al.
1995](https://lup.lub.lu.se/search/files/6363840/8498924.pdf);
[revisited](https://hal.science/hal-00394988/document)). The
contact's friction is parameterised by a *bristle deflection state*
`z` (one scalar per contact in 1D; a Vec2f per contact in 3D for
each tangent direction):

```
dz/dt = v − σ_0 · |v| / g(v) · z
F_friction = σ_0 · z + σ_1 · dz/dt + σ_2 · v
g(v) = μ_d + (μ_s − μ_d) · exp(-(v/v_s)²)
```

Five parameters: `σ_0` (bristle stiffness, N/m typical 1e5–1e6),
`σ_1` (bristle damping, N·s/m typical 100–1000), `σ_2` (viscous
friction, N·s/m typical 0.1–10), `v_s` (Stribeck velocity), and the
Stribeck pair `μ_s`, `μ_d`. The state `z` is bounded:
`|z| ≤ g(v_max)/σ_0`.

When to use: robotics manipulation (gripper finger pre-sliding before
slip); vehicle low-speed stick-slip (judder, brake stiction); haptic
teleoperation (bilateral control needs LuGre's passivity guarantees).
Standard model in robotics manipulation literature; recent extensions
include physics-informed neural network identification ([arXiv
2504.12441](https://arxiv.org/html/2504.12441v1)) for sim-to-real
transfer.

**Per-contact state:** the bristle deflection `z` lives somewhere.
Two options:

- (a) **Folded into the persistent contact warm-start cache** keyed
  by `(body_a_id, body_b_id, feature_id)` per ADR-0068 §8.4.
  Concentrates state with the contact-point cache that already
  exists; LuGre adds 16 bytes per active contact pair (Vec2f
  `z_tangent` + Vec2f `dz_dt_tangent` — two tangent directions).
- (b) Separate per-pair LuGre state pool referenced from material
  flags. More cache misses; harder to fit into the existing
  warm-start cache lifecycle.

**Cerid picks (a).** §10 spells out the storage. The warm-start
cache already needs a bit-stable (body-pair, feature-id) key for
ADR-0063 determinism; LuGre tags onto that key as a payload
extension, gated on `material.friction_model == LuGre` (otherwise
the 16 bytes are unallocated).

Determinism: LuGre's ODE integration must be Tustin-discretized
(implicit trapezoidal — bilinear transform), NOT explicit Euler. The
explicit Euler form is *conditionally* stable
(`σ_0 · dt < ~0.5`; at 60 Hz substep this requires `σ_0 < ~30 N/m`,
too soft for realistic stiction at `σ_0 ~ 1e5–1e6 N/m`); its
truncation is also step-size-dependent. Tustin (the implicit
trapezoidal bilinear transform) is **A-stable — unconditionally
stable for any positive `dt`** at LuGre's stiff regime
([Tustin discretization
overview](https://www.sciencedirect.com/topics/engineering/tustins-method);
[LuGre integration study
(ScienceDirect)](https://www.sciencedirect.com/science/article/pii/S2405896326000935)).
Tustin discretization at the eylem fixed substep gives the same
bit-pattern result across compilers because the trapezoidal update
involves only `+ - *` and one division by a per-pair constant
precomputed at material-cook time (so runtime never divides). The
v9c fixed-point fallback (per ADR-0063 §6) replaces the FP ODE with
a lookup-table Tustin solve for esports-tier determinism.

The substep choice is therefore an **accuracy concern, not a
stability concern**. LuGre's bristle dynamics have a characteristic
timescale of `1/σ_0` (typically 10 µs at `σ_0 = 1e5`); accurate
capture of stiction transitions during high-frequency stick-slip
oscillation (vehicle judder, gripper finger slip) benefits from a
substep finer than the 60 Hz physics tick. **Cerid runs LuGre
contacts at a 4× sub-substep (240 Hz)** for accuracy, executed in
`EylemSolveSystem` between broadphase and the SI velocity iteration.
The 4× rate is calibrated against the published LuGre identification
literature (≥ 1 kHz is overkill for typical robotics manipulation;
240 Hz preserves the dominant stiction transitions). The sub-substep
is gated per-pair on `friction_model == LuGre` so non-LuGre pairs
pay nothing. The 4× cost is folded into the §11 bench budget for
LuGre-heavy scenes.

### 3.4 Karnopp (piecewise stick-slip; numerical-stability fallback)

Karnopp's dead-zone model treats friction as Coulomb above a small
dead-zone velocity `v_dz` (typical 0.001–0.01 m/s) and as static
zero below
([Karnopp 1985](https://www.semanticscholar.org/paper/Computer-simulation-of-stick-slip-friction-in-Karnopp/1a90361eeb41229358624c834e3d9653135b1ae9)):

```
F = F_static_clamp · v / v_dz       if |v| < v_dz       (dead-zone region)
F = F_dynamic · sign(v)              if |v| ≥ v_dz       (Coulomb region)
```

Two parameters added to Coulomb: `v_dz` (dead-zone velocity), and
`F_static_clamp` is computed from `μ_s · |F_n|`. The dead-zone
absorbs the high-frequency stick-slip oscillation that LuGre solves
via state but Karnopp solves via numerical pragmatism — at the cost
of giving up pre-sliding displacement modeling.

When to use: vehicle ODE integration where LuGre's stiff ODE is too
expensive; fast tire-friction models in tight CPU budgets; driving
simulators where the wheel's contact patch must integrate at >1 kHz;
fixed-point solvers (v9c) where transcendental functions are
forbidden.

Determinism: trivial. Pure piecewise-linear FP arithmetic; no
transcendental, no division (after `1/v_dz` precomputed at material
cook time).

### 3.5 Anisotropic (direction-dependent μ; tires, ice, conveyors)

A single μ scalar is replaced by a Vec3f friction vector authored in
the *material's local frame* (NOT the body's frame — the material's
own frame, set per-shape via `local_rotation`). This decouples the
anisotropy axis from body orientation, fixing the Bullet
limitation (§2.2): a tire and its wheel rotate, but the
friction-axis ("forward = high μ, lateral = low μ") stays in the
tire's local frame.

```
μ(direction) = (μ_x, μ_y, μ_z) projected onto the contact's tangent plane
F_t = -μ_projected · |F_n| · v̂_t (per-axis dynamic Coulomb)
```

Three parameters: `friction_anisotropy` (Vec3f). The material's
`local_rotation` (already on the `Collider` per `collider.hpp:165`)
defines the frame. PhysX's anisotropy scheme has the friction
direction on the *shape* and the magnitudes on the material; Cerid
collapses these onto the material's local frame, eliminating the
3-place authoring problem.

When to use: vehicle tires (high forward μ, low lateral μ); ice
skates (high blade-axis μ, low perpendicular); conveyor rollers
(high cross-roll μ, low along-roll); cloth-on-cloth zipper
constraints.

Determinism: trivial. Pure FP arithmetic; the Vec3f projection is
two dot products and a clamp.

### 3.6 Friction-cone vs friction-pyramid (linearisation for the solver)

These are not friction *models* — they are how the chosen friction
model is fed to the solver. Cerid's v1e SI solver consumes the
4-direction pyramid linearisation per Catto's GDC 2005 material; the
v6 nonsmooth Newton solver will consume the full 3D friction cone
([Drake's
formulation](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)).
Both consume the SAME `Material` — the per-solver linearisation is a
solver concern, not a material schema concern. This is the
single-Material-multiple-solvers pattern Chrono failed to ship and
Cerid commits to.

---

## 4. Restitution model catalogue

Three restitution models cover the survey.

### 4.1 Constant CoR — default, universal

Newton's coefficient of restitution `e ∈ [0, 1]`: post-collision
relative normal velocity = `−e · pre-collision relative normal
velocity`. Single parameter `restitution`. Every engine in §2 ships
this. v1e SI solver applies it as a velocity bias after the normal
impulse computation.

Determinism: trivial (one FP multiply).

### 4.2 Newton restitution (velocity-dependent)

Real surfaces show velocity-dependent CoR — bouncing a steel ball
hard reduces e by elastic-to-plastic transition. The published form
([Lun & Savage; reviewed in Sondergaard 1990; cited in
`osti.gov/1541207`](https://www.osti.gov/servlets/purl/1541207)):

```
e(v) = e_0 · exp(-α · |v|)     where v is the pre-impact normal velocity
```

Two parameters: `e_0` (low-velocity CoR, typical 0.8–0.99 for steel),
`α` (decay rate, typical 0.01–0.1 s/m). Required for granular DEM
(Project Chrono v8d MPM substrate consumes it), realistic
sports-ball physics, billiard cue impact.

Determinism: requires `exp` from `crd::math::deterministic`.

### 4.3 Hunt-Crossley compliant restitution

Replaces the discrete impulse model with a continuous force model:
contact force = stiffness × penetration^n + dissipation ×
penetration^n × penetration_rate. The model is restitution-aware
through the dissipation parameter `d` such that the bounce velocity
after impact is bounded by `1/d`
([Drake](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)).

```
F_normal = k · δ^n · (1 + (3/2) · d · δ̇)
```

Three parameters: `k` (contact stiffness, N/m), `d`
(dissipation, s/m), `n` (Hertz exponent, default 1.0 for flat-flat
contact, 1.5 for sphere-sphere Hertzian).

Required for compliant-contact robotics manipulation; cinematic
soft-deforming actor falls (the dragon's body settles into the
ground rather than discrete-bouncing); medical
soft-tissue simulation. **Hunt-Crossley is the v7 FEM substrate's
default contact model**; ships in v1a-material as the enum value,
gets its solver treatment in v7. Until then, materials with
`HuntCrossley` selected fall back to Constant-CoR computed via
`e ≈ 1 / (1 + d · v_impact)` — degraded fidelity, correct API.

PhysX 5.4's compliant-contact path uses signed restitution + damping
to encode the same model with two parameters; Cerid keeps the
3-parameter Drake form for clarity.

Determinism: pure FP arithmetic (`pow` for the `δ^n` term must use
`crd::math::deterministic::pow`; for `n = 1.0` the codegen path
collapses to a single FP multiply).

---

## 5. Surface velocity

A material-authored Vec3f added to the contact's relative tangent
velocity at solver time. Used for:

- Conveyor belts (frame-local +X velocity).
- Rolling tire surfaces (tangent to the tire's roll direction).
- Escalators (frame-local 45° vector).
- Water currents on solid colliders (a "river collider" pushing
  bodies floating on the surface).
- Treadmills (frame-local −Z velocity for character-running training).

PhysX's split (kinematic-body + ContactModify-callback, NOT on
PxMaterial) is the principal failure mode. The PhysX kinematic-body
path requires the entire surface to be a kinematic body; the
ContactModify path is GPU-incompatible in IsaacSim. Bullet has no
direct API. Box2D v3 has none. **Cerid corrects this: surface
velocity is a first-class material field, authored in the material's
local frame, applied identically across CPU and GPU paths.**

```
Material::surface_velocity_local : Vec3f   // default (0,0,0)
```

Combine rule: when two materials with non-zero surface velocity
contact, the contact-frame surface velocity is the *sum* (or
*replace* via a per-material flag — same Add/Replace pattern as
ADR-0067 fields). Default Add. The two-conveyor-meeting case is rare
enough to delegate to designer override.

Apply at solver time as a velocity bias on the friction constraint:
the relative slip velocity used by the friction solver becomes
`v_rel - v_surface` (with appropriate frame transforms via the
material's `local_rotation`). The bias is conservative — it doesn't
add energy to the system if the body is at rest relative to the
surface.

Determinism: trivial. The bias is a pure FP add per contact.

---

## 6. Combine modes

When two colliders touch with different materials, their parameters
combine via a per-material `CombineMode` enum. The survey's modes:

| Mode | Formula | Where shipped |
|---|---|---|
| `Average` | `(a + b) / 2` | PhysX, Unity classic, Unreal Chaos (default) |
| `Min` | `min(a, b)` | PhysX, Unity, Unreal, Godot ("not rough") |
| `Max` | `max(a, b)` | PhysX, Unity, Unreal, Godot ("rough"), most engines for restitution default |
| `Multiply` | `a · b` | PhysX, Unity, Unreal, Bullet (hard-coded) |
| `GeometricMean` | `sqrt(a · b)` | Box2D v3 (default for friction), Jolt (default), Unity DOTS, AGX (default) |

Five values cover everything in the survey. Cerid ships all five.

**Defaults:**
- Friction: `GeometricMean` — matches Box2D v3 / Jolt / Unity DOTS /
  AGX modern consensus. Box2D's [combine
  rationale](https://box2d.org/posts/2024/08/determinism/) is the
  most rigorous published: geometric mean preserves
  `combine(0, x) == 0` (zero-friction surface forces zero-friction
  contact regardless of the other surface) and `combine(x, x) == x`
  (idempotency). Average has neither; multiply has the first but
  doesn't preserve idempotency.
- Restitution: `Max` — matches PhysX / Unreal default. The
  bouncier-surface-wins matches player intuition ("a rubber ball is
  bouncy regardless of the wall").

**Pairwise determinism contract.** All five proposed modes are
commutative (`f(a, b) == f(b, a)`), so the order in which two
materials' parameters are passed to the combine function does not
affect the result. **However**, future asymmetric modes (a "first
material wins" mode for robotics priority, mirroring MuJoCo's
`priority` rule) would break this. As structural insurance, the
contract is:

> The combine function is always invoked as
> `combine(material[min(id_a, id_b)], material[max(id_a, id_b)])`
> ordered by stable `MaterialId`.

This rule has zero cost today (commutative formulas don't care) but
guards against any future enum addition that introduces asymmetry.
Cite Box2D's [determinism
post](https://box2d.org/posts/2024/08/determinism/) for the
ordering discipline.

**Asymmetric combine (Godot's "rough" boolean):** explicitly
rejected. The `rough` flag is a designer-friendly hack that encodes
"if either side is rough, use max friction" — exactly equivalent to
`CombineMode::Max` per-material. Cerid expresses this through the
enum value, not through a per-material asymmetric flag.

**Combine-mode mismatch.** What if material A's friction_combine is
`Average` and material B's is `Min`? The contract: **the higher
priority enum value wins**, with priority order
`Max > Min > Multiply > GeometricMean > Average`. This is the
PhysX-documented rule; Cerid lifts it. Designers who need precise
control author both materials with the same combine mode.

---

## 7. Density vs explicit mass authoring

Contact: every dynamic body in eylem needs mass + inertia tensor.
The two authoring paths:

**Density path (default).** Material carries `density` (kg/m³,
default 1000.0 = water). At body construction or when colliders are
added, the engine computes:

```
mass = Σ (collider_volume · collider_material.density)
inertia_tensor = Σ (analytic_tensor(collider) · density)
```

Summation order: by `ColliderId` (stable across runs), with bit-
stable `+` per ADR-0063 commutative-merge protocol. The analytic
tensor for each shape is documented:

- Sphere: `(2/5) · m · r²` per axis
- Box: `(1/12) · m · (h_y² + h_z²)` for x-axis (etc.)
- Capsule: closed-form integral of cylinder + 2 hemispheres
- ConvexHull: Mirtich's [Fast and Accurate Computation of
  Polyhedral Mass
  Properties](http://www.cs.berkeley.edu/~jfc/mirtich/massProps.html)
- TriangleMesh: not supported (the mesh is static-only at v1; static
  bodies don't need mass)
- Heightfield: not supported (same)
- Sdf: integral over the SDF's negative region; deferred to v7 (FEM
  needs it anyway)

**Explicit mass path (override).** `RigidBody::inv_mass` and
`RigidBody::inv_inertia` (already on `rigid_body.hpp:62-63`) are
non-zero → engine uses the authored values, ignores density.
Convention: `inv_mass = 1.0 / mass`; setting `inv_mass = 0` marks
the body as effectively static (matches Bullet / Box2D / PhysX).

**Default density: 1000 kg/m³ (water).** Justification:

- Water is the most physically intuitive default — designers grasp
  "denser than water" or "less dense than water" instantly.
- Box2D v3's default is `1.0` (in 2D — `kg/m²`, not the same unit);
  not portable as a 3D default.
- PhysX has no default — mass must be explicitly computed via
  `updateMassAndInertia(actor, density)`. Cerid chooses
  designer-friendly defaults over PhysX's
  always-explicit discipline.
- 1000 kg/m³ on a 1m box gives 1000 kg mass — round numbers easy
  to sanity-check.
- Materials shipped in the v1k `MaterialLibrary`
  (rubber/steel/ice/wood/water/concrete/flesh) override the default
  to physically correct values.

**Inertia tensor override (the rare third path).** Compound shapes
with displaced centers (a hammer = handle + head; the analytic
tensor of the union is wrong because the analytic algorithms assume
each collider's mass is at its centroid). For these, designers
author `inertia_tensor_override` on the body (Vec3f for the diagonal,
the single off-diagonal term reserved for v1f via
`rigid_body.hpp` already-present `inv_inertia`). This is the
4th-generation PhysX discipline: density default, density-derived
inertia, full override.

**The contract (locked):**

> If `RigidBody::inv_mass == 0` (default), engine computes mass and
> inertia from `Σ collider_volume · material.density`. If
> `RigidBody::inv_mass > 0`, engine uses the authored values and
> ignores density entirely. The mass-derivation runs at body
> construction AND on `add_collider` / `remove_collider` calls.

---

## 8. Per-collider material override

A character entity has a foot capsule (rubber boots), a body capsule
(leather armor), a hand capsule (silk gloves), and a head sphere
(steel helmet). Each surface has different friction. The contract:

**Material lives in a scene-owned `MaterialPool` indexed by
`MaterialId` (32-bit handle).** The `Collider` struct carries a
`MaterialId` field; default = `MaterialId::default_material()` (a
shipped fallback with `μ = 0.5`, `e = 0`, `density = 1000`). The
`RigidBody` struct does NOT carry a material — bodies are just rigid
state; *material lives on the geometry*. PhysX's per-shape model is
the right one.

Three storage shapes considered:

- **(a) Inline `Material` in `Collider`.** Blows the union budget
  (`Material` at 64B + collider variant at 24B = 88B per collider;
  the union currently caps at 24B). Rejected — exceeds the v1a
  freeze constraint.
- **(b) `MaterialId` handle + scene `MaterialPool`.** Adds 4 bytes
  to `Collider` for the handle. `MaterialPool` is a contiguous
  `Array<Material>` owned by the scene; lookup is one indirection
  (`scene.materials[collider.material_id]`). Materials shared
  across many colliders cost 64B once + 4B per reference. **This is
  PhysX's shape and Jolt's shape.** Picked.
- **(c) `Material*` raw pointer.** Rejected — pointer storage breaks
  the snapshot-replay determinism contract (pointers vary across
  runs). Handle + pool is the only deterministic shape.

**`MaterialPool` properties:**

- Indexed by `MaterialId` (`crd::u32`, layout `[generation:8 |
  index:24]` matching `BodyId`/`ColliderId` per `types.hpp`).
- Capacity bounded by `crd::u32` index (~16M materials per scene),
  but warns at 10k unique materials (cache footprint > L2; matches
  the IsaacSim 64k PhysX limit motivation).
- Round-trips through CRDR (`'EMAT'`) — the snapshot includes the
  full pool.
- Hot-swappable at runtime via `scene.update_material(id, new_value)`
  — the next substep sees the change. Useful for designer tuning
  loops.

**Default material:** `MaterialId{1, 1}` (slot 1, generation 1) is
the shipped "default" with parameters matching the v1k `Default`
library entry. Slot 0 is reserved as null. Every shipped scene has
the default material present; designers can override per-collider
without authoring a material first.

---

## 9. Damage / fracture material parameters (reservation)

For cinematic destruction (post-v1 `GeometryCollectionComponent` per
ADR-0068 §10 — geometry collections / mass-scale destruction — and
the Phase 8 cinematic module). The Material schema reserves *room*
for these fields via fixed-position padding bytes; impl ships with
destruction in a future slice. Reserved fields:

- `yield_stress` (f32, Pa) — stress at which the material yields
  plastically. Triggers fracture in the destruction substrate.
- `fracture_toughness` (f32, Pa·√m) — energy required to propagate
  a crack. Combined with yield stress to compute fracture
  threshold per the Griffith criterion.
- `damage_threshold` (f32) — accumulated damage at which a piece
  fractures (post-v1 destruction substrate's `accumulator` consumes
  this).

These three fields cost 12 bytes; reserved via padding in §12. The
damage substrate (v1+ destruction; not on the v1 critical path)
will read them; v1 ignores them.

Why reserve at v1a freeze rather than grow the struct in a major
version: ADR-0068 §10.10 reserves
`GeometryCollectionComponent` as a future component composed atop
dynamic bodies. The component's spawn pattern needs material
fracture parameters at construction time. Forcing a major-version
bump on the Material struct to accommodate this would cascade to
every consumer (cooker artifact format, snapshot layout, every
solver). Reserving the bytes at v1a is cheap (12 bytes of padding);
the alternative (rewriting half the v1 code in v2) is not.

This matches ADR-0067's discipline of reserving slots in the
`FieldFormula` enum for Tier 3 (Script) before Phase 4 scripting
ships. Substrate stability across the planned roadmap is part of the
v1a freeze's value.

---

## 10. Determinism (cross-cutting)

Per ADR-0063, materials participate in the cross-platform replay-
hash CI. Six material-specific failure modes are blocked at the
architecture level:

**1. Friction-model evaluation must be deterministic FP.** Coulomb
and Karnopp use only `+ - * /` and IEEE-754 `min`/`max` — bit-exact
across compilers. Stribeck and LuGre call `exp` and require
`crd::math::deterministic::exp` (per ADR-0063 §2). Hunt-Crossley
calls `pow` requiring `crd::math::deterministic::pow`. The v9b CI
matrix asserts the deterministic-substitute path is taken; a custom
clang-tidy check (already proposed in ADR-0063 §1) bans
`std::sin/cos/exp/pow/log` in `engine/eylem/**` paths.

**2. LuGre ODE integration uses Tustin discretization.** The
explicit Euler form is FP-deterministic but step-size-dependent at
LuGre's stiff regime (`σ_0 · dt < 0.5` for stability). Tustin
(implicit trapezoidal) is unconditionally stable AND bit-exact
across compilers because the trapezoidal update reduces to `+ - *`
plus one division by a per-pair constant computed at material-cook
time. **The constant is precomputed in the cooker and stored in the
cooked Material; runtime never divides.**

**3. Mass derivation summation order pinned by `ColliderId`.** When
a body has N colliders contributing to mass via `Σ
collider_volume·density`, the summation runs in ascending
`ColliderId` order (stable across runs). FP `+` is commutative but
not associative; without pinned order, `mass = sum_in_order_A` and
`mass = sum_in_order_B` differ in the last bit. The pin is an
internal `ColliderId`-keyed sort before `accumulate`, costing
~µs per body construction. ADR-0063 §4's "fixed-position write"
protocol covers this directly.

**4. Combine-mode evaluation order pinned by `MaterialId`.** Per §6,
the combine function is invoked as
`combine(material[min(id_a, id_b)], material[max(id_a, id_b)])`.
Today commutative; structurally insured against future asymmetric
modes.

**5. Material library indices are content-addressed, not
sequential.** A `MaterialId` assigned via `scene.create_material()`
*could* be a sequential counter, but the öbek/cooker path computes
the id as FNV-1a-64 over the material's serialized parameters
(matching the `FieldId` discipline in ADR-0067 §3). Identical
material parameters produce identical ids regardless of authoring
order. The id is stable across runs — replay-hash CI proves it.
The runtime-only mutation path (`scene.update_material`) gets a
sequential id from the cooker hash + a salt; subsequent updates
rehash.

**6. Surface-velocity application order.** When two non-zero surface
velocities meet, the contact's bias is computed as `surface_a +
surface_b` (the default Add combine) — order-independent and
deterministic. The optional Replace mode breaks this; the contract
in §5 picks `min(material_id)` wins (stable per §6 ordering).

The v9b CI matrix runs the v1j replay-hash test (10 seconds of
"100 falling boxes with 4 different materials + 1 ragdoll with LuGre
gripper material + 1 character running on Stribeck-tire wheel") and
asserts the snapshot hash matches across MSVC / clang / gcc × x64 /
ARM × Windows / Linux. Regression fails the build; bisect points at
the diverging compiler.

---

## 11. Performance

**Bytes per Material — target 64 bytes.** Justification:

- One x64 / ARM64 cache line. A material lookup is a single cache
  miss; subsequent reads of all fields are L1 hits.
- 16 materials per 1 KB; 1024 materials per 64 KB (typical L1
  data cache size). Realistic scenes use 50–500 unique materials;
  the working set fits L1.
- Allows the friction triple (sliding/torsional/rolling per §2.8) to
  ship in v1f without struct growth.

The current stub at 20 bytes (per `material.hpp:36`) is too small
for the v1a freeze's parameter set. The v1a layout proposed in §12
is 64 bytes with 4 bytes of reserved padding for the post-v1
destruction parameters (§9) plus 4 bytes for v1f rolling friction.

**LuGre per-contact bristle state.** Per §3.3 option (a), folded
into the persistent contact warm-start cache. Cost:

- 16 bytes per active contact pair using LuGre material (2× Vec2f for
  tangent-direction bristle deflection + rate).
- Allocated lazily when the cache observes
  `material.friction_model == LuGre` on either pair material.
- A 1k-contact scene with LuGre uses 16 KB of bristle state — fits
  L2 trivially.
- A 10k-contact scene (mass destruction with LuGre rubble): 160 KB,
  spills into L3 but does not affect non-LuGre pair throughput
  (the cache is keyed by pair-id; non-LuGre pairs see no overhead
  beyond the tag check).

**Cooker artifact size.** A material is ~100 bytes serialized
(struct + small TOML overhead). A typical scene's MaterialPool
serialised: 50 materials × 100 bytes ≈ 5 KB — negligible in the
CRDR `'EYLM'` snapshot.

**Hot-path cost.** The friction solver reads `μ` (1 cycle per
contact) and applies the linearized pyramid (8 cycles per contact in
SIMD). Material lookup adds 1 cache-line-aware indirection per
contact. The `MaterialId` handle + cache-line-aligned `MaterialPool`
keeps this at ~3 cycles per contact in the warm path; cold misses
~30 cycles. SIMD-friendly because materials are read once per pair
and broadcast to all lanes.

**Bench targets** (calibrated against PhysX, Bullet, Box2D v3
published numbers for friction-rich scenes; CI assertions in
`tests/eylem-rigid3d/bench_materials.cpp` shipped with v1a-material):

| Workload | Budget |
|---|---|
| 10k contacts, all Coulomb | ≤ 0.05 ms (trivial) |
| 10k contacts, mixed Coulomb + Stribeck (90/10) | ≤ 0.10 ms |
| 1k contacts, all LuGre (4× sub-substep) | ≤ 0.40 ms |
| 100 contacts, all Hunt-Crossley + compliant | ≤ 0.05 ms |
| Mass derivation, 1k bodies × 5 colliders | ≤ 0.20 ms |

Regressions fail the build, same model as the Phase 2.5 jobs
benchmarks. Measured on Zen 4 / Raptor Lake; NEON 4-lane M-series
scales to ~2× the budget.

---

## 12. Recommended Cerid architecture (for ADR-0069)

Lock the following.

### 12.1 Material struct shape (locked at v1a freeze)

```cpp
struct Material
{
    // ----- Friction (24 bytes) -----
    FrictionModel    friction_model;      // 1B  — Coulomb / Stribeck / LuGre / Karnopp / Anisotropic
    CombineMode      friction_combine;    // 1B  — default GeometricMean
    crd::u8          _pad_friction[2];    // 2B  — alignment to f32
    crd::f32         friction_static;     // 4B  — μ_s
    crd::f32         friction_dynamic;    // 4B  — μ_d
    crd::math::Vec3f friction_anisotropy; // 12B — Vec3f in material-local frame

    // ----- Friction model parameters (8 bytes) -----
    crd::f32         stribeck_velocity;   // 4B  — v_s (Stribeck/LuGre)
    crd::f32         viscous_coefficient; // 4B  — α (Stribeck), or σ_2 (LuGre)

    // ----- Restitution (12 bytes) -----
    RestitutionModel restitution_model;   // 1B  — Constant / Newton / HuntCrossley
    CombineMode      restitution_combine; // 1B  — default Max
    crd::u8          _pad_restitution[2]; // 2B
    crd::f32         restitution;         // 4B  — e_0 (Constant, Newton); replaced by stiffness for HC
    crd::f32         restitution_decay;   // 4B  — α (Newton); dissipation d (HuntCrossley)

    // ----- Surface (12 bytes) -----
    crd::math::Vec3f surface_velocity;    // 12B — material-local frame, m/s

    // ----- Mass derivation (4 bytes) -----
    crd::f32         density;             // 4B  — kg/m³, default 1000.0 (water)

    // ----- Damage / fracture reservation (4 bytes; impl ships post-v1) -----
    crd::f32         yield_stress;        // 4B  — Pa, reserved for §9; v1 ignores
};

// API surface freeze pin.
static_assert(sizeof(Material)  == 64, "Material must pack to 64 bytes (one cache line)");
static_assert(alignof(Material) == 4,  "Material alignment is 4");
```

64 bytes — one cache line. 4 bytes of yield_stress reserved
(actually used by post-v1 destruction). Two parameter slots
overloaded by friction model (v_s + α for Stribeck, σ_0 + σ_2 for
LuGre via cooker translation; LuGre's σ_1 lives in the per-contact
bristle cache); two slots overloaded by restitution model. The
`FrictionTriple` enum slot (reserved at v1a freeze; impl in v5)
reinterprets the `friction_anisotropy` Vec3f as
`(sliding, torsional, rolling)` per the MuJoCo §2.8 pattern — same
12 bytes, different reading. **The struct does not grow; the enum
gates the slot interpretation.**

### 12.2 New enums in `types.hpp` (additive, before v1a freeze)

```cpp
enum class FrictionModel : crd::u8
{
    Coulomb        = 0, // default; constant μ
    Stribeck       = 1, // velocity-dependent low-speed dip
    LuGre          = 2, // state-variable; per-contact bristle
    Karnopp        = 3, // dead-zone piecewise (vehicle ODE)
    Anisotropic    = 4, // Vec3f friction in material-local frame
    FrictionTriple = 5  // sliding/torsional/rolling triple (MuJoCo §2.8 pattern; v5 impl)
};

enum class RestitutionModel : crd::u8
{
    Constant     = 0, // default; e ∈ [0, 1]
    Newton       = 1, // velocity-dependent: e(v) = e_0 · exp(-α · |v|)
    HuntCrossley = 2  // compliant: F = k · δ^n · (1 + 1.5 · d · δ̇)
};

// Extend existing CombineMode enum:
enum class CombineMode : crd::u8
{
    Average       = 0, // (a + b) / 2
    Min           = 1, // min(a, b)
    Max           = 2, // max(a, b)
    Multiply      = 3, // a · b
    GeometricMean = 4  // sqrt(a · b)  -- new in v1a-material
};
```

`GeometricMean` slot is *additive* — not a breaking change to the
existing enum because no shipped code reads value 4 yet.

### 12.3 MaterialId + MaterialPool (in `eylem.hpp` + scene)

```cpp
struct MaterialId
{
    crd::u32 raw = 0;
    [[nodiscard]] constexpr crd::u32 index() const noexcept;
    [[nodiscard]] constexpr crd::u32 generation() const noexcept;
    [[nodiscard]] constexpr bool is_null() const noexcept;
    [[nodiscard]] static constexpr MaterialId null() noexcept;
    [[nodiscard]] static constexpr MaterialId default_material() noexcept;
    [[nodiscard]] static constexpr MaterialId make(u32 index, u32 generation);
    [[nodiscard]] constexpr bool operator==(const MaterialId&) const = default;
};
static_assert(sizeof(MaterialId) == 4);
```

Layout matches `BodyId`/`ColliderId` (`[generation:8 | index:24]`).
Scene owns `MaterialPool` (a `crd::containers::Array<Material>`);
`scene.create_material(material)` returns a `MaterialId`;
`scene.update_material(id, new)` mutates in place. Slot 0 is null;
slot 1 is the shipped `default_material()`.

### 12.4 Per-collider material assignment

Add to `Collider` struct (per `collider.hpp:160`):

```cpp
struct Collider
{
    ColliderShape    shape           = ColliderShape::Sphere;
    ColliderFlags    flags{};
    MaterialId       material        = MaterialId::default_material(); // NEW
    crd::math::Vec3f local_position{0.0F, 0.0F, 0.0F};
    crd::math::Quatf local_rotation{0.0F, 0.0F, 0.0F, 1.0F};
    union { /* ... */ };
};
```

Cost: 4 bytes per collider; one `Collider` becomes 60 bytes (current
56 + 4); the 64-byte cache line still fits. The `RigidBody` struct
does NOT grow — bodies do not carry materials, only colliders do.

### 12.5 Combine-mode pinning rule (locked)

```
combine(a, b) := mode_priority_winner(a.combine_mode, b.combine_mode)
                 ( material[min(id_a, id_b)].param,
                   material[max(id_a, id_b)].param )
```

Where `mode_priority_winner` resolves combine-mode mismatch as
`Max > Min > Multiply > GeometricMean > Average`. The `min`/`max` on
ids guards against future asymmetric mode addition; today the
ordering has zero cost (commutative formulas).

### 12.6 Per-pair material (AGX-style) — explicitly REJECTED for v1

Storage cost is `O(N²)` in distinct-material count even sparse;
authoring cost ("declare every pair I care about") is significant;
robotics workflows that need it (granular DEM with cohesive soil)
land in v8d MPM substrate alongside their own material extension
(`SoilMaterialComponent` per §2.15 Vortex pattern).

The v1 contract is "Material + commutative combine modes". Pairwise
override is a v9+ revisit if FEM / hydroelastic substrate (v7) needs
it; even then likely ships as a side-channel
`MaterialPairOverridePool` rather than expanding `Material` itself.

### 12.7 Density default: 1000 kg/m³

Locked. Designer-friendly; matches a 1m³ box → 1000 kg.

### 12.8 LuGre per-contact bristle state

Folded into the persistent contact warm-start cache (§3.3 option
(a)). Storage:

```cpp
struct ContactWarmStartEntry
{
    BodyId    body_a;
    BodyId    body_b;
    crd::u32  feature_id;          // ADR-0068 §8.4 contact-feature stable hash
    crd::f32  normal_impulse;      // warm-start
    crd::math::Vec2f friction_impulse;
    // LuGre payload (only allocated when material's friction_model == LuGre):
    crd::math::Vec2f bristle_z;          // bristle deflection per tangent direction
    crd::math::Vec2f bristle_dz_dt;      // bristle deflection rate
};
```

The LuGre fields take 16 bytes; allocated lazily — non-LuGre cache
entries omit them via a per-pair tag bit. Cache key matches
ADR-0068 §8.4 already.

### 12.9 Cooker artifact: `'EMAT'` (CRDR FourCC)

`.physics-material.toml` cooker handler (lands with v1k cooker
batch) parses the TOML schema:

```toml
[material]
friction_model    = "Coulomb"     # or Stribeck / LuGre / Karnopp / Anisotropic
friction_static   = 0.6
friction_dynamic  = 0.4
friction_combine  = "GeometricMean"
restitution_model = "Constant"
restitution       = 0.3
restitution_combine = "Max"
surface_velocity  = [0.0, 0.0, 0.0]
density           = 1000.0

# Friction model–specific parameters (overlay; cooker validates).
[material.stribeck]
v_s = 0.01
alpha = 0.05
```

The cooker:
1. Validates the parameter ranges (e.g., `density > 0`, `μ ≥ 0`,
   restitution ∈ [0, 1] for Constant model).
2. For LuGre, precomputes the Tustin-discretization constants from
   `(σ_0, σ_1, σ_2, dt_substep)` to make the runtime ODE step
   division-free (§10 failure mode 2).
3. Computes the `MaterialId` as FNV-1a-64 over the canonical
   parameter bytes.
4. Emits the CRDR `'EMAT'` artifact. Multi-material packs ship as
   a `MaterialLibrary` (CRDR `'EMLB'` reserving for v1k) with
   indexed lookup.

### 12.10 Shipped MaterialLibrary (8 default materials)

Authored as a CRDR `'EMLB'` pack shipping with v1k. Each material
is the canonical physical reference; sandbox demos and the öbek
default prefabs reference these by name. Locked parameter sets
(approximate published material constants):

| Material | μ_s | μ_d | e | density (kg/m³) | model | Notes |
|---|---|---|---|---|---|---|
| `Default` | 0.5 | 0.5 | 0.0 | 1000 | Coulomb | universal fallback |
| `Rubber` | 1.0 | 0.8 | 0.7 | 1100 | Coulomb | tire baseline |
| `Steel` | 0.5 | 0.4 | 0.4 | 7850 | Coulomb | structural metal |
| `Ice` | 0.05 | 0.02 | 0.1 | 920 | Anisotropic | low μ on x/y, lower along skate axis |
| `Wood` | 0.4 | 0.3 | 0.3 | 700 | Coulomb | mid-range |
| `Concrete` | 0.7 | 0.6 | 0.1 | 2400 | Coulomb | level geometry default |
| `Water` | 0.0 | 0.0 | 0.0 | 1000 | Coulomb + drag | DAW / fluid demo |
| `Flesh` | 0.6 | 0.5 | 0.1 | 1050 | HuntCrossley | ragdoll / cinematic actor |

Designers mix-and-match per scene; the öbek prefabs in v1k
(`StackingDemo`, `RagdollDemo`, `VehicleDemo`) reference the library
by name. Eight materials is enough to ship a credible demo without
forcing every project to rebuild the catalogue.

### 12.11 Slice plan (locked)

Slot into Phase 3.1 v1 as:

| Sub-slice | Scope | LOC | Tests |
|---|---|---|---|
| **v1a-material-a** | `Material` struct (64B) + `FrictionModel` / `RestitutionModel` enums + `CombineMode::GeometricMean` extension + `MaterialId` strong type + `default_material()` constant. Static-asserts pin layout. | ~150 | ~5 |
| **v1a-material-b** | `MaterialPool` on scene; `create_material` / `update_material` / `get_material` API; öbek/cooker round-trip stub. | ~200 | ~4 |
| **v1a-material-c** | Per-collider `material` field on `Collider`; `set_material` API; default-fallback validation; per-collider compound test (a body with 3 colliders, 3 materials). | ~100 | ~3 |
| **v1a-material-d** | Mass derivation: `Σ collider_volume · material.density`; `inv_mass = 0` triggers derivation, otherwise uses authored. ColliderId-stable summation order. | ~250 | ~5 |
| **v1e-material** | Coulomb friction integration into SI solver (v1e); friction-pyramid linearization consumes `material.friction_static`/`friction_dynamic`; combine-mode evaluation per §6. | ~300 | ~6 |
| **v1k-material-cooker** | `.physics-material.toml` handler + CRDR `'EMAT'` + `MaterialLibrary` (`'EMLB'`) + 8 shipped materials. | ~400 | ~5 |
| **v1k-material-bench** | `bench_materials.cpp` per §11 budget table; CI assertions. | ~100 | bench |

**Deferred sub-slices** (ship at their natural slot inside the
already-frozen v1a API surface):

| Sub-slice | Scope | When | LOC |
|---|---|---|---|
| v5-material-stribeck | Stribeck friction implementation in solver | with vehicles v5 | ~150 |
| v5-material-lugre | LuGre integration: per-contact bristle state in warm-start cache + Tustin sub-substep + cooker preprocessing | with vehicles v5 | ~400 |
| v5-material-karnopp | Karnopp friction in solver (vehicle ODE path) | with vehicles v5 | ~100 |
| v5-material-anisotropic | Anisotropic friction in SI solver: Vec3f frame-projected μ | with vehicles v5 | ~150 |
| v8d-material-newton | Newton restitution in SI solver | when granular DEM lands (v8d MPM) | ~100 |
| v7-material-huntcrossley | Hunt-Crossley compliant contact in FEM solver | with FEM v7 | ~250 |
| v5-material-friction-triple | Sliding/torsional/rolling friction triple (MuJoCo §2.8 pattern; uses `FrictionModel::FrictionTriple` enum slot reserved at v1a freeze) | with v5 vehicles slice | ~200 |
| post-v1-material-fracture | Fracture parameters consumed by destruction substrate | with `GeometryCollectionComponent` | ~150 |

The deferred slices fill in formula impls inside the already-frozen
`Material` shape — same discipline as ADR-0067's blocked sub-slices
(Gradient, Script). The v1a-material-{a, b, c, d, e}, v1e-material,
and v1k cluster are the v1a freeze critical path; they MUST close
before v1a interface freeze.

### 12.12 API surface freeze — cascading consequences

`sizeof(Material) == 64` is a frozen pin. Changing it post-v1a
requires a major-version bump of `crd-eylem`, cascading to:

- The cooker artifact format (`'EMAT'` schema_version field).
- The snapshot artifact format (`'EYLM'` material pool layout).
- Every öbek prefab serializing material-bearing colliders.
- Every shipped scene file referencing `MaterialLibrary` materials.

The 4 bytes of `yield_stress` reservation (§9) absorb the post-v1
destruction parameters; the `FrictionModel::FrictionTriple` enum
slot (reserved at v1a; v5 impl) reinterprets `friction_anisotropy`
as `(sliding, torsional, rolling)` without struct growth; the
`viscous_coefficient` slot reserves room for LuGre's `σ_2` by
overloading interpretation. The schema is designed to absorb the
planned roadmap without bumps.

If a future need emerges that genuinely doesn't fit
(e.g., hyperelastic Mooney-Rivlin coefficients for FEM), the path is
a side-channel `MaterialFEMComponent` attached to colliders in v7
— matching the §2.15 Vortex pattern (core Material + domain-specific
sub-component). The base `Material` does not grow.

---

## 13. References

### Engines and tools (primary docs)

- [PhysX 5.1 PxMaterial](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/_build/physx/latest/class_px_material.html)
- [PhysX 5 Rigid Body Dynamics — friction & restitution](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/docs/RigidBodyDynamics.html)
- [PhysX 5.4 PxMaterialFlag (eCOMPLIANT_CONTACT deprecation)](https://nvidia-omniverse.github.io/PhysX/physx/5.3.1/_api_build/struct_px_material_flag.html)
- [PhysX surface velocity discussion (Omniverse #280)](https://github.com/NVIDIA-Omniverse/PhysX/discussions/280)
- [Bullet btCollisionObject (source)](https://github.com/bulletphysics/bullet3/blob/master/src/BulletCollision/CollisionDispatch/btCollisionObject.h)
- [Bullet anisotropic friction (forum)](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=12500)
- [Jolt Body class reference](https://jrouwe.github.io/JoltPhysics/class_body.html)
- [Jolt per-shape material discussion (#1311)](https://github.com/jrouwe/JoltPhysics/discussions/1311)
- [Box2D v3 Shape group docs](https://box2d.org/documentation/group__shape.html)
- [Box2D v3 simulation overview](https://box2d.org/documentation/md_simulation.html)
- [Box2D Determinism (Catto 2024)](https://box2d.org/posts/2024/08/determinism/)
- [Unity DOTS Physics Material struct](https://docs.unity3d.com/Packages/com.unity.physics@1.2/api/Unity.Physics.Material.html)
- [Unity DOTS Custom Physics Materials](https://docs.unity3d.com/Packages/com.unity.physics@1.2/manual/custom-materials.html)
- [Unreal UPhysicalMaterial](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/PhysicsCore/UPhysicalMaterial)
- [Unreal UPhysicalMaterialMask](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/PhysicalMaterials/UPhysicalMaterialMask)
- [Godot 4 PhysicsMaterial](https://docs.godotengine.org/en/stable/classes/class_physicsmaterial.html)
- [Godot proposal #11715 — combine modes](https://github.com/godotengine/godot-proposals/issues/11715)
- [MuJoCo XML reference (geom + solref/solimp/friction)](https://mujoco.readthedocs.io/en/stable/XMLreference.html)
- [MuJoCo modeling overview](https://mujoco.readthedocs.io/en/latest/modeling.html)
- [Drake compliant contact](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)
- [Drake hydroelastic user guide](https://drake.mit.edu/doxygen_cxx/group__hydroelastic__user__guide.html)
- [Drake default contact parameters](https://drake.mit.edu/doxygen_cxx/group__contact__defaults.html)
- [Drake hydroelastic basics tutorial](https://github.com/RobotLocomotion/drake/blob/master/tutorials/hydroelastic_contact_basics.ipynb)
- [Drake Hunt-Crossley PR #5197](https://github.com/RobotLocomotion/drake/pull/5197)
- [IsaacLab schemas (RigidBodyMaterialCfg)](https://isaac-sim.github.io/IsaacLab/main/source/api/lab/isaaclab.sim.schemas.html)
- [IsaacSim per-actor randomize_rigid_body_material](https://isaac-sim.github.io/IsaacLab/main/source/api/lab/isaaclab.envs.mdp.html)
- [Project Chrono ChMaterialSurfaceNSC](https://api.projectchrono.org/classchrono_1_1_ch_material_surface_n_s_c.html)
- [Project Chrono ChMaterialSurfaceSMC](https://api.projectchrono.org/classchrono_1_1_ch_material_surface_s_m_c.html)
- [Project Chrono Collisions overview](https://api.projectchrono.org/collisions.html)
- [AGX Dynamics Material](https://www.algoryx.se/documentation/complete/agx/tags/latest/doc/html/classagx_1_1Material.html)
- [AGX Dynamics for Unreal — Materials chapter](https://us.download.algoryx.se/AGXUnreal/documentation/current/materials.html)
- [AGX Surface Velocity Conveyor Belt example](https://www.algoryx.se/documentation/complete/agx/tags/latest/doc/UserManual/source/surface_velocity_conveyor_belt.html)
- [Vortex Studio (CM Labs)](https://cm-labs.com/en/vortex-studio/)
- [Maya nDynamics overview](https://download.autodesk.com/global/docs/maya2013/en_us/files/GUID-03923C56-9612-4C27-834E-2F417FC07323.htm)
- [Houdini Vellum Constraint Properties (DOP)](https://www.sidefx.com/docs/houdini/nodes/dop/vellumconstraintproperty.html)
- [Houdini Vellum Constraints](https://www.sidefx.com/docs/houdini/nodes/dop/vellumconstraints.html)
- [Simbody Hunt-Crossley force](https://simtk.org/api_docs/molmodel/api_docs22/Simbody/html/classSimTK_1_1HuntCrossleyForce.html)

### Algorithms and papers

- [Canudas-de-Wit, Olsson, Åström, Lischinsky — A New Model for Control of Systems with Friction (LuGre), IEEE TAC 1995](https://lup.lub.lu.se/search/files/6363840/8498924.pdf)
- [Åström, Canudas-de-Wit — Revisiting the LuGre friction model (2008)](https://hal.science/hal-00394988/document)
- [Hunt & Crossley — Coefficient of restitution interpreted as damping in vibroimpact (1975); 50-year retrospective](https://www.sciencedirect.com/science/article/pii/S0094114X2500312X)
- [A compendium of contact force models inspired by Hunt and Crossley's cornerstone work](https://www.sciencedirect.com/science/article/abs/pii/S0094114X21002573)
- [Karnopp — Computer simulation of stick-slip friction in mechanical dynamic systems (1985)](https://www.semanticscholar.org/paper/Computer-simulation-of-stick-slip-friction-in-Karnopp/1a90361eeb41229358624c834e3d9653135b1ae9)
- [LuGre PINN identification (arXiv 2504.12441)](https://arxiv.org/html/2504.12441v1)
- [LuGre identification — Friction in Motion Systems (ScienceDirect)](https://www.sciencedirect.com/science/article/pii/S2405896326000935)
- [Tustin's Method (bilinear transform) — overview](https://www.sciencedirect.com/topics/engineering/tustins-method)
- [Mirtich — Fast and Accurate Computation of Polyhedral Mass Properties](http://www.cs.berkeley.edu/~jfc/mirtich/massProps.html)
- [Velocity-dependent restitution coefficient — DEM-Hopkins (osti.gov 1541207)](https://www.osti.gov/servlets/purl/1541207)
- [Coefficient of restitution: Newton's Experimental Law from energy considerations (arXiv 2009.11903)](https://arxiv.org/pdf/2009.11903)
- [Catto — Modeling and Solving Constraints (Box2D GDC 2009)](https://box2d.org/files/ErinCatto_ModelingAndSolvingConstraints_GDC2009.pdf)
- [Coefficient of restitution — Wikipedia](https://en.wikipedia.org/wiki/Coefficient_of_restitution)
- [Friction Models and Friction Compensation (Åström lecture notes)](https://www.control.lth.se/fileadmin/control/Education/DoctorateProgram/PhysicalModeling/Lectures/L6-FrictionModelseight.pdf)

### Performance & determinism

- [Box2D Determinism — Catto 2024 (combine-mode formulas)](https://box2d.org/posts/2024/08/determinism/)
- [Rapier `enhanced-determinism` docs](https://rapier.rs/docs/user_guides/rust/determinism/)
- [Sleef library (deterministic SIMD math)](https://sleef.org/)

---

**This file is the source of truth for the *why* behind eylem
materials.** Decisions locked here flow into ADR-0069 (architecture)
and the Phase 3.1 v1a-material slice plan. The struct shape, enums,
and combine-mode contract gate v1a interface freeze.
