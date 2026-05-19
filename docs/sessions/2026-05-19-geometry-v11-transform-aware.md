# Session 2026-05-19 — geometry v11 transform-aware + **Phase 3.1.7 FULLY CLOSED**

## Summary

Shipped Phase 3.1.7 v11 in one day: transform-aware shape helpers + typed
boundary in `crd-geometry-primitives`. **THIS CLOSES PHASE 3.1.7.** 12 of
11 sub-modules complete; substrate fully done; consumer-side work resumes
per the Strategic Execution Plan locked 2026-05-15.

## What landed in v11

### `engine/geometry-primitives/include/crd/geometry/primitives/transform.hpp`

**14 3D entry points + 7 2D entry points + `TransformedShape<Shape>`
composition wrapper.** Per the user's 2026-05-19 elite-completeness
mandate (`feedback_elite_only_no_shortcuts`), surface widened from the
phase-doc's 7 enumerated entries to the FULL primitive catalog.

**3D transforms:**
- `transform_aabb(M, AABB3)` — 8-corner method (D219).
- `transform_obb(M, OBB3)` — three-tier cascade (D220):
  rigid-exact → uniform-scale-exact → conservative-via-axis-aligned-OBB.
- `transform_sphere(M, Sphere)` — uniform-scale-exact / max-axial-scale
  loose bound (D221).
- `transform_capsule3(M, Capsule3)` / `transform_cylinder3(M, Cylinder3)`.
- `transform_triangle3(M, Triangle3)` / `transform_tetrahedron(M, Tet)`.
- `transform_plane(M, Plane)` — inverse-transpose-3 + `CRD_ASSERT` on
  singular (D222 / D225).
- `transform_ray3(M, Ray3)` — direction NOT renormalized (D223).
- `transform_ray3_to_local(world_ray, world_to_local)` — inverse-
  direction pattern; rigid + uniform-scale precondition `CRD_ASSERT`
  (D224).
- `transform_segment3(M, Segment3)` / `transform_line3(M, Line3)`.
- `transform_frustum(M, Frustum)` — batched inverse-transpose reused
  across 6 plane transforms (D232).

**2D peers** (via `Mat3<T>` homogeneous matrices — D227):
- `transform_aabb2` / `transform_obb2` / `transform_circle` /
  `transform_capsule2` / `transform_segment2` / `transform_ray2` /
  `transform_triangle2`.

**`TransformedShape<Shape>` composition wrapper** with
**trait-based scalar deduction** via `shape_scalar<Shape>::type` (D218 —
21 specialisations). `TransformedShape<AABB3<f64>>` auto-deduces a
`Mat4<f64>` to_world; the two-template-param form
(`<Shape, T>`) was rejected as a silent precision-loss trap.

**`MatrixAttributes3<T>` / `MatrixAttributes2<T>`** struct cached once
at function entry (D228), holding
`{is_identity, is_rigid, is_uniform_scale, uniform_scale_factor,
max_axial_scale, determinant}`. Reused across decisions per shape.

**Identity-matrix bit-exact fast path** (D229): `M == Mat4::identity()`
short-circuits to bit-exact pass-through. Useful for batched static-
geometry transforms.

### `engine/geometry-primitives/include/crd/geometry/primitives/transform_typed.hpp`

Quantity-aware `transform_*_typed` wrappers covering every transform
helper. Strip-compute-retag pattern (D233) mirroring `queries_typed.hpp`
(ADR-0078 §5 D34). Mat4/Mat3 stay raw (dimensionless world transform);
shape input + output typed. Adds missing strip/retag helpers for
Line3 / Tetrahedron / Frustum / 2D peers not covered in
`queries_typed.hpp`.

### Tests — `tests/geometry-primitives/test_transform.cpp`

**26 cases / 95 assertions** including 5 advisor-pinned discriminators:

1. **Identity bit-exact pass-through** on every shape (catches FP-
   rounding in identity path).
2. **45° rotation AABB diagonal grows by sqrt(2)** exactly (catches
   axis-projection bugs).
3. **Plane normal stays perpendicular to transformed in-plane vector**
   within `1e-4` (pins inverse-transpose correctness reliably).
4. **`transform_ray3_to_local` round-trip** — local hit transforms back
   to bit-equal world hit.
5. **Negative-determinant (reflection) preserves OBB volume** (catches
   sign bugs in reflection handling).

Plus per-shape correctness (rigid + uniform + non-uniform + general
affine), 2D peers, TransformedShape composition + auto-deduced scalar,
typed-boundary round-trip on AABB3<Length32> and Sphere<Length32>, and
f64 instantiations.

### Decisions locked

**D217-D233** (17 decisions) — see ADR-0076 §28 for the full pin list.

### Definition of Done

- **Per-slice DoD** (`scripts/per-slice-check.ps1 -Parallel`): 4
  configs PASS in 34 s (win-debug + win-asan + win-shipping +
  win-tidy).
- **18-config full sweep** (`scripts/full-sweep.ps1`): 11 Windows + 7
  Linux configs all PASS — **and PHASE 3.1.7 CLOSES**.

## Phase 3.1.7 CLOSURE

**🎉 Phase 3.1.7 `crd-geometry` substrate FULLY CLOSED 2026-05-19.**

12 of 11 sub-modules complete (12 = original 11 + v10 curves
extension):

1. `crd-geometry-primitives` ✅ (v0 family + v11 transform-aware)
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
12. `crd-geometry-curves` ✅ (v10)

ADR-0076 amendments §1-§28 all Accepted.

## What's next (post-Phase 3.1.7)

Per the Strategic Execution Plan locked 2026-05-15:

1. **`crd-hesap-dense` v0** — dense numerical substrate (Phase 3.1.6
   prologue). Ships before eylem v1c resumes so the physics substrate
   can lean on its dense linear algebra primitives.
2. **Phase 3.1 eylem v1c+ resume** — broadphase / narrowphase / SI
   solver / joints / islands / scene queries / character controller /
   snapshot+replay / sandbox / close. Consumes `crd-geometry` from day
   one (per the 2026-05-11 sequencing pivot — geometry-before-physics).
3. **Future cluster — direct-manipulation UX (gizmos)** — user-flagged
   high-priority workstream filed in `docs/debt.md` and tracked in
   `MEMORY.md` (`project_gizmos_direct_manipulation_cluster.md`).
   Until that cluster opens, sandbox showcase scenes use ImGui
   `DragFloat3` for control-point editing.

## Files added (v11)

- `engine/geometry-primitives/include/crd/geometry/primitives/transform.hpp`
- `engine/geometry-primitives/include/crd/geometry/primitives/transform_typed.hpp`
- `tests/geometry-primitives/test_transform.cpp`
- `docs/sessions/2026-05-19-geometry-v11-transform-aware.md` (this file)

## Files changed (v11)

- `tests/geometry-primitives/CMakeLists.txt` — add `test_transform.cpp`.
- `docs/decisions/0076-geometry-substrate-architecture.md` — §28
  amendment + Phase 3.1.7 CLOSE.
- `docs/decisions/README.md` — §28 entry, Phase 3.1.7 closed marker.
- `docs/systems/geometry-primitives.md` — v11 row added to the
  per-slice status table.
- `docs/phases/phase-3.1.7-geometry.md` — ARCHIVE note at top per
  AGENTS.md ritual; v11 + v11-close rows flipped to ✅.
- `context.md` — Phase 3.1.7 fully closed; current focus shifts to
  `crd-hesap-dense` v0 → eylem v1c resume.

## Filed follow-ons (not blocking v11-close)

Consumer-pull when a real consumer surfaces; all listed in ADR-0076 §28:

- `v11-ray-to-local-non-uniform` — `{LocalRay, t_scale}` return-type
  variant of `transform_ray3_to_local` for the general-affine case.
- `v11-precomputed-inv-transpose` — public `transform_plane_with_inv_
  transpose` overload for batched plane transforms outside frustum.
- `v11-batched-aabb-simd` — `transform_aabb_batch(M, ConstSpan<AABB3>)`
  with AVX2 4-corner-pair multiplies.
- `v11-future-runtime-module` — reserved `crd-geometry-runtime` module
  slot remains reserved; opens if a consumer-side facade duplication
  problem ever surfaces.

## Test summary at v11-close

| Binary | Cases | Assertions |
|---|---:|---:|
| `crd-geometry-primitives-tests` v11 additions | 26 | 95 |
| (whole module corpus continues to grow but unaffected by this slice) | | |
