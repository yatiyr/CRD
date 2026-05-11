# Session — 2026-05-11 — v1b-c — RigidBodyComponent + ColliderComponent + EylemSystem

## Goal

Per phase plan §v1b-c (~150 LOC + ~3 tests). First eylem entity
integrating motion in the ECS world. Connect the v1a interface +
v1b-a/b storage pools to the scene's Schedule via a System running in
`SchedulePhase::Physics`. The honest v1b-c scope is "bodies fall through
the world" — no broadphase (lands v1c), no SI solver (lands v1e). Just
gravity + position/rotation integration so we can prove the
`Component → Pool → Integrator → Transform → TransformPropagation`
end-to-end wire.

## What we built / changed

- **`engine/eylem/include/crd/eylem/components.hpp`** — new header.
  - `RigidBodyComponent` POD: `BodyId body_id` + `u8 sync_to_transform`
    + 3-byte pad. `static_assert(sizeof == 8)` and `alignof == 4`.
  - `ColliderComponent` POD: `ColliderId collider_id`. `static_assert(sizeof == 4)`.
  - Both register via `world.register_component<T>(StorageHint::SparseSet)`
    per ADR-0050 (sparse vs total entity count; lifecycle dominated by
    add/remove not iteration).
  - Doc explains the `attach_rigid_body(world, e, scene, RigidBody{...})`
    helper pattern (single canonical attachment path; lands at v1b-d/e
    when the sandbox actually exercises it).

- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_system.hpp`** — new.
  - `EylemSystem : public crd::scene::ISystem`.
  - `phase() = SchedulePhase::Physics`, `fixed_step() = true`,
    `name() = "EylemSystem"`.
  - Constructor takes `BodyPool&` + `PhysicsConfig&` snapshot
    (gravity + fixed_dt). Caller is responsible for passing the same
    `fixed_dt` to `World::step_fixed`.

- **`engine/eylem-rigid3d/src/eylem_system.cpp`** — implementation.
  - `run(World&)` queries `(Transform, RigidBodyComponent)`. For each
    entity:
    - Skip if `body_id.is_null()` or pool doesn't `contains()` it.
    - Skip if static (`inv_mass == 0`) — matches the RigidBody
      convention from `rigid_body.hpp:60`.
    - Integrate linearly: `v += g·dt; v *= (1 - lin_damp·dt); p += v·dt`.
    - Integrate angularly: `ω *= (1 - ang_damp·dt); q = normalize(q + 0.5·dt·(ω_quat · q))`
      (standard explicit Euler on quaternion derivative
      `q̇ = 0.5 · ω_quat · q`).
    - Write back to `BodyPool` via `pool.write(id, body)`.
    - Sync back to `Transform` via `World::set_translation` /
      `set_rotation_quat` (so TransformPropagation tags
      `TransformDirtyFlag` and refreshes `Transform.world` in
      `PreRender`). Per-entity opt-out via
      `RigidBodyComponent::sync_to_transform = 0` for cinematic-bridge
      use cases (v4d).

- **`engine/eylem-rigid3d/CMakeLists.txt`** — added `crd-scene` PUBLIC
  dependency. EylemSystem inherits `ISystem`, calls `World::query` +
  `set_translation` + `set_rotation_quat`. Components themselves
  (RigidBodyComponent / ColliderComponent) live in `crd-eylem` so
  cookers / tooling / other systems can register & serialise them
  without depending on the concrete pool family.

- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_rigid3d.hpp`**
  — added `eylem_system.hpp` to umbrella.

- **`engine/eylem/include/crd/eylem/eylem.hpp`** — added `components.hpp`
  to umbrella.

- **`tests/eylem-rigid3d/test_eylem_system.cpp`** — new (5 cases):
  - `RigidBodyComponent + ColliderComponent register with SparseSet
    hint`: round-trip via `add_component` + `get_component`, asserts
    handle pack/unpack matches.
  - `EylemSystem reports Physics phase + fixed_step()`: pins the
    schedule contract.
  - `EylemSystem integrates motion under gravity`: 60 substeps under
    -9.81 m/s² from rest, asserts Transform.translation.y matches
    discrete-Euler closed form `Δp = g·dt²·N·(N+1)/2` within 1e-3.
    Pool state mirrors the synced Transform.
  - `EylemSystem under World::step_fixed runs expected substep count`:
    uses **exactly-representable ratio** (fixed_dt=1/64, frame_dt=10/64)
    to avoid f64-floor slop on 1/60 (10*1/60 in FP rounds slightly
    below 10 → floor=9 → off-by-one substep). Asserts the schedule
    runs exactly 10 substeps.
  - `static body (inv_mass==0) does not integrate`: 60 substeps under
    gravity; assert position unchanged.

- **`tests/eylem-rigid3d/CMakeLists.txt`** — added `test_eylem_system.cpp`
  + `crd-scene` link.

## Plain-English explanation

Until now the eylem stack was three independent layers: an interface
(`crd-eylem` POD types), pool storage (`crd-eylem-rigid3d` BodyPool +
ColliderPool), and the scene's ECS (separately). v1b-c wires them into
one runtime: an entity carries a `RigidBodyComponent` holding a
`BodyId` handle into the pool; the `EylemSystem` runs in the
`Physics` phase of the scene's 7-phase schedule and integrates motion
each fixed step.

The integrator is intentionally minimal — the v1b-c scope per the phase
plan is "bodies fall through the world." No collision (lands v1c
broadphase + v1d narrow phase), no constraints (lands v1f), no
sub-stepped solver (lands v1e). What v1b-c proves is that the
end-to-end wire from `pool.insert(body)` → `world.add_component(e,
RigidBodyComponent{id})` → `EylemSystem::run` → `pool.read/write` →
`World::set_translation` → `TransformPropagation refresh` works.

The schedule integration is the subtle part. ECS systems with
`fixed_step() = true` get called N times per `World::step_fixed(dt,
fixed_dt, max_substeps)`, where N = floor(accum / fixed_dt). The
schedule owns the accumulator. The System reads its per-step `dt` from
the `PhysicsConfig` it was constructed with — caller is expected to
pass the same `fixed_dt` to both. (A future refinement: pass dt as a
run() argument so the schedule is the single source of truth. Reserved
for v1f when per-system fixed_dt lands.)

## Decisions made

- **Components live in `crd-eylem`, System lives in `crd-eylem-rigid3d`.**
  Components are pure handles (BodyId / ColliderId); no need to depend
  on the concrete pool family. The cooker / editor / tooling can
  serialise these PODs without pulling rigid3d's AoSoA-8 storage code.
- **`StorageHint::SparseSet`** for both components per ADR-0050. Rigid
  bodies are hundreds–thousands per scene, not millions; archetype
  storage would explode on every spawn/despawn cycle.
- **Lifecycle hook approach: caller-owned, not on_add/on_remove.** The
  phase plan mentioned "on_add hook allocates pool slot" but the
  scene's ComponentRegistry doesn't expose lifecycle callbacks at this
  granularity (would require a new IComponentIndex shell). For v1b-c
  we ship the simpler caller-owned attach pattern (`pool.insert` then
  `world.add_component`); the canonical helper lands at v1b-d/e where
  the sandbox actually instantiates bodies. v1f can revisit and add
  scene-level on_add hooks if a workload demands it.
- **Per-entity `sync_to_transform` flag** (default 1). Matches the
  cinematic-bridge use case (ADR-0074 v4d): a kinematic body driven by
  an animation curve has its Transform written by an animation system,
  not the physics integrator. Defaulting to 1 means dynamic bodies "just
  work" in the typical case.
- **Linear damping applied AFTER gravity integration, BEFORE position
  update.** Matches Bullet / PhysX convention: damping acts as
  exponential drag on the post-gravity velocity, not as a force.
- **Small-angle quaternion update via `q + 0.5·dt·(ω_quat · q)` then
  normalize.** Standard explicit-Euler on `q̇ = 0.5·ω_quat·q`. Sufficient
  for v1b-c's "bodies fall through the world" honest scope; v1e SI
  solver replaces this with a substepped integrator that handles large
  angular velocities better.
- **Static bodies (`inv_mass == 0`) skip integration.** Matches the
  RigidBody POD's documented convention (`rigid_body.hpp:60`). Avoids
  pointless work + makes the test for "static body doesn't move" trivial.
- **Test for substep count uses fixed_dt=1/64** (exactly representable)
  not 1/60 (FP slop causes off-by-one floor). Acknowledged as a known
  limitation: a future v1f refinement could add a deterministic-mode
  flag that snaps the accumulator to the nearest dt within ±epsilon
  before flooring. For v1b-c the test sidesteps it via power-of-two ratio.

## Files touched

- `engine/eylem/include/crd/eylem/components.hpp` — new (~70 LOC, doc-heavy)
- `engine/eylem/include/crd/eylem/eylem.hpp` — +1 line (umbrella)
- `engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_system.hpp` — new (~80 LOC)
- `engine/eylem-rigid3d/src/eylem_system.cpp` — new (~115 LOC)
- `engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_rigid3d.hpp` — +1 line (umbrella)
- `engine/eylem-rigid3d/CMakeLists.txt` — added crd-scene PUBLIC dep
- `tests/eylem-rigid3d/test_eylem_system.cpp` — new (~220 LOC, 5 cases)
- `tests/eylem-rigid3d/CMakeLists.txt` — added test src + crd-scene link

## Tests / verification

| Target | Result |
|---|---|
| crd-eylem-rigid3d-tests | **246 assertions / 24 cases PASS** (was 215/19; +31 / +5) |
| ctest --preset win-debug | **1046/1046 PASS** (was 1041; +5 from new EylemSystem cases) |
| ctest --preset win-shipping | **1043/1043 PASS** (was 1038; +5; release excludes debug-only `#if CRD_ENABLE_ASSERTS` cases) |

Sweep cadence: full 14-config sweep deferred to v1b cluster close (after
v1b-d eylem-viz + v1b-e sandbox demo) per the user's instruction. v1b-c
verified on win-debug + win-shipping per the user's request — both
the development build and the production-shipping build pass.

## Decision deltas vs phase plan §v1b-c

- Phase plan: "on_add hook allocates pool slot + on_remove hook frees
  pool slot." → **Implemented as caller-owned attachment** (pool.insert
  then add_component). Scene's ComponentRegistry doesn't expose
  per-component lifecycle callbacks at this granularity; adding them
  would be a v1f scope expansion. Documented in components.hpp.
- Phase plan: "~150 LOC + ~3 tests." → **Shipped ~210 LOC + 5 tests.**
  Extra LOC is the per-entity `sync_to_transform` opt-out path, the
  static-body fast-path, and the doc blocks. Extra tests are the
  static-body assertion + the explicit name() pin (defensive against
  accidental rename).

## Next session starts with

- **v1b-d**: `crd-eylem-viz` companion module per phase plan. Depends on
  `crd-eylem` + `crd-eylem-rigid3d` + `crd-draw`.
  `register_eylem_visualizers(VisualizerRegistry&)` registers:
  RigidBodyComponent → axis triad (always) + velocity arrow (when
  ShowVelocity flag); ColliderComponent → wireframe matching shape
  kind (sphere_wire / box_wire / capsule_wire from crd-draw). Joint
  visualizer = no-op shell (joints land v1f). ~150 LOC + ~2 tests.
- **v1b-e**: Sandbox demo — spawn 3-5 rigid bodies + Transform +
  RigidBodyComponent + ColliderComponent + DebugVizComponent, gravity
  wired, smoke verifies bodies' Y descends frame-over-frame. ~100 LOC
  + smoke. **CLOSES v1b cluster — full 14-config sweep follows.**
