# crd-geometry-primitives

> First sub-module of **crd-geometry** (ADR-0076 §1). Primitive shape types +
> closest-point / intersection / containment helpers — the leaf substrate every
> later geometry sub-module (`-bvh`, `-convex`, `-mesh`, …) and every consumer
> (eylem, sdf, renderer, scene) builds on.

## Status

| Slice | What | State |
|---|---|---|
| v0a | Module skeleton + v0 type catalogue + **the `crd::math::geometry` move-and-delete** (ADR-0076 §13): the old `Ray`/`Plane`/`Sphere`/`AABB`/`Triangle`/`Frustum` + ~16 helpers + their formatters migrated here as `crd::geometry::primitives::*`; `crd/math/geometry.hpp` deleted; the 9 consumers repointed; `crd-math` thereafter ships only Vec/Mat/Quat/Transform/SIMD/`deterministic`. | ✅ 2026-05-12 |
| v0b | **2D peer set + closest-point catalogue (2D + 3D) + segment↔segment** (ADR-0076 §14): added `Line2`/`Segment2`/`Ray2`/`AABB2`/`OBB2`/`Circle`/`Capsule2`/`Triangle2`/`Point2`; renamed the v0a 3D types to their `…3` forms (dimension-suffix naming rule); `closest_point.hpp` with `closest_point` + `closest_param` + `distance` + `distance_squared` for every primitive and `closest_points(Segment,Segment)` (the mutually-closest pair), 2D and 3D, sharing dimension-agnostic cores (Ericson Voronoi-region triangle, Ericson segment↔segment). | ✅ 2026-05-13 |
| v0c | **intersection corpus (2D + 3D)** — `intersect.hpp`: ray casts (`intersect_ray_{aabb,obb,cylinder,capsule}` + 2D `intersect_ray2_{aabb,obb,line,segment,circle,capsule,cylinder}`); SAT pairs (`intersects(AABB3,Triangle3)` Akenine-Möller 13-axis, `intersects(OBB3,OBB3)` 15-axis, `intersects(OBB2,OBB2)` 4-axis, `intersects(Triangle2,Triangle2)` 6-axis), `intersects(Triangle3,Triangle3)` Möller 1997, `segments_intersect(Segment2,Segment2)`; sphere/circle/capsule ↔ X reductions via the v0b distance API; frustum↔OBB3. **Added types `Cylinder3`/`Cylinder2`** (flat-cap segment-with-radius — distinct from `Capsule3`'s hemispherical caps) + their `operator==`/formatters/`contains`. | ✅ 2026-05-13 |
| v0d | **`Tetrahedron` type + barycentric / tetrahedron utilities** (ADR-0076 §7; Ericson §3.4) — `barycentric.hpp`: `barycentric(Tetrahedron,p)→Vec4` (signed-volume ratios; orientation-stable), `contains(Tetrahedron,p)`, `from_barycentric` (the inverse, for `Triangle3`/`Triangle2`/`Tetrahedron`), `decompose_prism_to_tets(bottom,top)` (the canonical 3-tet split of a triangular wedge — Marching-Tetrahedra / FEM-meshing form, fixed diagonal). Type `Tetrahedron` (3D-only → no suffix) + `operator==`/formatter/aliases + `centroid`/`signed_volume`/`volume` in `primitives.hpp`. | ✅ 2026-05-13 |
| v0e | **iq formulary + `reduce_argmax_with_lex_tiebreak` + shader-helpers skeleton** (ADR-0076 §7) — `formulary.hpp`: smooth-min/max (`smin_poly`/`smin_cubic`/`smin_exp` + `smax_*`; `smin_exp` via `crd::math::deterministic::exp2/log2`), value-domain ops (`op_round`/`op_onion`/`extrude_2d`), position-domain ops (`domain_repeat`/`domain_mirror`/`domain_elongate` exact + `domain_twist`/`domain_bend` via `crd::math::deterministic::sin/cos`). New `crd::math::simd::reduce_argmax_with_lex_tiebreak` (`crd/math/simd/reduce.hpp` — scalar-deterministic argmax with score-desc / (x,y,z)-asc-lex / earliest-index tiebreak; ADR-0076 §4 pin #10, what geometry v3 Quickhull needs). New module `crd-geometry-shader-helpers` (skeleton — the GLSL/HLSL twin of these formulas; v9e fills in the cooker + the ULP-conformance test). | ✅ 2026-05-13 |
| v0f | **cutting-edge / branchless / SIMD intersection corpus — closes the v0 sub-phase.** Scalar: `watertight_ray_tri.hpp` (Woop/Benthin/Wald 2013 watertight ray-tri w/ per-ray shear precompute + closed-on-edge double-fallback; Baldwin-Weber-style precomputed per-triangle affine transform), `robust_ray_aabb.hpp` (Williams 2005 precomputed slab + Ize 2013 conservative `tmax` widening; `RayAABBPrecompute` = the single-ray "RayPacket"), `plucker.hpp` (Plücker side test + sign-only ray-tri boolean). SIMD (`simd_batch.hpp` + out-of-line `simd_batch.cpp`, f32/`Vec8f`): `ray_vs_8_aabb` (1 ray × 8 boxes — leaf-batch), `ray_packet8_vs_aabb` (8 rays × 1 box — packet), `ray_vs_8_triangle` (MT × 8), `aabb8_vs_aabb`, `sphere8_vs_sphere`, `segment8_vs_segment_distsq` + `precompute_ray_packet8`. All cross-checked vs the v0b/v0c/`robust_ray_aabb` scalar references. ADR-0076 §4 pins #12 (watertight axis selection + closed-on-edge) & #13 (Plücker fixed sum order, sign-zero = on-the-line) realised. | ✅ 2026-05-13 |
| v1h | **Primitives-substrate hardening** (ADR-0076 §15) — `constants.hpp` (the geometry epsilon/tolerance policy named by intent — `k_distance/area/parallel/degenerate_extent_epsilon`, `k_sah_cost_epsilon`, `k_default_fat_margin`, `k_robust_aabb_pad_ulps`; value-preserving retrofit of `bvh_build_internal.hpp`'s `k_sah_cost_epsilon` + `robust_ray_aabb.hpp`'s `ray_aabb_robust_pad`), `is_finite.hpp` (`is_finite(x)` for every primitive + `all_finite(span)` + **the NaN/Inf contract**: queries tolerate, builders reject in debug — asserts added to `bvh_build`/`bvh_build_parallel`/`DynamicBvh::insert`/`update`/`bvh4_collapse`), `signed_distance.hpp` (20 Inigo Quilez analytic SDFs in C++ — 3D sphere/box/round_box/box_frame/plane/capsule/cylinder/cone/torus/triangle/ellipsoid/octahedron + 2D circle/box/round_box/segment/triangle/equilateral_triangle/pentagon/hexagon; sqrt/abs/min/max only — the C++ reference for `crd-geometry-shader-helpers` v9e + `crd-sdf` v0), `ConvexHullView<T>` in `primitives.hpp` (non-owning hull view + `support`/`contains`). | ✅ 2026-05-13 |

Then v1a–v1g (the `-bvh` sub-module, complete) and v1i/v1j+ are the other crd-geometry sub-modules / slices — separate libraries, see `docs/phases/phase-3.1.7-geometry.md`.

## What you get today (v0a–v0f — the v0 sub-phase, complete — plus v1h hardening)

`#include <crd/geometry/primitives/primitives.hpp>` — all types + the basic
helpers + `ConvexHullView`. `#include <crd/geometry/primitives/closest_point.hpp>` —
the closest-point / distance catalogue (2D + 3D) + `closest_points(Segment,Segment)`.
`#include <crd/geometry/primitives/intersect.hpp>` — the intersection corpus (ray
casts + boolean overlap, 2D + 3D). `#include <crd/geometry/primitives/barycentric.hpp>`
— tetrahedron barycentric/contains, `from_barycentric`, the 3-tet prism split.
`#include <crd/geometry/primitives/formulary.hpp>` — the iq smooth-blend & domain
operators. `#include <crd/geometry/primitives/signed_distance.hpp>` — the iq analytic
SDFs (`sd_*`). `#include <crd/geometry/primitives/constants.hpp>` — the epsilon/tolerance
policy (`k_*`). `#include <crd/geometry/primitives/is_finite.hpp>` — `is_finite` /
`all_finite` + the NaN/Inf contract. `#include <crd/geometry/primitives/format.hpp>` —
`std::format` support. Namespace `crd::geometry::primitives`. Link `crd-geometry-primitives`
(it pulls `crd-core` + `crd-math` PUBLIC).

Types (templated on `T : crd::math::MathScalar`):
- **3D** — `Line3`, `Segment3`, `Ray3`, `Plane`, `AABB3`, `OBB3`, `Sphere`,
  `Capsule3`, `Cylinder3`, `Triangle3`, `Tetrahedron`, `Frustum`, `Point3<T>` (= `Vec3<T>`).
- **2D** — `Line2`, `Segment2`, `Ray2`, `AABB2`, `OBB2`, `Circle`, `Capsule2`,
  `Cylinder2`, `Triangle2`, `Point2<T>` (= `Vec2<T>`). (No `Plane2` — a 2D
  oriented line *is* the `Plane`-analog; `signed_distance(Line2,·)` covers it.
  No `Frustum2`.) `Cylinder` = a flat-cap segment-with-radius (contrast
  `Capsule`'s hemispherical caps); `Cylinder2` is the "thick segment" /
  axis-parameterised rectangle.
- Scalar aliases throughout: `Line3f`/`AABB3d`/`Circlef`/`Cylinder3f`/`Triangle2d`/…

Basic helpers (migrated v0a + the 2D peers): `point_at`, plane normalise/
construct/`signed_distance`/`closest_point(Plane)`, `center`/`extents`/`contains`/
`intersects`/`positive_vertex` for AABB2/AABB3, `contains`/`intersects` for
Sphere & Circle (incl. AABB↔Sphere/Circle), `centroid`/`normal`/`barycentric`/
`contains`/`signed_area` for triangles, `intersect_ray_plane`/`_sphere`/`_triangle`
(Möller-Trumbore — 3D), `frustum_from_view_projection`/`contains`/`intersects`.

Closest-point catalogue (v0b — `closest_point.hpp`): for every primitive,
`closest_point(shape, p)` + `distance(shape, p)` + `distance_squared(shape, p)`;
plus `closest_param(Segment, p)` (the t∈[0,1]) and `closest_points(seg1, seg2,
out_c1, out_c2)` + `distance(seg1, seg2)`. Triangle uses the **Ericson §5.1.5
Voronoi-region cascade** (7 dot-product regions, branch-light); segment↔segment
the **Ericson §5.1.9** form (robust on parallel & degenerate). Sphere/Circle/
Capsule return the closest point on the *surface/boundary* (well-defined for an
interior `p`); a degenerate query (on the center/spine) resolves to a fixed `+x`
direction so the result is deterministic. The dimension-agnostic cores are
written once over a vector concept and used by both the 2D and 3D overloads.

Intersection corpus (v0c — `intersect.hpp`):
- **Ray casts** — `intersect_ray_{aabb,obb,cylinder,capsule}(ray, shape, T& out_t, eps)` (3D) and `intersect_ray2_{aabb,obb,line,segment,circle,capsule,cylinder}` (2D) — nearest hit with t ≥ 0 (the `intersect_ray_plane/_sphere/_triangle` convention from `primitives.hpp`). Ray↔AABB/OBB is the branch-light Tavianator slab (the robust precomputed-`RayPacket` form is v0f).
- **Boolean overlap** — extends the `intersects` set: `intersects(AABB3,Triangle3)` (Akenine-Möller 2001, 13-axis SAT), `intersects(OBB3,OBB3)` (15-axis SAT), `intersects(OBB2,OBB2)` (4-axis), `intersects(Triangle2,Triangle2)` (6-axis SAT), `intersects(Triangle3,Triangle3)` (Möller 1997 — coplanar case delegates to the 2D test), `segments_intersect(Segment2,Segment2)` (orientation tests; an out-point overload too), the `Sphere`/`Circle`/`Capsule` ↔ `{AABB,OBB,Triangle,Segment,Plane,Capsule}` reductions (exact — `distance_squared(X, center/spine) ≤ r²` via the v0b machinery), `intersects(Plane,{AABB3,OBB3,Sphere})`, `intersects(Frustum,OBB3)`, `intersects(Triangle2,{AABB2,Segment2,Circle})`, `intersects(Line2,{AABB2,OBB2})`. `intersects(Capsule3,{AABB3,OBB3})` is **conservative** (segment-vs-box-grown-by-r — no false negatives; false positives only in the box's rounded-corner Minkowski region; the exact SAT-box-pair is eylem v2). Determinism: SAT axes in a fixed enumeration order; a near-zero cross-product axis is skipped (never a false "separated").

Barycentric / tetrahedron (v0d — `barycentric.hpp`; the triangle barycentric forms are in `primitives.hpp`): `barycentric(Tetrahedron, p) → Vec4` (the 4 weights as signed-volume ratios — orientation-stable, asserts on a flat tetra), `contains(Tetrahedron, p)` (all weights ≥ −ε), `from_barycentric(Triangle3|Triangle2|Tetrahedron, weights)` (the inverse — reconstruct the point), `decompose_prism_to_tets(Triangle3 bottom, Triangle3 top) → StaticArray<Tetrahedron,3>` (the canonical 3-tet split of a triangular wedge through `a₀`, fixed diagonal so neighbouring wedges share matching tets — the Marching-Tetrahedra / FEM-meshing form). Plus `centroid`/`signed_volume`/`volume` for `Tetrahedron` (in `primitives.hpp`; `signed_volume = (1/6)·det[b−a|c−a|d−a]`, positive when (a,b,c,d) is positively oriented).

iq formulary (v0e — `formulary.hpp`; SDF combinators after Inigo Quilez): smooth-min/max — `smin_poly`/`smin_cubic` (C¹/C² quadratic & cubic polynomial; exact `min` outside the ±k blend band, dip = k at the crossover, collapse to `min` as k→0) and `smin_exp` (associative — order-free chaining; always a hair below `min`; via `crd::math::deterministic::exp2/log2`), plus the `smax_*` counterparts. Value-domain ops — `op_round(d, r)` (inflate by r), `op_onion(d, t)` (shell of half-thickness t), `extrude_2d(d2, p_z, h)` (2D-SDF + z → 3D-SDF). Position-domain ops — `domain_repeat`/`domain_mirror` (tile / mirror-tile a centered cell; 2D/3D/scalar; exact, no trig — `std::floor`-based modulo), `domain_elongate(p, h)` (stretch a shape's domain by ±h), `domain_twist(p, k)` / `domain_bend(p, k)` (rigid rotational warps; via `crd::math::deterministic::sin/cos`). These are the ULP-conformance reference for the GLSL/HLSL twins that `crd-geometry-shader-helpers` (v9e) will emit.

`reduce_argmax_with_lex_tiebreak` (v0e — `crd/math/simd/reduce.hpp`, `crd::math::simd`): a scalar-deterministic horizontal argmax over a `Vec8f`/`Vec4f` chunk (fold chunk-by-chunk via an `ArgmaxLex{index,score,x,y,z}` running-best and `argmax_lex_beats`) — wins by highest `score`, ties by lexicographically-smallest `(x,y,z)`, then by earliest global index; NaN-scores never win; partition-independent (folding 16 lanes as 8+8 / 4×4 / 1×16 gives the same result). ADR-0076 §4 pin #10 — what geometry v3 Quickhull needs to pick the furthest point from a face identically on every SIMD width.

Cutting-edge / branchless / SIMD ray corpus (v0f — closes the v0 sub-phase):
- `watertight_ray_tri.hpp` — **Woop / Benthin / Wald 2013** watertight ray-tri: `precompute_ray_tri(Ray3) → RayTriShear` (per-ray shear+permuted-axes — pick the dominant `dir` axis max-then-X-then-Y-then-Z, then map `dir` to +Z) + `intersect_ray_triangle_watertight(Ray3, RayTriShear, Triangle3, t, bary, cull_back, tnear)` (2D edge functions; an edge function landing exactly on 0 is re-evaluated in `double` and accepted if still 0 — "closed on-edge"; a ray grazing an edge shared by two triangles hits ≥1 of them, never falls through). Plus a Baldwin-Weber-style **precomputed per-triangle affine transform**: `precompute_triangle_affine(Triangle3) → TriAffine` (the inverse of `[edge1 | edge2 | normal]` — robust by construction, det = ‖normal‖²; 12 f32 / 48 B) + `intersect_ray_triangle_precomputed(Ray3, TriAffine, t, bary)` (branchless per-ray). `crd-geometry-mesh` v4d picks: Woop for dynamic / BVH-leaf, the precomputed affine for cooked statics.
- `robust_ray_aabb.hpp` — **Williams 2005** precomputed slab + **Ize 2013** conservative `tmax` widening (`× (1 + 2γ₃)` so accumulated `inv_dir` rounding never drops a true surface hit): `RayAABBPrecompute<T>{inv_dir, sign[3]}` (the single-ray precompute the `-bvh` v1g traversal consumes) + `intersect_ray_aabb_robust(Ray3, RayAABBPrecompute, AABB3, t0, t1, out_t)`. NaN/∞-safe (a zero `dir` component → `inv = ±∞` and the IEEE `min`/`max` drop that axis). `intersect_ray_aabb` (the un-precomputed slab) stays in `intersect.hpp` (v0c) as the cross-check reference.
- `plucker.hpp` — Plücker-coordinate edge classification: `PluckerLine<T>{d, m}` + `plucker_from(Line3|Segment3|Ray3|p,q)` + `plucker_side(a, b)` (= `dot(a.d, b.m) + dot(b.d, a.m)` — fixed sum order, sign-zero = "the lines are coplanar / on each other"; ADR-0076 §4 #13) + `intersect_ray_triangle_plucker(Ray3, Triangle3, tmax, tnear)` (sign-only all-same-side test as the reject; only on a pierce does it compute the one plane-`t` to clip the ray — the cheapest ray-tri *boolean*, watertight on shared edges).

SIMD batch kernels (v0f — `simd_batch.hpp` declarations, `simd_batch.cpp` out-of-line, f32/`Vec8f`) — **the two BVH-traversal shapes**:
- **leaf-batch** (one ray vs N primitives held as AoSoA columns): `ray_vs_8_aabb(Ray3f, RayAABBPrecompute<f32>, Aabb8, t0, t1) → {Vec8f t_enter, Vec8f hit_mask}` (Williams slab × 8 child boxes), `ray_vs_8_triangle(Ray3f, Triangle38, cull_back, tnear) → {Vec8f t, u, v, hit_mask}` (Möller-Trumbore × 8 leaf triangles), `aabb8_vs_aabb(Aabb8, AABB3f) → Vec8f mask`, `sphere8_vs_sphere(Sphere8, Spheref) → Vec8f mask`.
- **packet** (N coherent rays vs one box per node — Wald-style; each lane an independent ray): `precompute_ray_packet8(ox8,oy8,oz8, dx8,dy8,dz8) → RayPacket8` + `ray_packet8_vs_aabb(RayPacket8, AABB3f, t0, t1) → {Vec8f t_enter, Vec8f hit_mask}` (per-lane signs handled by the `min(t1,t2)`/`max(t1,t2)` form — no explicit sign bits).
- `segment8_vs_segment_distsq(Segment38Pair) → Vec8f` — eight segment-pair squared closest distances (Ericson §5.1.9 in SIMD with masked selects; robust on parallel / degenerate; eylem broadphase capsule-vs-N-capsule).

All comparisons return all-bits-set masks; feed the kernels' `Vec8f` outputs to `reduce_argmax_with_lex_tiebreak` when a "best lane" is wanted. A partial tail (< 8 valid items) is the caller's mask. Each kernel is ULP-cross-checked lane-by-lane against its scalar reference (`intersect.hpp` / `closest_point.hpp` / `robust_ray_aabb.hpp`). (Follow-up: wire `simd_batch.cpp`'s `.obj` into the `crd-simd-emission-check` scan — currently it inspects only `crd-math`'s SIMD `.obj`.)

Hardening (v1h — `constants.hpp`, `is_finite.hpp`, `signed_distance.hpp`, `ConvexHullView`):
- **`constants.hpp`** — `template <MathScalar T> constexpr T k_*()`: `k_distance_epsilon` / `k_area_epsilon` / `k_parallel_epsilon` (dot near-zero) / `k_degenerate_extent_epsilon` (zero-extent test) — `1e-6F` for `f32`, `1e-12` for `f64`; `k_sah_cost_epsilon` (`bvh_build_internal.hpp` re-exports this — one source of truth); `k_default_fat_margin` = `0.1` (a *world unit*, matches `DynamicBvhConfig`); `k_robust_aabb_pad_ulps` = `3` (`ray_aabb_robust_pad<T>()` derives `γ_n` from it). A value-preserving rename — the existing function-signature defaults still bind `crd::math::default_epsilon<T>()`; migrating them onto the named constants is a later pass.
- **`is_finite.hpp`** — `is_finite(x)` for `Vec2`/`Vec3` and every primitive (3D Line3/Segment3/Ray3/Plane/Sphere/AABB3/OBB3/Capsule3/Cylinder3/Triangle3/Tetrahedron/Frustum/ConvexHullView; 2D peers) + `all_finite(ConstSpan<Primitive>)`. **The NaN/Inf contract:** queries tolerate (a NaN/∞ primitive is never-hit / never-closest, never UB — the v0c/v0f IEEE-comparison corpus already gives this; finiteness predicates are *not* on the query path), builders reject in debug — `bvh_build` / `bvh_build_parallel` `CRD_ASSERT(all_finite(prims))`, `DynamicBvh::insert`/`update` `CRD_ASSERT(is_finite(aabb))`, `bvh4_collapse` `CRD_ASSERT(is_finite(binary.bounds()))`; release → valid-but-possibly-degenerate, still no UB. Internal sentinels are exempt: `aabb_empty() = {+∞,−∞}` is a deliberate non-finite union identity.
- **`signed_distance.hpp`** — 20 Inigo Quilez analytic SDFs: 3D `sd_sphere`/`sd_box`/`sd_round_box`/`sd_box_frame`/`sd_plane`/`sd_capsule`/`sd_cylinder`/`sd_cone`/`sd_torus`/`sd_triangle` (unsigned)/`sd_ellipsoid` (iq bound)/`sd_octahedron`; 2D `sd_circle`/`sd_box_2d`/`sd_round_box_2d`/`sd_segment_2d` (unsigned)/`sd_triangle_2d` (signed)/`sd_equilateral_triangle_2d`/`sd_pentagon_2d`/`sd_hexagon_2d`. `sqrt`/`abs`/`min`/`max`/`clamp`/`dot`/`length` only (no transcendental libm); component-wise helpers (`vabs`/`vmax0`/`max3`/`sgn`) in a `sd_detail` sub-namespace so `crd-math` stays lean. The C++ scalar reference `crd-geometry-shader-helpers` (v9e) emits GLSL/HLSL twins of and `crd-sdf` v0 reuses (`closest_point.hpp` gives the unsigned distance + point on the *primitive structs* in world space; this gives the signed, negative-inside value in *shape-local* space). Strong tests: cross-checked vs `closest_point.hpp`'s `distance()` for sphere/box/circle/box_2d/segment_2d/capsule/plane/triangle, sign vs `contains`, closed-form spot values for the rest, 1-Lipschitz on random pairs (ellipsoid exempt — it is a bound).
- **`ConvexHullView<T>`** (in `primitives.hpp`) — non-owning view: `ConstSpan<Vec3> vertices` + `ConstSpan<Plane> faces` (outward-facing) + `ConstSpan<u32> face_vertex_indices` + `ConstSpan<u32> face_vertex_offsets` (prefix-sum, size `faces+1`, so face `f` owns `indices[offsets[f]..offsets[f+1])`). `support(hull, dir)` (extreme vertex, lowest-index tiebreak), `contains(hull, point)` (inside iff `signed_distance ≤ ε` for every face). `crd-convex` v3 *produces* one, `crd-eylem`'s `Collider::ConvexHull` references one; ray-vs-hull / closest-point-on-hull / GJK-contains land in v2.

```cpp
using namespace crd::geometry::primitives;
const Triangle3f tri(Vec3f(-1,-1,0), Vec3f(1,-1,0), Vec3f(0,1,0));
const Vec3f q = closest_point(tri, Vec3f(0.25F, 0.25F, 9.0F)); // → (0.25, 0.25, 0)
const float d = distance(Capsule3f(Vec3f(0,0,0), Vec3f(0,0,10), 1.0F), Vec3f(5,0,4)); // → 4
float t = 0.0F;
if (intersect_ray_obb(Ray3f(Vec3f(-5,0,0), Vec3f(1,0,0)), OBB3f({}, Vec3f(1,1,1), Mat3f::identity()), t)) { /* t == 4 */ }
const bool overlap = intersects(AABB3f(Vec3f(-1,-1,-1), Vec3f(1,1,1)), tri); // 13-axis SAT
Vec2f c1{}, c2{};
closest_points(Segment2f(Vec2f(-1,0), Vec2f(1,0)), Segment2f(Vec2f(0,-1), Vec2f(0,1)), c1, c2); // ≈ origin, origin
```

## Naming rule (pin — read before adding a type; ADR-0076 §14)

All shape types are templated on the scalar `T`. **Where a concept has both a 2D
and a 3D form under the same name, BOTH carry a dimension suffix** — `Line2`/
`Line3`, `Segment2`/`Segment3`, `Ray2`/`Ray3`, `AABB2`/`AABB3`, `OBB2`/`OBB3`,
`Triangle2`/`Triangle3`, `Capsule2`/`Capsule3` — mirroring `crd::math::Vec2`/
`Vec3`/`Mat2`/`Mat3`. Where the forms have distinct natural names, neither is
suffixed: `Circle` (2D) / `Sphere` (3D). Where only one dimension exists, no
suffix: `Plane`, `Frustum`, `Tetrahedron` (3D-only). `Point2`/`Point3` alias
`Vec2`/`Vec3`.

## API layers

Two-layer per ADR-0076 §5 (mirrors `crd-hesap`):
- **Typed C++ "Eigen-class" layer** — what v0a ships: zero-overhead inlined
  templates, data-oriented (consumers pass `ConstSpan` of vertex/index data,
  never `Mesh*` objects; functional form `bvh_build(...)` not `BvhTree::build`).
  This is what eylem / sdf / renderer / scene call.
- **Opt-in cooker/editor handle-based façade** — reserved for later sub-slices;
  nothing in v0a forbids it.

## Determinism

Inherits the ADR-0063 contract (ADR-0076 §4): no `std::sin/cos/tan/exp/log/pow`
in this module (the `crd-no-std-math-check` CI guard now scopes
`engine/geometry-primitives`); `std::sqrt` is allowed (IEEE-754 mandates a
correctly-rounded single-rounding sqrt everywhere). Algorithm-specific tiebreaks
(GJK simplex Ericson-not-vandenBergen, SAH-split X-then-Y-then-Z, Quickhull lex
order, watertight ray-tri axis selection, Plücker sign-zero) are pinned in
ADR-0076 §4 and land with their algorithms in v0c–v0f / v2+. The `signed_distance.hpp`
SDFs (v1h) are deliberately `sqrt`/`abs`/`min`/`max` only — no transcendental
needed for any of the 20, so the GLSL/HLSL twins are ULP-portable.

**Tolerance policy (v1h):** every geometry tolerance is named in `constants.hpp`
by intent (`k_distance_epsilon`, `k_parallel_epsilon`, …) — when adding a helper
that needs an ε, reach for the named constant whose *meaning* matches, don't
introduce a fresh `1e-6F`. **NaN/Inf (v1h):** queries tolerate (return
no-hit/false/NaN, never UB); accelerator builders `CRD_ASSERT` finite *inputs*
in debug — never assert on internal accumulators (the `aabb_empty()` sentinel is
non-finite on purpose).

## Sibling: `crd-geometry-shader-helpers`

v0e created `engine/geometry-shader-helpers/` (target `crd-geometry-shader-helpers`, namespace `crd::geometry::shader_helpers`) — currently a **skeleton** (a force-link stub + a reserved header). It is the 11th `crd-geometry` sub-module (ADR-0076 §1): the GLSL/HLSL twin of this module's primitive distance functions + the iq formulary, *cooked* at build time and consumed by the renderer's DFAO / DF-soft-shadow passes (Phase 3.5+), font MTSDF, and the editor preview. v9e fills in the formula-IR cooker, the GLSL/HLSL backends, and the ULP-conformance test that checks the emitted shader math against `formulary.hpp` (the C++ scalar reference).

## References

- `docs/decisions/0076-geometry-substrate-architecture.md` — the architecture (§1 sub-modules, §3/§5 API, §4 determinism, §13 the move-and-delete + the v0f corpus)
- `docs/phases/phase-3.1.7-geometry.md` — the 30-slice phase plan
- `docs/research/cerid-geometry.md` + `docs/research/cerid-geometry-supplement.md` — the research dossiers
