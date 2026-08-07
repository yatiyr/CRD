# Cerid Eylem — Force-Field Architecture Research

> **Outcome:** **adopted** — ADR-0067 (three-tier force-field substrate); shipped through v1b scope, rest resumes v1c+. *(stamped 2026-08-07, doc-hygiene pass)*

> **Companion to** [`cerid-eylem.md`](cerid-eylem.md). That file backed
> ADR-0062 (architecture) + ADR-0063 (determinism contract). This file
> backs **ADR-0067 (eylem force-field architecture)** and the
> Phase 4 force-field slice plan.
>
> **Date:** 2026-05-11. **Audience:** anyone working on `crd-eylem`'s
> field substrate who needs the *why* behind an architecture choice. The
> *what* is in ADR-0067 and the phase plan.
>
> **Scope:** ambient/regional forces applied to bodies (and later
> particles) — gravity overrides, wind, drag, vortex, attractor,
> turbulence, baked vector grids, scripted formulas. NOT contact forces,
> joint constraints, or controller inputs — those have their own ADRs.

---

## 1. Why force fields are a substrate, not a component

Every shipped engine surveyed below grew an *ad-hoc* force-field surface:
a `RadialForceComponent` here, a Niagara `Wind Force` module there, a
custom `btActionInterface` subclass somewhere else, an `Area3D` gravity
override in a fourth corner. Five years later the engine has six
incompatible "apply force in a region" code paths, three different
falloff conventions, two different trigger semantics, and a designer
asks "why does my magnet field affect rigids but not cloth?" because the
cloth solver only knows about the cloth-specific wind grid.

Cerid's mandate is broader than any single shipped engine: rigid 3D +
rigid 2D + soft + cloth + rope + articulations + vehicles + FEM + GPU
particles + acoustic occlusion, sharing one world. Six parallel field
implementations would dominate the bug budget. Eylem ships **one** field
substrate that every consumer samples identically.

This dossier answers: (1) what field types real engines ship, (2) how
they compose when N overlap a body, (3) how they reconcile with the
determinism contract, (4) what spatial dispatch is right, (5) what
authorability surface designers need, (6) what failure modes have shown
up in production.

---

## 2. Industry survey

### 2.1 PhysX 5

| Aspect | Detail |
|---|---|
| Field-specific API | **None.** PhysX has no "force field" type. |
| Force application | `PxRigidBody::addForce(vec, mode, autowake)`, modes: `eFORCE`, `eIMPULSE`, `eACCELERATION`, `eVELOCITY_CHANGE` ([docs](https://nvidia-omniverse.github.io/PhysX/physx/5.3.1/_api_build/class_px_rigid_body.html)) |
| Dispatch | User-driven. Trigger volume reports `onTrigger` overlap pairs via `PxSimulationEventCallback`; user iterates and calls `addForce` ([docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/RigidBodyCollision.html)) |
| Determinism | None — PhysX is not cross-platform deterministic at all (O3DE explicitly disables determinism in its PhysX integration) |
| Designer surface | Programmer's job entirely. Editor knows nothing about fields. |
| Failure modes | Triggers carry a 65,535-interaction-per-actor cap; overflow is silently dropped with an error log. A field volume containing thousands of bodies hits the cap fast. |

Takeaway: PhysX gives you a *primitive* (apply force) and a *report*
(overlap callback). The semantics of "field" — falloff, formula,
composition — are entirely the application's problem.

### 2.2 Bullet 3, Havok, Jolt

All three follow PhysX's minimalism. Bullet's
[`btActionInterface`](https://pybullet.org/Bullet/BulletFull/classbtActionInterface.html)
is `updateAction(world, dt)` + `debugDraw` — a custom field subclasses
it, queries the world for bodies in its region, calls
`btRigidBody::applyForce` per body. The canonical Bullet pattern for
radial gravity is to derive `btDiscreteDynamicsWorld` and override
`applyGravity()`
([forum](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=12415));
buoyancy is "attach floats, compute submerged volume, apply Archimedes"
([forum](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=8307)).
Havok's `hkpAction` mirrors this; `hkpSpringAction` is the canonical
shipped example. Jolt exposes
[`BodyInterface::AddForce` / `AddTorque`](https://jrouwe.github.io/JoltPhysics/class_body_interface.html)
plus a per-body `SetGravityFactor(float)` scalar — no region component;
user iterates the broadphase.

Common takeaway: the action interface is the universal pattern, but
designers cannot author fields without code.

### 2.3 Unreal Engine

Unreal carries **three generations** of field code simultaneously, which
is itself a lesson:

| Generation | API | Status |
|---|---|---|
| Cascade attractors | `Point/Line/Particle Attractor`, `Point Gravity`, `Orbit` per-emitter modules ([docs](https://docs.unrealengine.com/en-US/Engine/Rendering/ParticleSystems/Reference/Modules/Attractor/index.html)) | Deprecated |
| Built-in actor components | `URadialForceComponent` (one-shot impulse), `UPhysicsThrusterComponent` (continuous along -X), `WindDirectionalSource` (foliage+cloth+Niagara) ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-components-in-unreal-engine)) | Live |
| **Chaos Field System** | Blueprint node graph → Transient/Construction/Persistent fields applied to Chaos rigids + geometry collections ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-fields-user-guide-in-unreal-engine)) | Canonical for new code |

The Chaos Field System is the most ambitious shipped surface in any
mainstream engine. A Chaos field is a DAG of *field nodes*: leaf nodes
generate values (`RadialFalloff`, `PlaneFalloff`, `BoxFalloff`,
`RadialVector`, `UniformVector`, `NoiseField`); inner nodes combine
(scalar-scalar math, scalar-vector multiply, blend); the root feeds a
*target physics type* (Linear Force, External Strain, Linear Velocity,
Disabled Threshold). Three lifetimes — *Transient* (single-step
explosion), *Construction* (one-shot at scene init), *Persistent* (every
step) — cover the full design space.

Two design choices matter most for Cerid: (a) **per-target dispatch** —
the same field can target rigids, geometry collections, or "all", with
separate iteration sets but identical expression; (b) **no
designer-visible composition rule between fields** — multiple fields
on the same physics type sum additively; "disabled threshold" is
max-of. The composition rule is baked into the *target type*, not
exposed as a per-field knob. § 7 expands.

### 2.4 Unity DOTS Physics

ECS-native, stateless solver: a region field is just a *system* that
queries bodies whose `LocalToWorld.Position` is inside its volume and
mutates their `PhysicsVelocity`
([docs](https://docs.unity3d.com/Packages/com.unity.physics@1.2/manual/attract-body.html)).
Per-body `PhysicsGravityFactor` scales world gravity. No "field
component" ships with the package — the manual shows an `AttractSystem`
example as the canonical recipe. Takeaway: ECS-native engines express
fields as *systems over spatial queries*. That's the shape Cerid lands
on.

### 2.5 Godot 4

`Area3D` is the only mainstream "region" object that exposes
composition order to designers as a first-class concept
([docs](https://docs.godotengine.org/en/stable/classes/class_area3d.html)).
`gravity_space_override` has five values: `DISABLED`, `COMBINE`,
`COMBINE_REPLACE`, `REPLACE`, `REPLACE_COMBINE`. Wind: `wind_force_magnitude`,
`wind_attenuation_factor` (exponential decay), `wind_source_path`
(Node3D defines direction). The Replace/Combine vocabulary is exactly
what you reach for once a vehicle drives into a vortex tunnel and shoots
straight up.

### 2.6 Box2D v3

`b2Body_SetGravityScale(body, scalar)` (default 1, 0 disables, -1
reverses) is per-body, not per-volume
([docs](https://box2d.org/documentation/group__body.html)). No region
field — gameplay's job. Relevant pattern: gravity scale is a *body
attribute*; the "am I in the field?" question is answered upstream and
collapses to a per-body scalar.

### 2.7 Houdini POP / DOP

The most expressive field substrate ever shipped: every force is a node
in the DOP graph; the solver runs each enabled node per substep. Shipped
catalogue:
[POP Wind](https://www.sidefx.com/docs/houdini/nodes/dop/popwind.html)
(directional + swirl + pulse),
[POP Drag](https://www.sidefx.com/docs/houdini/nodes/dop/popdrag.html) /
[Drag Spin](https://www.sidefx.com/docs/houdini/nodes/dop/popdragspin.html),
[POP Axis Force](https://www.sidefx.com/docs/houdini/nodes/dop/popaxisforce.html),
[POP Attract](https://www.sidefx.com/docs/houdini/nodes/dop/popattract.html),
[POP Force](https://www.sidefx.com/docs/houdini/nodes/dop/popforce.html)
(uniform),
[Wind Force](https://www.sidefx.com/docs/houdini/nodes/dop/windforce.html)
(rigids + cloth),
[Vortex Force](https://www.sidefx.com/docs/houdini/nodes/dop/vortexforce.html),
[Drag Force](https://www.sidefx.com/docs/houdini/nodes/dop/drag.html)
(quadratic),
[Magnet Force](https://www.sidefx.com/docs/houdini/nodes/dop/magnetforce.html)
(metaball-defined volume),
[Field Force](https://www.sidefx.com/docs/houdini/nodes/dop/fieldforce.html)
(sample an arbitrary Houdini volume),
[Gas Curve Force](https://www.sidefx.com/docs/houdini/nodes/dop/gascurveforce.html)
(force along a curve — tornado spine, river flow),
[POP VOP / Wrangle](https://www.sidefx.com/docs/houdini/nodes/dop/popwrangle.html)
(arbitrary VEX — the escape hatch).

Composition: linear sum by default (forces accumulate on the `force`
attribute); Wrangle lets a TD override per-particle. Every node
optionally takes a "group" string for filtering.

Takeaway: Houdini's catalogue is the *upper bound* of what designers
ask for. Nine-tenths reduces to {Directional, Radial, Vortex, Drag,
Noise, GridSample, Script}; the rest is what drives TDs to VEX. Cerid
needs both the catalogue and the escape hatch.

### 2.8 NVIDIA FleX, NVIDIA Flow, Niagara, Cascade

**FleX** exposed `flexExtSetForceFields` — position + radius + strength
+ linear-falloff toggle, dispatched as a CUDA kernel across all particles
([manual](https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/physx/flex/manual.html)).
This is the canonical *GPU mirror* shape eylem v8 inherits — a flat
buffer of field descriptors, one compute kernel that iterates particles
and accumulates force.

**Flow** ([Omniverse docs](https://nvidia-omniverse.github.io/PhysX/flow/index.html))
is sparse-voxel Stable-Fluids fluid + combustion on a 3D grid; relevant
because it's the reference for "vector grid as field source" —
trilinear-sample the velocity volume to be pushed by fluid. CRDR
vector-grid asset → runtime sample is the eylem Tier-2 shape.

**Niagara** layers three field kinds
([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/particle-update-group-reference-for-niagara-effects-in-unreal-engine)):
Wind Force modules pull from a global `WindDirectionalSource` actor
([setup](https://yelzkizi.org/wind-in-unreal-engine-5-winddirectionalsource-foliage-wind-niagara-forces-cloth-and-groom-hair-setup/));
Vector Field modules sample a baked 3D field asset (FGA, usually
Houdini-authored)
([docs](https://docs.unrealengine.com/en-US/Engine/Rendering/ParticleSystems/Reference/Modules/VectorField/index.html));
Curl Noise Force does fast turbulence. The "Apply Vector Field" advanced
options (scale, transform, fade-by-distance) are the parameter set
every grid consumer eventually grows.

Published benchmark: 5 systems × 3 emitters × ~300 particles (≈ 4.5 k
particles) with several force modules → 6.2 ms tick
([Epic profiling tutorial](https://dev.epicgames.com/community/learning/tutorials/0qPO/unreal-engine-optimizing-niagara-measuring-performance)).
Per-emitter overhead matters (1 emitter × 1k beats 10 emitters × 100).

**Cascade** (legacy) parametrised force per *emitter* —
`Acceleration`, `Drag`, `Gravity Scale` per emitter, no sharing. The
lesson absorbed everywhere since: fields are world-space objects, not
per-emitter parameters. Cerid skips this generation.

### 2.9 Maya nDynamics + Blender

Maya ships the textbook taxonomy
([docs](https://download.autodesk.com/global/docs/maya2013/en_us/files/GUID-03923C56-9612-4C27-834E-2F417FC07323.htm)):
**Air** (directional wind matching velocity), **Drag**, **Gravity**,
**Newton** (inverse-square attraction), **Radial**, **Turbulence**
(4D noise), **Uniform**, **Vortex**, **Volume axis**. All connect to
the *Nucleus* solver that drives nCloth, nParticles, nHair through one
shared substrate. Composition is linear sum.

Blender's catalogue
([docs](https://docs.blender.org/manual/en/latest/physics/forces/force_fields/introduction.html))
is broader: Force, Wind, Vortex, Magnetic, Harmonic, Charge,
Lennard-Jones, Texture, Curve guide, Boid, Turbulence, Drag, Fluid flow.
Several are domain-specific (Boid = flocking, Lennard-Jones = molecular
dynamics, Charge = Coulomb) but their shipped existence is informative
— the world *does* contain users wanting Coulomb and harmonic
oscillators. Per-domain compatibility is tagged ("soft bodies react only
to Force, Wind, Vortex"). Falloff shapes: Sphere, Tube, Cone — the
same three every production engine eventually ships.

---

## 3. Algorithms — the canonical field formulas

All equations are per-body, per-substep (input: body position, velocity,
mass; output: force).

| Field | Formula | Notes |
|---|---|---|
| Directional / Uniform | `f = F` (or `m·a`) | Ambient wind, custom gravity. Cheapest possible. |
| Radial | `f = k · sign · r̂ / (r + r_min)^p` | `p=2` Newton/Coulomb; `p=1` Hooke (with `sign=-1` for spring). Clamping `r ≥ r_min` is mandatory — without it, two particles passing through the origin produce NaN. |
| Vortex | `f = ω × (p - axis)` | `ω` direction = axis, magnitude = swirl. Variants: pure tangent, helical (+ axial component). |
| Drag (linear) | `f = -k · v` | Stokes, low Reynolds. *Order-dependent* — reads `v` mutated by earlier fields. |
| Drag (quadratic) | `f = -k · \|v\| · v` | Newtonian, high Reynolds. |
| Magnetic (Lorentz) | `f = q · v × B` | Force perpendicular to motion — a `Drag` node cannot express this. |
| Coulomb | `f = k · q₁q₂ / r² · r̂` | Same shape as Newton with a sign — collapses into `Radial` with `polarity`. |

### Noise / turbulent wind

**Perlin** ([Perlin 1985](https://en.wikipedia.org/wiki/Perlin_noise))
sampled three times gives a vector field — cheap (~30 ops scalar, ~50
Simplex), bit-exact when implemented as a fixed permutation table +
integer hash. NOT divergence-free — particles drift toward sinks.

**Curl noise**
([Bridson, Hourihan, Nordenstam SIGGRAPH 2007](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf))
takes `v = ∇ × A` for `A` made of three Perlin samples. Divergence-free,
so particles follow turbulent streamlines without bunching. The boundary-
modulating form respects solid surfaces (modulate `A` to zero at the
boundary; curl becomes tangent).

**Determinism subtlety.** Bridson's analytic-derivative form
(`∂A_z/∂y − ∂A_y/∂z`) is bit-exact because it's algebraic. The "fast"
finite-difference form (`(A(x+h) − A(x−h)) / (2h)`) is *not* — the
`1/(2h)` reciprocal varies across builds and the subtraction order is
FP-sensitive. eylem uses the Simplex analytic-derivative variant
([Dziewanowski notes](https://emildziewanowski.com/curl-noise/)); the
first derivative costs roughly the same as the noise itself.
[*Improving Curl Noise* (SIGGRAPH Asia 2025)](https://dl.acm.org/doi/10.1145/3757377.3763980)
documents the divergence-bias artifacts of the finite-difference path
that motivate this choice.

### Custom vector grid (Stable Fluids tier)

3D grid of velocity vectors, sampled with trilinear interpolation
([Wikipedia](https://en.wikipedia.org/wiki/Trilinear_interpolation)) — 8
reads, 7 lerps, ~40 ops total. Higher-order alternatives:

| Sampling | Cost | Accuracy |
|---|---|---|
| Trilinear | 8 reads | C⁰, O(h²) |
| Tricubic Catmull-Rom | 64 reads + cubic weights | C¹, O(h⁴) |
| B-spline cubic | 64 reads, more arithmetic | C², O(h⁴), smoothed (lossy near sharp features) |

GPUs provide 1-tap hardware trilinear via 3D textures; CPUs pay all 8
reads. Trilinear is the right default — the source is already noisy,
8× cheaper, and the C⁰ creases at cell boundaries are below the
typical field gradient. Tricubic only for slow, smooth flows.

Grid sources: **authored** (Houdini paint, baked to FGA/CRDR),
**procedural** (curl-noise evaluated once per cell at bake), or
**live-baked** (Stable-Fluids semi-Lagrangian advection
[Stam 1999](https://pages.cs.wisc.edu/~chaol/data/cs777/stam-stable_fluids.pdf)
— eylem v8+ when GPU compute lands).

### Barnes-Hut for N-body gravity

When N point sources mutually attract (planetary system, plasma),
brute-force `O(n²)` is prohibitive past ~1000.
[Barnes-Hut](https://en.wikipedia.org/wiki/Barnes%E2%80%93Hut_simulation)
octree-grouping costs `O(n log n)`; FMM further reduces to `O(n)` at
higher constant cost
([Yokota, FMM with CUDA](https://arxiv.org/pdf/1010.1482)). Eylem v1
of fields does NOT ship this — the use case is narrow and the
substrate (per-source octree, multipole expansion) is orthogonal to
region fields. Reserve as a Phase 4.x optional node.

---

## 4. Determinism considerations specific to fields

ADR-0063 covers the substrate. Fields add three new failure modes.

**1. Summation order when N fields apply to one body.** FP addition is
commutative but not associative; if body B is in F₁, F₂, F₃, the order
of accumulation perturbs the last-bit sum. Fix: every field carries a
stable `FieldId`; per-body accumulation iterates ascending. Parallel
evaluation must use ADR-0063 §4's pre-reserved slot pattern (no atomic
counters), with the final merge walking slots in id order.

**2. Velocity-dependent fields (drag, magnetic).** Drag reads `v`; the
result depends on which earlier field mutated it. Two reproducible
choices exist: (a) evaluate all fields against substep-start `v`, then
sum; (b) sequential apply, each field reads live `v`. Eylem locks (a)
for parallelism and authoring locality — adding/removing a field never
perturbs an unrelated body until the next substep. Escape hatch: a
per-field `apply_after = [FieldId, ...]` declares dependencies that the
field scheduler topo-sorts into waves. Default empty deps = parallel.

**3. Noise function determinism.** Per ADR-0063, eylem noise lives in
`crd::math::deterministic::noise` with a fixed 256-entry permutation
table (duplicated to 512), integer hash, FP arithmetic limited to
`+ - *` (no transcendentals, no division). Analytic-derivative Simplex
is bit-exact across MSVC / clang / gcc × x64 / ARM, covered by the v9b
CI matrix. Reject any "fast curl noise" PR using finite differences
with `1/(2h)` division.

The "which fields overlap which bodies" query is deterministic by id —
stable order in, stable order out. The merge sorts overlap pairs by
`(body_id, field_id)` before delivering them to the per-body
accumulator.

---

## 5. Spatial dispatch — answering "which fields overlap which bodies"

Three approaches in the wild:

| Approach | Engines | Pros | Cons |
|---|---|---|---|
| User callback per field | PhysX, Bullet, Havok | Zero engine cost; field can do anything | Designer-hostile; one slow callback stalls the world; no cross-cutting "which fields apply" view |
| Built-in iteration over all bodies × all fields | Cascade, Maya nDynamics | Trivial to implement | O(N · M); breaks at >100 fields × 1k bodies |
| Acceleration structure (BVH / hash grid / kd-tree) | Chaos (uses Chaos broadphase), Niagara (per-emitter sampling against a global grid), Houdini (per-node group filters) | O((N + M) log) typical; designer surface clean | Adds maintenance cost; rebuild scheduling matters |

**Eylem's choice:** reuse the existing eylem rigid-body dynamic AABB
tree (ADR-0062 §3) as the spatial index. Each field carries an AABB
(its volume); the broadphase is queried once per step for "fields that
overlap each body's AABB". Cost amortises with the body broadphase
(both walks share cache lines). Empty-volume fields (Directional with
"world-wide" semantics) bypass the query and apply to every active
body.

For very large field counts (> 1k, e.g., a galaxy of point gravities), a
secondary BVH over the field set itself is appropriate. The field-set
BVH rebuilds incrementally on field add/remove/move, in the same job
graph slot as the body tree. Don't over-engineer this — the thresholds
where the secondary BVH wins are well above the v1 target workload.

**Comparison with spatial hashing.** Spatial hashes win on uniform
distributions of small AABBs (e.g., self-collision among soft-body
particles); BVH wins on heterogeneous AABB sizes — which is exactly
the field workload (a tiny vortex next to a world-spanning wind)
([forum](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=10172)). BVH
is the right pick.

---

## 6. Performance targets observed in the wild

Hard published numbers are sparse — most engines profile internally.
What's documented:

| Engine | Workload | Time | Source |
|---|---|---|---|
| Niagara | 5 systems × 3 emitters × 300 particles + force modules | 6.2 ms tick | [Epic tutorial](https://dev.epicgames.com/community/learning/tutorials/0qPO/unreal-engine-optimizing-niagara-measuring-performance) |
| PhysX 5 | Trigger interaction cap | 65,535/actor before silent overflow | [PhysX 5.4 docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/RigidBodyCollision.html) |

Published physics benchmarks measure stacking, ragdolls, solvers — not
field application, because that side is engine-specific. **Cerid's
targets** (synthesised from the survey + known eylem substrate cost):

| Workload | Budget | Notes |
|---|---|---|
| 1 directional field × 10k bodies | < 0.1 ms | Bypasses spatial query; SIMD over body pool |
| 100 analytic fields × 1k bodies | < 1.0 ms | BVH overlap query + accumulate |
| 10 grid-sampled fields (64³) × 1k bodies | < 0.5 ms | 8-tap trilinear per body × field; SIMD-friendly |
| 1 curl-noise field × 10k particles | < 1.5 ms | Analytic-derivative Simplex × 3 axes per particle |
| Field set rebuild (100 fields, 10 dirty) | < 0.05 ms | Incremental BVH update |

These are 8-lane SIMD on a modern x64 (Zen 4 / Raptor Lake). NEON 4-lane
on M-series scales linearly to ~2× the budget. GPU compute (eylem v8)
should hit 10× these for the grid-sample and curl-noise rows; analytic
fields stay CPU-bound because their per-source cost is too low to
justify dispatch overhead.

---

## 7. Composition rules — the part designers care about

Three observable patterns in the survey:

**Pattern A: implicit linear sum.** PhysX, Bullet, Niagara, Maya,
Blender, Houdini (default), Chaos Field default for Linear Force.
Composition is `f = Σ f_i`. Designers must mentally add. Fails when
fields semantically mean "override" (a teleporter zone wants to *replace*
gravity, not add to it).

**Pattern B: per-body scalar override.** Box2D's `gravity_scale`,
Jolt's `SetGravityFactor`, Unity's `PhysicsGravityFactor`. Per-body, not
per-region; compositional only by knobs the gameplay code sets. Works
well for global "this body floats" but doesn't compose with regions.

**Pattern C: explicit composition mode (Replace/Combine/etc).** Godot
4's `gravity_space_override`. Five modes, designer-visible. Most
expressive; most cognitive load.

**Eylem's choice:** *additive by default, with explicit overrides when
the field declares them.* Each field carries a `CompositionMode` enum:

```
Add        f_total += f_field             (default; matches Houdini, Niagara, Maya)
Replace    f_total = f_field              (last-applied wins; for "replace gravity" zones)
Multiply   f_total *= f_field             (for damping multipliers)
Max        f_total = max(f_total, f_field)  (for "deepest wind wins")
Min        f_total = min(f_total, f_field)  (rare; for resistance caps)
```

Composition still requires id-stable iteration order for determinism;
"Replace" semantically means "replace whatever lower-id fields wrote so
far." Designers compose by ordering field priority. The five-mode
vocabulary is enough for every shipped use case in the survey without
Godot's split between Combine / CombineReplace / Replace / ReplaceCombine
(those four collapse to the simpler `Add` / `Replace` axis when
priority is explicit).

The default is `Add`. Designers reach for `Replace` only when they know
they need it — matching the principle that the common case must be the
zero-config case.

---

## 8. Mass coupling

Fields apply force, but the *meaning* of "force" varies:

| Mode | Formula | Use case |
|---|---|---|
| `Force` | `Δv = f / mass · dt` | Wind on a leaf vs a boulder behave differently — leaf accelerates more |
| `Acceleration` | `Δv = a · dt` (mass-independent) | Gravity-like fields where everything accelerates equally |
| `GravityStyle` | `Δv = g · gravity_factor · dt` (uses per-body gravity factor) | Per-body opt-in/out (matches Box2D/Jolt/Unity convention) |
| `Impulse` | `Δv = J / mass` (one-shot, dt-independent) | Explosions, hits — RadialForceComponent's mode |
| `VelocitySet` | `v = v_field` (clamps velocity to field value) | Conveyor belts, magic teleport zones, "match the wind" (Maya Air) |

Eylem ships all five. The dominant observation across engines: gravity
should be `GravityStyle` so that a per-body opt-out (`gravity_factor =
0`) cleanly disables it without the designer rewriting the field. Wind
is `Force` (mass-coupled, default). Custom gravity zones are
`Acceleration` (mass-independent). Maya's Air-field "match the air
velocity" semantic is `VelocitySet` with a configurable lerp rate
(`v ← lerp(v, v_field, k·dt)`). Explosions are `Impulse`.

---

## 9. Continuous vs trigger semantics

Two failure modes that need design attention:

**Continuous fields** (the default) apply each substep while the body
is in the volume. Cost: O(field × overlapping bodies) per substep.

**Trigger fields** apply once on enter, optionally once on exit. Cost:
O(enter/exit events) per substep — typically much smaller. The
RadialForceComponent's "fire and forget" impulse mode is the canonical
example. Without trigger semantics, gameplay code must remember which
bodies it has already kicked; with trigger semantics, the engine
remembers.

Eylem ships both as a per-field `Trigger` enum: `Continuous`,
`OnEnter`, `OnExit`, `OnEnterOnce` (fire-and-forget; the field
auto-disables after firing). The enter/exit transitions are derived
from the broadphase overlap delta — no extra spatial query.

---

## 10. Authorability — three tiers

Mapped to Cerid's existing infrastructure:

- **Tier 1 — analytic ECS components.** A `ForceFieldComponent` holds
  formula enum, parameters, composition mode, trigger mode, volume
  AABB, priority id. Authored as an Öbek (ADR-0058): drag a
  `WindZoneObek` into a scene, tweak parameters, ship.
- **Tier 2 — custom vector grid.** A `VectorGridResource` is a CRDR
  artifact (FourCC `'EFLD'`) holding a 3D voxel grid of `Vec3`. The
  cooker (`crd-cooker` hook) accepts `.vfield.toml` pointing at
  FGA / EXR / Houdini bgeo, bakes to CRDR. Runtime: `GridSample`
  formula consumes a `VectorGridResource` handle.
- **Tier 3 — scripted formula.** The reserved spatial DSL operators in
  `ScriptComponent` (Phase 3.0 v1o) get an
  `eval_field(p, v, m, t) -> Vec3` entry point. Engine compiles once,
  evaluates per-body per-substep. Fine for hundreds of bodies, not
  tens of thousands. Houdini POP Wrangle equivalent — escape hatch.

Most production use cases live in Tier 1.

---

## 11. Debug visualization

Standard "arrow field" pattern, identical across Maya / Houdini /
Blender / Niagara / Unity / Godot: build a stride-`s` 3D lattice over
the field's AABB, evaluate the formula at each point, draw an arrow
from `p` to `p + v · scale`, colour by `|v|`. Stride `s` is a
power-of-two fraction (`extent / N`, N = 8/16/32 via designer slider);
arrow `scale` auto-fits longest arrow to `s/2`.

Velocity-dependent fields (drag, magnetic) need a probe velocity —
convention is sample at `v = 0` and flag in the inspector. Eylem hooks
this into the existing `crd-draw` `VisualizerRegistry` plug-in pattern;
a `FieldVisualizer` plugin produces arrow-mesh primitives. Grid-sampled
fields bake a downsampled (4×/8×) LOD for fast preview rendering.

---

## 12. Production failure modes documented in the wild

Synthesised from forum threads, GDC talks (where indexed publicly), and
the survey above:

1. **Force fields stack into instability (NaN explosions).** Two radial
   fields with overlapping origins, `r_min = 0`, a body crosses through:
   `r → 0 → 1/r² → ∞`. PhysX's debug build asserts; the release build
   propagates NaN through the integrator and the body teleports. *Fix:
   clamp `r ≥ r_min`; default `r_min` to half the smallest collider
   radius in the scene.*
2. **Frame-rate-dependent drag.** Designer authors `f = -k·v` with `k`
   tuned at 60 Hz. Project ships at 30 Hz; drag is half-effective; cars
   feel ice-coated. *Fix: drag coefficient is in units of `1/s` and is
   integrated against `dt`. Eylem fields take a `coefficient_units` enum
   (`PerSecond`, `PerSubstep`, `Absolute`) so the integration semantics
   are explicit.*
3. **Compound vortex instability.** Two vortex fields rotating against
   each other near the same axis produce alternating-direction force on
   bodies in the overlap region; bodies oscillate violently in tight
   loops. *Fix: vortex forces clamp by a `max_tangential_velocity` cap
   that defaults to a fraction of `c · sqrt(g · r)` (terminal-velocity-
   class limit).*
4. **Designer confusion when fields don't sum as expected.** Reported
   on every Houdini/Niagara forum: "I have wind A and wind B, why is
   the result wind A * 2 not wind A + wind B?" — the answer is usually
   one field has `mass-coupling = Force` and the other `Acceleration`,
   producing different effective magnitudes. *Fix: every field's
   inspector shows the *effective* per-body acceleration in a probe
   panel, not just the raw force vector. Also: composition mode is
   shown front-and-center, not buried in advanced.*
5. **Trigger callbacks blow the budget.** PhysX trigger pairs cost ~1 µs
   each in the user callback. 10k pairs = 10 ms; the engine misses
   frame. *Fix: eylem's tier-1 fields are pure data + a formula tag —
   no callbacks, no script overhead. Tier-3 scripts pay the cost, but
   the budget pre-allocates and warns when exceeded.*
6. **Snapshot replay fails because field iteration order changed.** A
   designer adds a new field; the auto-assigned id permutes some sort;
   the snapshot hash diverges. *Fix: field ids are content-addressed
   from the field's authoring data (FNV-1a 64 over the öbek source) +
   a salt — adding a new field doesn't perturb existing ids. Same
   discipline as Öbek (ADR-0058 §4) and SceneId (ADR-0059).*
7. **Physics + cloth + particle solvers each apply wind separately.**
   Cascade-era Unreal: foliage shader read the wind global, cloth read
   the wind direction differently, particles ran their own wind module
   — three different "wind directions" simultaneously. *Fix: every
   eylem solver consumes the *same* field substrate. There is one
   `WindFieldComponent` per zone; rigid solver, XPBD solver, future
   particle solver, future audio occlusion all sample it identically.*

Each fix is a constraint that shaped §13's recommended architecture.

---

## 13. Recommended Cerid architecture (for ADR-0067)

Lock the choices below.

### 13.1 Three-tier model — confirmed

**Verdict: confirm the three-tier model.** Tier 1 (analytic) handles the
common case; Tier 2 (vector grid) handles authored or live-baked flow;
Tier 3 (script) is the escape hatch. Houdini's success proves the model;
the tiers map cleanly to Cerid's substrate (Tier 1 = ECS component,
Tier 2 = CRDR resource, Tier 3 = ScriptComponent). No combination of two
tiers covers Tier 3's escape-hatch role; no combination of two tiers
covers Tier 2's "designer paints a flow in Houdini" workflow.

### 13.2 Field formula enum (Tier 1)

```
enum class FieldFormula : u8
{
    Directional,    // f = direction · magnitude
    Radial,         // f = k · sign / (r + r_min)^p · r̂  (p covers Newton, Coulomb, Hooke)
    Vortex,         // f = ω × (p - axis)
    Drag,           // f = -k · v · |v|^(p-1)        (p = 1 linear, p = 2 quadratic)
    Noise,          // f = curl_noise(p, t, scale, octaves) — analytic-derivative Simplex
    Magnetic,       // f = q · v × B
    Gradient,       // f = ±k · ∇φ(p) sampled from a crd-sdf SdfResource
    GridSample,     // f = trilinear_sample(VectorGrid, p)
    Script,         // f = ScriptComponent::eval_field(...)
};
```

Coverage rationale: Directional + Radial(p=2) covers Maya's
Gravity/Newton/Radial; Radial(p=1, sign=-1) covers Harmonic; Drag
covers Air/Drag; Vortex covers Vortex; Noise covers Turbulence; Magnetic
covers Charge/Magnetic; **Gradient** covers Houdini's Field Force on a
scalar volume + Maya's Volume Axis (push away from / pull toward an
SDF surface with configurable falloff); GridSample covers vector-volume
sources (Niagara FGA, NVIDIA Flow); Script covers VOP / Wrangle. Nine
enum values, no missing semantic the survey turned up.

`Coulomb`, `Newton`, `Harmonic` are **not** distinct enum values —
each is `Radial` with the right `polarity`, `falloff`, and
`mass_coupling`. `Wind` is `Directional` (steady), `Noise` (turbulent),
or both composed. Fewer enums = cleaner cooker grammar.

**Gradient explicitly justified.** Cerid ships `crd-sdf` as a peer
substrate (Phase 3.1.5; CLAUDE.md). A `Gradient` field consuming an
`SdfResource` reuses the SDF infrastructure already in flight — the
field declares `sdf_handle + sign + scale`, the runtime samples
`closest_point + gradient` (already a hot path in `crd-sdf` for mesh
collision). This subsumes "push away from this collider", "pull toward
that ribbon", "respect this volumetric flow boundary" without any new
authoring tool — designers paint or import the SDF once, every
consumer (collision, Gradient field, audio occlusion) shares it. The
alternative (bake gradient into a `GridSample` vector grid) costs 3×
memory for the same data and divorces the field from the live SDF
authoring loop.

`Boid` and `Lennard-Jones` from Blender are **rejected**: domain-
specific (flocking, molecular dynamics) and easily expressible as a
`Script` field for the rare consumer.

### 13.3 Falloff models

```
enum class FieldFalloff : u8
{
    Constant,         // f stays at full magnitude inside volume
    Linear,           // f scales (1 - r / r_max) linearly to zero at boundary
    InverseLinear,    // f scales 1 / (r + r_min)
    InverseSquare,    // f scales 1 / (r + r_min)^2
    Smoothstep,       // f scales smoothstep(r_max, r_min, r) — C^1 continuous boundary
    Polynomial,       // f scales by user-tunable polynomial coeffs (escape hatch)
};
```

Smoothstep is the one production engines reach for after
"`Linear` looks bad at the boundary"; ship it from v1. Polynomial covers
the long tail (cubic, exponential decay) without adding more enum
values.

### 13.4 Mass coupling modes

Five modes per §8 — `Force`, `Acceleration`, `GravityStyle`, `Impulse`,
`VelocitySet`. Locked.

### 13.5 Composition rule

Per-field `CompositionMode`:
`Add` (default) | `Replace` | `Multiply` | `Max` | `Min`. Application
order is **id-stable** (composition by ascending `FieldId`). For
velocity-dependent ordering, optional `apply_after = [FieldId, ...]`
declares dependencies; the field scheduler topologically sorts and
applies in waves. Default empty deps = parallel evaluation.

### 13.6 Trigger semantics

Per-field `Trigger` enum: `Continuous` (default) | `OnEnter` | `OnExit`
| `OnEnterOnce` (auto-disables after one fire). Enter/exit transitions
derived from broadphase overlap delta — no extra spatial query.

### 13.7 Spatial dispatch

Reuse the eylem dynamic AABB tree (ADR-0062 §3) as the field index;
each field's volume AABB participates in the broadphase. World-spanning
fields (Directional with infinite volume) bypass the query. For very
large field counts (>1k), a secondary BVH over fields rebuilds
incrementally — defer until measured need.

### 13.8 ECS surface

```
ForceFieldComponent      sparse-set storage — one per field-bearing entity
   formula, parameters, falloff, composition_mode, trigger_mode,
   mass_coupling, priority_id (FNV-1a content-addressed),
   volume_aabb, optional VectorGridResource handle, optional Script handle

EylemFieldSystem         in PrePhysics phase — broadphase overlap query,
                          per-body field accumulation, hand off to integrator

EylemFieldVisualizerHook visualization plugin in crd-draw VisualizerRegistry
```

### 13.9 Determinism

Every field formula must be in the deterministic stdlib subset (per
ADR-0063). Specifically:
- `Noise` uses `crd::math::deterministic::noise::simplex_curl` — the
  analytic-derivative form, no finite differences, no division.
- Field iteration order is by ascending `FieldId`.
- Per-body force accumulator uses fixed-position writes per ADR-0063 §4
  (no atomic counters).
- Velocity-dependent fields (`Drag`, `Magnetic`) read substep-start
  velocity; a single optional dependency edge (`apply_after`) lets a
  field opt into reading post-update `v`.
- Field add/remove/move events go through the öbek serialisation path
  so that `FieldId` is content-addressed and stable across runs.

### 13.10 Performance budget targets

Per §6 Cerid targets table. Each line in the table becomes a benchmark
in `tests/eylem/bench_fields.cpp` checked against budget by the v9b CI
matrix.

### 13.11 Phase 4 slice plan implications (suggested)

The active slice plan lives in `docs/phases/phase-4-fields.md` once
written; this dossier proposes ordering for the ADR to reference.
Roughly:

| Slice | Scope |
|---|---|
| v1a | `ForceFieldComponent` + Tier 1: Directional, Radial, Drag |
| v1b | Tier 1: Vortex, Magnetic |
| v1c | Tier 1: Noise (deterministic curl-noise) |
| v1d | Composition modes (Add / Replace / Multiply / Max / Min); priority_id; trigger semantics |
| v1e | Tier 1: Gradient (consumes `SdfResource` from `crd-sdf`) |
| v1f | Tier 2: VectorGridResource + cooker hook + GridSample formula |
| v1g | Tier 3: Script formula via ScriptComponent integration |
| v1h | Visualizer hook in crd-draw |
| v1i | Bench suite + budget assertions in CI; sandbox demo |

Gradient lands after `crd-sdf` Phase 3.1.5 closes (the SDF substrate
must exist to consume). Each slice ships through the standard DoD
(six-config quality pass + all headless smokes + tests + docs).

---

## 14. References

### Engines and tools (primary docs)

- [PhysX 5.4 PxRigidBody](https://nvidia-omniverse.github.io/PhysX/physx/5.3.1/_api_build/class_px_rigid_body.html)
- [PhysX 5.4 Rigid Body Collision](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/RigidBodyCollision.html)
- [Bullet btActionInterface](https://pybullet.org/Bullet/BulletFull/classbtActionInterface.html)
- [Jolt BodyInterface](https://jrouwe.github.io/JoltPhysics/class_body_interface.html)
- [Jolt MotionProperties](https://jrouwe.github.io/JoltPhysics/class_motion_properties.html)
- [Box2D v3 Body group](https://box2d.org/documentation/group__body.html)
- [Unreal Chaos Fields User Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-fields-user-guide-in-unreal-engine)
- [Unreal Physics Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-components-in-unreal-engine)
- [Unreal Vector Field modules](https://docs.unrealengine.com/en-US/Engine/Rendering/ParticleSystems/Reference/Modules/VectorField/index.html)
- [Unreal Cascade Attractor modules](https://docs.unrealengine.com/en-US/Engine/Rendering/ParticleSystems/Reference/Modules/Attractor/index.html)
- [Unity Physics — Attracting bodies](https://docs.unity3d.com/Packages/com.unity.physics@1.2/manual/attract-body.html)
- [Godot Area3D](https://docs.godotengine.org/en/stable/classes/class_area3d.html)
- [Houdini POP Wind](https://www.sidefx.com/docs/houdini/nodes/dop/popwind.html) /
  [POP Drag](https://www.sidefx.com/docs/houdini/nodes/dop/popdrag.html) /
  [Vortex Force](https://www.sidefx.com/docs/houdini/nodes/dop/vortexforce.html) /
  [POP Force](https://www.sidefx.com/docs/houdini/nodes/dop/popforce.html) /
  [POP Attract](https://www.sidefx.com/docs/houdini/nodes/dop/popattract.html) /
  [Magnet Force](https://www.sidefx.com/docs/houdini/nodes/dop/magnetforce.html) /
  [Field Force](https://www.sidefx.com/docs/houdini/nodes/dop/fieldforce.html) /
  [POP Wrangle](https://www.sidefx.com/docs/houdini/nodes/dop/popwrangle.html)
- [NVIDIA FleX Manual](https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/physx/flex/manual.html)
- [NVIDIA Flow (Omniverse)](https://nvidia-omniverse.github.io/PhysX/flow/index.html)
- [Maya Nucleus + dynamic fields](https://download.autodesk.com/global/docs/maya2013/en_us/files/GUID-03923C56-9612-4C27-834E-2F417FC07323.htm)
- [Blender Force Fields manual](https://docs.blender.org/manual/en/latest/physics/forces/force_fields/introduction.html)
- [WindDirectionalSource setup notes](https://yelzkizi.org/wind-in-unreal-engine-5-winddirectionalsource-foliage-wind-niagara-forces-cloth-and-groom-hair-setup/)

### Algorithms and papers

- [Bridson, Hourihan, Nordenstam — Curl-Noise for Procedural Fluid Flow, SIGGRAPH 2007](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf)
- [Stam — Stable Fluids, SIGGRAPH 1999](https://pages.cs.wisc.edu/~chaol/data/cs777/stam-stable_fluids.pdf)
- [Improving Curl Noise, SIGGRAPH Asia 2025](https://dl.acm.org/doi/10.1145/3757377.3763980)
- [Differentiable Curl-Noise, ACM CGIT 6(1) 2023](https://dl.acm.org/doi/10.1145/3585511)
- [Dziewanowski — Dissecting Curl Noise (notes)](https://emildziewanowski.com/curl-noise/)
- [Barnes-Hut algorithm — Wikipedia](https://en.wikipedia.org/wiki/Barnes%E2%80%93Hut_simulation)
- [Yokota — Treecode and FMM with CUDA](https://arxiv.org/pdf/1010.1482)
- [Trilinear interpolation — Wikipedia](https://en.wikipedia.org/wiki/Trilinear_interpolation)
- [Kahan summation — Wikipedia](https://en.wikipedia.org/wiki/Kahan_summation_algorithm)

### Performance

- [Niagara performance measurement tutorial](https://dev.epicgames.com/community/learning/tutorials/0qPO/unreal-engine-optimizing-niagara-measuring-performance)
- [Measuring Performance in Niagara](https://dev.epicgames.com/documentation/en-us/unreal-engine/measuring-performance-in-niagara)
- [BVH vs Spatial Hash — Bullet forum](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=10172)

### Determinism

- [Box2D Determinism — Catto 2024](https://box2d.org/posts/2024/08/determinism/)
- [Rapier `enhanced-determinism`](https://rapier.rs/docs/user_guides/rust/determinism/)

---

**This file is the source of truth for the *why* behind eylem fields.**
Decisions locked here flow into ADR-0067 (architecture) and the Phase 4
slice plan.
