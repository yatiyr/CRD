# 2026-05-07 — Phase 3.0 v1j: Transform + TransformPropagation (cross-domain robust)

**Status at start:** Phase 3.0 v1i shipped. IComponentIndex framework + ChangeDetect + AsyncAware + 5 reserved shells. Scene tests 192 / 34669, six-config 708/708.

**Status at end:** v1j shipped — first concrete `ISystem` consumer that ties together the entire 8-layer architecture (L1 entities, L2 archetype storage, L3 ChildOf relations + reverse index, L4 PreRender phase + Commands, L5 ChangeDetect on Transform writes). Math layer extended with cross-domain primitives (`from_euler` with explicit ordering, `from_to_rotation`, `from_trs`, `to_trs` with negative-determinant handling). Six-config 727/727 / 724 release / 17 smokes. Scene tests 211 / 34716.

---

## Goal of this session

Land Layer 5's **first real consumer**: the Transform component + TransformPropagation system that drives every transform-bearing entity's world matrix from its TRS triple via the ChildOf hierarchy. The user explicitly asked for cross-domain robustness — the system must serve games, robotics, aerospace, AND DAW spatial audio.

## Cross-domain robustness — what shipped

| Domain | v1j coverage |
|---|---|
| **Games** | f32 Transform with TRS + cached world. Default. |
| **Robotics** | Joint chains via ChildOf; multiple rotation-set APIs (`set_rotation_axis_angle` for servo control, `set_rotation_euler` with explicit ordering, `set_rotation_quat_unnormalized` for IMU integration). Determinism guaranteed. |
| **Aerospace** | f32 default; `crd::math::Transformd` (f64) already exists at math layer. Domains register their own f64 component + propagation system using v1j patterns. v1n freeze pinned the path. |
| **DAW (3D audio)** | f32 Transform; scale ignored. Same propagation. Audio engine reads `transform.world` per audio block. |

The same propagation algorithm serves all four. The robustness is layered:
1. **Multiple rotation-set APIs** for domain-flexible input.
2. **Robust set_world** with try-variant for validating decomposition.
3. **Determinism guarantee** — verified by bit-exact replay test.
4. **Numerical-precision limits documented** with f64-component escape hatch.
5. **Hierarchy depth limit** (kMaxTransformDepth = 256) asserted in debug.

## What shipped

### New module files

```
engine/scene/include/crd/scene/transform.hpp                   ~110 LOC
engine/scene/include/crd/scene/transform_propagation.hpp       ~70 LOC
engine/scene/src/transform_propagation.cpp                     ~140 LOC
tests/scene/test_transform.cpp                                 ~330 LOC, 19 cases
```

### Math library extensions (`engine/math/include/crd/math/quat.hpp`)

Added in `crd-math` (header-only) — the right home per advisor decision #1:

- `EulerOrder` enum (4 conventions: XYZ_Intrinsic / ZYX_Intrinsic / XYZ_Extrinsic / ZYX_Extrinsic).
- `from_euler(x, y, z, order)` — explicit-order Euler-to-quat conversion.
- `from_to_rotation(from, to)` — shortest-arc rotation; handles antiparallel via axis-seed fallback.
- `from_trs(translation, rotation, scale)` — column-major TRS matrix construction.
- `to_trs(matrix, t_out, r_out, s_out)` → bool — robust decomposition. Returns false on singular columns; succeeds with negative X-scale on negative-determinant input (CAD/robotics-URDF mirror handedness preserved).

### Scene-layer additions

- `Transform` component: `Vec3f translation`, `Quatf rotation`, `Vec3f scale`, `Mat4f world`. 96 bytes total. Documented numerical limits + f64-escape-hatch path.
- `TransformDirtyFlag` SparseSet marker — frame-scoped dirty flag.
- `kMaxTransformDepth = 256` constant + debug-mode CRD_ASSERT during dirty-subtree marking.
- World writer API: `set_translation` / `set_rotation_quat` (auto-normalised) / `set_rotation_quat_unnormalized` / `set_rotation_axis_angle` / `set_rotation_euler` / `set_rotation_from_to` / `set_rotation_look_at` / `set_scale` / `set_local` / `set_world` / `try_set_world` / `mark_transform_subtree_dirty`.
- `TransformPropagation` `ISystem` in PreRender phase.
- `engine/scene/CMakeLists.txt` adds `crd-math` to the public link list.

## Seven architectural decisions pinned

### 1. crd-math is the home for the math additions

UI / particles / future tooling will all want from_euler / from_trs / to_trs. Putting them in scene would force every consumer to depend on scene; promotion later means breaking changes when callers pop up. Pinned in code: math layer additions live in quat.hpp.

### 2. set_rotation_quat default = normalize

Domains that need raw values are the exception. Two methods (not a default-arg flag):
- `set_rotation_quat(e, q)` — auto-normalises.
- `set_rotation_quat_unnormalized(e, q)` — explicit opt-out, name visible in code review.

Robotics / aerospace consciousness > game-code accident.

### 3. Determinism via hash, not record-replay

`test_transform.cpp::"Determinism: identical input order produces bit-exact world matrices"` builds the same scene twice, FNV-hashes every world matrix's bytes in a stable iteration order, asserts the hashes match. Verifies the SEMANTIC contract (same inputs → same outputs); not brittle to memory-layout reorderings (which would break record-replay).

### 4. try_set_world succeeds with negative scale on negative-det input

Standard CAD / mirror-matrix convention. URDF importers regularly produce mirror joints; failing on negative-det forces every robotics user to pre-flip-detect. The decomposition succeeds with X-scale negated (handedness preserved). Documented behaviour: callers needing "all positive scale" check `s.x * s.y * s.z > 0` after the call.

### 5. Mark-dirty in public writers ONLY

Propagation's `get_component_mut<Transform>(e)` writes the world cache without re-marking the subtree dirty. The mark-dirty function is invoked ONLY by the public writers (`set_translation` etc.), NOT by storage's on_update path. Pinned in `transform_propagation.cpp` doc-block.

This avoids the infinite-re-dirty bug: propagation writing world fires ChangeDetect's on_update (good — downstream consumers see the change), but does NOT re-add TransformDirtyFlag (because flag-marking is in the public writers, not in the storage event path).

### 6. TransformDirtyFlag is SparseSet (not Archetype)

Frame-scoped, sparse, added/removed every step. Archetype storage would archetype-explode on every mark/unmark cycle. SparseSet's O(1) add/remove + O(1) has matches the access pattern.

### 7. Hierarchy depth: assert in debug, document in release

`kMaxTransformDepth = 256` + `CRD_ASSERT(depth < kMaxTransformDepth)` during `mark_subtree_via_childof`. Real workloads (humanoid robot 30-40, deep UI 100-150, particle attachment chains 50-100) fit comfortably. A user accidentally writing the root of a 1M-entity tree would trip the assert instantly in dev.

## Determinism contract (test_transform.cpp)

```cpp
TEST_CASE("Determinism: identical input order produces bit-exact world matrices",
          "[scene][transform][determinism]")
{
    auto build_and_step = []() {
        World w; setup_test_world(w);
        EntityId parent = spawn_with_transform(w, Vec3f{1, 2, 3});
        for (int i = 0; i < 5; ++i) {
            EntityId c = spawn_with_transform(w, Vec3f{static_cast<f32>(i), 0, 0});
            w.add_relation<ChildOf>(c, parent);
            w.set_rotation_axis_angle(c, Vec3f{0, 1, 0}, 0.1F * (i + 1));
        }
        w.step(1.0 / 60.0);
        return hash_world_matrices(w, entities);
    };
    CHECK(build_and_step() == build_and_step());
}
```

This is the cross-domain robustness asset. Robotics replay, networking rollback (Phase 4.2), simulation bisection (Phase 8) all rely on this guarantee.

## Numerical precision contract (deep-chain test)

```cpp
TEST_CASE("Deep chain (30-deep): precision within documented f32 tolerance",
          "[scene][transform][precision][deep-chain]")
{
    // 30-deep ChildOf chain; each link translates +X by 1.
    // Tip world position should be (30, 0, 0) within f32 epsilon × 30.
    // Tolerance pinned at 1e-4F (generous; real-world drift is ~1e-6F).
}
```

Documented in `transform.hpp` doc-block: f32 multiplication accumulates ~1 ULP per op; deep chains (>30) MAY exhibit visible drift over seconds at 60 Hz; mitigations are `renormalize_rotation()`, HistoryIndex (Phase 3.2), or custom TransformF64 component.

## Bugs caught during integration

### crd-scene wasn't linking crd-math

Initial build failed with `'crd/math/mat.hpp': No such file or directory`. Fix: added `crd-math` to `engine/scene/CMakeLists.txt`'s public link list.

### Circular include pattern between mat.hpp and quat.hpp

First draft put `from_trs` / `to_trs` in `mat.hpp`, but `to_trs` calls `from_mat3` which lives in `quat.hpp`. Quat already includes mat. Moving the TRS helpers to quat.hpp (next to to_mat3 / from_mat3) where they bridge naturally.

### World is non-movable

Test helper `make_test_world() → World` triggered a deleted-move-constructor error. World has `m_storage` referencing `*this` → non-movable by design. Replaced with `setup_test_world(World&)` configure-in-place pattern.

### Windows lacks M_PI_2 by default

`<cmath>` doesn't define M_PI_2 unless `_USE_MATH_DEFINES` is set globally. Replaced with the literal `1.5707963267948966F` to keep the test header-clean.

## Numbers

### Six-configuration green

| Config | Build | CTest |
|---|---|---|
| win-debug          | clean | 727 / 727 |
| win-relwithdebinfo | clean | 727 / 727 |
| win-release        | clean | 724 / 724 |
| win-asan           | clean | 727 / 727 |
| win-clang-cl       | clean | 727 / 727 |
| win-tidy           | clean | — (5 pre-existing warnings in unrelated files; none v1j-introduced) |

17/17 headless smokes per non-tidy config.

### Scene tests

- Pre-v1j: 192 cases / 34669 assertions.
- Post-v1j: 211 cases / 34716 assertions (+19 cases / +47 assertions).

### LOC

- Math additions:                ~100
- transform.hpp:                 ~110
- transform_propagation.hpp/cpp: ~210
- world.hpp / world.cpp delta:   ~250 (writer API decl + impl)
- test_transform.cpp:            ~330
- Total                          ~1000

## Deferred items pinned (`docs/debt.md`)

The cross-domain ambition surfaces several follow-ups that v1j explicitly does NOT ship; updating docs/debt.md to track them:

1. **Polar decomposition for skewed matrices** — `to_trs` currently best-efforts via `from_mat3` and loses skew. Real polar decomposition (SVD or iterative orthogonalisation) is reserved for v1j+1 if a use case appears.

2. **TransformF64 component + propagation** — orbital / aerospace scales need f64 precision. Math layer already ships `crd::math::Transformd`; scene-layer requires user code to register custom component + system. v1n's reserved-slot freeze test will verify this round-trips. v1k SceneLoader will accept a `TransformF64` registration grammar without changes.

3. **Parallel propagation** — single-threaded per ADR-0054. Phase 3.5 evolution once `par_each` over Query chunks lands. Per-subtree parallelism is straightforward (independent dirty roots → independent subtree DFS); the work-item is one DFS, dispatched to a worker fiber.

4. **AttachedTo socket propagation** — out of scope; Phase 3.2 (animation) ships an attachment-pose system that composes with TransformPropagation.

5. **Per-system change tracking for `.changed<T>()`** — current ChangeDetect snapshot is "modified during current frame" (v1i pin). Cross-frame "what changed since my system last ran" needs per-system state. v1h+1 evolution.

6. **Auto-renormalize rotation policy** — v1j makes renormalize OFF-by-default to preserve user values. A registration trait (`AutoNormalizeRotation{}`) could opt-in per component. Reserved slot if drift becomes visible.

7. **Multi-uniform-scale matrix decomposition fallback** — `to_trs` succeeds on uniform-scale matrices but loses precision on non-uniform-scale + skew combinations. Polar decomposition (item 1) addresses this.

## What this unlocks

v1k (SceneResource + SceneLoader) is now the natural next slice. It consumes v1j directly: a loaded scene's entity-component graph includes Transform components with parented hierarchies; SceneLoader populates them, registers TransformPropagation, and the scene starts rendering correctly on the first step().

Beyond v1k:
- **v1m (sandbox renderer integration)**: `query<Transform, Renderable>().skip_pending<Renderable>()` in RenderExtract drives every visible entity. v1j's world matrices are read directly.
- **Phase 3.1 (physics)**: physics writes Transform.translation / rotation in the Physics phase (or a fixed-step Physics sub-system); v1j propagation runs in PreRender on the same frame, render observes the updated state.
- **Phase 3.2 (animation)**: skeletal poses set bone Transform locals; v1j propagates the chain.
- **Phase 4.2 (networking rollback)**: HistoryIndex<60> on Transform + bit-exact determinism = rollback rewinds to the recorded world matrices and replays.
- **DAW spatial audio (Phase 5+)**: audio engine queries `transform.world` of source entities each audio block; doppler / panning derived from translation deltas.

## Commit message proposal

```
feat(scene+math): Transform + TransformPropagation (v1j, ADR-0054)

Phase 3.0 v1j ships the first concrete ISystem consumer that ties the
8-layer architecture together end-to-end:
  - Transform component (TRS + cached world matrix).
  - TransformDirtyFlag SparseSet marker.
  - World writer API: 6 rotation-set APIs (quat / quat_unnormalized /
    axis_angle / euler with explicit order / from_to / look_at), set_local,
    set_world / try_set_world, set_translation / set_scale, public
    mark_transform_subtree_dirty.
  - TransformPropagation ISystem in PreRender phase. DFS-based, single-
    threaded, deterministic.

Math library additions in crd-math (header-only):
  - EulerOrder enum (4 conventions).
  - from_euler, from_to_rotation, from_trs, to_trs (with negative-
    determinant handling for CAD/URDF mirror cases).

Cross-domain robustness pinned:
  - f32 default; f64 path documented (math::Transformd already exists).
  - Determinism: bit-exact world-matrix hashes across runs (verified).
  - kMaxTransformDepth = 256 hierarchy depth check (debug assert).
  - 7 architectural decisions documented in session log.

Six-config DoD: 727/727 (was 708). 17/17 headless smokes per non-tidy
config. 211 scene tests / 34716 assertions (was 192 / 34669).
```
