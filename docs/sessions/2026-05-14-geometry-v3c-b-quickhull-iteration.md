# Session — 2026-05-14 — Phase 3.1.7 v3c-b — Quickhull main iteration loop

## Goal

Ship the algorithmic core of Quickhull: eye-point selection, visible-face DFS, horizon-edge identification, face replacement with neighbor relink, and conflict-list redistribution. Per the per-seam discipline locked in v3c-a, build on the initial-tetrahedron skeleton and add ONLY the iteration loop.

## What we built

Replaced the v3c-a placeholder "Step 4: main iteration loop (...TODO v3c-b...)" comment in `engine/geometry-convex/src/quickhull.cpp::quickhull()` with the full 8-step Quickhull iteration:

### Step 0 — Interior witness

Centroid of the initial tetrahedron `(p0 + p1 + p2 + p3) / 4` — guaranteed to lie inside the final hull (since the hull contains the initial tet). Used as the orientation reference for new-face vertex ordering: a new face's vertex order is CCW-from-outside iff `orient3d(v0, v1, v2, interior_witness) > 0` (Shewchuk convention: "below" = inside).

### Step 1 — Pick work face

Lowest-face-index face with non-empty `outside_set`. Deterministic via increasing-face-index scan.

### Step 2 — Pick eye point

Argmax `plane_distance(work_face, p)` over `work_face.outside_set`. Tiebreak on lowest input index (ADR-0076 §4 pin #11).

### Step 3 — Visible-face DFS

Iterative stack-based traversal from `work_face`. For each neighbor (visited in edge-index 0/1/2 order), if `orient3d(neighbor.v0, neighbor.v1, neighbor.v2, eye) < 0` (eye is "above" = outside), mark visible and recurse. Uses v3a's full Stage D `orient3d` — bit-exact decision on near-coplanar visibility tests.

### Step 4 — Horizon edges

For each visible face, walk its 3 edges in order. Edges where the neighbor is non-visible (or removed) are horizon edges. Collected with metadata: `(face_idx, edge_idx, outer_neighbor, va, vb)`.

### Step 5 — Build new faces

For each horizon edge, create a triangle `(eye, va, vb)`. Verify orientation against the interior witness via `orient3d`; flip `v[1]`/`v[2]` if needed to ensure CCW-from-outside. Faces are appended to the mesh `Array<QhFace>` (existing indices stay stable — old faces tombstoned, not compacted).

### Step 6 — Wire neighbors

Two passes:
- **6a**: each new face's "base" edge (the horizon edge) connects to the outer-neighbor (non-visible) face. Walk the new face's 3 edges to find the one matching `(va, vb)` or `(vb, va)`. Update both directions of the neighbor link.
- **6b**: between new faces. The eye-incident edges of new faces are shared between consecutive new faces around the eye. For each new face's uninitialized edge, find the OTHER new face that contains the same edge.

### Step 7 — Tombstone + orphan collection

Mark all visible faces `removed = true`. Collect their outside-set points (excluding the eye, which is now a hull vertex) as orphans.

### Step 8 — Orphan redistribution

For each orphan, test against each new face's plane in lowest-face-index order. Assign to the first new face whose plane is above the orphan (Shewchuk convention: `plane_distance > 0` = above = outside).

### Outer loop

Repeats while at least one non-removed face has a non-empty outside_set. Bounded by `opts.max_iterations` (default 100,000) with `CRD_ASSERT` on cap-hit (a bug, not a runtime-tolerable case).

## Tests added

`tests/geometry-convex/test_quickhull.cpp` — 14 test cases / 171 assertions:

### Closed-form hulls (4 cases)
- Regular tetrahedron (4 points) → 4 vertices, 4 triangles, all faces CCW, all inputs contained.
- Unit cube (8 corners) → 8 vertices, 12 triangles (Quickhull triangulates), all-CCW, all-contained.
- Regular octahedron (6 vertices) → 6 vertices, 8 triangles.
- Cube + 5 interior points → 8 vertices (interior points excluded), 12 triangles.

### Random point clouds (2 cases)
- 50 random points in unit cube — hull contains all, CCW invariant holds.
- 500 random points in unit sphere — **Euler characteristic verified**: `F == 2(V - 2)` exact equality (the canonical closed-triangulated-polyhedron invariant). All-CCW + all-inputs-contained also hold.

### Degenerate inputs (5 cases)
- Empty input → empty result.
- Single point → 1 vertex + `is_coincident` flag.
- 2 coincident points → 1 vertex + `is_coincident`.
- 5 collinear points (on `y = 2x, z = 3x` line through origin) → 2 vertices + `is_colinear`.
- 5 coplanar points (z=0 plane) → `is_coplanar` flag (v3c-a flag-detection only; full flat-3D-hull reconstruction is v3c-c).

### Determinism + f32 + view (3 cases)
- Replay produces identical vertex positions + face indices (32-point random cloud).
- f32 tetrahedron → 4 vertices, 4 faces (f32→f64 adaptive predicate path verified).
- `convex_hull_view_of(QuickhullResult)` produces matching spans (size + content).

## Verification

- **win-debug**: 14 v3c cases / 171 assertions + full convex suite 174 cases / 20849 assertions ✅
- **win-asan**: 14 v3c cases / 171 assertions ✅
- **win-shipping**: 14 v3c cases / 171 assertions ✅
- **win-tidy**: clean on `quickhull.cpp` + `quickhull.hpp` + `test_quickhull.cpp` (3 v3c-specific warnings fixed inline: `misc-unused-using-decls` for `QuickhullOptions`, `readability-identifier-naming` for local constexpr `V`/`F` renamed to `num_vertices`/`num_faces`)

Full 17-config sweep deferred to v3-close per standing directive.

## Decisions made / pinned

- **Interior witness = initial-tet centroid**. Cheap, deterministic, guaranteed inside the final hull. Computed once after `build_initial_tetrahedron`; never updated. Reused for every new-face orientation check throughout the iteration.
- **Visible-face DFS via iterative stack, not recursion**. Avoids stack overflow on pathological deep-visibility cases (a single eye point can see hundreds of faces on adversarial input).
- **Horizon edges collected in (visible-face-index, edge-index) order**. Deterministic per ADR-0076 §4 pin #11. Same input → same horizon-edge sequence → same new-face sequence → bit-exact replay.
- **Orphan redistribution in input-index order**. Orphans are collected in the order they appeared in visible faces' outside_sets, which is itself in input-index order (because Step 8 assignments use lowest-face-index tiebreak that preserves the original input ordering through iterations).
- **Tombstone, don't compact**. Visible faces stay in the `Array<QhFace>` with `removed = true`. Result extraction (`Step 5` of the parent function) walks all faces and skips removed ones. Compaction would force renumbering all neighbor indices — a much bigger debugging surface.
- **`CRD_ASSERT(iteration < opts.max_iterations)`** in the post-loop check. Hitting the cap means there's a bug (the algorithm doesn't terminate). Caller would get a partial hull silently otherwise.

## Bugs caught during implementation

**None.** Tests passed on first run.

The reasons it worked first-try (worth recording for future-me):
1. The advisor's design pass before writing code surfaced the orientation convention (Shewchuk "below = positive"), the interior witness pattern, and the deterministic-traversal-order requirement upfront. The implementation just translated the design.
2. v3c-a's initial-tetrahedron build already exercised the face-plane / orient3d / face-neighbor patterns end-to-end. v3c-b extended the same primitives without inventing new ones.
3. v3a-debt's full Stage D `orient3d` provides the exact-sign primitive that visibility tests need on near-coplanar visibility decisions. Without Stage D, the DFS could spuriously include or exclude faces on adversarial input, producing wrong hulls.

## Files touched

- **Edited**: `engine/geometry-convex/src/quickhull.cpp` (~310 LOC added in `quickhull()`'s Step 4 region — the iteration loop)
- **New**: `tests/geometry-convex/test_quickhull.cpp` (~410 LOC; 14 cases / 171 assertions)
- **Edited**: `tests/geometry-convex/CMakeLists.txt` (added `test_quickhull.cpp`)

## Next session starts with

**v3c-c — `enrich_for_gjk` + flat-3D-hull coplanar reconstruction.**
- `enrich_for_gjk(QuickhullResult&)` populates `vertex_adjacency_indices/offsets` (v2g hill-climb hull support) + `vx_soa/vy_soa/vz_soa` (v2h SoA Vec8f SIMD support).
- Coplanar fallback: when `find_initial_tetrahedron` returns `is_coplanar`, build the flat 3D hull by projecting the input onto the dominant plane, running v3b `convex_hull_2d_indices`, then constructing 2 copies of the 2D hull as front + back face arrays.
- Promote `test_hill_climb.cpp::compute_vertex_adjacency_from_faces` to a public helper (reused by `enrich_for_gjk`).

Estimated: ~250 LOC engine + ~150 LOC tests + 1-2 days.

After v3c-c: v3d hull simplification, then v3-close (full 17-config sweep + tiebreak conformance + perf bench).

## Notes for future-me

- Quickhull iteration loop went smoother than expected because the design was pinned and the substrate (v3a Stage D orient3d, v3c-a face structures) was solid. The per-seam discipline (initial tet → verify → iteration → verify → enrichment → verify) is paying off.
- The `Array<bool> face_visible` resize-and-fill-false at the top of each iteration is O(N_faces) — for very large hulls this could be a perf bottleneck. A "generation counter" pattern (each face stores the iteration number it was last visited; comparing against current iter number = "visited this round") would be O(1) but adds complexity. Defer to v3d/v3-close if benchmarks surface this.
- Cap of 100,000 iterations is large but not infinite. For pathological adversarial input (random cospherical points), Quickhull can have superlinear iteration counts. v3-close should add a stress test that approaches the cap to verify the assert fires correctly on a known-bad case.
