# Session — 2026-05-13 — Phase 3.1.7 v1i-b: shapecast (closed-form TOI) +
# `bvh_shapecast_sphere` / `bvh_shapecast_box` + Bvh4 variants

## Goal

Second sub-slice of the v1i split (ADR-0076 §15). v1i-a parked the unified
`queries.hpp` facade with `raycast` / `overlap` / `closest_point`. v1i-b
extends it with **shapecast** — "sweep a moving shape along `dir` from t=0
to t=tmax; report the first time-of-impact (TOI) with `target`" — for the
closed-form moving-shape × target-shape pairs the broadphase needs.

The general convex-shape shapecast (sweep an arbitrary `ConvexHullView`) is
the GJK-cast in `-convex` v2; eylem v6 CCD's two-moving-convex case stays
in eylem. v1i-b ships the cheap closed-form reductions.

## What we built / changed

### `engine/geometry-bvh/include/crd/geometry/bvh/bvh_shapecast.hpp` (new)

The BVH-side shapecast functions in `crd::geometry::bvh`:

- `bvh_shapecast_sphere(BvhTree, prims, Sphere moving, Vec3 dir, f32 tmax)
  → optional<BvhRayHit>` — sphere-cast over a static `BvhTree`.
- `bvh4_shapecast_sphere(Bvh4Tree, prims, ...)` — same, over the 4-wide tree.
- `bvh_shapecast_box(BvhTree, prims, AABB3 moving, Vec3 dir, f32 tmax)
  → optional<BvhRayHit>` — box-cast over a static `BvhTree`.
- `bvh4_shapecast_box(...)` — same, over the 4-wide tree.

### `engine/geometry-bvh/src/bvh_shapecast.cpp` (new)

Two file-local traversal helpers — `binary_inflated_raycast` and
`bvh4_inflated_raycast` — that walk the corresponding tree and ray-cast
against each node + leaf-prim AABB **inflated by a per-axis pad**. The
inflation is the closed-form Minkowski reduction:

- Sphere-cast pad = `(radius, radius, radius)` on every axis →
  **conservative** (true Minkowski sum of a sphere and an AABB has rounded
  corners + edges; this approximation produces square corners so the worst
  case is a hit reported at a corner that the rounded form would have
  skirted — the broadphase pre-condition every shipped physics engine
  accepts; exact corners via GJK-cast in `-convex` v2).
- Box-cast pad = `(moving.max - moving.min) / 2` (the moving box's
  half-extents) → **exact** (Minkowski sum of two AABBs is an AABB).

Traversal shape: identical to `bvh_raycast` / `bvh4_raycast` — near-child-
first per `split_axis`, the running `best_t` prunes the far subtree, leaves
do per-prim slab tests. The slab kernel is the v0f precomputed Williams/Ize
robust form (`intersect_ray_aabb_robust`) on the inflated bounds. `dir`
need not be unit; `t` is the parameter along `dir`. NaN/Inf-tolerant
queries (per v1h §16 pin #3); the asserts on the inputs (`is_finite(origin
/ dir / pad)`) catch garbage in debug.

**Performance note:** `bvh4_inflated_raycast` walks the 4 children scalar
sequentially per node (four `inflate` + `intersect_ray_aabb_robust` calls).
The `Vec4f` `ray_vs_4_aabb` kernel from v1g is **not** reused — the
shapecast equivalent would need a SIMD inflate-then-slab kernel, which is a
clean ~40 LOC follow-up (mirror of v1g's pattern). Not in v1i-b scope.

### `engine/geometry-bvh/include/crd/geometry/queries.hpp` — facade extension

Added the unified `cast_ray` / `cast_sphere` / `cast_box` overloads, all
inline `crd::geometry::*` free functions:

**Primitive `cast_ray`** (degenerate shapecast — zero-extent moving shape):
- `cast_ray(Ray3, AABB3, tmax = ∞) → optional<T>` — wraps `intersect_ray_aabb`.
- `cast_ray(Ray3, OBB3, ...)` — wraps `intersect_ray_obb`.
- `cast_ray(Ray3, Sphere, ...)` — wraps `intersect_ray_sphere`.
- `cast_ray(Ray3, Plane, ...)` — wraps `intersect_ray_plane`.

  The `intersect_ray_*` family returns `bool` + an out-parameter; the facade
  wraps them to `optional<T>` to match `cast_sphere` / `cast_box`. Caller
  picks either style.

**Primitive `cast_sphere`**:
- `cast_sphere(Sphere moving, Vec3 dir, T tmax, AABB3 target) → optional<T>` —
  conservative: inflate `target` by `moving.radius` on every face, ray-cast
  `moving.center` along `dir` via `intersect_ray_aabb` (the slab test
  naturally returns t=0 when the inflated AABB contains the start position).
- `cast_sphere(Sphere moving, ..., Sphere target)` — **exact**: replace
  target with `Sphere(target.center, moving.radius + target.radius)`,
  ray-cast `moving.center`. Returns `t=0` if the two spheres are already
  overlapping at start (explicit early return — `intersect_ray_sphere`
  would otherwise return the *exit* time of the inflated sphere).
- `cast_sphere(Sphere moving, ..., Plane target)` — **exact**: closed-form
  on `signed_distance(plane, center) ± radius`. Returns `t=0` on
  already-touching.

**Primitive `cast_box`**:
- `cast_box(AABB3 moving, Vec3 dir, T tmax, AABB3 target) → optional<T>` —
  exact: inflate `target` by `moving`'s half-extents, ray-cast `moving`'s
  center along `dir`.

**BVH `cast_sphere` / `cast_box`** — forward to the new `bvh::bvh_shapecast_*`
/ `bvh::bvh4_shapecast_*` functions; return `optional<BvhRayHit>` (=
`RayHit<u32>`) just like the raycast surface.

**Argument convention pinned** (v1i-b decision — propagates to v2 GJK-cast
and v6 CCD):
- Primitive overloads: `(moving, dir, tmax, target)` — moving shape first,
  target last. Matches PhysX / Jolt convention; reads naturally as "cast a
  sphere along dir for tmax against a box".
- BVH overloads: `(tree, prims, moving, dir, tmax)` — structure first.
  Matches the existing `raycast(BvhTree, prims, ray, tmax)` /
  `closest_point(BvhTree, prims, ...)` shape.

  The asymmetry between primitive and BVH forms is real but consistent with
  v1i-a's convention for `closest_point`.

### `tests/geometry-bvh/test_shapecast.cpp` (new)

15 TEST_CASEs, +1409 assertions (50 → 65 cases, 66091 → 67500 assertions on
`crd-geometry-bvh-tests`):

**Degenerate-recovery pins:**
1. `cast_sphere(radius=0) == cast_ray` for primitive AABB target.
2. `cast_box(half=0) == cast_ray` for primitive AABB target.

**Overlap-at-start (t=0 semantics):**
3. `cast_sphere returns t=0 when already overlapping at start` — covers
   sphere×AABB, sphere×sphere, sphere×plane.
4. `cast_box returns t=0 when already overlapping at start`.

**Closed-form correctness — exact-numeric spot checks:**
5. `cast_sphere vs AABB`: r=1 sphere at (-3,1,1) → AABB face at x=2 →
   t=4 (the inflated face is at x=1, ray reaches it at t=4).
6. `cast_sphere vs sphere`: r=1 + r=1, centers 10 apart → t=8.
7. `cast_sphere vs plane`: r=0.5 at y=5, plane y=0, sweep -y → t=4.5.
8. `cast_box vs AABB`: half=(1,1,1) each, centers 10 apart on x → t=8.

**Bisection cross-check (advisor recommendation — catches Minkowski-sum sign
AND magnitude bugs the per-algo brute force can't):**
9. `cast_sphere vs AABB bisection: distance squared at TOI equals r squared`
   — at the closed-form TOI, the swept center is *on the inflated AABB
   boundary*, so `r² ≤ distance²(target, swept_center(TOI)) ≤ 3·r²` (= r²
   on a face contact, up to 3·r² on a corner contact where the swept
   center is at a corner of the inflated AABB and the closest point on
   target is the corresponding corner at distance r·√3). 300 trials with
   sweeps aimed at the target (with jitter) to keep the hit rate
   meaningful — pinned by `REQUIRE(hits >= 30)` so a future refactor that
   tanks the hit rate surfaces immediately instead of silently turning the
   test into a no-op. Just before TOI: `d² ≥ r² − ε` (sphere not yet
   touching).

   **Verified the test catches what advisor recommended it for.** Two
   round-trip bug injections during the slice:
   - **Magnitude bug** (pad = `r/2` instead of `r`): originally the test's
     upper bound was `d² ≤ r² + ε` only — vacuous, passed silently because
     the missing trials short-circuited via `if (!toi) continue;` and the
     handful of hits had `d² = r²/4 < r²`, satisfying the upper bound.
     With the lower bound `d² ≥ r² − ε` added (advisor recommendation),
     the bug injection fails at `0.99 ≥ 3.96` (= r²/4 vs r²·full radius).
   - **Hit rate regression**: my first strengthening hit 8/300 in random
     trials — pinned by `REQUIRE(hits >= 30)`, fails with "8 >= 30". Fixed
     by aiming the sweep at the target center with ±2 jitter; now 65 cases
     / **68 383 assertions** on the binary.

**BVH shapecast matches brute force:**
10. `cast_sphere(BvhTree) and cast_sphere(Bvh4Tree) match brute force` —
    200 random AABBs, 300 random sphere sweeps with `radius ∈ [0.2, 5]`,
    varying `tmax`. Brute force iterates every prim with the SAME
    `intersect_ray_aabb`-on-inflated-AABB kernel the BVH traversal uses;
    matches up to ~1e-4 tolerance (the BVH uses the v0f robust slab
    `intersect_ray_aabb_robust` and brute uses the non-robust
    `intersect_ray_aabb` — they agree on `tmin` for well-formed inputs to
    1-2 ULPs).
11. `cast_box(BvhTree) and cast_box(Bvh4Tree) match brute force` — 300
    random box sweeps with `half ∈ [0.2, 3]`, varying `tmax`.

**BVH degenerate-recovery:**
12. `BVH cast_sphere(radius=0) returns same t as raycast`.
13. `BVH cast_box(half=0) returns same t as raycast`.

**Edge cases:**
14. `BVH cast_sphere respects tmax`.
15. `BVH shapecast over empty tree returns nullopt` (sphere + box × BVH +
    Bvh4).

**Sweep drive-by fix:** Test #9's original name had an em-dash + ² Unicode
chars in the `TEST_CASE` name. The bvh-tests binary direct run reported all
65 cases passing — but `ctest` flagged that test as Failed because Windows
ctest's name-matching doesn't roundtrip non-ASCII through the cmd codepage
(same pattern as the v0f sweep fix on em-dashes; the
`crd-no-non-ascii-test-names` guard would have caught it, but I didn't run
the guard before the first ctest). Renamed to ASCII; sweep green.

## Decisions made

- **Argument convention pinned** (v1i-b decision, propagates to v2 / v6):
  primitive shapecast overloads use `(moving, dir, tmax, target)`; BVH
  shapecast overloads use `(tree, prims, moving, dir, tmax)`. Documented in
  the facade's section header so the next slice doesn't relitigate.
- **Conservative sphere-vs-AABB acknowledged in docs.** Phase doc explicitly
  authorises the cheap form (`ray-vs-AABB-grown-by-r`); the exact-corners
  answer is GJK-cast in `-convex` v2. Documented in the facade comment + the
  bvh_shapecast.hpp header. No follow-up debt.
- **Bvh4 SIMD reuse deferred.** The `Vec4f` ray-vs-4-AABB kernel from v1g
  isn't used for `bvh4_shapecast_*` — would need a per-call SIMD inflate
  splat + slab kernel (~40 LOC mirror of v1g's pattern). Out of scope for
  v1i-b; ~2-day follow-up if/when shapecast benchmarks demand it. Notes in
  the impl + system doc.
- **Overlap-at-start = t=0 conventionally.** `intersect_ray_aabb` and the
  inflated-AABB BVH path return t=0 naturally when the start position is
  inside the inflated target. `intersect_ray_sphere` does NOT — it returns
  the exit time when origin is inside the sphere. Added an explicit
  `if (overlap) return 0` check to `cast_sphere(Sphere, Sphere)` and
  `cast_sphere(Sphere, Plane)` to match the convention across all
  primitive overloads. Pinned an invariant comment on the
  `cast_sphere(Sphere, Plane)` `denom == 0` (parallel-ray) branch so a
  future refactor doesn't silently lose the straddling-overlap case if the
  early return is moved (advisor polish #2).
- **Capsule-cast vs triangle deferred to v2 / v4.** The phase doc's
  original v1i row listed it as an example; v1i-b ships sphere×{AABB,
  Sphere, Plane} + box×AABB. Capsule-vs-triangle is ~50 LOC of
  Ericson §5.5.7; it lives more naturally with the mesh-collider
  machinery (v4) or the GJK-cast surface (v2). Phase doc v1i-b row no
  longer mentions it; v2 row carries the GJK-based form forward.
- **`cast_ray` primitive overloads are thin `optional`-wrappers** around the
  existing `intersect_ray_*` family rather than new derivations. Two API
  styles coexist (bool+out-param vs optional-return); callers pick.

## Files touched

- New: `engine/geometry-bvh/include/crd/geometry/bvh/bvh_shapecast.hpp`,
  `engine/geometry-bvh/src/bvh_shapecast.cpp`,
  `tests/geometry-bvh/test_shapecast.cpp`, this session log.
- Modified: `engine/geometry-bvh/include/crd/geometry/queries.hpp` (added
  `cast_ray` / `cast_sphere` / `cast_box` overloads — primitives + BVH;
  shapecast section header documents the argument convention),
  `engine/geometry-bvh/include/crd/geometry/bvh/bvh.hpp` (added
  `bvh_shapecast.hpp` to the umbrella), `tests/geometry-bvh/CMakeLists.txt`
  (added `test_shapecast.cpp`), docs (this session log + phase doc + system
  doc + context.md).

## Tests / verification

Per the in-flight `-bvh` directive (full 17-config sweep deferred to v1
cluster close, after v1j):

- **win-debug**: full build ✅; ctest **1247/1247 PASS** (was 1232 — +15
  new TEST_CASEs in test_shapecast.cpp registered under
  `[geometry][shapecast]`).
- **win-asan**: full build ✅; ctest **1247/1247 PASS** (~62 s). No
  use-after-free / heap-buffer-overflow flagged by the new shapecast paths.
- **win-shipping**: full build ✅ (full LTO, MSVC); ctest **1242/1242
  PASS** (debug-only tests gated correctly skipped).
- **win-tidy**: build ✅ (clang-tidy gate); zero new warnings on the new
  files (`bvh_shapecast.hpp` / `bvh_shapecast.cpp` / `test_shapecast.cpp`
  / the queries.hpp additions). The pre-existing
  `bugprone-unchecked-optional-access` notes on `test_queries.cpp`
  lines 452/453 are the standard Catch2 `REQUIRE(opt.has_value())` then
  `opt->member` pattern that tidy can't see through — same as v1a/v1d/v1e
  test files; the build target succeeds.
- **CI guards**: `crd-no-std-math-check` + `crd-no-std-sort-check` +
  `crd-no-non-ascii-test-names` (post the em-dash fix) green;
  `crd-simd-emission-check` green under vcvars.

`crd-geometry-bvh-tests` directly: **65 cases / 68 383 assertions** on
win-debug (was 50 / 66 091 after v1i-a).

## Next session starts with

- **Phase 3.1.7 v1i-c** — broadphase pairs + validation discipline.
  `find_overlapping_pairs(const DynamicBvh&, OutFn)` — Catto GDC 2019
  dual-descent self-overlap, producing all `(i<j)` fat-AABB-overlapping
  leaf pairs in one traversal. The all-pairs primitive eylem v1c's
  broadphase wraps directly. Reusable test helper header
  (`tests/geometry-primitives/include/test_corpus.hpp` or equivalent) with
  degenerate-geometry generators (zero-volume AABBs, collinear / zero-area
  triangles, coincident points, NaN-Inf inputs) and the large-coordinate
  sweep (+1e6 / +1e7 origin shift). New `test_validation.cpp` per module
  applying the corpora across the existing query suite. ~150 LOC engine +
  ~150 LOC tests, ~1 day.
- After v1i-c closes: **v1j** (`crd-geometry-viz` companion module —
  debug-draw bridge to `crd-draw`). Then v1 cluster closes → full 17-config
  `scripts/full-sweep.ps1` → on to v2 (`-convex`: GJK + EPA + SAT +
  Quickhull, including GJK-cast).
- **Follow-up debt:** `Vec4f` inflate-and-slab kernel for `bvh4_shapecast_*`
  (mirror of v1g's pattern, ~40 LOC + a lane-by-lane test) — to be scheduled
  if a shapecast-heavy consumer surfaces.
