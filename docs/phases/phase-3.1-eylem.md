# Phase 3.1 — Eylem: Cerid-native physics

**Status:** ⏸ **PAUSED at v1b close (2026-05-11)** — v0 ✅ + v1a ✅ + v1a-draw d0..d4 ✅ + v1a-material a/b/c/d ✅ + v1b-a…e ✅ (the ADR-0076 §12 sequencing pivot). v1c+ resumes after detour D-007 + hesap complete, consuming geometry (✅ closed 2026-05-19) + hesap (v0–v16 ✅) from day 1. (Header refreshed 2026-08-07.)
**ADRs:** ADR-0062 (architecture; §4.5 five-category collider model + §5.5 Wave 1+2+3 reservations) · ADR-0063 (determinism contract) · ADR-0066 (`crd-draw` substrate; §19.2.1 Diagnostic viz reservation) · ADR-0067 (force-field substrate, three-tier; 9 formulas + Reserved_J2 slot) · ADR-0068 (body types + collision filtering + callbacks; 3 motion types + sensor + 5-tier filter + deferred ECS event streams + ContactModify) · ADR-0069 (materials substrate; 64-byte Material struct + 6 friction models + 3 restitution models + 5 combine modes + MaterialId + 8 shipped materials) · ADR-0075 (testing rigor; 6 test categories + cross-engine bench). Plus reserved Planned: ADR-0070 (solver catalog) · ADR-0071 (robotics importers + actuators) · ADR-0072 (sensor substrate) · ADR-0073 (aerospace substrate + new `crd-eylem-aero` module) · ADR-0074 (cinematic / animation-physics bridge + new `crd-eylem-cine` module). Each Planned ADR has its phase-plan slot reserved; mints when its dossier ships at slice time.
**Research dossiers:** `docs/research/cerid-eylem.md` (architecture) · `cerid-eylem-fields.md` (5,447 words, fields industry survey) · `cerid-eylem-collision-filtering.md` (10,370 words, body/filter/callback survey across 5 mandate domains) · `cerid-eylem-coverage-audit.md` (8,604 words, gap audit across 5 domains) · `cerid-eylem-materials.md` (9,650 words, 16 engines surveyed). 6 more dossiers planned for the Wave 2/3 ADRs.
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

## Resume directive 2026-07-02 — post-hesap sequencing, the crush mandate, and the two-profile architecture

> Pinned from the 2026-07-02 planning session (user direction; session
> `docs/sessions/2026-07-02-v13z-windows-close.md`). Supplements the 2026-05-15 plan below.

**1. THE SEQUENCE (locked):** hesap v14 (tensors) → v15/16 (autodiff) → v17 (GPU) → v18 (notebook + agent platform)
→ **EYLEM RESUME (v1c → v9, the flagship platform consumer)** → `crd-ui` → the editor. **Rationale — eylem waits for
the full arc** (the same logic as the geometry-before-physics pivot, ADR-0076 §12): ADR-0086's design (powered
ragdolls with learned controllers, GPU crowds, differentiable contact) *consumes* v16 AD + v17 GPU — resuming after
the arc means eylem integrates them from day one instead of retrofitting (the deferred-refactor debt we refuse).
**Renderer/material work is NOT a standalone phase** — it is pulled by consumers (deformables ⇒ skinning/blend-shapes
in the material system; the editor ⇒ picking/outline/viewport; sciviz ⇒ plot rendering), per the real-workload-before-
optimization principle. The **gizmos/direct-manipulation cluster** (long-standing high user priority) is part of the
editor arc. **The editor rides the v18/ADR-0081 registry** — the GUI emits commands, so undo = replay and every
button is agent-drivable by construction; UI-before-command-surface is the mistake other engines made.

**2. THE CRUSH MANDATE (user 2026-07-02): eylem must be stronger, more performant, and generally better than PhysX,
Jolt, and everything else — honestly, full-board.** The honest crush map:

| axis | posture |
|---|---|
| **Cross-platform bit-determinism** | UNIQUE WIN — Jolt is deterministic only same-binary/arch; PhysX not meaningfully at all. The crd::math moat is the enabler ⇒ lockstep multiplayer + replay-exact physics nobody else ships. |
| **Articulated bodies** | WIN by import — reduced-coords Featherstone (ADR-0086) + hesap sparse direct + v15 exact Jacobians = Drake/MuJoCo-grade articulation in a game engine. |
| **Stiff/large systems** | WIN — sparse-direct/implicit options (the CHOLMOD-crushing solvers) for stacks/vehicles/cables where iterative game solvers explode. |
| **Differentiable physics** | WIN (v16) — only research engines have it; none deterministic, none shipping. |
| **Soft body / real-time FEM** | WIN-capable (eylem v7 over hesap direct/iterative). |
| **Raw rigid-body throughput vs Jolt** | FIGHT to parity-or-win, promise nothing early — the measure-first campaign (the v5/v7 lattice playbook: profile → find the lever → flip losses one by one). |
| **GPU physics vs PhysX** | Honest: PhysX's CUDA home turf; v17 portable-Vulkan fights with the determinism column as differentiator (the FFT-vs-MKL posture: portable + principled, any residual gap named). |

**The eylem scoreboard is full-board from day one** (ADR-0075 already mandates cross-engine bench): Jolt's own
samples, PhysX SDK scenes, Bullet, MuJoCo/Drake (robotics), matched fidelity, ms/frame @ N bodies game-style budgets,
losses named + fixed-or-escalated (SANITY #9). **Already-built inventory to assemble, not rebuild:** broadphase BVH +
GJK/EPA/CCD raycast (geometry, shipped) · the QP/MLCP solver experience (v7-k) · raw-span fixed-step kernels (the v9
two-layer contract was designed FOR eylem's hot loop) · the OTG for kinematic characters (v13) · fiber jobs · v14
batched tensors for SoA body math.

**3. TWO MODES = TWO SOLVER PROFILES ON ONE SUBSTRATE (user 2026-07-02), not a fork** — the convergence of
ADR-0062/0063 (multi-domain) + ADR-0060 (profiles) + ADR-0078 (f32/f64 orthogonality):
- **Game profile:** f32 · fixed-step XPBD/TGS · frame-budgeted (the v13 WCET pillars repurposed as a frame-time
  contract) · the ADR-0086 LOD continuum + authoritative-coarse/cosmetic-fine split · deterministic for lockstep.
- **Engineering profile:** f64 · implicit/variational integrators over hesap direct solvers · energy/momentum audit
  gates · mm-accurate contact · the certification pillars + the v17-k certificate chain (auditable simulation runs).
- Same API, same scene, same constraint graph; the profile picks integrator/precision/budget. Robotics + medical mix
  per-system between the two. **The engineering profile is the in-house ORACLE for the game profile's accuracy
  claims** — the hesap gold-standard doctrine applied to physics testing.

**4. Resume-opening tasks:** ratify **ADR-0086** (still Proposed) + reconcile ADR-0074 (powered-ragdoll vs
cinematic-kinematic) · write the resume session's crush-scoreboard harness FIRST (the ADR-0075 cross-engine bench,
extended with the full board above) · per-slice Windows DoD from day one (the 2026-07-02 scar).

## Strategic Execution Plan 2026-05-15 — eylem v1c+ resume sequencing

Per `docs/ROADMAP.md` § Strategic Execution Plan (locked 2026-05-15),
eylem v1c+ resume is scheduled at **~+6 to +9 months** from 2026-05-15
(after Phase 3.1.7 geometry CLOSE + Phase 3.1.6 `crd-hesap-dense` v0).
The pre-conditions when v1c+ resumes:

- **Units throughout.** Phase 3.1.7.5 `crd-units` shipped; eylem v1c+ ships with `RigidBody` / integrator / force fields / contacts / constraints all dimensional from day 1.
- **Geometry consumed from day 1.** Phase 3.1.7 CLOSED; `crd-geometry-bvh::DynamicBvh` consumed by v1c broadphase; v2 GJK/EPA + v2j feature clipping consumed by v1d narrowphase + v1d-manifold; v4 mesh closest-point + raycast consumed by v1d-mesh.
- **`crd-hesap-dense` v0 available.** BLAS L1/L2/L3 + LAPACK-class direct (Cholesky / LU / QR) shipped; eylem v7 FEM later consumes hesap natively (no narrow internal PCG → refactor pattern; v7 FEM ships hesap-consuming from day 1).
- **Profiler instrumentation.** Detour D-003 shipped; eylem v1c+ instrumented from day 1.
- **Deterministic-replay validated.** Detour D-004 shipped; eylem v1c+'s determinism contract gets its first cross-config replay-hash CI lane (the early version of the v9 9-config CI gate).

### Eylem cold-storage mitigation (in-flight during Phase 3.1.7)

Eylem v1b shipped 2026-05-11; resume target ~2026-12. To prevent code
rot over the ~7-month gap, as each Phase 3.1.7 geometry sub-module
ships run a **~30-min integration smoke** against the corresponding
eylem v1c+ stub path (per [[feedback_per_slice_run_ctest]] memory):

| Geometry sub-module ships | Eylem v1c+ stub smoke |
|---|---|
| v1 BVH (already shipped) | `eylem::Broadphase` builds + minimal overlap-query against a 100-AABB scene |
| v2 convex (already shipped) | `eylem::Narrowphase` builds + minimal GJK pair-test on box/box |
| v3 hull construction + simplification (already shipped) | `eylem::Collider::ConvexHull` builds + simplification of a 100-vertex cube |
| v4 mesh (NEXT in geometry phase) | `eylem::TriangleMeshCollider` builds + minimal raycast against a baked CRDR mesh |
| v5 spatial | `eylem::SpatialIndex` builds + minimal point-query |
| v6 polygon | (no eylem consumer; smoke skipped) |
| v7 mesh-processing | `eylem::ColliderCooker::simplify_for_physics` builds + QEM-reduces a test mesh |
| v8 delaunay | (no eylem consumer until eylem v7 FEM; smoke skipped) |
| v9 GPU LBVH | `eylem::Broadphase::gpu_path` builds (compile-only; no GPU execution required for smoke) |
| v9c V-HACD | `eylem::ColliderCooker::vhacd_decompose` builds + decomposes a test mesh into N sub-convexes |

These smokes are NOT formal slices and do NOT block geometry
sub-module close. They catch eylem build breakage before it
accumulates. If a smoke fails: file a short debt entry in
`docs/debt.md` and continue. eylem v1c+ resume will pay it down.

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
| **v0** | `crd-math` SIMD substrate | 6 (v0a–v0f) | ~1.5 wk | AoSoA-8 / AoSoA-4 SIMD types in `crd-math`; deterministic trig + sort + hash; bit-exact across MSVC/clang/gcc × x64/ARM; **`Mat4f operator*` SIMD-routed (12.7× speedup, public API unchanged)** |
| **v1** | Rigid 3D substrate + draw substrate + material substrate + filtering + callbacks + force fields | ~30 sub-slices (v1a + v1a-material-{a,b,c,d} + v1a-sandbox-smoke + v1a-draw d0..d4 + v1b-{a,b,c,d,e} + v1c + v1c-sensor + v1d + v1d-{mesh,hf,filter-a,filter-b,filter-c,callback-a,callback-b,callback-c} + v1e + v1e-material + v1f + v1f-articulation-filter + v1f-fields-{a..j} + v1g + v1g-contactmodify + v1h + v1i + v1j + v1k + v1k-material-{cooker,bench} + v1l + v1l-test-{conservation,closedform,stress}) | ~9–14 wk | Boxes / spheres / capsules / hulls stack stably, ragdoll falls, character runs around, raycasts work, snapshot-replay deterministic across the v9b CI matrix; **`crd-draw` substrate live** (peer module per ADR-0066) — wireframe + solid translucent, three depth modes, per-component visualizer plug-in registry, replay-friendly retained buffer; **Material substrate (Coulomb friction + Constant restitution shipped, 5 friction + 2 restitution models reserved for v5/v7/v8d)** + **5-tier collision filter** + **deferred ECS event-stream callbacks** + **force-field substrate (9 formulas; analytic + grid + scripted)** + **conservation-law CI**; **sandbox `--smoke-test` flag** wired into `scripts/full-sweep.ps1` |
| **v2** | Rigid 2D specialisation | 3 (v2a–v2c) | ~2 wk | Sprites + edge-chain terrain + 2D wheel/motor joints |
| **v3** | XPBD soft / cloth / rope | 5 (v3a–v3e) | ~3–4 wk | Cloth, rope, soft body, two-way coupling with rigid |
| **v4** | Maximal-coord articulations + cinematic bridge | 6 (v4a–v4c + v4d-cine-{a,b,c}) | ~3 wk | Ragdolls, robot arms (maximal-coords first; reduced-coords queued for v6/v7) + animation-physics bridge per ADR-0074 |
| **v5** | Vehicles | 4 (v5a–v5d) | ~2 wk | Drivable car with raycast suspension + tire model + 5 deferred friction models from ADR-0069 (Stribeck/LuGre/Karnopp/Anisotropic/FrictionTriple) |
| **v6** | CCD + reduced-coord articulation + robotics + aerospace + sensors | ~12 sub-slices (v6a/b/c + v6c-{urdf,sdf,mjcf,actuators} per ADR-0071 Planned + v6d-nonsmooth-newton per ADR-0070 Planned + v6e-sensor-{a,b,c} per ADR-0072 Planned + v6f-aero-{a,b,c,d} per ADR-0073 Planned) | ~3–4 mo | Fast bullets stop tunnelling; Featherstone reduced-coord articulations for robotics fidelity; URDF/SDF/MJCF importers + nonsmooth Newton solver + IMU/LIDAR/proximity sensors + aerodynamic forces + atmospheric model + propulsion — **Wave 2 robotics + aerospace substrate lands here** |
| **v7** | FEM mesh deformation | 4 (v7a–v7d) | ~3 wk | Co-rotated linear FEM + Stable Neo-Hookean + hydroelastic contact + v7-material-huntcrossley (HuntCrossley restitution per ADR-0069) |
| **v8** | GPU acceleration | 5 (v8a–v8e) | ~3–4 wk | LBVH broadphase + GPU XPBD + MPM (snow / sand / fluid) + v8d-material-newton (Newton restitution per ADR-0069) |
| **v9** | Differentiable + determinism hardening | 7 (v9a–v9d + v9b-test-{cross-engine,drift,property} per ADR-0075) | ~3–4 wk | Gradient-checked differentiable rigid path; 9-config replay-hash CI; optional fixed-point fallback + cross-engine bench + drift CI per ADR-0075 |

Total **~12–18 months** engineer-equivalent for the full v0-v9 + Wave 1/2/3 ADR fills. **v0 ✅ + v1 + v2 + v3 = the minimum credible multi-domain physics module (~4 months)**; the v6 cluster (Wave 2 robotics + aerospace + sensors + nonsmooth Newton) lands the multi-domain claim against MuJoCo / Drake / IsaacSim / Project Chrono / AGX Dynamics / GMAT/Orekit. Without the Wave 2/3 fills, Phase 3.1 would close a games-engine-equivalent substrate; with them, Cerid genuinely matches the elite tier domain-by-domain.

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
| **v0f** | ✅ shipped 2026-05-10 — `Mat4<f32> operator*` (Mat4×Mat4 + Mat4×Vec4) routes through SIMD internally via `mat_simd_f32.hpp`. **12.7× measured speedup** (per v0e bench, win-release AVX2). Public API unchanged; non-template overload wins for exact `Mat4<f32>` match, other `Mat4<T>` instantiations stay scalar. Bit-exact parity with scalar reference verified (3 tests, 18 assertions). Doc-comments added to `vec.hpp` + `quat.hpp` explaining why per-instance Vec3f/Quatf are intentionally NOT SIMD-ified (12-byte struct → wasted lane + cache bloat for Vec3; 0.65× regression measured for per-instance Quatf — Bullet btVector3 cautionary tale). Eylem v1+ batched workloads use `simd::Soa<TChunk, Lane>` AoSoA columns instead. ~110 LOC. | ~110 | ~3 |

**v0 done = AoSoA-8 SIMD foundation + deterministic stdlib substitutions
+ benchmark harness + Mat4f SIMD specializations, all green on the
12-config sweep + the bit-exact trig CI check.**

---

## v1 — Rigid 3D substrate + `crd-draw` (~9–14 weeks)

The core physics engine. After this, the engine has real, deterministic,
ECS-integrated physics with snapshot/replay — enough for a vertical-slice
demo without any of the fancy features. **And visual debugging from
day 1** thanks to the `crd-draw` substrate slipped in between
v1a (interface) and v1b (storage), so v1c (broadphase) / v1d (GJK/EPA) /
v1e (solver) are all debuggable visually rather than via printf + faith.

ADR-0066 locks the draw substrate architecture; research dossier:
`docs/research/cerid-draw.md`. Companion module `crd-eylem-viz`
ships in v1a-draw d3 to bridge eylem components into the visualizer
plug-in registry — keeps `crd-eylem` itself free of any rendering
dependency (dependency-inverted plug-in pattern).

| Slice | What | LOC est | Tests |
|---|---|---|---|
| **v1a** | ✅ shipped 2026-05-10 — `crd-eylem` interface module: `IPhysicsScene`, `RigidBody`, `Collider` (8-value `ColliderShape` enum per ADR-0062 §4.5: Sphere / Box / Capsule / ConvexHull / Plane + TriangleMesh / Heightfield / Sdf reserved at v1a freeze; impl per category lands in v1d / v1d-mesh / v1d-hf / Phase 3.1.5), `Material` (locked at v1a-material-a per ADR-0069), `Joint` interface, `PhysicsConfig` (extended with `persistent_alloc` + `solver_scratch` in v1b-a). Determinism guarantees in the public contract per ADR-0063. `NullPhysicsScene` factory ships for tests/tools. | ~600 | ~15 |
| **v1a-material-a** | ✅ shipped 2026-05-11 — Locked 64-byte `Material` struct (one cache line; static_assert pinned at v1a freeze) + 6-value `FrictionModel` enum (Coulomb / Stribeck / LuGre / Karnopp / Anisotropic / FrictionTriple — last reserves room for v5 vehicles' MuJoCo §2.8 sliding/torsional/rolling triple via `friction_anisotropy` Vec3f reinterpretation; struct does NOT grow) + 3-value `RestitutionModel` enum (Constant / Newton / HuntCrossley) + extended `CombineMode` adding `GeometricMean = 4` (Box2D v3 / Jolt / Unity DOTS / AGX consensus default for stacking-stable friction) + `MaterialId` strong type matching BodyId/ColliderId/JointId pattern + content-addressed via FNV-1a-64 + `default_material_value()` constexpr helper. 161 assertions across 19 test cases pass. Per ADR-0069 §1-§3. Research dossier: `docs/research/cerid-eylem-materials.md`. | ~150 | ~5 |
| **v1a-material-b** | ✅ shipped 2026-05-11 — `MaterialPool` on scene (slot 0 = null sentinel; slot 1 = `default_material()` allocated at construction); `create_material(material) → MaterialId` / `update_material(id, new) noexcept` / `material(id) → const Material&` (default-fallback for invalid id) / `has_material(id)` API on `IPhysicsScene` + `NullPhysicsScene` impl. Append-only insert (v1; remove via standard generation-bump pattern when needed post-v1). Determinism: `MaterialId.index()` = slot position; identical insert sequences across runs produce identical IDs. Per ADR-0069 §3 + §11. 193 assertions / 23 cases. Session log: `docs/sessions/2026-05-11-v1a-material-b.md`. | ~200 | 4 |
| **v1a-material-c** | ✅ shipped 2026-05-11 — Per-collider `Collider::material` field (4-byte `MaterialId` handle, defaults to `MaterialId::default_material()`). `IPhysicsScene::add_collider` refactored: 2-arg `(body, collider)` is the canonical pure virtual (typical scene-load path: cooker pre-allocates materials, loader streams colliders); 3-arg `(body, collider, material)` becomes a non-virtual NVI convenience that calls `create_material` then forwards. `NullPhysicsScene::StoredCollider` drops inline `Material` (lives in pool now). `using IPhysicsScene::add_collider;` to preserve the convenience overload through name-hiding rules. Per ADR-0069 §3 + §11. 206 assertions / 26 cases (+13/+3). Session log: `docs/sessions/2026-05-11-v1a-material-c.md`. | ~50 | 3 |
| **v1a-material-d** | ✅ shipped 2026-05-11 — `derive_mass_properties(colliders, accessor, user_data) → DerivedMassProperties{mass, com_local, inertia_diagonal}` free function in `crd::eylem` + `IPhysicsScene::derive_body_mass(BodyId)` virtual. Walks colliders in **ascending ColliderId order** (FP-deterministic per ADR-0063 §4 fixed-position-write protocol). Two-pass: pass 1 = mass + COM accumulation; pass 2 = full 3×3 symmetric inertia tensor (rotated to body local frame via `R I R^T`, parallel-axis-shifted to body COM, summed) → diagonal extracted (off-diagonal reserved for v1c full diagonalisation + v1f side-channel). Analytic volumes + inertia diagonals: sphere `(4/3)π r³` + isotropic `(2/5) m r²`; box `8 hx hy hz` + `(1/3) m (hi² + hj²)`; capsule (axis = Y) `π r² (2h) + (4/3)π r³` + composite cylinder + 2-hemisphere with parallel-axis shift to capsule centre. ConvexHull / Plane / TriangleMesh / Heightfield / Sdf carry zero-volume in v1a (cooker pre-computes at v1d-mesh / v1d-hf / v1d-sdf + v1k). NullPhysicsScene's `StoredCollider` tracks owning `BodyId` so the per-body walk filters cleanly. RigidBody application convention: when `inv_mass == 0` (default sentinel), caller may apply `inv_mass = 1/mass` + `inv_inertia = 1/diag`; explicit `inv_mass > 0` is honoured as a manual override. Per ADR-0069 §3 + §8 + §11. 236 assertions / 31 cases (+30/+5). Session log: `docs/sessions/2026-05-11-v1a-material-d.md`. | ~250 | 5 |
| **v1a-sandbox-smoke** | ✅ shipped 2026-05-10 — Added `--smoke-test [duration_seconds]` flag to `crd-sandbox` (boots normally, runs main loop for N seconds, exits 0 on clean shutdown). win-debug verified: 354 frames presented over 2.0s (177 fps), clean exit. Returns exit=2 if 0 frames present (catches swapchain failure loops). Extended `scripts/full-sweep.ps1` with new `-SkipSandboxSmoke` + `-SandboxSmokeDurationSeconds` parameters (default 3.0s); after each per-config ctest on the 6 Win configs, the sweep runs `crd-sandbox.exe --smoke-test N` and treats non-zero exit as `SANDBOX-SMOKE-FAIL`. Skipped on win-tidy / win-shipping (build-only) and on Linux (WSL2 GPU passthrough brittle). Catches Vulkan validation + resource init order + profile/preset apply-cycle + render-path runtime bugs the unit tests can't. ~95 LOC sandbox + ~30 LOC sweep extension. | ~125 | smokes |
| **v1a-draw-d0a** | ✅ shipped 2026-05-10 — `crd-draw` module skeleton + `RenderBuffer` (PhysX-style retained SoA: `DebugPoint`/`DebugLine`/`DebugTriangle`/`DebugText`, packed RGBA8 `Color`, `PrimFlags` u32 with 3 depth modes + 12 categories + 16-bit picking_id reserved) + `line` + `box_wire` + `aabb_wire` shape generators + `line_aa.{vert,frag}.glsl` shaders cooked via existing asset_cooker pipeline into `<bin>/assets/cooked/draw_shaders.crdr` (8KB pack, 2 SPIR-V artifacts). 12 tests / 50+ assertions. | ~700 | 12 |
| **v1a-draw-d0b** | ✅ shipped 2026-05-10 — `crd::draw::init(rm, device, InitConfig)` loads SHDR resources from cooked pack via `ResourceManager::load_sync<ShaderResource>`, creates Vulkan `ShaderModule` (vert+frag) + `PipelineLayout` (80-byte push-constant range) + `GraphicsPipeline` (TriangleList, blend-on, conditional depth-test, dynamic viewport, no depth-write — overlay never modifies depth). Per-frame instance buffer ring (1 buffer per frame-in-flight, `MemoryUsage::CpuToGpu` so persistently mappable). `shutdown()` releases everything in dep order. Side-fix: added `Format::R32Uint` + `Format::R32Sfloat` to RHI enum (line-instance attributes need them) + Vulkan format mapping. ~270 LOC. | ~270 | (build-link only) |
| **v1a-draw-d0c** | ✅ shipped 2026-05-10 — `add_draw_overlay_pass(FrameGraph&, ImageHandle color, ImageHandle depth, const RenderBuffer&, OverlayPassConfig)` declares a frame-graph overlay pass: imports color (ColorWrite, alpha-blend over) + depth (DepthRead if valid; pipeline doesn't write depth). Execute lambda: `begin_rendering` → maps the per-frame instance buffer + packs `LineInstanceGpu[N]` (start/end/color_packed/flags_raw/width = 36 bytes/instance, matches shader inputs at locations 0-4) → `bind_pipeline` → `push_constants` (DrawPushConstants: view_proj/viewport_px/category_mask/time_s = 80 bytes) → `bind_vertex_buffer` → `draw_instanced(6, N, 0, 0)` → `end_rendering`. Side-fix: added `CommandBuffer::draw_instanced(vc, ic, fv, fi)` to RHI interface + Vulkan + 4 fakes (Cerid RHI didn't have instanced draw before). Internal `crd/draw/detail/gpu_types.hpp` shares `RendererState` + `LineInstanceGpu` + `DrawPushConstants` between renderer.cpp and overlay_pass.cpp. ~120 LOC + RHI extensions. | ~120 + RHI ext | (build-link only) |
| **v1a-draw-d0d** | ✅ shipped 2026-05-10 — Sandbox calls `crd::draw::init()` after ResourceManager mounts both demo_assets and draw_shaders packs; sandbox_layer emits 3-axis world triad + 1 wire box per frame; invokes `add_draw_overlay_pass` via a dedicated 2nd FrameGraph after ForwardRenderPath executes. `crd-sandbox --smoke-test 2` verifies 354 frames / 2.0s / 176.8 fps clean (Vulkan validation layer clean after the SPIR-V `discard` -> `return` swap). Full DoD sweep all 14 build steps green. | ~150 | smokes |
| **v1a-draw-d0d-fix** | ✅ shipped 2026-05-10 — Black-line bug fix: lines were rendering but black (depth-test rejection vs reverse-Z scene depth + per-pixel AA falloff zeroing alpha for thin lines). Flipped pipeline default to `DepthMode::Always` for d0 (per-DepthMode pipelines reserved for d2-depth) + verified color unpack across Win + Linux. ~50 LOC. | ~50 | smokes |
| **v1a-draw-d1** | ✅ shipped 2026-05-10 — Solid translucent rendering pipeline (sort-by-centroid + alpha blend; WBOIT reserved for SDF cell viz Phase 3.1.5+) + per-shape unit primitives (`kUnitBoxLines`, `kUnitBoxTriangles`, `kUnitSphereWireframe16x8`, `kUnitSphereIcosphere1`, `kUnitCapsuleWireframe16x8`, `kUnitCapsuleSolid24x8`). Adds `box_solid` / `sphere_wire` / `sphere_solid` / `capsule_wire` / `capsule_solid` immediate-mode functions. UV sphere for wireframe (recognisable equator/axis) + icosphere for solid (vertex regularity). Per ADR-0066 §5, §7. | ~600 | ~8 |
| **v1a-draw-d2** | ✅ shipped 2026-05-10 — Full immediate-mode API: `arrow_to` (line stem + 4-tri solid cone head, gizmo-ready) / `axis_triad_to` (3 arrows from local frame columns, RGB convention) / `arc_to` (joint limits, rotation handles) / `cross_3d_to` / `grid_to` / `frustum_to` / `aabb_wire_to`. Phase 7 editor manipulators + brush previews ride this surface. Sandbox demo shows all primitives. Per ADR-0066 §3, §13. ~400 LOC. | ~400 | smokes |
| **v1a-draw-d2-fix** | ✅ shipped 2026-05-10 — Capsule semicircle parameterization corrected ([0, π] sweep instead of broken [-π/2, +π/2]); sphere_solid switched to UV tessellation matching sphere_wire's grid for perfect alignment. Latent RHI-Vulkan blend-state bug also surfaced + fixed (default-zero blend factors made every blended fragment write black; first consumer of `enable_blend=true`). | ~50 | smokes |
| **v1a-draw-d2-overflow** | ✅ shipped 2026-05-10 — Multi-batch submit per ADR-0066 §19.4: when N > max_per_frame, the overlay pass loops over batches re-using the same instance buffer. No primitive truncation, no GPU memory churn. Removes the "100k AABB workload caps out" blocker for eylem v1c+. ~50 LOC in overlay_pass.cpp + 2-3 buffer-overflow tests. | ~50 | ~3 |
| **v1a-draw-d2-depth** | ✅ shipped 2026-05-10 — 6-pipeline matrix per ADR-0066 §19.1: `(line, triangle) × (Test, Always, GreaterDimmed)`. Submit-time bin-by-DepthMode; XRay primitives emit twice (dimmed-occluded + full-visible). Removes the "always-on-top forever" workaround from d0d-fix. ~150 LOC renderer + ~50 LOC overlay-pass binning + 4 tests. | ~200 | ~4 |
| **v1a-draw-d2-curbuf** | ✅ shipped 2026-05-10 — Thread-local `active_buffer()` + convenience wrappers per ADR-0066 §19.3: `crd::draw::line(a, b, color)` etc. routes to thread-local buffer; `*_to(buf, ...)` canonical kept verbatim. CRD_ASSERT in debug if no active buffer; no-op release. ~80 LOC + 3 tests. | ~80 | ~3 |
| **v1a-draw-d2-grid** | ✅ shipped 2026-05-10 — Infinite faded floor grid: full-screen quad shader (`infinite_grid.vert/frag.glsl`) cooked into the existing draw_shaders.crdr pack. New 3rd pipeline (no instance buffer; push-constant driven). Multi-scale grid pattern with distance fade. New `infinite_grid_to(plane_y, primary_cell, secondary_cell, primary_color, secondary_color, fade_distance)` API. Editor + sandbox + future tools all consume. ~200 LOC + 1 shader pair + 4 tests. | ~200 | ~4 |
| **v1a-draw-d2-theme** | ✅ shipped 2026-05-10 — Themable color/sizing palette. `engine/draw/include/crd/draw/theme.hpp` + `src/theme.cpp` add `DrawTheme` struct + `current_theme()`/`set_theme()` global accessors; defaults match Blender 3D View axis gizmo hues (X = 255,51,82 / Y = 139,220,0 / Z = 40,144,255 from Blender's `userdef_default_theme.c` TH_AXIS_X/Y/Z) — `kAxisX/Y/Z` in `types.hpp` updated to match. Adds `pack_rgb_u8(r,g,b,a=255)` constexpr helper for callers with raw byte triples. Grid axis colors moved from hardcoded shader literals to push-constant fields (8 bytes of previously-padded space repurposed); shader unpacks them per fragment so axis colors are themable per-frame without recooking. Adds `OverlayPassConfig::GridConfig::apply_theme(theme = current_theme())` chainable helper. Sandbox opts in. ~150 LOC + 1 shader edit + 1 RHI side-effect (push-constant layout now spans Vertex+Fragment shader stages because the grid frag uses them). Tests: smokes only (875+ frames clean, no Vulkan validation). | ~150 | smokes |
| **v1a-draw-d2-aa** | **DEFERRED — rescheduled to land alongside scene-wide MSAA in a later phase.** Per-pixel distance AA in `line_aa.frag` already produces good 1x quality; overlay-local MSAA infrastructure (RHI sample-count fields + transient MSAA attachment + resolve-on-end-rendering) brings real engineering work without a near-term consumer. Design of record stays in ADR-0066 §19.5; that section is unchanged. When it ships: 5-tier quality (Off/Low/Med 2x/High 4x/Ultra 8x), `QualityPreset::debug_overlay_aa` schema v1→v2 bump (v1 loaders default to Low). Original estimate ~300 LOC. | ~300 | ~5 |
| **v1a-draw-d3** | ✅ shipped 2026-05-10 — Hook-based auto-viz substrate. New files: debug_viz_component.hpp (bitfield flags: AxisTriad/Wireframe/Solid/ShowVelocity/ShowAabb/ShowJoints/ShowContacts/Highlight + tint + scale), visualizer_registry.hpp + .cpp (typed fn-ptr plug-in, register_for<T>(VisualizerFn, Category) captures T's identity in a captureless lambda → ComponentFetchFn), debug_viz_system.hpp + .cpp (ISystem in SchedulePhase::PostRender, single-path explicit registry+buffer refs at ctor, no thread-local fallback), default_visualizers.hpp + .cpp (Transform → axis triad reference visualizer, gated on AxisTriad flag, stamped Category::Scene). New module dep edge crd-draw → crd-scene (PUBLIC, called out in CMakeLists per CLAUDE.md no-cycles review). Sandbox wires registry + system + spawns 3 demo entities at (4,0,0)/(4,0,3)/(4,0,-3) with set_translation + DebugVizComponent{scale=0.5}. Buffer clear moved from render_scene to top of on_update so PostRender writes into a fresh buffer. **`crd-eylem-viz` deferred to v1b** (needs RigidBodyComponent/ColliderComponent/JointComponent which land in v1b — registering visualizers for non-existent components would be a stub target, violates quality bar). 3 new tests + 1 stale assertion fixed (kDefaultFlags.depth()=Always not Test, d0d-fix had flipped default but missed assertion). 167/167 assertions across 15 test cases pass. Sandbox smoke: 879 frames @ 175 fps exit 0. Known pre-existing tech debt: ImGui's vulkan backend (third-party imgui_impl_vulkan.cpp:592) emits vkFlushMappedMemoryRanges with size not aligned to nonCoherentAtomSize, surfaced because d3's added entities shift ImGui's per-frame buffer layout. ~600 LOC + 3 tests. | ~600 | ~3 |
| **v1a-draw-d4** | ✅ shipped 2026-05-10 — **`crd-draw` public API frozen** (ADR-0066 §19.6 — kDrawApiVersion = 1, sizeof asserts on every public type). New peer module `crd-draw-imgui` (depends on `crd-draw` + `crd-imgui`; keeps headless/DAW builds free of ImGui transitively). ImGui control panel: master enable + master scale + 12 category checkboxes + theme dropdown + grid sub-panel. Profile gating via `is_overlay_enabled()` / `set_overlay_enabled(bool)` runtime toggle (zero-CPU early-out in `add_draw_overlay_pass` + `DebugVizSystem::run`). `serialize_render_buffer(buf, dst)` + `serialize_render_buffer_size(buf)` no-op stubs (Phase 7 replay viewer impl per ADR-0066 §19.7). Sandbox smoke 879 frames clean. **1k-box ragdoll demo deferred to v1b cluster** (needs RigidBodyComponent which lands v1b-c). | ~250 | smokes |
| **v1b-a** | ✅ shipped 2026-05-11 — New peer module `crd-eylem-rigid3d` (sibling of `crd-eylem` interface). Extended `PhysicsConfig` with `persistent_alloc` (TLSF expected; scene-lifetime body/collider/joint pools + contact cache + variable-size geometry) + `solver_scratch` (LinearAllocator expected; per-step bump arena). `BodyPool` AoSoA-(8 on AVX2, 4 on scalar) over `crd::math::simd::Soa`: 19 SIMD float columns (pos/rot/linvel/angvel/inv_mass/inv_inertia/damping) + per-lane integer side-bands (flags/generation/live) in same chunk for cache locality. Free-list reclaim, generation-bumped BodyId. Allocator strategy doc: `docs/systems/eylem-allocators.md`. **First real consumer of v0 SIMD substrate.** 8 cases / 140 assertions. | ~410 | 8 |
| **v1b-b** | ✅ shipped 2026-05-11 — `ColliderPool` per shape kind (Sphere / Box / Capsule for v1b — ConvexHull / Plane defer to v1d, TriangleMesh to v1d-mesh, Heightfield to v1d-hf, Sdf to Phase 3.1.5). `ColliderId` index encodes routing `[kind:4 \| per_kind_idx:20]` — handle dispatches without a global record table; 16 shape kinds reservable, 1M colliders/kind. Per-kind chunks store common columns (local_pos, local_rot, body_idx, generation, live) + kind-specific columns (sphere=radius; box=half_extents; capsule=radius+half_height). 11 cases / 75 assertions. Per ADR-0062 §4.5. | ~360 | 11 |
| **v1b-c** | ✅ shipped 2026-05-11 — `RigidBodyComponent` (8B: BodyId + sync_to_transform flag + 3B pad) + `ColliderComponent` (4B: ColliderId) PODs in `crd-eylem` interface, registered with `StorageHint::SparseSet` per ADR-0050 (sparse vs total entity count; lifecycle dominated by add/remove). `EylemSystem` in `crd-eylem-rigid3d` reports `phase()=SchedulePhase::Physics` + `fixed_step()=true`. Constructor takes `BodyPool&` + `PhysicsConfig` snapshot (gravity + fixed_dt). `run(World&)` queries `(Transform, RigidBodyComponent)`, reads body from pool, integrates: `v += g·dt; v *= (1 - lin_damp·dt); p += v·dt; ω *= (1 - ang_damp·dt); q = normalize(q + 0.5·dt·(ω_quat · q))`. Static bodies (`inv_mass == 0`) skipped. Writes back via `World::set_translation` / `set_rotation_quat` so TransformPropagation marks dirty + refreshes `Transform.world` in PreRender. Per-entity opt-out via `RigidBodyComponent::sync_to_transform=0` (for cinematic-bridge in v4d). NOT a workaround: lifecycle hooks via on_add/on_remove deferred — current path is `pool.insert(body)` then `world.add_component(e, RigidBodyComponent{id})` (caller-owned attachment; spawn helper lands at v1b-d/e). 5 new TEST_CASEs / 21 assertions: registration round-trip, phase + fixed_step + name pins, integration vs discrete-Euler closed form (60 substeps under -9.81 m/s² gravity from rest, ε=1e-3), `World::step_fixed` runs 10 substeps for 10/64-second frame at 1/64 fixed_dt (exact ratio avoids FP-floor slop), static body inv_mass=0 doesn't integrate. crd-eylem-rigid3d-tests: 246 assertions / 24 cases. ctest win-debug 1046/1046; win-shipping 1043/1043 (release excludes debug-only `#if CRD_ENABLE_ASSERTS` cases). | ~210 | 5 |
| **v1b-d** | ✅ shipped 2026-05-11 — `crd-eylem-viz` companion module created (peer to `crd-eylem-rigid3d` per ADR-0066 §13 dependency-inverted plug-in pattern). Depends on `crd-eylem` + `crd-eylem-rigid3d` + `crd-draw` + `crd-scene` (PUBLIC); kept out of `crd-eylem` itself so headless / DAW / cooker / scientific-computing builds can omit. API surface: `register_eylem_visualizers(VisualizerRegistry&, const BodyPool&, const ColliderPool&)`. Visualizers: RigidBodyComponent → velocity arrow (when DebugVizComponent::ShowVelocity flag set; reads Transform for origin + body_pool.read(body_id).linear_velocity for direction; speed-threshold-gated to skip near-stationary bodies; max-arrow-length cap of 5 m so a 100 m/s body doesn't draw a 100 m arrow); ColliderComponent → wireframe matching shape kind (Sphere → sphere_wire_to; Box → box_wire_to with translation-only Mat4f; Capsule → capsule_wire_to with axis = local Y per ColliderCapsule convention; ConvexHull/Plane/TriangleMesh/Heightfield/Sdf no-op until v1d / v1d-mesh / v1d-hf / Phase 3.1.5 ship narrow-phase impls). JointComponent visualizer commented as no-op shell pointing v1f author at the file. **Pre-req:** extended `VisualizerContext` with `const World*` (defaulted nullptr; non-breaking for existing default_visualizers) so visualizers can look up Transform via `world->get_component<Transform>(entity)`. Pool refs captured at registration time as file-scope statics — v1b-d-tier solution; one (BodyPool, ColliderPool) pair per process; revisit when multi-eylem-world workload appears. 5 new TEST_CASEs / 14 assertions: registration round-trip with sphere collider + ShowVelocity emits both lines + triangles; sphere wireframe lines-only; box wireframe exactly 12 edges; ShowVelocity flag-gating (without flag = empty buffer); zero-velocity body emits no arrow. **Tests use `crd::memory::TlsfAllocator{16 MB}` per fixture** (production-realistic + catches leaks at destruction; saved as feedback memory `feedback_named_allocators_in_tests.md` for project-wide convention). Built clean win-debug + win-shipping; ctest 1051/1051 + 1048/1048. | ~250 | 5 |
| **v1b-e** | 🚧 in-flight 2026-05-11 — Sandbox eylem demo + **render interpolation** (Glenn Fiedler "Fix Your Timestep" §5). Sandbox spawns 3 rigid bodies (sphere/box/capsule) at +y=5 with full `Transform + RigidBodyComponent + ColliderComponent + DebugVizComponent{AxisTriad+Wireframe+ShowVelocity}`; 16 MB dedicated TLSF heap (`crd::memory::TlsfAllocator{..,"eylem-tlsf"}`); per-frame `World::step_fixed(dt, 1/64, max=8)`. Bodies fall through the world (no collision until v1d) — honest framing. **Interpolation substrate added inside v1b-e (was not in original v1b-e scope but the user flagged jerky variable-rate render of fixed-step physics; addressed in-slice to avoid shipping a visible regression):** `BodyChunkT<Lane>` grew from 19 → 26 columns (608 → 832 B per AVX2 chunk) with prev_pos_{x,y,z} + prev_rot_{x,y,z,w} mirror columns; `BodyPool::snapshot_state_to_prev()` is one Vec8f assignment per column per chunk (O(chunks), free-lane safe via `live[lane]==0` guards downstream); `BodyPool::write_curr_only(BodyId, RigidBody)` writes only curr (integrator semantics — preserves the prev snapshot captured earlier in the substep); `BodyPool::write(BodyId, RigidBody)` mirrors pos/rot into prev (teleport semantics — spawning / level-reset / network-snap-correction); `BodyPool::read_prev(BodyId) → PrevState{position, rotation}` for renderers. `EylemSystem::run` calls `snapshot_state_to_prev()` ONCE at the start of each substep then uses `write_curr_only` instead of `write`. New `World::fixed_step_alpha(fixed_dt) → f64 ∈ [0,1]` + `fixed_step_accumulator()` accessors. New `RigidBodyInterpolationSystem` (`crd-eylem-rigid3d`) — phase = `SchedulePhase::PreRender`, `fixed_step()=false`, registered BEFORE `TransformPropagation` so propagation reads the lerped pose. Per-body: skip null/stale body_id and `sync_to_transform==0`; lerp pos linearly, **nlerp** rot with `dot<0` short-arc fix (visually identical to slerp at 60 Hz substep rate, ~6× faster — no `acos`/`sin`); write via `World::set_translation` / `set_rotation_quat` (TransformDirtyFlag tagged → propagation refreshes `Transform.world`). 7 new TEST_CASEs / +41 assertions: PreRender + variable-rate pins; alpha=0 emits prev; alpha=0.5 emits midpoint; **nlerp short-arc validation with non-degenerate antipodal pair** (prev=identity, curr=negated small Y-rot; without fix would land near 180° wrong); sync_to_transform=0 honoured; null/stale body_id no-op; fixed_step_alpha clamps on irregular accumulator; **multi-substep flow** (frame_dt=2.5·fixed_dt, max_substeps=4 → 2 substeps run + alpha=0.5; verifies prev=pose_after_substep_1, curr=pose_after_substep_2, lerp = 2·g·dt²). crd-eylem-rigid3d-tests: 287 assertions / 32 cases (+41/+7). Tests use named `TlsfAllocator{4 MB, ..,"interp-test"}` per fixture (project allocator-discipline convention). **NOT in v1b-e but flagged for v1c+:** double-write to Transform per frame (EylemSystem writes curr, InterpolationSystem overwrites with lerped value) — functionally correct, marginally wasteful (two dirty-flag taggings). Visual smoothness must be verified by the user (sandbox already runs clean; eye-test pending). | ~480 | 12 |
| **⏸ PAUSE — Phase 3.1.7 `crd-geometry` executes here** | Per ADR-0076 §12 amendment (2026-05-11), the full 29-slice `crd-geometry` phase executes between v1b cluster close and v1c. v1c onward then consumes `crd-geometry` from day 1 with no deferred-refactor. See `docs/phases/phase-3.1.7-geometry.md`. | — | — |
| **v1c** | Dynamic AABB tree broadphase (Catto GDC 2019). Insert / remove / query / raycast. Single-threaded first. **Consumes `crd-geometry-bvh` directly** (post-§12 amendment); the original "ships own BVH first, refactor later" reservation is OBSOLETE — v1c starts only after Phase 3.1.7 has shipped the BVH substrate. | ~700 | ~20 |
| **v1c-sensor** | 🚧 partial — `ColliderFlags::is_sensor` per-collider flag (NOT per-body, per ADR-0068 §10.1 / §10.2 — Jolt's per-body model rejected for multi-domain mandate) ✅ shipped 2026-05-11 in `collider.hpp`. **Pending:** scene-component round-trip + öbek serialisation + EylemSystem fast-path (sensor pairs skip contact response, fire trigger events only). Lands fully alongside v1c broadphase. Per ADR-0068 §2. | ~50 | ~3 |
| **v1d** | **Narrow-phase substrate + ConvexHull + Plane (dispatch + single-point contact).** Per-pair dispatch over `ColliderShape` × `ColliderShape` to the right narrow-phase entry point in `crd-geometry-convex`: closed-form for Sphere-vs-{Sphere,Box,Capsule,Plane}; SAT for Box-vs-Box (v2d); GJK distance + EPA penetration for Hull-vs-anything (v2a + v2c, via the concept + `support()` ADL pattern). Emits a *single-point* contact (deepest point from EPA + normal) into `ContactCache` keyed by feature pair. Regression battery of pre-baked contact pairs. v1d-mesh + v1d-hf fold their per-shape contact gen into the same dispatch. **Consumes `crd-geometry-convex` v2a–f directly** (post-§12 amendment); the original "ships own narrow GJK+EPA, refactor later" reservation is OBSOLETE. **Multi-point contact manifold reduction is split out to v1d-manifold** (which lands BEFORE v1d-mesh because mesh contacts also need the manifold-reduction pipeline). | ~700 | ~20 |
| **v1d-manifold** | **PhysX/Jolt-grade contact manifold generation (NEW — added 2026-05-13 alongside Phase 3.1.7 v2 expansion).** EPA gives the deepest contact point; real-world face-vs-face contacts have up to 4 coplanar points (the "stable stack" requirement), edge-vs-edge has 2. This sub-slice ships the PhysX/Jolt-grade manifold builder eylem needs to compete on stacking stability: (1) **Feature identification** — given the EPA contact normal, pick the **reference face** of body A (the face whose plane is most-aligned with the contact normal) and the **incident face** of body B (the face whose plane is *least*-aligned with the contact normal, i.e. most parallel to the reference face). Uses `crd::geometry::convex::enumerate_faces` from v2j. (2) **Face clipping** — clip the incident face polygon against the reference face's side planes (the planes of the adjacent faces, oriented outward from the reference face), retaining only points on the reference-face side. Uses `crd::geometry::convex::clip_against_convex_volume` (Sutherland-Hodgman, v2j). (3) **Manifold reduction** — from the clipped polygon's vertices, keep up to 4 points using the published "deepest + furthest-three-from-deepest" algorithm (Erin Catto, Box2D — picks a stable simplex spanning the contact patch). (4) **Edge-vs-edge degeneracy** — when both shapes' incident features are edges (rare; sharp-corner contact), reduce to the 2 closest-point witnesses from GJK directly. Output: `ContactManifold{normal, points[≤4]}` written to `ContactCache`. Determinism: face/edge enumeration order pinned per v2j; reduction tiebreak is the published Catto order. **Stacking validation:** 10-box vertical stack at v1e (already in roadmap) must achieve stable rest within 30 substeps — this is the multi-point contact test, not single-point. Without v1d-manifold the stack would slowly drift / fall apart. | ~600 + ~400 tests | ~12 |
| **v1d-filter-a** | Tier 1 (64-bit bit-mask layers, mutual consent) + Tier 2 (Box2D-style i16 group index override) + filter eval pipeline. Bench: layer reject rate at 10k bodies. Per ADR-0068 §10.4. | ~250 | ~6 |
| **v1d-filter-b** | Tier 3 (explicit excluded pairs) + storage (hash set of (min(a),max(b)) tuples). Bench: 30-link humanoid self-collision matrix. Per ADR-0068 §10.4 Tier 3. | ~150 | ~3 |
| **v1d-filter-c** | Tier 4 (ECS-native predicate hook with **closed read set declared at registration**, PHYSICALLY enforced at API surface — Bevy Rapier `BevyPhysicsHooks` formalism + Cerid stricter extension). Bench: 1% pair predicate cost. Per ADR-0068 §10.4 Tier 4. | ~200 | ~4 |
| **v1d-callback-a** | Deferred ECS event streams: `ContactEvent` + `TriggerEvent` ECS buffers; `EylemPostPhysicsSystem` writes; sort by `(min(body_a,body_b), max(body_a,body_b), kind)` for deterministic delivery regardless of which fibre generated which contact. Per ADR-0068 §6. | ~300 | ~6 |
| **v1d-callback-b** | Per-pair `ContactPairFlags`: opt-in for `Persist`/`Stay` (default OFF — destruction storm avoidance per Box2D v3); opt-in for contact-point detail; default lifecycle = Begin/End only. Per ADR-0068 §6. | ~100 | ~3 |
| **v1d-callback-c** | Bench suite: callback storm test (10k events/step destruction scene) + budget assertions (≤ 0.5 ms peak; ≤ 0.05 ms typical 1k events) per ADR-0068 §11. | ~50 | bench |
| **v1d-mesh** | **`ColliderShape::TriangleMesh` impl.** BVH over triangles (binned-SAH builder; refit-only updates for moving meshes if any), per-tri SAT against primitive colliders (Sphere/Box/Capsule/ConvexHull). `.mesh-collider.toml` cooker handler. Best for STATIC level geometry where SDF voxel cost would be wasteful. | ~600 | ~15 |
| **v1d-hf** | **`ColliderShape::Heightfield` impl.** Per-cell analytic contact gen (bilinear-sampled height + normal vs primitives). `.heightfield.toml` cooker handler with optional R16 PNG height-source ingest. 100× cheaper than equivalent SDF voxels for terrain at scale. | ~400 | ~10 |
| **v1e** | Sequential Impulses contact solver (Catto GDC 2005), warm-started, persistent contact cache hashed by feature pair, Baumgarte stabilisation. Single-threaded. 8 vel / 3 pos iter default, configurable. Golden 10-box vertical stack stable. | ~700 | ~20 |
| **v1f** | Joints: revolute, spherical, fixed, prismatic. Joint-as-constraint (maximal coords). | ~500 | ~15 |
| **v1f-articulation-filter** | Tier 5 articulation auto-filter (lands with the joint slice that introduces articulations). `Joint::collide_connected = false` default; `ArticulationLinkComponent::self_collision_enabled = false` default + per-link-pair allowlist. Standard URDF/SDF/MJCF pattern. Per ADR-0068 §10.4 Tier 5. | ~150 | ~3 |
| **v1f-fields-a** | `ForceFieldComponent` declaration in `crd-eylem` (interface, ✅ shipped 2026-05-11 — see ADR-0067) + `EylemFieldSystem` skeleton in `crd-eylem-rigid3d` + Tier 1 formulas: `Directional`, `Radial`, `Drag` (the three most common). | ~350 | ~6 |
| **v1f-fields-b** | Tier 1: `Vortex`, `Magnetic`. | ~150 | ~3 |
| **v1f-fields-c** | Tier 1: `Noise` — analytic-derivative Simplex curl-noise via `crd::math::deterministic::noise::simplex_curl`. CI guard rejecting finite-difference variants. Per ADR-0067 §7. | ~250 | ~4 |
| **v1f-fields-d** | Composition modes (`Add` / `Replace` / `Multiply` / `Max` / `Min`) + content-addressed `FieldId` (FNV-1a) + trigger semantics (`Continuous` / `OnEnter` / `OnExit` / `OnEnterOnce`). Per ADR-0067 §6. | ~200 | ~5 |
| **v1f-fields-e** | Tier 1: `Gradient` formula consuming `SdfResource` from `crd-sdf`. **Lands AFTER Phase 3.1.5 closes** — not a v1g prerequisite; surface frozen at v1l, formula impl fills inside it. | ~150 | ~3 |
| **v1f-fields-f** | Tier 2: `VectorGridResource` + `.field.crdr` cooker handler + `GridSample` formula + trilinear sampling. | ~200 | ~4 |
| **v1f-fields-g** | Tier 3: `Script` formula via `ScriptComponent` integration. **Lands AFTER Phase 4 scripting ships** — same blocking treatment as v1f-fields-e. | ~100 | ~2 |
| **v1f-fields-h** | Visualizer hook in `crd-eylem-viz` — force-field arrows via `crd-draw`'s `VisualizerRegistry` (d3 pattern). Per-field arrow density slider. | ~150 | smokes |
| **v1f-fields-i** | Bench suite + budget assertions in CI (per ADR-0067 §12 budget table) + sandbox demo (gravity well + wind tunnel + vortex trap + drag field running together) + 6 default öbek prefabs (`GravityWell`, `WindTunnel`, `VortexTrap`, `DragField`, `MagneticPulse`, `TurbulentZone`). | ~150 | bench + smokes |
| **v1f-fields-j** | `FieldFormula::Reserved_J2` impl — Earth-oblateness gravity correction, fills the reserved enum slot from ADR-0067. **Aerodynamic drag is NOT field-shaped** (reads body velocity; ships as separate `AeroDynamicsComponent` per ADR-0073). Lands alongside the v6f aerospace cluster. Per ADR-0073 (Planned). | ~80 | ~2 |
| **v1g** | Island detection (single-thread incremental union-find, deterministic id-stable order); island-parallel solve via `crd-jobs` fibers. Sleeping (energy-threshold). 100-island parallel speed bench. | ~500 | ~15 |
| **v1g-contactmodify** | `IContactModifyCallback` API + **API-enforced pure-function constraint** (signature provides BodyId + Span<ContactPoint> only — NO World handle, NO RNG, NO time, NO external state) + post-modify sort by stable feature_id to recover determinism even though callback fires in fibre-arrival order. Sandbox demo: one-way platforms. Per ADR-0068 §10.6. **Lands AFTER v1d basic dispatch is stable** — not a v1l close requirement; surface frozen at v1l, impl fills inside it. | ~250 | ~5 |
| **v1h** | Scene queries: ray, sphere overlap, sphere/capsule sweep, closest-point. Single API on top of broadphase + narrow. | ~400 | ~20 |
| **v1i** | Capsule kinematic character controller (sweep + slide + step + ground-detect). Reference: Bullet `btKinematicCharacterController` + DigitalRune. | ~500 | ~10 |
| **v1j** | World snapshot (CRDR `'EYLM'` artifact, ADR-0063 §7) + replay test harness (record inputs + RNG seed → re-run on the same machine → assert hash; CI matrix asserts hash matches across MSVC/clang/gcc × x64/ARM in v9b). | ~400 | ~10 |
| **v1k** | Sandbox integration: spawn 100 falling boxes + ragdoll + character controller; `.collider.toml` cooker handler; ImGui debug viz (contact normals, AABB tree, islands). Profile against the existing renderer. | ~600 | smokes |
| **v1l** | Phase 3.1 v1 close: API surface freeze (sizeof + version `static_assert`s on `crd-eylem` public types) — locks every public type from v1a-material-a (Material), v1b-a (BodyId pool layout), v1b-b (ColliderId encoding), v1f-fields-* (ForceFieldComponent + 9 formulas + 6 falloffs + 5 mass-coupling + 5 composition + 4 trigger), v1c-sensor (ColliderFlags), v1d-callback-* (ContactEvent / TriggerEvent / ContactPairFlags), v1g-contactmodify (IContactModifyCallback + ContactPoint). 14-config sweep clean (Win × 8 + Linux × 6 per `scripts/full-sweep.ps1`); phase doc + ADR README updates. | small | freeze tests |
| **v1l-test-conservation** | Conservation-law test infrastructure per ADR-0075: kinetic energy + angular momentum + linear momentum + total energy + constraint violation + sleep-wake idempotency. Runs on SI solver in v1l; expanded as new solvers ship. Per ADR-0075 (Accepted). | ~300 | ~12 |
| **v1l-test-closedform** | Closed-form regression tests per ADR-0075: pendulum period, projectile range, SHO, drag terminal velocity, circular motion, rolling ball, two-body Kepler orbit. Catches "self-consistent but wrong" failures. | ~250 | ~10 |
| **v1l-test-stress** | 1000-box stack + 30-link humanoid + broadphase-co-located + XPBD-particle stress benches per ADR-0075. Bench-style: regression-baselined. | ~200 | bench |

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
| **v4d-cine-a** | NEW MODULE `crd-eylem-cine` skeleton + animation-curve evaluator + kinematic-body-driven-by-curve binding per ADR-0074 (Planned). Animator-friendly: animation curves drive `RigidBodyType::Kinematic` body's pose; engine infers velocity (per ADR-0068 §1 Kinematic-with-velocity-inference). | ~400 | ~6 |
| **v4d-cine-b** | Pre-roll simulation (let scene settle for N steps before take starts) + per-shot physics overrides (gravity scalar, time scale per scene/take) per ADR-0074 (Planned). | ~250 | ~5 |
| **v4d-cine-c** | Slow-motion substepping (200 Hz physics for 24fps slow-mo render) per ADR-0074 (Planned). | ~200 | ~4 |

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
| **v6c-urdf** | URDF importer per ADR-0071 (Planned). Standard ROS robot description; gateway to ROS / Gazebo / IsaacSim ecosystems. | ~400 | ~6 |
| **v6c-sdf** | SDFormat importer per ADR-0071 (Planned). Gazebo's native format; Drake supports it; ROS 2 ecosystem standard. | ~300 | ~5 |
| **v6c-mjcf** | MJCF importer per ADR-0071 (Planned). MuJoCo's native format; de facto RL-research robot description. | ~350 | ~6 |
| **v6c-actuators** | Motor model catalogue per ADR-0071 (Planned): servos / BLDC / stepper / hydraulic / pneumatic. Industrial robotics + factory simulation; AGX Dynamics parity target. | ~500 | ~10 |
| **v6d-nonsmooth-newton** | Drake-class nonsmooth Newton solver per ADR-0070 (Planned). Best contact accuracy for manipulation; selection guidance documents when to pick over SI. | ~700 | ~15 |
| **v6e-sensor-a** | IMU sensor per ADR-0072 (Planned): accel + gyro + magnetometer with noise / bias / drift models. Standard robotics. | ~250 | ~5 |
| **v6e-sensor-b** | LIDAR + ultrasonic + IR proximity raycast sensors per ADR-0072 (Planned). | ~300 | ~6 |
| **v6e-sensor-c** | Threshold-event sensors (velocity/energy/force/position thresholds; sleep/wake events; joint-limit events; solver-iteration warnings) per ADR-0072 (Planned). | ~200 | ~5 |
| **v6f-aero-a** | Variable-mass body API + Tsiolkovsky integration (`set_body_mass()` mid-step contract; impulse correction) per ADR-0073 (Planned). | ~200 | ~4 |
| **v6f-aero-b** | Aerodynamic force model (`AeroDynamicsComponent` + `AeroForceEvaluator`): lift/drag with attack-angle curves, induced drag, viscous coefficients per ADR-0073 (Planned). NOT field-shaped (reads body velocity). | ~400 | ~8 |
| **v6f-aero-c** | Atmospheric model (US Standard Atmosphere 1976 + exponential fallback) per ADR-0073 (Planned). | ~250 | ~5 |
| **v6f-aero-d** | Propulsion model (thrust vector + gimbal + throttle + Isp) + multi-body separation events per ADR-0073 (Planned). `Reserved_J2` field formula impl ships in v1f-fields-j alongside this. | ~300 | ~6 |

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
| **v9b-test-cross-engine** | Box2D + Bullet integration (test-only deps via CPM.cmake) + canonical-scene comparison harness per ADR-0075. "Within 2× best-of-three" elite-tier bar. | ~400 | ~6 |
| **v9b-test-drift** | 60-minute long-duration drift CI jobs across 14 configs per ADR-0075. Snapshot-hash divergence rate-bounded. | ~100 | nightly |
| **v9b-test-property** | Property-based random-scene invariants via Catch2 generators per ADR-0075. 100 random scenes per test. | ~200 | 100/scene |
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
