# Session log — 2026-05-16 — geometry v4a: `mesh_closest_point`

> First slice of Phase 3.1.7 v4 `-mesh` cluster. New `engine/geometry-mesh/`
> module shipping `TriangleMeshView` + per-mesh BVH + `mesh_closest_point`
> via Ericson cascade + BVH branch-and-bound + typed-Quantity wrapper layer
> per ADR-0078 §5 two-layer rule.

## Scope landed

| Element | Path |
|---|---|
| Module skeleton | `engine/geometry-mesh/CMakeLists.txt` |
| Umbrella header | `engine/geometry-mesh/include/crd/geometry/mesh/mesh.hpp` |
| Triangle-mesh view (non-owning) | `engine/geometry-mesh/include/crd/geometry/mesh/triangle_mesh.hpp` |
| Per-mesh BVH (storage + builder) | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_bvh.hpp` + `src/mesh_bvh.cpp` |
| Closest-point query (raw `<MathScalar T>`) | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_closest_point.hpp` + `src/mesh_closest_point.cpp` |
| Typed Quantity-overload wrappers | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_queries_typed.hpp` |
| Tests | `tests/geometry-mesh/test_mesh_closest_point.cpp` + `CMakeLists.txt` |
| Module wired into root build | `CMakeLists.txt` line 403 |
| Test target wired in | `tests/CMakeLists.txt` |

## Scope-discovery moment

Session opened intending to ship v3d hull simplification. Probe showed
`engine/geometry-convex/src/hull_simplify.{hpp,cpp}` (744 LOC) +
`tests/geometry-convex/test_hull_simplify.cpp` (528 LOC, 16 cases /
191 assertions) + `tests/geometry-convex/test_v3_close.cpp` (540 LOC,
9 cases / 243 assertions) all green. Git log confirms — commit
`109c870` "v3 is closed" predates the entire Phase 3.1.7.5 `crd-units`
work (2026-05-14).

`project_state` memory was stale on this point — corrected mid-session
to reflect: v3 (Shewchuk + 2D hull + Quickhull + hull simplification +
v3-close) shipped 2026-05-14; the actual next is **v4 `-mesh` cluster**.

## `crd-geometry-mesh` v4a design

### Three-piece API surface

```cpp
namespace crd::geometry::mesh
{
// 1. Non-owning view — what callers hand to every mesh query.
template <MathScalar T> struct TriangleMeshView {
    ConstSpan<Vec3<T>> vertices;
    ConstSpan<u32>      indices;
};

// 2. Per-mesh BVH — built once, reused across thousands of queries.
struct TriangleMeshBvh {
    Array<AABB3<f32>> triangle_aabbs;
    BvhTree           tree;
};
[[nodiscard]] TriangleMeshBvh build_triangle_mesh_bvh(TriangleMeshViewf, IAllocator*);

// 3. The query.
[[nodiscard]] std::optional<MeshClosestPoint>
mesh_closest_point(TriangleMeshViewf, const TriangleMeshBvh&,
                   Vec3<f32> query, f32 max_dist = inf) noexcept;
}
```

`MeshClosestPoint = ClosestPointResult<u32>` (payload = triangle index;
field order pinned by ADR-0076 §16 pin #2 — `{point, distance_squared, payload}`).

### Algorithm — branch-and-bound BVH walk

```
push (root, aabb_dist_sq(root, query))
while stack non-empty:
    pop (node, lower_bound)
    if lower_bound >= best_dsq:   continue  // pruned (best tightened after push)
    if node.is_leaf():
        for each triangle ti in leaf:
            cp  = primitives::closest_point(Triangle3, query)   // Ericson §5.1.5
            dsq = |cp - query|²
            if dsq < best_dsq         (or)
               dsq == best_dsq AND ti < best_tri:               // determinism tiebreak
                update best
    else:
        compute aabb_dist_sq for left + right child
        push children — nearer last (popped first) so best tightens early
        skip child if its bound >= current best
```

### Determinism contract (ADR-0076 §4 pin #11)

- **Lower bound:** `aabb_dist_sq` = clamp-to-bounds-then-square — branchless,
  bit-exact across compilers / SIMD widths / OSes.
- **Leaf cascade:** delegated to `primitives::closest_point(Triangle3, p)`
  which is itself deterministic per ADR-0076 (Ericson Voronoi-region
  cascade with pinned vertex/edge tiebreak).
- **Tie-break on equal squared distance:** lowest triangle index wins
  (explicit `if dsq == best_dsq && ti < best_tri` branch).
- **Traversal order:** deterministic given a built tree — the
  `nearer-last push` rule is symmetric on `ld <= rd` (≤ not <), so when
  the two children's lower bounds are equal we deterministically push
  right first → left popped first.

### Two-layer typing (ADR-0078 §5)

The raw algorithm header (`mesh_closest_point.hpp`) consumes
`TriangleMeshViewf` + `Vec3<f32>` — **lower layer** per §5 D34. Typed
consumers (eylem `TriangleMeshCollider`, future scene queries with
typed inputs) include `mesh_queries_typed.hpp`:

```cpp
template <typename D, typename T> struct TriangleMeshViewT {
    ConstSpan<Vec3<Quantity<D, T>>> vertices;
    ConstSpan<u32>                   indices;
};

template <typename D, typename T> struct MeshClosestPointT {
    Vec3<Quantity<D, T>>           point;
    Quantity<DimMul<D, D>, T>      distance_squared;   // Area when D = Length
    u32                            payload;
};

template <typename D, typename T>
std::optional<MeshClosestPointT<D, T>>
mesh_closest_point(TriangleMeshViewT<D, T>, ConstSpan<Vec3<T>> raw_vertices,
                   const TriangleMeshBvh&, Vec3<Quantity<D, T>> query) noexcept;
```

**Strip-compute-retag** at the boundary — call site bridges with
`to_raw_vec(typed_query)`, calls the raw algorithm, wraps the result with
`from_raw_vec<D>(raw.point)` + `Quantity<DimMul<D, D>, T>{raw.distance_squared}`.

Zero runtime overhead — `to_raw_vec` / `from_raw_vec` are constexpr;
`.value` accessors compile away. Same pattern as
`crd-geometry-primitives/queries_typed.hpp` (v0d-2).

The typed wrapper takes a `ConstSpan<Vec3<T>> raw_vertices` separately
from the typed view because `TriangleMeshBvh` was built from raw vertex
positions (the AABBs are raw `f32` per §5 D34 — SIMD-boundary pin D22).
The typed view's `vertices` span is layout-compatible by ADR-0078 §1 D2
(`sizeof(Vec3<Length32>) == sizeof(Vec3f)`) — the caller could even
reinterpret_cast between them, but the explicit `raw_vertices` argument
documents the bridge.

## Test corpus

`tests/geometry-mesh/test_mesh_closest_point.cpp` — 6 cases / 145 assertions:

1. **Empty mesh returns nullopt** — degenerate input contract.
2. **Single triangle** — query directly above the centroid of an
   xy-plane triangle returns the centroid (z=0, dist²=4).
3. **Unit cube + +X face** — query at (5, 0, 0) yields (0.5, 0, 0) with
   dist²=20.25.
4. **125-query brute-force corpus** — 5³ grid of integer-coord queries
   spanning [-2, 2]³, each one cross-checked against a brute-force
   per-triangle scan with the same lowest-index tiebreak. Every BVH
   result matches brute-force distance² to within 1e-5.
5. **`max_dist` culls hits beyond the radius** — `max_dist=4.0` on the
   +X face (which is 4.5 m away) returns `nullopt`; `max_dist=5.0`
   returns the hit.
6. **Typed Quantity wrapper bridges through raw algorithm** — same
   unit-cube query with `Vec3<Length32>` input returns a typed
   `MeshClosestPointT<dim::Length, f32>` with `point.x.value == 0.5F`
   + `distance_squared.value == 20.25F` (Length² = Area, in m²).

## Build notes

- `BvhTree` is move-only (copy ctor + assign deleted; move ctor + assign
  `noexcept = default`). The `out.tree = bvh_build(...)` assignment in
  `build_triangle_mesh_bvh` uses the implicit move-assign — works.
- Unused-local warning in `mesh_closest_point.cpp` (the `auto& aabbs =
  bvh.triangle_aabbs;` line went stale during a refactor; removed) was
  caught by MSVC `/W4 /WX` on the first build attempt. Fixed in place.
- No `default_epsilon`, no `static_cast<T>` patterns that would trip
  Quantity instantiation — the raw algorithm stays clean MathScalar.

## 5-config DoD

| Config | Build | CTest |
|---|---|---|
| win-debug | clean | **1919/1919** (+6 from v4a) |
| win-asan | clean | 1919/1919 |
| win-shipping | clean | 1832/1832 |
| win-shipping-profile | clean | 1914/1914 |
| win-tidy | clean | — |

Full project ctest 1913 (Phase 3.1.7.5 close) → **1919 win-debug** after
v4a.

## Decisions locked

- **Per-mesh BVH is a separate value, not a member of `TriangleMeshView`.**
  Keeps the view trivially-copyable; callers build the BVH once and reuse
  it across thousands of queries. Same pattern as `crd-geometry-bvh` v1a's
  separation of `BvhTree` from the per-prim AABB span.
- **Per-triangle AABBs stored alongside the BVH** — needed to reconstruct
  the broadphase lower bound at each interior node (the BVH tree stores
  node AABBs internally, so leaf-prim AABBs aren't strictly required for
  the closest-point walk; but keeping them lets v4b's raycast share the
  same `TriangleMeshBvh` storage).
- **`aabb_dist_sq` inlined in `mesh_closest_point.cpp`, not routed through
  `primitives::closest_point(AABB3, p)`** — that helper returns a `Vec3`
  the caller then subtracts; the squared-distance form here is 3 conditional
  adds vs 3 subtracts + a length_sq. Hot-loop micro-optimisation; bit-exact
  equivalent to the primitives path.
- **`MeshClosestPoint = ClosestPointResult<u32>` payload = triangle index**
  (not `(triangle_index, u, v)` barycentrics). Barycentric extraction is a
  follow-on if a consumer needs it; the canonical query just wants "which
  triangle, where on it, how far". Triangle index → barycentrics is a
  trivial `Triangle3` + `point`-on-it inversion at the call site.

## Open follow-ups inside v4

- **v4b raycast** — Möller-Trumbore over the same `TriangleMeshBvh`,
  using the v0f watertight ray-tri or Baldwin-Weber form (TBD per
  perf/precision tradeoff at slice time).
- **v4c Jacobson 2013 winding number** — generalised winding for robust
  inside/outside on non-watertight meshes. Builds on closest-point
  (winding evaluator accumulates per-triangle solid-angle contributions
  along the way).
- **v4d per-leaf SIMD Möller-Trumbore** — `Vec8f` over 8 triangles per
  leaf. Requires repacking the triangle data per BVH leaf (SoA layout
  inside the leaf storage). Affects `TriangleMeshBvh` layout — design
  in v4d.
- **v4-validate** — formal mesh validation pipeline stage (manifoldness,
  orientation, area-zero, vertex-duplication, edge-non-manifold).
  Renewed-scope addition from 2026-05-14 ADR-0076 §15 review.
- **v4-close** — ADR-0076 §17 amendment + system doc
  `docs/systems/geometry-mesh.md` + 18-config full sweep (Phase rule
  per `feedback_full_sweep_required.md` is sub-slice = 5-config, sub-
  module close = full 18-config).

## References

- ADR-0076 §16 pin #2 — `RayHit{t, payload}` / `ClosestPointResult{point, distance_squared, payload}` field order.
- ADR-0076 §4 pin #11 — determinism tiebreak rules.
- ADR-0078 §5 D27/D32-D36 — two-layer typed architecture; strip-compute-retag boundary pattern.
- Christer Ericson, *Real-Time Collision Detection* §5.1.5 — closest-point-on-triangle Voronoi cascade.
- Wald 2007 — binned-SAH BVH (the v1f builder this slice consumes via `bvh_build`).
- `engine/geometry-bvh/` v1a-v1g — the per-prim AABB tree this slice indexes.
- `engine/geometry-primitives/include/crd/geometry/primitives/closest_point.hpp` — the Ericson cascade we call at leaves.
- `engine/geometry-primitives/include/crd/geometry/primitives/queries_typed.hpp` (v0d-2) — the strip-compute-retag pattern we mirror.
