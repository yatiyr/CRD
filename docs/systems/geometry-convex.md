# crd-geometry-convex

The convex-shape narrowphase sub-module of `crd-geometry` (ADR-0076 §1, the
third sub-module after `-primitives` and `-bvh`). Distance, overlap,
penetration depth, contact normals, shapecast TOI, and feature enumeration for
convex shapes — the eylem narrowphase substrate (and the substrate any
domain needing precise convex-shape contact reaches for: robotics, medical
visualisation, cinematic asset collision, etc.).

Module: `engine/geometry-convex/`, target `crd-geometry-convex`, namespace
`crd::geometry::convex`. Depends on `crd-core` + `crd-math` + `crd-containers`
+ `crd-geometry-primitives` (uses `Sphere`/`OBB3`/`Capsule3`/`ConvexHullView`
+ `SupportPoint` + the four `support()` ADL overloads from `-primitives` v1h
+ v2a).

## Status — `crd-geometry-convex` v2 ✅ COMPLETE 2026-05-14

| Slice | Scope | State |
|---|---|---|
| v2a | GJK distance kernel + 3 locked substrate decisions (ADR-0076 §4 pin #14): C++20 `ConvexShape` concept + ADL `support()` overloads (Sphere/OBB3/Capsule3/ConvexHullView); local-frame shapes + two `Transform`s (driver works in A's local frame via `T_BA` computed once); `SupportPoint<T>{point, vertex_idx}` with `k_invalid_vertex` sentinel for analytic shapes and lowest-vertex-idx tiebreak for polyhedral. Ericson §9.5 1/2/3/4-simplex sub-distance reductions in `detail/sub_distance.hpp` (NOT van den Bergen's Johnson form). Primary index-match termination (Box2D pattern — epsilon-free, bit-exact cross-platform). | ✅ 2026-05-14 |
| v2b | `gjk_overlap` boolean fast-out + facade. Dedicated boolean driver, no witness reconstruction. `crd::geometry::overlap(convex_a, xa, convex_b, xb) → bool` facade. Touching-boundary convention pinned per-pair-kind. | ✅ 2026-05-14 |
| v2c | EPA penetration depth + contact normal (`epa.hpp` + `detail/epa_polytope.hpp`). Catto 2010 GDC silhouette walk over a fixed-size polytope (64 verts / 128 faces). Starting-simplex completion for GJK simplex sizes 1/2/3/4. Two-component termination `eps_abs + eps_rel * \|F.distance\|` (eps_rel=1e-3 physics-grade). `EpaResult<T>` carries normal (A→B world frame), depth, witnesses, `face_vidx_a/b[3]` for eylem v1d-manifold feature identification. Known limitation: rotated non-cube OBB-OBB pairs can produce a polytope-overshoot in ~5% of trials — SAT preempts (v2d). | ✅ 2026-05-14 |
| v2d | SAT box-pair fast path (`sat.hpp`). Gottschalk 1996 15-axis test for OBB-OBB. `sat_obb_obb` + `sat_aabb_obb` (6-line wrapper). Facade overload preempts GJK for OBB-OBB, bypassing the EPA-OBB-rotated pathology. Edge-edge witnesses via `closest_points(Segment3, Segment3)`. Determinism: A-face → B-face → edge-cross axes; lowest-axis-kind wins on ties within `k_distance_epsilon`. | ✅ 2026-05-14 |
| v2e | `ConvexHullView` queries (`hull_queries.hpp`). `ray_vs_hull` Cyrus-Beck parametric face-plane clipping (lowest-face-index tiebreak); `closest_point(hull, p)` via GJK against `PointShape<T>` (a zero-extent ConvexShape — eylem v1 particle-vs-shape callers will use it); from-inside path projects to nearest face plane. Facade: `raycast(ConvexHullView, Ray3, tmax) → optional<RayHit<u32>>`. | ✅ 2026-05-14 |
| v2f | GJK-based convex shapecast (`shapecast.hpp`). `shapecast_convex<T,A,B>` Newton+bisection hybrid TOI (not pure Newton — pure Newton oscillates at the overlap boundary). EPA-on-overlap-at-start for accurate t=0 normal. Facade: `cast_convex(a, xa, sweep_dir, tmax, b, xb) → optional<T>`. Translational-only (rotational shapecast is eylem v6 CCD). | ✅ 2026-05-14 |
| v2g | Hill-climbing hull support (perf, no API change). Extended `ConvexHullView` with optional vertex adjacency. `hill_climb_support<T>(hull, dir, start_idx)` does the PhysX/Havok best-neighbor walk; `support_with_hint(shape, dir, hint)` dispatches (linear scan fallback). Determinism contract preserved via **iterative walk among tied-projection neighbors + strict `==` equality** (the load-bearing fix — single-step tiebreak diverges from linear-scan's lowest-index). | ✅ 2026-05-14 |
| v2h | `Vec4f`/`Vec8f` SIMD-batched hull support (perf, no API change). Extended `ConvexHullView` with optional SoA vertex layout (`vx_soa`/`vy_soa`/`vz_soa`). `support_simd_f32(hull, dir)` declared in `primitives.hpp`, defined out-of-line in `engine/geometry-primitives/src/hull_support_simd.cpp`. `support_with_hint` dispatch: SoA + SIMD (T==f32, N ≤ `k_simd_support_threshold = 32`) → hill-climb (adjacency + hint) → linear scan. Padding contract pinned: SoA padded to multiple-of-8 with vertex 0's coords (branch-free, ties on lowest index). | ✅ 2026-05-14 |
| v2i | f64 instantiation pin + aerospace orbital corpus. Static asserts in `convex.hpp` pin `ConvexShape<S, f64>` conformance for all 5 shape types; aerospace test corpus (`test_f64_orbital.cpp`) at LEO altitude (~7×10⁶ m) where f32 has lost sub-meter precision. The SoA SIMD path stays f32-only by construction (`if constexpr (T == f32)` guard in `support_with_hint`). | ✅ 2026-05-14 |
| v2j | Sutherland-Hodgman convex polygon clipping + feature enumeration (`feature_clip.hpp`). The eylem v1d-manifold gate. `is_smooth(Shape)` predicate; `enumerate_faces(OBB3)` (host-frame `ObbFaceFeature`), `enumerate_faces(ConvexHullView, Array&)` (`HullFaceFeature` with ConstSpan into hull data); `enumerate_edges_obb()` (static 12-edge table) + `enumerate_edges(ConvexHullView, Array&)` (face-pair matching); `enumerate_spine(Capsule3) → Segment3`; `closest_face_index(Shape, dir)` (lowest-face-index tiebreak); `clip_convex_polygon(input, plane, output)` + `clip_against_convex_volume(input, planes, output, scratch)` (caller-supplied ping-pong buffers, no hidden alloc). Sutherland-Hodgman lerp form pinned: `t = sd_i / (sd_i - sd_{i+1})`, `out = v_i + t * (v_{i+1} - v_i)` — test-locked for seam bit-equality across plane orderings. | ✅ 2026-05-14 |
| v2-close | Tiebreak conformance sweep (`test_tiebreak_conformance.cpp`, 9 cases / 103 assertions — every ADR-0076 §4 pin #14 rule + the v2j carryovers) + degenerate corpus (`test_degenerate_corpus.cpp`, 17 cases / 37 assertions — zero-radius/zero-extent primitives, 1/2/3/4-vertex hulls, coplanar hulls, identical-pose pairs, tangent contacts, far-origin 1e6+/1e7 inputs, near-zero separations) + v2j throughput bench. Full 17-config sweep PASS. | ✅ 2026-05-14 |

## API surface

### Concept and shape-support layer (`-primitives` v1h + v2a)

The substrate's fundamental abstraction is the C++20 `ConvexShape<S, T>`
concept (`crd/geometry/convex/support.hpp`). A type `S` participates if a
free function `support(const S&, const Vec3<T>&) → SupportPoint<T>` is
findable via ADL. The four shipped overloads (in `-primitives`):

- `support(Sphere<T>, dir)` — `dir × radius + center`; `vertex_idx = k_invalid_vertex`.
- `support(OBB3<T>, dir)` — sign-extraction along the 3 orientation columns; `vertex_idx` ∈ [0, 8) packs the 3 sign bits.
- `support(Capsule3<T>, dir)` — better-of-endpoints + `dir × radius`; `vertex_idx = k_invalid_vertex` (smooth radial).
- `support(ConvexHullView<T>, dir)` — linear scan over hull vertices with lowest-index tiebreak; routes through `support_with_hint` if adjacency or SoA is present.

`SupportPoint<T> { Vec3<T> point; crd::u32 vertex_idx; }`. The
`vertex_idx` carries the polyhedral-extreme identity that drives GJK's
index-match termination and EPA's polytope vertex de-duplication.

`PointShape<T> { Vec3<T> p; }` — a zero-extent convex shape; any
direction's support is `p`. eylem v1 particle-vs-shape callers and
v2e closest-point use it.

### Distance / overlap / penetration / contact

```cpp
// v2a — distance, witnesses, simplex
template <MathScalar T, ConvexShape<T> A, ConvexShape<T> B>
GjkResult<T> gjk_distance(const A& a, const Transform<T>& xa,
                          const B& b, const Transform<T>& xb,
                          GjkConfig<T> = {}) noexcept;

// v2b — boolean fast path
template <MathScalar T, ConvexShape<T> A, ConvexShape<T> B>
bool gjk_overlap(const A& a, const Transform<T>& xa,
                 const B& b, const Transform<T>& xb,
                 GjkConfig<T> = {}) noexcept;

// v2c — penetration depth + contact normal (call only if gjk_distance.overlapping)
template <MathScalar T, ConvexShape<T> A, ConvexShape<T> B>
EpaResult<T> epa_penetration(const A& a, const Transform<T>& xa,
                             const B& b, const Transform<T>& xb,
                             const GjkSimplex<T>& starting_simplex,
                             EpaConfig<T> = {}) noexcept;

// v2c — distance + penetration in one call
template <MathScalar T, ConvexShape<T> A, ConvexShape<T> B>
std::optional<EpaResult<T>> compute_contact(...) noexcept;

// v2d — SAT box-pair fast path (preempts GJK for OBB-OBB via facade)
template <MathScalar T>
SatResult<T> sat_obb_obb(const OBB3<T>& a, const Transform<T>& xa,
                         const OBB3<T>& b, const Transform<T>& xb) noexcept;

// Facade — picks SAT / GJK / etc. by shape pair
template <ConvexShape A, ConvexShape B>
bool overlap(const A&, const Transform&, const B&, const Transform&) noexcept;
```

### Hull queries (v2e)

```cpp
std::optional<RayHit<u32>> ray_vs_hull(ConvexHullView<T>, Ray3<T>,
                                       T tmax = ∞) noexcept;
Vec3<T> closest_point(ConvexHullView<T>, Vec3<T> p) noexcept;

// Facade
std::optional<RayHit<u32>> raycast(ConvexHullView<T>, Ray3<T>, T tmax) noexcept;
```

### Shapecast (v2f)

```cpp
template <MathScalar T, ConvexShape A, ConvexShape B>
std::optional<ConvexShapecastResult<T>> shapecast_convex(
    const A&, const Transform<T>& xa, const Vec3<T>& sweep_dir, T tmax,
    const B&, const Transform<T>& xb) noexcept;

// Facade
template <ConvexShape A, ConvexShape B>
std::optional<T> cast_convex(const A&, Transform xa, Vec3 dir, T tmax,
                             const B&, Transform xb) noexcept;
```

### Feature enumeration + polygon clipping (v2j)

```cpp
// "Should I face-clip?" — Sphere/Capsule = true, OBB/Hull = false.
template <MathScalar T> bool is_smooth(const Shape<T>&) noexcept;

// OBB host-frame face/edge enumeration.
StaticArray<ObbFaceFeature<T>, 6> enumerate_faces(const OBB3<T>&) noexcept;
StaticArray<EdgeFeature, 12> enumerate_edges_obb() noexcept;
Segment3<T> enumerate_spine(const Capsule3<T>&) noexcept;

// Hull face/edge enumeration (vertex_indices is a span into hull.face_vertex_indices).
void enumerate_faces(const ConvexHullView<T>&, Array<HullFaceFeature<T>>&) noexcept;
void enumerate_edges(const ConvexHullView<T>&, Array<EdgeFeature>&) noexcept;

// Face-pick — lowest face_index tiebreak within k_parallel_epsilon.
u8 closest_face_index(const OBB3<T>&, const Vec3<T>& dir_local) noexcept;
u32 closest_face_index(const ConvexHullView<T>&, const Vec3<T>& dir) noexcept;

// Sutherland-Hodgman convex polygon clipping.
void clip_convex_polygon(ConstSpan<Vec3<T>> input,
                         const Plane<T>& clipping_plane,
                         Array<Vec3<T>>& output) noexcept;
void clip_against_convex_volume(ConstSpan<Vec3<T>> input,
                                ConstSpan<Plane<T>> planes,
                                Array<Vec3<T>>& output,
                                Array<Vec3<T>>& scratch) noexcept;
```

## Substrate decisions (ADR-0076 §4 pin #14)

These are locked across every v2 entry point:

1. **ConvexShape concept + ADL `support()` overloads** — compile-time
   overload polymorphism, no virtual dispatch, no registry. New shape
   types (eylem `Collider::ConvexHull`, future user-cooked hulls)
   participate by writing one free function in their own namespace.

2. **Local-frame shapes + two transforms** — GJK works in shape A's local
   frame (`T_BA = inv(xform_a) * xform_b` computed once). Support
   functions take direction *already in the shape's local frame* and are
   pure axis-aligned math (~6–10 ops each). ~40% faster on OBB pairs than
   world-frame driving.

3. **`SupportPoint{point, vertex_idx}` with lowest-index tiebreak** —
   enables Box2D-style **primary index-match termination** (epsilon-free,
   bit-exact cross-platform). Same `vertex_idx` two iterations in a row ⇒
   converged. EPA polytope vertex de-duplication is O(1) integer-compare.

4. **Sutherland-Hodgman lerp form pinned** (v2j) — `t = sd_i / (sd_i -
   sd_{i+1})`, `out = v_i + t * (v_{i+1} - v_i)`. NOT `(1-t)·a + t·b`.
   Different rounding breaks seam-vertex bit-equality across adjacent
   clipping planes; locked by a test (`test_feature_clip.cpp:clip seam
   vertex bit-equal across plane orderings`).

5. **Capsule spine is a `Segment3`**, not an `EdgeFeature` (v2j) — face_a
   / face_b indices are meaningless for a capsule. Cleaner than
   uniform-with-sentinels.

6. **SAT preempts GJK for OBB-OBB via facade overload** (v2d) — both
   `overlap(OBB,OBB)` and `compute_contact_obb_obb` route through SAT.
   Production callers never hit the EPA-OBB-rotated pathology.

## Determinism contract

Every v2 entry point is bit-exact on identical inputs across platforms
(ADR-0063). The substrate-level rules:

- GJK: lowest-vertex_idx wins on coincident hull extrema; index-match
  termination is primary (geometric-eps fallback for analytic shapes).
- EPA: lowest-face-index on coincident face distances; replay-equal.
- SAT: A-face (0..2) → B-face (3..5) → edge-cross (6..14) traversal;
  lowest-axis-kind wins within `k_distance_epsilon`.
- `ray_vs_hull`: lowest-face-index on coincident `t_enter`.
- Hill-climb: iterative walk among tied-projection neighbors + strict
  `==` equality (single-step tiebreak diverges from linear-scan).
- SIMD support: strict `>` reducer + lowest-index tiebreak (matches
  linear scan by construction).
- Sutherland-Hodgman: locked lerp form (above).
- `closest_face_index`: lowest face_index within `k_parallel_epsilon`.

`test_tiebreak_conformance.cpp` exercises every rule with inputs designed
to force the tie.

## Known limitations and follow-ups

- **EPA OBB-OBB rotated pathology** (v2c, documented + contained): heavily
  rotated non-cube OBB-OBB pairs (different half-extents per axis + 45°+
  rotations) can produce a polytope where the closing-face approximation
  reports a too-large depth (~5% of trials). SAT preempts via facade
  (v2d). If a future hull cooker produces shapes EPA can't close on,
  route through SAT or revisit the polytope-overflow path.
- **Rotational shapecast** — translational-only in v2f. eylem v6 CCD
  ships full TOI (rotational and translational) for two moving convexes.
- **`closest_face_index(OBB3)` host-frame convention** — direction is in
  the same frame as `orientation`'s columns (world frame for OBBs whose
  `orientation` maps local→world). Callers needing local-frame input
  rotate via `inverse(xform.rotation) * world_dir` first.

## Test summary (v2-close, 2026-05-14)

- 11 test files / 146 cases / 20624 assertions in `crd-geometry-convex-tests`.
- Tiebreak conformance: 9 cases / 103 assertions.
- Degenerate corpus: 17 cases / 37 assertions.
- f64 aerospace orbital corpus: 9 cases.
- Feature enumeration + clipping: 23 cases / 313 assertions.
- Full 17-config `scripts/full-sweep.ps1` PASS.
