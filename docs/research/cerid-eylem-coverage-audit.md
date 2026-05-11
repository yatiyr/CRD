# Cerid Eylem — Multi-Domain Coverage Audit

> **Purpose.** Identify the BLANK PARTS of `crd-eylem` against the
> "beyond industry standard, multi-domain" mandate. The five domains
> the engine is contracted to serve simultaneously — **games**,
> **scientific computing**, **robotics**, **aerospace**, **cinematic /
> animation / film** — pull on different parts of the substrate, and a
> gap in any one domain breaks the multi-domain claim.
>
> **Scope.** This is an audit, not a re-litigation. Where ADRs 0062
> (architecture), 0063 (determinism contract), 0066 (`crd-draw`),
> 0067 (force fields), and the just-approved ADR-0068 (body types +
> filtering + callbacks per `cerid-eylem-collision-filtering.md` §10)
> have already locked an answer, we mark it `Covered` and move on.
> Where the answer is sketched only in a research dossier, we mark
> `Sketched`. Where Phase 3.1's plan slots a slice but no ADR exists,
> we mark `Planned`. Where the mandate implies a capability the engine
> needs but no ADR / dossier / phase slice mentions, we mark **`GAP`**
> and pay attention.
>
> **Audience.** Architects committing the next ADRs, anyone judging
> whether Phase 3.1 closes the multi-domain claim or only the
> games-engine-equivalent claim.
>
> **Scope note.** This audit covers the five mandate domains the task
> enumerated: games, scientific computing, robotics, aerospace,
> cinematic / animation / film. Cerid's broader CLAUDE.md mandate also
> names DAW and medical simulation; medical maps roughly to scientific
> computing's coverage here, and DAW physics belongs in a future
> audio-physics dossier — both are out of scope for this document.
>
> **Date:** 2026-05-11. **Companion to** `cerid-eylem.md`,
> `cerid-eylem-fields.md`, `cerid-eylem-collision-filtering.md`.

---

## 1. Coverage matrix

Rows = capability area. Columns:
- **Status** — `Covered` (locked in ADR), `Sketched` (in dossier; not
  ADR-locked), `Planned` (slice exists but no ADR), `Implicit`
  (assumed by other systems but never written down), **`GAP`** (no
  locked decision; the multi-domain mandate needs one), `OOS`
  (legitimately out of scope).
- **Source** — ADR / dossier / phase slice reference for closed rows;
  the consequence for open ones.
- **Domains affected** — G (games), S (scientific), R (robotics),
  A (aerospace), C (cinematic / animation / film).
- **Severity** — for gaps only. Critical / Important / Nice-to-have.
- **Resolution** — for gaps only.

### 1.1 Body / shape / collision substrate (mostly closed)

| Area | Status | Source |
|---|---|---|
| Body motion types (Static/Kinematic/Dynamic) | Covered | ADR-0068 §10.1 |
| Kinematic implicit-velocity inference (Maya animated rigid body / MuJoCo mocap) | Covered | ADR-0068 §10.1 |
| Five-category collider model (Sphere/Box/Capsule/ConvexHull/Plane + TriangleMesh + Heightfield + Sdf) | Covered | ADR-0062 §4.5 |
| Per-collider sensor flag | Covered | ADR-0068 §10.1 |
| 5-tier filtering (mask + group + excluded pairs + ECS predicate + articulation auto) | Covered | ADR-0068 §10.4 |
| Deferred ECS event-stream callbacks (Begin/End first-class, Stay opt-in) | Covered | ADR-0068 §10.5 |
| ContactModify pure-function hook (v1g+) | Covered | ADR-0068 §10.6 |
| Articulation self-collision per-link allowlist | Covered | ADR-0068 §10.7 |
| Force-field substrate (3 tiers, 9 formulas, collider-shape volumes) | Covered | ADR-0067 |
| AABB-tree broadphase (Catto GDC 2019); GPU LBVH (v8) | Covered | ADR-0062 §3 v1c table |
| GJK + EPA + SAT-fast-path narrow phase | Covered | ADR-0062 §3 v1d table |
| Sequential Impulses solver (warm-started, persistent contact cache) | Covered | ADR-0062 §3 v1e table |
| Snapshot/replay (CRDR `'EYLM'`; bit-exact across MSVC/clang/gcc × x64/ARM) | Covered | ADR-0063 §5–7; v1j |
| ECS integration (RigidBody/Collider/Joint components; PrePhysics/Physics/PostPhysics phases) | Covered | ADR-0062 §6 |
| Debug viz substrate (`crd-draw` shared with renderer/sdf/audio/editor) | Covered | ADR-0066 |

### 1.2 Materials substrate — **largely a GAP**

| Area | Status | Source / consequence |
|---|---|---|
| Material struct in v1a interface | Planned | phase-3.1-eylem.md v1a slice mentions `Material` |
| Coulomb friction (constant μ) | Implicit | every games engine ships this; v1e SI solver assumes it |
| **Stribeck friction** (velocity-dependent low-speed dip) | **GAP** | Required for vehicles (tire stick-slip), robotics manipulation, industrial sim |
| **LuGre friction** (state-variable; pre-sliding, frictional lag, stiction) | **GAP** | Standard robotics-manipulation model ([Canudas-de-Wit 1995](https://hal.science/hal-00394988/document)); MuJoCo and PyBullet ship simplified models, recent research extends LuGre via PINNs ([arXiv 2504.12441](https://arxiv.org/html/2504.12441v1)) |
| **Karnopp friction** (piecewise stick-slip approximation; vehicle dynamics) | **GAP** | The numerical-stability fallback when Stribeck integration is too stiff for vehicle ODE solvers; standard in tire dynamics literature |
| **Anisotropic friction** (different μ per direction; tires, conveyors, ice skates) | **GAP** | PhysX `PxMaterial::friction_anisotropy` ships this; vehicles substrate (v5) implicitly needs it |
| **Surface velocity** (conveyor belts; rolling tires) | **GAP** | PhysX `PxMaterial::surface_velocity`; ContactModify covers it but as an after-the-fact hook, not a designer-authored material property |
| **Restitution models — constant** | Implicit | every engine ships |
| **Newton restitution** (velocity-dependent coefficient) | **GAP** | Real bouncing bodies have velocity-dependent restitution; constant is a games-engine compromise |
| **Hunt-Crossley compliant restitution** (force-displacement constitutive law) | **GAP** | Standard for compliant-contact robotics; Drake ships it as part of hydroelastic ([Drake compliant contact](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)) |
| Combine modes (Average / Min / Max / Multiply) | Implicit | Standard across PhysX/Bullet/Jolt; needs to be in the locked Material struct |
| Density vs explicit mass authoring | Implicit | Inertia tensor computation from collider + density is universal; needs API lock |
| Per-collider material override (compound bodies with different friction per face) | **GAP** | Standard authoring need; the v1a `Material` interface scope is undefined |
| Material composition (compound materials, wear, temperature-dependence) | OOS for v1 | Defer to Phase 8 domain modules |
| Damage / fracture material parameters (cinematic destruction) | **GAP** | Driven by post-v1 GeometryCollectionComponent; needs Material schema room reserved |

**Single biggest blank in §1.** A new ADR is needed (proposed
**ADR-0069 — Eylem materials substrate**) before v1a closes, because
the public `Material` struct shape gates downstream interfaces
(vehicles v5 needs anisotropic friction at the API surface; robotics
needs LuGre; cinematic needs Hunt-Crossley for soft-deforming actor
falls). Detail in §3.1.

### 1.3 Solver catalog (partially planned; selection guidance gap)

| Solver | Status | Source |
|---|---|---|
| Sequential Impulses (warm-started, persistent cache) | Covered | ADR-0062 v1e — primary v1 solver |
| TGS (substep wrapper around SI; Macklin 2019) | Sketched | ADR-0062 §3 table notes "v2 adds TGS"; no ADR |
| XPBD-rigid (Macklin 2020 unification) | Sketched | ADR-0062 §3 mentions "v9 may unify rigid into XPBD" |
| XPBD-substep for soft / cloth / rope | Planned | v3a–e slice plan |
| PGS / NPGS | Implicit alternative | Bullet's traditional solver; no Cerid posture |
| **Nonsmooth Newton (Drake-class — best contact accuracy for manipulation)** | **GAP** | Drake's recommended solver for manipulation ([TRI Rethinking Contact Simulation](https://medium.com/toyotaresearch/rethinking-contact-simulation-for-robot-manipulation-434a56b5ec88)); MuJoCo's primal-dual PGS is the closest sibling. No mention in any Cerid ADR. Robotics RL wants this for contact-rich manipulation policies |
| Featherstone reduced-coord ABA | Planned | v6c slice |
| **"When to use which" selection guidance per domain** | **GAP** | Phase 3.1 plan ships solvers without a doc telling downstream domain authors which to pick |

ADR-0069's sibling (proposed **ADR-0070 — Eylem solver catalog and
selection contract**) closes the picker gap and locks the per-solver
opt-in API surface. Detail in §3.2.

### 1.4 Constraint types beyond joints

| Area | Status | Source |
|---|---|---|
| Revolute / spherical / fixed / prismatic joints | Planned | v1f |
| Cone-twist / hinge / ball-socket / slider with limits + breakable | Planned | v4a |
| Distance / rope (XPBD) | Planned | v3c |
| Spring-damper (suspension) | Planned | v5a (vehicles) |
| Per-axis position/rotation limits | Planned | v1f / v4a |
| Conveyor (kinematic surface velocity) | Covered partially | Surface velocity is the materials gap (§1.2) + ContactModify (ADR-0068 §10.6) |
| **Path constraints** (motion along authored curve — animation rigs, rollercoasters, vehicle physics) | **GAP** | PhysX `PxD6Joint` + PhysX articulations support drive-along-path; Houdini POP `Path` constraint; no Cerid plan |
| **Pulley / gear ratio constraints** | Sketched | v5b mentions "differential" — pulley/gear is the same constraint family; no explicit ADR |

Path constraints likely worth a one-line v4 addendum, not a new ADR.

### 1.5 Robotics-specific (significant gaps)

| Area | Status | Source / consequence |
|---|---|---|
| Featherstone reduced-coord articulation | Planned | v6c |
| Joint force/torque sensor at joint frames | Covered | ADR-0068 §10.7 (`JointForceSensorComponent`) |
| Articulation self-collision per-link-pair allowlist | Covered | ADR-0068 §10.7 |
| **URDF importer** | **GAP** | Standard ROS robot description; MuJoCo/Drake/IsaacSim all ship importers ([Isaac Sim URDF importer](https://docs.isaacsim.omniverse.nvidia.com/6.0.0/importer_exporter/ext_isaacsim_asset_importer_urdf.html), [Drake parsing](https://drake.mit.edu/doxygen_cxx/group__multibody__parsing.html)). Without it, "robotics domain" is theoretical |
| **SDF (SDFormat) importer** | **GAP** | Gazebo's native format; Drake supports it; ROS 2 ecosystem standard |
| **MJCF importer** | **GAP** | MuJoCo's native format; IsaacSim ships MJCF importer ([Isaac Sim MJCF importer](https://docs.isaacsim.omniverse.nvidia.com/5.0.0/importer_exporter/ext_isaacsim_asset_importer_mjcf.html)); MJCF is the de facto RL-research robot description format. Beyond URDF in physics fidelity ([source-robotics blog](https://source-robotics.com/blogs/blog/robot-simulation-files-urdf-vs-mjcf-vs-usd)) |
| **ROS 2 integration / DDS bridge** | OOS | Domain-layer concern (Phase 8 robotics module); document explicitly |
| **Inverse kinematics — analytical (closed-form per topology)** | Sketched | v4c references FABRIK for ragdoll-pose recovery; production robotics IK (KDL, TRAC-IK, MoveIt) is more |
| **Inverse kinematics — numerical (Jacobian damped least squares; selectively damped least squares)** | **GAP** | Standard robotics need; pose-targeting in the editor consumes it |
| **Forward dynamics control** (torque control vs position control; RL-friendly API) | **GAP** | Articulation drives in PhysX/Drake/MuJoCo expose torque/velocity/position modes; no Cerid plan |
| **Motor models** (servos, BLDC, stepper, hydraulic; force/torque limits, current limits, gear backlash, cable elasticity) | **GAP** | Critical for industrial robotics + factory simulation; AGX Dynamics ships these natively ([AGX Dynamics](https://www.algoryx.se/agx-dynamics/)) |
| **IMU sensor** (accel + gyro + magnetometer; noise / bias / drift) | **GAP** | IsaacSim IMU is a first-class sensor type; STMicro ships an Isaac extension for their MEMS IMUs |
| Force/torque sensor at joint frames | Covered | ADR-0068 §10.7 |
| **Tactile sensor (contact-area integrators)** | Sketched | ADR-0068 §10.7 defers to v7+ when FEM ships; no API stub |
| **LIDAR sensor** (raycast cones + scan patterns) | **GAP** | IsaacSim ships PhysX SDK Lidar + RTX Lidar; certified configs from HESAI/Ouster |
| **Ultrasonic / IR / range proximity sensors** | **GAP** | Same shape as LIDAR but cheaper rays; standard robotics need |
| **Camera / depth sensor** (renderer integration) | OOS for eylem; renderer concern | Document the contract; physics raycast is the eylem half |
| Soft contact models (MuJoCo's foundation for manipulation) | Sketched | v7d "hydroelastic-style smooth contact"; no ADR |
| **Hydraulic / pneumatic actuators** | **GAP** | Industrial / heavy machinery; Algoryx AGX + Vortex Studio ship these |

Two new ADRs proposed for this row cluster:
- **ADR-0071 — Robotics importers (URDF / SDF / MJCF) and motor /
  actuator catalogue.** Big enough to deserve its own dossier first.
- **ADR-0072 — Eylem sensor substrate (IMU / LIDAR / proximity /
  tactile / contact-area / camera-bridge).** Companion to ADR-0068's
  callback substrate.

Detail in §3.3 + §3.4.

### 1.6 Aerospace-specific (the largest single domain gap)

| Area | Status | Source / consequence |
|---|---|---|
| **Variable-mass bodies** (rocket Tsiolkovsky `m·dv/dt = -ṁ·v_e − F_drag + F_thrust`) | Sketched only | `cerid-eylem-collision-filtering.md` §2.7 dismisses in 4 sentences ("update mass at substep boundary; not a fourth body type"). No ADR; no slice; no API for `set_body_mass()` mid-step |
| **Aerodynamic force model** (lift/drag with attack-angle curves; induced drag; viscous coefficients) | **GAP** | Force fields cover global wind (Directional formula); per-body aero forces (CL/CD curves vs angle of attack) are a different shape |
| **Atmospheric model — US Standard Atmosphere 1976** | **GAP** | Foundation for re-entry, missile sim, satellite drag ([NASA NTRS 1976](https://ntrs.nasa.gov/citations/19770009539), [PDAS atmos](https://www.pdas.com/atmos.html)) |
| Atmospheric model — exponential (cheap fallback) | **GAP** | Standard quick-and-dirty for low-fidelity work |
| **Propulsion model** (thrust vector, gimbal angle, throttle, specific impulse) | **GAP** | First-class need for any rocket / missile / aircraft sim |
| **Multi-body separation events** (rocket stages, satellite deploy, fairing drop) | Sketched | Could be expressed via öbek + per-step API; no canonical pattern |
| **Reaction wheels / control moment gyros** (angular momentum exchange) | **GAP** | Standard satellite ADCS primitive |
| **Gravity model — point-mass** | Implicit | Force fields cover; no aerospace-specific tuning |
| **Gravity model — J2 oblateness** (Earth flattening; standard for LEO sat sim) | **GAP** | Not a force-field formula; needs custom evaluator. NASA GMAT / ESA Orekit / STK all ship ([NAIF SPICE](https://naif.jpl.nasa.gov/naif/spiceconcept.html), [Cnes/JPL DE421](https://help.agi.com/stk/12.7.0/LinkedDocuments/SPICE-BasedJPLDE421PlanetaryEphemerides.pdf)) |
| N-body gravity (Barnes-Hut) | Sketched | `cerid-eylem-fields.md` §3 mentions; no eylem hook |
| **Orbital mechanics primitives** (Kepler orbits, two-body propagator, Hohmann transfer) | OOS | Domain-layer (Phase 8 aerospace module). Document explicitly |
| **Re-entry heating** (FEM thermal coupling) | OOS | v7+ FEM territory; out of scope for Phase 3.1 |
| **Hypersonic CCD** | Planned | v6a/v6b cover CCD; "hypersonic" is just a velocity regime — no special API needed |
| **Satellite swarms / deterministic orbit prop** | Implicit | Snapshot-replay (v1j) covers the determinism half; orbit propagation is the gap |

Aerospace is the single weakest mandate domain. **ADR-0073 — Eylem
aerospace substrate (variable mass + aerodynamic forces + atmospheric
models + propulsion + J2 gravity + multi-body separation events)** is
the right packaging — much of it composes onto the existing force-field
substrate (variable-mass = body-state mutation API; J2 = Gradient
formula evaluator; aero = per-body Drag formula extension), but the
package needs a dossier and an ADR to not become bolt-on later. Detail
in §3.5.

### 1.7 Cinematic / animation-specific

| Area | Status | Source / consequence |
|---|---|---|
| Animation curves driving kinematic body poses | Implicit | Kinematic-with-implicit-velocity (ADR-0068 §10.1) is the primitive; no ADR locks the animation-curve consumer pattern |
| **Pose blending** (animation pose ↔ ragdoll pose interpolation; hit-react blends) | **GAP** | UE `PhysicalAnimationComponent`, Unity Active Ragdoll patterns, IK rigs; first-class need for any character-driven game / film |
| **Pre-roll simulation** (settle scene N steps before "take" starts) | **GAP** | Maya nDynamics ships this as Cached Playback's Pre-roll ([Maya Nucleus simulation](https://download.autodesk.com/us/maya/2011help/files/nCloth_nodes_nucleus.htm)); cinematic + medical workflows assume it |
| **Per-shot physics override** (per-scene gravity scalar, time-scale, integrator iterations) | **GAP** | `PhysicsConfig` is global per ADR-0062 §6; per-scene/per-take override is the cinematic norm |
| **Slow-motion sub-stepping** (200 Hz physics for 24 fps slow-mo render) | Sketched | Fixed-step + interpolation (ADR-0063 §1) covers the math; no API for "this scene runs physics at 4× rate" |
| **Mass-scale destruction** (geometry collections — Chaos Destruction-class) | Sketched | ADR-0068 §10.8 names `GeometryCollectionComponent`, deferred post-v1 |
| **Motion-blur sub-frame contact** (sample contact mid-substep for blur correctness) | Sketched | ADR-0068 §10.8 explicitly defers; cinematic studios must ask for it |
| **Time scrubbing in editor** (snapshot/replay UI; cached playback) | Planned | v1j snapshot/replay is the substrate; UI is editor (Phase 7) — but the cache-management contract is unwritten |
| **Per-take / per-shot recording for film** | Sketched | Snapshot-replay covers the data; no shot-level API |
| **Vellum-class constraint sets on point clouds** (cloth + grain + hair + soft + glue + stitch + pin + attach) | Sketched | XPBD substrate (v3) overlaps with Vellum; no API mapping doc ([Vellum constraints](https://www.sidefx.com/docs/houdini/nodes/sop/vellumconstraints.html)) |
| **Animation-physics import bridge** (FBX / Alembic curves → physics bodies) | **GAP** | Animation pipeline integration |

Two cinematic ADRs proposed:
- **ADR-0074 — Cinematic / animation-physics bridge** (animation curves
  → kinematic poses; pose blending; pre-roll; per-shot override). Likely
  cleaner as a separate module `crd-eylem-cine` than as a v1
  modification.
- **Defer** geometry collections (mass destruction) and motion-blur
  sub-frame contact to a Phase 4.1 mini-phase post-v1, with
  `GeometryCollectionComponent` reserved in v1l API freeze. Same
  treatment ADR-0068 §10.8 already implies; promote to ADR.

Detail in §3.6.

### 1.8 Sensors substrate beyond contacts

| Area | Status |
|---|---|
| Contact event Begin/End/Persist | Covered (ADR-0068 §10.5) |
| Trigger Enter/Exit/Stay | Covered (ADR-0068 §10.5) |
| Sleep/wake events | **GAP** — gameplay listens to these constantly |
| **Velocity-threshold events** (body crosses speed limit; vehicles, projectiles) | **GAP** |
| **Energy-threshold events** (body kinetic energy crosses threshold; destruction trigger) | **GAP** |
| **Force-threshold events** (contact force exceeds threshold; break event) | **GAP** |
| **Position-threshold events** (body crosses authored plane; gameplay zones) | Implicit | Sensor + plane collider covers most; some workloads need the plane to be a real authored asset |
| **Solver-iteration warnings** (convergence failure for tuning) | **GAP** |
| **Joint limit events** (rotation hit stop; suspension bottom-out; ragdoll constraint break) | **GAP** |

Folded into ADR-0072 (sensor substrate) — these are predicate-driven
event emitters that compose with the existing event-stream model.

### 1.9 Cooker / importer catalog

| Asset | Status |
|---|---|
| `.collider.toml` (primitives + hull + plane) | Planned (v1k handler) |
| `.mesh-collider.toml` (TriangleMesh) | Planned (v1d-mesh) |
| `.heightfield.toml` (with R16 PNG ingest) | Planned (v1d-hf) |
| `.field.crdr` (vector grid for `GridSample` formula) | Planned (v1f-fields-f) |
| **`.physics-material.toml`** (material library: friction/restitution/density per name) | **GAP** — paired with materials ADR §1.2 |
| **`.ragdoll.toml`** (humanoid skeleton ragdoll template; per-bone collider + joint limits) | Sketched | v4b mentions "Ragdoll preset cooker"; needs schema lock |
| **`.vehicle.toml`** | Planned (v5b implies it; no schema yet) |
| **`.force-field.toml`** | Implicit | öbek serialization (ADR-0067 §11) covers; no separate cooker file |
| **`.urdf` / `.sdf` / `.mjcf` importers** | **GAP** — paired with robotics ADR-0071 §1.5 |
| **`.scene.toml` whole physics scene** | Implicit | öbek + scene serialization already cover (ADR-0055 / ADR-0058) |
| **glTF KHR_physics_rigid_bodies extension** | **GAP** | Khronos draft proposal in 2026; will become standard; mesh cooker is the natural integration point ([KHR_physics_rigid_bodies PR](https://github.com/KhronosGroup/glTF/pull/2424), [glTF Physics repo](https://github.com/eoineoineoin/glTF_Physics)) |
| **`.alembic` / `.fbx` animation-curves-to-kinematic** | **GAP** — paired with cinematic ADR §1.7 |

Most of these collapse into the cooker work that ADR-0040 already
established — the gap is at the schema level (need ADR locks for
`.physics-material.toml` and `.ragdoll.toml` shape) and at the importer
level for robotics + glTF physics + animation pipelines.

### 1.10 Integration hooks with other Cerid systems

| Hook | Status |
|---|---|
| Physics → debug viz | Covered (ADR-0066, `crd-eylem-viz` companion module) |
| Physics → contact event → audio (impact sound) | **GAP** — Phase 3.4 audio not yet shipped, but the contract needs to exist |
| Physics → contact event → particles (sparks/dust) | **GAP** — Niagara-style, no ADR |
| Physics raycast → AI navigation (line-of-sight; navmesh from collision geometry) | OOS (AI is Phase 4+); document the contract |
| Physics → VFX (destruction → decals/particles) | **GAP** — same as above |
| Physics → UI (rare; physics-driven UI elements) | OOS |
| Editor — time scrubbing, constraint visualization, solver-convergence viewer | Planned partially | `crd-draw` (v1a-draw-d4) covers debug viz hooks; editor UI is Phase 7 |
| Renderer — visual-physics sync, motion blur sub-frame | Sketched | Cinematic gap (§1.7) |
| Scripting (Phase 4) — physics events from script, queries | Sketched | ADR-0067 v1f-fields-g blocks on Phase 4 scripting |

The audio + VFX integration contracts are real gaps; they should land
as **stub IPs (interface promises)** in v1l API freeze even though the
consumers ship later. The pattern matches the
`ScriptComponentHandle` reservation in `ForceFieldComponent`
(ADR-0067 §9).

### 1.11 Testing rigor

| Test | Status |
|---|---|
| Replay-hash (1-second + 10-second canned scenes) | Covered (ADR-0063 §5; v1j) |
| 9-config CI replay matrix (MSVC/clang/gcc × x64/ARM × Win/Lin) | Planned (v9b) |
| Per-slice replay test (small canned scene per slice) | Covered (Phase 3.1 plan DoD #9) |
| Bench suites for fields, filtering, callbacks (CI assertions) | Planned (`bench_fields.cpp`, `bench_filter.cpp`) |
| **Property-based / invariant tests** (energy conservation under no external forces; momentum conservation; angular momentum) | **GAP** — standard scientific-computing rigor |
| **Cross-engine comparison benchmarks vs Box2D / Bullet / PhysX (apples-to-apples)** | **GAP** — required to substantiate "beyond industry standard" |
| **Long-duration drift tests** (60-minute sim, snapshot every 10s, drift ratchet check) | **GAP** |
| **Stress tests** (1k stack stability; 100k particles; 30-link humanoid; 1k bodies in cell) | Sketched | Phase 3.1 plan mentions "100-rigid-body stress" for v1k sandbox; not formal |
| **Numerical-accuracy tests** (compare known closed-form solutions: pendulum period, projectile range, etc.) | **GAP** — required for scientific-computing domain credibility |

The testing rigor gap is the single most important one for the
**scientific computing** domain claim. Locked down via **ADR-0075 —
Eylem testing rigor and conservation-law CI** detail in §3.7.

### 1.12 Performance-tuning cookbook

| Knob | Status |
|---|---|
| Iteration count (vel / pos) | Configurable (`PhysicsConfig`); no per-domain guidance |
| Layer setup recommendations | Mentioned in collision dossier §10; no cookbook |
| Sleeping thresholds | Configurable; no per-domain guidance |
| Substepping policy | Mentioned in cinematic context only |
| Contact cache sizing | Implicit |
| Memory pool sizing (BodyPool/ColliderPool capacities) | Per ADR-0062 v1b allocator strategy doc; partial |
| **Per-domain tuning starting points** (games vs robotics vs cinematic vs scientific vs aerospace defaults) | **GAP** — this is the missing handbook |

Resolution: a `docs/systems/eylem-tuning.md` deep-dive ships at v1l
close, providing **canonical PhysicsProfiles per domain** —
`PhysicsProfile::Games`, `::Robotics`, `::Cinematic`, `::Scientific`,
`::Aerospace`. Composes with ADR-0060 `crd-profile`. Detail in §3.8.

### 1.13 Editor / runtime tools (Phase 7 territory but contracts now)

| Tool | Status |
|---|---|
| Time scrubbing UI | Planned partially (snapshot-replay is the data; UI is Phase 7) |
| Constraint visualization (joint frames, limits, contact forces) | Covered (ADR-0066 / `crd-draw` + ADR-0068 visualizer hooks) |
| Per-body inspector (state, sleep status, applied forces) | Implicit | Editor / sandbox concern |
| Penetration depth visualization | Covered | `crd-draw` consumer |
| Solver convergence viewer (per-iter residual norms) | **GAP** — needs solver-internal hook to publish |
| Profiler (per-system timing) | Implicit | `crd-jobs` already provides; physics-specific dashboard is Phase 7 |
| Scene comparison (snapshot diff) | **GAP** — debugging and bug-repro tool; small but high leverage |

Most defer to Phase 7 editor; the solver-convergence-viewer hook
needs to be locked in v1l API freeze (a callback the solver invokes
each iteration) so the editor can light up later without a v1
re-freeze.

### 1.14 Domain-layer concerns (where physics ends)

| Topic | Status / placement |
|---|---|
| Orbital mechanics (Kepler) | OOS — Phase 8 aerospace domain module |
| Crowd simulation (boids, social forces) | OOS — Phase 4+ AI module; force-field substrate composes |
| Granular media (sand, gravel) | Planned (v8d MPM); covered by GPU substrate |
| Fluid dynamics (SPH/PIC; Stam fluids) | Planned partially (v8d MPM); high-fidelity fluid is Phase 5+ separate substrate |
| Rope / chain dynamics | Planned (v3c XPBD rope) |
| Procedural animation rigs (IK, look-at) | Hybrid — Phase 4 animation module + IK gap (§1.5) |

These are intentionally OOS for eylem proper; document them as
"composition consumers" in ADR-0062's "what eylem is not" section.

---

## 2. Domain-by-domain coverage assessment

Each domain answered as: **what works**, **what fails**, **what elite
domain engines ship that we don't plan**, **recommended fill**.

### 2.1 Games

**What works after Phase 3.1 v1l (locked plan):** stacking + ragdoll +
character controller + raycast pick + 100-rigid-body stress + cooked
öbek with collider components, all deterministic, debuggable, ECS-native,
fiber-parallel. v2 (rigid 2D), v3 (XPBD soft/cloth/rope), v4 (max-coord
articulations), v5 (vehicles), v6 (CCD + reduced-coord), v8 (GPU LBVH +
GPU XPBD), and v9 (replay-hash CI hardening) close the games-engine
gap completely.

**What fails:** mass-scale destruction (`GeometryCollectionComponent`)
ships post-v1; no path constraints; **no per-domain tuning cookbook
means engineers re-derive iteration counts / sleep thresholds per
project**.

**Beyond-industry-standard claim is solid for games** as long as the
testing rigor (§1.11) and tuning cookbook (§1.12) gaps close.
Determinism (ADR-0063) puts Cerid above Unity/Unreal/Godot for any
games workload that wants rollback netcode (Photon Quantum class), and
Cerid's debuggability via shared `crd-draw` substrate (ADR-0066) is
better than any shipped engine — none of PhysX/Bullet/Jolt/Box2D ship
a substrate-shared debug renderer that the entire engine reuses.

**Recommended fill:** none specific to games. The cross-cutting fills
(materials §1.2, testing §1.11, cookbook §1.12) close it.

### 2.2 Scientific computing

**What works:** snapshot-replay determinism (ADR-0063); reproducible
RL training environments via fixed seeds + content-addressed öbek hashes
(ADR-0058); deterministic stdlib substitutions for cross-platform
bit-exact replay; numerical foundations sketched for `crd-hesap`
substrate (ADR-0065).

**What fails:** **conservation-law tests are not in the test plan.** A
"scientific computing physics engine" without energy/momentum
conservation tests is not a credible scientific tool. Drift over
60-minute sim runs is a standard quality metric. Closed-form regression
tests (pendulum period, projectile range, two-body orbit) are absent.
Comparison benchmarks vs MuJoCo / Drake / Project Chrono on canonical
test scenes do not exist.

**Elite engines ship:**

- **Drake** — closed-form analytical regressions for every multibody
  system; `MultibodyPlant` ships with a documented hierarchy of
  validity tests ([Drake compliant contact](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)).
- **MuJoCo** — published benchmark suite (DM Control); reproducible
  metrics across MuJoCo versions.
- **Project Chrono** — large open testbed of academic benchmarks.

**Recommended fill:** ADR-0075 (testing rigor §3.7) — conservation-law
CI + closed-form regression suite + comparison benchmark sweep against
Box2D/Bullet (apples-to-apples on canonical 10-box stack, swept-rotor
pendulum, cradle).

### 2.3 Robotics

**What works:** Featherstone reduced-coord articulations (v6c);
deterministic replay for reproducible RL training (ADR-0063);
articulation self-collision per-link allowlist (ADR-0068 §10.7); joint
force/torque sensors (ADR-0068 §10.7); snapshot-replay enabling
hardware-in-the-loop deterministic verification.

**What fails:** This is the second-largest domain gap after aerospace.

- **No URDF / SDF / MJCF importers.** Standard ROS / Drake / MuJoCo /
  IsaacSim entry path. Without importers, importing a robot model
  means hand-translating XML to TOML — every robotics project's first
  3 days are gone. **Critical.**
- **No motor / actuator catalogue.** Industrial robotics engines
  (AGX Dynamics, Vortex Studio) ship hydraulic / pneumatic /
  servo / BLDC / stepper actuator models with realistic torque-current
  curves. PhysX articulation drives expose torque/velocity/position
  modes; Cerid has no plan. **Critical for industrial; important for
  research.**
- **No robotics sensor catalogue.** IsaacSim ships IMU + LIDAR (incl.
  certified manufacturer configs from HESAI / Ouster) +
  contact + tactile + camera as first-class types ([Isaac Sim Sensors](https://docs.isaacsim.omniverse.nvidia.com/5.0.0/sensors/index.html)).
  Cerid has nothing planned beyond the joint-force-sensor stub.
  **Critical for any robotics RL workload.**
- **No friction model beyond Coulomb (assumed).** LuGre is the
  manipulation gold standard. No current Cerid plan. **Important.**
- **No FDC / RL-friendly torque-control API.** Drives are sketched in
  v6c; no exposure for "set torque, get sensor, repeat." **Critical
  for RL.**

**Elite engines ship:** MuJoCo (XML modeling + soft contact + IMU +
LIDAR), Drake (URDF/SDF + hydroelastic + closed-form regressions +
constraint forces), IsaacSim (URDF/MJCF importers + GPU-parallel sim
+ certified sensor catalogue + IMU/LIDAR/tactile/contact), Project
Chrono (granular + cable + heavy machinery), AGX Dynamics (industrial
actuators + granular + cable), Newton (NVIDIA's open-source robotics
engine on Warp, integrating MuJoCo Warp + Vertex Block Descent +
hydroelastic).

**Recommended fill:**

- ADR-0071 (robotics importers + actuator catalogue, §3.3)
- ADR-0072 (sensor substrate, §3.4)
- Materials ADR-0069 (must include LuGre + Hunt-Crossley, §3.1)
- v6c reduced-coord articulation slice extended to expose
  torque-control API; new sub-slice v6c-fdc.

Without these four, the "robotics domain" claim is aspirational. With
them, Cerid matches MuJoCo / Drake / IsaacSim feature-for-feature with
a more disciplined determinism contract.

### 2.4 Aerospace

**What works:** snapshot-replay determinism is a natural fit for
FAA / ESA / NASA scientific-code-review reproducibility requirements;
fixed-step + interpolation contract handles orbital propagation epoch
synchronisation; hypersonic CCD (v6a/b) covers re-entry collision.

**What fails:** This is the **single weakest mandate domain**.

- **Variable-mass bodies** dismissed in 4 sentences in the
  collision-filtering dossier. Rocket sim is impossible without the
  Tsiolkovsky equation. **Critical.**
- **No aerodynamic force model.** Lift/drag with attack-angle curves
  is the foundation of every aircraft / missile / rocket sim. Force
  fields' Drag formula handles linear/quadratic body drag, not lift.
  **Critical.**
- **No atmospheric model.** US Standard Atmosphere 1976 + COESA
  exponential are the workhorses. Cerid has no altitude-dependent ρ /
  T / p evaluator. **Critical.**
- **No propulsion model.** Thrust vector, gimbal angle, throttle,
  specific impulse — first-class need. **Critical.**
- **No reaction wheels / control moment gyros.** Standard satellite
  ADCS primitive. **Important.**
- **No multi-body separation events** as first-class API. Could be
  expressed via öbek + per-step API call, but no canonical pattern.
  **Important.**
- **No J2 oblateness gravity.** Standard for any LEO satellite sim
  ([NAIF SPICE](https://naif.jpl.nasa.gov/naif/spiceconcept.html);
  [GMAT](https://software.nasa.gov/software/GSC-17177-1)). Cerid's
  force fields cover point-mass gravity only. **Important.**

**Elite tools ship:** NASA GMAT (open-source mission analysis;
spacecraft mass tab, propagators with J2/J3/etc., maneuver modeling),
ESA Orekit, AGI STK, JPL SPICE (ephemeris kernels), MATLAB Aerospace
Toolbox (US Std Atm 1976, propagators, IMU/GPS sensor models). None
of these are physics engines per se; they are aerospace-flight-mechanics
substrates. The gap is not "Cerid should be GMAT" — the gap is
**"Cerid should support the same workloads via its physics substrate
+ aerospace-specific extensions."**

**Recommended fill:** ADR-0073 (aerospace substrate; §3.5). Most of it
composes onto existing substrates (variable-mass = `set_body_state` API
extension; J2 = Gradient force-field formula evaluator on a prebuilt
gravity model SDF; aero = per-body force evaluator extending the Drag
formula). No new module; ~1 month of work spread across v1, v3, and
v9 sub-slices.

### 2.5 Cinematic / animation / film

**What works:** kinematic-with-implicit-velocity covers the simple
"animator drives a body" case (ADR-0068 §10.1); snapshot-replay covers
take recording (v1j); `crd-draw` substrate gives editor visualization
hooks (ADR-0066); öbek prefab system supports per-shot scene composition
(ADR-0058); XPBD substrate (v3) covers cloth + rope + soft body for
cinematic deformation; force fields (ADR-0067) cover wind / vortex /
gravity dial.

**What fails:**

- **Pose blending** (animation pose ↔ ragdoll pose interpolation) is
  the core "hit react then ragdoll" workflow. No plan. **Critical for
  characters.**
- **Pre-roll simulation** — Maya nDynamics's first-class feature
  ([Maya Cached Playback](https://damassets.autodesk.net/content/dam/autodesk/www/html/maya-cached-playback/2024/MayaCachedPlaybackWhitePaper.html)).
  Cinematic + medical workflows assume it. No Cerid plan. **Important.**
- **Per-shot physics override** (gravity scalar, time scale, iterations).
  `PhysicsConfig` is global. Cinematic norm is per-take override.
  **Important.**
- **Slow-motion sub-stepping** (4× physics rate for slow-mo render).
  Substrate supports it (fixed-step is configurable); no per-shot API.
- **Mass-scale destruction** (geometry collections). ADR-0068 §10.8
  defers explicitly. Acceptable post-v1. Reserve schema room.
- **Motion-blur sub-frame contact.** ADR-0068 §10.8 defers explicitly.
  Acceptable for now if cinematic studios don't surface a concrete
  need.
- **Animation-physics import bridge** (FBX / Alembic curves →
  kinematic poses). Implicit; no resource loader plan.

**Elite engines ship:** Houdini Vellum (cloth/grain/hair/soft/glue/
stitch/pin/attach as constraint-set composition; pre-roll; cached
playback; per-shot solver settings; FBX/Alembic import); Maya nDynamics
(animator-friendly pre-roll + cached playback + per-take overrides;
nCloth/nParticle/nHair); Chaos Destruction (Geometry Collections; hit
events; field-driven destruction); Niagara physics (GPU particle
collision; field force operators).

**Recommended fill:** ADR-0074 (cinematic / animation-physics bridge;
§3.6). Likely spawns a new module `crd-eylem-cine` that wraps eylem
with the cinematic-specific patterns (pre-roll, per-shot config,
animation-curve consumers, pose blending). ~1.5 months of work mostly
in v1 + v4 + Phase 4.1 destruction mini-phase.

---

## 3. Cross-cutting fill plan

The §1 + §2 audit identified seven cross-cutting gap categories. Each
gets: scope; mandate domains it impacts; recommended action;
estimated complexity (S/M/L = 1–2 weeks / 1 month / 2–3 months).

### 3.1 Materials substrate — **ADR-0069** (M)

**Scope.** Lock the public `Material` struct shape and the friction /
restitution / surface-velocity / density catalogue before v1a closes.
Concretely:

```cpp
struct Material
{
    // Friction
    FrictionModel friction_model;          // Coulomb / Stribeck / LuGre / Karnopp / Anisotropic
    f32           friction_static;
    f32           friction_dynamic;
    Vec3f         friction_anisotropy;     // direction-dependent μ (tires, ice, conveyors)
    f32           stribeck_velocity;       // v_s in Stribeck curve
    LuGreParams   lugre;                   // σ0 σ1 σ2 internal-state model

    // Restitution
    RestitutionModel restitution_model;    // Constant / Newton / HuntCrossley
    f32              restitution_coefficient;
    HuntCrossleyParams hunt_crossley;      // k_compliance + d_dissipation

    // Surface
    Vec3f surface_velocity_local;          // conveyors, rolling tires (frame-local)

    // Combine
    CombineMode friction_combine;          // Average / Min / Max / Multiply / GeometricMean
    CombineMode restitution_combine;

    // Mass / inertia derivation
    f32 density;                           // for auto-inertia from collider geometry
};
```

**Domains.** G + R + A + C — every domain pulls on materials.

**Action.** New ADR + new sub-dossier `cerid-eylem-materials.md`.
Slot: v1a-material sub-slice before v1a interface freeze. Material
cooker handler = `.physics-material.toml` ships in v1k.

**Complexity.** Medium. Most friction models are pure-math
implementations; LuGre's internal state is the only structural
addition (per-contact bristle-deflection state, ~16 bytes).

### 3.2 Solver catalog + selection guidance — **ADR-0070** (S)

**Scope.** Lock the per-solver opt-in API (which solvers a build links)
+ the "when to use which" guidance doc. Cerid solver matrix:

| Solver | Best for | Slice |
|---|---|---|
| Sequential Impulses | Default games + general | v1e |
| TGS | High-mass-ratio + heavy stacking | v2 (post v1) |
| XPBD-rigid | Cables + fast soft contact | v3 / v9 unification |
| **Nonsmooth Newton** | **Robotics manipulation (Drake-class)** | **NEW: v6d sub-slice** |
| Featherstone reduced-coord ABA | Robotics arm fidelity (no joint drift) | v6c |
| XPBD-substep | Cloth / rope / soft | v3 |
| MPM | Granular / snow / sand | v8d |

**Domains.** G + R + S.

**Action.** New ADR codifying the matrix; new sub-slice **v6d
nonsmooth Newton** (was implicit gap; promote to plan). Tuning doc
`docs/systems/eylem-solver-selection.md` ships at v1l.

**Complexity.** Small (mostly documentation; nonsmooth Newton sub-slice
is a separate ~3 weeks but slots cleanly into v6).

### 3.3 Robotics importers + actuator catalogue — **ADR-0071** (L)

**Scope.** URDF / SDF / MJCF importers; motor / actuator catalogue
(servo, BLDC, stepper, hydraulic, pneumatic) with realistic
force/torque limits, current limits, gear backlash, cable elasticity,
friction. URDF imports a robot's articulation tree, collision matrix
(self-collision exclusions per ADR-0068 §10.4 tier 3), inertial
properties, and material assignments. MJCF imports the same plus
MuJoCo's actuator + sensor declarations (we map them to Cerid sensor
substrate ADR-0072). SDF imports Gazebo's nested-model topology.

Companion module `crd-eylem-robotics` (skeleton at v6c); importers ship
as cooker handlers consuming `.urdf` / `.sdf` / `.mjcf` files into
existing öbek prefabs.

**Domains.** R primarily; R-adjacent industrial / cinematic robotics.

**Action.** New ADR + new dossier `cerid-eylem-robotics.md`.
Slot: a 4-slice cluster in v6 between v6b (CCD) and v6c
(reduced-coord), specifically:

- v6c-urdf — URDF parser + cooker handler
- v6c-sdf — SDFormat parser + cooker handler
- v6c-mjcf — MJCF parser + cooker handler (most complex)
- v6c-actuators — motor / actuator catalogue + Drive API

Also blocked downstream: Phase 8 robotics module (consumer).

**Complexity.** Large. URDF alone is a 2-week parser project (XML +
mesh resolution + material cross-reference); MJCF is similar; actuator
catalogue is 2 weeks. Total ~2 months for a full v6c-robotics cluster.

### 3.4 Sensor substrate — **ADR-0072** (M)

**Scope.** First-class sensor types beyond contact + trigger. Mirror
ADR-0067's three-tier model (analytic / authored / scripted) but for
sensor *outputs*:

| Sensor | Tier | API |
|---|---|---|
| **IMU** (accel + gyro + magnetometer) | Tier 1 analytic | `IMUSensorComponent`; reads body state; adds noise/bias from `IMUNoiseProfile` |
| **LIDAR** (raycast cones, scan patterns) | Tier 1 analytic | `LidarSensorComponent`; uses scene-query raycast batch |
| **Ultrasonic / IR proximity** | Tier 1 analytic | `ProximitySensorComponent`; raycast |
| **Force/torque at joint frames** | Already in ADR-0068 §10.7 | `JointForceSensorComponent` |
| **Tactile** (contact-area integrators on FEM-skinned bodies) | Tier 2 deferred | `TactileSensorComponent` blocks on v7 FEM |
| **Velocity / energy / force / position thresholds** | Tier 1 analytic | `ThresholdEventEmitterComponent` |
| **Sleep/wake events** | Tier 1 analytic | First-class `BodySleepEvent` event stream |
| **Joint limit events** | Tier 1 analytic | `JointLimitEvent` event stream |
| **Solver-iteration warnings** | Tier 1 hook | `SolverDiagnosticsCallback` (debug-build only) |
| **Camera / depth** | OOS for eylem | Renderer concern; physics raycast hook documented |

All sensors emit through the existing ECS event-stream model (ADR-0068
§10.5); sort key extended to `(sensor_type, body_id, timestamp)`.

**Domains.** R + A (robotics + aerospace need IMU/LIDAR; G needs
threshold events; S needs solver diagnostics).

**Action.** New ADR + new dossier `cerid-eylem-sensors.md`. Slot: a
3-slice cluster in v6 alongside reduced-coord articulation:

- v6e-sensor-a — IMU + threshold events + sleep/wake/joint-limit
- v6e-sensor-b — LIDAR + proximity (consume v1c broadphase raycast)
- v6e-sensor-c — Solver diagnostics + tactile stub (full impl blocks on v7)

**Complexity.** Medium. IMU is straightforward; LIDAR is bench-pinned
on raycast batch perf; threshold events are predicate-driven event
emitters reusing the contact-event substrate.

### 3.5 Aerospace substrate — **ADR-0073** (M-L)

**Scope.** The "physics engine for spacecraft" extension layer ships
as a sibling module `crd-eylem-aero` (parallel to `crd-eylem-vehicles`
naming pattern from ADR-0062 §1). Lock:

```cpp
// API extensions in eylem core:
void IPhysicsScene::set_body_mass(BodyId id, f32 new_mass) noexcept;
// Triggers inertia tensor refresh from collider + new mass.
// Body sees stair-step mass curve; documented in ADR-0073.

// New components in crd-eylem-aero (companion module):
struct AeroDynamicsComponent {
    // Lift/drag curves vs angle of attack.
    LiftDragCurve cl_curve;       // CL(α) — typically a piecewise polynomial
    LiftDragCurve cd_curve;       // CD(α)
    f32           reference_area; // m²
    Vec3f         body_axis;      // for AoA computation
};

struct PropulsionComponent {
    Vec3f thrust_axis_local;
    f32   thrust_magnitude_max;
    f32   throttle;               // [0,1]
    f32   specific_impulse;       // for mass-flow rate
    Vec3f gimbal_angle;           // for thrust vectoring
};

struct AtmosphericModel {
    enum class Kind { USStd1976, Exponential, Custom };
    Kind kind;
    f32  scale_height_m;          // exponential model
    // For US Std 1976: piecewise table; evaluate ρ/T/p at altitude.
};

// New force-field formula addition (J2 IS field-shaped):
// FieldFormula::J2Gravity   — Earth oblateness gravity (not just point-mass)

// Aerodynamics is NOT a field — it is a per-body evaluator that reads
// the body's velocity, AoA, and an atmospheric model. Ship as a
// separate component family on `crd-eylem-aero` (the AeroDynamicsComponent
// above) plus a per-step `AeroForceEvaluator` system that integrates
// against the AtmosphericModel; do NOT pollute ADR-0067's closed
// FieldFormula enum with a non-field-shaped formula.

// First-class API for staged separation:
ResultId IPhysicsScene::detach_subassembly(EntityId parent, EntityId subassembly) noexcept;
// Severs joints; subassembly bodies become free.
```

**Domains.** A primarily; cross-pollinates with R (drone sim) and S
(reproducible flight-mechanics research).

**Action.** New ADR + new dossier `cerid-eylem-aerospace.md` (this is
where NASA GMAT / ESA Orekit / JPL SPICE / DE421 ephemeris primary
sources go). Slot: 5-slice cluster intercut with v3 (force fields
extension) and v6 (CCD / reduced-coord):

- v1f-fields-j (extends ADR-0067) — `J2Gravity` formula slot reserved
  at v1l close; impl ships post-v1 inside the frozen surface
- v6f-aero-a — `set_body_mass` API + variable-mass test (Tsiolkovsky
  closed-form regression)
- v6f-aero-b — Atmospheric model evaluator (US Std 1976 + Exponential)
- v6f-aero-c — `AeroDynamicsComponent` + `AeroForceEvaluator` system
  + `PropulsionComponent` + sandbox rocket-ascent demo (separate from
  the field substrate per the architectural note above)
- v6f-aero-d — `detach_subassembly` API + multi-stage separation event
  + bench

ADR-0067's `FieldFormula` enum is *closed* and locks at v1l (per §3
of that ADR). The closed-enum discipline gates the deterministic-
formula audit — every closed-enum slot must be field-shaped (volume
× falloff × mass-coupling, evaluated at a sample point against bodies
that overlap the volume). **Only `J2Gravity` is field-shaped**; it is
a gravity formula evaluated at a sample point above a planet, with
mass coupling and a volume (the gravitating body's sphere of
influence). Reserve it in v1l freeze: `enum class FieldFormula :
crd::u8 { ..., Reserved_J2 = 10 };` so post-v1 fills the slot inside
the frozen surface, same blocking-sub-slice pattern as `Gradient` /
`Script`.

**Aerodynamic force is NOT field-shaped** — it is a per-body evaluator
that reads the body's velocity + AoA + atmosphere. Shipping it as a
field would require giving fields access to the body's velocity at
evaluation time, which collides with ADR-0067 §6's "each field reads
the substep-start velocity, parallelizable, deterministic" contract
(or worse, requires the `apply_after` DAG escape hatch by default).
Ship aerodynamics as a separate `AeroDynamicsComponent` + per-step
`AeroForceEvaluator` system in `crd-eylem-aero`. Keep ADR-0067's
closed-enum integrity intact.

**Complexity.** Medium-large. Most pieces are algebra (Tsiolkovsky,
US Std 1976 piecewise table); `detach_subassembly` is the structural
addition.

### 3.6 Cinematic / animation-physics bridge — **ADR-0074** (M)

**Scope.** Companion module `crd-eylem-cine` providing:

- **Animation-curve consumers** — `KinematicAnimatedComponent` reads
  an animation curve handle each frame, computes pose, eylem infers
  velocity (per ADR-0068 §10.1).
- **Pose blending** — `PhysicalAnimationBlendComponent` blends between
  a target animation pose and the simulation pose with a per-bone
  alpha; Maya / UE pattern.
- **Pre-roll simulation** — `IPhysicsScene::pre_roll(steps)` runs N
  physics steps without driving the rest of the engine; deterministic;
  composes with snapshot/replay.
- **Per-shot physics override** — `ShotPhysicsConfig` ECS singleton
  per scene, overriding gravity / time-scale / iterations / fixed-step
  / max-substeps.
- **Slow-motion substepping** — `slow_mo_factor` field on
  `ShotPhysicsConfig` ramps physics rate (e.g., 200 Hz) while
  presentation stays at 24 fps.
- **Animation pipeline import** — FBX + Alembic loader handlers
  resolving curve targets to kinematic body components (cooker work).

**Domains.** C primarily; G secondarily (hit-react + animation-driven
characters).

**Action.** New ADR + new dossier `cerid-eylem-cinematic.md`. Slot:
3-slice cluster in v4 (post-articulation, since pose blending needs
ragdolls):

- v4d-cine-a — pre-roll API + per-shot ShotPhysicsConfig
- v4d-cine-b — `KinematicAnimatedComponent` + animation-curve cooker
  handlers
- v4d-cine-c — pose blending + ragdoll-anim hybrid sandbox demo

Mass-scale destruction (`GeometryCollectionComponent`) and motion-blur
sub-frame contact remain post-v1 / Phase 4.1 deferred per ADR-0068
§10.8.

**Complexity.** Medium. Pre-roll is small; pose blending is the
complex part (per-bone alpha + force/impulse blending into the
solver, careful to preserve determinism).

### 3.7 Testing rigor and conservation-law CI — **ADR-0075** (S)

**Scope.** Codify the test discipline:

1. **Conservation-law tests** — for every solver, a frictionless +
   gravityless scene asserts kinetic + potential energy is conserved
   within `eps_energy` over 60 seconds; angular momentum is conserved
   within `eps_ang_mom`; linear momentum likewise.
2. **Closed-form regression tests** — pendulum period (`T = 2π√(L/g)`),
   projectile range (`R = v² sin(2θ) / g`), two-body Kepler orbit
   (period from Kepler's third law), simple-harmonic oscillator
   amplitude / period.
3. **Long-duration drift tests** — 60-minute sim, snapshot every 10s,
   drift ratchet check (snapshot hash should diverge no faster than
   `O(t / dt)` accumulated rounding error).
4. **Cross-engine comparison benchmark** — canonical scenes (10-box
   stack, Newton's cradle, swept-rotor pendulum, 30-link humanoid)
   simulated in eylem + Box2D + Bullet; report metrics (sleep time,
   stack drift, energy drift, contact count); CI assertion that eylem
   is within 2× of best-of-three.
5. **Property-based tests** — random scenes with random seeds; assert
   universal invariants (no body penetrates ground when initialized
   above; collision count is bounded; etc).
6. **Stress tests** — 1k stack, 100k particles, 30-link humanoid, 1k
   bodies in a single broadphase cell.

**Domains.** S primarily; G + R + A + C all benefit.

**Action.** New ADR + new test infrastructure under
`tests/eylem-rigid3d/conservation/` and `tests/eylem-rigid3d/regression/`.
Slot: split between v1l (sandbox demo + initial regression scenes)
and v9b (full cross-platform CI matrix).

**Complexity.** Small (mostly test code over existing solver). The
cross-engine benchmark is the only nontrivial piece (need to wire up
Box2D + Bullet for comparison; both are MIT-licensed, fits the
"reference implementations inform algorithm choice — not source code"
posture in ADR-0065 §3 used for tests only, not shipping engine).

### 3.8 Performance-tuning cookbook — `docs/systems/eylem-tuning.md` (S)

**Scope.** A handbook documenting:

- **Per-domain `PhysicsProfile` defaults** — `PhysicsProfile::Games`
  (8 vel iter, 3 pos iter, 60 Hz, sleep enabled, energy threshold X),
  `::Robotics` (12 vel iter, 4 pos iter, 240 Hz, sleep disabled),
  `::Cinematic` (16 vel iter, 5 pos iter, 60 Hz with 4× substep
  available, sleep disabled), `::Scientific` (32 vel iter, 8 pos iter,
  240 Hz, sleep disabled, IEEE-strict FP), `::Aerospace` (24 vel iter,
  6 pos iter, 60 Hz with mass-mutation epoch, sleep disabled).
- **Layer setup recommendations** — target broadphase rejection
  ratio 80–95% per PhysX guidance.
- **Sleep-threshold tuning** — when to lower for robotics-grade
  fidelity, when to raise for game perf.
- **Substepping policy** — slow-mo cinematic substep budgets;
  hypersonic CCD substep recommendations.
- **Memory pool sizing** — per-domain BodyPool / ColliderPool /
  contact cache starting points.

**Action.** No ADR (it's a doc); new file
`docs/systems/eylem-tuning.md` ships at v1l close. Composes with
ADR-0060 `crd-profile`: `PhysicsProfile` is a profile asset, dial-able
at runtime.

**Complexity.** Small (one weekend writing).

---

## 4. Recommended fill plan — concrete

### 4.1 New ADRs to mint (8)

| ADR | Title | Slot | Companion dossier |
|---|---|---|---|
| 0069 | Eylem materials substrate | Before v1a freeze | `cerid-eylem-materials.md` |
| 0070 | Eylem solver catalog + selection contract | Before v1l freeze (just doc + new v6d slice) | `cerid-eylem-solvers.md` |
| 0071 | Robotics importers (URDF/SDF/MJCF) + actuator catalogue | New v6 sub-cluster (v6c-urdf, -sdf, -mjcf, -actuators) | `cerid-eylem-robotics.md` |
| 0072 | Eylem sensor substrate (IMU/LIDAR/threshold/diagnostics) | New v6 sub-cluster (v6e-sensor-a, -b, -c) | `cerid-eylem-sensors.md` |
| 0073 | Eylem aerospace substrate (variable mass + aero + atm + propulsion + J2 + separation) | New v6 sub-cluster (v6f-aero-a..d) + v1f-fields-j extension | `cerid-eylem-aerospace.md` |
| 0074 | Cinematic / animation-physics bridge (`crd-eylem-cine`) | New v4 sub-cluster (v4d-cine-a, -b, -c); deferrals to Phase 4.1 | `cerid-eylem-cinematic.md` |
| 0075 | Eylem testing rigor + conservation-law CI | Mostly v1l + v9b | (no separate dossier; ADR self-contained) |

### 4.2 New research dossiers (6)

The first six in §4.1. Pattern: tone + structure + citation style as
`cerid-eylem-fields.md` / `cerid-eylem-collision-filtering.md`. ADR-0075
is small enough to be self-contained.

### 4.3 ADR extensions needed

| Existing ADR | Extension |
|---|---|
| ADR-0062 (architecture) | Reserve `Material` field shape to absorb ADR-0069. Reserve `FieldFormula::Reserved_Aero / Reserved_J2` per §3.5 (or accept post-v1l major-version bump) |
| ADR-0067 (force fields) | Acknowledge §3.5 reservations; document that aero + J2 fill inside the frozen enum. Same blocking-sub-slice pattern as `Gradient`/`Script` |
| ADR-0068 (filters/callbacks) | Add §10.7 stub for sensor substrate cross-reference (ADR-0072) and §10.8 stub for cinematic ADR cross-reference (ADR-0074) |
| ADR-0066 (`crd-draw`) | Reserve solver-convergence-viewer hook category (mentioned but not slotted in §1.13) |
| Phase 3.1 plan | Extend with new sub-slices: v1a-material, v6c-urdf/sdf/mjcf/actuators, v6d-nonsmooth-newton, v6e-sensor-a/b/c, v6f-aero-a/b/c/d, v4d-cine-a/b/c, v1f-fields-j (aero+J2 reservations) |

### 4.4 Deferred (with rationale)

- **Mass-scale destruction (`GeometryCollectionComponent`)** — already
  deferred per ADR-0068 §10.8. Phase 4.1 mini-phase. Reserve schema
  room in v1l now via `GeometryCollectionComponent` empty-shell
  declaration.
- **Motion-blur sub-frame contact** — already deferred per ADR-0068
  §10.8. Cinematic studios drive priority.
- **Differentiable refactor onto `crd-hesap-autodiff`** — already
  documented in phase plan v9a. Blocks on Phase 3.1.6.
- **FEM v7 internal PCG → `crd-hesap-iterative` refactor** — already
  documented in phase plan v7a. Blocks on Phase 3.1.6.
- **GPU substrate (v8a–e)** — gated on `crd-rhi` compute-shader
  pipeline maturity (Phase 3.5+).
- **ROS 2 integration** — out of physics scope; Phase 8 robotics
  module concern.
- **Orbital mechanics primitives** (Kepler propagator, Hohmann
  transfer) — out of physics scope; Phase 8 aerospace module concern.
  ADR-0073 ships the underlying physics; orbital algebra builds atop.

### 4.5 Out of scope (explicit)

- **Full SPH / FLIP / film-grade fluids** — Phase 5+ separate substrate
  (`crd-fluid` likely).
- **Crowd simulation (boids, social forces)** — Phase 4+ AI module.
- **Tearing topology changes (cloth tear, bone fracture topology)** —
  Phase 4.1 destruction mini-phase.
- **Networked physics (rollback, lag-compensated)** — Phase 4.2
  networking, consuming v1j snapshot/replay.
- **Vehicle physics for tracked / aircraft / boats** — v5 ships
  car-class only; explicit per phase plan.
- **Camera / depth sensor implementation** — renderer concern; eylem
  ships only the raycast hooks (covered).

### 4.6 Priority + total fill estimate

The fills are **not equal**. Two are P0 — they block the v1 freeze
and any post-v1 work that depends on the frozen API. Two are P1 —
they block the multi-domain claim being defensible. The rest are P2 —
post-v1 fills that don't block anything but a fully-shipped engine.

| Priority | Fill | Must close before |
|---|---|---|
| **P0** | ADR-0069 materials substrate | **v1a interface freeze** (Material struct shape locks the public API the rest of v1 builds on) |
| **P0** | ADR-0075 testing rigor + conservation-law CI | **v1l close** (without it, the scientific computing domain claim is unsubstantiated; CI infrastructure should land with the snapshot-replay harness, not after) |
| **P1** | ADR-0073 aerospace substrate | The "5 domains" claim being defensible (currently ~zero aerospace coverage) |
| **P1** | ADR-0071 robotics importers + actuators | Phase 8 robotics module + any "robotics domain" demo |
| **P2** | ADR-0072 sensor substrate | Robotics RL workloads and gameplay threshold events |
| **P2** | ADR-0074 cinematic bridge | Cinematic / animation production demos |
| **P2** | ADR-0070 solver catalog + nonsmooth Newton (v6d) | Manipulation-class robotics fidelity |
| **P2** | Tuning cookbook | Documentation / onboarding (no code blocked) |

**Effort estimates:**

- **Materials (ADR-0069)**: M, ~3 weeks, P0 — slots before v1a freeze.
- **Testing rigor (ADR-0075)**: S, ~2 weeks of test code at v1l, P0.
- **Aerospace substrate (ADR-0073)**: M-L, ~1.5 months, P1, v1+v6 clusters.
- **Robotics importers + actuators (ADR-0071)**: L, ~2 months, P1, v6 cluster.
- **Sensor substrate (ADR-0072)**: M, ~1 month, P2, v6 cluster.
- **Cinematic bridge (ADR-0074)**: M, ~1 month, P2, v4 cluster.
- **Solver catalog (ADR-0070)**: S doc, plus new v6d slice ~3 weeks
  for nonsmooth Newton, P2.
- **Tuning cookbook**: S, ~1 weekend, P2.

Total ~6 additional months on top of Phase 3.1's existing ~6–9 month
plan. The materials + testing + cookbook fills are critical and small;
they should ship inside the existing v1 timeline, not after. Robotics
+ aerospace + cinematic + sensors are the substantial post-v1
expansions; without them the multi-domain claim is partial.

**Without these fills**, Phase 3.1 closes a games-engine-equivalent
substrate. **With them**, Cerid genuinely matches MuJoCo + Drake +
PhysX + Bullet + Houdini Vellum + Maya nDynamics + GMAT/Orekit
domain-by-domain — the "beyond industry standard, multi-domain"
mandate becomes substantiated.

---

## 5. References

### Primary engine / tool documentation

**Robotics engines and simulators:**
- [Drake — Compliant Contact](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html),
  [Hydroelastic Contact User Guide](https://drake.mit.edu/doxygen_cxx/group__hydroelastic__user__guide.html),
  [MultibodyPlant](https://drake.mit.edu/doxygen_cxx/classdrake_1_1multibody_1_1_multibody_plant.html),
  [Multibody Parsing (URDF/SDF)](https://drake.mit.edu/doxygen_cxx/group__multibody__parsing.html)
- [MuJoCo Modeling](https://mujoco.readthedocs.io/en/stable/modeling.html),
  [MuJoCo Computation](https://mujoco.readthedocs.io/en/stable/computation/index.html)
- [Isaac Sim — URDF Importer](https://docs.isaacsim.omniverse.nvidia.com/6.0.0/importer_exporter/ext_isaacsim_asset_importer_urdf.html),
  [MJCF Importer](https://docs.isaacsim.omniverse.nvidia.com/5.0.0/importer_exporter/ext_isaacsim_asset_importer_mjcf.html),
  [Sensors](https://docs.isaacsim.omniverse.nvidia.com/5.0.0/sensors/index.html),
  [Contact Sensor](https://docs.isaacsim.omniverse.nvidia.com/4.5.0/sensors/isaacsim_sensors_physics_contact.html),
  [Effort Sensor](https://docs.isaacsim.omniverse.nvidia.com/5.1.0/sensors/isaacsim_sensors_physics_effort.html),
  [Open-sourcing URDF/MJCF importers](https://discourse.openrobotics.org/t/open-sourcing-nvidia-isaac-sim-urdf-and-mjcf-importer-extensions/34412)
- [Isaac Lab paper (arXiv 2511.04831)](https://arxiv.org/html/2511.04831v1)
- [Project Chrono — ChBody](https://api.projectchrono.org/classchrono_1_1_ch_body.html),
  [Rigid Bodies](https://api.projectchrono.org/rigid_bodies.html)
- [TRI — Rethinking Contact Simulation for Robot Manipulation](https://medium.com/toyotaresearch/rethinking-contact-simulation-for-robot-manipulation-434a56b5ec88)
- [AGX Dynamics product page (Algoryx)](https://www.algoryx.se/agx-dynamics/),
  [Vortex Studio (CM Labs)](https://cm-labs.com/en/vortex-studio/)
- [Source Robotics — URDF vs MJCF vs USD](https://source-robotics.com/blogs/blog/robot-simulation-files-urdf-vs-mjcf-vs-usd)
- [Beyond URDF (arXiv 2512.23135)](https://arxiv.org/html/2512.23135v1),
  [Understanding URDF survey (arXiv 2302.13442)](https://arxiv.org/pdf/2302.13442)

**Friction / contact modeling:**
- [LuGre — Revisiting the LuGre friction model (Olsson et al.)](https://hal.science/hal-00394988/document)
- [Learning Transferable Friction Models and LuGre Identification (arXiv 2504.12441)](https://arxiv.org/html/2504.12441v1)
- [Stribeck Friction overview (ScienceDirect Topics)](https://www.sciencedirect.com/topics/engineering/stribeck-friction)
- [3D LuGre adapted to varying normal forces (Multibody Sys Dynamics)](https://link.springer.com/article/10.1007/s11044-022-09820-5)
- [Models of Friction (BME chapter)](https://www.mogi.bme.hu/TAMOP/robot_applications/ch07.html)

**Aerospace tools and atmospheric models:**
- [NASA GMAT (NASA Software Catalog)](https://software.nasa.gov/software/GSC-17177-1),
  [GMAT documentation](https://documentation.help/gmat/WelcomeToGmat.html)
- [US Standard Atmosphere 1976 (NASA NTRS)](https://ntrs.nasa.gov/citations/19770009539),
  [USS76 (PDAS reference)](https://www.pdas.com/atmos.html),
  [USS76 reference document (NOAA)](https://www.ngdc.noaa.gov/stp/space-weather/online-publications/miscellaneous/us-standard-atmosphere-1976/us-standard-atmosphere_st76-1562_noaa.pdf),
  [Python/C++ implementation (GitHub)](https://github.com/spyderkam/1976-USA-Atmospheric-model)
- [NAIF SPICE (JPL)](https://naif.jpl.nasa.gov/naif/spiceconcept.html),
  [SPICE Tutorials (NASA)](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/Tutorials/pdf/individual_docs/SPICE_Tutorials_all.pdf),
  [SPICE-based JPL DE421 (STK)](https://help.agi.com/stk/12.7.0/LinkedDocuments/SPICE-BasedJPLDE421PlanetaryEphemerides.pdf)

**Cinematic / animation tools:**
- [Maya nDynamics — Nucleus solver](https://download.autodesk.com/us/maya/2011help/files/nCloth_nodes_nucleus.htm),
  [Simulation and Effects (Maya 2023)](https://knowledge.autodesk.com/support/maya/downloads/caas/CloudHelp/cloudhelp/2023/ENU/Maya-SimulationEffects/files/GUID-04EA8AE9-5081-426F-8122-D56461A69B0A-htm.html),
  [Maya Cached Playback whitepaper](https://damassets.autodesk.net/content/dam/autodesk/www/html/maya-cached-playback/2024/MayaCachedPlaybackWhitePaper.html)
- [Houdini Vellum Constraints (SOP)](https://www.sidefx.com/docs/houdini/nodes/sop/vellumconstraints.html),
  [Vellum Constraints (DOP)](https://www.sidefx.com/docs/houdini/nodes/dop/vellumconstraints.html),
  [Vellum nodes overview](https://www.sidefx.com/tutorials/vellum-nodes/),
  [HoudiniVellum cgwiki](https://www.tokeru.com/cgwiki/HoudiniVellum.html),
  [Vellum strut softbody](https://www.sidefx.com/docs/houdini/shelf/vellumsoftbody.html)
- [Unreal Chaos Destruction](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-destruction-in-unreal-engine),
  [Niagara GPU Raytracing Collisions](https://dev.epicgames.com/documentation/en-us/unreal-engine/gpu-raytracing-collisions-in-niagara-for-unreal-engine)

**Physics / asset interchange standards:**
- [glTF KHR_physics_rigid_bodies (Pull Request 2424)](https://github.com/KhronosGroup/glTF/pull/2424),
  [Reference impl repo](https://github.com/eoineoineoin/glTF_Physics),
  [glTF Now and Next (Khronos blog)](https://www.khronos.org/blog/gltf-now-and-next)
- [glTF Extension Registry](https://registry.khronos.org/glTF/)

**Internal Cerid documents:**
- [ADR-0062 — Eylem physics architecture](../decisions/0062-eylem-physics-architecture.md)
- [ADR-0063 — Eylem determinism contract](../decisions/0063-eylem-determinism-contract.md)
- [ADR-0066 — `crd-draw` substrate architecture](../decisions/0066-draw-substrate-architecture.md)
- [ADR-0067 — Eylem force-field architecture](../decisions/0067-eylem-force-field-architecture.md)
- [`cerid-eylem.md`](cerid-eylem.md) — primary research dossier
- [`cerid-eylem-fields.md`](cerid-eylem-fields.md) — force fields dossier
- [`cerid-eylem-collision-filtering.md`](cerid-eylem-collision-filtering.md) — body / filter / callback dossier (backs ADR-0068)
- [`cerid-hesap.md`](cerid-hesap.md) — numerical substrate research
- [`cerid-sdf.md`](cerid-sdf.md) — signed-distance-field substrate research
- [Phase 3.1 plan](../phases/phase-3.1-eylem.md) — slice plan
