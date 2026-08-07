# ADR-0067 — Eylem force-field architecture

> Status: **Accepted** (2026-05-11)
> Companions: [ADR-0062](0062-eylem-physics-architecture.md) (eylem
> architecture), [ADR-0063](../decisions/0063-eylem-determinism-contract.md)
> (determinism contract), [ADR-0064](0064-sdf-substrate-architecture.md)
> (`crd-sdf`), [ADR-0066](0066-draw-substrate-architecture.md) (`crd-draw`).
> Research dossier: [`docs/research/cerid-eylem-fields.md`](../research/cerid-eylem-fields.md).

## Context

Force fields — gravity overrides, wind, vortex, drag, magnetic actuators,
attractor wells, turbulent flow, baked vector grids — appear in every
shipped physics engine, but **as ad-hoc additions** rather than as a
designed substrate. PhysX gives you `PxRigidBody::addForce` and a trigger
overlap callback; the rest is your problem. Bullet's `btActionInterface`
is the same. Havok's `hkpAction` is the same. Jolt's `BodyInterface::AddForce`
is the same. Unreal carries **three generations** of field code
simultaneously (Cascade attractors, RadialForceComponent, Chaos Field
System) — the Chaos Field System is the most ambitious, but only because
the previous two generations failed to scale to all the use cases.

Cerid's mandate is broader than any shipped engine: **rigid 3D + rigid 2D
+ soft + cloth + rope + articulations + vehicles + FEM + GPU particles +
acoustic occlusion**, all sharing one ECS world (ADR-0062). Six parallel
field implementations would dominate the bug budget. The research dossier
(`cerid-eylem-fields.md`) surveys 14 engines/tools across 13 domains and
concludes: **fields must be a substrate, not a per-system component.**

The user-facing requirements that drove this ADR:

1. **Designer-authorable** without writing code (the 90% case is gravity
   wells, wind tunnels, drag zones, vortex traps — all parametric).
2. **Extensible** to artist-painted vector grids (turbulent wind, swirling
   galaxy maps, magnetic field maps from MRI scanners, baked CFD output).
3. **Scriptable** for the 1% of cases that need gameplay-driven
   custom formulas (Phase 4 territory).
4. **Performant for games** — not just VFX/sim. SIMD over AoSoA bodies
   in the inner loop; budget-checked in CI.
5. **Debuggable** — force-field arrows visible in the editor, sampled at
   the field's sample stride, integrated with `crd-draw`'s VisualizerRegistry.
6. **Saveable as prefabs (öbeks)** in the Phase 7 editor —
   `ComponentSerialize` trait on `ForceFieldComponent`.
7. **Deterministic** — bit-exact replay across MSVC/clang/gcc × x64/ARM
   per ADR-0063. Field application participates in the replay-hash CI.
8. **Reuses existing substrates** — collider shapes for field volumes,
   `crd-sdf` for gradient fields, `crd-draw` for debug viz, öbek for
   prefabs, ECS for everything.

## Decision

### 1. Three-tier force-field model

Eylem ships **one** force-field substrate consumed by every physics
solver (rigid, soft, cloth, rope, articulation, FEM, particles). The
substrate offers three tiers, each addressing a different authoring
workflow:

| Tier | Authoring path | Coverage | When |
|---|---|---|---|
| **1. Analytic primitives** | `ForceFieldComponent` parameters in TOML / editor | gravity wells, wind, vortex, drag, magnets, turbulence, SDF gradients — the 90% case | v1f-fields-a..e ship |
| **2. Vector grid** | `.field.crdr` resource cooked from artist-painted grid / Houdini export / CFD bake | swirling galaxies, baked turbulence, MRI-derived magnetic maps | v1f-fields-f ships |
| **3. Scripted** | `ScriptComponent` callback registered against the field's id | gameplay-driven dynamic formulas — the 1% case | v1f-fields-g ships (Phase 4 scripting prereq) |

All three tiers share **the same dispatch loop**, the same
composition rules, the same debug viz, the same determinism contract,
the same öbek serialization. The tier shows up only in *what value the
formula returns* — analytic = compute from parameters, grid = trilinear
sample, script = call user code.

This is the design Houdini's POP/DOP networks vindicated over 20 years:
analytic primitives + grid sampling + VEX wrangle covers everything any
serious VFX/sim shop needs. Cerid lifts the model wholesale because no
other shipped engine has a strictly better one.

### 2. Field volume = collider shape

A force field's volume of effect is defined by **any of the five eylem
collider shapes** (ADR-0062 §4.5):

- `Sphere` → gravity well, repulsor, point magnet
- `Box` → wind tunnel, conveyor zone, room-scale pressure
- `Capsule` → vortex spine, river current, tornado funnel
- `Plane` → infinite ground gravity override, half-space drag
- `ConvexHull` → arbitrary convex region (Phase v1d)
- `TriangleMesh` → static authored region (Phase v1d-mesh)
- `Heightfield` → terrain-conformal field (Phase v1d-hf)
- `Sdf` → arbitrary smooth volume (Phase 3.1.5) — **also feeds Gradient
  formula** for fields that push along the SDF gradient

Plus a special **infinite** volume (no collider) for world-spanning
fields like global gravity overrides.

This is a deliberate composition choice. The collider system already
ships AABB tree integration, cooker support, debug viz, öbek
serialization, ECS attachment. Field volumes inherit all of it for free
and get every collider extension automatically. PhysX/Bullet/Havok all
lean on collider/trigger reuse for the same reason.

### 3. Field formula enum (Tier 1 + 2 + 3)

```cpp
enum class FieldFormula : crd::u8
{
    Directional = 0, // f = direction · magnitude
    Radial      = 1, // f = k · sign / (r + r_min)^p · r̂
                     //   covers Newton (p=2, sign=-1, mass-couple),
                     //   Coulomb (p=2, sign=±1, charge-couple),
                     //   Hooke (p=1, sign=-1, signed by displacement)
    Vortex      = 2, // f = ω × (p - axis)
    Drag        = 3, // f = -k · v · |v|^(p-1)
                     //   p=1 linear (Stokes), p=2 quadratic (form drag)
    Noise       = 4, // f = curl_noise(p, t, scale, octaves)
                     //   analytic-derivative Simplex per Bridson 2007
                     //   (DETERMINISM-CRITICAL — see §7)
    Magnetic    = 5, // f = q · v × B  (B is a field-local vector)
    Gradient    = 6, // f = ±k · ∇φ(p) sampled from a crd-sdf SdfResource
                     //   covers "push away from collider", "respect SDF
                     //   surface", Houdini's Volume Axis push/pull
    GridSample  = 7, // f = trilinear_sample(VectorGridResource, p)
                     //   Tier 2 — consumes a cooked .field.crdr asset
    Script      = 8, // f = ScriptComponent::eval_field(...)  Tier 3 (Phase 4)
};
```

**Nine values cover every formula the industry survey turned up.**
Coulomb / Newton / Harmonic / Spring collapse into `Radial` parameterised
by polarity + falloff + mass-coupling — fewer enum values means a
cleaner cooker grammar. `Boid` and `Lennard-Jones` (Blender) are
explicitly **rejected**: domain-specific, expressible as `Script` for
the rare consumer.

**Reservation for J2 (added 2026-05-11 per coverage audit §3.5).** The
[ADR-0073](0073-eylem-aerospace-substrate.md) aerospace substrate adds
**`Reserved_J2`** — Earth-oblateness gravity correction `f = -μ·m·r̂/r²
+ J₂·correction(p, ω_E)`. J2 is field-shaped (pure function of position),
fits the existing dispatch loop, fills inside the closed-enum surface
when aerospace ships. **Aerodynamic drag is NOT field-shaped** (reads
the body's own velocity at evaluation time, not just position) — it
ships as a separate `AeroDynamicsComponent` + `AeroForceEvaluator`
system per ADR-0073. Keeping aero out of `FieldFormula` preserves the
closed-enum determinism audit.

The enum is locked at v1l API freeze. New formulas require a major-
version bump of `crd-eylem`. New formulas SHOULD prove themselves as
`Script` first; if they earn a permanent slot they get promoted.

### 4. Falloff models

```cpp
enum class FieldFalloff : crd::u8
{
    Constant     = 0, // f stays at full magnitude inside volume
    Linear       = 1, // f scales (1 - r / r_max) linearly to zero at boundary
    InverseLinear = 2, // f scales 1 / (r + r_min)
    InverseSquare = 3, // f scales 1 / (r + r_min)^2  (real Newton/Coulomb)
    Smoothstep   = 4, // f scales smoothstep(r_max, r_min, r) — C¹ continuous boundary
    Polynomial   = 5, // f scales by user-tunable cubic coefficients (escape hatch)
};
```

Smoothstep is what production engines reach for after "Linear looks
bad at the boundary." Polynomial covers the long tail (cubic, near-
exponential decay) without exploding the enum. Six values, locked.

### 5. Mass coupling modes

```cpp
enum class FieldMassCoupling : crd::u8
{
    Force        = 0, // f applied as raw force; heavier bodies accelerate less
    Acceleration = 1, // f applied as acceleration; mass-independent
    GravityStyle = 2, // f scaled by mass internally, then applied as force
                      //   (matches Earth's gravity — heavier bodies fall the same)
    Impulse      = 3, // f integrated as impulse (for OnEnter / OnEnterOnce triggers)
    VelocitySet  = 4, // f *replaces* velocity component (Maya Air-style; rare)
};
```

`VelocitySet` is the Maya nDynamics "Air" field's behaviour — the
field forces the body's velocity toward a target rather than adding
force. Rare but well-known to artists; the survey kept turning it up.
Five values, locked.

### 6. Composition rules

When N fields overlap a body, **how do their forces compose?** The
research dossier (`cerid-eylem-fields.md` §7) found that "additive sum"
is the dominant pattern but breaks for replace-style fields (gravity
override) and clip-style fields (drag cap). Cerid ships **5 composition
modes** per field:

```cpp
enum class FieldComposition : crd::u8
{
    Add      = 0, // accumulate (default — vector sum into body's force)
    Replace  = 1, // overwrite the accumulator with this field's contribution
    Multiply = 2, // multiply the accumulator (for damping / scaling fields)
    Max      = 3, // take componentwise max (clip-style)
    Min      = 4, // take componentwise min
};
```

**Application order is id-stable** — fields apply in ascending
`FieldId` order. `FieldId` is a content-addressed hash (FNV-1a over the
field's serialized parameters), guaranteeing the same field-set produces
the same iteration order across machines.

**Velocity-dependent ordering.** Some fields (Drag, Magnetic) read the
body's current velocity to compute their force. If field A modifies
velocity and field B reads it, B sees A's update — order matters.
Default: each field reads the substep-start velocity (parallelizable,
deterministic, breaks intuition for "drag after thrust"). Optional
per-field `apply_after = [FieldId, ...]` declares dependencies; the
field scheduler topologically sorts and applies in waves. Default empty
deps = parallel evaluation, maximum throughput.

### 7. Determinism contract (CRITICAL)

Per ADR-0063, fields participate in the cross-platform replay-hash CI.
Four field-specific failure modes were identified in the research and
are blocked at the architecture level:

1. **Non-deterministic summation order.** Fix: id-stable ordering by
   `FieldId` + fixed-position writes per ADR-0063 §4 (no atomic
   counters; per-fiber accumulators reduced via the ADR-0063
   commutative-merge protocol).

2. **Curl-noise finite differences.** Bridson 2007 introduced curl-
   noise as `∇ × ψ(p)` where ψ is a Simplex potential. The naive
   implementation computes ∇ × ψ via finite differences (small ε
   shifts in each axis), which is **NOT bit-exact** across compilers
   (FP precision of `(a − b) / ε` varies). Fix: `Noise` formula MUST
   use the **analytic-derivative Simplex curl-noise** form
   (`crd::math::deterministic::noise::simplex_curl`). Finite-difference
   variants are explicitly forbidden. CI guard rejects code referencing
   `simplex_finite_diff_*`.

3. **Velocity-dependent ordering racing the integrator.** Fix:
   `apply_after` DAG is part of the öbek's content-addressed hash;
   topologically sorted in setup, applied in waves at substep.

4. **Field add/remove permuting `FieldId` between runs.** Fix: `FieldId`
   is the FNV-1a hash of the field's serialized parameters (per the
   öbek serialisation path), NOT a sequential counter. Identical field
   sets produce identical ids regardless of insertion order.

The bench suite (`tests/eylem/bench_fields.cpp`, lands with v1f-fields-i)
runs the replay-hash check at every CI tier.

### 8. Spatial dispatch

Reuse the eylem dynamic AABB tree (ADR-0062 §3) as the field index.
Each field's volume AABB participates in the broadphase alongside body
AABBs. World-spanning infinite-volume fields (e.g., a global gravity
override) bypass the spatial query entirely.

For very large field counts (>1k active) a secondary BVH over fields
rebuilds incrementally — **deferred until measured need**. Today's
budgets accommodate ~hundreds of fields with no secondary structure.

### 9. ECS surface

```cpp
// In crd-eylem (interface module).
struct ForceFieldComponent
{
    FieldFormula      formula;        // Directional / Radial / ... / Script
    FieldFalloff      falloff;        // Constant / Linear / ... / Polynomial
    FieldMassCoupling mass_coupling;  // Force / Acceleration / GravityStyle / ...
    FieldComposition  composition;    // Add / Replace / Multiply / Max / Min
    FieldTrigger      trigger;        // Continuous / OnEnter / OnExit / OnEnterOnce

    // Formula parameters (interpreted per `formula`).
    crd::math::Vec3f  direction;      // Directional / Magnetic (B vector)
    crd::math::Vec3f  axis;           // Vortex
    crd::f32          magnitude;      // all
    crd::f32          radius_min;     // Radial / Magnetic (avoid 1/0 singularity)
    crd::f32          radius_max;     // Linear / Smoothstep falloff cutoff
    crd::f32          falloff_p;      // Radial exponent / Drag exponent
    crd::f32          polarity;       // Radial sign (+1 attract, -1 repel)
    crd::math::Vec4f  poly_coeffs;    // Polynomial falloff coefficients

    // Tier 2 / Tier 3 handles (only one is used, by formula).
    crd::resources::ResourceHandle<VectorGridResource> grid;
    crd::resources::ResourceHandle<SdfResource>        sdf;     // Gradient
    crd::scene::ScriptComponentHandle                  script;  // Phase 4

    // Determinism-stable id (FNV-1a hash of all above + entity öbek path).
    crd::u64          field_id;

    // Composition DAG (rare — empty default = parallel).
    crd::containers::Array<crd::u64> apply_after;
};
static_assert(sizeof(ForceFieldComponent) == /* TBD; freezes at v1l */);
```

**Storage hint:** SparseSet (low total count per scene, high per-frame
read rate, attached to relatively few entities — same ADR-0050
rationale as `RigidBodyComponent`).

**Serialization trait:** `ComponentSerialize` (FourCC `'EYFF'`,
version 1) — round-trips through öbek prefabs in the Phase 7 editor.

```cpp
// In crd-eylem-rigid3d (impl module — and later crd-eylem-soft, etc.).
class EylemFieldSystem : public crd::scene::ISystem
{
    // SchedulePhase::PrePhysics — fields apply BEFORE the integrator
    // reads forces.
    SchedulePhase phase() const override { return SchedulePhase::PrePhysics; }
    void run(crd::scene::World& world) override;
};
```

Single system, deterministic per-substep. Fiber-jobified (per-tile
parallelism over the body AoSoA) once profiling justifies — until then
single-threaded, deterministic by trivial means.

### 10. Debug visualization

Force fields have a **dedicated visualizer** in `crd-eylem-viz`
(deferred from d3, lands with v1b in the broader v1b-d slice). It
registers against `crd-draw`'s `VisualizerRegistry` (d3 hook) and emits
arrows sampled on the field volume's interior:

- Sample stride = `volume_extent / 8` per axis (configurable per-field
  via `DebugVizComponent.scale`)
- Per-sample arrow direction = field formula evaluated at sample point
- Per-sample arrow color = `tint × normalized_magnitude` (so weak
  regions fade)
- `DebugVizComponent::ShowFieldArrows` flag gates emission (default ON
  for entities with `ForceFieldComponent`)

The arrow density scales with the field's debug detail slider. A dense
1024-arrow grid for editor inspection; a sparse 16-arrow scout for
gameplay. Both go through the same emit path.

`crd-draw`'s axis-conventional Blender hues (X red, Y green, Z blue)
DO NOT apply — field arrows use a configurable tint with magnitude-
modulated alpha. Designers can override per-field for clarity.

### 11. Öbek prefab serialization (Phase 7 editor)

`ForceFieldComponent` carries the `ComponentSerialize` trait so it
round-trips through the öbek system (ADR-0058):

- TOML schema: `[components.eylem.force_field]` block with all
  enum/scalar fields named explicitly. Resource handles serialize as
  asset paths.
- Cooker handler: `tools/asset_cooker/src/cook_handlers/eylem_field.cpp`
  validates parameter ranges + canonicalises FieldId.
- Editor (Phase 7): drag-drop `ForceFieldComponent` onto an entity,
  edit fields in the property panel, save as prefab (öbek). The
  `Vortex Trap` öbek bundles a `Capsule` collider + `ForceFieldComponent
  {formula=Vortex, axis=+y, magnitude=20}` ready to drop into any
  scene.

Default öbeks shipped with v1f-fields-i (sandbox demo): `GravityWell`,
`WindTunnel`, `VortexTrap`, `DragField`, `MagneticPulse`,
`TurbulentZone`. Authored as TOML, cooked into the sandbox demo pack.

### 12. Performance budget

Per the research dossier §6, calibrated against published Niagara
numbers (10k particles + force module @ 60 Hz ≈ 0.5 ms on consumer
hardware). Cerid's targets at 8-lane SIMD on a modern x64:

| Field count | Body count | Budget (per substep) |
|---|---|---|
| 1 (analytic) | 1k | ≤ 0.1 ms |
| 1 (analytic) | 10k | ≤ 0.6 ms |
| 1 (analytic) | 100k | ≤ 6.0 ms |
| 100 (analytic, mixed formulas) | 1k | ≤ 0.5 ms |
| 1 (GridSample, 64³ grid) | 10k | ≤ 1.5 ms |
| 1 (Noise) | 10k | ≤ 2.0 ms (curl-noise is cycle-heavy) |
| 1 (Script) | 1k | ≤ 4.0 ms (script overhead dominates) |

These become hard CI assertions in `tests/eylem-rigid3d/bench_fields.cpp`
shipped with v1f-fields-i. Regressions fail the build — same model the
Phase 2.5 jobs benchmarks use.

### 13. Slice plan (locked)

Field substrate slots into Phase 3.1 v1 between **v1f (joints)** and
**v1g (islands)** — fields apply per-body forces **before** constraint
resolution, which is the natural boundary. Nine sub-slices, ~1500 LOC
total + ~30 tests + 1 bench:

| Sub-slice | Scope | LOC | Tests |
|---|---|---|---|
| **v1f-fields-a** | `ForceFieldComponent` declaration in `crd-eylem` (interface) + `EylemFieldSystem` skeleton in `crd-eylem-rigid3d` + Tier 1 formulas: `Directional`, `Radial`, `Drag` (the three most common). | ~350 | ~6 |
| **v1f-fields-b** | Tier 1: `Vortex`, `Magnetic`. | ~150 | ~3 |
| **v1f-fields-c** | Tier 1: `Noise` — analytic-derivative Simplex curl-noise via `crd::math::deterministic::noise::simplex_curl`. CI guard rejecting finite-difference variants. | ~250 | ~4 |
| **v1f-fields-d** | Composition modes (`Add` / `Replace` / `Multiply` / `Max` / `Min`) + content-addressed `FieldId` (FNV-1a) + trigger semantics (`Continuous` / `OnEnter` / `OnExit` / `OnEnterOnce`). | ~200 | ~5 |
| **v1f-fields-e** | Tier 1: `Gradient` formula consuming `SdfResource` from `crd-sdf` (Phase 3.1.5). **Lands AFTER Phase 3.1.5 closes.** | ~150 | ~3 |
| **v1f-fields-f** | Tier 2: `VectorGridResource` + `.field.crdr` cooker handler + `GridSample` formula + trilinear sampling. | ~200 | ~4 |
| **v1f-fields-g** | Tier 3: `Script` formula via `ScriptComponent` integration. **Lands AFTER Phase 4 scripting ships.** | ~100 | ~2 |
| **v1f-fields-h** | Visualizer hook in `crd-eylem-viz` — force-field arrows via `crd-draw`'s `VisualizerRegistry` (d3 pattern). Per-field arrow density slider. | ~150 | smokes |
| **v1f-fields-i** | Bench suite + budget assertions in CI + sandbox demo (gravity well + wind tunnel + vortex trap + drag field running together) + 6 default öbek prefabs. | ~150 | bench + smokes |

**Dependency notes:**
- v1f-fields-e blocked by Phase 3.1.5 (SDF substrate).
- v1f-fields-g blocked by Phase 4 (scripting).
- All other slices (a, b, c, d, f, h, i) are unblocked and ship in v1
  proper between v1f (joints) and v1g (islands).

The two blocked slices ship at their natural slot when their
dependencies arrive — they are NOT prerequisites for v1g (islands) or
v1l (v1 close). v1l ships with the API surface frozen; the blocked
slices fill in their formula impls inside the already-frozen surface.

## Rationale

### Why three tiers, not two

Two-tier models (analytic + scripted) force every artist into either
the rigid analytic catalogue or programmer-maintained custom code.
Houdini's 20-year reign at the top of VFX/sim is largely because POP
networks let artists composite analytic fields **and** sample
artist-painted vector grids without writing VEX. The middle tier —
authored vector grids — is non-negotiable for cinematic + medical
workflows.

Two-tier models (analytic + grid) force every gameplay programmer to
either accept the analytic catalogue or hack around it. Phase 4
scripting will need a "register a formula" hook regardless; designating
it as Tier 3 from day one means the substrate composes with the
inevitable.

### Why reuse collider shapes for field volumes

Five reasons:

1. **Authoring economy.** Designers learn ONE volume system. A `Capsule`
   collider's parameters work identically as a vortex spine, a drag
   field, a magnet field, a sound-occlusion volume.
2. **Cooker economy.** The `.collider.toml` cooker handler validates
   shape parameters once. Field volumes inherit it.
3. **Spatial dispatch economy.** Field AABBs go into the same dynamic
   AABB tree the broadphase uses. One spatial structure, one update
   path, one cache footprint.
4. **Debug viz economy.** `crd-eylem-viz` already renders collider
   wireframes; field visualization adds arrows ON TOP of the existing
   shape outline.
5. **Composability.** A `Sdf` collider can be the field volume AND the
   gradient source for a `Gradient` formula in the same component.
   "Push outward from this exact volume" becomes one line of TOML.

PhysX, Bullet, Havok, Unreal Chaos all reuse collider/trigger shapes
for field volumes. Cerid follows the consensus.

### Why `FieldId` is content-addressed, not sequential

A sequential counter would assign different ids on different runs if
field add/remove order varies (which it does — async load, scene-graph
order, editor undo/redo). Content-addressing via FNV-1a guarantees the
same field-set produces the same iteration order regardless of
insertion sequence — which is the determinism contract that ADR-0063
demands.

The cost is an FNV-1a hash on each field at registration time (~1 µs
for a 256-byte component) and a 64-bit comparison on each iteration.
Both are negligible vs the SIMD inner loop.

### Why `Gradient` consumes `crd-sdf` directly

The alternative (bake gradient into a `GridSample` vector grid) costs
3× memory for the same data and divorces the field from the live SDF
authoring loop. When a designer edits the SDF, the gradient field
updates automatically. When `crd-sdf`'s narrow-band sparse storage
(Phase 3.1.5 v3) saves 90% memory, the field benefits without any
porting work. Tight coupling here is correct because the alternative
duplicates infrastructure.

This same logic justifies why Cerid's `crd-sdf` is a substrate, not a
"physics SDF" + "renderer SDF" + "audio SDF" pile.

### Why `FieldFormula` is closed (not "register a formula at runtime")

The closed enum gates the deterministic-formula audit. The CI guard
that rejects non-deterministic FP can verify a fixed nine-formula set
exhaustively. An open registry would defer the audit to runtime
registration — fragile, loses the build-time guarantee.

`Script` is the open-formula escape hatch. Its determinism is the
script runtime's contract (Phase 4 territory), not the field
substrate's. Putting all open-formula behaviour behind a single
`Script` entry keeps the determinism boundary sharp.

## Consequences

**Positive:**
- One field substrate shared by every solver. No "but the cloth solver
  doesn't know about magnets."
- Designer-authorable for the 90% case via Tier 1 + öbek prefabs.
- Cinematic / medical workflows unblocked by Tier 2 vector grids.
- Phase 4 scripting auto-extends fields without API changes.
- Determinism pre-baked at the architecture level (id-stable
  iteration, content-addressed ids, analytic-derivative noise).
- Reuses existing substrates (collider, sdf, draw, öbek, ECS) — no
  new authoring surfaces.
- Performance budgets pinned in CI from day one.

**Negative:**
- Closed `FieldFormula` enum requires a major-version bump for new
  formula slots. Mitigation: `Script` covers experimental formulas
  until they earn promotion; the nine current slots cover everything
  the survey turned up.
- `FieldId` content-addressing means renaming a field's parameters
  changes its id, which breaks `apply_after` references. Mitigation:
  cooker emits a warning on `FieldId` change in an existing scene; the
  editor offers a "preserve dependencies" rename.
- Three blocked sub-slices (v1f-fields-e, v1f-fields-g, v1f-fields-i)
  ship at their dependency dates rather than as a single batch.
  Mitigation: API surface freezes in v1l regardless — the blocked
  slices fill in formulas inside the frozen surface, not extend it.

**Neutral:**
- Field substrate adds ~1500 LOC + ~30 tests + 1 bench to v1. Material
  but proportional to the value (a substrate every Cerid solver
  consumes).
- `crd-sdf` becomes a load-bearing dep for v1f-fields-e. Not a new dep
  — `crd-sdf` is already on the v1d-mesh path.

## References

**Research:** [`docs/research/cerid-eylem-fields.md`](../research/cerid-eylem-fields.md)
— full industry survey across PhysX, Bullet, Havok, Jolt, Unreal Chaos,
Unity DOTS, Godot, Box2D, Houdini, FleX, Flow, Niagara, Cascade, Maya
nDynamics, Blender; algorithm references (Bridson curl-noise, Stam
fluids, Barnes-Hut, FMM, trilinear sampling); determinism failure modes;
performance benchmarks.

**Companion ADRs:**
- [ADR-0050](0050-scene-storage-backends.md) — sparse-set storage rationale
- [ADR-0058](0058-obek-system.md) — öbek prefab system
- [ADR-0062](0062-eylem-physics-architecture.md) §3 (broadphase reuse), §4.5 (collider model), §6 (ECS integration)
- [ADR-0063](../decisions/0063-eylem-determinism-contract.md) — determinism contract
- [ADR-0064](0064-sdf-substrate-architecture.md) — `crd-sdf` substrate
- [ADR-0066](0066-draw-substrate-architecture.md) §11–12 — VisualizerRegistry pattern (d3)
