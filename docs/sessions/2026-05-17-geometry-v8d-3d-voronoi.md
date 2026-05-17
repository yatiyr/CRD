## Session 2026-05-17 — Phase 3.1.7 v8d-3d 3D Voronoi cells extraction

### Goal

Ship v8d-3d of the `crd-geometry-delaunay` cluster — geometric dual of
v8c's tetrahedralisation. Each Delaunay tet's circumcentre is a Voronoi
vertex; each Delaunay edge becomes a Voronoi face (the perpendicular
bisector polygon between the edge's two endpoint sites); cells emit as
DCEL (faces+vertices+is_bounded) PLUS a `ConvexHullView` helper for
consumers that want the convex-hull form (NNI, Worley, grain structures).

### What we built / changed

- **New primitive `crd::geometry::primitives::circumcenter_3d`** added to
  `engine/geometry-primitives/include/crd/geometry/primitives/circumcenter.hpp`.
  Computes tet circumcentre via Cramer's rule on the 3x3 linear system,
  **lifted to f64** then cast back to T (D100 — mirror of D95 for 3D).
  Returns centroid as finite fallback on degenerate input. Reusable by
  3D Ruppert + future tet meshers.
- **New public `engine/geometry-delaunay/include/crd/geometry/delaunay/voronoi_3d.hpp`**:
  `VoronoiStatus3` enum (`Ok` / `TooFewPoints` / `NonFiniteInput` /
  `DuplicatePoint` / `Coplanar` / `InternalInvariant`), `VoronoiFace3<T>`
  (neighbor_site_index + vertex_indices + is_unbounded), `VoronoiCell3<T>`
  (site_index + faces + is_bounded), `VoronoiResult3<T>` (voronoi_vertices
  + cells + status), `VoronoiCellHull3<T>` (vertices + Plane faces +
  face_vertex_indices + face_vertex_offsets prefix-sum + non-owning
  `view()` returning ConvexHullView).
- **New `engine/geometry-delaunay/src/voronoi_3d.cpp`** implementing:
  - **Phase 1**: run `delaunay_3d(sites)`; propagate diagnostic.
  - **Phase 2**: rebuild tet face adjacency via sort-and-scan over 4T
    half-faces keyed `(sorted_v0, sorted_v1, sorted_v2, tet_id, opp_local_idx)`.
    Pairs of consecutive entries with matching triples = interior faces;
    singletons = hull faces.
  - **Phase 3**: compute circumcentre per Delaunay tet.
  - **Phase 4**: sort-and-scan 6T half-edges keyed `(vmin, vmax, tet_id,
    local_edge_idx)`. Consecutive runs with same (vmin, vmax) = the tet
    fan around that Delaunay edge.
  - **Phase 5**: per edge run, walk the tet fan via face-adjacency. From
    start tet with edge (vmin, vmax) at local indices (si, ni), the 2
    fan-faces are at `{0,1,2,3} \ {si, ni}`. Walk one direction; if it
    closes (returns to start_t) the face is bounded; if it hits a null
    opposite-tet, walk the OTHER fan-face from start_t and splice as
    `reverse(backward) + forward`.
  - **Phase 6**: compute fan normal via triangle-fan cross-product sum;
    dot against axis `(vmax - vmin)`. If dot > 0, natural order is CCW
    from vmin side → emit forward for vmin, reverse for vmax. Else
    swap. Both sides get correctly outward-oriented faces from a SINGLE
    fan walk.
  - **Phase 7**: `is_bounded` = !any face.is_unbounded.
- **`convex_hull_for_cell` helper** — collects unique Voronoi vertices used
  by any face, builds a remap table (global → local), emits one
  `Plane<T>` per face with normal pointing from site TOWARD neighbor
  (outward) + d = -dot(normal, midpoint). Face-vertex-offsets is a
  prefix-sum. Returns empty for unbounded cells or out-of-range index.
- **Umbrella `delaunay.hpp`** re-exports v8d-3d.
- **`engine/geometry-delaunay/CMakeLists.txt`** docstring updated.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_voronoi_3d.cpp`.
- **`tests/geometry-delaunay/test_voronoi_3d.cpp`** — 14 cases / 87
  assertions including defining-property + face-normals-outward validators
  + ConvexHullView helper assertions + cospherical-pathology carry-over.

### Plain-English explanation

A 3D Voronoi diagram partitions space into POLYHEDRA where each polyhedron
contains all points closest to one input site. Mathematically it's the
geometric DUAL of the Delaunay tetrahedralisation:

- Each Delaunay tet's CIRCUMCENTRE is a Voronoi VERTEX.
- Each Delaunay EDGE (shared by N tets in the fan around it) becomes a
  Voronoi FACE (with N vertices = the N circumcentres).
- Each Delaunay TRIANGLE FACE (shared by 2 tets) becomes a Voronoi EDGE
  (the segment between the 2 circumcentres).
- Each Delaunay VERTEX (input site) becomes a Voronoi CELL (the
  polyhedron).

The algorithm runs Delaunay first, then for each input site enumerates
its Delaunay neighbours; for each (site, neighbor) pair, walks the tet
fan around that edge (cycling via shared faces) to collect the
circumcentres that form the cell's face dual to that edge.

The tricky bits:
- **Tet-edge adjacency**: each Delaunay edge is in N tets. We rebuild via
  sort-and-scan over 6T half-edge records.
- **Edge-fan walk**: a tet (v0, v1, v2, v3) containing edge (vi, vj) has
  exactly 2 faces touching BOTH vi and vj — those are the fan-faces. The
  other 2 faces (each touching only one of vi, vj) are off-axis. Walk
  through fan-faces in one direction, splice if unbounded.
- **Face orientation**: each Voronoi face has TWO sides; the same vertex
  ring serves both sides' cells with OPPOSITE CCW orderings. The fan walk
  produces ONE ordering; we test against the edge axis and emit one
  direction for one side, reverse for the other. Both sides get correct
  outward-normal CCW in a single walk — no separate fan per cell.

### Decisions made (D98-D101, pinned for ADR-0076 §23 amendment at v8-close)

- **D98.** **Edge-fan walk direction**: from start tet with edge (s, n)
  at local indices (si, ni), the 2 fan-faces are at `{0,1,2,3} \ {si, ni}`.
  Walk one face's neighbour; if closed, bounded. Else walk the OTHER from
  start_t and splice as `reverse(backward) + forward`.
- **D99.** **Face vertex CCW determined by axis dot product, not walk
  direction**. The fan's natural order is CCW from one of {vmin, vmax}'s
  side; the triangle-fan cross-product sum's dot against `(vmax - vmin)`
  picks which side. Emit forward for the matched side, reverse for the
  other. Single fan walk produces correct orientation for BOTH cells the
  face belongs to.
- **D100.** **circumcenter_3d lifted to f64** (mirror of D95 for 3D).
  Cramer's rule on 3x3 with `det6` denominator. Centroid fallback on
  degenerate input.
- **D101.** **Unbounded face = any null opposite-tet during the fan walk;
  cell unbounded = any face unbounded.** Propagated to `cell.is_bounded`
  in a final pass.

### Files touched

- `engine/geometry-primitives/include/crd/geometry/primitives/circumcenter.hpp`
  — extended with `circumcenter_3d`.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/voronoi_3d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports v8d-3d.
- `engine/geometry-delaunay/src/voronoi_3d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated.
- `tests/geometry-delaunay/CMakeLists.txt` — added test source.
- `tests/geometry-delaunay/test_voronoi_3d.cpp` — NEW (14 cases / 87 assertions).

### Tests / verification

- **14 cases / 87 assertions on v8d-3d suite**:
  - 4 diagnostics propagated from Delaunay (TooFewPoints / NonFiniteInput
    / DuplicatePoint / Coplanar).
  - 4-site tetrahedron (1 Voronoi vertex, 4 unbounded cells with 3 faces
    each).
  - 5-site tet + interior (1 bounded interior cell + 4 unbounded hull
    cells + face_normals_outward validator).
  - 8-site cube (`[cocircular]`, all 8 cells unbounded — Stage D incircle
    resolves cocircular configs).
  - 9-site cube + interior (1 bounded + 8 unbounded + face_normals_outward).
  - 24-site random cloud + defining_property + face_normals_outward.
  - **Cospherical-pathology 9-site mixture** (`[cospherical]` — Stage D
    insphere validator).
  - **ConvexHullView helper** on bounded cell (face planes pass through
    their vertices within 1e-9 + site is on NEGATIVE side of every face
    plane = interior convention).
  - ConvexHullView helper on unbounded cell returns empty.
  - Insertion-order determinism.
  - f64 precision tier.
- **Defining property** + **face_normals_outward** validators run on
  every non-trivial test. The defining-property check verifies cell
  topology is right; face_normals_outward catches CCW-reversal bugs.
- Combined delaunay suite: **59 cases / 749 assertions**, all green.
- **Mid-slice bug fixes**:
  - `EdgeFanResult res{};` defaulted an `Array<u32>` member that has no
    default ctor; added explicit `(alloc)` ctor on the struct.
  - `build_edge_fans` had unused `alloc` parameter (Cerid uses `/W4 /WX`
    so unused params are compile errors).
  - 3 unused using-decls in test file (`VoronoiCell3` / `VoronoiCellHull3`
    / `VoronoiFace3`) flagged by `misc-unused-using-decls` after the
    project-wide `WarningsAsErrors: '*'` policy flip.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel`.

### Next session starts with

v8e — Lloyd's CVT iteration (Lloyd 1982 Centroidal Voronoi Tessellation
relaxation). Run Voronoi, move each site to centroid of its cell, repeat
until convergence. Converges to "evenly distributed" optimum. Powers
stippling, blue-noise sampling, isotropic remeshing alternative, particle
initialisation. Builds on v8d-2d (and optionally v8d-3d for 3D Lloyd).
The cell-centroid computation is straightforward DCEL-form integration.
