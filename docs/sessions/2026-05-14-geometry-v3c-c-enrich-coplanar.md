# Session — 2026-05-14 — Phase 3.1.7 v3c-c — `enrich_for_gjk` + flat-3D-hull coplanar reconstruction (v3c CLOSED)

## Goal

Close the v3c Quickhull sub-phase by shipping the two remaining seams: (1) `enrich_for_gjk(QuickhullResult&)` populating v2g vertex adjacency + v2h SoA SIMD arrays for downstream GJK hill-climb / SIMD support paths, and (2) the flat-3D-hull coplanar reconstruction so v3c's `is_coplanar` branch returns a valid 2-face polytope instead of just a flag.

## What we built

### Public helper promoted — `compute_vertex_adjacency_from_faces`

New header `engine/geometry-primitives/include/crd/geometry/primitives/hull_adjacency.hpp`:

```cpp
namespace crd::geometry::primitives {
inline void compute_vertex_adjacency_from_faces(
    ConstSpan<u32> face_vertex_indices,
    ConstSpan<u32> face_vertex_offsets, usize num_vertices,
    Array<u32>& out_indices, Array<u32>& out_offsets) noexcept;
}
```

Algorithm: walk each face's vertex sequence, treat every consecutive pair (with wrap-around) as an undirected edge, insert both directions into per-vertex neighbor lists with dedup-on-insert, then flatten to prefix-sum form.

**Contract** (ADR-0076 §4 pin #14):
- Edge-symmetric: every edge `(u, v)` appears in both `u`'s and `v`'s neighbor lists.
- Duplicate-free.
- Deterministic neighbor order: first-encountered-while-walking-faces-in-face-index-order.

Lives in `crd-geometry-primitives` (the leaf substrate) so any higher tier consumes without depending on `-convex`. This was the algorithm `tests/geometry-convex/test_hill_climb.cpp::compute_vertex_adjacency_from_faces` originated; v3c-c promotes it to public so `enrich_for_gjk` and any future consumer (V-HACD cooker, eylem `Collider::ConvexHull` cooker) reuse a single implementation.

### `enrich_for_gjk(QuickhullResult&)`

Two-step in-place mutator:

1. **Vertex adjacency** (both f32 and f64 paths): calls `compute_vertex_adjacency_from_faces` over `r.face_vertex_indices` + `r.face_vertex_offsets`, writes into `r.vertex_adjacency_indices` + `r.vertex_adjacency_offsets`. Output matches the v1h `ConvexHullView<T>::vertex_adjacency_*` schema exactly — drops into the v2g hill-climb hull-support path without conversion.

2. **SoA SIMD arrays** (f32 path only, via `if constexpr (std::is_same_v<T, f32>)`): builds `vx_soa`, `vy_soa`, `vz_soa` flat `Array<f32>` from `r.vertices`, padded to a multiple of 8 by repeating vertex 0's coordinates. Matches the v2h `support_simd_f32` consumer's padding convention exactly. f64 hulls keep these arrays empty (v2h SIMD path is f32-only by construction per ADR-0076 §16 / v2h pin).

### Flat-3D-hull coplanar reconstruction in `quickhull()`

Replaces the v3c-a/v3c-b placeholder that just stored the 3 extremal points on `is_coplanar` detection. Now builds a proper flat 3D polytope:

1. Compute plane normal from first 3 extremals (`p0`, `p1`, `p2`); normalize.
2. Determine the dominant axis (the one with the largest absolute normal component) for the 2D projection.
3. Project all input points to 2D using the two non-dominant axes (drop the dominant axis).
4. Run v3b `convex_hull_2d_indices` on the projected points → CCW polygon in 2D.
5. Build flat 3D hull: vertices = the 2D-hull's vertex positions in their ORIGINAL 3D coordinates (preserves on-plane positions exactly).
6. Construct 2 face planes: front (outward normal = +plane_normal) and back (outward normal = -plane_normal), `plane.d` derived from the first vertex.
7. Determine 2D-CCW-to-3D-CCW correspondence by checking the cross product of the first two edges against +plane_normal; if reversed, flip the front face's vertex order.
8. `face_vertex_indices`: front face in CCW-from-+plane_normal order, back face in REVERSE (CCW-from-(-plane_normal) order).

The result is a valid `ConvexHullView`-compatible polytope. v2 GJK/EPA on it produces correct distance/overlap behavior with non-flat shapes via the support function (which only reads vertices). v2j feature-clipping and v2e ray-vs-hull work correctly with the 2 face planes.

## Tests — 8 new cases / 230 assertions

`tests/geometry-convex/test_quickhull.cpp` extends with section (8) `enrich_for_gjk` + section (9) coplanar:

**Adjacency:**
- Tetrahedron: each of 4 vertices has exactly 3 neighbors (the other 3); all neighbors valid indices; self never appears.
- Cube: each of 8 corners has ≥ 3 neighbors (cube edges + triangulation diagonals); all neighbors valid; **edge-symmetric contract** verified: every `u → v` neighbor implies `v → u`.

**SoA SIMD:**
- f32 cube (N=8): `vx_soa.size() == 8` (already padded); first 8 entries match `vertices[i].x` exactly.
- f32 tetrahedron (N=4): padded to 8; first 4 match vertices, last 4 repeat vertex 0's coordinates (the v2h padding convention).
- f64 hull: SoA arrays stay empty (`if constexpr` guard verified).

**Coplanar:**
- 4-point square on z=0: `is_coplanar = true`; 4 vertices, 2 faces; front/back normals exact opposites; each face has 4 vertex indices.
- 5-point square + interior on z=0: `is_coplanar`; 4 vertices (interior excluded), 2 faces.
- Coplanar hull view: `convex_hull_view_of` produces matching spans.

## Verification

- **win-debug**: 22 v3c cases / 401 assertions ✅. Full convex suite: **182 cases / 21079 assertions** (was 174 / 20849 before v3c-c; +8 / +230).
- **win-asan**: 22 v3c cases / 401 assertions ✅.
- **win-shipping**: 22 v3c cases / 401 assertions ✅.
- **win-tidy**: clean on v3c-c-specific files. One nested-ternary warning fixed inline (dominant-axis selection in coplanar branch refactored from nested `?:` to explicit `if/else if`).

Full 17-config sweep deferred to v3-close per standing directive.

## Decisions made

- **`compute_vertex_adjacency_from_faces` lives in `-primitives`**, not `-convex`. The leaf substrate where `ConvexHullView` lives. Any higher tier consumes without forced dependency on `-convex`. Multi-consumer (test_hill_climb, enrich_for_gjk, future V-HACD + eylem `Collider::ConvexHull` cooker) — one impl.
- **SoA SIMD arrays are f32-only via `if constexpr`**. Matches the v2h ADR-0076 §16 pin. f64 hulls don't pay the SoA build cost; their support paths take the AoS linear-scan or hill-climb branches.
- **SoA padding via vertex 0 duplication**, matching v2h's branch-free reducer convention (padded lanes tie with lane 0 on projection and lose by lowest-index tiebreak in the SIMD argmax). Zero runtime overhead, zero correctness risk.
- **Flat 3D hull = 2 face copies (front + back) of the 2D hull**. Degenerate-volume polytope but a valid `ConvexHullView` for v2 consumers. Front face uses `+plane_normal`; back uses `-plane_normal`. Vertex order: front in CCW-from-+plane_normal; back in reverse.
- **CCW determination via cross-product test**. After running v3b 2D hull, the CCW-in-2D may project to CCW or CW in 3D depending on the dominant-axis sign. A single `cross(edge1, edge2) · plane_normal` sign test determines the correct order for the front face.

## Bugs caught during implementation

**None.** All 8 v3c-c tests passed first-try. The reasons (similar to v3c-b's first-try success):
1. Design pinned before code (the 2-face flat hull form, the dominant-axis projection, the SoA padding scheme).
2. v3b 2D hull (already shipped and exercised) handles the 2D projection step robustly.
3. `compute_vertex_adjacency_from_faces` is a translation of the existing test helper — well-exercised algorithm.

## Files touched

- **New**: `engine/geometry-primitives/include/crd/geometry/primitives/hull_adjacency.hpp` (~80 LOC; the promoted public helper).
- **Edited**: `engine/geometry-convex/src/quickhull.cpp` (~110 LOC added in the coplanar branch + ~40 LOC `enrich_for_gjk` impl; ~150 LOC total).
- **Edited**: `tests/geometry-convex/test_quickhull.cpp` (~250 LOC added; 8 new test cases).

## v3c (Quickhull) — overall sub-phase summary

v3c is now CLOSED. Three sub-slices over the same day (2026-05-14):

- **v3c-a** (skeleton + types + initial tetrahedron + degenerate fallbacks): ~480 LOC engine. Built on v3a's full Stage D `orient3d` for bit-exact degeneracy decisions.
- **v3c-b** (main iteration loop): ~310 LOC engine. 14 test cases / 171 assertions passed first-try across 3 configs.
- **v3c-c** (`enrich_for_gjk` + coplanar reconstruction): ~150 LOC engine + ~80 LOC helper. 8 test cases / 230 assertions passed first-try.

**Total v3c**: ~1020 LOC engine + ~660 LOC tests (the earlier "honest 1500 LOC" estimate was generous — clean per-seam discipline + tight Cerid container idioms came in under budget). 22 test cases / 401 assertions.

## Next session starts with

**v3d — hull simplification.** QEM-style cost-driven edge-collapse decimation targeting `target_vertex_count` (default 32 — eylem `Collider::ConvexHull` sweet spot). Hard cap on relative-volume-change. `HullSimplifyOptions::keep_vertex_indices` for locked-vertex constraint (Q4 multi-domain decision — CAD/FEA/robotics consumers). API: `simplify_hull(QuickhullResult, opts) → QuickhullResult`. Estimated: ~400 LOC engine + ~300 LOC tests + 2-3 days.

After v3d: **v3-close** (tiebreak conformance + degenerate corpus + perf bench + full 17-config sweep + doc finalization).

## Notes for future-me

- `enrich_for_gjk` is a one-way operation. Calling it on an already-enriched `QuickhullResult` would double up the arrays. If we ever need re-enrichment, add a precondition assert OR clear the arrays at the top. Defer to v3-close if a consumer surfaces.
- The flat 3D hull's `face_vertex_indices` size is `2 * num_hull_vertices`. For a heavy-polygon coplanar input (e.g. 100-point convex polygon), this scales linearly. Not a perf concern at typical scales.
- The dominant-axis projection drops the axis with the LARGEST absolute normal component, which gives the BEST 2D-area-preserving projection (the projection plane is most-perpendicular to that axis). Standard CG practice.
- v3c is the largest single slice family in v3 (1020 LOC). Per-seam discipline (v3c-a → v3c-b → v3c-c, each with build+test+tidy) caught design issues before they compounded. Pattern worth applying to v3d if it grows.
