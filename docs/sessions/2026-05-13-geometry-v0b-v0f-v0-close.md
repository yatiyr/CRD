# Session — 2026-05-13 — Phase 3.1.7 v0b → v0f: the `crd-geometry-primitives` v0 sub-phase, closed

## Goal

Land v0b through v0f of `crd-geometry-primitives` in one push and close the v0 sub-phase
with a full 14/17-config sweep. v0b–v0e verified on win-debug + win-shipping only (user
directive, for speed); v0f closes v0, so it gets the full `scripts/full-sweep.ps1`.

## What we built / changed

- **v0b — 2D peer set + closest-point catalogue + segment↔segment** (ADR-0076 §14 amendment).
  Added 2D types `Line2`/`Segment2`/`Ray2`/`AABB2`/`OBB2`/`Circle`/`Capsule2`/`Triangle2`/`Point2`
  + `operator==`/helpers/formatters. **Renamed** the v0a 3D types to their `…3` forms
  (`Line→Line3` … `Capsule→Capsule3`; `Plane`/`Sphere`/`Frustum`/`Triangle3`/`Point3`
  unchanged) per the new **dimension-suffix naming rule** (both forms suffixed where the
  natural name collides; distinct natural names like `Circle`/`Sphere` unsuffixed;
  single-dimension types like `Plane`/`Frustum`/`Tetrahedron` unsuffixed). ~8 consumers
  repointed. New `closest_point.hpp`: `closest_point` + `closest_param(Segment)` + `distance`
  + `distance_squared` for every primitive (2D & 3D), plus `closest_points(seg1,seg2,…)` (the
  mutually-closest pair) + `distance(seg1,seg2)`. Dimension-agnostic cores: Ericson §5.1.5
  Voronoi-region closest-point-on-triangle (+ collinear-triangle fallback), Ericson §5.1.9
  segment↔segment, clamped line-projection. `test_closest_point.cpp` (TEMPLATE_TEST_CASE
  float+double).

- **v0c — intersection corpus (2D + 3D)** (user-confirmed `Cylinder3`/`Cylinder2` addition).
  New `intersect.hpp`: ray casts `intersect_ray_{aabb,obb,cylinder,capsule}` (3D, Tavianator
  branch-light slab) + 2D `intersect_ray2_{aabb,obb,line,segment,circle,capsule,cylinder}`.
  Boolean overlap: `intersects(AABB3,Triangle3)` (Akenine-Möller 2001 13-axis SAT — rewrote
  the 9 edge-cross axes as a clean `sep(axis)` lambda after the first hand-derived version had
  a vertex-pair-selection bug the test's brute-force cross-check caught), `intersects(OBB3,OBB3)`
  (15-axis SAT), `intersects(OBB2,OBB2)` (4-axis), `intersects(Triangle2,Triangle2)` (6-axis),
  `intersects(Triangle3,Triangle3)` (Möller 1997; coplanar → the 2D test), `segments_intersect(Segment2,Segment2)`,
  sphere/circle/capsule ↔ X reductions via v0b `distance_squared`, plane↔box, frustum↔OBB3,
  several 2D reductions. `intersects(Capsule3,{AABB3,OBB3})` is **conservative** (segment-vs-
  box-grown-by-r — no false negatives; exact SAT-box-pair is eylem v2). New types
  `Cylinder3`/`Cylinder2` (flat-cap segment+radius — distinct from `Capsule`'s hemispheres).
  `test_intersect.cpp`.

- **v0d — `Tetrahedron` + barycentric / tetrahedron utilities** (Ericson §3.4). Type
  `Tetrahedron` (3D-only → no suffix) + `centroid`/`signed_volume`(`= (1/6)·det[b−a|c−a|d−a]`)/
  `volume`/`operator==`/formatter/aliases. New `barycentric.hpp`: `barycentric(Tetrahedron,p)→Vec4`
  (signed-volume ratios — orientation-stable; asserts on a flat tetra), `contains(Tetrahedron,p)`,
  `from_barycentric` (the inverse, for `Triangle3`/`Triangle2`/`Tetrahedron`),
  `decompose_prism_to_tets(bottom,top)→StaticArray<Tetrahedron,3>` (the canonical 3-tet split
  of a triangular wedge through `a₀` — Marching-Tetrahedra / FEM hex-to-tet form, fixed
  diagonal so neighbouring wedges share matching tets). `test_barycentric.cpp`.

- **v0e — iq formulary + `reduce_argmax_with_lex_tiebreak` + shader-helpers skeleton.** New
  `formulary.hpp`: smooth-min/max `smin_poly`/`smin_cubic` (iq quadratic & cubic; exact `min`
  outside the ±k band, dip = k at the crossover, →`min` as k→0) + `smin_exp` (associative; via
  `crd::math::deterministic::exp2/log2`) + `smax_*`; value-domain `op_round`/`op_onion`/`extrude_2d`;
  position-domain `domain_repeat`/`domain_mirror` (exact `std::floor`-modulo, no trig) +
  `domain_elongate` + `domain_twist`/`domain_bend` (via `crd::math::deterministic::sin/cos`).
  New `crd::math::simd::reduce_argmax_with_lex_tiebreak` (`crd/math/simd/reduce.hpp`, wired into
  `simd.hpp`): scalar-deterministic horizontal argmax over a `Vec8f`/`Vec4f` chunk —
  `ArgmaxLex{index,score,x,y,z}` running-best + `argmax_lex_beats` (score-desc → (x,y,z)-asc-lex
  → earliest-index; NaN-scores never win; partition-independent) — the ADR-0076 §4 #10
  determinism pin v3 Quickhull needs. New module `engine/geometry-shader-helpers/` (target
  `crd-geometry-shader-helpers`, namespace `crd::geometry::shader_helpers`) — **skeleton only**
  (force-link stub + reserved header; the formula-IR cooker + GLSL/HLSL backends + ULP-conformance
  test land in v9e); added to root `CMakeLists.txt` and the `crd-no-std-math-check` scoped dirs.
  New `tests/math/test_reduce.cpp` + `tests/geometry-primitives/test_formulary.cpp`.

- **v0f — cutting-edge / branchless / SIMD intersection corpus — closes the v0 sub-phase.**
  New scalar headers: `watertight_ray_tri.hpp` — Woop/Benthin/Wald 2013 watertight ray-tri
  (`precompute_ray_tri(Ray3)→RayTriShear` per-ray shear+scale to the dominant-axis frame,
  edge-function form, sign consistent across shared edges → no cracks; on an exact-zero edge
  function it recomputes that test in `double` when `T==float`; the default ray-tri for
  `-mesh` v4d leaves + `crd-sdf` v2 mesh-bake) + Baldwin-Weber 2016 precomputed per-triangle
  affine (`TriAffine` = inverse of `[e1|e2|n]`, ~9 mul/ray, branchless — opt-in for cooked
  static meshes). `robust_ray_aabb.hpp` — `RayAABBPrecompute{inv_dir, sign[3]}` +
  `intersect_ray_aabb_robust` (Williams 2005 precomputed slab + Ize 2013 conservative `tmax`
  widening `×(1+2γ₃)`; NaN-safe min/max). `plucker.hpp` — `PluckerLine{d,m}` +
  `plucker_from(Segment3|Ray3|Line3|p,q)` + `plucker_side(a,b)=dot(a.d,b.m)+dot(b.d,a.m)`
  (fixed sum order, sign-zero = on-the-line) + branchless `intersect_ray_triangle_plucker`.
  **Single-ray vs multi-ray made explicit:** the scalar precompute structs are *per-ray*
  (leaf-batch — one ray vs the N AoSoA primitive columns of a BVH leaf); the `Vec8f`
  `RayPacket8` is *per-packet* (Wald-style — 8 coherent rays, the box scalar/broadcast). New
  SIMD piece: `simd_batch.hpp` + out-of-line `engine/geometry-primitives/src/simd_batch.cpp`
  (so `crd-simd-emission-check` has a SIMD `.obj` to inspect) — `Vec8f` AoSoA bundles
  `Aabb8`/`Sphere8`/`Triangle38`/`Segment38Pair` + kernels `ray_vs_8_aabb` (1 ray, 8 child
  boxes), `ray_packet8_vs_aabb` (8 rays, 1 box), `ray_vs_8_triangle` (Möller-Trumbore ×8,
  `cull_back` flag), `aabb8_vs_aabb`, `sphere8_vs_sphere`, `segment8_vs_segment_distsq`
  (Ericson §5.1.9 in SIMD via masked selects). All comparisons return all-bits-set masks;
  `1/x` is `_mm256_div_ps`. New `test_v0f_corpus.cpp` (scalar — Woop basic+back-cull,
  shared-edge no-crack property, Woop↔v0c-MT agreement on a random corpus, Baldwin-Weber↔Woop,
  robust ray-AABB vs v0c slab + conservatism, Plücker side-values + ray-tri↔Woop) +
  `test_v0f_simd.cpp` (each kernel lane-by-lane vs its scalar reference; mask convention
  hit≠0/miss==0).

- **Sweep-driven fixes** (the v0-close sweep surfaced what win-debug-only had hidden):
  (a) GCC `-Werror=shadow` — `intersects(Triangle3,Triangle3)`'s `interval` lambda params
  `d0/d1/d2` shadowed outer `d1`/`d2`; renamed `e0/e1/e2`.
  (b) **Non-ASCII characters in `TEST_CASE` names break Windows `ctest` name-matching** — the
  em-dash/`↔`/`×`/`≠`/`ö` in the new v0b–v0f test-case names made `ctest` report `(Failed)` on
  all 9 Windows configs even though the code was fine; replaced with ASCII in every new test
  file. `scripts/check_no_non_ascii_test_names` (which exists for exactly this) is now green.
  (c) The `.sh` `crd-no-std-math-check` greps comments too — `// No std::sin/cos/exp/…`
  doc-comments in `barycentric.hpp`/`intersect.hpp`/`watertight_ray_tri.hpp` tripped it;
  reworded to "No transcendental libm calls".

## Decisions made

- **Dimension-suffix naming rule** (ADR-0076 §14 amendment): where 2D and 3D peers share a
  natural name → both suffixed (`Line2`/`Line3`, `AABB2`/`AABB3`, …); distinct natural names →
  unsuffixed (`Circle`/`Sphere`); single-dimension types → unsuffixed (`Plane`, `Frustum`,
  `Tetrahedron`).
- `Cylinder3`/`Cylinder2` are flat-cap (segment + radius), kept distinct from `Capsule*`'s
  hemispherical caps (user-confirmed in the v0c plan).
- `intersects(Capsule3,box)` ships *conservative* now; the exact SAT-box-pair is deferred to
  eylem v2. `intersects(Triangle2,AABB2)` likewise an approximation call.
- v0f exposes single-ray (leaf-batch) and multi-ray (`RayPacket8`) traversal precomputes as
  separate, documented forms — the BVH layer (`-bvh` v1g) picks per call site.
- The "win-debug + win-shipping only" verification shortcut was a v0-internal speed measure; it
  **expired at v0 close**. v1+ slices use the normal DoD (full sweep at slice close unless the
  user sets a narrower scope for a given slice).

## Files touched

- New: `engine/geometry-primitives/include/crd/geometry/primitives/{closest_point,intersect,
  barycentric,formulary,watertight_ray_tri,robust_ray_aabb,plucker,simd_batch}.hpp`,
  `engine/geometry-primitives/src/simd_batch.cpp`, `engine/geometry-shader-helpers/**`,
  `engine/math/include/crd/math/simd/reduce.hpp`,
  `tests/geometry-primitives/test_{closest_point,intersect,barycentric,formulary,v0f_corpus,v0f_simd}.cpp`,
  `tests/math/test_reduce.cpp`
- Modified: `engine/geometry-primitives/include/crd/geometry/primitives/{primitives,format}.hpp`
  (2D types + the `…3` rename + `Cylinder*` + `Tetrahedron`); `engine/math/include/crd/math/
  {math,format,simd/simd}.hpp`; `engine/scene/include/crd/scene/{query,world}.hpp` (the
  `AABB3<f32>` shape on the ADR-0053 reserved index); `runtime/examples/smoke_math.cpp`;
  `tests/bench/test_bench.cpp`; `tests/scene/test_phase_3_0_freeze.cpp`;
  `tests/{geometry-primitives,math}/CMakeLists.txt`; `CMakeLists.txt`;
  `scripts/check_no_std_math.{ps1,sh}`; `docs/decisions/0076-geometry-substrate-architecture.md`
  (§14 amendment); `docs/systems/geometry-primitives.md`; `docs/phases/phase-3.1.7-geometry.md`;
  `context.md`
- Deleted: `scripts/v0f-build-tmp.bat` (a throwaway build script — was accidentally tracked)

## Tests / verification

- Built? ✅ all 17 configs
- Tests pass? **`scripts/full-sweep.ps1` → RESULT: PASS** (17 configs, 0 failed, ~09:00).
  `crd-geometry-primitives-tests` 74 cases; `crd-math-tests` (with `test_reduce`); guards
  `crd-no-std-math-check` + `crd-no-non-ascii-test-names` + `crd-simd-emission-check` green;
  clang-format / clang-tidy clean on changed files.
- (The first two sweep attempts failed on the GCC-shadow + non-ASCII-test-name + std-math-
  comment issues above — all fixed, third attempt clean.)

## Next session starts with

- Phase 3.1.7 **v1a** — `crd-geometry-bvh`: binned-SAH BVH build + Catto 2019 refit + quad-BVH
  topology. (`-bvh` first; ADR-0076 §7 slice list, `docs/phases/phase-3.1.7-geometry.md`.)
- User commits the v0b–v0f changeset themselves (suggested commit split is in the chat at the
  end of the 2026-05-13 session).
