# ADR-0062 — Eylem: Cerid-native physics architecture

**Status:** Accepted (2026-05-10)
**Tags:** [arch] [physics] [eylem] [ecs] [jobs] [simd] [determinism]
**Supersedes:** ADR-0018 (Physics architecture — PhysX-first plan abandoned)
**Related ADRs:** ADR-0005 (Math v1), ADR-0033 (Job system shape — `crd-jobs`),
ADR-0049 (Entity / SlotMap), ADR-0050 (Storage backends), ADR-0052
(Query · System · Schedule), ADR-0053 (L5 component-index framework),
ADR-0054 (Transform hierarchy), ADR-0058 (Öbek system), ADR-0063 (Eylem
determinism contract).
**Phase:** Phase 3.1 — Eylem (Cerid-native physics).

---

## Context

ADR-0018 (April 2026) committed Cerid to PhysX 5 as the first physics
backend behind a Cerid-owned `crd-physics` interface, with a Cerid-native
Phase 6 replacement keeping PhysX pluggable. The reasoning at the time:
exercise the interface against a mature engine early.

Three things changed by 2026-05-10:

1. **Phase 3.0 closed.** The full ECS substrate (entity / storage /
   relations / queries / schedule / commands / index framework / transform /
   serialization / Öbek / Preset / Profile / async upload) is in place —
   eylem can integrate as ECS-native rather than as a sibling world that
   syncs each tick.
2. **Industry research surfaced specific architectural choices that bind
   physics fundamentals to the substrate** (`docs/research/cerid-eylem.md`):
   determinism is bake-in or bolt-on (Jolt's flag costs ~8% perf because
   it's retrofit), threading model is "own threads" or "submit to host
   jobs" (Box2D v3 chose the latter, matching Cerid's `crd-jobs`),
   data layout is AoSoA-N or vendor-fixed (Box2D v3 / Avian / Jolt all
   land on graph-coloured AoSoA-8 — wraps cleanly into Cerid's existing
   templated math). PhysX as a shipped binding gets us none of these
   choices; it gives us *PhysX's* choices.
3. **The "tak-çıkar third-party" cornerstone in PRINCIPLES.md** says
   external libs sit behind Cerid-owned interfaces. The interface alone
   is the part that matters for portability. Building the interface
   *and the first backend* native is one less wrap-then-replace cycle.

The deciding question:

> Do we wrap PhysX/Jolt as the first backend (3–4 weeks to demo, locked
> into vendor design choices), or do we build the Cerid-native module
> from day 1 (6–8 weeks for v1 rigid 3D, all Cerid-shaped)?

This ADR answers it.

## Decision

### 1. Module name + scope

The Cerid-native physics module is **eylem** (Turkish: *eylem* — "action /
motion"). Module identifiers:

```
crd-eylem                  interface, deterministic-by-construction
crd-eylem-rigid3d          v1 — rigid 3D substrate (SI solver, AABB tree, joints, queries, char ctrl)
crd-eylem-rigid2d          v2 — rigid 2D specialisation (templated single codebase, NOT a fork)
crd-eylem-soft             v3 — XPBD soft / cloth / rope / two-way coupling
crd-eylem-articulation     v4 — maximal-coords first; reduced-coords (Featherstone) v6/v7
crd-eylem-vehicles         v5 — raycast suspension, tire model, gearbox
crd-eylem-ccd              v6 — continuous collision (conservative advancement + speculative)
crd-eylem-fem              v7 — co-rotated linear FEM, stable Neo-Hookean, hydroelastic contact
crd-eylem-gpu              v8 — LBVH broadphase, GPU XPBD, MPM
crd-eylem-diff             v9 — differentiable rigid + cross-platform CI hardening
```

Per cornerstone "module isolation" (PRINCIPLES.md): every sub-module is
independently linkable. A DAW build that wants only soft bodies for a
synth-cable visualisation links `crd-eylem` + `crd-eylem-soft` and pays
zero for rigid-body code.

### 2. Build the substrate from day 1, not as a wrap-replace cycle

PhysX/Jolt as first backend is **not** chosen. ADR-0018 is **superseded**.

Reasons enumerated:

- **Determinism choice.** Cerid wants determinism as a first-class option
  (PRINCIPLES.md cornerstone). Designed in, the FP contract + stdlib
  substitutions + commutative cross-thread merges + id-stable iteration
  cost ~1–3% perf. Retrofit (Jolt's flag) costs ~8%. Building native
  takes the cheaper path; binding PhysX takes neither — PhysX is not
  cross-platform deterministic at all (O3DE explicitly disables
  determinism in its PhysX integration).
- **Threading.** Cerid has fiber jobs (`crd-jobs`, ADR-0033). Binding
  Jolt brings a second thread pool; binding PhysX brings `PxTask`. Both
  waste cores and add coordination cost. Eylem submits to `crd-jobs` —
  no second pool.
- **ECS-native.** PhysX/Jolt maintain a parallel "physics world" that
  syncs to/from the ECS each frame (component → body → solver →
  body → component). Eylem's bodies *are* ECS storage rows in
  `crd-eylem-rigid3d`'s SoA pools; sync is a phase boundary, not a copy.
- **XPBD as a peer.** Both PhysX and Jolt treat PBD as bolt-on. Eylem
  ships XPBD-substep (Macklin 2019) as the v3 substrate for soft / cloth
  / rope, treated as a peer of the rigid solver, with two-way coupling
  through shared constraints.
- **Multi-domain by design.** Robotics needs angular momentum
  conservation + reduced coords; medical needs hyperelastic + smooth
  contact (hydroelastic); games need contact stiffness + warm starting.
  PhysX is games-first with a CUDA-locked robotics path; Jolt is games-
  only. Designing eylem for all three from day 1 is cheaper than
  retrofitting either.
- **No wrap-then-replace.** ADR-0018's plan was bind-then-replace; the
  bind step's value evaporates once the replace is on the calendar.

### 3. v1 architectural choices (locked)

Lock the choices the research pinned. Each is documented in
`docs/research/cerid-eylem.md` § *Architectural battle lines* + § *Answers
to the eight discriminating questions*.

| Aspect | v1 choice | Migration path |
|---|---|---|
| Broadphase | Dynamic AABB tree (Catto GDC 2019) | v8 adds GPU LBVH (Karras 2012) for n > 10 k |
| Narrow phase | GJK + EPA + SAT-fast-path for boxes + Sutherland-Hodgman manifold reduction (≤ 4 contacts) | MPR (XenoCollide) reserved as alternative if EPA conditioning hurts |
| Solver | Sequential Impulses (Catto GDC 2005), warm-started, persistent contact cache, 8 vel / 3 pos iter @ 60 Hz fixed step | v2 adds TGS (Macklin substep wrapper around the SI iteration) |
| Soft / cloth / rope | XPBD-substep (Macklin 2019) | v9 may unify rigid into XPBD if Vellum-style proves out |
| Articulations | Maximal-coords first (joints in the SI solver) | Reduced-coords (Featherstone ABA) added in v6/v7 |
| 2D vs 3D | One templated codebase (Rapier model) | Per-dim specialisations selected at compile time |
| Threading | Submit to `crd-jobs` (Box2D v3 model) | Never own threads |
| Snapshot / replay | First-class API in v1j; bit-exact across MSVC/clang/gcc × x64/ARM | v9b expands CI matrix to 9 configs |
| GPU rigid | Deferred to v8 | CPU-SIMD-fiber wins below ~10 k bodies |
| Determinism | Bake-in (ADR-0063) | n/a — retrofit cost is ~5–6× the bake-in cost |

### 4. Body / shape data layout: AoSoA-8

Body state pools are stored as Array-of-Structs-of-Array with width 8
(AVX2) on x64 and width 4 (NEON / SSE2) elsewhere. Body fields:

```
position[8]       linear_velocity[8]      angular_velocity[8]
mass_inv[8]       inertia_inv[8]          orientation[8]
flags[8]          sleep_state[8]
```

Shape arrays are SoA per shape type (separate pool for spheres, boxes,
capsules, hulls). The choice mirrors Box2D v3's pattern; the SIMD
substrate sits in `crd-math` (v0a–v0e of Phase 3.1) and the physics code
is platform-agnostic — the wrapper picks SSE2 / AVX2 / NEON at compile
time.

### 5. Snapshot-replay is the core deliverable, not a feature

Per `docs/research/cerid-eylem.md` § *Recommended phasing for Cerid*:
**the most important deliverable after v1 is NOT feature breadth — it's
the snapshot-replay determinism harness.** That single capability unlocks
rollback netcode (Phase 4.2), robotics RL (Phase 8 domain modules),
cinematic reproducibility (Phase 8 domain modules), and bug-repro tooling
simultaneously.

v1j is non-negotiable in v1: world snapshot serialise / restore + replay
test harness (record inputs + RNG seed → re-run on a different host →
assert world snapshot hash matches) + CI matrix asserting the hash matches
across MSVC / clang / gcc × x64 / ARM.

### 6. ECS integration: components + systems + commands

Eylem integrates through the existing `crd-scene` substrate, not as a
sibling world:

- `RigidBodyComponent` — sparse-set storage (most entities don't simulate);
  carries handle into `crd-eylem-rigid3d`'s body pool.
- `ColliderComponent` — sparse-set storage; carries shape descriptor +
  handle into the per-shape pool.
- `JointComponent`, `ConstraintComponent` — sparse-set; one per
  connection.
- `PhysicsConfig` (singleton-on-World) — fixed timestep, gravity,
  iterations, determinism mode.
- `EylemPrePhysicsSystem` (PrePhysics phase) — sync `Transform` →
  RigidBody where Transform is authoritative (kinematic bodies).
- `EylemSolveSystem` (Physics phase) — substep loop: broadphase →
  narrow phase → island detect → solve → integrate.
- `EylemPostPhysicsSystem` (PostPhysics phase) — sync RigidBody →
  `Transform` where physics is authoritative (dynamic bodies).
- Scene queries (`raycast`, `sphere_overlap`, `sphere_sweep`,
  `closest_point`) are free functions on the world / pool, not bound to
  a system.

The empty `PrePhysics` / `Physics` / `PostPhysics` schedule phases that
have lived in `crd-scene` since v1h finally find their consumer.

### 7. Cooker integration

Mesh shapes (convex hulls today; triangle meshes when v1 grows that)
cook to a CRDR artifact via a new handler `.collider.toml` →
`crd-cooker` (Phase 3.1 v1k). Cooked artifact carries the convex
decomposition or BVH; runtime mounts via `ResourceManager` like every
other Cerid asset. ADR-0058 Öbek can carry collider components via the
existing component-trait grammar — no öbek changes required.

### 8. Determinism contract is its own ADR

The determinism rules (FP contract, stdlib substitutions, cross-thread
merge discipline, RNG strategy, replay-hash CI) are large enough to
deserve their own ADR — see ADR-0063. Every eylem sub-module honours
that contract.

## Rationale

### Why eylem now, not later

The argument I rejected was "wrap Jolt for the demo, replace later":

- Wrap-then-replace is a 3–4 week investment that will be rewritten in
  Phase 6 anyway. Two wrap-replace cycles per ADR-0018 (PhysX → native).
- Vendor design choices propagate into the interface even when the
  interface is "Cerid-owned" — the API shape that fits Jolt's
  `BodyInterface` doesn't fit Cerid's ECS-native goal.
- Cerid's existing infrastructure (fibers, ECS, math, allocators)
  removes most of the cost that makes physics-from-scratch hard. We're
  not building everything from zero; we're building the physics-specific
  algorithms on top of substrate already shipped.

### Why SI for v1 (not TGS, not XPBD-rigid)

- Sequential Impulses has the most documentation, code, GDC talks, and
  blog posts to cross-reference (Catto's 2005–2019 series is the
  canonical curriculum). Time-to-correctness is the lowest.
- Production-proven by Jolt, Bullet, Box2D v2, Chipmunk.
- Cleanest fit to a fixed iteration budget.
- Migration to TGS in v2 is mostly the same code with a substep loop
  wrapped around it — the cost of "wrong solver in v1" is bounded.

XPBD-rigid is genuinely better in some workloads (cables, fast soft
contact) but production failure modes are still being characterised
(jitter under pinpoint contact, friction realism vs SI). It belongs in
v3 where it's the right tool (soft / cloth / rope), with a possible v9
unification once the field has stabilised.

### Why maximal-coord articulations first

Maximal coords reuse the existing constraint solver — joints are just
constraints between rigid bodies. Cost is "a few new joint types".
Reduced coords (Featherstone ABA) is a separate code path with its own
mass-matrix algebra and is the right choice for *robotics fidelity* (no
joint drift, O(n) per chain). PhysX 5 ships both. Eylem starts where
the existing solver pays back.

### Why one templated codebase for 2D + 3D

- Rapier proves the model; Box2D's separation is historical (Catto wrote
  Box2D before C++ templates were a comfortable answer for this).
- `crd-math` already templates over `MathScalar` and dimension; eylem's
  SoA storage is templated already.
- Maintenance + feature parity dominate ~10–20% perf in 2D-only paths.
- A robotics + DAW + cinematic engine genuinely needs both.

### Why physics never owns threads

Box2D v3 is the strongest case study: callback-only threading
(`b2EnqueueTaskCallback` / `b2FinishTaskCallback`) gave it
production-grade scaling without owning threads. Cerid has fibers — using
them is free. Owning a second pool would waste cores under load and
fight Cerid's existing schedule.

## Consequences

- **`docs/decisions/0018-physics-architecture.md`** marked **Superseded
  by ADR-0062**.
- **`docs/phases/phase-3.1-eylem.md`** (new) — full slice plan, ~30
  slices over v0–v9.
- **`docs/phases/phase-6-native-physics.md`** — folded into Phase 3.1
  (the "native physics" phase IS the physics phase now).
- **`docs/research/cerid-eylem.md`** (new) — research backing this
  decision; the *why* file.
- **`docs/decisions/0063-eylem-determinism-contract.md`** (new) — the
  FP contract + stdlib substitutions + replay CI matrix.
- **`docs/ROADMAP.md`** — Phase 3.1 line updated from "physics (PhysX)"
  to "eylem (Cerid-native physics)".
- **`docs/debt.md`** — any PhysX backend entries removed.
- **`crd-math`** gets a SIMD substrate pass first (Phase 3.1 v0a–v0e)
  before physics code lands. AoSoA-8 (AVX2) + AoSoA-4 (NEON / SSE2)
  wrappers; deterministic stdlib substitutions land here too. Benefits
  every future consumer (animation, transform propagation, particles,
  audio DSP).
- New module skeleton:
  `engine/eylem/include/crd/eylem/...` + `engine/eylem/src/...` +
  `tests/eylem/...` + `runtime/examples/smoke_eylem*.cpp`.
- Cooker grows `.collider.toml` handler in v1k; öbek + scene already
  consume eylem components via the existing component-trait grammar.
- Sandbox grows an eylem-driven demo (stacking + ragdoll + character
  controller + raycast pick) in v1k.

## References

- `docs/research/cerid-eylem.md` — full industry research
- `docs/decisions/0063-eylem-determinism-contract.md` — sister ADR
- `docs/phases/phase-3.1-eylem.md` — phased slice plan
- `docs/decisions/0018-physics-architecture.md` — superseded
- ADR-0033 — `crd-jobs` (the substrate eylem schedules onto)
- ADR-0050 — Storage backends (eylem bodies live in SparseSet)
- ADR-0052 — Schedule (eylem occupies PrePhysics / Physics / PostPhysics)
- ADR-0053 — Component index slot framework (potential future
  `EylemContactIndex` for "which entities are in contact this frame")
- PRINCIPLES.md — "tak-çıkar third-party" + "module isolation" +
  "determinism is a first-class option"
