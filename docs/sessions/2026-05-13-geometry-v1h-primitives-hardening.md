# Session — 2026-05-13 — Phase 3.1.7 v1h: `crd-geometry-primitives` substrate hardening (ADR-0076 §15)

## Goal

The first of the three ADR-0076 §15 checklist-driven additions, slotted after the
`-bvh` sub-module (v1a–v1g, complete). v1h hardens the `-primitives` leaf
substrate: a named epsilon/tolerance policy, the NaN/Inf contract (with the
accelerator-builder asserts), the Inigo Quilez analytic signed-distance
functions in C++, and the `ConvexHullView` query-side hull type.

## What we built / changed

### `engine/geometry-primitives/include/crd/geometry/primitives/constants.hpp` (new)

The geometry-wide epsilon/tolerance policy, **named by intent, not magnitude** —
templated `template <MathScalar T> [[nodiscard]] constexpr T k_*() noexcept`
(the `k_<name>` snake form `bvh_build_internal.hpp` already established for
type-dependent constexpr): `k_distance_epsilon` / `k_area_epsilon` /
`k_parallel_epsilon` (dot near-zero) / `k_degenerate_extent_epsilon` (zero-extent
test) — `1e-6F` for `f32`, `1e-12` for `f64` (the `default_epsilon` step);
`k_sah_cost_epsilon` (`1e-6F`); `k_default_fat_margin` (`0.1` — a *world unit*,
not an ε, matches `DynamicBvhConfig`'s default); `k_robust_aabb_pad_ulps`
(`3` — the Ize-2013 `γ_n` ULP count). **Value-preserving rename**: the existing
function-signature defaults still bind `crd::math::default_epsilon<T>()`
(migrating those onto the named constants is a separate ergonomic pass — out of
v1h scope); the two retrofitted call-sites compute the *same* values they always
did:
- `engine/geometry-bvh/src/bvh_build_internal.hpp` — `k_sah_cost_epsilon` is now
  `crd::geometry::primitives::k_sah_cost_epsilon<crd::f32>()` (one source of
  truth; `bvh_build`/`bvh_build_parallel`'s SAH-tiebreak comparison unchanged →
  `crd-geometry-bvh-tests` stays 40/40, bit-for-bit).
- `engine/geometry-primitives/include/crd/geometry/primitives/robust_ray_aabb.hpp`
  — `ray_aabb_robust_pad<T>()` now derives `γ_n = (n·u)/(1−n·u)` with
  `n = k_robust_aabb_pad_ulps<T>()` (= 3) instead of the literal `3` → identical
  result for every `T`.

### `engine/geometry-primitives/include/crd/geometry/primitives/is_finite.hpp` (new)

`is_finite(x)` — true iff every component is a finite IEEE value — for `Vec2`/
`Vec3` and every primitive (3D Line3/Segment3/Ray3/Plane/Sphere/AABB3/OBB3/
Capsule3/Cylinder3/Triangle3/Tetrahedron/Frustum/ConvexHullView; the 2D peers) +
`all_finite(ConstSpan<Primitive>)` (the span form the accelerator builders
assert on). The header documents **the NaN/Inf contract** (ADR-0076 §15):
- *Queries tolerate.* A query (`raycast` / `overlap` / `closest_point` /
  `contains` / `signed_distance` / `intersects`) given a NaN/∞ primitive must not
  invoke UB — it returns "no hit" / "not contained" / a NaN result, never a
  crash. The IEEE-comparison semantics the v0c/v0f intersection corpus is built
  on already give this (`a < b` is false when either is NaN; the `min`/`max` slab
  ordering drops a ±∞ axis). The finiteness predicates are **not** on the query
  path and must not be sprinkled there.
- *Builders reject (in debug).* An accelerator builder that ingests a corpus it
  is about to permute / partition / hash `CRD_ASSERT` finite caller-supplied
  data first — a NaN centroid silently corrupts a SAH split / a tree rotation in
  ways that are miserable to debug downstream. Added: `bvh_build` /
  `bvh_build_parallel` `CRD_ASSERT(all_finite(prims))`; `DynamicBvh::insert` /
  `update` `CRD_ASSERT(is_finite(aabb))`; `bvh4_collapse`
  `CRD_ASSERT(is_finite(binary.bounds()))`. In release the assert compiles away
  and the builder produces a valid-but-possibly-degenerate structure (still no
  UB). `engine/geometry-bvh/src/{bvh_build,bvh_build_parallel,dynamic_bvh,bvh4}.cpp`
  gained `#include <crd/geometry/primitives/is_finite.hpp>` (the module already
  links `crd-geometry-primitives` PUBLIC).
- *Not subject to the contract: internal sentinels.* `aabb_empty() = {min = +∞,
  max = −∞}` is the deliberate identity for AABB union — `is_finite(aabb_empty())`
  is `false` by design and that is correct. Finiteness asserts go on *inputs*,
  never on accumulators. Pinned by a test.

### `engine/geometry-primitives/include/crd/geometry/primitives/signed_distance.hpp` (new)

20 Inigo Quilez analytic signed-distance functions translated to C++ over
`crd::math::Vec2/3`. `sd_*(p, params...)` returns the signed distance from a
query point `p` (in the shape's local frame — translate/rotate at the call site)
to the surface: negative inside, zero on the boundary, positive outside; for
shapes with no interior (a triangle / a segment in 3D) the result is the
*unsigned* distance, noted per function. The set:
- **3D** — `sd_sphere`, `sd_box`, `sd_round_box`, `sd_box_frame`, `sd_plane`,
  `sd_capsule`, `sd_cylinder` (flat caps), `sd_cone` (apex at origin, `c =
  (sinθ, cosθ)`), `sd_torus`, `sd_triangle` (unsigned — iq's `udTriangle`),
  `sd_ellipsoid` (iq's well-behaved bound — exact has no closed form),
  `sd_octahedron`.
- **2D** — `sd_circle`, `sd_box_2d`, `sd_round_box_2d`, `sd_segment_2d`
  (unsigned), `sd_triangle_2d` (signed — iq's `sdTriangle`),
  `sd_equilateral_triangle_2d`, `sd_pentagon_2d`, `sd_hexagon_2d`.

`sqrt` / `abs` / `min` / `max` / `clamp` / `dot` / `length` only — no
transcendental libm (so `crd-no-std-math-check` stays green, and the GLSL/HLSL
twins `crd-geometry-shader-helpers` v9e will emit are ULP-portable); constants
like `sqrt(3)` and the regular-polygon vertex cosines are pre-evaluated literals.
Component-wise vector helpers (`vabs`, `vmax0`, `max3`, `sgn`) live in a
`crd::geometry::primitives::sd_detail` sub-namespace — `crd-math` stays the lean
substrate. This is the C++ scalar reference `crd-geometry-shader-helpers` (v9e)
emits GLSL/HLSL twins of and `crd-sdf` v0 reuses; `closest_point.hpp` gives the
unsigned distance + closest point on the *primitive structs* in world space —
this gives the signed, negative-inside value in *shape-local* space the way SDF
pipelines expect.

### `ConvexHullView<T>` (in `primitives.hpp`)

A non-owning view of a convex polyhedron — the query-side hull (`crd-convex` v3
*produces* one, `crd-eylem`'s `Collider::ConvexHull` references one):
`ConstSpan<Vec3<T>> vertices` + `ConstSpan<Plane<T>> faces` (one outward-facing
plane per face — `dot(n, x) + d <= 0` is the inside half-space) +
`ConstSpan<u32> face_vertex_indices` packed CCW per face + `ConstSpan<u32>
face_vertex_offsets` (prefix-sum, size `faces.size() + 1`, so face `f` owns
`indices[offsets[f] .. offsets[f+1])`). Plus the two trivially-correct queries:
`support(hull, dir)` (the vertex maximising `dot(v, dir)`, lowest-index tiebreak
— replace the running best only on a strictly greater projection) and
`contains(hull, point, eps = default_epsilon)` (inside iff `signed_distance ≤
eps` for every face plane). `ray_vs_hull` / `closest_point_on_hull` /
GJK-`contains` land with GJK/EPA/SAT in `-convex` v2a–v2f — added now would blur
the slice boundary. `is_finite(ConvexHullView)` added in `is_finite.hpp`.
`primitives.hpp` gained `#include <crd/containers/span.hpp>` + `<crd/core/types.hpp>`.

### Tests (new — `tests/geometry-primitives/`)

- `test_constants.cpp` — every named constant equals its legacy magnitude; the
  two retrofitted call-sites (`k_sah_cost_epsilon`, `ray_aabb_robust_pad`) still
  compute the old values.
- `test_is_finite.cpp` — true/false matrix per primitive (finite ✓; one
  component NaN / +∞ / −∞ → false); the empty-AABB sentinel pinned as
  `is_finite == false` by design; `all_finite` span helper.
- `test_signed_distance.cpp` — three families: **strong cross-checks** vs
  `closest_point.hpp`'s `distance()` where a matching primitive exists
  (`sd_sphere`/`sd_box`/`sd_circle`/`sd_box_2d`/`sd_segment_2d`/`sd_capsule`/
  `sd_plane`/`sd_triangle`) + sign vs `contains`; **closed-form spot values** at
  axis points for the rest (`sd_round_box(O,b,r) == -min(b)`,
  `sd_torus((maj,0,0)) == -min`, `sd_octahedron(O,s) == -s/√3`,
  `sd_ellipsoid((2r.x,0,0)) == r.x`, …); **1-Lipschitz** (`|sd(p)−sd(q)| ≤
  |p−q|`) on random pairs for every function (ellipsoid exempt — it is a bound,
  not an exact SDF).
- `test_convex_hull_view.cpp` — `support` picks the right corner (incl. a
  face-normal-direction tie → lowest index); `contains` == inside every
  half-space; `is_finite` + the offsets layout addresses each face's verts.

`tests/geometry-primitives/CMakeLists.txt` gained the four files.

## Plain-English explanation

The geometry substrate had a scatter of bare `1e-6F`s and `default_epsilon`s
whose meaning had to be guessed; v1h gathers them into `constants.hpp` named by
*what they're for* (a distance ε, a parallelism ε, a fat-AABB margin) — same
numbers, one place. It writes down the rule for NaN/∞ inputs that the corpus had
been following implicitly (queries don't crash, they just don't hit; builders
that are about to *sort* your data check it's finite first) and adds those
checks to the BVH/DynamicBvh builders. It ships Inigo Quilez's library of
closed-form distance-to-a-shape functions in C++ — `sd_box`, `sd_torus`,
`sd_octahedron`, the 2D polygons — which the renderer's distance-field passes and
`crd-sdf` will reuse, and which the shader-helpers cooker will mirror to GLSL.
And it adds `ConvexHullView` — a lightweight "here's a convex shape as some
vertices + face planes" handle — with the two queries that need nothing but a
dot product (which vertex is furthest that way; is this point inside); the real
convex-shape machinery (GJK/EPA/SAT) is `-convex` v2.

## Decisions made

- **Value-preserving rename, not a retune.** `constants.hpp`'s values *equal* the
  pre-v1h magic numbers; the retrofit (`bvh_build_internal.hpp`,
  `robust_ray_aabb.hpp`) is mechanical; `crd-geometry-bvh-tests` stays 40/40
  bit-for-bit. The function-signature default arguments stay on
  `crd::math::default_epsilon<T>()` — switching them to the named constants is a
  later ergonomic pass, deliberately not bundled here.
- **Builder-reject = on caller-supplied inputs, not internal accumulators.** The
  `aabb_empty() = {+∞,−∞}` union sentinel is non-finite by design — a test pins
  that, and `is_finite.hpp` documents it, so the next reader doesn't "fix" it.
- **SDF menu of 20, named not "~30".** The phase doc said "~30"; that's a
  planning estimate. v1h ships a concrete, all-closed-form, all-mirrorable set:
  12 3D + 8 2D, every one from iq's articles. The rest (more polygons, the
  rounded variants of each 3D shape, etc.) are a follow-up if benchmarks /
  consumers demand — trying to ship 30 in one slice is the failure mode.
- **`ConvexHullView` queries: `support` + `contains` only in v1h.** Ray-vs-hull /
  closest-point-on-hull / contains-via-GJK belong to v2a–v2f (the GJK/EPA/SAT
  slice). Adding them now would couple v1h to v2's simplex machinery.
- **`sd_triangle` (3D) is unsigned** — a triangle is a 2-manifold with no
  interior; iq's `udTriangle`. A signed-by-plane-side form is a mesh / thin-shell
  concern, not the primitive layer. `sd_segment_2d` likewise unsigned.
- **Component-wise vector helpers in a `sd_detail` sub-namespace** — `vabs`,
  `vmax0`, `max3`, `sgn`. They're genuinely useful but `crd-math` stays lean
  (ADR-0076 §13 pin); they're an implementation detail of the iq translations.

## Files touched

- New: `engine/geometry-primitives/include/crd/geometry/primitives/{constants,is_finite,signed_distance}.hpp`,
  `tests/geometry-primitives/{test_constants,test_is_finite,test_signed_distance,test_convex_hull_view}.cpp`,
  this session log.
- Modified: `engine/geometry-primitives/include/crd/geometry/primitives/primitives.hpp`
  (`ConvexHullView` + `support` + `contains` + the new includes),
  `engine/geometry-primitives/include/crd/geometry/primitives/robust_ray_aabb.hpp`
  (`ray_aabb_robust_pad` derives `n` from `k_robust_aabb_pad_ulps`),
  `engine/geometry-bvh/src/bvh_build_internal.hpp` (re-exports `k_sah_cost_epsilon`),
  `engine/geometry-bvh/src/{bvh_build,bvh_build_parallel,dynamic_bvh,bvh4}.cpp`
  (the builder-reject asserts + the `is_finite.hpp` include),
  `tests/geometry-primitives/CMakeLists.txt`,
  `docs/systems/geometry-primitives.md`, `docs/phases/phase-3.1.7-geometry.md`,
  `context.md`.

## Tests / verification

- Built? ✅ — win-debug (incl. `crd-bench`), win-asan, win-shipping, win-tidy
  (clean with `/WX`; win-tidy build = the clang-tidy gate — my new files
  produced zero warnings; the pre-existing `bugprone-unchecked-optional-access`
  notes in `test_bvh_raycast.cpp` / `test_bvh4.cpp` are from v1a/v1d, unchanged).
- Tests pass? ✅ `crd-geometry-primitives-tests` 103 cases / 64407 assertions on
  win-debug, win-asan, win-shipping; `crd-geometry-bvh-tests` 40/40 on all three
  (the builder asserts didn't move any result — value-preserving retrofit holds).
  `crd-no-std-math-check` + `crd-no-non-ascii-test-names` + `crd-simd-emission-check`
  (run under vcvars for `dumpbin`) + `crd-no-std-sort-check` green. clang-format
  / clang-tidy clean.
- Full 17-config `scripts/full-sweep.ps1` — **deferred** (the `-bvh` sub-module
  closed at v1g and the sweep was due then; per the user it now runs once at the
  end of the `v1` slices, not per slice).

## Next session starts with

- Phase 3.1.7 **v1i** — the unified query facade (ADR-0076 §15): `crd/geometry/
  queries.hpp` (compile-time-overload-polymorphic `raycast` / `overlap` /
  `closest_point` / `contains` / `distance` over `{primitive, BvhTree, Bvh4Tree,
  DynamicBvh}` + the shared `RayHit{t, payload}` / `ClosestPointResult` types);
  **shapecast** (`cast_ray` / `cast_sphere` / `cast_box` vs a primitive and a
  BVH — closed-form TOI for the closed-form shapes); **broadphase pairs**
  (`find_overlapping_pairs(const DynamicBvh&, OutFn)` — dual-descent self-overlap,
  the all-`(i<j)` fat-AABB-overlapping leaf pairs in one traversal); the
  **degenerate-geometry corpus** + **large-coordinate** validation sweep added
  across the whole `crd-geometry` test suite. Then v1j (`crd-geometry-viz`).
  After v1 closes: run the full 17-config sweep.
