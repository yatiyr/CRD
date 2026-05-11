# Session — 2026-05-11 — v1b-e — sandbox eylem demo + render interpolation (Fix Your Timestep)

## Goal

Per phase plan §v1b-e: spawn 3 rigid bodies into the sandbox (sphere /
box / capsule) at +y=5 with full ECS component set, prove the
end-to-end wire ECS → BodyPool → EylemSystem → Transform → eylem-viz
runs in the live renderer. Bodies fall through the world (no
broadphase yet — that's v1c). **In-scope expansion (not in original
v1b-e):** user flagged that variable-rate render of fixed-step physics
produces visible stutter. Addressed in-slice with the canonical Glenn
Fiedler "Fix Your Timestep" §5 interpolation: integrator snapshots
curr → prev at the start of each substep, then a variable-rate
render-side system lerps between them by `accumulator / fixed_dt`.

## What we built / changed

### Sandbox demo (the originally-scoped v1b-e)

- **`sandbox/src/sandbox_layer.{hpp,cpp}`** — wired eylem into `init_scene_world`:
  - 16 MB dedicated `crd::memory::TlsfAllocator{..,"eylem-tlsf"}` (named per project allocator-discipline convention).
  - `BodyPool` with `max_bodies=256`, `ColliderPool` with `capacity_per_kind=256`.
  - `PhysicsConfig{ gravity=(0,-9.81,0), fixed_dt=1/64 (exact f32) }`.
  - `EylemSystem` registered into Physics phase.
  - `register_eylem_visualizers(viz_registry, body_pool, collider_pool)`.
  - 3 demo entities at x=−2/0/+2, y=5: each gets `Transform` (with `Transform.world` pre-baked via `from_trs` for first-frame viz) + `RigidBodyComponent` (sync_to_transform=1, inv_mass=1, zero damping) + `ColliderComponent` (sphere r=0.5 / box half=0.5×0.5×0.5 / capsule r=0.4 h=0.5) + `DebugVizComponent{AxisTriad|Wireframe|ShowVelocity, scale=0.5}`.
  - Per-frame loop calls `world.step_fixed(dt, fixed_dt, max_substeps=8)` when eylem initialised.

- **Component-registration order fix:** `RigidBodyComponent` + `ColliderComponent` registered BEFORE the d3 demo entities are spawned. Prior order (register after spawn) crashed when first entity carrying RBC was inserted into a yet-to-grow SparseSet pool — root cause confirmed via cdb dump analysis. Saved as feedback `feedback_use_crash_dumps_first.md` mandating cdb-first crash-dump workflow over log-bisect.

### Render interpolation substrate (added in-slice)

- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/body_pool.hpp`** — `BodyChunkT<Lane>` grew:
  - +7 columns: `prev_pos_{x,y,z}` + `prev_rot_{x,y,z,w}` (mirror columns at end of struct).
  - Lane=8 chunk: 19 → 26 columns → 608 B → 832 B (~37% growth; acceptable for the smooth-render gain).
  - Lane=4 chunk: same shape.
- New public API on `BodyPool`:
  - `PrevState{ Vec3f position; Quatf rotation; } read_prev(BodyId)` — single-chunk lookup of just the prev pose.
  - `snapshot_state_to_prev()` — whole-chunk SIMD assignment per column (`tile.prev_pos_x = tile.pos_x; ...`). O(chunk_count); free-lane safe via `live[lane]==0` guards downstream.
  - `write(BodyId, RigidBody)` — TELEPORT semantics: also mirrors pos/rot into prev so the renderer's next lerp produces no visual jump. Used for spawning / level resets / network snap corrections.
  - `write_curr_only(BodyId, RigidBody)` — INTEGRATOR semantics: writes only curr columns; prev is preserved (was already snapshotted earlier in the substep). Used by `EylemSystem` per substep.
- Internal split: `store_curr_only_lane` writes the 19 curr columns + flags; `store_lane` calls it then also writes prev. `insert()` uses `store_lane` so newly-spawned bodies start with `prev == curr` (no first-frame visual jump from default-init prev).

### Integrator updates

- **`engine/eylem-rigid3d/src/eylem_system.cpp`** — `EylemSystem::run`:
  - First action: `m_body_pool->snapshot_state_to_prev()`. Captures the integrator's current pose into prev cols BEFORE this substep advances state.
  - Per-body integrator write now goes through `write_curr_only` (was `write`). Crucial: `write` would clobber prev with the just-integrated curr and the interpolation system would lerp from curr to curr (zero motion).

### World accessors

- **`engine/scene/include/crd/scene/world.hpp`** — two new public accessors:
  - `fixed_step_accumulator() → f64` — raw accumulator (what `step_fixed` maintains).
  - `fixed_step_alpha(fixed_dt) → f64` — clamped to [0, 1]; defensive when `fixed_dt <= 0`.

### Interpolation system

- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/interpolation_system.hpp` + `src/interpolation_system.cpp`** — new.
  - `RigidBodyInterpolationSystem : public crd::scene::ISystem`.
  - `phase() = SchedulePhase::PreRender`, `fixed_step() = false`, `name() = "RigidBodyInterpolationSystem"`.
  - Constructor takes `BodyPool&` + `PhysicsConfig&` (same pattern as EylemSystem; `fixed_dt` captured for the alpha-compute).
  - `run(World&)`:
    - `alpha = world.fixed_step_alpha(m_config.fixed_dt)`.
    - For each `(Transform, RigidBodyComponent)`:
      - Skip `rbc.sync_to_transform == 0` (user owns Transform).
      - Skip null / stale `body_id`.
      - `curr = body_pool.read(body_id)`; `prev = body_pool.read_prev(body_id)`.
      - `interp_pos = lerp(prev.pos, curr.pos, alpha)`.
      - `interp_rot = nlerp(prev.rot, curr.rot, alpha)` with **dot<0 short-arc fix** (negate effective curr if `dot(prev, curr) < 0` so the lerp takes the short arc; standard id Tech / UE4 anim pattern). Final `try_normalize`; fallback to identity on the degenerate zero-quat case (should never arise for unit-quat inputs).
      - Write via `world.set_translation` / `set_rotation_quat` (TransformDirtyFlag tagged → `TransformPropagation` (registered AFTER this system in PreRender) refreshes `Transform.world` this frame).

### Sandbox schedule wiring

- Restructured `init_scene_world` to register systems in the right phase order:
  - **PreRender phase**: `RigidBodyInterpolationSystem` registered FIRST, then `TransformPropagation`. Within a phase, registration order = run order per `world.cpp:807`. Reversing this would have `TransformPropagation` refresh world matrices from un-interpolated local poses → renderer sees one-frame-stale visuals.
  - **Physics phase**: `EylemSystem` (registration order doesn't matter; it's the only Physics system in this build).
  - **PostRender phase**: `DebugVizSystem` (unchanged).
- Eylem pool init + `PhysicsConfig` now happen BEFORE the PreRender systems are registered so the InterpolationSystem can capture the pool refs at construction.

### Tests

- **`tests/eylem-rigid3d/test_interpolation_system.cpp`** — new, 8 TEST_CASEs / +41 assertions:
  1. Schedule pins: phase=PreRender, fixed_step()=false, name="RigidBodyInterpolationSystem".
  2. `alpha=0 → prev`: integrate 1 substep, verify Transform holds prev (origin) not curr (post-step).
  3. `alpha=0.5 → midpoint`: hand-construct prev=A, curr=B via `write` + `write_curr_only`, step_fixed half a tick, verify Transform = (A+B)/2.
  4. **Multi-substep flow**: `frame_dt=2.5·fixed_dt`, `max_substeps=4` → 2 substeps run + alpha=0.5; verifies prev=pose_after_substep_1, curr=pose_after_substep_2, lerp = 2·g·dt² (midpoint between 1·g·dt² and 3·g·dt²).
  5. **Nlerp short-arc with non-degenerate antipodal pair**: prev=identity, curr=negated small Y-rotation (theta=0.2 rad). Without fix, lerp lands near 180° wrong. Asserts result `.w > 0.9F`, `.y > 0`, magnitude≈1.
  6. `sync_to_transform=0` honoured (Transform untouched).
  7. Null / stale body_id no-op (no crash, no write).
  8. `World::fixed_step_alpha` clamps to [0, 1] on irregular accumulator + `fixed_dt <= 0` sentinel returns 0.
- All 8 tests pass: `crd-eylem-rigid3d-tests` now 287 assertions / 32 cases (was 246/24, +41/+8).
- Fixture uses `crd::memory::TlsfAllocator{4 MB, .., "interp-test"}` per project convention.

### Tooling

- **WinDbg / cdb installed via `winget install Microsoft.WinDbg`.** Path:
  `C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe`.
- Saved as memories: `build_system.md` (workflow recipe) + `feedback_use_crash_dumps_first.md` (rule + reason). Non-interactive invocation:
  ```powershell
  & "<cdb-path>" -z .\crashes\crash_YYYYMMDD_HHMMSS.dmp `
      -y "srv*c:\symbols*https://msdl.microsoft.com/download/symbols" `
      -c "!analyze -v; ~*kn 30; q"
  ```

## Why this is correct (and not a workaround)

- The `write` (teleport) vs `write_curr_only` (integrator) split is the principled fix, not a flag. Two distinct caller intents → two distinct entry points. `store_curr_only_lane` is the shared core; `store_lane` is a thin wrapper that adds the prev mirror.
- The snapshot happens at the START of each substep, BEFORE the integrator writes new curr. This means in a multi-substep frame, prev ends up as `pose_after_substep_(N-1)` and curr as `pose_after_substep_N` — Fiedler's one-frame-latency tradeoff, which is the industry standard.
- Schedule ordering pinned by registration order in PreRender: `Interp → TransformPropagation`. World accessors expose accumulator + alpha so other backends (e.g. v6 reduced-coord articulations, v9 differentiable) can write their own interpolators against the same contract.
- `nlerp` over `slerp`: at 60 Hz physics the angular delta per substep is tiny → nlerp visually indistinguishable from slerp at ~6× the throughput (no acos / sin). The dot<0 short-arc fix is the only inhabited branch difference vs naive lerp.
- Tests use named `TlsfAllocator` per project convention (never `default_allocator()` — saved as feedback `feedback_named_allocators_in_tests.md` on v1b-d).

## Known issue addressed during this session

- **Component registration order:** registering `RigidBodyComponent` + `ColliderComponent` AFTER d3 demo entities had been spawned crashed at insertion time. Root-caused with cdb (after the user pointed out my prior log-bisect approach perturbed inlining + timing). Fix: register all components UPFRONT in `init_scene_world`, before any entity is added. Saved feedback memory mandating cdb-first crash-dump workflow.
- **Test name UTF-8 argv mojibake:** an em-dash `—` in one TEST_CASE name tripped the v0e-era UTF-8 → ANSI mismatch under ctest, even though the test runs fine standalone. Renamed to `:` separator. The `crd-no-non-ascii-test-names` CI guard exists specifically for this class of bug.

## Tests

- win-debug: `cmake --build --preset win-debug && ctest --preset win-debug --output-on-failure` → **1059/1059 PASS** (was 1051; +8 from the new interpolation suite). Total time 21.6s.
- Sandbox smoke: `crd-sandbox.exe` runs clean for 4s; 3 rigid bodies spawn at +y=5, init logs report `fixed_dt=0.01562s (64.0 Hz)`.
- Full 14-config sweep: launched in background with `-Reconfigure` (clean rebuild for win-release + win-relwithdebinfo — guards against the C4789 LTCG buffer-overrun bug that triggers on stale .obj when struct sizes grow, per CLAUDE.md known issues; `BodyChunk` grew 608 B → 832 B in this slice, exactly the C4789 trigger pattern). Result will be appended on completion.

## Flagged for v1c+

- **Double-write to Transform per frame:** `EylemSystem` (Physics) writes integrated curr via `set_translation`/`set_rotation_quat`, then `RigidBodyInterpolationSystem` (PreRender) overwrites with the lerped value. Functionally correct, two dirty-flag taggings per body per frame. Optimisation: have `EylemSystem` skip the Transform write when an Interpolation System is registered against the same pool. Defer until profiling reveals it matters.

## Pending (user-side)

- **Visual smoothness eye-test of the sandbox demo** — the deliverable is "smooth physics motion at variable framerate"; the build green confirms no-regression but not subjective smoothness. Run `crd-sandbox.exe` and watch the 3 falling bodies.

## Pinned next slice

- **v1b (cluster close)** once full sweep returns PASS across all 14 configs (Win × 9 build/test/smoke + Linux × 6 build/test). Then v1c (broadphase: dynamic AABB tree per Catto GDC 2019).
