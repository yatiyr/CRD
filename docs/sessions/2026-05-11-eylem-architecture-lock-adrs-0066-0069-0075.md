# Session — 2026-05-11 — eylem multi-domain architecture lock (5 ADRs + audit)

## Goal

The user pushed three strategic questions during this session — "what
about complex mesh colliders?", "what about force fields?", "what
about body types + collision filtering + callbacks?" — each pointing at
genuine architectural blanks the v1b implementation was about to land
into. Rather than answer ad-hoc, lock each via a research dossier + ADR
+ phase-plan reservation, then audit the entire eylem coverage against
the 5-domain mandate (games + scientific computing + robotics +
aerospace + cinematic) to surface anything else missing.

Outcome: **5 ADRs minted today** (0066 extended, 0067, 0068, 0069,
0075) + **5 ADRs reserved Planned** (0070-0074) + **4 research dossiers
written** (force fields, collision filtering, materials, coverage
audit) + **4 small extensions** to existing ADRs (0062 / 0066 / 0067 /
0068) cross-referencing the new architecture.

## What we built / changed

### Mesh colliders — five-category model (ADR-0062 §4.5)

User asked "what about complex mesh colliders?" Answered by extending
ADR-0062 with a new §4.5 locking the **five canonical collider
categories** every modern engine ships:
- Primitives (Sphere/Box/Capsule) — analytic, v1d
- Convex (`ConvexHull`) — GJK + EPA, v1d
- Plane (`Plane`) — analytic, v1d
- Static triangle mesh (`TriangleMesh`) — BVH + per-tri SAT, v1d-mesh
- Terrain (`Heightfield`) — per-cell analytic, v1d-hf
- Smooth dynamic mesh (`Sdf`) — closest-point + gradient on baked SDF
  via `crd-sdf`, Phase 3.1.5

`ColliderShape` enum extended with `TriangleMesh = 5` / `Heightfield = 6`
/ `Sdf = 7` + new struct types `ColliderTriangleMesh` /
`ColliderHeightfield` / `ColliderSdf`. `ColliderId` encoding bumped from
`[kind:3 | per_kind_idx:21]` → `[kind:4 | per_kind_idx:20]` to leave
room for compound / fluid / soft-body shapes in Phase v3+.
Test count: ColliderPool tests went 11 → 11 (kept count) but added new
encoding + per-kind unsupported-shape coverage; 215 assertions → 219.

### Force fields — three-tier substrate (ADR-0067)

User asked "what about force fields? gravity wells, magnetic, wind...?"
Spawned a research agent (5,447-word dossier surveying PhysX / Bullet /
Havok / Jolt / Unreal Chaos / Unity DOTS / Godot / Box2D / Houdini /
FleX / Flow / Niagara / Maya nDynamics / Blender). Output:
**ADR-0067 — Eylem force-field architecture** locking three tiers:
- Tier 1: 9 analytic primitives (Directional / Radial / Vortex / Drag /
  Noise / Magnetic / **Gradient** / GridSample / Script). `Gradient`
  consumes `crd-sdf` directly — composes with the SDF substrate
  already in flight.
- Tier 2: vector grid sampling (CustomGrid via cooked `.field.crdr`)
- Tier 3: scripted (Phase 4 prereq)

Plus: field volumes REUSE the 5-category collider model (no new shape
system); `FieldId` content-addressed via FNV-1a-64 (determinism stable);
composition modes Add/Replace/Multiply/Max/Min with id-stable order +
optional `apply_after` DAG for velocity-dependent fields; **Noise must
use analytic-derivative Simplex curl-noise** (Bridson 2007), NOT
finite-difference variants — explicitly forbidden by CI guard.

Slice plan: 9 sub-slices `v1f-fields-a..i` between v1f (joints) and
v1g (islands). `Gradient` (v1f-fields-e) waits on Phase 3.1.5 SDF;
`Script` (v1f-fields-g) waits on Phase 4 scripting.

Public API surface shipped today: `engine/eylem/include/crd/eylem/force_field.hpp`
(~165 LOC, no impl, frozen at v1l). Plus `kAxisX/Y/Z` updated to
Blender 3D View axis hues (X = 255,51,82 / Y = 139,220,0 / Z = 40,144,255)
to match user's visual preference.

### Body types + collision filtering + callbacks (ADR-0068)

User asked broad question about body types + tag-based collision
exclusion + collide/enter/leave callbacks + per-shape filter rules.
Spawned a deeper research agent (10,370-word dossier explicitly
covering 5 mandate domains, surveying MuJoCo / Drake / IsaacSim /
Project Chrono / AGX Dynamics / Vortex Studio in addition to standard
game engines). Output: **ADR-0068 — Eylem body types + collision
filtering + callbacks** locking:

- **3 motion types** (Static / Kinematic / Dynamic) — no fourth.
  Maya "Animated Rigid Body" + MuJoCo mocap body fold into Kinematic
  with engine-inferred velocity from per-step pose delta.
- **Sensor as per-collider flag** (NOT per-body — Jolt is documented
  exception, Cerid follows the 8-engine majority). `ColliderFlags::is_sensor`
  added to `Collider` struct.
- **Specialised actor types via ECS composition** — CharacterController
  / VehicleBody / ArticulationLink / SoftBody / GpuParticle /
  GeometryCollection all atop the 3 motion types.
- **5-tier filtering pipeline** (cheapest-first; pair survives only if
  every tier passes):
  1. 64-bit bit-mask layers (mutual consent) — ~3 cycles
  2. Box2D-style group index (signed i16 override) — ~1 cycle
  3. Explicit excluded pairs (URDF self-collision matrix) — ~10-20 cycles
  4. ECS-native predicate with **closed read set declared at registration**
     (Bevy Rapier `BevyPhysicsHooks` formalism, Cerid stricter) — ~50-500 cycles
  5. Articulation / joint implicit auto-filter — ~5 cycles
- **PhysX-style filter shader EXPLICITLY REJECTED** on determinism grounds.
- **Deferred ECS event-stream callbacks** sorted by `(min(body_a,body_b),
  max(body_a,body_b), kind)` — NOT synchronous virtual callbacks.
  Begin/End first-class for both ContactEvent + TriggerEvent. **Persist /
  Stay opt-in per pair** (default OFF — destruction-scene event-storm
  avoidance, Box2D v3 default).
- **`ContactModify` as separate v1g+ pure-function API** (signature
  enforces no World handle, no RNG, no time, no external state).

Public API surface shipped today: `engine/eylem/include/crd/eylem/collision_filter.hpp`
(~270 LOC) + extended `collider.hpp` with `ColliderFlags::is_sensor` +
extended `physics_scene.hpp` with Tier 3 + Tier 4 + ContactModify
registration + event drains. `NullPhysicsScene` null-impls all new
methods.

Slice plan: 9 sub-slices spanning v1c-sensor + v1d-filter-{a,b,c} +
v1d-callback-{a,b,c} + v1f-articulation-filter + v1g-contactmodify.

### Coverage audit (`docs/research/cerid-eylem-coverage-audit.md`)

User asked "do we cover everything for a beyond-industry-standard
physics engine?" Spawned the third research agent — full coverage
audit against the 5-domain mandate. Output: **8,604-word dossier
identifying 7 real gaps**, with proposed fill plan ordered into
**3 priority waves**:

- **P0 (must close before v1l freeze)**: ADR-0069 materials substrate
  (gates v1a interface), ADR-0075 testing rigor (gates the scientific
  computing claim).
- **P1 (block multi-domain claim)**: ADR-0073 aerospace substrate (new
  `crd-eylem-aero` module), ADR-0071 robotics importers + actuator
  catalogue.
- **P2 (post-v1)**: ADR-0070 solver catalog (Nonsmooth Newton),
  ADR-0072 sensor substrate, ADR-0074 cinematic / animation-physics
  bridge (new `crd-eylem-cine` module), tuning cookbook.

**Critical architectural insight**: aerodynamics is NOT field-shaped
(reads body velocity at evaluation time) — would have been an ugly
retrofit had we tried to cram it into ADR-0067 a year from now. Locked
as separate `AeroDynamicsComponent` + `AeroForceEvaluator` system
(ADR-0073). Only `J2Gravity` is field-shaped → reserved enum slot in
ADR-0067 (filled by v1f-fields-j alongside v6f aero cluster).

### Wave 1 ships — ADR-0069 + ADR-0075 + 4 ADR extensions

User approved the audit. Wave 1 (P0) shipped today:

**ADR-0069 — Eylem materials substrate** (research dossier:
`docs/research/cerid-eylem-materials.md`, 9,650 words across 16
engines/tools surveyed). Locks:
- 64-byte cache-line `Material` struct (frozen at v1a freeze)
- 6-value `FrictionModel` (Coulomb / Stribeck / LuGre / Karnopp /
  Anisotropic / **FrictionTriple** — last reserves room for v5 vehicles'
  MuJoCo-style sliding/torsional/rolling triple via `friction_anisotropy`
  reinterpretation; struct does NOT grow)
- 3-value `RestitutionModel` (Constant / Newton / HuntCrossley)
- Extended `CombineMode` adds **GeometricMean** (slot 4) — Box2D v3 /
  Jolt / Unity DOTS / AGX consensus default for stacking-stable friction
- `MaterialId` content-addressed via FNV-1a-64 (same discipline as `FieldId`)
- Per-collider material via handle (PhysX / Jolt pattern)
- LuGre per-contact bristle state folded into ADR-0068 §8 contact
  warm-start cache (16B lazy alloc); **Tustin (implicit trapezoidal)
  discretisation with cooker-precomputed division-free constants** —
  runtime never divides; bit-exact across compilers.
- AGX-style per-pair `ContactMaterial` REJECTED for v1 (O(N²) storage)
- 8 shipped default materials: Default / Rubber / Steel / Ice / Wood /
  Concrete / Water / Flesh

**ADR-0075 — Eylem testing rigor** (self-contained, no companion
dossier; ~1450 LOC + ~30 tests + 5 benches plan). Six categories:
1. Conservation-law CI (energy / momentum / angular momentum /
   constraint violation) — every solver, frictionless gravityless scene
2. Closed-form regression (pendulum / projectile / SHO / drag terminal /
   circular / rolling / Kepler) — catches "self-consistent but wrong"
3. Long-duration drift (60-min, snapshot-hash rate-bounded, nightly CI)
4. Cross-engine comparison (Box2D + Bullet test-only deps; "within 2×
   best-of-three" elite-tier bar)
5. Property-based (random scenes via Catch2 generators; 100 / test)
6. Stress (1k stack / 30-link humanoid / broadphase-co-located / 100k particles)

**4 ADR extensions** applied:
- ADR-0062 §5.5 — full reservations list cites ADR-0069 / 0070 / 0071 /
  0072 / 0073 / 0074 / 0075
- ADR-0067 §3 — `Reserved_J2` enum slot + clarification that
  aerodynamics is NOT field-shaped (separate `AeroDynamicsComponent`
  per ADR-0073)
- ADR-0068 §9 + §10 — cross-references to ADR-0072 + ADR-0074
- ADR-0066 §19.2.1 — Diagnostic category slot reservation
  (solver-convergence viewer)

### 5 ADRs reserved Planned (0070-0074)

For Wave 2 (P1) + Wave 3 (P2), the slot is reserved but the ADR mints
later when its dossier ships. README + phase plan + context bullets
reflect Planned status.

## Plain-English explanation

The day was less about code than about LOCKING the architecture so
future code work isn't a scramble. Three user prompts — each opening a
deep architectural cavity — became three locked ADRs (0067 fields,
0068 bodies/filters/callbacks, 0069 materials) plus one (0075) that
catches the "scientific computing claim" if we don't bake it in from
day one.

Plus a coverage audit identified 4 more concerns we didn't have time
to dossier today (aerospace, robotics importers, sensors, cinematic +
solver catalog). Those landed as Planned reservations — phase-plan slot
reserved, ADR row in the README marked Planned, mint when the dossier
ships at slice time.

The cumulative effect: **the entire Phase 3.1 v1 surface is now locked
across 8 ADRs + reserved across 5 more.** v1b-c onward is implementation
along the locked surface. No more "wait, what about X?" surprises that
force retrofits.

## Decisions made

(See individual ADRs for the full decision lock; this is the meta-list.)

- **No fourth body type** — the universal 3 motion types + sensor as
  per-collider flag is the right answer for a multi-domain engine.
- **5 collider categories shipped, not just SDF** — picking ONE shape
  category locks Cerid out of workloads where another is 10-100× better.
- **5 filtering tiers, no PhysX filter shader** — recovers expressiveness
  via Tier 4 (ECS predicate with closed read set), keeps determinism.
- **Deferred ECS event-stream callbacks** — sync virtual callbacks
  break determinism the moment narrow phase fans to fibres.
- **Persist/Stay opt-in per pair** — the destruction-scene event-storm
  prevention every elite engine learned the hard way.
- **`Material` is 64 bytes (one cache line)** — locked size; field
  overloading by enum gates slot interpretation; struct does NOT grow.
- **`Gradient` consumes `crd-sdf` directly** — unifies the SDF
  substrate across physics + renderer + audio + font + editor.
- **Aerodynamics is NOT field-shaped** — ships as separate
  `AeroDynamicsComponent` per ADR-0073; J2 IS field-shaped → reserved
  enum slot in ADR-0067.
- **All ID types content-addressed (FNV-1a-64)**: `FieldId` (ADR-0067),
  `MaterialId` (ADR-0069). Determinism contract from day one.
- **6 test categories, "within 2× best-of-three" elite-tier bar** —
  the scientific computing domain claim becomes substantiated.

## Files touched

### New ADRs (mints today, Accepted)
- `docs/decisions/0067-eylem-force-field-architecture.md` (~10K words)
- `docs/decisions/0068-eylem-body-types-collision-filtering-callbacks.md` (~10K words)
- `docs/decisions/0069-eylem-materials-substrate.md` (~5K words)
- `docs/decisions/0075-eylem-testing-rigor.md` (~3K words; self-contained)

### New research dossiers
- `docs/research/cerid-eylem-fields.md` (5,447 words, 14 sections)
- `docs/research/cerid-eylem-collision-filtering.md` (10,370 words, 11 sections)
- `docs/research/cerid-eylem-coverage-audit.md` (8,604 words, 5 sections + 14 sub-tables)
- `docs/research/cerid-eylem-materials.md` (9,650 words, 13 sections)

### ADR extensions
- `docs/decisions/0062-eylem-physics-architecture.md` — added §4.5 (five-category collider model) + §5.5 (Wave 1+2+3 ADR reservations)
- `docs/decisions/0066-draw-substrate-architecture.md` — §19.2.1 Diagnostic category slot
- `docs/decisions/0067-eylem-force-field-architecture.md` — §3 reserved aero/J2 + clarified aero-not-field-shaped
- `docs/decisions/0068-eylem-body-types-collision-filtering-callbacks.md` — §9/§10 cross-refs to ADR-0072/0074

### Code (API surface only — no impl)
- `engine/eylem/include/crd/eylem/force_field.hpp` — ADR-0067 surface (~165 LOC)
- `engine/eylem/include/crd/eylem/collision_filter.hpp` — ADR-0068 surface (~270 LOC)
- `engine/eylem/include/crd/eylem/collider.hpp` — added 3 new ColliderShape enum values + 3 new shape data structs (TriangleMesh / Heightfield / Sdf) + `ColliderFlags::is_sensor`
- `engine/eylem/include/crd/eylem/physics_scene.hpp` — extended IPhysicsScene with Tier 3 (`exclude_pair` etc.) + Tier 4 (`set_collision_predicate`) + ContactModify registration + event drains
- `engine/eylem/src/null_physics_scene.cpp` — null-impl for all new methods

### Index updates
- `docs/decisions/README.md` — added rows for ADR-0067 / 0068 / 0069 / 0075 (Accepted) + 0070-0074 (Planned)
- `docs/phases/phase-3.1-eylem.md` — 26 new sub-slice rows woven in across v1, v4, v6, v9
- `context.md` — arrow chain extended with all new sub-slices + 5 new locked-decisions bullets
- `engine/eylem/include/crd/eylem/eylem.hpp` — umbrella now includes `force_field.hpp` + `collision_filter.hpp`

## Tests / verification

- Built? ✅ Whole engine clean across all changes
- `crd-eylem-tests`: 117/117 assertions pass (15 cases) — ContactEvent /
  TriggerEvent / ContactPairFlags / ColliderFlags layout pinned via
  static_asserts
- `crd-eylem-rigid3d-tests`: 219/219 assertions pass (19 cases — 8
  BodyPool + 11 ColliderPool, all updated for new encoding)
- Sweep cadence: win-debug only this session (architectural lock work,
  no algorithmic risk). Full sweep batched at end of v1a-material
  cluster per project policy.

## Next session starts with

- **v1a-material-a**: lock the 64-byte `Material` struct + enums in
  code (per ADR-0069 §1-§3). Surface mint that v1a-material-b (Pool) +
  v1a-material-c (per-collider material on Collider) + v1a-material-d
  (mass derivation) build on. Critical-path P0 — gates v1a-b interface
  freeze.
