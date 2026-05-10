# Phase 3.1 — Eylem: Cerid-native physics

**Status:** ⏳ planned (starts after Phase 3.0 closure 2026-05-10)
**ADRs:** ADR-0062 (Eylem architecture), ADR-0063 (Eylem determinism contract)
**Supersedes:** ADR-0018 (PhysX-first physics plan); Phase 6 (Native physics)
folded into this phase.
**Cornerstones:** ADR-0020 (scene/ECS hybrid), ADR-0033 (`crd-jobs`),
ADR-0050 (storage backends), ADR-0052 (schedule), PRINCIPLES.md
("determinism is a first-class option", "modular by default",
"tak-çıkar third-party").
**Module:** `crd-eylem` (substrate) + `crd-eylem-rigid3d` / `-rigid2d` /
`-soft` / `-articulation` / `-vehicles` / `-ccd` / `-fem` / `-gpu` /
`-diff` (sub-modules; each independently linkable).
**Background:** `docs/research/cerid-eylem.md` is the *why* file —
industry survey, algorithm catalogue, architectural battle lines,
discriminating-question answers. Read it once before starting.

---

## Goal

Build the Cerid-native physics module from day 1: deterministic by
construction, ECS-native, fiber-jobified, multi-domain (games + robotics
+ medical + cinematic + DAW), templated 2D + 3D, GPU-extensible. Match the
architectural depth of Frostbite / Havok / Chaos / PhysX 5 / Jolt / Box2D
v3 — by reusing their well-documented algorithms, not their code.

The phase ships in ~30 granular slices grouped into 10 versions (v0–v9).
Each slice is testable + shippable independently and follows the project's
Definition of Done (12-config sweep green; tests; ADR pointer; docs).

---

## Slice overview

| v | Domain | Slices | Time est | What works at the end |
|---|---|---|---|---|
| **v0** | `crd-math` SIMD substrate | 5 (v0a–v0e) | ~1.5 wk | AoSoA-8 / AoSoA-4 SIMD types in `crd-math`; deterministic trig + sort + hash; bit-exact across MSVC/clang/gcc × x64/ARM |
| **v1** | Rigid 3D substrate | 12 (v1a–v1l) | ~6–8 wk | Boxes / spheres / capsules / hulls stack stably, ragdoll falls, character runs around, raycasts work, snapshot-replay deterministic across the v9b CI matrix |
| **v2** | Rigid 2D specialisation | 3 (v2a–v2c) | ~2 wk | Sprites + edge-chain terrain + 2D wheel/motor joints |
| **v3** | XPBD soft / cloth / rope | 5 (v3a–v3e) | ~3–4 wk | Cloth, rope, soft body, two-way coupling with rigid |
| **v4** | Maximal-coord articulations | 3 (v4a–v4c) | ~2 wk | Ragdolls, robot arms (maximal-coords first; reduced-coords queued for v6/v7) |
| **v5** | Vehicles | 4 (v5a–v5d) | ~2 wk | Drivable car with raycast suspension + tire model |
| **v6** | CCD + reduced-coord articulation | 3 (v6a–v6c) | ~2 wk | Fast bullets stop tunnelling; Featherstone reduced-coord articulations for robotics fidelity |
| **v7** | FEM mesh deformation | 4 (v7a–v7d) | ~3 wk | Co-rotated linear FEM + Stable Neo-Hookean + hydroelastic contact |
| **v8** | GPU acceleration | 5 (v8a–v8e) | ~3–4 wk | LBVH broadphase + GPU XPBD + MPM (snow / sand / fluid) |
| **v9** | Differentiable + determinism hardening | 4 (v9a–v9d) | ~2–3 wk | Gradient-checked differentiable rigid path; 9-config replay-hash CI; optional fixed-point fallback |

Total ~6–9 months engineer-equivalent. v0 + v1 + v2 + v3 = the
**minimum credible multi-domain physics module** (~3 months) and the
slice cluster that lands first.

---

## v0 — `crd-math` SIMD substrate (~1.5 weeks)

Everything below builds on AoSoA-N SIMD types. Doing this in `crd-math`
first means animation, transform propagation, particles, audio DSP all
inherit the speedup later for free.

| Slice | What | LOC est | Tests |
|---|---|---|---|
| **v0a** | ✅ shipped 2026-05-10 — `crd::math::simd::Vec4f` / `Vec8f` / `Mat4f` / `Quatf` + `cmake/CrdSimd.cmake` (CRD_SIMD_LEVEL + CRD_DETERMINISTIC_FP) + scalar-parity preset + AVX2-emission CTest. ~800 LOC, 30 cases / 148 assertions. Session: `docs/sessions/2026-05-10-v0a-simd-substrate.md`. | ~600 | ~25 |
| **v0b** | ✅ shipped 2026-05-10 — `crd::math::simd::Soa<TChunk, Lane>` typed AoSoA container + `soa_for_each_chunk`/`soa_for_each_lane` iteration + `gather8`/`scatter8`/`gather4`/`scatter4` cross-chunk lane movers. ~530 LOC, 19 cases / 345 assertions. Session: `docs/sessions/2026-05-10-v0b-soa-substrate.md`. | ~400 | ~15 |
| **v0c** | ✅ shipped 2026-05-10 — `crd::math::deterministic` Cephes-style sin/cos/tan/asin/acos/atan/atan2/exp/exp2/log/log2/log10/pow + IEEE-correct rounding wrappers (floor/ceil/trunc/round/abs/copysign/fmod) + CI guard `crd-no-std-math-check` that bans std::* math in engine/eylem + engine/hesap. **+ same-day v0c-debt-A paydown:** all 5 v0c debt items closed (f64 overloads of all 26 functions; sinh/cosh/tanh; expm1/log1p; erf/erfc/gamma/lgamma/beta f32+f64; Vec4f/Vec8f branchless SIMD batching with Vec4i/Vec8i/convert.hpp infrastructure). Math suite went 23 → 51 cases, 59 → 535 assertions. Sessions: `docs/sessions/2026-05-10-v0c-deterministic.md`, `docs/sessions/2026-05-10-v0c-debt-A-paydown.md`. | ~700 | ~30 |
| **v0d** | ✅ shipped 2026-05-10 — `crd::containers::sort` / `stable_sort` / `nth_element` / `push_heap` / `pop_heap` / `make_heap` / `sort_heap` — merge-sort-based deterministic stable sort + median-of-three quickselect + Floyd's bottom-up heapify. Lint check `crd-no-std-sort-check` bans `std::sort` etc. in `engine/eylem/**` + `engine/hesap/**`. ADR-0063 §3. ~280 LOC + 14 cases / 1068 assertions. Session: `docs/sessions/2026-05-10-v0d-sort-substrate.md`. | ~500 | ~20 |
| **v0e** | ✅ shipped 2026-05-10 (closes Phase 3.1 v0) — Math benchmark harness `tests/bench/test_bench_simd.cpp` with AoSoA-8 vs scalar micro-benchmarks for Vec3f dot / cross / Mat4f multiply / Quatf compose. Win-release measured speedups: 5.9× / 5.6× / 12.7× / Quatf regresses (per-instance SIMD overhead — documented; AoSoA-8 quaternion batching reserved for eylem v4). ~225 LOC. Session: `docs/sessions/2026-05-10-v0e-bench-harness.md`. | ~400 | ~10 |

**v0 done = AoSoA-8 SIMD foundation + deterministic stdlib substitutions
+ benchmark harness, all green on the 12-config sweep + the new bit-exact
trig CI check.**

---

## v1 — Rigid 3D substrate (~6–8 weeks)

The core physics engine. After this, the engine has real, deterministic,
ECS-integrated physics with snapshot/replay — enough for a vertical-slice
demo without any of the fancy features.

| Slice | What | LOC est | Tests |
|---|---|---|---|
| **v1a** | `crd-eylem` interface module: `IPhysicsScene`, `RigidBody`, `Collider` (Sphere/Box/Capsule/ConvexHull/Plane), `Material`, `Joint` interface, `PhysicsConfig`. Determinism guarantees in the public contract. | ~600 | ~15 |
| **v1b** | Body / shape AoSoA-8 storage in `crd-eylem-rigid3d`. Hooks into ECS as `RigidBodyComponent` + `ColliderComponent` (SparseSet hint per ADR-0050). | ~400 | ~10 |
| **v1c** | Dynamic AABB tree broadphase (Catto GDC 2019). Insert / remove / query / raycast. Single-threaded first. | ~700 | ~20 |
| **v1d** | Narrow phase: GJK distance + EPA penetration over a `support<T>(d)` template family (specialise per shape); Sutherland-Hodgman manifold reduction; SAT fast path for box-box. Regression battery of pre-baked contact pairs. | ~900 | ~25 |
| **v1e** | Sequential Impulses contact solver (Catto GDC 2005), warm-started, persistent contact cache hashed by feature pair, Baumgarte stabilisation. Single-threaded. 8 vel / 3 pos iter default, configurable. Golden 10-box vertical stack stable. | ~700 | ~20 |
| **v1f** | Joints: revolute, spherical, fixed, prismatic. Joint-as-constraint (maximal coords). | ~500 | ~15 |
| **v1g** | Island detection (single-thread incremental union-find, deterministic id-stable order); island-parallel solve via `crd-jobs` fibers. Sleeping (energy-threshold). 100-island parallel speed bench. | ~500 | ~15 |
| **v1h** | Scene queries: ray, sphere overlap, sphere/capsule sweep, closest-point. Single API on top of broadphase + narrow. | ~400 | ~20 |
| **v1i** | Capsule kinematic character controller (sweep + slide + step + ground-detect). Reference: Bullet `btKinematicCharacterController` + DigitalRune. | ~500 | ~10 |
| **v1j** | World snapshot (CRDR `'EYLM'` artifact, ADR-0063 §7) + replay test harness (record inputs + RNG seed → re-run on the same machine → assert hash; CI matrix asserts hash matches across MSVC/clang/gcc × x64/ARM in v9b). | ~400 | ~10 |
| **v1k** | Sandbox integration: spawn 100 falling boxes + ragdoll + character controller; `.collider.toml` cooker handler; ImGui debug viz (contact normals, AABB tree, islands). Profile against the existing renderer. | ~600 | smokes |
| **v1l** | Phase 3.1 v1 close: API surface freeze (sizeof + version `static_assert`s on `crd-eylem` public types); 12-config sweep clean; phase doc updates. | small | freeze tests |

**v1 done = deterministic SI rigid-3D engine, ECS-integrated,
fiber-parallel, snapshot-replayable, character-controller demo running.**

---

## v2 — Rigid 2D specialisation (~2 weeks)

Templates over `Dim` (2 vs 3) — single codebase, not a fork (Rapier model).

| Slice | What |
|---|---|
| **v2a** | Templatize `crd-eylem-rigid3d` over Dim → `crd-eylem-rigid2d` 2D variant (broadphase, narrow phase, solver paths all share template). |
| **v2b** | 2D-specific shapes: edge chain (`b2ChainShape` analogue), polygon (≤ 8 verts). 2D-specific joints: wheel, motor, distance, weld, mouse. |
| **v2c** | 2D character controller (capsule swept against edge chain) + sandbox 2D demo (sprite stacking + draggable mouse joint). |

---

## v3 — XPBD soft / cloth / rope (~3–4 weeks)

Position-based dynamics for the deformation domains. XPBD-substep
(Macklin 2019) is the substrate.

> **Dependency:** v3 depends on **Phase 3.1.5 — `crd-sdf` substrate**
> (ADR-0064; plan: `docs/phases/phase-3.1.5-sdf.md`). Soft-body /
> cloth particles collide against the static environment via SDF
> queries (`DenseSdfGrid::evaluate` + `gradient` for distance + contact
> normal). Phase 3.1.5 ships between eylem v2 (rigid 2D) and v3 so SDF
> environment colliders are available from day 1 of soft-body work.
> Without `crd-sdf`, v3 would have to ship a per-module SDF
> sub-implementation that gets ripped out later.

| Slice | What |
|---|---|
| **v3a** | `crd-eylem-soft`: particle SoA + XPBD substep loop (`n_substeps × 1 iter`). |
| **v3b** | Distance + bend + volume constraints; cloth from triangle mesh. SDF environment collisions via `crd-sdf` queries inside the constraint solve. |
| **v3c** | Rope (chain of distance constraints + bend); demo scene. |
| **v3d** | Soft-body via tetrahedral XPBD (volume + edge constraints). |
| **v3e** | Two-way coupling: cloth/rope ↔ rigid bodies through shared constraint solve (rigid pose drives anchor; constraint impulse pushes back through the rigid solver). |

---

## v4 — Maximal-coord articulations (~2 weeks)

Ragdolls + robot arms via joints in the existing rigid solver.
Featherstone reduced-coords (more robotics-accurate, no joint drift)
queues for v6/v7.

| Slice | What |
|---|---|
| **v4a** | Articulation factory + cone-twist, hinge, ball-socket, slider joint types with limits + breakable thresholds. |
| **v4b** | Ragdoll preset cooker (skeleton + collider per bone + joint per parent edge). |
| **v4c** | IK-pose assist (FABRIK) for spawning ragdolls at last-animated pose. Sandbox demo: shoot ragdoll, watch it fall. |

---

## v5 — Vehicles (~2 weeks)

Raycast suspension + analytic tire model.

| Slice | What |
|---|---|
| **v5a** | Raycast suspension: per-wheel ray query → spring + damper → wheel pose; analytic friction-pair tire model (slip ratio + slip angle → linear/lateral force). |
| **v5b** | Engine model (torque curve), gearbox, differential, brake, steering. |
| **v5c** | Ackermann steering, anti-roll bar, downforce. |
| **v5d** | Drivable demo (sandbox vehicle + terrain). |

---

## v6 — CCD + reduced-coord articulation (~2 weeks)

| Slice | What |
|---|---|
| **v6a** | Conservative-advancement CCD for fast bodies (closest-point GJK → safe Δt → repeat). Bullet-through-paper regression. |
| **v6b** | Speculative contacts (PhysX-style) as cheaper alternative for medium-speed projectiles. Per-body CCD opt-in. |
| **v6c** | Featherstone reduced-coordinate ABA articulation: O(n) forward dynamics over a chain; Composite Rigid Body Algorithm (CRBA) for joint-space mass matrix. Reference: Featherstone *Rigid Body Dynamics Algorithms* (2008). Mode flag on articulation factory: `Maximal | Reduced`. |

---

## v7 — FEM mesh deformation (~3 weeks)

> **Solver dependency note.** Co-rotated linear FEM and Stable
> Neo-Hookean both produce sparse symmetric (positive-definite for
> co-rotated linear; symmetric indefinite for Neo-Hookean inversion-
> safe regions) linear systems each substep. The right consumer is
> **`crd-hesap-iterative` (PCG with IC(0)/AMG preconditioner)** and
> **`crd-hesap-direct` (sparse Cholesky)** — but `crd-hesap` is
> Phase 3.1.6, which ships **after** the eylem v0–v9 sequence
> completes (ADR-0065 + `docs/phases/phase-3.1.6-hesap.md`).
>
> **Resolution:** v7 ships its own narrow internal PCG (Saad 2003,
> Jacobi-preconditioned, ~1500 LOC scoped to FEM's exact need)
> + sparse CSR storage helper. When `crd-hesap` lands, eylem v7
> refactors to use `crd-hesap-iterative::pcg` +
> `crd-hesap-direct::sparse_cholesky` with the same determinism
> contract (both inherit ADR-0063). This is the only place in
> the eylem v0–v9 sequence where a "narrow internal solver →
> later refactor to substrate" pattern applies; everywhere else
> the algorithms are physics-specific (broadphase / SI / XPBD /
> articulation kinematics) and live permanently in eylem.

| Slice | What |
|---|---|
| **v7a** | Co-rotated linear FEM (Müller 2002): tetrahedral mesh, per-element rotation extract, rotated-frame stiffness. **Internal narrow PCG (Jacobi-preconditioned, ~1500 LOC) for the linear solve — refactored to use `crd-hesap-iterative` + `crd-hesap-direct` once Phase 3.1.6 ships.** |
| **v7b** | Stable Neo-Hookean (Smith 2018) for inversion-safe flesh. |
| **v7c** | FEM ↔ XPBD soft path selection (constitutive choice on `SoftBody` factory), cooker for FEM tetrahedral meshes. |
| **v7d** | Hydroelastic-style smooth contact for compliant bodies (Drake-inspired). The right model for medical sim (compliant tissue) and robotics (gripper-on-soft-object). |

---

## v8 — GPU acceleration (~3–4 weeks)

Gated on `crd-rhi` compute-shader pipeline maturity (Phase 3.5+ likely
prerequisite for the compute-pipeline plumbing).

| Slice | What |
|---|---|
| **v8a** | GPU broadphase (LBVH, Karras 2012): Morton + radix sort + tree build + traversal compute kernels. |
| **v8b** | GPU narrow phase (per-pair GJK in compute) for 100k+ bodies. |
| **v8c** | GPU XPBD particle solver (Vivace-style, Jacobi-iter for parallelism). Cloth + grains on GPU. |
| **v8d** | GPU MPM for snow / sand / fluid (Disney *Frozen*-style). |
| **v8e** | Hybrid CPU/GPU dispatch: CPU when n_bodies < threshold, GPU otherwise; readback budget. |

---

## v9 — Differentiable + determinism hardening (~2–3 weeks)

> **Solver dependency note.** Like v7, v9a's differentiable rigid
> path will eventually consume **`crd-hesap-autodiff`** (Phase 3.1.6)
> for the gradient pass. v9a ships its own narrow forward-AD via
> dual numbers + small reverse-mode tape (~1000 LOC, scoped to the
> "differentiable scene" subset). When `crd-hesap` lands, v9a
> refactors to use `crd-hesap-autodiff::reverse` with the same
> determinism contract.

| Slice | What |
|---|---|
| **v9a** | Differentiable rigid path (forward AD via dual numbers + small reverse-mode tape, scoped to a "differentiable scene" subset). Gradient-check vs analytic. **Refactored to use `crd-hesap-autodiff` once Phase 3.1.6 ships.** |
| **v9b** | 9-config replay-hash CI matrix: `{MSVC, clang-cl, gcc} × {x64, ARM64} × {Win-debug, Linux-debug, Linux-release}`. Each runs the v1j replay test; snapshot hash must match the golden reference. (ADR-0063 §5.) |
| **v9c** | Optional fixed-point fallback (`crd-eylem-fp`) for esports / lockstep — Q42.20 wrappers around the SI solver. (ADR-0063 §6.) |
| **v9d** | Public `replay record / replay play` CLI tool for QA + bug repro. |

---

## Definition of Done — per slice

Every slice ships under the standard project Definition of Done
(`CLAUDE.md` § *Definition of Done*):

1. Compile clean, zero warnings on all 12 configs.
2. Pass `clang-tidy` + `clang-format` for changed files.
3. Unit tests; existing tests still pass.
4. **12-config sweep all green**: Win × 7 + Linux × 5.
5. Public API changes → `context.md` + relevant `docs/systems/*.md`.
6. Architectural decision → ADR + `docs/decisions/README.md` + roadmap
   tag.
7. Conventional Commits commit message.

Eylem-specific additions on top of the standard DoD:

8. **Determinism contract honoured** (ADR-0063 §1): no banned stdlib
   trig / sort / hash; FP contract pinned in CMake; cross-thread merges
   commutative + associative; iteration order id-stable. Lint-checked.
9. **Replay test added** for any slice that adds simulation behaviour
   (anything in v1 onward affecting body / contact / constraint state).
   The test records 1 second of simulation, asserts the snapshot hash
   matches a golden reference checked into the repo. Catches
   determinism breaks before they reach v9b's 9-config CI matrix.
10. **Sub-module independence**: every `crd-eylem-*` sub-module compiles
    + links + tests independently. A DAW build linking only
    `crd-eylem` + `crd-eylem-soft` (skipping rigid3d) must succeed.

---

## Definition of Done — phase

The phase closes when v0 + v1 are green (the "minimum credible" scope).
v2–v9 ship as continuations; v3 (soft) follows immediately because it's
the most-used non-rigid feature; v4–v9 sequence by demand from
downstream consumers (Phase 3.5 PBR may need v3 cloth, Phase 4.2
networking will need v9b CI matrix, Phase 8 robotics modules will need
v6c reduced-coord articulations + v9a differentiable).

A vertical-slice demo (sandbox: stacking + ragdoll + character controller
+ raycast pick + 100-rigid-body stress + cooked öbek with collider
components instantiated and simulated) ships at end of v1k.

---

## Pulled-forward prerequisites

- **`crd-math` SIMD substrate** (v0a–v0e) — formally part of Phase 3.1
  but lands as the first slice cluster because every later slice depends
  on it.
- **`crd-jobs`** — already shipped (Phase 2.5). Eylem submits to it; no
  changes needed.
- **`crd-scene`** — already shipped (Phase 3.0). Eylem occupies the
  empty `PrePhysics` / `Physics` / `PostPhysics` schedule phases.
- **`crd-cooker`** — exists (Phase 2.6 + 3.0); v1k adds `.collider.toml`
  handler.

No prerequisites missing. Phase 3.1 can start immediately.

---

## Out of scope for Phase 3.1

- **Physics-driven destruction** (Chaos Destruction, Frostbite) — Phase
  4.1 once a vertical-slice demo justifies it.
- **Fluids beyond XPBD particles** (full SPH, FLIP, MPM at film grade)
  — v8d ships GPU MPM as a starting point; production fluid is Phase 5+.
- **Cloth tearing / topology changes** — XPBD constraints are removable
  but the demo doesn't drive it; Phase 4.1.
- **Networked physics** (rollback, lag-compensated authority) —
  consumes the v1j snapshot/replay; lives in Phase 4.2 networking.
- **Physics LOD / spatial partitioning at world scale** — Phase 4 streaming.
- **Vehicle physics for tracked vehicles / aircraft / boats** — v5
  ships car-class vehicles only.

---

## References

- `docs/research/cerid-eylem.md` — full industry research (engines,
  algorithms, battle lines, discriminating-question answers, sources)
- ADR-0062 — Eylem physics architecture
- ADR-0063 — Eylem determinism contract
- ADR-0033 — `crd-jobs` (the substrate eylem schedules onto)
- ADR-0050 — Storage backends (eylem bodies live in SparseSet)
- ADR-0052 — Schedule (eylem occupies PrePhysics / Physics / PostPhysics)
- ADR-0053 — Component index slot framework (potential future
  `EylemContactIndex` for "which entities are in contact this frame")
- ADR-0058 — Öbek system (carries collider components via the existing
  trait grammar)
- PRINCIPLES.md — "tak-çıkar third-party" + "module isolation" +
  "determinism is a first-class option"
