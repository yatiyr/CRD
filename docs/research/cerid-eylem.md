# Cerid Eylem — Physics Industry Research

> **Outcome:** **adopted** — ADR-0062/0063; eylem v0–v1b shipped, then ⏸ paused (2026-05-11); this dossier remains the v1c+ plan. *(stamped 2026-08-07, doc-hygiene pass)*

> **Eylem** (Turkish: *eylem* — "action" / "motion") is the Cerid-native physics
> module. Built from day one rather than wrapping PhysX or Jolt. This file
> captures the deep research pass that informed ADRs 0062 (architecture) and
> 0063 (determinism contract) plus the Phase 3.1 phase plan
> (`docs/phases/phase-3.1-eylem.md`).
>
> **Date:** 2026-05-10. **Audience:** anyone working on `crd-eylem` who needs
> the *why* behind an architecture choice — not the *what*. The *what* is in
> the ADRs and the phase plan.

---

## 1. The engine landscape

### AAA / proprietary

- **Frostbite (DICE/EA)** — hand-rolled in-engine physics; the publicly
  documented pieces are destruction (Bad Company / Battlefield) and the
  contact-solver / multithreading work shown in the GDC course "High
  Performance Physics Solver Design for Next Generation Consoles".
  Architecture: task-graph + island-parallel sequential impulses, vehicles +
  cloth + large-scale networked destruction integrated into the gameplay
  layer rather than the physics core. Distinguishing pattern: physics as
  *jobs the renderer/streaming/audio also produce work for*, no global lock
  per step. Most details are paywalled; the public architectural pattern is
  what we copy.

- **Havok 2024 (Microsoft, since 2015)** — reference closed-source AAA
  engine. PGS-style sequential impulses with a shape graph (broadphase =
  balanced AABB tree + persistent SAP overlay), island-parallel solve,
  world-snapshot serialization, deterministic mode with platform-specific
  math libraries. Powers Halo, FromSoftware, several Nintendo titles.
  Distinguishing choice: the "rigid body world snapshot" object
  (saveable/replayable) is a first-class API, not bolted on. Eylem ships a
  comparable snapshot/replay surface in v1j.

- **Chaos (Unreal 5, default since 5.0)** — replaced PhysX as UE default.
  Modular: Chaos Rigid (sequential impulses + persistent contact), Chaos
  Cloth (XPBD), Chaos Flesh (FEM tetrahedral, hyperelastic — UE 5.4+),
  Chaos Vehicles (raycast + analytic suspension), Chaos Destruction
  (geometry collection + cluster fields → Niagara). Distinguishing choice:
  every domain (cloth, flesh, destruction, fluid) is a separately
  shippable plugin sharing only the `IPhysicsScene` and `FBodyInstance`
  contracts, so games strip what they don't use. Eylem mirrors this with
  the `eylem-rigid3d` / `eylem-soft` / `eylem-articulation` / `eylem-vehicles` /
  `eylem-fem` / `eylem-gpu` sub-module split.

- **Unity Physics + Havok (DOTS)** — two backends behind one ECS-native
  API. Unity Physics is **stateless** (recomputes contact graph each
  tick — burst+SIMD compensates) → easily networkable / rollbackable.
  Havok backend is stateful — caches islands, contact graph, sleep — ~2×
  faster on cluttered scenes. Same `PhysicsWorld` component data flows
  between them. Distinguishing choice: stateless variant exists *because*
  of rollback netcode requirements. Cerid stays stateful with deterministic
  snapshot — rollback becomes "snapshot + replay inputs" rather than
  "stateless replay".

### Open-source production

- **PhysX 5 (NVIDIA, BSD-3 since 2022)** — two solvers shipped: PGS
  (legacy) and **TGS** (Temporal Gauss-Seidel, default for high-end use
  since 5.1). TGS subdivides the timestep into N substeps (= position
  iterations); each substep solves all constraints once with already-
  integrated positions, giving stiffness ≈ N× more iterations at ≈ 1×
  the cost. Defaults: 4 position iterations / 1 velocity iteration on TGS.
  Articulations: Featherstone reduced-coordinate ABA
  (`PxArticulationReducedCoordinate`) and a maximal-coord constraint
  variant; both shipped. GPU rigid bodies, GPU particles (PBD-based,
  descendant of Flex), Vehicle SDK with raycast + sweep suspension and
  per-tire friction-pair tables. Cooking: offline mesh/heightfield bake
  to GPU-friendly layouts. Eylem chooses SI for v1 (most documented +
  cleanest fit to fixed iteration budget) with TGS as v2 upgrade (the
  substep loop wraps the existing solver — cheap migration).

- **Jolt Physics (Jorrit Rouwé, MIT, ships in Horizon Forbidden West +
  Death Stranding 2)** — the closest peer to where Eylem aims to land.
  Sequential Impulses with warm starting; **lock-free quad-tree
  broadphase** (4 children per node, SIMD-friendly; nodes "expanded"
  lock-free during step, tight tree rebuilt in background and atomically
  swapped at frame end); **lock-free island detection** (atomic union-find
  over contact pairs); island-parallel solve. `BodyID` is a sequence-tagged
  handle; `BodyLockRead/Write` mutex-array hashes many bodies onto few
  mutexes. **`JPH_CROSS_PLATFORM_DETERMINISTIC`** (a) replaces `std::sin/cos
  /sort/hash/push_heap/pop_heap` with internal versions, (b) enforces FP
  rounding mode + denormal handling, (c) costs ~8% perf, (d) gives
  bitwise-identical results across MSVC / clang / gcc / emscripten on x86 /
  ARM / RISC-V / PowerPC / LoongArch and 32 vs 64-bit. Eylem bakes the
  same recipe in from v1 — without the retrofit tax.

- **Bullet 3 (Erwin Coumans, zlib, dormant since ~2021)** — sequential
  impulses + warm starting; `btDbvtBroadphase` = two dynamic AABB trees
  (static + dynamic); GJK + EPA narrow phase; soft body via tetrahedral
  linear FEM with stiffness warping (~30–150 iterations to converge), plus
  mass-spring; MLCP (Dantzig / Lemke / PGS) selectable for special use.
  Featherstone articulations exposed as `btMultiBody`. PyBullet wrap is
  what most physics-RL papers actually used. Reference codebase to read,
  not the codebase to build a 2026 engine on.

- **Box2D v3 (Erin Catto, MIT, August 2024)** — **full C rewrite** (no
  C++); new **Soft Step** solver (= TGS_Soft from the Solver2D study —
  substepping + soft constraints + warm starting); persistent islands;
  **graph-coloring SIMD** — constraints are colored so within one color a
  body appears at most once, then 4-wide (SSE2/NEON) or 8-wide (AVX2)
  blocks of constraints solve as one wide constraint with body state in
  32 B fitting one AVX register. Cross-platform deterministic floats:
  `-ffp-contract=off`, no fast-math, custom `atan2f`, bit-arrays merged
  via OR (commutative) instead of cross-thread atomics; verified on
  x64+ARM, MSVC+GCC+Clang, M2 ↔ Ryzen. **Box2D does not own threads** —
  application provides `b2EnqueueTaskCallback` / `b2FinishTaskCallback`,
  samples wire enkiTS. Reported ≥ 2× faster than v2.4. Eylem copies the
  AoSoA-8 + graph-coloring + no-threads patterns wholesale.

- **Chipmunk2D (Slembcke, MIT)** — 2D rigid bodies; Catto-style impulse
  solver; broadphase = bounding-box tree *or* spatial hash (both shipped);
  sleeping. Was Cocos2d's default. Smaller, simpler, less actively
  maintained than Box2D v3.

- **Rapier (Sébastien Crozet, Dimforge, Apache-2, Rust)** — single
  codebase, parameterised over Real (`f32`) and `Dim` for **rapier2d /
  rapier3d**. Sequential impulses + warm starting; SIMD via `packed_simd`;
  **`enhanced-determinism`** Cargo feature gives cross-platform
  determinism but is mutually exclusive with `simd-nightly`/`simd-stable`/
  `parallel`. Joints: revolute, prismatic, fixed, spherical, generic
  6-DOF. Continuous Collision Detection via sweep tests. Used in Bevy via
  `bevy_rapier`. Eylem follows Rapier's templated 2D/3D model rather than
  Box2D/Box3D's fork model — saves maintenance, costs ~10–20% perf in
  2D-only paths (acceptable; the math module is already template-friendly).

- **Newton Dynamics 4 (Julio Jerez, MIT)** — exact-solver lineage: PGS
  *and* a Dantzig-style direct LCP solver, intended for stable
  robotics-grade stacking. Less popular today; the name "Newton" is now
  confusable with NVIDIA's 2025 Newton.

### Robotics-grade

- **MuJoCo (DeepMind, Apache-2)** — soft-constraint contact (penalty +
  complementarity hybrid via convex optimization on contact forces),
  implicit Euler, generalized coordinates throughout. **MJX** = MuJoCo
  re-expressed in JAX/XLA primitives, JIT-compiled to GPU; 10–50× over
  MuJoCo on the same hardware when batched, dense matrix path preferred
  on GPU. Differentiable end-to-end (vjp/jvp) — used as the de-facto RL
  sim since 2023. Eylem v9 reserves a differentiable rigid-body path; MJX
  is the reference for *how* to expose differentiability.

- **Drake (TRI/MIT, BSD-3)** — multibody on top of Simbody-derived
  dynamics (Sherman). Headline feature: **hydroelastic contact** —
  contact between *compliant* bodies modelled as a precomputed pressure
  field over tetrahedral interior + intersection of pressure fields on
  the contact surface (instead of one-point-per-contact). Smooth force,
  no popping, robust to mesh tessellation. Eylem v7d is the eventual
  hydroelastic-contact slice — the right model for medical sim
  (compliant tissue) and robotics (gripper-on-soft-object).

- **DART (Georgia Tech / UW / OSRF, BSD-2)** — Featherstone reduced-coord
  articulations, exposes mass matrix / Coriolis / Jacobians as first-class.
  Used as a Gazebo backend; favored in research that needs to differentiate
  or insert custom controllers.

- **ODE (Russ Smith)** — historical reference. Quick-step (PGS) +
  worldstep (Dantzig direct). Replaced almost everywhere.

- **PyBullet** — Bullet behind a Python wrapper, used in OpenAI baselines
  pre-Isaac. Dying.

- **NVIDIA Newton (2025)** — different beast from "Newton Dynamics".
  GPU-accelerated robotics engine atop NVIDIA Warp; integrates **MuJoCo
  Warp** as default rigid solver, plus a Vertex Block Descent solver for
  cables/cloth/volumetric, hydroelastic contact, SDF collisions. Joint
  NVIDIA × DeepMind × Disney effort. Eylem v8 GPU plans take cues from
  Newton's compute-shader pipeline.

### Specialized / research

- **Genesis (Dec 2024, CMU + Stanford + MIT CSAIL + NVIDIA + Tsinghua)** —
  GPU-native multi-physics (rigid + MPM + cloth + fluids + tactile) in
  pure Python. Reports 43 M FPS / 430 000× real-time on RTX 4090 for some
  configs, "10–80× faster than Isaac Gym". MPM solver is differentiable;
  rigid differentiability is on the roadmap. Python first because the
  audience is RL researchers.

- **Brax (Google, JAX, Apache-2)** — massively parallel rigid simulation
  in JAX, fully differentiable, generalized-coords. Reaches hundreds of
  millions of steps/s on TPU/GPU with thousands of envs. Two solvers:
  "spring" (older), "generalized" (Featherstone). The thing that made
  GPU-resident RL credible.

- **DiffTaichi (Hu et al, ICLR 2020)** — differentiable programming
  language with source-to-source gradient transformation; lightweight
  tape; 4.2× shorter than hand CUDA at the same speed; 188× over
  TensorFlow on the same physics. Ten reference simulators ship.

- **Vivace (Fratarcangeli et al, SIGGRAPH 2016)** — parallel randomized
  Gauss-Seidel via graph coloring for soft bodies on GPU; the
  foundational paper for the GPU-XPBD lineage that became Vellum.

- **NVIDIA Flex / Houdini Vellum** — unified XPBD solver — rigid (XPBD),
  cloth, soft body, grains, ropes, fluids (XPBD-fluids w/ surface tension
  since H19) all sharing a single particle + constraint substrate.
  Vellum is the production version of the Flex idea, JIT-compiled inside
  Houdini's DOPs. Enables grains-deform-cloth-into-rigid in one solve.

- **Tiny Differentiable Simulator (Coumans)** — compact reference for
  differentiable rigid physics in C++.

- **Planck.js / Matter.js / p2.js** — web 2D adaptations. Planck.js is
  essentially Box2D v2 ported. Architecturally not interesting except
  for showing the API surface most 2D users actually want.

---

## 2. Algorithm catalogue

For each topic: canonical reference, complexity, typical real-time tuning,
which engines implement it.

### Broadphase

- **Sweep-and-Prune (SAP)** — O(n + k) with k overlaps; great if
  frame-to-frame coherence is high. Three sorted axis-projection lists,
  swap-and-fix on insertion. Persistent SAP keeps the sort across frames.
  Used by Box2D v2, Bullet `btAxisSweep3`, ODE.
- **Dynamic AABB tree (Dbvt / BVH)** — Catto's GDC 2019 *Dynamic Bounding
  Volume Hierarchies* is the canonical exposition. Insert is O(log n)
  amortised; AABB is "fattened" by a small margin so trees only re-fit
  when the tight AABB leaves the fat one. Used by Bullet `btDbvtBroadphase`,
  Box2D v2.4 + v3, Jolt (quad-tree variant). Outperforms SAP on dynamic
  worlds with many moving objects per Bullet forum benchmarks. **Eylem v1c
  uses this.**
- **Quad-tree BVH (Jolt)** — 4-way fan-out maps onto SIMD lane width 4;
  lock-free expansion + background tight-tree rebuild swap atomically.
- **Spatial hashing** — O(1) amortised query; choose cell ≈ avg shape size;
  used by Chipmunk2D, MuJoCo, many particle systems. Worse than BVH when
  shape sizes are heterogeneous.
- **GPU broadphase = LBVH** — Karras 2012, *Maximizing Parallelism in the
  Construction of BVHs, Octrees, and k-d Trees* (HPG). Sort primitives by
  Morton code, build the binary radix tree fully in parallel (every
  internal node knows its range from the longest common prefix). Karras
  2013 gives the parallel SAH version when quality matters. Bullet's
  `b3GpuParallelLinearBvh` is the OpenCL reference. **Eylem v8a uses
  this.**

### Narrow phase

- **GJK** (Gilbert, Johnson, Keerthi 1988) — linear in iterations;
  near-constant with simplex caching frame-to-frame. Works on any pair of
  convex shapes that supply a *support function* `s(d) = argmax(p·d)`.
  Returns distance + closest-point pair in the disjoint case. Reference:
  Catto 2010, *Computing Distance*, GDC.
- **EPA** (Expanding Polytope Algorithm) — extends GJK into the
  penetrating case; iteratively expands the simplex toward the
  Minkowski-difference origin to recover penetration depth + normal.
  Slower and shakier than GJK; numerical conditioning is the usual
  failure mode.
- **MPR** (Minkowski Portal Refinement, Snethen, *Game Programming Gems
  7*, 2008 — XenoCollide source) — like GJK uses support functions; finds
  a "portal" tetrahedron containing the origin and refines. Doesn't
  compute distance for separated shapes (boolean only) but is simpler,
  often more numerically robust, and natively handles translational
  sweeping. Crystal Dynamics shipped it in *Tomb Raider: Underworld*.
- **SAT** (Separating Axis Theorem) — for boxes / hulls with known face +
  edge axes (n_face + n_face + n_edge×n_edge candidate axes for two hulls);
  cheap when axis count is small (boxes: 15 axes); doesn't extend to
  curved shapes. Box2D's hull-hull is SAT.
- **Polytope clipping (Sutherland-Hodgman)** — once SAT picks a reference
  face and an incident face, clip incident against side-planes of
  reference, keep penetrating points → up to 4 contact points for a
  box-box manifold. Catto's blog "Contact Manifolds" + Box2D source.
- **CCD: conservative advancement** (Mirtich 1996) — for a pair of convex
  shapes compute closest distance d, max relative speed v⊥ along closest-
  point normal → safe step Δt ≤ d / v⊥; advance, repeat until contact or
  t = step. Linear-time per iteration via GJK; 2–6 iterations typical.
  Generalised to non-convex articulated by Tang et al, *C2A: Controlled
  Conservative Advancement* (IEEE 2009). Catto 2013, *Continuous
  Collision*, GDC, is the go-to game-engine treatment.
- **Swept volumes** — analytical for sphere-sphere / capsule-capsule;
  for general convex use minimum-time root-finding on
  `f(t) = signed_distance(A(t), B(t))`.

### Contact manifold

- **Persistent contacts + warm starting** — keep impulses across frames
  keyed by a feature pair (vertex+vertex, edge+edge, face+vertex). Box2D /
  PhysX / Jolt all do this; without it, a 10-box stack is unstable in 10
  iterations; with it, in 4.
- **Reduce to N points** — typically ≤ 4 for 3D (box-box generates a quad;
  sphere always 1; hull-hull clipped to ≤ 4 by area-heuristic).
- **Featherstone-style articulation contact** — solve articulation as one
  composite body via cross-coupling block in the system matrix.

### Solvers

- **Sequential Impulses** (Catto, GDC 2005/2006) — PGS reframed as
  impulse-fix-impulse-fix. Warm-started, Baumgarte-stabilised. Default
  Box2D v2 / Bullet / Jolt. Converges fast for small mass ratios and
  short stacks; degrades on long chains, high mass ratios. Default 8
  velocity / 3 position iterations (Box2D v2 historical). **Eylem v1e
  uses this.**
- **TGS — Temporal Gauss-Seidel** (PhysX 5.1+; Box2D v3 "Soft Step") —
  substep the timestep N times, do *one* iteration per substep, integrate
  position between substeps so the next substep sees updated positions.
  Stiffness scales ≈ linearly with N at ≈ 1× cost. PhysX default 4
  substeps × 1 vel iter; Box2D Solver2D study used 4 primary / 2 secondary
  iterations × 60 Hz substeps internally. **Eylem v2 upgrade path.**
- **PGS** (Projected Gauss-Seidel) — the underlying math, applied in
  velocity space with non-negativity projection on normals,
  Coulomb-cone projection on tangents.
- **MLCP via Dantzig / Lemke** — exact solve of the LCP each step; O(n³)
  worst case; used by ODE worldstep, Newton Dynamics, Bullet
  `btMLCPSolver`. Stable for medium scenes; doesn't scale to thousands of
  contacts; useful when you need "no jitter, no penetration, mass ratio
  100:1".
- **Featherstone ABA / CRBA** — Roy Featherstone, *Rigid Body Dynamics
  Algorithms* (2008). Articulated Body Algorithm: O(n) forward dynamics
  over a chain of n links via reduced (joint-space) coordinates. CRBA =
  Composite Rigid Body Algorithm = O(n²) for the joint-space mass matrix.
  Used by MuJoCo, DART, PhysX articulated, Drake. **Eylem v6 / v7 reduced-
  coord articulation slice.**
- **XPBD** (Macklin/Müller, MIG 2016) — position constraints with
  compliance α = 1/k. Each iteration projects positions to satisfy
  `C(x) − α λ̂ = 0` and accumulates Lagrange multipliers; recovers
  mass-independent stiffness behaviour PBD lacked.
- **XPBD Substep** (Macklin et al, MIG 2019, *Small Steps in Physics
  Simulation*) — surprising result: n substeps × 1 iteration beats 1 step
  × n iterations. Used as the default for new XPBD-based engines (Vellum,
  Bevy XPBD, Avian). **Eylem v3 uses this for soft / cloth / rope.**
- **Projective Dynamics** (Bouaziz et al, SIGGRAPH 2014) — quasi-Newton
  over a fixed-system-matrix prefactorization; very fast for cloth + soft,
  less suited to contact-heavy rigid.
- **Vivace** (Fratarcangeli 2016/2018) — GPU randomized graph-coloured
  PGS; the parent of GPU-XPBD pipelines.

### Soft body / cloth / FEM

- **Mass-spring** (Provot 1995) — cheap, jittery, position-dependent
  stiffness. Don't ship as primary.
- **Shape Matching** (Müller 2005) — cluster-based, fast, no FEM math;
  used in older Flex.
- **Co-rotated linear FEM** (Müller 2002 *Stable Real-Time Deformations*;
  Sin/Schroeder/Barbic 2013 *Vega*) — decompose element rotation per
  step → linear elasticity in the rotated frame → no large-deformation
  artifacts. Standard for real-time character flesh / volumetric.
  **Eylem v7a uses this.**
- **Hyperelastic Neo-Hookean / SVK** (Smith et al, *Stable Neo-Hookean
  Flesh Simulation*, TOG 2018) — better behaviour under inversion than
  SVK; PhysX Flesh + Chaos Flesh both use this family. **Eylem v7b uses
  this.**
- **PBD cloth** (Müller 2007) — distance + bend constraints; warm-started;
  frame-stable; *the* baseline for game cloth.
- **XPBD cloth/soft/rope** (Macklin 2016/2019) — replaces all of the above
  as a unified substrate. **Eylem v3b/c/d use this.**
- **Vellum / Flex unified solver** — XPBD substrate, particles +
  constraint set + collision; rigid is just stiff distance constraints;
  fluids via PBF (Position Based Fluids, Macklin 2013).

### Vehicles

- **Raycast suspension** (PhysX Vehicle SDK / Unity Wheel Collider) — one
  ray (or sweep) per wheel from upper-suspension limit toward lower;
  suspension force from compression × stiffness − damping × compression
  rate; tire force from slip ratio + slip angle through a friction-pair
  curve. PhysX docs document "sprung mass model" + scene query → forces →
  forward-integrate the chassis rigid body. Pacejka magic-formula shape
  is a tunable curve, not literally Pacejka in PhysX. **Eylem v5a uses
  this.**
- **N-wheel constrained chassis** — heavier; needs joint between wheel
  and chassis, suspension as compliant joint with limits. Carmack quote:
  raycast wheels are simpler than people think — except for trucks and
  snow.

### Ragdolls / characters

- **Ball-socket / hinge / cone-twist joints** with limits; breakable
  threshold = max joint force.
- **FABRIK / CCD IK** for posing the ragdoll on death frame.
- **Character controller** — capsule (sphere-cylinder-sphere) is the
  default; Bullet's `btKinematicCharacterController` does ghost-object +
  convex sweep test + step-recovery; PhysX `PxController` likewise.
  Kinematic = directly assign displacement, sweep, slide along contact
  plane (typical FPS); Dynamic = capsule-as-rigid-body with locked
  rotation, drive via velocity (better for Souls-like physics
  interactions, harder to tune). **Eylem v1i ships kinematic.**

### Spatial queries

Ray / segment / sphere / capsule / box overlap and sweep, all routing
through broadphase BVH then narrow-phase support function. PhysX, Jolt,
Rapier expose these as first-class scene queries. **Eylem v1h ships
this.**

### Determinism

- **Floating-point IS deterministic per IEEE-754** but reproducibility
  across compiler / CPU / OS varies because of:
  (a) FMA contractions (`a*b+c` vs `fma(a,b,c)` differ in last bit),
  (b) `x87` 80-bit intermediates on legacy x86,
  (c) transcendental functions (`sin`, `cos`, `atan2`, `exp`) implemented
  in libm differently per libc,
  (d) reduction order across threads,
  (e) denormal handling (FTZ/DAZ).
- **Fixed-point** (Photon Quantum, BEPUphysics integer fork) —
  bitwise-deterministic by construction, but reduces feasible world size,
  breaks SIMD, awkward for trig. Eylem v9c reserves an *optional*
  fixed-point fallback for esports / lockstep — not the default.
- **Modern FP-deterministic recipe** (Box2D v3, Jolt with the flag) —
  `-ffp-contract=off`; no `-ffast-math`; custom `atan2`; replace
  `std::sort`, `std::push_heap`, `std::pop_heap`, `std::hash` with
  deterministic variants; pin denormal mode; ensure cross-thread
  reductions are commutative (e.g. OR over bit arrays); pin iteration
  order regardless of arrival order. **Eylem v0c bakes this in;
  ADR-0063 documents the contract.**
- **Replay tests** — record (input + RNG seed) → re-run in CI on a
  different host → check world snapshot hash matches. Standard practice
  in Quantum, Jolt, Rapier. **Eylem v1j ships the harness; v9b expands
  it to a 9-config CI matrix.**

### SIMD + data layout

- **AoSoA** (= "block SoA"): pack N constraints' fields together
  (`float linear_x[8]; float linear_y[8]; ...`). Width matches SIMD
  register: 4 (SSE2/NEON), 8 (AVX2), 16 (AVX-512). **Eylem chooses
  AoSoA-8 (AVX2) with AoSoA-4 fallback.**
- **Body state for 2D fits in 32 B → one AVX register** (Box2D v3); for
  3D it's bigger but still aligns to lane multiples.
- **Graph coloring** — make N constraints in the same batch have *no
  shared body* (lock-free SIMD solve). Greedy graph-coloring is fine —
  Box2D v3 / Avian both use it.
- **Gather/scatter** body velocities at the start/end of each wide
  block.
- **Cross-platform** — use SIMD wrappers (Jolt's `Vec4`, Rapier's
  `simba`, Box2D's hand-rolled SSE2/NEON). AVX-512 still niche on
  consumer; design around 256-bit. **Eylem v0a/b/c builds the wrapper
  layer in `crd-math`.**

### Multithreading

- **Island detection + island parallelism** — once islands are built
  (atomic union-find or single-thread incremental), each island is
  independent → solve in parallel.
- **Lock-free contact buffers** — per-thread accumulators merged at end
  (commutative merge to keep determinism).
- **Atomic warm-start cache** — hash from (bodyA_id, bodyB_id, feature)
  → impulse; lookup at contact creation.
- **Job system integration**: like Box2D v3, *do not* own threads —
  accept a task callback. **Eylem submits to `crd-jobs`, never owns
  threads.**

### GPU acceleration

- **Pays off when** the entity count × contacts/entity is large enough
  to amortise dispatch + readback (typically n > ~10 k bodies / 100 k
  constraints). MuJoCo MJX wins because it batches *thousands of
  independent envs*, not because one env runs faster. For a single-scene
  game with 1 k bodies CPU+SIMD wins by 5–20×.
- **GPU broadphase** = Morton + radix sort + LBVH (Karras).
- **GPU narrow phase** = per-pair compute thread, support-function GJK.
- **GPU PBD** = Vivace / Vellum / Flex pattern — randomized colour
  ordering, Jacobi instead of Gauss-Seidel for parallel-friendliness
  (slower convergence, more iterations).
- **MPM / FLIP / PBF** for fluids/granular/snow — purely particle-grid,
  GPU-native; Disney *Frozen* shipped MPM snow on GPU.
- **Differentiable physics on GPU** — Brax (JAX), MJX (JAX), DiffTaichi
  (Taichi), Genesis (Taichi+CUDA). Not yet a game-engine concern; will
  be by Phase 6+.

### 2D specifics

- **Edge chains** for terrain (Box2D `b2ChainShape`).
- **Sprite collision shapes**: AABB, circle, capsule, convex polygon
  (typically ≤ 8 verts), compound.
- **Joints**: revolute, prismatic, distance, weld, wheel (Box2D-style
  suspension), motor, mouse.
- **2D character controller** = same capsule sweep, but boxes-on-boxes
  is so common SAT is the cheaper narrow phase.

---

## 3. Architectural battle lines

1. **Broadphase: SAP vs dynamic BVH.** Modern consensus = dynamic AABB
   tree (Catto GDC 2019; Bullet/Box2D/Jolt all on it). SAP only wins for
   *tightly clustered, slow* worlds. **Eylem picks BVH** (quad-tree
   variant for SIMD friendliness on the path that becomes v8 GPU-LBVH).
2. **Solver: SI vs TGS vs XPBD-substep.** SI = dependable baseline (Jolt,
   Bullet, Box2D v2). TGS = strictly better for stacking + high mass
   ratios (PhysX 5, Box2D v3). XPBD-substep = unifies rigid+soft+cloth,
   increasingly default for new engines (Avian, Bevy XPBD, Vellum,
   Genesis). All three are credible v1 choices; the field is *moving
   toward XPBD-substep* but it's still less battle-tested for production
   rigid stacking. **Eylem v1 = SI; v2 upgrade to TGS (substep loop wraps
   existing solver — cheap migration); v3 brings XPBD-substep for the
   soft/cloth/rope domain where it excels.**
3. **Articulations: reduced (Featherstone) vs maximal (constraint-based).**
   Reduced = O(n) per chain, no joint drift, but harder to break/cut.
   Maximal = uniform code path with rigid constraints, works with any
   solver, cheap to break/insert. PhysX 5 ships **both** because robotics
   wants reduced and gameplay wants maximal. **Eylem ships maximal first
   (v4); reduced later (v6 or v7).**
4. **Determinism: bake in vs bolt on.** Bake in = pin FP contract,
   replace stdlib trig/sort/hash from day one, design merges to be
   commutative, design queues to be FIFO regardless of thread. Bolt on
   later = months of rewrite (Jolt's flag is ~8% perf because it had to
   be retrofit-friendly; designing it in from scratch is closer to
   ~1–3%). **Eylem bakes it in (ADR-0063).**
5. **2D vs 3D: shared codebase vs two engines.** Box2D + Box3D (Catto) =
   two engines, no code share. Rapier = one templated codebase
   (`Real`, `Dim`). Sharing wins on maintenance cost and feature parity,
   costs you ~10–20% perf in 2D-only paths and forces some asymmetric
   APIs (joints differ). **Eylem templates** because the math module is
   already templated and a robotics-DAW-cinematic engine genuinely needs
   both.
6. **Threading: own threads vs callback.** Box2D v3 = callback-only
   (engine doesn't own threads). PhysX = configurable. Jolt = own
   threadpool with submit callback. **Eylem submits to `crd-jobs` — no
   second thread pool.**
7. **Stateful vs stateless.** Unity Physics shows a stateless engine is
   feasible for rollback. Cost: every tick recomputes the contact graph.
   **Eylem ships stateful with deterministic snapshot** (rollback is
   "snapshot + replay inputs") — this matches Cerid's network-tolerant-
   but-not-network-first goal.
8. **GPU rigid body: yes/later/no.** PhysX 5 / Genesis / Newton are on
   GPU; production AAA games still ship CPU rigid. Below ~10 k bodies,
   CPU-SIMD wins. **Defer to Eylem v8.**

---

## 4. Answers to the eight discriminating questions

1. **Smallest credible v1 (6–8 weeks).** Behind a `crd-eylem` interface
   analogous to `crd-rhi`: rigid 3D only; spheres, boxes, capsules,
   convex hulls (no triangle mesh yet); dynamic AABB tree broadphase;
   GJK + SAT-for-boxes narrow phase + Sutherland-Hodgman manifold
   reduction to ≤ 4 contacts; warm-started Sequential Impulses (8 vel /
   3 pos iterations, fixed-step 60 Hz) — *not* TGS, *not* XPBD;
   revolute + spherical + fixed joints; ray + sphere sweep scene queries;
   capsule kinematic character controller; deterministic-by-construction
   (FP contract, custom trig, sorted iteration, commutative merges);
   island-parallel solve via existing fiber jobs; sleeping. **Out of v1**:
   triangle meshes, CCD, soft body, cloth, vehicles, articulations, GPU.
   PhysX-style cooked colliders deferred. This matches the SI-baseline
   shape Jolt had at the equivalent point in its life and matches what
   Catto recommends as the starting point in his GDC talks.

2. **2D vs 3D — share or split?** Share. Template `crd-math` already
   supports both. Rapier proves the single-codebase model works; Box2D's
   separation is historical. The cost saving (one solver, one broadphase,
   one query API) outweighs ~10% perf loss. v1 ships rigid 3D; v2 adds
   the 2D specialisation by templating Dim and adding edge-chain shape +
   2D-specific joints (wheel, motor).

3. **Determinism: bake in.** Specifically (a) module-level
   `#pragma fp_contract(off)` + `-ffp-contract=off`, no `-ffast-math`,
   (b) ship `crd-eylem`-internal `sin`/`cos`/`atan2`/`sqrt` (sqrt is
   fine in libm, atan2 is not), (c) replace any `std::sort` /
   `std::stable_sort` / `std::hash` use with deterministic versions in
   `crd-containers`, (d) all cross-thread merges commutative (bit OR,
   fixed-order index push), (e) island ordering by stable id, not
   pointer, (f) RNG = stateless splittable (PCG, seeded per island),
   (g) snapshot serialization API as v1 deliverable. Jolt's flag
   retrofit costs ~8%; designing it in from day one costs ~1–3%.

4. **Solver for v1.** **Sequential Impulses, with TGS as v2 upgrade.**
   Rationale: (a) most documentation, code, GDC talks, blog posts you
   can copy from; (b) Jolt + Bullet + Box2D v2 + Chipmunk all proved
   its production fitness; (c) cleanest fit to a fixed iteration budget;
   (d) warm starting is uncomplicated. TGS is strictly better for
   stacking + mass ratios and Box2D v3 + PhysX 5 prove it scales — but
   migrating SI → TGS is mostly the same code with substep loop wrapped
   around it. **XPBD-substep**: do *not* pick as the rigid solver in v1
   — it's being adopted but the production failure modes (jitter under
   pinpoint contact, friction realism vs SI) are not yet fully
   understood. Reserve XPBD for v3 (soft/cloth/rope), then optionally
   promote to a unified XPBD-rigid path in v9 if Vellum-style
   unification proves out.

5. **SIMD width + layout.** AoSoA-8 (256-bit AVX2) for x64; AoSoA-4
   fallback for NEON / SSE2. Box2D v3's exact pattern: 32 B body state
   per body, 8 in a wide block fits 256 B = one cache line group, fits
   two AVX registers. Body state =
   `{position(2/3), velocity(2/3), angular_vel, mass_inv, inertia_inv}`.
   Constraints in graph-coloured blocks of 8. SIMD wrapper layer in
   `crd-math` (`Vec4f`, `Vec8f`, `Mat4f`) so the physics code is
   platform-agnostic and the wrapper picks SSE2/AVX2/NEON at compile
   time. **Eylem v0a–v0e is exactly this work.**

6. **Articulations.** Ship maximal-coords first (each link is a rigid
   body, joints are constraints in the existing solver) — re-uses the
   entire solver path, complexity = a few new joint types. Reduced-coords
   (Featherstone ABA) is a separate code path with its own mass-matrix
   algebra and is *the* right choice for robotics fidelity (no joint
   drift, O(n) per chain). PhysX 5 ships both; Eylem plans v4 = maximal,
   v6 or v7 = reduced behind a `mode = Maximal | Reduced` switch on the
   articulation factory.

7. **GPU pays off above** approximately 5–10 k dynamic bodies and/or
   50–100 k particles, and only when you can amortize compute-shader
   dispatch + readback over a workload that doesn't need to round-trip
   back to CPU each tick. For typical AAA games (1–5 k dynamic bodies)
   CPU-SIMD-fiber-jobs wins by 5–20×. The GPU sweet spot is (a) huge
   particle/granular/cloth (Vellum, Flex), (b) batched RL training
   (MJX, Brax — thousands of envs), (c) film-grade destruction. Eylem
   v8 lands GPU when (a) `crd-rhi` compute path matures and (b) a
   vertical slice (a particle storm, a thousand-ragdoll demo) exists to
   justify it.

8. **Hot 2024–2026.**
   - **Genesis (Dec 2024)** — Python + Taichi + GPU multi-physics, 43
     M FPS reported, blowing up RL community.
   - **NVIDIA Newton (2025)** — open-source robotics engine on Warp,
     integrating MuJoCo Warp + Vertex Block Descent + hydroelastic;
     NVIDIA × DeepMind × Disney joint.
   - **MuJoCo Playground / MJX (2024–2025)** — JAX-native, 10–50× over
     CPU MuJoCo, became the RL default.
   - **Avian / Bevy XPBD ecosystem** — XPBD-substep going mainstream in
     indie/Rust ECS engines; production-quality is rapidly improving.
   - **Differentiable physics** going from research to production —
     DiffTaichi → DiffXPBD (TOG 2023) → MJX → Genesis. Still niche for
     games, central for robotics + ML.
   - **Neural physics / GNN simulators** (Sanchez-Gonzalez et al, ICML
     2020 → many 2024–2025 follow-ups). Surrogate models for slow PDEs;
     not replacing the rigid solver, augmenting fluids/cloth.
   - **Hydroelastic contact (Drake)** going mainstream as a robotics-
     grade alternative to point contact; appears in Newton 1.0.
   - **Vertex Block Descent solver** (NVIDIA Newton's deformable
     backend) — newer XPBD-cousin, very fast for cables/cloth/volumetric.

---

## 5. Sources (curated)

### Primary references

- **[Jolt Physics — Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)**
- [Architecting Jolt Physics for Horizon Forbidden West, Rouwe, GDC 2022](https://jrouwe.nl/architectingjolt/)
- [Jolt Multicore Scaling benchmarks](https://jrouwe.nl/jolt/JoltPhysicsMulticoreScaling.pdf)
- **[PhysX 5 Rigid Body Dynamics docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/RigidBodyDynamics.html)**
- [PhysX 5 Articulations docs](https://nvidia-omniverse.github.io/PhysX/physx/5.5.0/docs/Articulations.html)
- [PhysX 5 Vehicles docs](https://nvidia-omniverse.github.io/PhysX/physx/5.3.1/docs/Vehicles.html)
- **[Box2D v3 release post](https://box2d.org/posts/2024/08/releasing-box2d-3.0/)**
- [Box2D Solver2D comparison study, Catto 2024](https://box2d.org/posts/2024/02/solver2d/)
- [Box2D Determinism, Catto 2024](https://box2d.org/posts/2024/08/determinism/)
- [Box2D SIMD Matters, Catto 2024](https://box2d.org/posts/2024/08/simd-matters/)
- [Box2D Simulation Islands, Catto 2023](https://box2d.org/posts/2023/10/simulation-islands/)

### Catto GDC talks (the canonical sequential-impulses lineage)

- [Catto, Sequential Impulses, GDC 2006](https://box2d.org/files/ErinCatto_SequentialImpulses_GDC2006.pdf)
- [Catto, Modeling and Solving Constraints, GDC 2009](https://box2d.org/files/ErinCatto_ModelingAndSolvingConstraints_GDC2009.pdf)
- [Catto, Computing Distance (GJK), GDC 2010](https://box2d.org/files/ErinCatto_GJK_GDC2010.pdf)
- [Catto, Continuous Collision, GDC 2013](https://box2d.org/files/ErinCatto_ContinuousCollision_GDC2013.pdf)
- [Catto, Understanding Constraints, GDC 2014](https://box2d.org/files/ErinCatto_UnderstandingConstraints_GDC2014.pdf)
- [Catto, Dynamic BVH, GDC 2019](https://box2d.org/files/ErinCatto_DynamicBVH_Full.pdf)

### XPBD / soft-body / FEM lineage

- [Macklin et al, XPBD 2016](https://matthias-research.github.io/pages/publications/XPBD.pdf)
- **[Macklin et al, Small Steps in Physics Simulation 2019](https://mmacklin.com/smallsteps.pdf)**
- [Müller et al, Position Based Dynamics 2007](https://www.cs.toronto.edu/~jacobson/seminar/mueller-et-al-2007.pdf)
- [Müller 2002, Stable Real-Time Deformations](https://cgl.ethz.ch/Downloads/Publications/Papers/2002/p_Mue02.pdf)
- [Sin/Schroeder/Barbic, Vega FEM 2013](https://viterbi-web.usc.edu/~jbarbic/vega/SinSchroederBarbic2012.pdf)

### GPU acceleration

- [Karras 2012, Maximizing Parallelism in BVH/octree/k-d construction](https://research.nvidia.com/sites/default/files/pubs/2012-06_Maximizing-Parallelism-in/karras2012hpg_paper.pdf)
- [Karras 2013, Fast Parallel BVH](https://research.nvidia.com/sites/default/files/pubs/2013-07_Fast-Parallel-Construction/karras2013hpg_paper.pdf)
- [NVIDIA Thinking Parallel Part III: Tree Construction on the GPU](https://developer.nvidia.com/blog/thinking-parallel-part-iii-tree-construction-gpu/)
- [Vivace paper, Fratarcangeli 2016](https://www.semanticscholar.org/paper/Vivace:-a-practical-gauss-seidel-method-for-stable-Fratarcangeli-Tibaldo/6177b7fbc9cd889eb447695e44768eb0ccec345b)

### Robotics / differentiable

- [Rapier docs](https://rapier.rs/docs/), [Rapier Determinism](https://rapier.rs/docs/user_guides/rust/determinism/)
- [Featherstone's algorithm, Wikipedia + papers](https://en.wikipedia.org/wiki/Featherstone's_algorithm)
- [DART Sim](https://dartsim.github.io/), [DART repo](https://github.com/dartsim/dart)
- [MuJoCo MJX docs](https://mujoco.readthedocs.io/en/stable/mjx.html)
- [Drake Hydroelastic User Guide](https://drake.mit.edu/doxygen_cxx/group__hydroelastic__user__guide.html)
- [Genesis repo](https://github.com/Genesis-Embodied-AI/genesis-world), [Genesis docs](https://genesis-world.readthedocs.io/)
- [Brax repo](https://github.com/google/brax), [Brax paper arXiv 2106.13281](https://arxiv.org/abs/2106.13281)
- [DiffTaichi paper arXiv 1910.00935](https://arxiv.org/abs/1910.00935)
- [NVIDIA Newton announcement](https://developer.nvidia.com/blog/announcing-newton-an-open-source-physics-engine-for-robotics-simulation/)

### Other

- [Houdini Vellum docs](https://www.sidefx.com/docs/houdini/vellum/)
- [Disney MPM Snow paper](https://math.ucdavis.edu/~jteran/papers/SSCTS13.pdf)
- [Bullet btDbvtBroadphase header](https://github.com/bulletphysics/bullet3/blob/master/src/Bullet3Collision/BroadPhaseCollision/b3DynamicBvhBroadphase.h)
- [Bullet btKinematicCharacterController header](https://github.com/bulletphysics/bullet3/blob/master/src/BulletDynamics/Character/btKinematicCharacterController.h)
- [Christer Ericson GJK SIGGRAPH 2004 notes](https://realtimecollisiondetection.net/pubs/SIGGRAPH04_Ericson_GJK_notes.pdf)
- [Snethen XenoCollide / MPR (Erwin Coumans repo)](https://github.com/erwincoumans/xenocollide)
- [Tang et al, C2A: Controlled Conservative Advancement](http://gamma-web.iacs.umd.edu/papers/documents/articles/2009/tang09.pdf)
- [Unity Havok Physics FAQ](https://docs.unity3d.com/Packages/com.havok.physics@1.4/manual/faq.html)
- [Chaos Destruction docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-destruction-in-unreal-engine), [Chaos Flesh docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-flesh-overview)
- [zeux/phyx — SIMD 2D physics reference](https://github.com/zeux/phyx)
- [Avian Physics 0.4 — Parallel Solver With Graph Coloring](https://joonaa.dev/blog/09/avian-0-4)
- [Chipmunk2D repo](https://github.com/slembcke/Chipmunk2D)

---

**This file is the source of truth for the *why* behind eylem.** Decisions
locked here flow into ADR-0062 (architecture) and ADR-0063 (determinism
contract). The slice plan derived from this is in
`docs/phases/phase-3.1-eylem.md`.
