# Session 2026-05-19 — geometry v10 cluster CLOSED

## Summary

**Phase 3.1.7 v10 `-curves` cluster CLOSED.** 6 slices on a single day
(2026-05-19) covering substrate + sampling + arc-length + queries +
frames-viz-sandbox + typed boundary. ADR-0076 §27 ✅ Accepted with 35
locked decisions D182-D216. 18-config full sweep PASS. New module
`crd-geometry-curves` (12th `crd-geometry-*` sibling) shipped end-to-end.

## What landed in v10-close itself

### `engine/geometry-curves/include/crd/geometry/curves/queries_typed.hpp`

Quantity-aware `*_typed` wrappers covering the WHOLE v10 surface:
`evaluate_typed`, `evaluate_derivative_typed`, `sample_uniform_typed`,
`sample_adaptive_typed`, `sample_by_curvature_typed`,
`to_polyline_typed`, `build_arclength_table_typed`, `length_of_typed`
(table accessor + curve+alloc convenience), `t_at_distance_typed`,
`distance_at_t_typed`, `aabb_of_typed`, `closest_point_typed`,
`distance_typed`, `intersect_ray_typed`, `tangent_typed`,
`normal_typed`, `binormal_typed`, `compute_rmf_typed`.

**Pattern (D214):** unified `detail_typed::strip(curve, alloc)` for
every kind. Value kinds (Bezier / Hermite / Arc / EllipseArc)
constexpr-strip at zero cost. Owning kinds (Polyline3 / CatmullRom3 /
BSpline3) allocate a short-lived raw copy via the function's existing
`IAllocator*` parameter. Polyline3 also routes through
`detail_typed::to_view(...)` (the raw `evaluate` for polylines is
defined on `Polyline3View<T>`, not the owning form).

**Pattern (D215):** `ArclengthTable<T>` stays raw. Typed
`build_arclength_table_typed` returns the raw table; `length_of_typed`,
`t_at_distance_typed`, `distance_at_t_typed` re-tag at the read site.
Reason: the binary search on monotone-t is dimensionless.

**Pattern (D216):** `tangent_typed` / `normal_typed` / `binormal_typed`
/ `compute_rmf_typed` return RAW `Vec3<T>` / `Array<CurveFrame<T>>`.
Unit vectors are dimensionless by definition.

### Tests — `tests/geometry-curves/test_curves_typed.cpp`

14 cases / 89 assertions covering the typed surface:

1. evaluate on value-kind Bezier matches raw bit-exactly
2. **strip-then-call discriminator** — typed and `strip(typed)→raw`
   produce bit-identical results (catches dropped retag / wrong-tag bugs)
3. evaluate_derivative on Hermite returns Vec3<Length>
4. sample_uniform owning Polyline matches raw point-by-point
5. sample_adaptive takes Length<T> tolerance
6. sample_by_curvature takes Angle<T>
7. arc length round-trip (build / length_of / t_at_distance /
   distance_at_t — D215 raw-table + typed-accessor flow)
8. aabb_of returns AABB3<Length>
9. closest_point returns CurveClosestPointQ{t, typed point, Area
   distance²}
10. distance returns Length<T>
11. intersect_ray returns optional<CurveRayHitQ>
12. tangent / normal / binormal return RAW Vec3<T> (D216 pinned via
    `static_assert`)
13. compute_rmf_typed returns raw Array<CurveFrame<T>> (D216 pinned via
    `static_assert`)
14. f64 instantiations work end-to-end

Module total at cluster close: **95 cases / 3 010 assertions PASS**
(was v10e 81 / 2 921 → +14 / +89).

### Documentation

- **`docs/decisions/0076-geometry-substrate-architecture.md`** — added
  §27 amendment locking D182-D216 across 6 sub-slices.
- **`docs/systems/geometry-curves.md`** — NEW system overview doc
  (first authored at v10-close; the substrate proper had no prior
  overview because v10 shipped same-day).
- **`docs/phases/phase-3.1.7-geometry.md`** — v10-close row flipped to
  ✅.

### Definition of Done

- **Per-slice DoD** (`scripts/per-slice-check.ps1 -Parallel`):
  win-debug + win-asan + win-shipping + win-tidy all PASS.
- **18-config full sweep** (`scripts/full-sweep.ps1`): 11 Windows + 7
  Linux configs all PASS.

## Cluster totals

| Sub-slice | LOC engine | LOC tests | Decisions | New cases |
|---|---|---|---|---|
| v10a substrate         | ~700 | ~620 | D182-D192 (11) | +37 |
| v10b sampling          | ~300 | ~310 | D193-D197 (5)  | +11 |
| v10c arc-length        | ~220 | ~310 | D198-D202 (5)  | +13 |
| v10d queries           | ~360 | ~440 | D203-D207 (5)  | +11 |
| v10e frames+viz+sandbox| ~430 | ~350 | D208-D213 (6)  | +9 (+5 viz, +2 sandbox) |
| v10-close typed        | ~470 | ~280 | D214-D216 (3)  | +14 |
| **TOTAL**              | **~2 480** | **~2 310** | **35 (D182-D216)** | **+95 / +3 010 assertions** |

### Cluster-level discriminator tests (catch entire regressions)

- **v10a `evaluate` IS the algorithm** (D186) — every downstream
  consumer call bottoms out on the same evaluator entry point. A
  regression in evaluator alone collapses every test in v10b/c/d/e/close.
- **v10e Wang reflection-sign** (D211 zero-curvature fallback +
  reflection-sign discriminator) — planar circular arc → all RMF
  normals coplanar with arc plane, all binormals = plane normal up to
  global sign. A wrong Wang reflection produces alternating binormals
  on the second frame and the test catches it immediately.
- **v10-close strip-then-call** (D214 typed boundary discriminator) —
  every `*_typed` call produces bit-equal output to manually-stripped
  raw call. Catches dropped retag / wrong-tag retag / truncating
  static_cast bugs reliably.

## Phase 3.1.7 status

**Phase 3.1.7 sub-modules: 12 of 11 ✅** (12 = original 11 + v10 curves
extension). Substrate fully done across:

1. `crd-geometry-primitives` ✅ (v0 family)
2. `crd-geometry-bvh` ✅ (v1)
3. `crd-geometry-convex` ✅ (v2 + v3)
4. `crd-geometry-mesh` ✅ (v4)
5. `crd-geometry-spatial` ✅ (v5)
6. `crd-geometry-polygon` ✅ (v6)
7. `crd-geometry-mesh-processing` ✅ (v7)
8. `crd-geometry-delaunay` ✅ (v8)
9. `crd-geometry-decomposition` ✅ (v9c)
10. `crd-geometry-bvh-gpu` ✅ (v9a + v9b)
11. `crd-geometry-shader-helpers` ✅ (v9e)
12. `crd-geometry-curves` ✅ **(v10 — closes this session)**

ADR-0076 amendments: §12-§27 all accepted.

**Remaining in Phase 3.1.7:** v11 transform-aware geometry query
helpers (~2 days) — new `crd/geometry/primitives/transform.hpp` in the
existing `crd-geometry-primitives` module (NO new
`crd-geometry-runtime` module per the 2026-05-14 scope decision).

After v11 → Phase 3.1.7 fully closes → `crd-hesap-dense` v0 → Phase 3.1
eylem v1c resume per Strategic Execution Plan locked 2026-05-15.

## Filed follow-on slices

Consumer-pull when a real consumer surfaces; none blocking v10-close.
Catalogued in ADR-0076 §27. Highlights:

- **`v10e-arclength-rmf`** — RMF over arc-length-uniform t-table
  (truly-smooth cinematic-camera path).
- **`v10a-cr-analytic-2nd-derivative`** + **`v10a-bspline-analytic-
  2nd-derivative`** — per-kind h-trait override on
  `detail::second_derivative_step<Curve>` for tighter Frenet normals.
- **`v10c-analytic-arc-length`** — per-kind closed-form length where
  it exists (circular = r·θ, Bezier via Gauss-Legendre 5-pt).
- **`v10d-analytic-aabb`** — analytic AABB for arcs + Bezier
  convex-hull-loose.
- **`v10d-multi-hit-ray`** — return all intersections, not just first.
- **`v10a-2d-{hermite,catmull,bspline}`** — 2D peers for the 3D-only
  kinds.
- **`v10b-2d-sampling`** — 2D-peer samplers.
- **`MultiCubicBezier3`** — segmented closed-loop Bezier composition.

## Open / pinned future work (cross-cluster)

- **Direct-manipulation UX cluster** (gizmos / mesh + curve + navmesh
  editors) — user-flagged high-priority workstream filed in
  `docs/debt.md` 2026-05-19 + `MEMORY.md`
  `project_gizmos_direct_manipulation_cluster.md`. Until that cluster
  opens, every geometry showcase scene uses ImGui `DragFloat3` for
  control-point editing (sandbox curves showcase has a tooltip
  pointing at this cluster).

## Files added (v10-close)

- `engine/geometry-curves/include/crd/geometry/curves/queries_typed.hpp`
- `tests/geometry-curves/test_curves_typed.cpp`
- `docs/systems/geometry-curves.md` (NEW system doc)
- `docs/sessions/2026-05-19-geometry-v10-close.md` (this file)

## Files changed (v10-close)

- `engine/geometry-curves/include/crd/geometry/curves/curves.hpp` —
  umbrella include + `queries_typed.hpp`.
- `tests/geometry-curves/CMakeLists.txt` — add `test_curves_typed.cpp`.
- `docs/decisions/0076-geometry-substrate-architecture.md` — append
  §27 amendment.
- `docs/phases/phase-3.1.7-geometry.md` — v10-close row → ✅.

## Test summary at cluster close

| Binary | Cases | Assertions |
|---|---:|---:|
| `crd-geometry-curves-tests`      | 95 | 3 010 |
| `crd-geometry-viz-tests`         | (incl. +5 from v10e) | + 5 |
| `crd-sandbox-showcase-tests`     | 9 (incl. +2 from v10e) | 57 |
| **`crd-geometry-curves` cluster — slice gates ALL PASS** | | |
