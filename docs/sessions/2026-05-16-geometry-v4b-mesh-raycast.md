# Session log — 2026-05-16 — geometry v4b: `mesh_raycast`

> Second slice of Phase 3.1.7 v4 `-mesh` cluster. Nearest-hit raycast against
> an indexed triangle mesh via Woop 2013 watertight ray-tri at BVH leaves +
> Williams/Ize precomputed slab traversal + ordered nearer-first descent.
> Typed `Ray3T<Length32>` wrapper alongside the raw `<f32>` algorithm per
> ADR-0078 §5.

## Scope landed

| Element | Path |
|---|---|
| Raycast header | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_raycast.hpp` |
| Raycast impl   | `engine/geometry-mesh/src/mesh_raycast.cpp` |
| Typed wrappers (`Ray3T`, `MeshRayHitT`, typed `mesh_raycast`) | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_queries_typed.hpp` (extended) |
| Umbrella header | `engine/geometry-mesh/include/crd/geometry/mesh/mesh.hpp` (added include) |
| Tests | `tests/geometry-mesh/test_mesh_raycast.cpp` + CMakeLists |

## API surface

```cpp
struct MeshHitPayload {
    u32                tri;       // triangle index
    Vec3<f32>          bary;      // Woop barycentric (1-u-v, u, v)
};
using MeshRayHit = RayHit<MeshHitPayload>;  // {t, payload} per ADR-0076 §16 pin #2

[[nodiscard]] std::optional<MeshRayHit>
mesh_raycast(const TriangleMeshViewf&, const TriangleMeshBvh&,
             const Ray3<f32>& ray,
             f32 tmax = inf,
             bool cull_back = false) noexcept;
```

Typed Quantity-overload twin in `mesh_queries_typed.hpp` (per ADR-0078 §5
two-layer rule):

```cpp
template <typename D, typename T> struct Ray3T {
    Vec3<Quantity<D, T>>  origin;     // typed length
    Vec3<T>               direction;  // dimensionless unit vector
};

template <typename D, typename T> struct MeshRayHitT {
    Quantity<D, T>   t;          // typed length along ray
    MeshHitPayload   payload;    // raw — tri + bary are dimensionless
};

template <typename D, typename T>
std::optional<MeshRayHitT<D, T>>
mesh_raycast(TriangleMeshViewT<D, T>, ConstSpan<Vec3<T>> raw_vertices,
             const TriangleMeshBvh&, Ray3T<D, T>,
             Quantity<D, T> tmax = ...,
             bool cull_back = false) noexcept;
```

## Algorithm

Per-ray precomputes amortise the per-leaf work:

1. `RayAABBPrecompute = precompute_ray_aabb(ray)` — Williams/Ize sign array
   + reciprocals; powers the branchless slab test.
2. `RayTriShear = precompute_ray_tri(ray)` — Woop's longest-axis
   permutation + shear coefficients.

Traversal (manual stack of `(node_idx, enter_t)`, bounded by `k_max_bvh_depth`):

- At interior: slab-test both children with current `best_t` as the upper
  bound. Push children **far-first** so the **nearer is popped first** →
  `best_t` tightens before the far subtree gets visited.
- At leaf: for each indexed triangle, run
  `intersect_ray_triangle_watertight(ray, tri_pc, tri, t, bary, cull_back, 0.0F)`.
  Update best on `t < best_t`; on `t == best_t`, lowest triangle index
  wins (determinism tiebreak per ADR-0076 §4 pin #11).
- Skip pushing children whose slab test fails OR whose entry-t already
  exceeds `best_t`.

The Woop ray-tri test is watertight: rays passing exactly through a
shared edge / vertex hit BOTH adjacent triangles with sign-consistent
results, and the lowest-triangle-index tiebreak picks the winner
deterministically. No leaks.

## Test corpus

8 cases / 16 assertions in `tests/geometry-mesh/test_mesh_raycast.cpp`:

1. **Empty mesh** — `nullopt`.
2. **Single triangle** — ray straight down from (0.25, 0.25, 1) hits
   xy-plane triangle at t=1; tri index 0.
3. **Unit cube + X-face hit** — ray from (5, 0, 0) heading -X hits face
   at x=0.5 with t = 4.5.
4. **Miss** — ray parallel at y=5 above the cube returns `nullopt`.
5. **`tmax` cull** — `tmax=4.0` misses the 4.5 hit; `tmax=5.0` returns it.
6. **`cull_back`** — ray FROM INSIDE the cube going -X: with backfaces
   counted, hits the back-side of -X face at t=0.5; with `cull_back=true`,
   skipped → `nullopt`.
7. **Barycentric sum** — `bary.x + bary.y + bary.z ≈ 1` on a hit.
8. **Typed Quantity wrapper** — `Vec3<Length32>` origin via `Ray3T`
   returns `MeshRayHitT<dim::Length, f32>{t: Length32{4.5F}, ...}`.

## Decisions locked

- **Woop watertight, not Möller-Trumbore.** Original plan said
  Möller-Trumbore; switched to Woop after probe of
  `crd-geometry-primitives::watertight_ray_tri.hpp`. Reason: the
  watertight contract eliminates the edge-leak failure mode and the
  on-leaf cost is comparable. Determinism contract is stronger (exact-
  zero edge predicates promote to `double`; bit-exact across f32/f64
  rays).
- **Far-first stack push (nearer popped first).** Cuts redundant work
  by tightening `best_t` before the far subtree. Symmetric on
  `ltl <= ltr` (`≤` not `<`) for deterministic order on equal entry-t.
- **`MeshHitPayload` carries `(tri, bary)` not just `tri`.** Barycentric
  is the natural by-product of the Woop test — exposing it saves
  callers from re-running interpolation. Hit point = `bary.x * v0 +
  bary.y * v1 + bary.z * v2` (or `origin + t * direction` — same point,
  one less mul-add).
- **`cull_back=false` default.** Inside-of-mesh rays and double-sided
  geometry should hit on first contact. Closed-surface raycasts opt-in
  to `cull_back=true`.

## 5-config DoD

| Config | Build | CTest |
|---|---|---|
| win-debug | clean | **1927/1927** (+8 from v4b) |
| win-asan | clean | 1927/1927 |
| win-shipping | clean | 1840/1840 |
| win-shipping-profile | clean | 1922/1922 |
| win-tidy | clean | — |

Full project ctest 1919 (v4a close) → **1927** after v4b.

## Open follow-ups inside v4

- **v4c Jacobson 2013 generalised winding number** — robust inside/outside on
  non-watertight meshes. Builds on neither closest-point nor raycast; new
  per-query summation over triangles. The "is point inside the mesh?"
  question that v4a/b can't answer directly.
- **v4d per-leaf SIMD Möller-Trumbore** — `Vec8f` over 8 triangles per leaf.
  Would replace the inner loop in `mesh_raycast` with a transposed-SoA
  batched test. Requires SoA leaf storage in `TriangleMeshBvh` (probably
  a v4d-internal repack rather than a TriangleMeshBvh layout change, to
  keep v4a/b unchanged).
- **v4-validate** — formal mesh validation pipeline stage.
- **v4-close** — ADR-0076 §17 amendment + `docs/systems/geometry-mesh.md`
  + 18-config full sweep + ONE sandbox-viz session demonstrating all four
  queries together (per the 2026-05-16 viz-discipline call — phase-segment
  deliverable, not per-slice tax).

## References

- ADR-0076 §16 pin #2 — `RayHit{t, payload}` field order.
- ADR-0076 §4 pin #11 — determinism tiebreak rules (lowest tri index wins).
- ADR-0078 §5 D27/D32-D36 — two-layer typed architecture; strip-compute-retag wrapper pattern carries forward from v4a.
- Sven Woop, "Watertight Ray/Triangle Intersection" (JCGT 2013) — the on-leaf predicate.
- Williams et al., "An Efficient and Robust Ray-Box Intersection Algorithm" (JGT 2005) + Ize 2013 robust precompute — the slab traversal.
- `engine/geometry-primitives/include/crd/geometry/primitives/watertight_ray_tri.hpp` (v0f) — the Woop test we consume.
- `engine/geometry-primitives/include/crd/geometry/primitives/robust_ray_aabb.hpp` (v0f) — the slab precompute we consume.
- Preceding session log: `docs/sessions/2026-05-16-geometry-v4a-mesh-closest-point.md`.
