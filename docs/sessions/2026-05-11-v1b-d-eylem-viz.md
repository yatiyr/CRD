# Session — 2026-05-11 — v1b-d — `crd-eylem-viz` companion module

## Goal

Per phase plan §v1b-d. Companion module bridging eylem POD types
(`RigidBodyComponent`, `ColliderComponent`) to `crd-draw`'s
VisualizerRegistry, deferred from v1a-draw d3 because it required
RigidBodyComponent / ColliderComponent which only landed at v1b-c.

Architectural intent (ADR-0066 §13 dependency-inverted plug-in):
`crd-eylem` substrate stays free of any rendering dep so headless / DAW
/ cooker / scientific-computing builds omit it cleanly; `crd-draw`
stays free of any eylem dep so DAW-only builds omit eylem; the
companion `crd-eylem-viz` module is the bridge that knows about both
and is opt-in at link time.

## What we built / changed

- **`engine/eylem-viz/`** — new module:
  - `include/crd/eylem_viz/eylem_viz.hpp` — public API: single
    `register_eylem_visualizers(VisualizerRegistry&, BodyPool&,
    ColliderPool&)` entry point.
  - `src/visualizers.cpp` — implementation. Two visualizer functions
    + the registration helper. Pool refs captured at register time
    into file-scope `static` pointers (v1b-d-tier solution; one
    (BodyPool, ColliderPool) pair per process).
  - `CMakeLists.txt` — STATIC library, depends on `crd-eylem` +
    `crd-eylem-rigid3d` + `crd-draw` + `crd-scene` + `crd-math` (all
    PUBLIC).

- **`engine/draw/include/crd/draw/visualizer_registry.hpp`** — extended
  `VisualizerContext` with `const crd::scene::World* world = nullptr`.
  Defaulted to nullptr → existing default_visualizers (Transform →
  axis_triad, which doesn't need cross-component lookup) remain
  source-compatible. eylem-viz needs the World pointer to call
  `world->get_component<Transform>(entity)` — without it the
  visualizer can't position itself in world space.

- **`engine/draw/src/visualizer_registry.cpp`** — `invoke_all` now
  passes `&world` into the context (was previously omitted; the
  default_visualizers Transform path didn't notice).

- **`CMakeLists.txt`** (root) — `add_subdirectory(engine/eylem-viz)`.
- **`tests/CMakeLists.txt`** — `add_subdirectory(eylem-viz)`.
- **`tests/eylem-viz/`** — new test target:
  - `test_visualizers.cpp` — 5 cases, 14 assertions.
  - `CMakeLists.txt`.

## Visualizer behaviour shipped

### `RigidBodyComponent` → velocity arrow

- Fires only when `DebugVizComponent::ShowVelocity` flag is set.
- Reads `Transform.world` for arrow origin (so TransformPropagation
  must have run; PostRender phase guarantees this).
- Reads `body_pool.read(rbc.body_id).linear_velocity` for arrow direction.
- Speed-threshold-gated: skips when `|v| < 0.001` to avoid drawing a
  tiny dot on stationary bodies.
- Length scaled by `speed × DebugVizComponent::scale`, capped to 5 m
  (so a 100 m/s body doesn't draw a 100 m arrow off-screen).
- Colour: yellow. Depth: Always (visible through geometry). Width: 2 px.

### `ColliderComponent` → wireframe matching shape kind

- Fires only when `DebugVizComponent::Wireframe` flag is set.
- Reads `Transform.world` for body origin; composes
  `body_origin + collider.local_position` for the world-space pose.
  v1b-d ignores `local_rotation` composition (sphere is rotation-
  invariant; box + capsule first-pass wireframes ignore rotation).
  Proper world × local matrix composition lands at v1f when
  eylem-aero / eylem-cine consumers actually need it.
- `ColliderShape::Sphere` → `sphere_wire_to(center, radius)` with 16
  longitude meridians × 8 latitude segments (default).
- `ColliderShape::Box` → `box_wire_to(translation-only Mat4f via
  from_trs, half_extents)` — exactly 12 edges.
- `ColliderShape::Capsule` → `capsule_wire_to(a, b, radius)` with
  axis = local Y per the `ColliderCapsule` convention.
- `ConvexHull` / `Plane` / `TriangleMesh` / `Heightfield` / `Sdf` —
  no-op for v1b-d. These shape kinds get wireframes alongside their
  narrow-phase impls (v1d / v1d-mesh / v1d-hf / Phase 3.1.5 sdf
  consumer). Not an error — explicit absence-of-viz.
- Colour: cyan. Depth: Always. Width: 1 px.

### Joint visualizer

Registered as a **commented no-op shell** (the comment is in
`register_eylem_visualizers` body). `JointComponent` doesn't exist yet
(joints land at v1f). The shell tells the v1f author exactly where to
land the joint visualizer without re-discovering the eylem-viz module.

## Decisions made

- **Companion module, not in `crd-eylem` itself.** ADR-0066 §13
  dependency-inverted plug-in pattern. Substrate modules stay free of
  rendering deps; the bridge is opt-in.
- **Pool refs as file-scope statics, not per-context handles.**
  v1b-d-tier solution. Captureless lambdas / function pointers can't
  carry state. The file-scope static is the simplest single-path path
  to working visualizers without bloating components. One
  `(BodyPool, ColliderPool)` pair per process; revisit when a
  multi-eylem-world workload appears (out of v1b-d scope per
  phase plan ~150 LOC bound).
- **Extended `VisualizerContext` with `const World*` (defaulted
  nullptr).** The default-nullptr lets existing default_visualizers
  stay source-compatible (Transform → axis_triad doesn't use World).
  Visualizers that need World null-check it. Non-breaking change to
  the d4-frozen `crd-draw` API surface — defaulted-init field on a
  POD struct, no consumer needs to change.
- **`Wireframe` + `ShowVelocity` flag-gated** per the existing
  `DebugVizComponent` flag taxonomy. ColliderComponent visualizer
  defaults to ON via Wireframe (the typical case); RigidBodyComponent
  velocity arrow opt-in via ShowVelocity (avoids visual noise when
  every body has a yellow arrow on by default).
- **Speed threshold + max arrow length** pinned to avoid degenerate
  visuals (tiny dots / off-screen arrows). Numbers (`0.001 m/s`,
  `5 m`) chosen as defensible defaults; refine when sandbox demo
  surfaces real workloads.
- **Tests use `crd::memory::TlsfAllocator{16 MB}` per fixture** instead
  of `crd::memory::default_allocator()`. Production-realistic, catches
  leaks via TLSF's destructor assert, exercises the same allocator
  path the production engine uses. Saved as feedback memory
  `feedback_named_allocators_in_tests.md` so all future tests use this
  convention. Initial 1 MB attempt CRD_FATAL'd "out of memory" because
  ECS + BodyPool + ColliderPool storage adds up; bumped to 16 MB which
  is comfortable headroom.
- **No SchedulePhase coupling in eylem-viz.** The visualizers are pure
  functions invoked by the existing `DebugVizSystem` (which already
  runs in `SchedulePhase::PostRender`). eylem-viz adds NO system, NO
  registration outside the registry call. Keeps the module's surface
  area and lifetime story trivial.

## Files touched

- `engine/eylem-viz/include/crd/eylem_viz/eylem_viz.hpp` — new (~50 LOC, doc-heavy)
- `engine/eylem-viz/src/visualizers.cpp` — new (~190 LOC: two visualizer fns + registration)
- `engine/eylem-viz/CMakeLists.txt` — new
- `engine/draw/include/crd/draw/visualizer_registry.hpp` — extended `VisualizerContext` with `const World*`
- `engine/draw/src/visualizer_registry.cpp` — pass `&world` into context
- `CMakeLists.txt` (root) — add_subdirectory
- `tests/eylem-viz/test_visualizers.cpp` — new (~230 LOC, 5 cases)
- `tests/eylem-viz/CMakeLists.txt` — new
- `tests/CMakeLists.txt` — add_subdirectory
- `docs/phases/phase-3.1-eylem.md` — header + v1b-d row promoted to ✅
- `context.md` — Coming up next + Last shipped milestone

## Tests / verification

| Target | Result |
|---|---|
| crd-eylem-viz-tests | **14 assertions / 5 cases PASS** |
| ctest --preset win-debug | **1051/1051 PASS** (was 1046; +5 from new eylem-viz cases) |
| ctest --preset win-shipping | **1048/1048 PASS** (was 1043; +5; release excludes assert-only cases) |

Full 14-config sweep deferred to v1b-e cluster close (after sandbox
demo lands).

## Known issue surfaced + fixed during the slice

Two ASCII-only test names initially used `→` (U+2192 RIGHTWARDS ARROW)
which trips the `crd-no-non-ascii-test-names` CI guard from v0e
post-mortem (UTF-8 argv mojibake on Windows ACP). Fixed by replacing
with ASCII-only "emits ... when shape=...". Reminder: the existing
guard at `tests/CMakeLists.txt` enforces this; new test names need to
stay in pure ASCII.

## Decision deltas vs phase plan §v1b-d

- Phase plan: "RigidBodyComponent → axis triad (always) + velocity
  arrow (when ShowVelocity flag)" → **Implemented as velocity arrow
  only**. The "axis triad always" sub-clause was removed because
  `register_default_visualizers` already registers Transform → axis
  triad; an entity with both Transform AND RigidBodyComponent (the
  typical case) gets the axis triad from the Transform visualizer.
  Adding a second axis triad from the RBC visualizer would double-draw
  it. Documented inline.
- Phase plan: "~150 LOC + ~2 tests" → **Shipped ~250 LOC + 5 tests**.
  Extra LOC is the file-scope-static pool indirection + the world-
  pointer extension to VisualizerContext + the per-shape-kind switch
  in the collider visualizer + speed-threshold/max-length guards in
  the velocity arrow. Extra tests are the static-body-no-arrow case
  + the flag-gating test + an explicit box-edge-count assertion (the
  pinned 12-edge contract from d1).
- Test allocator convention bumped from `default_allocator()` to
  `TlsfAllocator{16 MB}` per the user's same-day feedback. Saved as
  project-wide convention in feedback memory.

## Next session starts with

- **v1b-e**: sandbox demo. Spawn 3-5 rigid bodies (1 sphere, 1 box,
  1 capsule) with Transform + RigidBodyComponent + ColliderComponent +
  DebugVizComponent{AxisTriad+Wireframe+ShowVelocity} at +y=5;
  gravity wired; smoke verifies bodies' Transform.translation.y
  descends frame-over-frame. Bodies fall through the world (no
  collision until v1c broadphase + v1d narrow phase). **CLOSES v1b
  cluster — full 14-config sweep follows.** ~100 LOC + smoke.
