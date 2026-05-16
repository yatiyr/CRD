# crd-geometry-mesh — system overview

> Phase 3.1.7 v4 cluster. Indexed-triangle-mesh queries built on the
> `crd-geometry-bvh` substrate. Five public queries — closest-point,
> raycast (watertight Woop), raycast (SIMD Möller-Trumbore), generalised
> winding number, and formal validation. Typed-Quantity wrapper layer at
> the API surface per ADR-0078 §5.

## Status

- v4a `mesh_closest_point` ✅ shipped 2026-05-16
- v4b `mesh_raycast` (Woop watertight) ✅ shipped 2026-05-16
- v4c `mesh_winding_number` (Jacobson 2013) ✅ shipped 2026-05-16
- v4d `mesh_raycast_simd` (AVX2 Möller-Trumbore) ✅ shipped 2026-05-16
- v4-validate `validate_triangle_mesh` ✅ shipped 2026-05-16
- v4-close ✅ shipped 2026-05-16 — this doc + ADR-0076 §19 amendment + 18-config sweep.

**Cluster totals:** 5 slices · 39 cases / 503 assertions · `engine/geometry-mesh/`
module (1 umbrella + 6 logical headers + 5 .cpp files) + typed-wrapper
layer for closest_point + raycast + winding.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Upper layer (typed) — mesh_queries_typed.hpp                    │
│  TriangleMeshViewT<D, T>, Ray3T<D, T>, MeshRayHitT<D, T>,        │
│  MeshClosestPointT<D, T>                                         │
│  Strip-compute-retag wrappers — bridges to raw f32 algorithm.    │
└──────────────────────────────────────────────────────────────────┘
                            │  ADR-0078 §5 boundary
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│  Lower layer (raw f32) — algorithm bodies                        │
│  triangle_mesh.hpp     TriangleMeshView<T>     non-owning data   │
│  mesh_bvh.hpp          TriangleMeshBvh         per-tri AABB BVH  │
│  mesh_closest_point.hpp                        Ericson + BVH B&B │
│  mesh_raycast.hpp                              Woop watertight   │
│  mesh_raycast_simd.hpp                         AVX2 MT 8-wide    │
│  mesh_winding_number.hpp                       Jacobson direct   │
│  mesh_validate.hpp                             6-defect report   │
└──────────────────────────────────────────────────────────────────┘
                            │
                            ▼
        crd-geometry-bvh (BVH traversal + slab tests)
        crd-geometry-primitives (Ericson cascade, Woop, Williams/Ize)
        crd-math::simd (Vec8f for SIMD MT)
```

## API at a glance

```cpp
#include <crd/geometry/mesh/mesh.hpp>

// 1. Build once.
TriangleMeshView<f32> view{vertices, indices};
TriangleMeshBvh        bvh = build_triangle_mesh_bvh(view, alloc);

// 2. Query many times.
auto cp = mesh_closest_point(view, bvh, query);
auto hit = mesh_raycast(view, bvh, ray);                   // Woop, watertight
auto hit_fast = mesh_raycast_simd(view, bvh, ray);         // MT, 8-wide AVX2
auto w = mesh_winding_number(view, query);                 // O(N) Jacobson
bool inside = mesh_is_inside(view, query);                 // threshold-0.5 wrapper

// 3. Validate.
MeshValidationOptions opts{};
opts.area_epsilon = Area32{1e-12F};
auto report = validate_triangle_mesh(view, alloc, opts);
if (!report.well_formed) { /* reject — see report.defects */ }
```

## Two-layer typing (ADR-0078 §5)

Raw algorithms live in `mesh_*.hpp` / `.cpp`. Typed-Quantity surface
lives in `mesh_queries_typed.hpp` — strip-compute-retag wrappers that
bridge `Vec3<Length32>` callers to the raw `f32` algorithms:

```cpp
TriangleMeshViewT<dim::Length, f32> typed_view{...};
Vec3<Length32> typed_query{Length32{5.0F}, ...};
auto r = mesh_closest_point(typed_view, raw_vertex_span, bvh, typed_query);
// r.point: Vec3<Length32>, r.distance_squared: Area32 (DimMul<Length, Length>)
```

Zero runtime overhead — `to_raw_vec` / `from_raw_vec` are constexpr;
`.value` accessors compile away.

Coverage:
- `closest_point` — typed in `Vec3<Length32>`, typed out `(Vec3<Length32>, Area32)`.
- `raycast` (Woop) — typed `Ray3T<D, T>` in, typed `MeshRayHitT<D, T>{t: Length32, ...}` out.
- `winding_number` — typed query point in; return is dimensionless `f32` (winding = rotations / 4π is dimensionless).
- `mesh_is_inside` — same as winding, returns `bool`.
- `mesh_raycast_simd` — currently no typed wrapper; use the Woop typed wrapper for typed callers (drop-in shape).
- `validate_triangle_mesh` — `area_epsilon: Area32` typed at the options surface; the rest of the report is raw indices + booleans.

## Query 1 — `mesh_closest_point` (v4a)

Returns the point on the mesh nearest to `query`, the squared distance,
and the winning triangle index.

```cpp
struct MeshClosestPoint /* = ClosestPointResult<u32> */ {
    Vec3<f32> point;
    f32       distance_squared;
    u32       payload;  // triangle index
};

std::optional<MeshClosestPoint>
mesh_closest_point(view, bvh, query, f32 max_dist = inf) noexcept;
```

**Algorithm.** Branch-and-bound BVH walk. Per node, lower-bound =
`distance_squared(query, node.bounds)` via clamp-to-bounds. Descend
nearer child first so `best` tightens before far subtree is reached. At
leaves: Ericson §5.1.5 Voronoi-region cascade per triangle. Lowest
triangle-index wins on equal squared distance.

**Verified against** brute-force per-triangle scan across a 125-query
grid in [-2, 2]³ on a unit cube — bit-exact within 1e-5.

## Query 2 — `mesh_raycast` (v4b, Woop watertight)

Nearest-hit raycast. Hole-free, NaN-safe.

```cpp
struct MeshHitPayload {
    u32       tri;
    Vec3<f32> bary;  // Woop barycentric (1-u-v, u, v)
};
using MeshRayHit = RayHit<MeshHitPayload>;  // {t, payload}

std::optional<MeshRayHit>
mesh_raycast(view, bvh, ray, f32 tmax = inf, bool cull_back = false) noexcept;
```

**Algorithm.** Precompute Williams/Ize slab + Woop shear once per call.
Walk BVH: interior nodes use the slab test against current `best_t` for
ordered nearer-first descent + far-subtree pruning. Leaves: precomputed-
shear Woop watertight per triangle (`intersect_ray_triangle_watertight`).
Lowest-index tiebreak on equal `t`.

**Watertight contract.** Rays passing exactly through a shared edge or
vertex hit BOTH adjacent triangles with sign-consistent results; the
deterministic tiebreak picks the winner. No leaks.

## Query 3 — `mesh_winding_number` (v4c, Jacobson 2013)

Generalised winding number for robust inside/outside on non-watertight
meshes.

```cpp
f32  mesh_winding_number(view, query) noexcept;
bool mesh_is_inside(view, query, f32 threshold = 0.5F) noexcept;
```

**Algorithm.** Direct O(N) sum of Van Oosterom-Strackee 1983 per-triangle
solid angles, scaled by `1/(4π)`:

```
Ω(p, tri) = 2 · atan2(a·(b×c),
                       |a||b||c| + (a·b)|c| + (b·c)|a| + (c·a)|b|)
w(p)      = (1/4π) · Σ Ω(p, tri)
```

Closed watertight manifolds → `w ∈ {0, 1}` exactly. Non-watertight
meshes → continuous real rounding to topological inside/outside at 0.5.
**Robust to T-junctions, edge cracks, and self-intersections** — the
canonical inside/outside test for imperfect input.

**Verified on** an open-cube fixture (+X face removed): interior point
returns `w ≈ 5/6 = 0.833`, above the 0.5 threshold → classified inside
per Jacobson's robustness claim.

**Determinism caveat.** `atan2` / `sqrt` are not bit-exact across libm
implementations. The 0.5 inside/outside threshold has comfortable
margin from the `{0, 1}` attractors; the boolean answer is robust to
1-2 ULP drift per contribution. Kahan summation reserved for
v4c-precision if a real corpus shows drift at the threshold (unlikely).

**Open follow-on:** v4c-fast — Jacobson 2013 §4 hierarchical treecode
(per-BVH-node dipole moments + adaptive descent) for O(log N) average
queries. Deferred until consumer surfaces.

## Query 4 — `mesh_raycast_simd` (v4d, AVX2 Möller-Trumbore)

Alternative raycast entry point using 8-wide batched Möller-Trumbore at
BVH leaves. Same `MeshRayHit` return shape as v4b — drop-in swap at call
sites that want the SIMD fast path.

```cpp
std::optional<MeshRayHit>
mesh_raycast_simd(view, bvh, ray, f32 tmax = inf, bool cull_back = false) noexcept;
```

**Algorithm.** Same BVH traversal as v4b (Williams/Ize slab + ordered
descent). Differs at the leaf inner loop: chunk leaf's triangles into
groups of 8, gather AoS `Vec3<f32>` vertex positions, transpose into
9 × `Vec8f` SoA registers, run SIMD MT (~30 FMAs), then scalar lane scan
applies masking decisions (cull_back / `|det| > ε` / `0≤u≤1` / `0≤v` /
`u+v≤1` / `0≤t≤best_t`).

**MT vs Woop divergence.** MT uses strict-sign det; Woop is watertight.
On tessellated curved surfaces, the two can pick different triangles at
near-edge rays. Cross-validation against v4b on a 36-ray sphere corpus
tolerates ≤6 divergent triangle-picks. Both hits are legitimate surface
points — choose the algorithm by precision-vs-speed need.

**When to use which:**

| Scenario | Recommended |
|---|---|
| CSG / robust booleans / winding-via-rays | **v4b Woop** — watertight contract load-bearing |
| Real-time pickers, navmesh height queries, broadphase culling | **v4d SIMD** — 4–8× faster on dense leaves |
| Default (no specific need) | **v4b Woop** — same speed for small leaves, stronger contract |

**Lesson recorded.** First-draft SIMD-mask AND via `min(mask_gt, mask_lt)`
silently lost the cull bit — `_mm256_min_ps` on NaN-encoded `_CMP_*`
results is implementation-defined. Fix: keep heavy ALU in SIMD, run
masking decisions in scalar lane scan.

## Query 5 — `validate_triangle_mesh` (v4-validate)

Formal mesh validation. Reports six defect classes.

```cpp
enum class MeshDefectKind : u8 {
    OutOfBoundsIndex,        // critical
    DegenerateTriangle,      // critical
    ZeroAreaTriangle,        // smell
    NonManifoldEdge,         // critical
    BoundaryEdge,            // informational
    InconsistentOrientation, // critical
};

struct MeshValidationReport {
    Array<MeshDefect> defects;
    u32  triangle_count, vertex_count;
    u32  manifold/boundary/non_manifold_edge_count;
    bool well_formed;  // no critical defects
    bool watertight;   // well_formed + 0 boundary edges + triangle_count > 0
};

MeshValidationReport
validate_triangle_mesh(view, alloc, opts = {});
```

**Three-pass deterministic algorithm.**

1. **Triangle-level scan.** Out-of-bounds index check, degenerate (i==i)
   check, area-zero check via `|edge1 × edge2|² < (2·ε)²` (squared
   threshold, no `sqrt`).
2. **Edge map build.** Per non-defective triangle, emit 3 `EdgeRec`
   entries with canonical key `(min(va, vb), max(va, vb))` and original
   winding direction. `std::sort` by canonical key.
3. **Edge classification.** Walk sorted edge list. Runs of identical
   `(v_lo, v_hi)` classify as boundary (count=1), manifold (count=2,
   with orientation check), or non-manifold (count≥3).

**Use cases:**
- **Cooker pipeline gate.** Reject `!well_formed` meshes from cooked
  artifacts; surface defects in the cooker log with `MeshDefect.a/b`
  pointers.
- **Editor mesh-import diagnostics.** Per-defect surface to the artist
  with file:line context.
- **Optional runtime gate.** Debug-only check before handing a
  scanned / network-received mesh to `eylem::TriangleMeshCollider`.

## File layout

```
engine/geometry-mesh/
  CMakeLists.txt
  include/crd/geometry/mesh/
    mesh.hpp                       Umbrella include
    triangle_mesh.hpp              TriangleMeshView<T> (data only)
    mesh_bvh.hpp                   TriangleMeshBvh + build_triangle_mesh_bvh
    mesh_closest_point.hpp         v4a
    mesh_raycast.hpp               v4b — Woop watertight
    mesh_raycast_simd.hpp          v4d — AVX2 MT
    mesh_winding_number.hpp        v4c — Jacobson
    mesh_validate.hpp              v4-validate — 6 defect kinds
    mesh_queries_typed.hpp         ADR-0078 §5 typed wrappers
  src/
    mesh_bvh.cpp
    mesh_closest_point.cpp
    mesh_raycast.cpp
    mesh_raycast_simd.cpp
    mesh_winding_number.cpp
    mesh_validate.cpp

tests/geometry-mesh/
  CMakeLists.txt
  test_mesh_closest_point.cpp      6 cases / 145 assertions
  test_mesh_raycast.cpp            8 cases /  16 assertions
  test_mesh_raycast_simd.cpp       7 cases /  15 assertions
  test_mesh_winding_number.cpp     8 cases / 281 assertions
  test_mesh_validate.cpp          10 cases /  46 assertions
                          Total:  39 cases / 503 assertions
```

## Architectural pins (carried from ADR-0076 §19)

1. **Watertight reference, SIMD fast path.** v4b (Woop) is canonical;
   v4d (SIMD MT) is the fast path. Two algorithms permanently — pretending
   one fits both contracts dead-ends at the SIMD-vs-watertight tradeoff.
2. **Non-owning view + separate BVH.** `TriangleMeshView<T>` stays
   trivially-copyable; `TriangleMeshBvh` is built once and reused. Same
   pattern as `crd-geometry-bvh` v1a.
3. **`MeshHitPayload` carries `(tri, bary)`.** Barycentrics save callers
   from re-running interpolation.
4. **O(N) winding at v4c-base.** Hierarchical Jacobson §4 treecode
   deferred to v4c-fast until consumer surfaces.
5. **`atan2`/`sqrt` libm drift accepted for winding.** Threshold margin
   absorbs it; the boolean answer is robust.
6. **`area_epsilon: Area32` typed at the boundary** per ADR-0078 §5 D34.
7. **Sorted-edge classification** for v4-validate — deterministic over
   hashmap.
8. **Zero-area triangles do NOT fail `well_formed`.** Authoring smell,
   not critical.
9. **Watertight requires `triangle_count > 0`.** Empty mesh is
   well-formed but not a closed surface.

## Open follow-ups

- **v4c-fast — Jacobson hierarchical treecode.** Per-BVH-node dipole
  moments + adaptive descent → O(log N) average winding query. Defer
  until eylem volumetric inside-checks at scale or editor "fill" tool
  consumer surfaces.
- **v4d-fast — per-leaf SoA repack at BVH build time.** Save gather +
  transpose cost per query. Defer until perf benchmarks justify
  (v4-bench, post-Phase 3.1.7 close).
- **Sandbox MeshQueries scene.** Phase-segment viz deliverable (per the
  2026-05-16 viz discipline). Drag-a-point UI that visualizes all 5
  queries on a picked mesh. Lands as a standalone session before Phase
  3.1.7 close.

## References

- ADR-0076 §19 — v4 cluster CLOSED amendment (locked decisions).
- ADR-0078 §5 D32-D36 — two-layer typed architecture.
- Christer Ericson, *Real-Time Collision Detection* §5.1.5 — closest-point Voronoi cascade.
- Sven Woop, "Watertight Ray/Triangle Intersection" (JCGT 2013).
- Williams et al., "An Efficient and Robust Ray-Box Intersection Algorithm" (JGT 2005) + Ize 2013.
- Alec Jacobson, Ladislav Kavan, Olga Sorkine-Hornung, "Robust inside-outside segmentation using generalized winding numbers" (ACM TOG / SIGGRAPH 2013).
- A. Van Oosterom, J. Strackee, "The Solid Angle of a Plane Triangle" (IEEE TBME 1983).
- Tomas Möller, Ben Trumbore, "Fast, Minimum Storage Ray-Triangle Intersection" (JGT 1997).
- Sessions: `2026-05-16-geometry-v4a-mesh-closest-point.md`, `-v4b-mesh-raycast.md`, `-v4c-mesh-winding-number.md`, `-v4d-mesh-raycast-simd.md`, `-v4-validate.md`.
